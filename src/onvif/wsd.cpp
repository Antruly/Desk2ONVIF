#include "onvif/wsd.h"

#include "common/log.h"
#include "onvif/net_identity.h"

// 顺序是硬性的：这一串必须排在 windows.h 之前。uv/win.h 自己会拉进
// winsock2.h 和 windows.h，所以只要 uvcpp 的头在最前面就够了 ——
// 反过来（先 log.h 之类）暂时也没事，但一旦哪个头开始碰 winsock 就会炸。
#include <handle/uvcpp_udp.h>
#include <req/uvcpp_udp_send.h>
#include <uvcpp/uvcpp_alloc.h>

#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace d2o {
namespace {

using uvcpp::uvcpp_udp;
using uvcpp::uvcpp_udp_send;

constexpr char kMcastAddr[] = "239.255.255.250";
constexpr int kMcastPort = 3702;

/// 单个入站报文的上限。真实 Probe 几百字节，1500 是 MTU 量级；留到 8K 是为了
/// 容忍实现粗糙的客户端，同时不给"往这个端口倒垃圾"的行为兜底。
constexpr std::size_t kMaxDatagram = 8192;

/// 回包速率上限（令牌桶）。放得比真实 NVR 的重试频率宽得多（正常一次发现
/// 也就 1~3 个 Probe），但足以让"被当成反射放大器"这件事不划算。
constexpr std::size_t kBurst = 30;
constexpr std::size_t kRefillPerSec = 30;

constexpr char kToDiscovery[] = "urn:schemas-xmlsoap-org:ws:2005:04:discovery";
constexpr char kToAnonymous[] =
    "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous";

std::uint64_t now_ms() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

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

std::string to_lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

/// 抠出 `<...:LocalName ...>文本</...:LocalName>` 里的文本。
///
/// 不用真正的 XML 解析器是有意的：这些报文来自**未鉴权的组播端口**，喂过来的
/// 东西可以是任意畸形内容。一个只会找标签、遇到不认识的就继续往下走的函数，
/// 比一个需要完整 DOM 的解析器更难被搞崩，并且它坏掉时的表现是"没认出 Action
/// →不回包"，正好是安全的那个方向。
///
/// 名字比较**大小写不敏感**：XML 本身是大小写敏感的，但既然回不回包的闸门是
/// Action 那个完整的 URI，标签名上宽松一点只会让格式粗糙的实现也能被我们认出来。
std::string find_tag_text(std::string const& xml, std::string const& local) {
  std::string const want = to_lower(local);
  std::size_t pos = 0;
  while (pos < xml.size()) {
    std::size_t const lt = xml.find('<', pos);
    if (lt == std::string::npos) return {};
    std::size_t const name_start = lt + 1;
    if (name_start >= xml.size()) return {};
    char const c0 = xml[name_start];
    // </... 是闭合标签，<?... <!-- 是声明和注释 —— 都不是我们要找的起始标签。
    if (c0 == '/' || c0 == '?' || c0 == '!') {
      pos = name_start;
      continue;
    }
    std::size_t i = name_start;
    while (i < xml.size() &&
           (std::isalnum(static_cast<unsigned char>(xml[i])) || xml[i] == '_' ||
            xml[i] == '-' || xml[i] == ':' || xml[i] == '.')) {
      ++i;
    }
    std::string const qname = xml.substr(name_start, i - name_start);
    std::size_t const colon = qname.rfind(':');
    std::string const localname =
        to_lower(colon == std::string::npos ? qname : qname.substr(colon + 1));

    // 标签名之后必须是空白或 '>'，否则 `Probe` 会匹配上 `ProbeMatches` 的开头。
    if (localname == want && i < xml.size() &&
        (xml[i] == '>' || std::isspace(static_cast<unsigned char>(xml[i])))) {
      std::size_t const gt = xml.find('>', i);
      if (gt == std::string::npos) return {};
      // 自闭合（<d:Types/>）—— 文本为空。
      if (gt > 0 && xml[gt - 1] == '/') return {};
      std::size_t const end = xml.find('<', gt + 1);
      if (end == std::string::npos) return {};
      std::string text = xml.substr(gt + 1, end - gt - 1);
      // 去掉首尾空白。探针和真机都会为了好看换行缩进。
      std::size_t b = 0, e = text.size();
      while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
      while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
      text = text.substr(b, e - b);
      if (!text.empty()) return text;
      // 找到了但空 —— 继续找同名的下一个（例如 Probe 里出现空的 Types）。
    }
    pos = lt + 1;
  }
  return {};
}

/// Action 的尾部（最后一个 '/' 之后）。用尾部比较而不是整体比较，是因为
/// 同一个动作在不同年份的命名空间里出现过多个 URI，而尾部是稳定的。
std::string action_tail(std::string const& action) {
  std::size_t const slash = action.rfind('/');
  if (slash == std::string::npos) return action;
  return action.substr(slash + 1);
}

/// 空格分隔的 URI 列表（Scopes / XAddrs 都是这个形状）。
std::vector<std::string> split_ws(std::string const& s) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    std::size_t const start = i;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i > start) out.push_back(s.substr(start, i - start));
  }
  return out;
}

