#pragma once

/**
 * @file src/web/web_server.h
 * @brief HTTP / WebSocket 服务层（libuvcpp webapp）。
 *
 * 线程模型：`start()` 起它自己的后台线程跑事件循环，采集线程通过 `publish()`
 * 把统计投递过去。**除了 `publish()` / `stop()` / 那几个常量读取，本类不提供
 * 任何线程安全的入口** —— 这与 libuvcpp 的约定一致（`uvcpp_web_context` 的
 * 注释：「所有方法都在 loop 线程上调用，`post()` 除外」）。
 *
 * ## 为什么强制单循环
 *
 * libuvcpp 支持 `set_loops(n)` 横向扩展，但 WS 会话是**按循环分片**的，而
 * 从一个 `uvcpp_ws_connection*` 反查它属于哪条循环**没有公开接口**（连接对象
 * 没有 id、没有 handle 访问器，`get_tcp_client()` 只在 `is_open()` 时有效）。
 * 于是"从别的线程推一条消息给所有 WS 客户端"在多循环下做不到：`post(fn)`
 * 只会落到 0 号循环，从 0 号循环的线程去写别的循环持有的连接就是跨线程操作。
 *
 * 所以要横向扩展就得先有"会话 → 循环"的映射，那需要改 libuvcpp。
 * 在拿到之前这里**钉死 1 条循环**，配置里写了别的值会打 WARN 并忽略 ——
 * 静默按 0 号循环跑才是真正危险的（统计只在部分客户端上刷新，
 * 而现象取决于连上来时命中了哪条循环）。
 */

#include "capture/frame.h"
#include "onvif/onvif_service.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace d2o {

struct SessionStats;

/// `POST /api/config` 的**逐字段**结果。
///
/// 为什么不能只回一个 `restart_required` 布尔值：一次保存里常常同时存在
/// "这项当场生效了"和"那项要重启"。只给一个布尔值时前端只能二选一 ——
/// 两种情况里必有一种是在骗人，而用户没有任何办法看出来是哪种。
struct ConfigApplyResult {
  /// 本次保存里**已经当场生效**的字段（点分路径，如 `onvif.device_name`）。
  std::vector<std::string> applied;
  /// **已保存但要重启**才生效的字段（如 `rtsp.port`）。
  ///
  /// 这里是"本次提交造成的差异"，不是全量 —— 上一次保存留下的、还没重启的
  /// 那些项由 `config_pending` 负责（两者在实现上是同一份数据）。
  std::vector<std::string> pending;
};

struct WebOptions {
  bool enabled = false;
  std::string host = "0.0.0.0";
  int port = 8080;
  /// 静态文件根目录（Vue 的 dist）。空 = 不挂静态路由。
  std::string static_root;

  /// Web 控制台的登录。**默认开**，凭据是 `web.user` / `web.password`。
  ///
  /// 和 ONVIF 那一份凭据**故意不共用**：NVR 里存着的那份是给机器用的（改一次
  /// 要去每台 NVR 上改），网页这份是人手敲的（改了只影响自己）。共用的后果是
  /// "为了改网页口令，得把 NVR 全部重配一遍"，于是没人会去改它。
  bool auth_enabled = true;
  std::string auth_user = "admin";
  /// 空 = 不拦人（见 `SessionStore::effective()`），启动时会打一条 WARN。
  std::string auth_password = "admin";

  /// `/api/config` 的读写桥。**用回调而不是让 WebServer 自己持一份
  /// `AppConfig`**：权威副本在 main.cpp 那边（它还要管命令行覆盖、校验和
  /// 持久化路径），Web 层再存一份就成了两个真相 —— 而"网页上改了没生效"
  /// 这类问题的根源几乎总是两处状态不同步。
  ///
  /// 两个都留空 = 不注册 `/api/config`。**注册与否必须由调用方明确决定**，
  /// 不能靠"注册了但功能不可用"：那样请求会落到静态兜底路由上，SPA 模式下
  /// 拿到的是 200 + 一份 index.html，前端 `JSON.parse` 报的错和真正的原因
  /// 毫无关系。
  std::function<std::string()> config_get;
  /// 返回 true 表示已应用并落盘；false 时 err 填给人看的说明。
  /// 成功时 `out` 填上本次**逐字段**的生效情况（见 `ConfigApplyResult`）。
  std::function<bool(std::string const& body, ConfigApplyResult& out,
                     std::string& err)> config_set;
  /// 已保存但**尚未生效**的字段（点分路径）。`GET /api/config` 会把它一并
  /// 返回（顶层 `pending` 数组）。
  ///
  /// 存在的理由只有一条：`POST` 那个回应是**一次性**的。用户存完配置、刷新
  /// 一下页面，那条"有几项等着重启"的提示就没了，而表单里显示的又都是新值
  /// —— 于是看起来一切已生效。把它挂在 GET 上，提示就跟配置本身一样活得久。
  std::function<std::vector<std::string>()> config_pending;

