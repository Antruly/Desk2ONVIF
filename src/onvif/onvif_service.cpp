#include "onvif/onvif_service.h"

#include "common/log.h"
#include "onvif/digest.h"
#include "onvif/net_identity.h"

#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <wsdl/uvcpp_soap_message.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace d2o {
namespace {

using uvcpp::uvcpp_soap_message;
using uvcpp::uvcpp_soap_parse;
using uvcpp::uvcpp_soap_version;
using uvcpp::uvcpp_web_request;
using uvcpp::uvcpp_web_response;

// ONVIF 的命名空间。**全部声明在 Envelope 上**，见头文件里那段说明 ——
// 这样内层元素想用哪个前缀就用哪个，不存在"声明管不到兄弟元素"的问题。
constexpr char kNsDevice[] = "http://www.onvif.org/ver10/device/wsdl";
constexpr char kNsMedia[] = "http://www.onvif.org/ver10/media/wsdl";
constexpr char kNsSchema[] = "http://www.onvif.org/ver10/schema";
constexpr char kNsNetwork[] = "http://www.onvif.org/ver10/network/wsdl";
// ONVIF 的**错误**命名空间（`ter`）。Fault 的子码就来自这里，
// 例如 `ter:ActionNotSupported`、`ter:NoSuchService`。
constexpr char kNsError[] = "http://www.onvif.org/ver10/error";

// 令牌。全都**不含空格和特殊字符**：有些 NVR 会把它直接拼进 URL 查询串。
constexpr char kTokenVideoSource[] = "VideoSource";
constexpr char kTokenVsConfig[] = "VideoSourceConfig";

std::string xml_escape(std::string const& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

/// 属性值也要转义。ONVIF 里 `Uri="rtsp://..."` 这类值出现得非常频繁，
/// 忘了转义就是一个只在特定地址下才炸的 bug。
std::string attr(std::string const& s) { return xml_escape(s); }

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/// ONVIF 的时间字段是**分开的数字元素**，不是 ISO8601 字符串 ——
/// 这一点和 WS-Discovery 那边（`AppSequence/@InstanceId`）不一样。
std::string date_time_fields(std::tm const& t) {
  std::string x;
  x += "<tt:Year>" + std::to_string(t.tm_year + 1900) + "</tt:Year>";
  x += "<tt:Month>" + std::to_string(t.tm_mon + 1) + "</tt:Month>";
  x += "<tt:Day>" + std::to_string(t.tm_mday) + "</tt:Day>";
  x += "<tt:Hour>" + std::to_string(t.tm_hour) + "</tt:Hour>";
  x += "<tt:Minute>" + std::to_string(t.tm_min) + "</tt:Minute>";
  x += "<tt:Second>" + std::to_string(t.tm_sec) + "</tt:Second>";
  return x;
}

void gmtime_utc(std::time_t t, std::tm& out) {
#if defined(_WIN32)
  ::gmtime_s(&out, &t);
#else
  ::gmtime_r(&t, &out);
#endif
}

void localtime_local(std::time_t t, std::tm& out) {
#if defined(_WIN32)
  ::localtime_s(&out, &t);
#else
  ::localtime_r(&t, &out);
#endif
}

/// 本机相对 UTC 的偏移（分钟，东为正）。
///
/// 用 `mktime(gmtime(t))` 再和 t 相减，而不是去碰 `timegm` / `_mkgmtime` ——
/// 后两者各平台名字不同（MinGW 上不保证有），而这里只需要标准库的两件套。
/// 推导：把 UTC 的钟面当成"本地时间"换算回 time_t，得到的是"当本地钟面等于
/// 此刻 UTC 钟面"的那个瞬间；它和此刻的差就是偏移量。
///
/// `tm_isdst = -1` 让 mktime 自己判夏令时。切换前后一小时可能差一小时，
/// 对设个相机时钟来说无所谓；而写死 0 会**全年**差一小时。
long utc_offset_minutes() {
  std::time_t const now = std::time(nullptr);
  std::tm g{};
  gmtime_utc(now, g);
  g.tm_isdst = -1;
  std::time_t const as_local = std::mktime(&g);
  if (as_local == static_cast<std::time_t>(-1)) return 0;
  return static_cast<long>(std::difftime(now, as_local) / 60.0);
}

/// ISO 8601 的时长形式：`PT8H`、`PT-5H30M`。ONVIF 的 `tt:TZ` 就是这么写的。
std::string iso_tz_duration(long minutes) {
  std::string s = "PT";
  long const abs_m = minutes < 0 ? -minutes : minutes;
  if (minutes < 0) s += "-";
  if (abs_m >= 60) {
    s += std::to_string(abs_m / 60) + "H";
    if (abs_m % 60) s += std::to_string(abs_m % 60) + "M";
  } else {
    s += std::to_string(abs_m) + "M";
  }
  return s;
}

/// 整个 SOAP 信封。Body 内容由调用方给。
///
/// **所有前缀只在这里声明一次。** 这是整个文件存在的前提：ONVIF 的响应是
/// 跨命名空间的（`<trt:GetProfilesResponse>` 里装着 `<tt:Name>`），而前缀
/// 声明的作用域只覆盖**该元素及其后代** —— 声明在顶层包装元素上就管不到
/// 兄弟，声明在 Envelope 上才全局有效。
std::string soap_envelope(uvcpp_soap_version v, std::string const& body_inner) {
  bool const v12 = (v == uvcpp_soap_version::V1_2);
  std::string x;
  x.reserve(body_inner.size() + 512);
  x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  x += "<soap:Envelope xmlns:soap=\"";
  x += v12 ? "http://www.w3.org/2003/05/soap-envelope"
           : "http://schemas.xmlsoap.org/soap/envelope/";
  x += "\" xmlns:tds=\"";  x += kNsDevice;
  x += "\" xmlns:trt=\"";  x += kNsMedia;
  x += "\" xmlns:tt=\"";   x += kNsSchema;
  x += "\" xmlns:dn=\"";   x += kNsNetwork;
  x += "\" xmlns:ter=\"";  x += kNsError;
  x += "\"><soap:Body>";
  x += body_inner;
  x += "</soap:Body></soap:Envelope>";
  return x;
}

/// Fault 的语义分类。**这不是"随便挑一个"**：ONVIF 的 Fault 表里，
/// "设备不支持这个可选操作"是 `env:Receiver` + 子码 `ter:ActionNotSupported`
/// —— 不是 `Sender`。发成 `Sender` 的话，按规范读的客户端会理解成"是你请求
/// 写错了"，有的实现会因此放弃整台设备的添加流程。
enum class FaultKind {
  /// 请求本身有问题（报文畸形、必填参数缺失）→ `Sender`。
  kBadRequest,
  /// 设备没实现这个可选操作 → `Receiver` + `ter:ActionNotSupported`。
  kNotSupported,
};

struct FaultInfo {
  FaultKind kind = FaultKind::kBadRequest;
  /// `ter:` 子码。空 = 不发子码。
  std::string subcode;
  /// 子码的下一层（`ter:NoSuchService` 这类）。空 = 不发。
  std::string sub_subcode;
  std::string reason;
};

/// 一条符合 ONVIF 的 Fault。
///
/// SOAP 1.2 的子码是**递归**的：`Code/Subcode/Value` 是这一层，再深一层要写成
/// `Code/Subcode/Subcode/Value`，而**不是**在同一个 Subcode 里并排放两个 Value。
/// 真实设备两种写法都有，但只有前者是良构的。
std::string soap_fault_xml(uvcpp_soap_version v, FaultInfo const& f) {
  bool const v12 = (v == uvcpp_soap_version::V1_2);
  std::string body;
  body += "<soap:Fault>";
  if (v12) {
    char const* const top =
        (f.kind == FaultKind::kNotSupported) ? "soap:Receiver" : "soap:Sender";
    body += "<soap:Code><soap:Value>";
    body += top;
    body += "</soap:Value>";
    if (!f.subcode.empty()) {
      body += "<soap:Subcode><soap:Value>ter:" + f.subcode + "</soap:Value>";
      if (!f.sub_subcode.empty()) {
        body += "<soap:Subcode><soap:Value>ter:" + f.sub_subcode +
                "</soap:Value></soap:Subcode>";
      }
      body += "</soap:Subcode>";
    }
    body += "</soap:Code>";
    body += "<soap:Reason><soap:Text xml:lang=\"en\">" + xml_escape(f.reason) +
            "</soap:Text></soap:Reason>";
  } else {
    // 1.1 的 `faultcode` **是**一个 QName，没有独立的子码字段；ONVIF 那套
    // `ter:` 子码塞进 `detail`。`soap:Server` 对应 1.2 的 `Receiver`。
    body += "<faultcode>";
    body += (f.kind == FaultKind::kNotSupported) ? "soap:Server" : "soap:Client";
    body += "</faultcode>";
    body += "<faultstring>" + xml_escape(f.reason) + "</faultstring>";
    if (!f.subcode.empty()) {
      body += "<detail><ter:" + f.subcode + "/></detail>";
    }
  }
  body += "</soap:Fault>";
  return soap_envelope(v, body);
}

/// 成功的响应：`<op xmlns="ns">inner</op>` 包进 Body。
std::string soap_response(uvcpp_soap_version v, char const* ns,
                          std::string const& op_local, std::string const& inner) {
  std::string body;
  body.reserve(inner.size() + op_local.size() * 2 + 64);
  body += "<" + op_local + " xmlns=\"" + ns + "\">";
  body += inner;
  body += "</" + op_local + ">";
  return soap_envelope(v, body);
}

}  // namespace

struct OnvifService::Impl {
  OnvifServiceOptions opt;
  bool anonymous_dt_ = true;
  std::atomic<std::uint64_t> requests{0};
  std::atomic<std::uint64_t> auth_failures{0};
  std::atomic<std::uint64_t> faults{0};
  /// 只在循环线程上写，所以不加锁。
  std::string last_unsupported;