std::string peer_ip_of(struct sockaddr const* addr) {
  if (addr == nullptr || addr->sa_family != AF_INET) return {};
  auto const* sin = reinterpret_cast<sockaddr_in const*>(addr);
  char buf[INET_ADDRSTRLEN] = {};
  if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) == nullptr) return {};
  return buf;
}

/// 一条 WS-Discovery 报文的公共骨架。
std::string envelope(std::string const& to, std::string const& action,
                     std::string const& relates_to, std::string const& message_id,
                     std::uint32_t instance_id, std::uint32_t message_no,
                     std::string const& body) {
  std::string x;
  x.reserve(body.size() + 768);
  x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  x += "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\"";
  x += " xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"";
  x += " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"";
  x += " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">";
  x += "<e:Header>";
  x += "<w:MessageID>" + xml_escape(message_id) + "</w:MessageID>";
  if (!relates_to.empty()) {
    x += "<w:RelatesTo>" + xml_escape(relates_to) + "</w:RelatesTo>";
  }
  // mustUnderstand 必须带 e: 前缀：SOAP 1.2 里它是信封命名空间的属性，
  // 不带前缀就落进了外层元素的默认命名空间（这里没有默认命名空间，
  // 那就成了"无命名空间的属性"）。严格的解析器会直接拒绝整条报文。
  x += "<w:To e:mustUnderstand=\"true\">" + xml_escape(to) + "</w:To>";
  x += "<w:Action e:mustUnderstand=\"true\">" + xml_escape(action) + "</w:Action>";
  // AppSequence：规范对 Hello/Bye 是必需，对 ProbeMatch 是推荐。少了它，
  // 有些实现会把重复收到的报文当成新的一次发现。
  x += "<w:AppSequence InstanceId=\"" + std::to_string(instance_id) +
       "\" MessageNumber=\"" + std::to_string(message_no) + "\"/>";
  x += "</e:Header><e:Body>";
  x += body;
  x += "</e:Body></e:Envelope>";
  return x;
}

}  // namespace

struct WsdResponder::Impl {
  WsdOptions opt;
  WsdPoster poster;

  /// shared_ptr 而不是 unique_ptr：停止时要发 Bye，而 send 是异步的 ——
  /// 报文的缓冲区归发送请求管，但**句柄必须活到发送完成回调跑完**。
  /// 让那个回调持有一份引用，最后一个引用消失时正好在循环线程上析构，
  /// 而 `~uvcpp_handle` 会替我们把 uv_close 发出去（见下面 shutdown_on_loop）。
  std::shared_ptr<uvcpp_udp> udp;
  std::string hello_payload;   ///< 启动时算好，避免每次重算选中地址

  std::atomic<bool> running{false};
  std::atomic<unsigned long long> seen{0};
  std::atomic<unsigned long long> answered{0};
  std::atomic<std::uint32_t> msg_no{1};

  /// `AppSequence/@InstanceId`：规范要求是"设备本次上电以来的秒数"，
  /// 并且在设备这一"实例"里保持不变。取系统运行时间即可 —— 它单调、
  /// 在进程活着的期间恒定，而且换一台机器看也是合理的值。
  std::uint32_t instance_id = 1;