  /// `GET /snapshot.jpg` 的取帧函数。返回空 = 还没有任何帧。
  ///
  /// 用回调而不是让 Web 层自己持一个 `CaptureSession`：依赖方向是 app → web，
  /// 反过来 Web 就得知道帧池、编码线程这些事，而它需要的只是"给我一张 BGRA"。
  ///
  /// 路由路径取 `onvif.snapshot_path`（默认 `/snapshot.jpg`），**刻意和
  /// `GetSnapshotUri` 报出去的那个共用同一个字段** —— 分成两处写死的话，
  /// 改了一边就会出现"NVR 拿到了 URL 却 404"，而两边看起来都没错。
  std::function<std::shared_ptr<BgraFrame const>()> snapshot_source;

  /// 开 ONVIF 的 SOAP 端点（`POST /onvif/device_service` 与 `/onvif/media_service`）。
  bool onvif_enabled = true;
  /// ONVIF 的身份、凭据与码流描述。**凭据也在这里**（`onvif.user/password/realm`）
  /// —— 和 WS-Discovery 无关（发现阶段本来就不鉴权），只用于 SOAP 的 Digest。
  ///
  /// 调用方至少要填 `user` / `password` / `rtsp_port` / `profiles`；
  /// `http_port` 由 `start()` 在 bind 之后自己填真实值。
  OnvifServiceOptions onvif;

  /// 开 WS-Discovery（UDP 3702）。关掉之后 NVR 只能手工填地址添加。
  bool wsd_enabled = true;
  /// 设备 UUID（`urn:uuid:...`）。空 = 用本机派生的稳定值。
  std::string device_uuid;
  /// 设备的 scope 列表，会原样出现在 ProbeMatch 里。
  std::vector<std::string> device_scopes;
};

class WebServer {
 public:
  /// **进程里第一件要做的事**，早于任何其它 libuvcpp 调用。
  ///
  /// libuv 的线程池默认只有 4 个线程，而静态文件、配置读写、快照编码全排在
  /// 同一条队上。libuvcpp 自己会在第一次往池子里投递时打一条 WARN 提醒
  /// 「现在还没投过，这一调还来得及」—— 那句"来得及"的窗口就是这里。
  ///
  /// 做成函数而不是让调用方直接 `#include <uvcpp/uvcpp_threadpool.h>`：
  /// 那个头会拉进 `uv/win.h`，而 main.cpp 已经通过采集层拉进了 windows.h，
  /// 两者对 `RTL_SRWLOCK` 之类的定义会打架（报错完全看不出是包含顺序问题）。
  /// 把 uv.h 关在 web 这一层里，别的地方就不用记这件事。
  ///
  /// 参数 0 = 按 CPU 核数。幂等；调晚了库会返回 UV_EALREADY 且**不生效**。
  static void init_runtime();

  WebServer();
  ~WebServer();

  WebServer(WebServer const&) = delete;
  WebServer& operator=(WebServer const&) = delete;

  /// 注册路由并起服务。**阻塞到 bind 有结果**（这是 libuvcpp 的语义）。
  /// 失败时 err 填人话，且不会有线程残留。
  bool start(WebOptions const& opt, std::string& err);
  /// 优雅停机。幂等。可从任何线程调。
  void stop();

  /// 线程安全。采集线程按秒调，内部投递到 web 循环再广播给 `/ws/stats`。
  void publish(SessionStats const& s);