  /// **整个进程只许建一次。** `DigestAuth` 里的 nonce 密钥是随机构造的，
  /// 每次新建都会让先前发出去的所有 nonce 立刻失效 —— 而症状极具欺骗性：
  /// 401 上的挑战完全合规、客户端按 RFC 算出的摘要也完全正确，服务端却一律
  /// 回 401，看起来像是"摘要算法写错了"。所以它跟着 `opt` 一起重建，不按
  /// 请求建。
  std::unique_ptr<DigestAuth> auth;

  DigestAuth& authenticator() {
    if (!auth) auth = std::make_unique<DigestAuth>(opt.realm, opt.user, opt.password);
    return *auth;
  }

  /// 一路都没配时也得能回答 —— `profiles.front()` 在空 vector 上是崩，
  /// 而崩在一条来自网络的 SOAP 请求里是最糟的失败方式。
  std::vector<OnvifStreamProfile> const& profiles() const {
    static std::vector<OnvifStreamProfile> const kFallback(1);
    return opt.profiles.empty() ? kFallback : opt.profiles;
  }

  std::string video_source_config(std::string const& ind,
                                  OnvifStreamProfile const& p) const {
    std::string x;
    x += ind + "<tt:Name>VideoSourceConfig</tt:Name>";
    x += ind + "<tt:UseCount>1</tt:UseCount>";
    x += ind + "<tt:SourceToken>" + kTokenVideoSource + "</tt:SourceToken>";
    x += ind + "<tt:Bounds x=\"0\" y=\"0\" width=\"" + std::to_string(p.width) +
         "\" height=\"" + std::to_string(p.height) + "\"/>";
    return x;
  }

