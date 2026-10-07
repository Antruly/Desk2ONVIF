#pragma once

/**
 * @file src/onvif/onvif_service.h
 * @brief ONVIF 的 SOAP 服务：设备服务 + 媒体服务。
 *
 * ## 为什么响应是手写信封，而不是 `uvcpp_soap_dump_response`
 *
 * 计划里原本打算用 libuvcpp 的信封层生成响应。实际做的时候发现那样做
 * **一定会产出错误命名空间的响应**，理由如下（已核对源码
 * `wsdl/uvcpp_soap_message.cpp:556` 的 `response_wrapper_block`）：
 *
 * 它只做两件事 —— 在包装元素上写一个 `xmlns="<op_ns>"`，然后把 `inner_xml`
 * **原样**塞进去。它不会扫描 `inner_xml` 里用到了哪些前缀。
 *
 * 而 ONVIF 的响应恰恰是**跨命名空间**的。以 `GetProfiles` 为例：
 *
 *     <trt:GetProfilesResponse>          ← media wsdl
 *       <trt:Profiles token="main">      ← media wsdl
 *         <tt:Name>main</tt:Name>        ← onvif schema！换命名空间了
 *
 * 包装元素上那个 `xmlns="…/media/wsdl"` 是个**默认**命名空间，会被所有不带
 * 前缀的后代继承。于是"内层不带前缀"这条规避办法在这里不但没用，还是错的：
 * `<Name>` 会落进 media wsdl，而 `tt:` 前缀又从未声明 —— 严格解析器直接拒收
 * 整条响应。
 *
 * 补救办法有两种，都不好：把 `xmlns:tt` 重复声明在**每一个**顶层子元素上
 * （声明的作用域是该元素及其后代，管不到兄弟），或者放弃包装元素自己去拼。
 * 后者只有十几行，而且**所有前缀只在 Envelope 上声明一次**就全局有效 ——
 * 于是这里选了后者。
 *
 * **解析仍然走 `uvcpp_soap_parse`。** 那才是这个库真正值钱的地方：报文来自
 * 网络，可能是恶意的，而它带了深度/节点数上限和良好的失败语义。生成那一侧
 * 的输入全是我们自己的常量，没有这类风险。
 */

#include "common/config.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace uvcpp {
class uvcpp_web_request;
class uvcpp_web_response;
}  // namespace uvcpp

namespace d2o {

/// 一路码流。ONVIF 的 Profile 就是"视频源 + 编码配置"的一套绑定。
struct OnvifStreamProfile {
  /// NVR 拿它来引用这一路（GetStreamUri 的 ProfileToken）。**不要带空格**
  /// —— 有些 NVR 会把它原样拼进 URL 查询串而不做转义。
  std::string token = "main";
  std::string name = "main";
  int width = 1920;
  int height = 1080;
  int fps = 30;
  int bitrate_kbps = 4000;
  int gop_seconds = 2;
  int quality = 4;
  std::string encoding = "H264";
  /// `tt:H264Profile` 的取值：`Baseline` / `Main` / `High` / `Extended`。
  /// **必须和编码器实际产出的档位一致** —— 有 NVR 会拿它决定要不要开硬件
  /// 解码通路，报高了会被拒。注意 `preset=ultrafast` 会强制降到
  /// Constrained Baseline（见 README 里那条），那种情况下应当报 `Baseline`。
  std::string h264_profile = "Baseline";
  /// RTSP 的路径部分（如 `/live`）。主机和端口由 OnvifServiceOptions 提供。
  std::string rtsp_path = "/live";
};

struct OnvifServiceOptions {
  /// `urn:uuid:...`。**必须跨重启稳定**，否则 NVR 会重复添加同一台设备。
  std::string uuid;
  std::string manufacturer = "Desk2ONVIF";
  std::string model = "Desk2ONVIF Virtual Camera";
  std::string firmware_version = "1.0.0";
  std::string hardware_id = "1.0";
  std::string serial_number;
  std::string device_name = "Desk2ONVIF";
  std::string location = "Desktop";

  /// RTSP 对外的端口。主机**不在这里** —— 它要按每条请求的来源地址反推
  /// （多网卡机器上写死一个地址，NVR 拿到的就是连不上的那个）。
  int rtsp_port = 554;
  std::vector<OnvifStreamProfile> profiles;

