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
#include "common/audit_log.h"
#include "onvif/onvif_service.h"
#include "onvif/wsd.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace d2o {

struct SessionStats;
class DeviceState;

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
  /// 给用户看的句子，**不是**"改了哪些字段"（那两样由 applied/pending 说）。
  ///
  /// 目前只有一个来源：提交里带了改不动的字段（硬件码那三个），被拒绝时
  /// 说明一句。**静默忽略是不行的** —— 用户看到"保存成功"、值却没变，会
  /// 以为是程序坏了；而下一个人读代码时会以为这个校验根本没做。
  /// 它最终被拼进 `/api/config` POST 响应里那个 `note` 字符串。
  std::vector<std::string> notes;
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

  /// `POST /api/reload` —— 用**当前保存的配置**重开采集/推流管线。
  ///
  /// 存在的理由是"一键降档"必须当场兑现：用户看到"性能不足，建议降到流畅"、
  /// 点了确认，如果只是写了个要重启才生效的配置，那他看到的就是一个**什么都没
  /// 发生**的按钮 —— 而这种按钮比没有按钮更坏，它会让人以为是软件坏了。
  ///
  /// **返回值是"当前有没有一条管线在跑"，不是成功/失败。** 这个操作没有失败
  /// 那条路：
  ///   * 有管线在跑 → 置一个重配位，采集线程下一拍体面收尾、按新配置重开；
  ///   * 没有管线在跑（比如已经停了录制）→ **请求本来就是空操作**，因为分段
  ///     循环每一趟都用当前配置重算一遍选项，下一段自然就是新的。
  ///
  /// 所以这里不能回 bool 的成功与否：那会把"没什么可重开"这件事报成一个错误，
  /// 而它恰恰是正常情况。
  ///
  /// 留空 = 不注册 `/api/reload`（同 `config_get` 的纪律：宁可没有这条路，
  /// 也不要一条落到静态兜底上、返回 200 + 一份 index.html 的路）。
  std::function<bool()> reload;

  /// `POST /api/restart` —— **让这个进程退出再起来**。留空 = 不注册这条路由。
  ///
  /// 和上面 `reload` 名字像、事情差得远，三个名字的分工是刻意的：
  ///   * `/api/onvif/restart` —— 只重播一次 WS-Discovery 组播（画面一秒不断）
  ///   * `/api/reload`        —— 重开采集管线（录制分段、RTSP 重连，进程还在）
  ///   * `/api/restart`       —— 进程本身重来（上面两样全都会发生）
  ///
  /// **回调只置一个原子位，不做任何实事。** 它跑在 web 循环线程上（在
  /// `resp.on_sent` 那之后同一拍内），而"停采集、收尾、重启"是主线程那条分段
  /// 循环该做的事 —— 也**必须**是它做：`session.request_stop()` 要人去 join，
  /// 而在这里做等于从循环线程里把采集线程拆掉。
  ///
  /// 实现方还要注意：**响应是真的发出去了才会调它**（框架在整包响应入队之后
  /// 同步触发），而"入队"不等于"已上网络"。中间隔着主线程一拍（≥40 ms），
  /// 足够；但这不是可以拿来做更长事情的许可。
  std::function<void()> restart;

  /// `POST /api/factory-reset` —— 删掉配置回到全新状态，然后重启。留空 = 不注册。
  ///
  /// 同样只置位。**调用方必须自己保证**这条路上 400 那一半是真的 400
  /// （body 不是 `{"confirm":"RESET"}` 就什么都不做）—— Web 层只负责挡掉形状
  /// 不对的请求，语义上"要不要真的恢复出厂"由调用方决定。
  ///
  /// ⚠️ **两个参数是"来源 + 身份"，与 `restart` 那个空的签名不同。** 差别来自
  /// 这一件事会不会把自己写的那行日志删掉：`restart` 写在 web 层、之后没人
  /// 动它，所以 web 层知道来源就够了；而恢复出厂在收尾时会 `audit->clear()`，
  /// web 层写的那一行**会被自己删掉**，于是"谁干的"必须先被递到主线程手上，
  /// 由清完之后那一行重新报出来（见 `factory_reset_files` 第 5 步）。
  /// **这是本轮唯一改过签名的回调** —— `config_set` / `on_patch` 一个都没动，
  /// 动它们是这堆改动里最容易碰坏别处的那个。
  std::function<void(std::string const& source, std::string const& identity)>
      factory_reset;

  /// `GET /api/time` —— 时间设置那一页要的**一切**，回一个 JSON 对象。留空 = 不注册。
  ///
  /// 两半来源不同，故意不合成一个：
  ///   * "设备此刻在用什么"（`offset_s` / `device_now` / `synced` / `synced_at`）
  ///     —— 只有 `DeviceState` 知道。NTP 模式下它与配置里那个 `offset_s`
  ///     **本来就该不一样**，见 `time_set` 的注释。
  ///   * "对时线程在干什么"（`last_sync_*` / `syncing` / 两个计数）
  ///     —— 只有 `NtpSyncer` 知道。
  ///
  /// 回字符串而不是 `AppConfig` 的一部分：它一半**不是配置**（是这台设备此刻的
  /// 状态），混进去会让人以为能存下来 —— 与 `audio_devices` 那条同一个理由。
  std::function<std::string()> time_get;

  /// `POST /api/time` —— 手动校准。body 是 `{"offset_s": n}` 或
  /// `{"device_now":"2026-10-08T15:30:00"}`（后者由服务端算偏移，按本机时区解析）。
  ///
  /// 回 `false` + `err` 就是 400 + 那句话。**实现方至少要挡住两种**：body 里
  /// 哪个字段都没有；以及当前是 NTP 模式（那时手动设的值会被下一次对时覆盖，
  /// 说清楚比让它"设成功了再自己变回去"好）。留空 = 不注册。
  std::function<bool(std::string const& body, std::string& err)> time_set;

  /// `POST /api/time/reset` —— 偏移归零（设备时间 = 主机时间）。留空 = 不注册。
  ///
  /// 单独一条路而不是让前端发 `{"offset_s":0}`：**归零要额外做一件事** ——
  /// 偏移本来就是 0、而 `synced` 仍是 true 的时候（NVR 对过一次恰好对齐的时），
  /// 只写配置不会让 `DeviceState` 动一下，于是界面上会一直挂着"已被同步过"。
  std::function<bool(std::string& err)> time_reset;

  /// `POST /api/time/sync` —— 立刻 NTP 对一次。**只置位**，不做事，回 202。
  ///
  /// 与 `restart` 同一条纪律：UDP 查询最长 2 秒、还要乘上地址数，在循环线程上
  /// 等它等于把 WS 推送与 SOAP 一起堵住。真正的查询交给对时线程，前端轮询
  /// `GET /api/time` 的 `syncing` / `last_sync_state` 看结果。
  std::function<void()> time_sync;

  /// `GET /api/audio/devices` —— 列出可选的音频端点，给音频设置里那个下拉框用。
  ///
  /// 回的是一个 JSON 对象（`{"capture":[…],"render":[…]}`），不是 `AppConfig`
  /// 的一部分：设备列表**不是配置**，它是这台机器此刻的状态 —— 插拔一个耳机
  /// 就会变，而且每台机器都不一样。混进配置里会让人以为它能存下来。
  ///
  /// 用回调而不是让 Web 层自己调 `list_audio_endpoints()`：理由和
  /// `snapshot_source` 是同一条 —— 依赖方向是 app → web，反过来 Web 就得知道
  /// COM、端点、WASAPI 这些事。（**COM 的线程亲和不是理由**：那个函数自己带了
  /// `ComApartment`，每次调用在**当前线程**上 `CoInitializeEx` 一次并容忍
  /// `RPC_E_CHANGED_MODE`，所以在循环线程上直接调其实是安全的。写在这里免得
  /// 下一个读的人以为这里有并发限制。）
  std::function<std::string()> audio_devices;

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

  /// 设备状态（设备时钟、摄像头名称）。**不持有**，调用方保证它活得比
  /// WebServer 长。
  ///
  /// 它同时是"水印上的名称"和"ONVIF 报出去的名称"的来源，也是
  /// `SetSystemDateAndTime` 的落点 —— 所以采集线程和 ONVIF 必须看到**同一个**
  /// 对象，各自存一份的话 NVR 改完名水印不会变，而两边看起来都对。
  ///
  /// 空 = 不接（`GetSystemDateAndTime` 退回主机时间，`SetSystemDateAndTime`
  /// 回 `ter:ActionNotSupported`）。
  DeviceState* device_state = nullptr;

  /// 开 WS-Discovery（UDP 3702）。关掉之后 NVR 只能手工填地址添加。
  bool wsd_enabled = true;
  /// 设备 UUID（`urn:uuid:...`）。空 = 用本机派生的稳定值。
  std::string device_uuid;
  /// 设备的 scope 列表，会原样出现在 ProbeMatch 里。
  std::vector<std::string> device_scopes;

  /// 通道 2 起要**一并宣告**的端点（每路 ONVIF 端口一个）。
  ///
  /// **只对 `wsd_enabled` 的那个 WebServer 有意义。** 多通道下每个通道各有一个
  /// WebServer，但只有一个绑得上 UDP 3702 —— 所以通道 1 那个 WebServer 拿着
  /// 全表，通道 2 起一律 `wsd_enabled = false` 且这一项留空。给它们也填上只会
  /// 让人以为"每个通道各自宣告"，而实际生效的只有一份。
  ///
  /// 空 = 只有本机这一台设备（第五轮之前的行为，逐字节相同）。
  std::vector<WsdEndpoint> wsd_extra_endpoints;

  /// 全部通道（含通道 1）此刻的状态，一个 JSON 数组。
  ///
  /// 每个元素：`{index, name, source, state, fps, bitrate_kbps, record_file,
  /// record_bytes}`。**只有安装了这个回调的那个 WebServer 会发这个字段**
  /// —— 多通道下每路通道各有一个 WebServer，但状态是**全机一份**，所以只有
  /// 通道 1 那个（绑 `web.port` 的、用户在浏览器里打开的那个）填它。通道 2 起的
  /// WebServer 只为 ONVIF 存在，它们的 `/api/status` 没人看。
  ///
  /// 回字符串而不是 `std::vector<ChannelStatus>`：与 `audio_devices` /
  /// `audit_read` 同一条纪律 —— Web 层于是完全不需要认识通道那一套结构，
  /// 而"通道在界面上叫什么、空档怎么显示"这些**呈现**上的决定全留在
  /// `main.cpp` 一处。
  ///
  /// 留空 = `channels` 恒为 `[]`（单通道构建、单元测试、以及所有不关心它的
  /// 路径）。**字段本身照样发** —— 少一个键会让 `tests\web_contract.py` 的
  /// [4] 段字段集合相等断言变红，而前端那一格会恒显示初值。
  std::function<std::string()> channels_json;

  /// 本机**能当通道来源的东西**：显示输出 + 摄像头。一个 JSON 对象
  /// `{"displays":[{index,description,width,height,x,y,capturable}],"cameras":[…]}`
  /// —— 配置页的「通道」那一段靠它把"有什么可以加"摆出来。
  ///
  /// 回字符串而不是 `std::vector<...>`：与 `channels_json` / `audio_devices` 同
  /// 一条纪律，Web 层于是完全不需要认识 DXGI 和 Media Foundation。
  ///
  /// ⚠️ **它必须是"启动时算好的一份快照"，不能每次请求现枚举** —— 可捕获性
  /// 要真的建一个 `IDXGIOutputDuplication` 去探，而一块屏同时只允许一个
  /// duplication（见 `channel_plan.h` 规则 2）。现枚举的话，用户一打开配置页
  /// 就会让**正在采集的那一路**失败。这一条是实测过的，别改成"顺手刷新一下"。
  ///
  /// 留空 = `/api/sources` 恒回空表（单通道构建、单元测试）。
  std::function<std::string()> sources_json;

  /// 网页预览"现在看第几路"。
  ///
  /// 控制台只有一个（绑 `web.port` 那个），而画面有 N 路可选 —— 于是"看哪一路"
  /// 是**全机一份**的状态，不是每个连接各一份：一次只看一路（用户选的形态），
  /// 而 `/api/snapshot.jpg` 那条无状态的 GET 也必须跟着同一路，否则界面上是
  /// 通道 2、按快门存下来的却是通道 1。
  ///
  /// 两个回调**成对**存在：只有 `preview_channel_set` 在、并且它拒了那个号，
  /// 才会回 400。留空 = 只有一路，`get` 恒回 1、`set` 只接受 1。
  std::function<int()> preview_channel_get;
  std::function<bool(int)> preview_channel_set;

  /// 操作日志（审计）的写入点。留空 = 不记。
  ///
  /// **为什么是回调而不是让 Web 层持一个 `AuditLog&`**：依赖方向与
  /// `config_set` 那条同源（app → web），而且这里还有第二个、更硬的理由 ——
  /// 写审计的**决策**（记什么、怎么脱敏、哪些要限流）必须和读它的那一侧
  /// （`main.cpp` 的 `config.set` 有一条"只记字段名不记值"的硬纪律）待在
  /// 一起。Web 层只负责回答"谁、从哪来"，不负责决定记成什么样。
  ///
  /// **签名与 `OnvifServiceOptions::audit` 一字不差**，理由也一样：写条目的是
  /// **知道来源的那一层**，而不是发起动作的那一层。`config_set` 的签名里没有
  /// IP（`bool(body, out, err)`），改它才是把这件事做坏的走法。
  std::function<void(AuditWho const& who, std::string const& action,
                     std::string const& detail)>
      audit;

  /// `GET /api/audit?days=N&limit=M&q=关键字` —— 读操作日志，回**完整的 JSON
  /// 文本**。留空 = 不注册这条路由。
  ///
  /// 回字符串而不是 `std::vector<AuditEntry>`：与 `audio_devices` 同一个理由
  /// —— Web 层于是完全不需要认识 `AuditLog`（连它的头文件都只是被签名带上
  /// 的），而"按天分组、倒序、关键字过滤、留存策略那句话"这些**呈现**上的决定
  /// 全留在 `main.cpp` 一处。多一次拷贝，换掉一个会两头漂的结构。
  std::function<std::string(int days, int limit, std::string const& keyword)>
      audit_read;
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

  /// 只把**这一路真实的编码尺寸**告诉 ONVIF（`GetProfiles` 里那两个
  /// `tt:Resolution`）。
  ///
  /// `publish()` 干的是同一件事，但它要的是一整份 `SessionStats`，而通道 2 起
  /// 那几路**没有**这条逐秒管道 —— 它们自己的 `WebServer` 只服务 ONVIF SOAP，
  /// 没有任何人会去 `/ws/stats`。后果是那些端口的 profile 永远停在启动时那个
  /// **猜**出来的尺寸上（`main.cpp` 里按 1920x1080 推的子码流宽度），而通道 2
  /// 实际编的是别的尺寸 —— NVR 报的分辨率和画面对不上，且谁也不报错。
  ///
  /// 所以给它们一条只送尺寸的窄管道。`sw`/`sh` 传 0 = 这一路没开子码流。
  /// 线程安全，可从采集线程调（和 `publish` 同一套投递与停机会合逻辑）。
  void publish_video_size(int w, int h, int sub_w, int sub_h);

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

  /// 报告"媒体时间 0 点对应的墙钟"（Unix 毫秒）。**一次会话只调一次**，
  /// 由采集会话在封装器定下 0 点之后调（见 `CaptureSession::set_preview_t0_hook`）。
  ///
  /// 它不参与 fMP4 的字节流 —— 走的是一条**文本**控制消息（`{"type":"t0",…}`），
  /// 位置在初始化段那条通道**之外**。前端的"真实延迟"尺子靠它把
  /// `<video>.currentTime` 换算成墙钟，没有它前端只能显示"未知"。
  ///
  /// **留存的，而且还会广播给当前连着的客户端**：前者管"之后连上来的"，
  /// 后者管"在 0 点定下来之前就连上的"（首页一加载就连，那时通常还没有 0 点）
  /// —— 两种都要能答，缺哪一种都有一半客户端拿不到这个数。
  ///
  /// 线程安全，可在任意线程调用。`t0_wall_ms <= 0` 直接忽略（= 采集侧没有
  /// 可报的值，比如预览这条路没在封装）。
  void publish_preview_t0(std::int64_t t0_wall_ms);

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
  /// false —— 没有服务实例可改，而"改一个没开的服务的身份"不是错误，用户按
  /// 保存时不该收到一条失败。
  ///
  /// **但身份仍然会被推进 `DeviceState`**（`device_name` 同时是水印上那个
  /// 名称，而水印不依赖 ONVIF）。所以"返回 false"只说明 SOAP 那侧要重启，
  /// 不说明什么都没发生 —— 调用方据此决定字段进 `applied` 还是 `pending`，
  /// 而两者都是真话。
  bool apply_onvif_identity(OnvifIdentity const& id);

  /// 运行中改水印（OSD）配置。和 `apply_onvif_identity` 同一套纪律：只能在
  /// 循环线程上调，同步生效。
  ///
  /// **没有返回值，这是刻意的**（水印改成热生效时一起改的，见 Phase S）。
  /// 从前它返回一个**恒为 false** 的 bool，用途是让调用方把 `osd.items` 记进
  /// `pending` —— 因为那时画面上的水印用的是采集启动时那份快照
  /// （`SessionOptions::osd` 在 `run()` 顶上取一次），改它必须重建整条管线。
  /// 现在水印走 `LiveOsd`，改完**下一帧就变**，`applied` / `pending` 由调用方
  /// 自己写 `running_cfg.osd` 来定（见 `main.cpp` 的 `apply_config`）。
  /// 这时再留一个 bool 就等于多出第二个"谁说了算"的地方，而两个来源迟早不一致
  /// —— 症状恰好是这套机制要防的那一类：界面说已生效、画面没变（或者反过来）。
  ///
  /// 所以这个函数现在只做一件事：**把新的那一份推给 SOAP 那侧**，因为
  /// `GetOSDs` 报的就是它。不推的话，用户在网页上删掉一项水印、转头在 NVR 的
  /// OSD 页面又看到它 —— 那只能理解成"没删掉"。
  void apply_osd_config(OsdConfig const& c);

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