  // ---- 以下只在循环线程上碰 ----
  std::uint64_t window_ms = 0;
  std::size_t window_count = 0;

  bool budget_ok() {
    std::uint64_t const t = now_ms();
    if (t - window_ms >= 1000) {
      window_ms = t;
      window_count = 0;
    }
    if (window_count >= kBurst) return false;
    ++window_count;
    return true;
  }

  std::uint32_t next_msg_no() {
    return msg_no.fetch_add(1, std::memory_order_relaxed);
  }

  std::string xaddrs_for(std::string const& peer_ip, int port) const {
    std::string ip = pick_local_address_for(peer_ip);
    if (ip.empty()) ip = default_local_address();
    if (ip.empty()) ip = "127.0.0.1";
    return "http://" + ip + ":" + std::to_string(port) + opt.device_service_path;
  }

  static std::string scopes_text(std::vector<std::string> const& scopes) {
    std::string s;
    for (std::string const& sc : scopes) {
      if (!s.empty()) s.push_back(' ');
      s += sc;
    }
    return s;
  }

  /// 展开成"要宣告的全部端点"。**通道 1 恒在下标 0**，后面按 `opt.extra`
  /// 的顺序 —— 别处（日志、`xaddrs_for(default_local_address())`）都按
  /// "第一个就是主设备"读，所以这个顺序是一份约定，不是巧合。
  std::vector<WsdEndpoint> endpoints() const {
    std::vector<WsdEndpoint> v;
    WsdEndpoint primary;
    primary.uuid = opt.uuid;
    primary.http_port = opt.http_port;
    primary.scopes = opt.scopes;
    v.push_back(std::move(primary));
    for (WsdEndpoint const& e : opt.extra) {
      WsdEndpoint x = e;
      if (x.scopes.empty()) x.scopes = opt.scopes;
      v.push_back(std::move(x));
    }
    return v;
  }

  /// 发一个 UDP 报文。`dst` 为空表示发到组播地址。
  ///
  /// 缓冲区用 `uvcpp_alloc_bytes` 分配，并把 `owner=true` 交给发送请求 ——
  /// 于是**请求析构时**它自己会 `uvcpp_free` 掉 base、再 `uvcpp_free` 掉
  /// uv_buf_t 本体（见 `~uvcpp_udp_send`）。前提是这两块都必须出自 `uvcpp_alloc`
  /// 那一族：全项目开了内存池时它们是池里的块，用 `std::free` 还就是堆损坏。
  /// 这就是这里不写 `malloc` 的原因。
  ///
  /// 完成回调是**必须**给的：`uvcpp_udp_server::send` 那条路上的"不给回调
  /// 就同步阻塞"虽然不在 `uvcpp_udp::send` 上，但给了回调才能确定
  /// "什么时候可以放掉这块内存"，也就不必去猜。
  void send_to(uvcpp_udp* u, std::string const& payload,
               struct sockaddr const* dst,
               std::function<void()> on_done = nullptr) {
    if (u == nullptr || payload.empty()) return;
    try {
      auto* b = uvcpp::uvcpp_alloc<uv_buf_t>();
      std::size_t const n = payload.size();
      b->base = static_cast<char*>(uvcpp::uvcpp_alloc_bytes(n));
      std::memcpy(b->base, payload.data(), n);
      b->len = n;

      auto* req = new uvcpp_udp_send();
      req->set_uv_buf(b, true);        // 析构时回收 base 和 b 本体
      req->set_self_free(true);        // 完成回调跑完 delete req

      int const rc = u->send(req, b, 1, dst,
                             [on_done](uvcpp_udp_send*, int status) {
                               if (status != 0) {
                                 D2O_DEBUG("WS-Discovery 送包失败（%d）", status);
                               }
                               if (on_done) on_done();
                             });
      if (rc != 0) {
        // 送没送出去，请求还是我们的 —— self_free 只有在完成回调那条路上
        // 才生效，这里必须自己收掉，否则连缓冲区一起漏。
        D2O_DEBUG("WS-Discovery 入队失败（%d）", rc);
        delete req;
        if (on_done) on_done();
      }
    } catch (std::bad_alloc const&) {
      // 分配失败而已。**绝不能让异常越过 libuv 的回调边界** —— 那会直接
      // 终止进程（C 栈帧里没人接）。丢一个包不吃亏。
      D2O_WARN("WS-Discovery 分配发送缓冲失败，丢弃这个包。");
    }
  }