  /// Digest 凭据。`GetSystemDateAndTime` 之外的操作都要过这一关。
  std::string user = "admin";
  std::string password;
  std::string realm = "Desk2ONVIF";

  /// 快照 URL 用的 HTTP 端口。
  int http_port = 8080;
  std::string snapshot_path = "/snapshot.jpg";

  /// 设备时间：返回 `UTCDateTime` 还是 `LocalDateTime+TimeZone`。
  /// Windows 上 `localtime_s` 拿得到时区偏移，所以默认两者都给 —— NVR 的
  /// 时间显示依赖它，而只给 UTC 时有些 NVR 会按本地时间直读，差 8 小时。
  bool prefer_local_time = true;
};

/// `AppConfig::onvif` 里**可以运行中改**的那一段：身份与凭据。
///
/// ## 为什么只有这一段能热生效
///
/// 热生效的代价取决于"改它要动什么"。这一段**什么都不用动**：它整个生命周期
/// 里只在**循环线程**上被读（SOAP 请求就在那条线程上处理），既不碰编码器，
/// 也不碰 socket，也不参与任何已经算好的东西。改它就是把几个字符串换掉。
///
/// 反过来，其余每一项都做不到：
///
///   - `capture.*`（分辨率/码率/fps/gop/preset/线程数）要在编码器上生效。
///     **码率这条实测过**：`avctx->bit_rate` 中途改了 FFmpeg 的 libx264 封装层
///     根本不看（见 `tests/x264_reconfig_check.cpp`），而唯一能让 x264 改主意
///     的 `x264_encoder_reconfig` 没有从 `avcodec-63.dll` 里导出（导出表里
///     零个 x264 符号）。所以真实做法只能是重建编码器 —— 而重建就意味着
///     RTSP 会话重来、**所有客户端掉线**。那就是重启，只是把失败模式搬进了
///     一个正在跑进程里，更糟。
///   - `rtsp.port` / `web.port` / `web.host` 要重新 bind。
///   - `rtsp.path` / `sub_path` 要重建 ServerMediaSession。
///   - `onvif.enabled` 要增删路由，而它和 WS-Discovery 是联动的。
///
/// 这些一律留在 `pending` 里告诉用户重启，不在这个结构里。
///
/// **不含 `uuid` / `rtsp_port` / `http_port` / `profiles`**：前两个跟着监听
/// 端口走，后一个被每秒一次的分辨率上报**持续改写** —— 整体替换会把真实
/// 分辨率打回启动时的猜测值（详见 `set_identity` 的实现注释）。
struct OnvifIdentity {
  std::string user = "admin";
  std::string password;
  std::string manufacturer = "Desk2ONVIF";
  std::string model = "Desk2ONVIF Virtual Camera";
  std::string firmware_version = "1.0.0";
  std::string hardware_id = "1.0";
  /// 空 = 调用方应当填上派生的稳定值（见 main.cpp 里 `make_serial` 那一段）。
  /// **这里的空不是"没配"**，交给一个明确的地方去派生，而不是在这里猜。
  std::string serial_number;
  std::string device_name = "Desk2ONVIF";
  std::string location = "Desktop";
};

/// 从配置里取出身份那一段。**不做任何派生** —— 派生要用 `machine_seed()`，
/// 而这个头不该知道它。
OnvifIdentity identity_from_config(OnvifConfig const& c);

/// 把身份写进完整的选项结构：**只动身份那几个字段**，`uuid` / `rtsp_port` /
/// `http_port` / `profiles` / `realm` 原样保留。
void apply_identity(OnvifServiceOptions& o, OnvifIdentity const& id);

/// 把身份写回**配置**结构，和 `apply_identity` 成对。
///
/// 存在的理由和上面那对一样：`main.cpp` 要维护一份"运行中那一侧"的配置快照，
/// 热改成功之后得把身份同步进去。手写那九个赋值的话，字段集就有三份了
/// （读配置的、写选项的、写快照的），加一个身份字段漏掉哪一份都不报错。
void identity_to_config(OnvifIdentity const& id, OnvifConfig& c);

class OnvifService {
 public:
  OnvifService();
  ~OnvifService();
  OnvifService(OnvifService const&) = delete;
  OnvifService& operator=(OnvifService const&) = delete;

