#pragma once

/**
 * @file src/onvif/wsd.h
 * @brief WS-Discovery 应答器（UDP 3702 组播）。
 *
 * 这是**整个项目能不能被 NVR 发现**的那一环：NVR 添加 IP 通道时先往
 * 239.255.255.250:3702 发一个 Probe，谁的 ProbeMatch 回得快就列谁。
 *
 * ## 与 Windows 自身 WS-Discovery 共存 —— 已本机实测
 *
 * 本机的 `fdPHost`（Function Discovery Host Provider，PID 19924）**本来就绑着
 * `0.0.0.0:3702`**。实测结论（`tests/wsd_probe.py`）：
 *
 *   - 用 `SO_REUSEADDR` 绑 3702 **能成功**，不报错；
 *   - 从**另一个临时端口的 socket**（即"局域网里另一台机器"的最小模型）
 *     发到组播地址，绑着 3702 的 socket **收得到**；
 *   - Windows **不会**替我们回答 ONVIF 的 `NetworkVideoTransmitter` Probe。
 *
 * 第二条是关键的：libuvcpp 文档里"两进程能共存，但流量全归先绑者"那条结论
 * 是作者拿 **TCP** 实测的，而 TCP 和 UDP 组播语义不同 —— TCP 的 accept 队列
 * 只能有一个主人，组播报文则是投递给所有加入组的 socket。所以这里没有照搬
 * 那条结论，而是实测了。
 *
 * ## 只回答 Probe 和 Resolve
 *
 * `Hello` / `Bye` / `ProbeMatch` / `ResolveMatch` 一律**不回应**。这不是省事：
 * 回应 Hello 会让两台设备互相应答到死（A 的 Hello 引来 B 的 Hello，B 的 Hello
 * 又引来 A 的……组播里这是个真实的雪球）。WS-Discovery 规范里本来也只有
 * Probe 和 Resolve 需要应答。
 *
 * ## 安全
 *
 * 3702 是**无鉴权**的组播端口，任何人都能往里灌包，源地址还能随便伪造。
 * Probe 只有几百字节而 ProbeMatch 有 1KB 上下，不做限制我们就成了一个
 * **放大反射面**（伪造源地址 + 我们回包 = 拿我们去打别人）。所以这里：
 * 限单包大小、限回包速率、认不出的报文一律不回。
 */

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uvcpp {
class uvcpp_loop;
}

namespace d2o {

/// 一台设备对外宣告的**一个端点**。多通道之后，"这台机器"在组播里是**多台
/// 摄像机**，每一路 ONVIF 端口一个 —— 所以端点的三要素（UUID、端口、scopes）
/// 都是一份一份的，而不是全局一份。
///
/// `uuid` 必须**每通道各不相同**：NVR 把 EndpointReference 当主键，两个通道
/// 共用一个 UUID 的话，第二台会被当成"我已经有这台了"直接忽略 —— 现象是
/// "只搜到一台设备"，而它看起来完全正常。
struct WsdEndpoint {
  std::string uuid;                  ///< `urn:uuid:...`
  int http_port = 8080;              ///< 这一路的 ONVIF 端口
  /// 空 = 用 `WsdOptions::scopes`。通道 2 起的 scopes 里会带自己的名字，
  /// 所以通常是要单独给的。
  std::vector<std::string> scopes;
};

struct WsdOptions {
  /// `urn:uuid:...`，跨重启必须稳定（见 net_identity.h）。**通道 1 的**。
  std::string uuid;
  /// HTTP 端口。XAddrs 是 `http://<本机地址>:<port>/onvif/device_service`，
  /// 地址部分**按探测包的来源动态选**，不能在这里写死。**通道 1 的**。
  int http_port = 8080;
  std::string device_service_path = "/onvif/device_service";
  std::string types = "dn:NetworkVideoTransmitter";
  /// 完整 scope 列表（`onvif://...`），Hello/ProbeMatch 里都要带。
  /// `extra` 里某一项自带 scopes 时，那一项不用这份。
  std::vector<std::string> scopes;

  /// 通道 2 起要**一并宣告**的端点。
  ///
  /// ## 为什么挂在通道 1 上，而不是每个通道各起一个 responder
  ///
  /// UDP 3702 只有**一个**。第二个 responder 去 bind 同一端口，靠
  /// `SO_REUSEADDR` 也许能成，但组播的语义是"所有绑定者都收到每个包" ——
  /// 于是 2 个通道就回 2 份 ProbeMatches，8 个通道回 8 份，每份里还都带全部
  /// 通道。NVR 那边看到的是同一台设备被重复收录。所以**一个 socket、
  /// 一次回答、里面打包全部通道**才是对的形状。
  ///
  /// 空 = 只有通道 1（第五轮之前的行为，逐字节相同）。
  std::vector<WsdEndpoint> extra;
};

/// 把一件事投递到事件循环线程上执行（就是 `uvcpp_web_app::post`）。
/// 单独抽出来是为了让本模块不依赖 webapp，测试里可以塞一个"就地执行"的。
using WsdPoster = std::function<void(std::function<void()>)>;

class WsdResponder {
 public:
  WsdResponder();
  ~WsdResponder();

  WsdResponder(WsdResponder const&) = delete;
  WsdResponder& operator=(WsdResponder const&) = delete;

  /// **必须在事件循环跑起来之前调**：那时是单线程，建句柄/加组最安全。
  /// 成功返回 true 并已发出 Hello。
  bool start(uvcpp::uvcpp_loop* loop, WsdOptions const& opt, WsdPoster poster,
             std::string& err);

  /// 从任何线程调用。内部经 `poster` 投到循环线程发 Bye 并解绑。
  /// 幂等；没启动过时是空操作。
  void stop();

  /// 重新宣告一次：**先 Bye 再 Hello**。从任何线程调用，内部投到循环线程。
  ///
  /// 为什么不是只补发一个 Hello：Hello 的语义是"我来了"，而 NVR 大多把
  /// 已见过的 EndpointReference 当作"这台我已经知道了"直接忽略 —— 只发 Hello
  /// 的话，用户在网页上改了设备名/端口之后，NVR 上显示的还是旧的那份。
  /// 先 Bye 把它从列表里摘掉再 Hello，才是"重新宣告"。
  ///
  /// 代价是**已添加的通道会短暂掉线**（NVR 重新握手）。所以这是用户手动点
  /// "重新广播"时才做的事，不放在任何周期性路径上。
  ///
  /// 没启动过时是空操作；不会重新绑定端口（那要 stop() 再 start()）。
  void reannounce();

  /// 当前是否已在监听。线程安全。
  bool running() const;

  /// 收到过多少个 Probe/Resolve（含因限流或类型不匹配被丢的）。诊断用。
  unsigned long long probes_seen() const;
  unsigned long long probes_answered() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace d2o