  void send_multicast(uvcpp_udp* u, std::string const& payload,
                      std::function<void()> on_done = nullptr) {
    sockaddr_in dst{};
    if (::uv_ip4_addr(kMcastAddr, kMcastPort, &dst) != 0) return;
    send_to(u, payload, reinterpret_cast<sockaddr const*>(&dst), std::move(on_done));
  }

  /// 组播的 Hello / Bye。Bye 按规范可以不带 XAddrs —— 地址在这里没有意义，
  /// 带上反而会让某些实现去连一个正在下线的设备。
  ///
  /// 多通道时**每个端点各发一条**：一条报文里只能有一个 EndpointReference，
  /// 而 NVR 就是按它建条目的。N 个通道 = N 条 Hello，这是规范内的做法。
  std::string make_hello_or_bye(std::string const& action_suffix,
                                WsdEndpoint const& ep) {
    std::string body;
    body += "<d:" + action_suffix + ">";
    body += "<w:EndpointReference><w:Address>" + xml_escape(ep.uuid) +
            "</w:Address></w:EndpointReference>";
    body += "<d:Types>" + xml_escape(opt.types) + "</d:Types>";
    body += "<d:Scopes>" + xml_escape(scopes_text(ep.scopes)) + "</d:Scopes>";
    if (action_suffix == "Hello") {
      body += "<d:XAddrs>" +
              xml_escape(xaddrs_for(default_local_address(), ep.http_port)) +
              "</d:XAddrs>";
    }
    body += "<d:MetadataVersion>1</d:MetadataVersion>";
    body += "</d:" + action_suffix + ">";
    return envelope(kToDiscovery,
                    "http://schemas.xmlsoap.org/ws/2005/04/discovery/" +
                        action_suffix,
                    std::string(), ep.uuid, instance_id, next_msg_no(), body);
  }

  /// 每个端点一条。Hello 和 Bye 都走这里，别在调用点手写循环 ——
  /// `stop()` / `reannounce()` / `start()` 三处各写一遍迟早漏一处，
  /// 而漏掉的那个端点表现为"这个通道在 NVR 里一直显示在线"。
  ///
  /// `on_done` 会**每个端点各被调一次**：调用方给的通常是"放掉一份 udp 引用"
  /// 那种幂等动作（见 `shutdown_on_loop`），多调几次没有副作用。
  void send_hello_or_bye(uvcpp_udp* u, std::string const& action_suffix,
                         std::function<void()> on_done = nullptr) {
    for (WsdEndpoint const& ep : endpoints()) {
      send_multicast(u, make_hello_or_bye(action_suffix, ep), on_done);
    }
  }

  void on_datagram(ssize_t nread, uv_buf_t const* buf, sockaddr const* addr);
  void shutdown_on_loop();
};