  std::string encoder_config(std::string const& ind,
                             OnvifStreamProfile const& p) const {
    // GovLength 是**帧数**不是秒数（配置里存的是秒）。
    int const gov = clamp_int(p.gop_seconds * p.fps, 1, 100000);
    std::string x;
    x += ind + "<tt:Name>" + xml_escape(p.name) + "</tt:Name>";
    x += ind + "<tt:UseCount>1</tt:UseCount>";
    x += ind + "<tt:Encoding>" + xml_escape(p.encoding) + "</tt:Encoding>";
    x += ind + "<tt:Resolution><tt:Width>" + std::to_string(p.width) +
         "</tt:Width><tt:Height>" + std::to_string(p.height) +
         "</tt:Height></tt:Resolution>";
    x += ind + "<tt:Quality>" + std::to_string(p.quality) + "</tt:Quality>";
    x += ind + "<tt:RateControl>";
    x += "<tt:FrameRateLimit>" + std::to_string(p.fps) + "</tt:FrameRateLimit>";
    x += "<tt:EncodingInterval>1</tt:EncodingInterval>";
    x += "<tt:BitrateLimit>" + std::to_string(p.bitrate_kbps) +
         "</tt:BitrateLimit>";
    x += "</tt:RateControl>";
    // H264 块里 GovLength 和 H264Profile 都是**必填**的。缺 GovLength 时
    // 大华那边会把 I 帧间隔当成 0 并判定这个 profile 非法。
    x += ind + "<tt:H264>";
    x += "<tt:GovLength>" + std::to_string(gov) + "</tt:GovLength>";
    x += "<tt:H264Profile>" + xml_escape(p.h264_profile) + "</tt:H264Profile>";
    x += "</tt:H264>";
    // **不给 Multicast。** 给了之后有的 NVR 会优先去连组播地址，而我们没有
    // 实现组播推流，结果是"添加成功但一直没画面"。
    x += ind + "<tt:SessionTimeout>PT60S</tt:SessionTimeout>";
    return x;
  }

  /// 一个完整的 Profile。NVR 的通道列表就是拿它渲染的。
  std::string profile_block(std::string const& ind,
                            OnvifStreamProfile const& p) const {
    std::string x;
    x += ind + "<trt:Profiles token=\"" + attr(p.token) + "\" fixed=\"true\">";
    x += "<tt:Name>" + xml_escape(p.name) + "</tt:Name>";
    x += "<tt:VideoSourceConfiguration token=\"" +
         std::string(kTokenVsConfig) + "\">";
    x += video_source_config("", p);
    x += "</tt:VideoSourceConfiguration>";
    x += "<tt:VideoEncoderConfiguration token=\"VideoEncoderConfig_" +
         attr(p.token) + "\">";
    x += encoder_config("", p);
    x += "</tt:VideoEncoderConfiguration>";
    x += "</trt:Profiles>";
    return x;
  }

  OnvifStreamProfile const* find_profile(std::string const& token) const {
    if (token.empty()) return nullptr;
    for (OnvifStreamProfile const& p : profiles()) {
      if (p.token == token) return &p;
    }
    return nullptr;
  }

  /// 从报文里抠出 `ProfileToken` 的值。
  ///
  /// **不做完整解析**：这个值是我们自己发出去的 token，只用来查表，认不出就
  /// 当空（→ Fault），没有安全后果。为了它再走一遍 pugixml 不值得。
  /// 带不带命名空间前缀都能命中 —— 客户端加不加都合法。
  static std::string profile_token_from_body(std::string const& body_xml) {
    static char const kNeedle[] = "ProfileToken>";
    std::size_t const at = body_xml.find(kNeedle);
    if (at == std::string::npos) return {};
    std::size_t const from = at + sizeof(kNeedle) - 1;
    std::size_t const to = body_xml.find('<', from);
    if (to == std::string::npos) return {};
    return body_xml.substr(from, to - from);
  }

  std::string host_for(std::string const& peer) const {
    std::string host = pick_local_address_for(peer);
    if (host.empty()) host = default_local_address();
    if (host.empty()) host = "127.0.0.1";
    return host;
  }

  std::string url_for(std::string const& peer, char const* scheme, int port,
                      std::string const& path) const {
    return std::string(scheme) + "://" + host_for(peer) + ":" +
           std::to_string(port) +
           (path.empty() || path[0] != '/' ? "/" + path : path);
  }

  std::string stream_uri(std::string const& peer,
                         OnvifStreamProfile const& p) const {
    return url_for(peer, "rtsp", opt.rtsp_port, p.rtsp_path);
  }