  /// 启动时设一次。之后不再改（改了就要考虑并发，而它整个生命周期里
  /// 都在循环线程上被读）。
  void set_options(OnvifServiceOptions o);
  OnvifServiceOptions const& options() const;

  /// 改身份与凭据，**运行中生效**。这是整个配置里唯一能热生效的一块。
  ///
  /// **只能在循环线程上调**（和 `handle()` 同一条线程）。这条约束是硬性的，
  /// 但它同时也就足够安全了：SOAP 请求在循环线程上处理，所以不可能有一条
  /// 请求正读到一半时被改。Web 的 POST 处理器本身就在循环线程上跑，
  /// 所以那边直接调即可，不需要 `app.post()` 绕一圈。
  ///
  /// 口令变了会把已发出的 nonce 全部作废（重建校验器）—— 换口令之后旧的
  /// nonce 不该还能用，而这是 `DigestAuth` 的既有语义（见陷阱 30）。
  void set_identity(OnvifIdentity const& id);

  /// 改 HTTP 端口。**只能在循环线程上调**（开机时用 `app->post()` 投一帧过去）。
  /// 单独开一个 setter 是因为它和别的选项不一样：监听端口要等 bind 成功才
  /// 知道（配置里写 0 表示由系统分配），而路由必须在那之前就注册好。
  void set_http_port(int p);

  /// 上报第 `index` 路码流的真实分辨率（0 = 主，1 = 子）。采集开始后由统计
  /// 回调带过来 —— 报错分辨率有些 NVR 会拒绝这个 profile，而症状只是
  /// "添加失败"，没有任何线索。
  ///
  /// `index` 越界是静默忽略。**不能写成"给所有 profile 设同一个尺寸"**：
  /// 子码流的分辨率本来就不同，那样会把两路报成一样大，NVR 拿到的
  /// GetProfiles 与实际码流对不上。
  ///
  /// 同样**只能在循环线程上调**。
  void set_video_size(std::size_t index, int w, int h);

  /// 处理一条 SOAP 请求。返回 true 表示响应**已经写完**（含 401 和 Fault）。
  ///
  /// `media_service` 为真表示路由到媒体服务，否则设备服务。两个服务的操作名
  /// 有重叠的可能（都有 `GetServiceCapabilities`），所以按**路由**而不是按
  /// Body 的命名空间来分派 —— 后者的行为取决于客户端写了什么，而路由是我们
  /// 自己定的，稳定得多。
  bool handle(uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
              bool media_service);

  /// 关掉之后连 `GetSystemDateAndTime` 也要鉴权。默认开着 —— 规范要求它是
  /// 匿名可访问的（客户端要先拿到时间才能算 Digest 的时钟偏移）。
  /// 实现在 .cpp 里：状态在 Impl 里，头文件里写不出来。
  void set_anonymous_date_time(bool on);

  /// 用 ONVIF 那一份 Digest 凭据校验一个**普通 HTTP** 请求。
  ///
  /// 用途只有一个：`GET /snapshot.jpg`。它同时被两种客户端拿 —— 浏览器走
  /// Web 会话 cookie，NVR 走 `GetSnapshotUri` 报出去的那个地址并按 ONVIF 的
  /// 规矩带 Digest。所以那个路由上的鉴权得认两套凭据，而 Digest 这一半必须
  /// 复用这里。
  ///
  /// **绝对不能就地新建一个 `DigestAuth`**：nonce 是用每进程一份的随机密钥
  /// 签的，重建对象就换了密钥，于是已经发出去的 nonce 全部失配 —— 客户端
  /// 会陷进"401 → 拿新 nonce → 还是 401"，而每一次往返看上去都完全合规。
  ///
  /// 返回 true 表示凭据正确；false 时调用方应当回 401。无论真假，`challenge`
  /// 都会被填成一份可以直接塞进 `WWW-Authenticate` 的挑战串。
  bool verify_http_digest(uvcpp::uvcpp_web_request& req, std::string& challenge);

  std::uint64_t requests() const;
  std::uint64_t auth_failures() const;
  std::uint64_t faults() const;

  /// 最近一次被拒绝的操作名，诊断用（NVR 调了我们没实现的接口时能一眼看到）。
  std::string last_unsupported() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace d2o