void WsdResponder::Impl::on_datagram(ssize_t nread, uv_buf_t const* buf,
                                     sockaddr const* addr) {
  // nread == 0 且 addr == nullptr 是"这一轮没有更多数据了"，不是错误。
  if (nread <= 0 || addr == nullptr || buf == nullptr || buf->base == nullptr) return;
  if (static_cast<std::size_t>(nread) > kMaxDatagram) {
    D2O_DEBUG("WS-Discovery 收到 %zd 字节的超大报文，忽略。", nread);
    return;
  }
  // 只认 IPv4 单播源。组播源（自己发出去被回环回来的）在这里被挡掉。
  if (addr->sa_family != AF_INET) return;

  std::string const ip = peer_ip_of(addr);
  if (ip.empty()) return;

  std::string const xml(buf->base, static_cast<std::size_t>(nread));
  std::string const action = find_tag_text(xml, "Action");
  if (action.empty()) return;

  std::string const tail = action_tail(action);
  // 只答这两个。见头文件的说明：回 Hello 会让设备之间互相应答到死。
  if (tail != "Probe" && tail != "Resolve") return;

  seen.fetch_add(1, std::memory_order_relaxed);

  // 类型匹配：Probe 的 Types 为空 = "什么都行"；否则要求里面出现
  // NetworkVideoTransmitter。用子串而不是解前缀，是因为前缀绑定要靠真正的
  // 命名空间解析，而各家的前缀写法五花八门（dn: / tns1: / onvif: / 无前缀）。
  bool type_ok = true;
  if (tail == "Probe") {
    std::string const types = find_tag_text(xml, "Types");
    if (!types.empty()) {
      type_ok = types.find("NetworkVideoTransmitter") != std::string::npos;
    }
  }
  if (!type_ok) {
    D2O_DEBUG("WS-Discovery %s 询问的类型我们不匹配，不回答。", ip.c_str());
    return;
  }

  // Scope 过滤：只在探针**明确列了 scope** 时才判。ONVIF 规定 scope 匹配是
  // 前缀式的（`onvif://www.onvif.org/type/` 能匹配 `.../type/x`），这里按
  // 互相为前缀来判，宽松一点 —— 判错的代价是"设备在 NVR 里搜不到"，
  // 而多回一次包的代价只是 NVR 试着添加一台它其实能连上的设备。
  //
  // **逐端点各判一次**：通道 2 起的 scopes 里带着自己的名字，某个 Probe 明确
  // 只要 `name/前门` 时，只有那一个通道该回答。判一次全局的话，要么全都回
  // （用户只想要一台却收到八台），要么全都不回。
  std::string const probe_scopes = find_tag_text(xml, "Scopes");
  auto scopes_match = [&probe_scopes](std::vector<std::string> const& mine) {
    if (probe_scopes.empty()) return true;
    for (std::string const& want : split_ws(probe_scopes)) {
      for (std::string const& m : mine) {
        if (want.size() <= m.size() ? m.compare(0, want.size(), want) == 0
                                    : want.compare(0, m.size(), m) == 0) {
          return true;
        }
      }
    }
    return false;
  };

  std::vector<WsdEndpoint> const eps = endpoints();

  // Resolve 问的是**指定**的那一个端点，所以先按 EndpointReference 挑。
  // Probe 是"谁在" —— 把所有匹配 scopes 的端点都答上。
  std::vector<WsdEndpoint> answering;
  if (tail == "Resolve") {
    // 请求里的第一个 `Address` 就是它要找的端点（`Types`/`Scopes` 里也可能
    // 有同名标签，但 `EndpointReference` 在最前面，`find_tag_text` 取首个）。
    std::string const want = find_tag_text(xml, "Address");
    for (WsdEndpoint const& ep : eps) {
      if (want.empty() || ep.uuid == want) answering.push_back(ep);
    }
    if (answering.empty()) {
      // 找的不是我们。**必须不回答** —— 单播回一句"其实我是别人"会让对方
      // 把它当成自己问的那台设备。
      D2O_DEBUG("WS-Discovery %s 的 Resolve 问的是 %s，不是本机。",
                ip.c_str(), want.c_str());
      return;
    }
  } else {
    for (WsdEndpoint const& ep : eps) {
      if (scopes_match(ep.scopes)) answering.push_back(ep);
    }
    if (answering.empty()) {
      D2O_DEBUG("WS-Discovery %s 的 scopes 与本机没有交集，不回答。", ip.c_str());
      return;
    }
  }

  std::string const relates = find_tag_text(xml, "MessageID");

  if (!budget_ok()) {
    // 不回答，但**要记下来**：静默限流和"根本没收到"在现象上一模一样，
    // 而这两种情况要查的方向完全不同。
    D2O_WARN("WS-Discovery 回包超过 %zu 次/秒，限流中（来源 %s）。",
             kBurst, ip.c_str());
    return;
  }

  // **一条报文装下全部端点**，而不是每个端点回一条。N 条单播包在 NVR 那边
  // 是 N 次独立的处理，中间丢一条就少一个通道，而且丢哪条是随机的 ——
  // 而一条报文要么整份到、要么整份不到。
  std::string body;
  std::string action_out;
  std::string first_xaddrs;
  if (tail == "Probe") {
    body = "<d:ProbeMatches>";
    for (WsdEndpoint const& ep : answering) {
      std::string const xa = xaddrs_for(ip, ep.http_port);
      if (first_xaddrs.empty()) first_xaddrs = xa;
      body += "<d:ProbeMatch>"
              "<w:EndpointReference><w:Address>" + xml_escape(ep.uuid) +
              "</w:Address></w:EndpointReference>"
              "<d:Types>" + xml_escape(opt.types) + "</d:Types>"
              "<d:Scopes>" + xml_escape(scopes_text(ep.scopes)) + "</d:Scopes>"
              "<d:XAddrs>" + xml_escape(xa) + "</d:XAddrs>"
              "<d:MetadataVersion>1</d:MetadataVersion>"
              "</d:ProbeMatch>";
    }
    body += "</d:ProbeMatches>";
    action_out = "http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches";
  } else {
    WsdEndpoint const& ep = answering.front();
    first_xaddrs = xaddrs_for(ip, ep.http_port);
    body = "<d:ResolveMatch>"
           "<w:EndpointReference><w:Address>" + xml_escape(ep.uuid) +
           "</w:Address></w:EndpointReference>"
           "<d:Types>" + xml_escape(opt.types) + "</d:Types>"
           "<d:Scopes>" + xml_escape(scopes_text(ep.scopes)) + "</d:Scopes>"
           "<d:XAddrs>" + xml_escape(first_xaddrs) + "</d:XAddrs>"
           "<d:MetadataVersion>1</d:MetadataVersion>"
           "</d:ResolveMatch>";
    action_out = "http://schemas.xmlsoap.org/ws/2005/04/discovery/ResolveMatches";
  }

  // ResolveMatch 的 To 应当是请求里的 ReplyTo；不去解析它的唯一理由是
  // 现实里发 Resolve 的实现几乎都会把自己的 ReplyTo 设成 anonymous。
  // 万一遇到一个用了真实地址的，走匿名角色仍然是合法且可路由的。
  std::string const to = (tail == "Probe") ? kToDiscovery : kToAnonymous;

  // 信封的发送者用**通道 1** 的身份：一条报文只能有一个 `w:Address`，而
  // 里面每个 ProbeMatch 各自带着自己的 EPR —— 收方按 EPR 建条目，不看这个。
  std::string const reply =
      envelope(to, action_out, relates, opt.uuid, instance_id, next_msg_no(), body);

  // 单播回给探测者。**不能发回组播地址** —— 那会让局域网里每一台设备都
  // 收到本与他们无关的回包，几十台摄像机就是几十倍的广播风暴。
  send_to(udp.get(), reply, addr);
  answered.fetch_add(1, std::memory_order_relaxed);
  D2O_INFO("WS-Discovery 回答了 %s 的 %s（%zu 个通道，首个 XAddrs %s）",
           ip.c_str(), tail.c_str(), answering.size(), first_xaddrs.c_str());
}