  std::string snapshot_uri(std::string const& peer) const {
    return url_for(peer, "http", opt.http_port, opt.snapshot_path);
  }

  std::string device_op(uvcpp_soap_version v, std::string const& op,
                        std::string const& peer);
  std::string media_op(uvcpp_soap_version v, std::string const& op,
                       std::string const& body_xml, std::string const& peer);
};

std::string OnvifService::Impl::device_op(uvcpp_soap_version v,
                                          std::string const& op,
                                          std::string const& peer) {
  OnvifServiceOptions const& o = opt;

  if (op == "GetSystemDateAndTime") {
    std::time_t const now = std::time(nullptr);
    std::tm g{};
    gmtime_utc(now, g);
    std::string inner;
    // 两个都给：规范只要求给一个，但只给 UTC 时有些 NVR 会按本地时间直读，
    // 于是界面上的时间差 8 小时。
    inner += "<tds:UTCDateTime>" + date_time_fields(g) + "</tds:UTCDateTime>";
    if (o.prefer_local_time) {
      std::tm l{};
      localtime_local(now, l);
      inner += "<tds:LocalDateTime>" + date_time_fields(l) +
               "</tds:LocalDateTime>";
      inner += "<tds:TimeZone><tt:TZ>" + iso_tz_duration(utc_offset_minutes()) +
               "</tt:TZ></tds:TimeZone>";
    }
    // `NTP` 而不是 `Manual`：我们没有实现 `SetSystemDateAndTime`，报 Manual
    // 会让客户端（以及 ONVIF Device Manager）给出"设置设备时间"的入口，
    // 而那是一个点下去必然失败的入口。NTP 的意思是"这个钟由别处维护"。
    inner += "<tds:DateTimeType>NTP</tds:DateTimeType>";
    inner += "<tds:DaylightSavings>false</tds:DaylightSavings>";
    return soap_response(v, kNsDevice, "GetSystemDateAndTimeResponse", inner);
  }

  if (op == "GetDeviceInformation") {
    std::string inner;
    inner += "<tds:Manufacturer>" + xml_escape(o.manufacturer) +
             "</tds:Manufacturer>";
    inner += "<tds:Model>" + xml_escape(o.model) + "</tds:Model>";
    inner += "<tds:FirmwareVersion>" + xml_escape(o.firmware_version) +
             "</tds:FirmwareVersion>";
    inner += "<tds:SerialNumber>" + xml_escape(o.serial_number) +
             "</tds:SerialNumber>";
    inner += "<tds:HardwareId>" + xml_escape(o.hardware_id) + "</tds:HardwareId>";
    return soap_response(v, kNsDevice, "GetDeviceInformationResponse", inner);
  }

  if (op == "GetServices") {
    // NVR 主要靠这个找到"媒体服务在哪个 URL"。地址按请求来源反推 ——
    // 和 WS-Discovery 的 XAddrs 同一套逻辑：多网卡机器上写死一个地址，
    // 就是让 NVR 去连一个它够不着的网段。
    std::string inner;
    inner += "<tds:Service><tds:Namespace>" + std::string(kNsDevice) +
             "</tds:Namespace><tds:XAddr>" +
             xml_escape(url_for(peer, "http", o.http_port,
                                "/onvif/device_service")) +
             "</tds:XAddr><tds:Version><tt:Major>2</tt:Major>"
             "<tt:Minor>6</tt:Minor></tds:Version></tds:Service>";
    inner += "<tds:Service><tds:Namespace>" + std::string(kNsMedia) +
             "</tds:Namespace><tds:XAddr>" +
             xml_escape(url_for(peer, "http", o.http_port, "/onvif/media_service")) +
             "</tds:XAddr><tds:Version><tt:Major>2</tt:Major>"
             "<tt:Minor>6</tt:Minor></tds:Version></tds:Service>";
    return soap_response(v, kNsDevice, "GetServicesResponse", inner);
  }

  if (op == "GetCapabilities") {
    std::string inner;
    inner += "<tt:Capabilities>";
    // 只报 Device 和 Media。Events/PTZ/Imaging/Analytics 一个都没有 ——
    // 报了我们就会被调，而被调了只能回 Fault，白白拉长一次添加流程。
    inner += "<tt:Device><tt:XAddr>" +
             xml_escape(url_for(peer, "http", o.http_port,
                                "/onvif/device_service")) +
             "</tt:XAddr>"
             "<tt:Network><tt:IPFilter>false</tt:IPFilter>"
             "<tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
             "<tt:IPVersion6>false</tt:IPVersion6>"
             "<tt:DynamicDNS>false</tt:DynamicDNS></tt:Network>"
             "<tt:System><tt:DiscoveryResolve>true</tt:DiscoveryResolve>"
             "<tt:DiscoveryBye>true</tt:DiscoveryBye>"
             "<tt:RemoteDiscovery>false</tt:RemoteDiscovery>"
             "<tt:SystemBackup>false</tt:SystemBackup>"
             "<tt:SystemLogging>false</tt:SystemLogging>"
             "<tt:FirmwareUpgrade>false</tt:FirmwareUpgrade></tt:System>"
             "<tt:IO><tt:InputConnectors>0</tt:InputConnectors>"
             "<tt:RelayOutputs>0</tt:RelayOutputs></tt:IO>"
             "<tt:Security><tt:TLS1.1>false</tt:TLS1.1>"
             "<tt:TLS1.2>false</tt:TLS1.2>"
             "<tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration>"
             "<tt:AccessPolicyConfig>false</tt:AccessPolicyConfig>"
             "<tt:X.509Token>false</tt:X.509Token>"
             "<tt:SAMLToken>false</tt:SAMLToken>"
             "<tt:KerberosToken>false</tt:KerberosToken>"
             "<tt:RELToken>false</tt:RELToken></tt:Security></tt:Device>";
    inner += "<tt:Media><tt:XAddr>" +
             xml_escape(url_for(peer, "http", o.http_port, "/onvif/media_service")) +
             "</tt:XAddr><tt:StreamingCapabilities>"
             "<tt:RTPMulticast>false</tt:RTPMulticast>"
             "<tt:RTP_TCP>true</tt:RTP_TCP>"
             "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
             "</tt:StreamingCapabilities></tt:Media>";
    inner += "</tt:Capabilities>";
    return soap_response(v, kNsDevice, "GetCapabilitiesResponse", inner);
  }

  if (op == "GetServiceCapabilities") {
    // 设备服务的能力表。**和媒体服务那个同名但内容完全不同** —— 这正是
    // 按路由（而不是按 Body 的命名空间）分派的原因。
    std::string inner;
    inner += "<tds:Capabilities>";
    inner += "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" "
             "IPVersion6=\"false\" DynDNS=\"false\"/>";
    inner += "<tds:Security TLS1.1=\"false\" TLS1.2=\"false\" "
             "OnboardKeyGeneration=\"false\" AccessPolicyConfig=\"false\" "
             "X.509Token=\"false\" SAMLToken=\"false\" KerberosToken=\"false\" "
             "RELToken=\"false\"/>";
    inner += "<tds:System DiscoveryResolve=\"true\" DiscoveryBye=\"true\" "
             "RemoteDiscovery=\"false\" SystemBackup=\"false\" "
             "SystemLogging=\"false\" FirmwareUpgrade=\"false\"/>";
    inner += "</tds:Capabilities>";
    return soap_response(v, kNsDevice, "GetServiceCapabilitiesResponse", inner);
  }

  if (op == "GetNetworkInterfaces") {
    std::string inner;
    for (LocalAddr const& la : local_ipv4_addresses()) {
      inner += "<tds:NetworkInterfaces token=\"" + attr(la.ip) + "\">";
      inner += "<tt:Enabled>true</tt:Enabled>";
      inner += "<tt:Info><tt:Name>" + xml_escape(la.adapter) + "</tt:Name>";
      if (!la.mac.empty()) {
        inner += "<tt:HwAddress>" + xml_escape(la.mac) + "</tt:HwAddress>";
      }
      inner += "<tt:MTU>1500</tt:MTU></tt:Info>";
      inner += "<tt:Link><tt:AdminSettings>"
               "<tt:AutoNegotiation>true</tt:AutoNegotiation>"
               "<tt:Speed>100</tt:Speed><tt:Duplex>Full</tt:Duplex>"
               "</tt:AdminSettings><tt:OperSettings>"
               "<tt:AutoNegotiation>true</tt:AutoNegotiation>"
               "<tt:Speed>100</tt:Speed><tt:Duplex>Full</tt:Duplex>"
               "</tt:OperSettings><tt:InterfaceType>6</tt:InterfaceType>"
               "</tt:Link>";
      inner += "<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config>"
               "<tt:Manual><tt:Address>" + xml_escape(la.ip) + "</tt:Address>"
               "<tt:PrefixLength>" + std::to_string(la.prefix_len) +
               "</tt:PrefixLength></tt:Manual>"
               "<tt:DHCP>false</tt:DHCP></tt:Config></tt:IPv4>";
      inner += "</tds:NetworkInterfaces>";
    }
    return soap_response(v, kNsDevice, "GetNetworkInterfacesResponse", inner);
  }

  if (op == "GetDNS") {
    std::string inner =
        "<tds:DNSInformation><tt:FromDHCP>false</tt:FromDHCP>"
        "<tt:SearchDomain>local</tt:SearchDomain>"
        "</tds:DNSInformation>";
    return soap_response(v, kNsDevice, "GetDNSResponse", inner);
  }

  return {};
}

std::string OnvifService::Impl::media_op(uvcpp_soap_version v,
                                         std::string const& op,
                                         std::string const& body_xml,
                                         std::string const& peer) {
  std::vector<OnvifStreamProfile> const& ps = profiles();

  if (op == "GetProfiles") {
    std::string inner;
    for (OnvifStreamProfile const& p : ps) inner += profile_block("", p);
    return soap_response(v, kNsMedia, "GetProfilesResponse", inner);
  }

  if (op == "GetProfile") {
    OnvifStreamProfile const* p = find_profile(profile_token_from_body(body_xml));
    if (p == nullptr) return {};
    // GetProfile 的响应体**就是**一个 Profiles 元素，不再多包一层。
    return soap_response(v, kNsMedia, "GetProfileResponse", profile_block("", *p));
  }

  if (op == "GetStreamUri") {
    OnvifStreamProfile const* p = find_profile(profile_token_from_body(body_xml));
    if (p == nullptr) return {};
    // StreamSetup 里的 Transport 我们**看但不改**：RTP over TCP 还是 UDP 是
    // 客户端通过 RTSP 自己的 Transport 头选的，GetStreamUri 只负责给地址。
    std::string inner = "<trt:MediaUri>";
    inner += "<tt:Uri>" + xml_escape(stream_uri(peer, *p)) + "</tt:Uri>";
    inner += "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>";
    // InvalidAfterReboot=true：重启后这个会话就没了，客户端要重连。
    // 报 false 会让 NVR 一直拿着一个已经失效的会话。
    inner += "<tt:InvalidAfterReboot>true</tt:InvalidAfterReboot>";
    inner += "<tt:Timeout>PT0S</tt:Timeout>";
    inner += "</trt:MediaUri>";
    return soap_response(v, kNsMedia, "GetStreamUriResponse", inner);
  }

  if (op == "GetSnapshotUri") {
    std::string inner = "<trt:MediaUri>";
    inner += "<tt:Uri>" + xml_escape(snapshot_uri(peer)) + "</tt:Uri>";
    inner += "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>";
    inner += "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>";
    inner += "<tt:Timeout>PT0S</tt:Timeout>";
    inner += "</trt:MediaUri>";
    return soap_response(v, kNsMedia, "GetSnapshotUriResponse", inner);
  }

  if (op == "GetVideoSources") {
    OnvifStreamProfile const& p = ps.front();
    std::string inner =
        "<trt:VideoSources token=\"" + std::string(kTokenVideoSource) + "\">";
    inner += "<tt:Framerate>" + std::to_string(p.fps) + "</tt:Framerate>";
    inner += "<tt:Resolution><tt:Width>" + std::to_string(p.width) +
             "</tt:Width><tt:Height>" + std::to_string(p.height) +
             "</tt:Height></tt:Resolution>";
    // Imaging 是可选的；我们没有曝光/白平衡可调，报出来只会引出必然失败的调用。
    inner += "</trt:VideoSources>";
    return soap_response(v, kNsMedia, "GetVideoSourcesResponse", inner);
  }

  if (op == "GetVideoSourceConfigurations" || op == "GetVideoSourceConfiguration") {
    std::string inner =
        "<trt:Configurations token=\"" + std::string(kTokenVsConfig) + "\">";
    inner += video_source_config("", ps.front());
    inner += "</trt:Configurations>";
    return soap_response(v, kNsMedia, op + "Response", inner);
  }

  if (op == "GetVideoEncoderConfigurations" ||
      op == "GetVideoEncoderConfiguration") {
    std::string inner;
    for (OnvifStreamProfile const& p : ps) {
      inner += "<trt:Configurations token=\"VideoEncoderConfig_" +
               attr(p.token) + "\">";
      inner += encoder_config("", p);
      inner += "</trt:Configurations>";
    }
    return soap_response(v, kNsMedia, op + "Response", inner);
  }

  if (op == "GetVideoEncoderConfigurationOptions") {
    std::string inner = "<trt:Options>";
    inner += "<tt:QualityRange><tt:Min>1</tt:Min><tt:Max>6</tt:Max>"
             "</tt:QualityRange>";
    // 每一路的分辨率都列进可选范围。有些 NVR 会拿这个列表校验，
    // 而"当前用的分辨率不在可选列表里"会被判成配置非法。
    for (OnvifStreamProfile const& q : ps) {
      inner += "<tt:H264><tt:ResolutionsAvailable><tt:Width>" +
               std::to_string(q.width) + "</tt:Width><tt:Height>" +
               std::to_string(q.height) +
               "</tt:Height></tt:ResolutionsAvailable>";
      inner += "<tt:GovLengthRange><tt:Min>1</tt:Min><tt:Max>" +
               std::to_string(clamp_int(q.fps * 10, 1, 100000)) +
               "</tt:Max></tt:GovLengthRange>";
      inner += "<tt:FrameRateRange><tt:Min>1</tt:Min><tt:Max>" +
               std::to_string(q.fps) + "</tt:Max></tt:FrameRateRange>";
      inner += "<tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max>"
               "</tt:EncodingIntervalRange>";
      inner += "<tt:H264ProfilesSupported>" + xml_escape(q.h264_profile) +
               "</tt:H264ProfilesSupported></tt:H264>";
    }
    inner += "</trt:Options>";
    return soap_response(v, kNsMedia,
                         "GetVideoEncoderConfigurationOptionsResponse", inner);
  }

  if (op == "GetServiceCapabilities") {
    std::string inner = "<trt:Capabilities>";
    inner += "<tt:ProfileCapabilities MaximumNumberOfProfiles=\"" +
             std::to_string(ps.size()) + "\"/>";
    inner += "<tt:StreamingCapabilities RTPMulticast=\"false\" "
             "RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\" "
             "NonAggregateControl=\"false\"/>";
    inner += "</trt:Capabilities>";
    return soap_response(v, kNsMedia, "GetServiceCapabilitiesResponse", inner);
  }

  return {};
}

OnvifService::OnvifService() : impl_(std::make_unique<Impl>()) {}
OnvifService::~OnvifService() = default;

OnvifIdentity identity_from_config(OnvifConfig const& c) {
  OnvifIdentity id;
  id.user = c.user;
  id.password = c.password;
  id.manufacturer = c.manufacturer;
  id.model = c.model;
  id.firmware_version = c.firmware_version;
  id.hardware_id = c.hardware_id;
  id.serial_number = c.serial_number;
  id.device_name = c.device_name;
  id.location = c.location;
  return id;
}

void apply_identity(OnvifServiceOptions& o, OnvifIdentity const& id) {
  o.user = id.user;
  o.password = id.password;
  o.manufacturer = id.manufacturer;
  o.model = id.model;
  o.firmware_version = id.firmware_version;
  o.hardware_id = id.hardware_id;
  o.serial_number = id.serial_number;
  o.device_name = id.device_name;
  o.location = id.location;
}

void identity_to_config(OnvifIdentity const& id, OnvifConfig& c) {
  c.user = id.user;
  c.password = id.password;
  c.manufacturer = id.manufacturer;
  c.model = id.model;
  c.firmware_version = id.firmware_version;
  c.hardware_id = id.hardware_id;
  c.serial_number = id.serial_number;
  c.device_name = id.device_name;
  c.location = id.location;
}

void OnvifService::set_options(OnvifServiceOptions o) {
  impl_->opt = std::move(o);
  // 凭据可能变了，重建校验器。顺带把先前发出去的 nonce 全部作废 —— 换口令
  // 之后旧的 nonce 不该还能用。
  impl_->auth.reset();
}

void OnvifService::set_identity(OnvifIdentity const& id) {
  Impl& d = *impl_;
  // **逐字段赋值，不是整体替换 `opt`。** 整体替换会把 `profiles` 里的分辨率
  // 一起打回启动时的猜测值 —— 那两个数是被每秒一次的统计上报**持续改写**的
  // （`set_video_size`），而 Web 服务和采集是两个独立的启动阶段：替换之后
  // `GetProfiles` 会在下一条上报到达之前报错分辨率，NVR 恰恰可能正好在这一秒
  // 里来拉 profile。
  bool const creds_changed = (d.opt.user != id.user) || (d.opt.password != id.password);
  apply_identity(d.opt, id);
  if (creds_changed) d.auth.reset();
}

OnvifServiceOptions const& OnvifService::options() const { return impl_->opt; }

void OnvifService::set_anonymous_date_time(bool on) { impl_->anonymous_dt_ = on; }

bool OnvifService::verify_http_digest(uvcpp_web_request& req,
                                      std::string& challenge) {
  Impl& d = *impl_;
  std::string const hdr = req.header("Authorization");
  bool stale = false;
  // **顺序不能反：先 verify 再取 challenge。** `stale` 是 `verify()` 在
  // "摘要算对了、但 nonce 过期了"时置上的，而 `challenge(stale)` 要靠它决定
  // 发不发 `stale=true`。先取挑战串的话，nonce 过期的客户端拿到的是一份
  // 非 stale 的挑战，它会原样重试同一个过期 nonce —— 又 401，循环。
  DigestAuth& auth = d.authenticator();
  char const* const m = req.method_name();
  bool const ok = auth.verify(m != nullptr ? m : "GET", req.path(),
                              req.raw_url().empty() ? req.path() : req.raw_url(),
                              hdr, stale);
  challenge = auth.challenge(stale);
  if (!ok) {
    D2O_WARN("快照鉴权失败：来自 %s 的 %s %s，%s。", req.peer_ip().c_str(),
             m != nullptr ? m : "GET", req.path().c_str(),
             hdr.empty() ? "请求里既没有会话 cookie 也没有 Authorization 头"
                         : "发来了 Authorization 但摘要对不上（口令或算法不符）");
  }
  return ok;
}

void OnvifService::set_http_port(int p) { impl_->opt.http_port = p; }

void OnvifService::set_video_size(std::size_t index, int w, int h) {
  if (w <= 0 || h <= 0) return;
  // 越界静默返回而不是崩：这条路每秒走一次，而它唯一可能的错误是调用方把
  // 码流下标搞错了 —— 那种错该在启动时被发现，不该在一条来自网络的 SOAP
  // 请求路径上炸掉。profiles() 的空兜底也在这里生效。
  std::vector<OnvifStreamProfile>& ps = impl_->opt.profiles;
  if (index >= ps.size()) return;
  ps[index].width = w;
  ps[index].height = h;
}

std::uint64_t OnvifService::requests() const {
  return impl_->requests.load(std::memory_order_relaxed);
}
std::uint64_t OnvifService::auth_failures() const {
  return impl_->auth_failures.load(std::memory_order_relaxed);
}
std::uint64_t OnvifService::faults() const {
  return impl_->faults.load(std::memory_order_relaxed);
}
std::string OnvifService::last_unsupported() const {
  return impl_->last_unsupported;
}

bool OnvifService::handle(uvcpp_web_request& req, uvcpp_web_response& resp,
                          bool media_service) {
  Impl& d = *impl_;
  d.requests.fetch_add(1, std::memory_order_relaxed);

  // SOAP 请求都是几百字节。给到 256 KB 是留足余量，同时挡住"往这个端点
  // 灌 16 MB"这种最简单的骚扰。
  constexpr std::size_t kMaxSoapBytes = 256u * 1024u;
  if (req.body_size() > kMaxSoapBytes) {
    resp.status(413).text("SOAP body too large").end();
    return true;
  }

  // **必须用 body_data()/body_size()，不能用 content_length()。** 后者是
  // Content-Length 头里的**声明值**，和实到的字节数可能对不上 ——
  // 不一致本身就是攻击信号，而按声明值去读会越界。
  std::string const body(req.body_data(), req.body_size());

  // 凭据有无，解析之前就要知道 —— 解析失败的岔路要用它（见下）。
  std::string const hdr = req.header("Authorization");

  // 决定一次"未鉴权"的应答：401 + 挑战。**必须带挑战** —— 空挑战 = 不发
  // `WWW-Authenticate` 头，而一个没有挑战的 401 客户端只能放弃。
  bool auth_stale = false;
  auto challenge_401 = [&]() {
    d.auth_failures.fetch_add(1, std::memory_order_relaxed);
    resp.status(401)
        .text("Unauthorized")
        .set_header("WWW-Authenticate", d.authenticator().challenge(auth_stale))
        .end();
  };

  uvcpp_soap_message msg;
  char const* why = nullptr;
  if (uvcpp_soap_parse(body, msg, uvcpp::uvcpp_wsdl_limits(), &why) !=
      uvcpp::wsdl_status::OK) {
    // **没带凭据时回 401，不回 Fault。** 报文坏了当然是客户端的错，但一个
    // 连凭据都还没发过的客户端，此刻需要知道的是"请先鉴权"，不是"你的 SOAP
    // 有问题" —— 后者会让它永远不去算摘要。
    //
    // 这条不是理论问题，是 curl --digest 的现场（多数 NVR 的第一次请求也走
    // 同一条路）：它第一发故意不带凭据、只等挑战；我们回 500，它就放弃了，
    // 于是现象变成"客户端算出的摘要完全正确、服务端却一律 401"，看着像摘要
    // 算法写反了。带上凭据却发来坏报文，那才是 Fault —— 那时候确实该告诉他。
    if (hdr.empty()) {
      challenge_401();
      return true;
    }
    // 解析失败时**没法知道客户端说的是哪个 SOAP 版本** —— 连信封都没读出来。
    // 默认按 1.2 回（ONVIF 的绝大多数实现都是 1.2），并且给出一条结构良好的
    // Fault：断掉的 HTTP 响应会让部分 NVR 直接放弃添加设备。
    d.faults.fetch_add(1, std::memory_order_relaxed);
    FaultInfo f;
    f.kind = FaultKind::kBadRequest;
    f.subcode = "InvalidArgVal";
    f.reason = std::string("Malformed SOAP message: ") +
               (why != nullptr ? why : "parse error");
    std::string const xml = soap_fault_xml(uvcpp_soap_version::V1_2, f);
    resp.status(500);
    resp.set_content_type(uvcpp::uvcpp_soap_content_type(uvcpp_soap_version::V1_2));
    resp.body(xml);
    resp.end();
    return true;
  }

  uvcpp_soap_version const v = msg.version;
  std::string const& op = msg.body_local;
  std::string const ct = uvcpp::uvcpp_soap_content_type(v);

  auto send_fault = [&](FaultInfo const& f) {
    d.faults.fetch_add(1, std::memory_order_relaxed);
    resp.status(500);
    resp.set_content_type(ct);
    resp.body(soap_fault_xml(v, f));
    resp.end();
  };

  if (!msg.is_call()) {
    FaultInfo f;
    f.kind = FaultKind::kBadRequest;
    f.subcode = "InvalidArgVal";
    f.reason = "Body must contain exactly one non-Fault element";
    send_fault(f);
    return true;
  }

  // ---- 鉴权 ----------------------------------------------------------
  // `GetSystemDateAndTime` 是唯一的匿名入口：客户端要靠它拿到设备时间来纠正
  // 自己时钟的偏移，而 Digest 的摘要正是拿时间算的 —— 要求它鉴权是先有鸡
  // 还是先有蛋。
  bool const is_date_time = (!media_service && op == "GetSystemDateAndTime");
  if (!(is_date_time && d.anonymous_dt_)) {
    // 注意是 `d.authenticator()`（进程内唯一），不是就地新建一个。
    DigestAuth& auth = d.authenticator();
    char const* const m = req.method_name();
    bool const ok =
        auth.verify(m != nullptr ? m : "POST", req.path(),
                    req.raw_url().empty() ? req.path() : req.raw_url(), hdr, auth_stale);
    if (!ok) {
      // 只记"有没有发凭据"和来源，**不记摘要头本身** —— 它对排查没有帮助，
      // 却是一份可重放的凭据。这两条信息正好区分开两种最常见的现场：
      // 客户端压根没发 Authorization（口令没配），还是发了但算错了（口令不对）。
      D2O_WARN("ONVIF 鉴权失败：来自 %s 的 %s %s，%s。",
               req.peer_ip().c_str(), m != nullptr ? m : "POST", req.path().c_str(),
               hdr.empty() ? "请求里没有 Authorization 头"
                           : "发来了 Authorization 但摘要对不上（口令或算法不符）");
      challenge_401();
      return true;
    }
  }

  std::string const peer = req.peer_ip();
  std::string const xml = media_service ? d.media_op(v, op, msg.body_xml, peer)
                                        : d.device_op(v, op, peer);

  if (xml.empty()) {
    // 两种情况会走到这里：不认识的接口，或者 ProfileToken 查不到。
    // 对客户端来说"你问的这个我没有"是同一件事，所以共用一条 Fault。
    d.last_unsupported = op;
    D2O_INFO("ONVIF 不支持的请求：%s（%s服务）",
             op.empty() ? "(空操作名)" : op.c_str(), media_service ? "媒体" : "设备");
    FaultInfo f;
    f.kind = FaultKind::kNotSupported;
    f.subcode = "ActionNotSupported";
    f.sub_subcode = "NoSuchService";
    f.reason = "Action " + op + " is not supported by this device";
    send_fault(f);
    return true;
  }

  resp.status(200);
  resp.set_content_type(ct);
  resp.body(xml);
  resp.end();
  return true;
}

}  // namespace d2o