  /// `/ws/preview` 的一块字节。**从编码线程调用**，内部只投递。
  ///
  /// `is_init = true` 表示这是 fMP4 的初始化段（ftyp+moov）。**顺序由调用方
  /// 保证**：初始化段必须先于任何分片出现 —— MSE 只认第一个 append 进去的
  /// 初始化段，晚一个字节后面所有分片都会被拒，而且报错指不到真正的原因。
  /// 调用方（采集会话）是单线程发的，`post()` 对同一来源保序，所以这里
  /// 不再另设序号。
  ///
  /// 初始化段会被**留存**：中途连上来的客户端要靠它才能解码，而封装器只在
  /// 会话开始时产一次。分片不留存 —— 每个分片都从关键帧开始，新客户端从
  /// 下一个分片就能出画，回放历史只是白白吃内存。
  void publish_preview(bool is_init,
                       std::shared_ptr<std::vector<std::uint8_t> const> bytes);

  /// 当前 `/ws/preview` 的订阅者数量。线程安全（原子读）。
  std::size_t preview_clients() const;

  /// 实际监听端口（配置里写 0 时由系统分配）。线程安全。
  int port() const;
  /// 当前 `/ws/stats` 的订阅者数量。线程安全（原子读）。
  std::size_t stats_clients() const;

  /// WS-Discovery 的计数（收到 / 回答）。线程安全。
  unsigned long long wsd_probes_seen() const;
  unsigned long long wsd_probes_answered() const;

  /// ONVIF SOAP 的计数（总请求 / 鉴权失败 / Fault）。线程安全。
  unsigned long long onvif_requests() const;
  unsigned long long onvif_auth_failures() const;
  unsigned long long onvif_faults() const;

  /// 运行中改 ONVIF 的身份与凭据。**这是整个配置里唯一能热生效的一块**，
  /// 理由见 `OnvifIdentity` 的注释（它不碰编码器、不碰 socket，而且只在循环
  /// 线程上被读）。
  ///
  /// **只能在循环线程上调**，而且**必须同步完成**：Web 的 `/api/config`
  /// 处理器就在那条线程上跑，所以那边直接调即可。换成 `app.post()` 投一帧
  /// 过去看似更"安全"，但在别的线程上调时它只是把函数排进队列 —— 调用方会
  /// 立刻往下走，然后回报 `applied: ["onvif.password"]`，而那一刻它其实
  /// 还没生效。**这正是这个功能要防的那句谎。**
  ///
  /// **返回值 = 这份身份是不是真的进了运行中的服务。** ONVIF 没开时返回
  /// false 且什么都不做 —— 没有服务实例可改，而"改一个没开的服务的身份"
  /// 不是错误，用户按保存时不该收到一条失败。
  ///
  /// 调用方**必须**用这个返回值决定要不要把字段记进 `applied`。ONVIF 关着
  /// 的时候改口令，实际效果是"下次带着 ONVIF 启动时才生效" —— 报成已生效
  /// 就是在骗人，而用户不会有任何办法发现。
  bool apply_onvif_identity(OnvifIdentity const& id);

  /// 运行中改 Web 控制台的登录开关与凭据。**和 `apply_onvif_identity` 同一套
  /// 纪律**：只能在循环线程上调（`/api/config` 的处理器就在那条线程上），同步
  /// 生效，调用方据此决定这几个字段进 `applied` 还是 `pending`。
  ///
  /// 安全性来自同一件事：`SessionStore` 只在循环线程上被读写，改它就是换几个
  /// 字符串 + 一个 bool，不碰 socket、不碰已经发出去的响应。
  ///
  /// **改口令不会把已登录的人踢下线。** 令牌签的是每进程一份的随机密钥而不是
  /// 口令，所以旧 cookie 照样有效 —— 这是刻意的，理由见 web_session.h。
  /// 真要把所有人踢掉只有一条路：重启进程（密钥换了，全部令牌立刻失效）。
  ///
  /// 返回值 = Web 服务是不是真的在跑（没跑就没有任何东西在被拦着，"已生效"
  /// 会是假话）。Web 服务在跑的时候永远是 true。
  bool apply_web_credentials(bool enabled, std::string const& user,
                             std::string const& password);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace d2o