void WsdResponder::Impl::shutdown_on_loop() {
  if (!udp) return;
  running.store(false, std::memory_order_relaxed);

  // 从这里起，"最后一个 shared_ptr 在谁手上"就是生命周期的全部。
  std::shared_ptr<uvcpp_udp> u = std::move(udp);
  u->recv_stop();

  // 把 udp 的最后一并交给完成回调：那条回调跑在循环线程上，于是在那里
  // 析构 udp 是安全的（`~uvcpp_handle` → `uv_close`，而 uv_close 只能在
  // 循环线程上调）。发送还没完成就把句柄拆了则会让 Bye 发不出去 —— 这是
  // 这里必须持引用而不是就地 reset 的原因。
  //
  // 完成回调万一不跑（循环正在被拆），这一份引用就一直留着不释放。
  // 代价是进程退出前那块 uv_udp_t 不还给分配器 —— 进程马上就没了，无所谓；
  // 而反过来（提前释放）是一个真的 use-after-free。选前者。
  //
  // 每个端点各一份引用（lambda 按值捕获同一个 shared_ptr），最后一个跑完的
  // 那个把它放掉 —— 于是"全部 Bye 都发完了"才真正释放，正是想要的。
  send_hello_or_bye(u.get(), "Bye", [u]() mutable { u.reset(); });
  D2O_INFO("WS-Discovery 已停止（发出 Bye）。");
}

WsdResponder::WsdResponder() : impl_(std::make_unique<Impl>()) {}
WsdResponder::~WsdResponder() = default;

bool WsdResponder::start(uvcpp::uvcpp_loop* loop, WsdOptions const& opt,
                         WsdPoster poster, std::string& err) {
  Impl& d = *impl_;
  if (d.udp) {
    err = "WS-Discovery 已经在跑了。";
    return false;
  }
  if (loop == nullptr) {
    err = "WS-Discovery 需要一个事件循环。";
    return false;
  }
  if (opt.uuid.empty()) {
    err = "WS-Discovery 需要设备 UUID（见 net_identity.h）。";
    return false;
  }
  // 附加端点也必须有 UUID，而且**不能和通道 1 相同**：NVR 按 EPR 建条目，
  // 撞了的话第二个通道会被当成"我已经有这台了"直接忽略 —— 现象是"只搜到
  // 一台设备"，而它看起来完全正常。这里当**启动失败**处理，不是跳过那一个：
  // 静默少一个通道比整个发现起不来更难查（NVR 上少一路，日志里一个字没有）。
  for (WsdEndpoint const& e : opt.extra) {
    if (e.uuid.empty() || e.uuid == opt.uuid) {
      err = "WS-Discovery 的附加通道需要一个**互不相同**的 UUID。";
      return false;
    }
    if (e.http_port == 0) {
      err = "WS-Discovery 的附加通道需要一个非零的 ONVIF 端口。";
      return false;
    }
  }

  d.opt = opt;
  d.poster = std::move(poster);
  d.instance_id = static_cast<std::uint32_t>(::GetTickCount64() / 1000ULL) + 1;

  auto u = std::make_shared<uvcpp_udp>(loop);

  // **标志只能从这里传。** `uvcpp_udp(loop, UV_UDP_REUSEADDR)` 在 libuv 1.51 上
  // 返回 UV_EINVAL：`uv_udp_init_ex` 把入参的低 8 位当**地址族**解析，而
  // UV_UDP_REUSEADDR = 4 不是合法的 AF_*。同一份标志走 bind 才是它该去的地方
  // （`uv_udp_bind` 把它直接映射成 SO_REUSEADDR；Windows 上还只在首次 bind 生效）。
  sockaddr_in local{};
  if (::uv_ip4_addr("0.0.0.0", kMcastPort, &local) != 0) {
    err = "构造 3702 的绑定地址失败。";
    return false;
  }
  int const rc = u->bind(reinterpret_cast<sockaddr const*>(&local), UV_UDP_REUSEADDR);
  if (rc != 0) {
    err = fmt("WS-Discovery 绑不上 UDP 3702（libuv 错误码 %d）。"
              "被别的程序独占时会出现这个 —— 但 Windows 自己的 WS-Discovery "
              "是可以共存的（SO_REUSEADDR），所以更可能是别的软件占着。",
              rc);
    return false;
  }

  // 组播回环开着：单机自测（探针和不放同一台机器上）必须能收到自己发的包。
  u->set_multicast_loop(1);
  u->set_multicast_ttl(4);   // 4 = site-local，与绝大多数 ONVIF 设备一致

  // 逐网卡加组。本机有三个 IPv4（含 WSL/Hyper-V 的虚拟网卡），而
  // `IP_ADD_MEMBERSHIP` 传 INADDR_ANY 时由内核挑一个 —— 挑错了就一个 Probe
  // 都收不到，且没有任何报错。所以这里对每个可用地址都加一次。
  auto const addrs = local_ipv4_addresses();
  std::size_t joined = 0;
  for (LocalAddr const& la : addrs) {
    int const jr = u->set_membership(kMcastAddr, la.ip.c_str(), UV_JOIN_GROUP);
    if (jr == 0) {
      ++joined;
      D2O_DEBUG("WS-Discovery 已在 %s（%s）上加入 %s", la.ip.c_str(),
                la.adapter.c_str(), kMcastAddr);
    } else {
      // 同一块网卡重复加组会返回 UV_EADDRINUSE，不是故障。
      D2O_DEBUG("WS-Discovery 在 %s 上加组返回 %d", la.ip.c_str(), jr);
    }
  }
  if (joined == 0) {
    int const jr = u->set_membership(kMcastAddr, nullptr, UV_JOIN_GROUP);
    if (jr != 0) {
      err = fmt("WS-Discovery 加入组播组 %s 失败（libuv 错误码 %d），"
                "没有任何网卡加成功。",
                kMcastAddr, jr);
      return false;
    }
    D2O_WARN("WS-Discovery 只能按默认网卡加入组播组（枚举不到任何网卡地址）。"
             "多网卡的机器上这可能收不到其它网段的 Probe。");
  }

  d.udp = std::move(u);

  // alloc 回调里分配的内存**由 recv 回调负责释放** —— uvcpp 的
  // `callback_alloc`/`callback_udp_recv` 只是转发，不碰缓冲区（已核对
  // `handle/uvcpp_handle.cpp:117` 与 `handle/uvcpp_udp.cpp:147`）。
  int const rr = d.udp->recv_start(
      [](uvcpp::uvcpp_handle*, std::size_t suggested, uv_buf_t* buf) {
        std::size_t n = suggested != 0 ? suggested : 2048;
        if (n > kMaxDatagram) n = kMaxDatagram;
        buf->base = static_cast<char*>(std::malloc(n));
        buf->len = buf->base != nullptr ? n : 0;
      },
      [&d](uvcpp_udp*, ssize_t nread, uv_buf_t const* buf,
           sockaddr const* addr, unsigned) {
        struct Guard {
          uv_buf_t const* b;
          ~Guard() { std::free(b->base); }
        } guard{buf};
        d.on_datagram(nread, buf, addr);
      });
  if (rr != 0) {
    err = fmt("WS-Discovery 启动收包失败（libuv 错误码 %d）。", rr);
    d.udp.reset();
    return false;
  }

  d.running.store(true, std::memory_order_relaxed);

  // Hello：告诉局域网"我来了"。NVR 通常不等 Hello（它们主动 Probe），
  // 但有些实现确实靠它维护在线列表。**每个端点一条**。
  d.send_hello_or_bye(d.udp.get(), "Hello");

  D2O_INFO("WS-Discovery 已启动：%s:%d，设备 %s（%zu 个通道）", kMcastAddr,
           kMcastPort, opt.uuid.c_str(), d.endpoints().size());
  return true;
}

void WsdResponder::stop() {
  Impl& d = *impl_;
  if (!d.udp) return;

  if (d.poster) {
    // 循环线程上跑：send/recv_stop/析构句柄全是循环线程的操作。
    // 用 shared_ptr<Impl> 之外的办法捕获 impl_ 是不行的 —— impl_ 归本对象，
    // 而本对象可能比这条投递活得短。这里捕获裸 this 是可以的：WebServer 保证
    // 先 stop() 再析构，且投递是在 stop() 里同步发出的。
    Impl* p = &d;
    d.poster([p]() { p->shutdown_on_loop(); });
  } else {
    // 没有 poster 说明 start() 没成功过，或者调用方自己就是循环线程。
    d.shutdown_on_loop();
  }
}

void WsdResponder::reannounce() {
  Impl& d = *impl_;
  if (!d.udp) return;
  if (!d.poster) return;

  // 和 stop() 同样的理由：捕获裸 this 是安全的（WebServer 保证先 stop() 再析构，
  // 而投递是在这里同步发出的），捕获 impl_ 的地址反而会在对象先死时悬垂。
  Impl* p = &d;
  d.poster([p]() {
    // 循环线程上跑。**顺序不能反**：Bye 先摘掉，Hello 再到 —— 反过来就是
    // "先声明我在、再说我走了"，NVR 的在线状态会停在"不在"。
    if (!p->udp) return;  // stop() 可能已经在这之前排进队列并跑完了
    p->send_hello_or_bye(p->udp.get(), "Bye");
    p->send_hello_or_bye(p->udp.get(), "Hello");
    D2O_INFO("WS-Discovery 已重新广播（Bye + Hello），NVR 会重新收录这台设备。");
  });
}

bool WsdResponder::running() const {
  return impl_->running.load(std::memory_order_relaxed);
}

unsigned long long WsdResponder::probes_seen() const {
  return impl_->seen.load(std::memory_order_relaxed);
}

unsigned long long WsdResponder::probes_answered() const {
  return impl_->answered.load(std::memory_order_relaxed);
}

}  // namespace d2o
