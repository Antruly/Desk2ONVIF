#include "web/web_server.h"

#include "app/capture_session.h"
#include "common/log.h"
#include "common/version.h"
#include "encoder/mjpeg_encoder.h"
#include "onvif/net_identity.h"
#include "onvif/wsd.h"
#include "web/web_session.h"

#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_static.h>
#include <webapp/uvcpp_web_ws.h>
#include <uvcpp/uvcpp_threadpool.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace d2o {
namespace {

// libuvcpp 的 webapp 全部在 `namespace uvcpp` 里（各头文件已逐一核对）。
// 这里集中引一次，免得正文里到处写 `uvcpp::` 把路由逻辑淹掉。
using uvcpp::log_level;
using uvcpp::uvcpp_json;
using uvcpp::uvcpp_web_app;
using uvcpp::uvcpp_web_next;
using uvcpp::uvcpp_web_request;
using uvcpp::uvcpp_web_response;
using uvcpp::uvcpp_web_sent_info;
using uvcpp::uvcpp_web_static;
using uvcpp::uvcpp_web_static_options;
using uvcpp::uvcpp_web_ws_request;
using uvcpp::ws_close_code;
using uvcpp::uvcpp_ws_connection;

/// 秒级墙钟。令牌的有效期是"7 天"这种量级，秒就够了，而且它是**签名载荷**
/// 的一部分 —— 换成毫秒会让"重放一个今天签发的令牌"这件事的窗口变得没必要地
/// 精确，而每台机器上 `time()` 的粒度都已经够用。
std::uint64_t now_s() { return static_cast<std::uint64_t>(::time(nullptr)); }

/// 路由把响应写完就必须 `end()`：它是"我说完了"的标记。不调不会丢响应，
/// 但框架会在收尾时打一条 WARN —— 而一条每个请求都出现的 WARN 会淹没真的
/// 警告，所以这里每个出口都老实调。
void send_json(uvcpp_web_response& resp, uvcpp_json const& j, int code = 200) {
  resp.status(code);
  resp.json(j);
  resp.end();
}

/// 客户端**现在**是不是带着一份有效的会话。
///
/// **不碰凭证直接返回 true，只在真的开着门的时候才拦人。** 空口令在
/// `SessionStore::effective()` 里已经判成"没门"（见那里的注释：空口令不是
/// "不鉴权"而是"口令就是空"，摆一道假门比承认没门更坏）。
bool has_session(uvcpp_web_request const& req, SessionStore const& s) {
  if (!s.effective()) return true;
  std::string const* tok = req.cookie(kSessionCookie);
  return tok != nullptr && !tok->empty() && s.verify(*tok, now_s());
}

/// 这条路径过不过登录这一关。
///
/// 只有 `/api/*` 要过。理由分三类：
///
///   - **静态资源不用过** —— 它们就是登录页本身（index.html 和那几个 chunk）。
///     拦了的话没登录的人连登录框都下载不到，只能看到一个 401 的空白页。
///     静态产物里没有任何机密，把它关起来防不住任何人。
///   - **`/onvif/*` 不用过** —— 那条路上是 NVR，凭据是另一套（Digest），
///     由 `OnvifService` 自己按 ONVIF 的规矩挑战。让它再走一遍 Web 会话等于
///     要求 NVR 先来网页登录一次。
///   - **快照是两条路径，别把它们看成一回事** —— NVR 那条
///     （`onvif.snapshot_path`，默认 `/snapshot.jpg`）**不在 `/api/` 下**，
///     本来就到不了这里；它自己按 ONVIF 的规矩挑战。网页用的那条是
///     `/api/snapshot.jpg`，**它要过这一关**，而且必须过 —— 它永远不发
///     Digest 挑战（发了浏览器就会对一张普通图片弹出原生的口令框，而那个
///     框里输的东西进不了我们的会话，用户完全不知道发生了什么）。
///
/// `/ws/*` **根本不经过这里**：`on_ws_upgrade` 直接调 WS 处理器，中间件链
/// 那一整条不参与（已核对 `uvcpp_web_app::on_ws_upgrade`）。所以两条 WS 路由
/// 的守卫写在各自的处理器里，见 `on_stats_socket` / `on_preview_socket`。
bool gate_covers(std::string const& path) {
  if (path.compare(0, 5, "/api/") != 0) return false;
  // 这三条必须匿名可达：前端打开的第一件事就是问"我登录了吗"，
  // 而"登录"本身当然不能在"已登录"后面。
  return path != "/api/login" && path != "/api/session" && path != "/api/logout";
}

uvcpp_json stats_json(SessionStats const& s) {
  uvcpp_json j;
  j["elapsed_s"] = s.elapsed_s;
  j["ticks"] = s.ticks;
  j["content_frames"] = s.content_frames;
  j["packets"] = s.packets;
  j["keyframes"] = s.keyframes;
  j["bytes"] = s.bytes;
  j["effective_fps"] = s.effective_fps;
  j["queue_dropped"] = s.queue_dropped;
  j["pool_exhausted"] = s.pool_exhausted;
  j["access_lost"] = s.access_lost;
  j["cursor_frames"] = s.cursor_frames;
  j["pointer_only_frames"] = s.pointer_only_frames;

  // --- 自适应降载 ---------------------------------------------------------
  // `drops_recent` 和上面那些累计量**并列但不同量纲**：它数的是"最近 10 秒
  // 丢了多少"，所以它回到 0 是常态。只有累计的 `queue_dropped` 时，用户看到
  // 一个四位数会以为一直在丢，而那个数多半是启动那一下（编码器还没热起来）
  // 造成的，之后再没涨过 —— 一个不会归零的计数器没法回答"现在还好吗"。
  j["adaptive_dropped"] = s.adaptive_dropped;
  j["osd_refresh_frames"] = s.osd_refresh_frames;
  // 水印热生效的次数（有人改了配置，采集侧真的换上了）。**和上面那个无关**：
  // 那个数的是"钟在走"，这个数的是"配置变了"。它是 `pending` 里那条否定式
  // 证据（"`osd.*` 不在 pending 里"）的肯定式伴 —— 见 capture_session.h。
  j["osd_hot_applied"] = s.osd_hot_applied;
  j["drops_recent"] = s.drops_recent;
  j["perf_state"] = s.perf_state;
  j["perf_duty"] = s.perf_duty;
  j["perf_target_fps"] = s.perf_target_fps;
  j["perf_hint"] = s.perf_hint;
  // 空串 = 没有建议。**不用 "none" 这类哨兵字面量**：前端要判的是"有没有
  // 一个能点的按钮"，空串在 JS 里就是假值，而多一个字面量等于多一处两边
  // 必须一致的口头约定。
  j["perf_suggested_quality"] = s.perf_suggested_quality;

  // --- 单路时代的四个键 ---------------------------------------------------
  // 前端的「链路质量」那组卡片读的就是它们，所以**必须继续存在**（改前端是
  // 另一件事，而后端先改字段名的后果是页面上四处数字变成空白，控制台一句
  // 错都不报）。语义按"所有路合起来"取，这样它们和只有一路时完全等价：
  //   nalu_dropped  各路的和 —— 那一组卡的标题就是"链路有损"，看的是总量
  //   max_nalu_bytes 各路的**最大**值。求和在这里没有意义（说的是单个 NALU
  //                 多大，而单帧大小不因为多了一路就变大），取最大才是
  //                 "有没有帧大到有截断风险"这个问题的答案
  //   rtsp_clients  各路的和
  //   rtsp_url      第 0 路（主码流）。手动给 NVR 添加通道时填的就是它。
  std::uint64_t dropped = 0;
  std::size_t max_nalu = 0;
  unsigned clients = 0;
  for (SessionStats::RtspStreamStats const& st : s.rtsp_streams) {
    dropped += st.nalu_dropped;
    if (st.max_nalu_bytes > max_nalu) max_nalu = st.max_nalu_bytes;
    clients += st.clients;
  }
  j["nalu_dropped"] = dropped;
  j["max_nalu_bytes"] = max_nalu;
  j["rtsp_clients"] = clients;
  j["rtsp_url"] = s.rtsp_streams.empty() ? std::string() : s.rtsp_streams[0].url;

  // --- 逐路 ---------------------------------------------------------------
  //
  // 主码流的**实际编码尺寸**。这一对本来就在进程里（`publish()` 每次都拿它去
  // 调 `onvif.set_video_size()`，所以 ONVIF 的 profile 一直报的是真值），
  // 只是从来没有一条路把它送到网页上 —— 子码流的两个数一直在，主码流的没有。
  //
  // 它和配置里的 `capture.max_height` **不是一回事**，两个都值得显示：
  // 配置那个是**上限**（0 = 桌面原生），而桌面比上限小的时候不放大。设备信息
  // 页要回答的是"现在到底推的是多大的画面"，那只有这个数答得上来。
  j["video_width"] = s.video_width;
  j["video_height"] = s.video_height;
  j["sub_width"] = s.sub_width;
  j["sub_height"] = s.sub_height;
  uvcpp_json arr = uvcpp_json::array();
  for (SessionStats::RtspStreamStats const& st : s.rtsp_streams) {
    uvcpp_json e;
    e["path"] = st.path;
    e["url"] = st.url;
    e["clients"] = st.clients;
    e["nalu_dropped"] = st.nalu_dropped;
    e["max_nalu_bytes"] = st.max_nalu_bytes;
    e["packets"] = st.packets;
    e["bytes"] = st.bytes;
    arr.push_back(std::move(e));
  }
  j["rtsp_streams"] = std::move(arr);

  // --- 音频 ---------------------------------------------------------------
  // **这一组是"我的麦克风到底有没有在工作"的唯一反馈。** 浏览器预览是**没有
  // 声音的**（fMP4 那一路是单流 + H.264 专用），而这条取舍是明说过的
  // （README 的"没做的"）—— 于是如果状态页也不显示电平，用户打开音频之后
  // 就只能靠去 NVR 那边拉一次流才知道成没成，而那正是他最可能出问题的一步。
  //
  // `audio_state` 的四档各指一件事，**不能合成一个布尔值**：
  //   off      没开（默认就是这个，不是故障）
  //   ok       音轨在，源也按时在给
  //   nosignal 音轨在，但源这边一直没数据 —— 麦克风拔了/被独占，或者环回
  //            在系统不出声时本来就不产数据（那是**正常**的，见陷阱 81）
  //   error    编码器出错，音轨从这里断掉（视频不受影响）
  j["audio_state"] = s.audio_state;
  // 峰值电平，0..1，已经过释放（不是瞬时值 —— 瞬时值在界面上是一串看不清的
  // 跳动数字）。L/R 分开给：混成一路再显示的话，"只有左声道有声音"这种
  // 接线问题会看起来完全正常。
  j["audio_level_l"] = s.audio_level_l;
  j["audio_level_r"] = s.audio_level_r;
  // 音轨已有的**样本帧数**。除以 `audio_sample_rate` 就是音轨时长 ——
  // 前端要判"音轨是不是明显比视频短"就靠这两个数，而那是 A/V 同步出问题时
  // 最先看得出来的形状。
  j["audio_frames"] = s.audio_frames;
  j["audio_sample_rate"] = s.audio_sample_rate;
  // 编出来的 AAC 包数（含 flush 出来的那几个）。`audio_frames` 是**时间轴**，
  // 这个是**实际产出**：两个数严重对不上（比如音轨 8.000s 而包只有 3 个）
  // 就是编码器那条路出了问题，而不是采集那条路。
  j["audio_packets"] = s.audio_packets;
  // 其中由我们补零的帧数。**系统安静时它本来就涨**，所以它涨不等于故障；
  // 它持续**不涨**而电平也是 0 才说明源那边真的出了问题（补零是兜底，
  // 不是"一切正常"的证据）。分开放正是为了让这两件事分得开。
  j["audio_zero_filled"] = s.audio_zero_filled;
  return j;
}

}  // namespace

/// `/ws/preview` 的一个订阅者。
///
/// **用 shared_ptr 而不是裸指针进表**，因为发送完成回调要减 `inflight`：
/// 浏览器断线时那条连接会先被摘表，而此刻它可能还有几块字节在队列里等着
/// 回调。裸指针的话回调一进来就是 use-after-free —— 而且只在"断线时正好
/// 有数据在飞"这个窗口里出现，是最难复现的那类崩溃。
struct PreviewConn {
  uvcpp_ws_connection* c = nullptr;
  /// 已经交给 WS 层、但还没写完的字节数。**只在 loop 线程上读写。**
  std::size_t inflight = 0;
  /// 已经因为积压超限被踢掉了。
  ///
  /// 广播时先拷一份连接表再遍历（遍历中可能摘表），于是**同一轮广播里被踢掉的
  /// 那个连接仍留在快照里**。没有这个标志，它会在这一轮里再收到一块 ——
  /// 而它的连接已经被 close 了，那块要么丢掉、要么往一条正在收尾的会话上写。
  bool dropped = false;
};

/// 单个预览客户端允许积压的字节上限。
///
/// WS 层的发送队列是**无上限**的（`send_q_` 是私有 deque，没有背压接口），
/// 浏览器一旦不读（切到后台标签页、断网但 TCP 还没报错），分片就会一直堆在
/// 服务端内存里。一个 GOP 分片几百 KB、两秒一块，挂一夜就是几个 GB ——
/// 而现象只是"进程内存一直涨"，没有任何一处会报错。
///
/// 超了就**断开**这个客户端而不是丢帧：丢帧等于给它一条时间轴有洞的流，
/// 浏览器那边表现为卡住不动，用户只会去查网络。断开是它能自己恢复的错
/// （前端有重连），而且断开这件事在日志里看得见。
constexpr std::size_t kPreviewMaxInflight = 8u * 1024 * 1024;

struct WebServer::Impl {
  WebOptions opt;

  // 表的锁。**只在 loop 线程上取**（注册、摘除、广播都在那儿），
  // 别的线程读写这些表都是未定义行为。
  std::mutex mu;
  std::vector<uvcpp_ws_connection*> stats_conns;
  std::string last_stats;  ///< 最近一帧统计的 JSON 文本，供新连上来的先拿一份

  /// `/ws/preview` 的订阅者。同样只在 loop 线程上动。
  std::vector<std::shared_ptr<PreviewConn>> preview_conns;
  /// 最近一份 fMP4 初始化段。中途连上来的客户端靠它才能解码。
  std::shared_ptr<std::vector<std::uint8_t> const> preview_init;
  /// 媒体时间 0 点对应的**墙钟**（Unix 毫秒），0 = 还不知道。
  ///
  /// 它随初始化段一起在连接建立时发给客户端（见 `send_preview_t0`），前端的
  /// "真实延迟"尺子靠它把 `currentTime` 换算成墙钟。**留存一份是必需的**：
  /// 0 点是在采集侧第一个包进封装器那一刻定下来的，而那可能比第一个浏览器
  /// 连上来早得多（也可能是之后 —— 首页一加载就连，那时还没有 0 点）。
  /// 两种情况都要能答，所以"存一份 + 连上时发"和"定下来时广播"两件都得做。
  std::int64_t preview_t0_ms = 0;
  /// 计数器：封装器投了几块分片。前两个是**正常**的量，第三个才是故障。
  ///
  /// 这三个数以前是两个，而那个被叫做 `preview_dropped` 的其实在数"没有观看者
  /// 时分片被丢掉了"—— 那是常态（没人开页面时每秒都在发生），却读起来像
  /// "流在丢数据"。真正的丢数据只有一处：客户端积压超过上限被踢掉，而它
  /// **一个计数器都没有**。名字和事实反着来，比没有这个字段更糟。
  std::uint64_t preview_fragments = 0;   ///< 产出的分片总数（含没人看的）
  std::uint64_t preview_unwatched = 0;   ///< 其中"当时没有观看者"的部分
  std::uint64_t preview_kicked = 0;      ///< 因积压超限被断开的客户端数

  // `publish()`（采集线程）与 `stop()`（任意线程）的会合点。
  // 只保护"投递"这一个动作，**不保护表** —— 表归 loop 线程。
  std::mutex post_mu;
  bool stopping = false;

  std::atomic<int> port{0};
  std::atomic<std::size_t> stats_clients{0};
  std::atomic<std::size_t> preview_clients{0};
  /// 已经进了停机流程。投出去的"启动 WSD"那一帧得靠它判断要不要放弃 ——
  /// 那一帧可能在 stop() 之后才被循环取到，那时候再建句柄就没人拆了。
  std::atomic<bool> shutdown_requested{false};

  // 声明成 unique_ptr 并且**在析构函数里显式 reset()**：路由 lambda 捕获了
  // `this`，会碰上面那几个成员。靠成员声明顺序去保证"app 先死"太隐晦了 ——
  // 析构顺序是声明的逆序，一行 `app` 位置的改动就能把它变成
  // "loop 线程还在跑，它要的 mutex 已经析构了"。
  std::unique_ptr<uvcpp_web_app> app;

  /// WS-Discovery。它跟 HTTP 共用同一条事件循环（见 wsd.h），所以在
  /// `app` 之前拆、在 `app` 之后建。
  WsdResponder wsd;

  /// ONVIF 的 SOAP 服务。选项是普通结构体，**只有循环线程读写它** ——
  /// 路由 lambda 在循环线程上跑，`set_http_port()` / `set_video_size()` 也是
  /// 通过 `app->post()` 投到循环线程再调，所以不需要锁。
  OnvifService onvif;

  /// 上一次往 ONVIF 报过的画面尺寸（主/子各一份）。用来避免每秒都去写一遍
  /// 选项 —— 只有循环线程碰它。
  ///
  /// **两路各记各的**：共用一个"上次报过的尺寸"会让第二路永远等不到更新
  /// （它的尺寸和主码流不同，于是每秒钟都判定为"变了"、每秒写一次；反过来
  /// 若拿主码流的值去比，子码流就一次都不写）。两种错法都不报错，只是
  /// GetProfiles 里的分辨率不对。
  int onvif_video_w = 0;
  int onvif_video_h = 0;
  int onvif_sub_w = 0;
  int onvif_sub_h = 0;

  /// 把尺寸写进 ONVIF 的 profile（变了才写）。**只在循环线程上跑。**
  ///
  /// 单拎出来是因为它有**两个**入口：`publish()`（通道 1，每秒一份完整统计）
  /// 和 `publish_video_size()`（通道 2 起，只有尺寸这一条窄管道）。两份逻辑
  /// 各写一遍的话，"只报真值"这件事就有了两个会各自漂的版本。
  void apply_video_size(int w, int h, int sw, int sh) {
    // 只在变了的时候写，因为这是每秒一次的路径而分辨率是常量：正常情况下
    // 这里一次都不会进去。
    if (w > 0 && h > 0 && (w != onvif_video_w || h != onvif_video_h)) {
      onvif_video_w = w;
      onvif_video_h = h;
      onvif.set_video_size(0, w, h);
    }
    // 子码流同理。`sw > 0` 就是"子码流真的开着"—— 采集侧推不出更小的尺寸
    // 时会把它报成 0，那时 profile 1 根本不存在，写它没有意义。
    if (sw > 0 && sh > 0 && (sw != onvif_sub_w || sh != onvif_sub_h)) {
      onvif_sub_w = sw;
      onvif_sub_h = sh;
      onvif.set_video_size(1, sw, sh);
    }
  }

  /// 快照编码器。**只有一个实例、且只在循环线程上用**，所以它内部不需要
  /// 加锁 —— 这恰好也是把它当成员而不是每次请求现建的理由：MJPEG 的
  /// 编码器上下文建一次要分配一堆表，而快照可能被 NVR 轮询。
  MjpegEncoder mjpeg;

  /// Web 控制台的登录会话。**只在循环线程上读写**（所有处理器都在那条线程），
  /// 所以它内部那些 map / string 不需要锁。唯一的例外是 `set_credentials`
  /// 与 `set_enabled`，它们也由循环线程通过 `apply_web_credentials` 调用。
  ///
  /// 签名密钥在构造时生成一次 —— 也就是说**进程重启 = 所有会话立刻失效**，
  /// 这是这个无状态方案里唯一能"踢人"的手段（见 web_session.h）。
  SessionStore sessions;

  ~Impl() { shutdown(); }

  void shutdown() {
    shutdown_requested.store(true, std::memory_order_relaxed);
    // WSD 先拆：它的 stop() 要往循环里投一件事（发 Bye），而 app->stop()
    // 之后循环就在收尾了，那时候投进去的东西不保证跑得到。
    wsd.stop();
    if (!app) return;
    {
      std::lock_guard<std::mutex> lk(post_mu);
      stopping = true;
    }
    app->stop();
    // `join()` 从 loop 线程自己调会死锁。正常路径下 stop() 都是外部调的，
    // 但路由里若有人调 stop() 就会走到这里 —— 用 loop_started() 兜一下，
    // 总比挂死强。
    app->join();
    app.reset();
  }

  void register_routes();
  void register_auth_routes();
  void start_wsd(WebOptions const& o);
  void on_stats_socket(uvcpp_web_ws_request& ws);
  void broadcast_stats(std::string text);
  /// 把"每秒统计快照之外"的那批字段补进一份统计 JSON。
  ///
  /// **调用方必须持有 `mu`，且必须在 loop 线程上** —— 它读的 `stats_conns` /
  /// `preview_conns` 两张表、以及 `preview_fragments` 这几个**非原子**计数器，
  /// 全都只在 loop 线程上被改。
  ///
  /// 存在的理由是一个真实踩过的坑：`stats_json()`（WS 每秒推的那一帧）只带
  /// 采集侧的字段，而 ONVIF / WS-Discovery / 预览通道的这些数只挂在
  /// `GET /api/status` 上。前端两边都读，于是**走 WS 的那条路上它们恒为
  /// 初值**，界面长期显示"初始化段未产生""WS-Discovery 没在监听"，而画面
  /// 明明在播。当时没被发现，是因为对账测试只比对了 HTTP 那份（见
  /// tests/web_contract.py 的 [4] 段）。
  ///
  /// 做成一个函数而不是在两处各写一遍：两份清单迟早会漂移，而漂移的症状
  /// 正是它要防的那一种 —— 某个字段只在其中一份里，界面看起来"有数"。
  void patch_live_counters(uvcpp_json& j);
  void on_preview_socket(uvcpp_web_ws_request& ws);
  /// 这条 WS 升级带没带有效会话。**升级请求不经过任何中间件**，所以两条
  /// WS 路由都得自己问一句。
  bool ws_allowed(uvcpp_web_ws_request const& ws) const;
  /// 没带凭据的 WS 升级：拒掉。**必须在登记进任何表之前调** —— 先进表再关，
  /// 那张表里就会留下一条正在死的连接，而下一秒的广播会往它上面写。
  void reject_ws(uvcpp_ws_connection* c, char const* route,
                 std::string const& ip) const;
  /// 快照的公共部分（取帧 → 按需重建编码器 → MJPEG → 响应）。
  /// 鉴权在外面分两条路做完，这里只管干活。
  void serve_snapshot(uvcpp_web_response& resp);
  /// 给一个预览客户端发一块。积压超限就把它从表里摘掉并断开。只在 loop 线程上跑。
  void send_preview_block(std::shared_ptr<PreviewConn> conn,
                          std::shared_ptr<std::vector<std::uint8_t> const> bytes);
  /// 给一个预览客户端发一帧**文本**控制消息，说明媒体时间 0 点对应哪个墙钟。
  ///
  /// 走文本帧而不是塞进二进制流里：二进制那一路是 fMP4 字节，MSE 只认它，
  /// 中间混进任何别的东西都会让 SourceBuffer 报错。WS 本来就分文本/二进制
  /// 两种帧类型，用它区分是框架自带的能力，不需要自己在字节流里做分帧。
  /// 只在 loop 线程上跑。
  void send_preview_t0(std::shared_ptr<PreviewConn> conn);
};

void WebServer::Impl::start_wsd(WebOptions const& o) {
  if (!o.wsd_enabled) {
    // 不写"按配置"：关掉它的可能是 `--no-wsd`，也可能是 ONVIF 整个没开
    // （见 main.cpp —— WS-Discovery 跟着 ONVIF 走）。原因由那边报。
    D2O_INFO("WS-Discovery 未启用（NVR 只能手工填 URL 添加这台设备）。");
    return;
  }

  WsdOptions wo;
  wo.uuid = o.device_uuid;
  if (wo.uuid.empty()) {
    // 稳定派生：NVR 把设备 UUID 当成主键，一变就会在列表里多出一台"新"摄像机。
    wo.uuid = d2o::make_uuid_urn(machine_seed());
  }
  // **用真实绑定端口，不能用 o.port。** 配置里写 0 表示"让系统分配"，
  // 那时候 o.port 是 0 —— 把 0 写进 XAddrs 就是让 NVR 去连 80 端口。
  wo.http_port = port.load(std::memory_order_relaxed);
  if (wo.http_port == 0) {
    D2O_ERROR("WS-Discovery 拿不到实际监听端口，跳过启动。");
    return;
  }
  wo.scopes = o.device_scopes;
  wo.extra = o.wsd_extra_endpoints;
  if (wo.scopes.empty()) {
    // 类型 scope 必须带：NVR/ONVIF Device Manager 主要靠它认出"这是一台摄像机"。
    // Network_Video_Transmitter 的写法（含大写和下划线）是 ONVIF 规范里定死的。
    wo.scopes = {
        "onvif://www.onvif.org/type/video_encoder",
        "onvif://www.onvif.org/type/Network_Video_Transmitter",
        "onvif://www.onvif.org/Profile/Streaming",
        "onvif://www.onvif.org/name/Desk2ONVIF",
        "onvif://www.onvif.org/hardware/Desk2ONVIF",
        "onvif://www.onvif.org/location/desktop",
    };
  }

  // 投递函数：WSD 要发 Hello/Bye/ProbeMatch，而发送只能在循环线程上做。
  WsdPoster poster = [this](std::function<void()> fn) { app->post(std::move(fn)); };

  // **投到循环线程去建。** 端口只有 `start_background()` 返回之后才知道，
  // 而那时候循环已经在跑了 —— 从本线程去建 uv_udp_t 就是跨线程动另一条
  // 线程的 loop。在循环线程上建句柄本来就是最正常的用法，投一帧过去即可。
  app->post([this, wo = std::move(wo), poster]() {
    if (shutdown_requested.load(std::memory_order_relaxed)) return;
    std::string err;
    if (!wsd.start(app->loop(), wo, poster, err)) {
      // 不是致命错误。3702 被别的软件独占、或者没有网卡，都不该让整个
      // Web 服务起不来 —— 手工填地址加进 NVR 仍然是可用的。
      D2O_ERROR("WS-Discovery 起不来：%s", err.c_str());
      D2O_WARN("NVR 的自动发现会不可用；请在 NVR 上手工添加 rtsp 地址。");
    }
  });
}

bool WebServer::Impl::ws_allowed(uvcpp_web_ws_request const& ws) const {
  if (!sessions.effective()) return true;
  std::string const* tok = ws.cookie(kSessionCookie);
  return tok != nullptr && !tok->empty() && sessions.verify(*tok, now_s());
}

void WebServer::Impl::reject_ws(uvcpp_ws_connection* c, char const* route,
                                std::string const& ip) const {
  // 用 1008（POLICY_VIOLATION）而不是直接断：前端靠这个码区分"没登录"和
  // "网络断了"。前者要跳登录页，后者该退避重连 —— 两者都断的话，用户在
  // 登录页和被踢回登录页之间反复横跳，而真正的原因（cookie 过期）看不见。
  D2O_WARN("WS %s 拒绝接入：来自 %s 的升级请求没有有效会话（未登录或 cookie 已过期）。",
           route, ip.c_str());
  c->close(ws_close_code::POLICY_VIOLATION, "unauthenticated");
}

void WebServer::Impl::on_stats_socket(uvcpp_web_ws_request& ws) {
  uvcpp_ws_connection* c = ws.connection();
  if (c == nullptr) return;

  // **必须在登记进表之前。** 这个处理器是 WS 升级的终点，中间件链不参与
  // （见 gate_covers 的注释），所以这一句就是这条路线上唯一的守卫。
  if (!ws_allowed(ws)) {
    reject_ws(c, "/ws/stats", ws.peer_ip());
    return;
  }

  {
    std::lock_guard<std::mutex> lk(mu);
    stats_conns.push_back(c);
    stats_clients.store(stats_conns.size(), std::memory_order_relaxed);

    // 立刻推一份当前状态。不这么做的话，页面要空等下一个整秒才出数，
    // 而这期间它看起来就"没连上"。
    //
    // 推之前**必须**把 ws_clients 重新盖一次：last_stats 是上一次广播留下的
    // 文本，那时这个客户端还没进表，于是第一眼看到的是"订阅者 0" —— 一个
    // 盯着自己刚建好的连接数 0 的界面，看起来就是坏的。
    if (!last_stats.empty()) {
      uvcpp_json j = uvcpp_json::parse(last_stats, nullptr, false);
      if (!j.is_discarded()) {
        // 和广播走同一个函数，顺带把那批"不在每秒快照里"的字段也刷新一遍 ——
        // 否则刚连上来的这一帧带的是上一秒的计数。
        patch_live_counters(j);
        last_stats = j.dump();
      }
      c->send_text(last_stats.data(), last_stats.size());
    }
  }

  D2O_DEBUG("WS /ws/stats 接入：%s:%u", ws.peer_ip().c_str(), ws.peer_port());

  // 摘表**必须**挂在 on_close 上：连接对象归框架所有，会话一终结框架就回收，
  // 留下的是一个悬垂指针，而它在下一秒的广播里就会被写。
  c->on_close([this, c](ws_close_code code, std::string const&) {
    std::lock_guard<std::mutex> lk(mu);
    for (auto it = stats_conns.begin(); it != stats_conns.end(); ++it) {
      if (*it == c) {
        stats_conns.erase(it);
        break;
      }
    }
    stats_clients.store(stats_conns.size(), std::memory_order_relaxed);
    D2O_DEBUG("WS /ws/stats 断开（码 %u），剩 %zu", static_cast<unsigned>(code),
              stats_conns.size());
  });

  // 框架对升级后的连接**豁免空闲超时**（对 WS 是必要的：60 秒没消息很正常），
  // 代价是它从此没有任何超时保护，也没有内建的 ping/pong。浏览器那边
  // 断网/休眠时 TCP 可能长时间不报错，表里就一直留着这条连接。
  // 心跳放在前端做（浏览器会自动回 pong），这里不强加 —— 理由见 README。
}

void WebServer::Impl::patch_live_counters(uvcpp_json& j) {
  // 版本号补在**这里**，和 web_port 是同一个理由：`/api/status` 与 `/ws/stats`
  // 两条路的字段集合被 `tests\web_contract.py` 的 [4] 段用**相等断言**钉着，
  // 所以任何新字段都必须落在这个两条路共用的补丁点上。只加在 handler 里
  // 会让那条断言变红，而这个字段本身还"看起来是有的"（单看 HTTP 那条路全对）。
  //
  // 值来自 `project(... VERSION)`（见 common/version.h），和 exe 的
  // VersionInfo、安装包的文件名是同一个来源 —— 三方一致由两个验收脚本盯着。
  j["version"] = d2o::kVersion;
  j["web_port"] = port.load(std::memory_order_relaxed);
  // 订阅数在**这里**补，不能在 publish() 里补：publish 跑在采集线程上，它看到
  // 的订阅数最多滞后一秒，而且新连上来的第一个客户端会看到"订阅者 0" ——
  // 一个盯着自己刚建立的连接数 0 的界面看起来就是坏了的。两个调用点都持着
  // 表锁，拿到的是权威值。
  j["ws_clients"] = static_cast<std::uint64_t>(stats_conns.size());

  // ONVIF / WS-Discovery 的计数。放在统计里是刻意的：NVR"添加失败"的现场，
  // 第一个要看的就是"它到底有没有发过 Probe"和"SOAP 有没有被拒" —— 没有这几
  // 个数就只能抓包。
  j["wsd_running"] = wsd.running();
  j["wsd_probes_seen"] = wsd.probes_seen();
  j["wsd_probes_answered"] = wsd.probes_answered();
  j["onvif_requests"] = onvif.requests();
  j["onvif_auth_failures"] = onvif.auth_failures();
  j["onvif_faults"] = onvif.faults();
  j["onvif_last_unsupported"] = onvif.last_unsupported();

  // 预览通道。`preview_fragments`/`preview_unwatched`/`preview_kicked` 是
  // **非原子**的成员，但它们只在循环线程上被改（publish_preview 是投递进来的），
  // 而两个调用点都在循环线程上跑 —— 同一条线程，不需要额外加锁。
  j["preview_clients"] = static_cast<std::uint64_t>(preview_conns.size());
  j["preview_ready"] = static_cast<bool>(preview_init);
  j["preview_fragments"] = preview_fragments;
  // "没人看"是常态，不是故障 —— 字段名照着事实写，免得看状态的人去查一个
  // 根本不存在的丢包问题。
  j["preview_fragments_unwatched"] = preview_unwatched;
  j["preview_clients_kicked"] = preview_kicked;

  // --- 时间设置（Phase U）--------------------------------------------------
  //
  // 补在**这里**的理由与 `version` 一字不差：两条路的字段集合被
  // `tests\web_contract.py` 的 [4] 段相等断言钉着。
  //
  // 但值的来源与文件里其它字段都不同：**这台设备此刻的钟不在 web 这边** ——
  // 一半在 `DeviceState`（设备时间 = 主机时间 + 偏移），一半在 `NtpSyncer`
  // （对时线程的状态），两者都归 main.cpp 管。所以只能问一次 `opt.time_get`，
  // 把它的 JSON 里那几个键搬过来。
  //
  // **必须一次调用取齐**（而不是分几个 accessor 各问一次）：这六个值合起来
  // 描述同一个瞬间，分开问就可能拼出"模式是 NTP、而 last_sync_* 还是手动那次
  // 留下的"这种自相矛盾的一帧。陷阱 88 那一族说的正是这件事。
  //
  // 每帧解一次小 JSON 的代价可以忽略（一秒一次、六个键），而换成"main 那边
  // 定期推一份缓存进来"要多一条跨线程推送路径 —— 项目里已经有 `post()`
  // 那条纪律（陷阱 14 的"就地执行"），为省这一次解析去碰它不划算。
  //
  // ⚠️ **这一句是在持有 WS 表锁的情况下跑的**（两个调用点都如此）。能这么做
  // 是因为 `time_get` 只碰 `DeviceState` 与 `NtpSyncer` 的锁，而那两把锁的
  // 持有者从不回头去拿 web 的表锁 —— 没有环。哪天要给 `time_get` 加"顺带问问
  // web 侧"的活，这个前提就没了。
  //
  // 回调没注册时**照样发这几个键的默认值**：状态页读的是一组固定的字段，缺一个
  // 就是那一格显示空白，而"这条路没接"和"时间就是 0 偏移"在界面上长得一样。
  // 发默认值至少让后一个解释成立。
  {
    std::string mode = "manual";
    std::int64_t offset_s = 0;
    std::int64_t last_at = 0;
    bool synced = false;
    std::string last_state;
    std::string last_error;

    // 取值一律**先问类型再取**：`nlohmann::json::value()` 在"键在、类型不对"
    // 时是**抛异常**而不是回退到默认值，而这里跑在循环线程上、还持着 WS 表锁
    // —— 一次抛出去就是整个 WS 推送停摆，而症状（页面数字冻住）与"采集卡了"
    // 长得一样。几行样板换这条路不可能断，值。
    if (opt.time_get) {
      uvcpp_json const t = uvcpp_json::parse(opt.time_get(), nullptr, false);
      if (!t.is_discarded() && t.is_object()) {
        auto const num = [&t](char const* k) -> std::int64_t {
          return (t.contains(k) && t[k].is_number()) ? t[k].get<std::int64_t>() : 0;
        };
        auto const str = [&t](char const* k) -> std::string {
          return (t.contains(k) && t[k].is_string()) ? t[k].get<std::string>()
                                                     : std::string();
        };
        auto const boolean = [&t](char const* k) -> bool {
          return (t.contains(k) && t[k].is_boolean()) ? t[k].get<bool>() : false;
        };
        mode = str("mode");
        offset_s = num("offset_s");
        synced = boolean("synced");
        last_at = num("last_sync_at");
        last_state = str("last_sync_state");
        last_error = str("last_sync_error");
      } else {
        D2O_WARN("时间的 JSON 解不动，`/api/status` 与 `/ws/stats` 上的时间字段"
                 "这一帧发的是默认值（模式 manual、偏移 0）。");
      }
    }
    if (mode.empty()) mode = "manual";
    j["time_mode"] = mode;
    j["device_offset_s"] = offset_s;
    j["time_synced"] = synced;
    j["last_sync_at"] = last_at;
    j["last_sync_state"] = last_state;
    j["last_sync_error"] = last_error;
  }

  // --- 多通道（第五轮 Phase AD）--------------------------------------------
  //
  // 补在**这里**的理由与 `version` / 时间那一块一字不差：两条路的字段集合被
  // `tests\web_contract.py` 的 [4] 段相等断言钉着，所以新字段只能落在这个
  // 两条路共用的补丁点上。只加在 handler 里会让那条断言变红，而这个字段本身
  // 还"看起来是有的"（单看 HTTP 那条路全对）。
  //
  // 值来自 `opt.channels_json`：通道状态一半在采集线程（fps / 字节数、每秒
  // 更新），一半在主线程（这一趟建起了哪几路、录到哪个文件），而 web 循环
  // 线程两边都不是 —— 所以由 `main.cpp` 那边攒成一份 JSON 交过来（与
  // `time_get` 同一条纪律：**一次取齐**，免得拼出"状态是运行中、fps 却是上
  // 一次的 0"这种自相矛盾的一帧）。
  //
  // ⚠️ 这一句同样是**持着 WS 表锁**跑的（两个调用点都如此）。填这个回调的
  // 一方因此只能碰它自己那把锁，**绝不能回头调 web 的任何东西** —— 那才有环。
  {
    uvcpp_json arr = uvcpp_json::array();
    if (opt.channels_json) {
      uvcpp_json const parsed =
          uvcpp_json::parse(opt.channels_json(), nullptr, false);
      // 解不动就发空数组，**不是不发这个键**：前端那一格于是显示"没有通道"
      // （一个能自己解释的读数），而不是一个 JSON 里根本不存在的键 —— 后者
      // 在前端读出来是 `undefined`，页面上是一片空白，看着像页面坏了。
      if (!parsed.is_discarded() && parsed.is_array()) {
        arr = parsed;
      } else {
        D2O_WARN("通道状态的 JSON 解不动，这一帧 `channels` 发的是空数组。");
      }
    }
    j["channels"] = std::move(arr);
  }
}

void WebServer::Impl::broadcast_stats(std::string text) {
  std::lock_guard<std::mutex> lk(mu);

  // 走 WS 的这一帧和 `GET /api/status` 必须是**同一套字段**：前端两条路都读，
  // 少一个就会让界面在那个字段上恒显示初值（见 patch_live_counters 的注释）。
  {
    uvcpp_json j = uvcpp_json::parse(text, nullptr, false);
    if (!j.is_discarded()) {
      patch_live_counters(j);
      text = j.dump();
    }
  }

  last_stats = text;

  if (stats_conns.empty()) return;

  // 发送队列在 WS 层内部（见 uvcpp_ws_connection 的私有段注释），
  // 所以这里连着调不会因为 UV_EALREADY 丢帧。返回值只用来发现真错误。
  for (uvcpp_ws_connection* c : stats_conns) {
    int const rc = c->send_text(text.data(), text.size());
    if (rc != 0) {
      D2O_DEBUG("WS 统计推送失败（%d），等 on_close 收拾", rc);
    }
  }
}

void WebServer::Impl::send_preview_block(
    std::shared_ptr<PreviewConn> conn,
    std::shared_ptr<std::vector<std::uint8_t> const> bytes) {
  if (!conn || conn->dropped || !bytes || bytes->empty()) return;
  uvcpp_ws_connection* const c = conn->c;
  if (c == nullptr) return;

  if (conn->inflight + bytes->size() > kPreviewMaxInflight) {
    // 踢掉它，**而不是丢掉这一块继续发**。丢一块等于给它一条时间轴有洞的
    // 流，浏览器那边表现为"画面卡住"，用户只会去查网络；而真正的原因
    // （这个客户端读得太慢）一点线索都不留。断开是它能自愈的错（前端带重连），
    // 日志里也看得见。
    conn->dropped = true;
    ++preview_kicked;   // 唯一会真丢数据的地方，状态接口里得看得见
    D2O_WARN("预览客户端积压超过 %zu MB，断开它（前端会重连）。",
             kPreviewMaxInflight / (1024 * 1024));
    for (auto it = preview_conns.begin(); it != preview_conns.end(); ++it) {
      if (it->get() == conn.get()) {
        preview_conns.erase(it);
        break;
      }
    }
    preview_clients.store(preview_conns.size(), std::memory_order_relaxed);
    c->close();
    return;
  }

  std::size_t const n = bytes->size();
  conn->inflight += n;
  // 完成回调要**恰好减一次**，所以用一个共享的一次性开关兜住：
  // send_frame 在"队列已坏"和"提交失败"两条路上会**就地同步**回调，
  // 而那条路上本函数又会自己再补一次 —— 不减会永久踢人，减两次会把计数
  // 压到比实际低（于是背压失效）。开关让两条路怎么走都只结算一次。
  auto done = std::make_shared<bool>(false);
  auto finish = [conn, n, done](int) {
    if (*done) return;
    *done = true;
    conn->inflight = conn->inflight >= n ? conn->inflight - n : 0;
  };
  int const rc = c->send_binary(reinterpret_cast<char const*>(bytes->data()), n, finish);
  if (rc != 0) {
    finish(rc);  // 提交失败时完成回调不保证会来（见上面那段注释）
    D2O_DEBUG("预览分片发送失败（%d），等 on_close 收拾", rc);
  }
}

void WebServer::Impl::send_preview_t0(std::shared_ptr<PreviewConn> conn) {
  if (!conn || conn->dropped || conn->c == nullptr) return;
  if (preview_t0_ms <= 0) return;  // 还不知道 0 点，什么都不发

  // `server_now_ms` 用的是**同一台机器的同一个钟**（Unix 毫秒）。前端拿它算
  // 一次 `skew = Date.now() - server_now_ms` 就够：两个钟的差是这台设备与
  // 浏览器所在机器之间的偏差，而"延迟"是个**差值**，同一条式子里的 skew 会
  // 被减掉 —— 前提是它是**常数**，所以只在连上的那一刻量一次、之后不再更新
  // （每次推送都重算的话，量到的差里会混进网络往返时间，反而抖起来）。
  //
  // **这是参考、不是授权**：它只让浏览器知道"服务端现在几点"，不改变任何
  // 事实。设备时间（主机 + NVR 下发的偏移）在这里一个字都不出现 —— 那正是
  // 这条链上最容易搞混的地方（见 capture_session.cpp 里播 t0 时那段注释）。
  std::int64_t const now_ms = static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  char buf[160];
  int const n = std::snprintf(buf, sizeof(buf),
                              "{\"type\":\"t0\",\"t0_wall_ms\":%lld,"
                              "\"server_now_ms\":%lld}",
                              static_cast<long long>(preview_t0_ms),
                              static_cast<long long>(now_ms));
  if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buf)) return;

  // 不计 inflight：这一帧只有一百来个字节，而背压那道闸门是给几百 KB 的分片
  // 准备的。为它去走一遍 inflight 的加减只会让那条路径多一个能算错的地方。
  int const rc = conn->c->send_text(buf, static_cast<std::size_t>(n));
  if (rc != 0) D2O_DEBUG("预览时间原点发送失败（%d），等 on_close 收拾", rc);
}

void WebServer::Impl::on_preview_socket(uvcpp_web_ws_request& ws) {
  uvcpp_ws_connection* c = ws.connection();
  if (c == nullptr) return;

  // 和 /ws/stats 同一条理由：升级不经过中间件，守卫只能在这里。
  if (!ws_allowed(ws)) {
    reject_ws(c, "/ws/preview", ws.peer_ip());
    return;
  }

  auto conn = std::make_shared<PreviewConn>();
  conn->c = c;

  {
    std::lock_guard<std::mutex> lk(mu);
    preview_conns.push_back(conn);
    preview_clients.store(preview_conns.size(), std::memory_order_relaxed);
    // 初始化段**立刻**发。等下一次 publish_preview 是不行的：那要等采集那边
    // 下一个关键帧，而在此之前客户端手里只有一个空的 SourceBuffer ——
    // 前端的 3 秒就绪计时器会先超时，报"连上了但没数据"。
    if (preview_init) send_preview_block(conn, preview_init);
    // 时间原点**不挂在上面那条 `if` 上**：0 点是采集侧第一个包进封装器那一刻
    // 定下来的，而初始化段要等第一个关键帧 —— 默认 2 秒的 GOP 下，这中间隔着
    // 整整一个 GOP，很多时候 0 点比初始化段**先**到。挂在 `if` 里面就会让这段
    // 窗口里的客户端拿不到原点，而它们正是刚打开页面、最盯着延迟看的那批人。
    send_preview_t0(conn);
  }

  D2O_DEBUG("WS /ws/preview 接入：%s:%u", ws.peer_ip().c_str(), ws.peer_port());

  // 和 /ws/stats 一样，摘表**必须**挂 on_close：连接对象归框架所有，会话一
  // 终结就回收，留个裸指针在表里，下一块分片就会往它上面写。
  c->on_close([this, conn](ws_close_code code, std::string const&) {
    std::lock_guard<std::mutex> lk(mu);
    for (auto it = preview_conns.begin(); it != preview_conns.end(); ++it) {
      if (it->get() == conn.get()) {
        preview_conns.erase(it);
        break;
      }
    }
    preview_clients.store(preview_conns.size(), std::memory_order_relaxed);
    D2O_DEBUG("WS /ws/preview 断开（码 %u），剩 %zu", static_cast<unsigned>(code),
              preview_conns.size());
  });
}

void WebServer::Impl::serve_snapshot(uvcpp_web_response& resp) {
  std::shared_ptr<BgraFrame const> img = opt.snapshot_source();
  if (!img || img->pixels.empty() || img->width <= 0 || img->height <= 0) {
    // 503 而不是一张黑图：还没开始采集时给张黑的，NVR 那边只会显示
    // "画面正常但全黑"，而真正的原因（还没采到帧）一点线索都不留。
    resp.status(503).text("还没有采集到任何画面。").end();
    return;
  }

  // 分辨率变了要重建。放在这里而不是启动时 init 一次：采集会话可能因为
  // 分辨率切换而重建（DXGI access lost），而那之后宽高就变了。
  if (!mjpeg.ready() || mjpeg.width() != img->width ||
      mjpeg.height() != img->height) {
    if (!mjpeg.init(img->width, img->height)) {
      D2O_ERROR("快照编码器初始化失败（%dx%d）。", img->width, img->height);
      resp.status(500).text("快照编码器初始化失败。").end();
      return;
    }
  }

  std::string const jpg = mjpeg.encode(img->pixels.data(), img->stride);
  if (jpg.empty()) {
    resp.status(500).text("快照编码失败。").end();
    return;
  }

  resp.status(200);
  resp.set_content_type("image/jpeg");
  // **no-store 是必须的。** 快照的 URL 永远不变，浏览器和 NVR 都会缓存
  // 它 —— 少了这个头，界面上就永远定格在第一张，而且看起来完全正常
  // （HTTP 200、一张合理的图），只有"画面不动"这一个线索。
  resp.set_header("Cache-Control", "no-store, no-cache, must-revalidate");
  resp.body(jpg);
  resp.end();
}

void WebServer::Impl::register_auth_routes() {
  uvcpp_web_app& a = *app;

  // POST /api/login —— 用 JSON 换一个会话 cookie。
  //
  // **响应体里不放任何"是用户名错了还是口令错了"的信息**：那等于给爆破的人
  // 一个免费的账号枚举器。`check_password` 两个比较都做、且是常数时间的，
  // 所以这里如实报"用户名或口令不对"就是它的真实语义。
  a.post("/api/login", [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                              uvcpp_web_next) {
    if (!sessions.effective()) {
      // 没开门的时候说清楚，别让前端对着一个永远失败的登录框查半天。
      uvcpp_json j;
      j["ok"] = false;
      j["error"] = "本机没有启用 Web 登录（web.auth_enabled=false 或口令为空）。";
      j["auth_required"] = false;
      send_json(resp, j, 400);
      return;
    }

    // 和 SOAP / 配置一样用 body_data()/body_size()：content_length() 是声明值。
    std::string const body(req.body_data(), req.body_size());
    uvcpp_json in = uvcpp_json::parse(body, nullptr, false);
    if (in.is_discarded() || !in.is_object()) {
      uvcpp_json j;
      j["ok"] = false;
      j["error"] = "请求体必须是 JSON 对象：{\"user\":\"…\",\"password\":\"…\"}";
      send_json(resp, j, 400);
      return;
    }

    std::string user;
    std::string pass;
    try {
      if (auto const it = in.find("user"); it != in.end() && it->is_string()) {
        user = it->get<std::string>();
      }
      if (auto const it = in.find("password"); it != in.end() && it->is_string()) {
        pass = it->get<std::string>();
      }
    } catch (std::exception const&) {
      uvcpp_json j;
      j["ok"] = false;
      j["error"] = "user / password 必须是字符串。";
      send_json(resp, j, 400);
      return;
    }

    std::string const ip = req.peer_ip();
    std::uint64_t const t = now_s();

    if (sessions.throttled(ip, t)) {
      std::uint64_t const left = sessions.lock_remaining(ip, t);
      uvcpp_json j;
      j["ok"] = false;
      j["error"] = fmt("失败次数过多，请 %llu 秒后再试。",
                       static_cast<unsigned long long>(left));
      j["retry_after_s"] = left;
      resp.set_header("Retry-After", std::to_string(left));
      send_json(resp, j, 429);
      return;
    }

    if (!sessions.check_password(user, pass)) {
      sessions.note_failure(ip, t);
      // 操作日志：**记录失败，但永远不含口令**。
      //
      // 这里的写入量不需要自己再限流一次，理由是**上面那个 `throttled` 分支**
      // 已经把上限压住了：`kMaxFailures = 5` / `kWindowSeconds = 300`，
      // 而冷却期内的请求在第 816 行就 429 返回、**根本走不到这一行**。
      // 所以同一个 IP 最多每 5 分钟写 5 行 —— 也就是用户要的"同 IP 一分钟一条"
      // 那个量级，而且这个上限是**既有限速机制的副产品**，不是又一处要自己维护
      // 的计数（那种计数迟早会和真限速漂开）。
      //
      // 身份写**提交上来的那个用户名**，不是"未知"：审计要回答的正是"有人在拿
      // 哪个账号试"。口令一个字都不写 —— 这一个字都不能有。
      if (opt.audit) {
        opt.audit(AuditWho{ip, "web:" + (user.empty() ? std::string("(空)") : user)},
                  "login.fail", "用户名或口令不对，未签发会话");
      }
      uvcpp_json j;
      j["ok"] = false;
      j["error"] = "用户名或口令不对。";
      // 只报还剩几次，**不报"你已经被锁了"这种只有攻击者才需要的信息** ——
      // 反过来，把剩余次数告诉一个正在手滑的人是有用的。
      send_json(resp, j, 401);
      D2O_WARN("Web 控制台登录失败：来自 %s 的用户名 %s。", ip.c_str(),
               user.empty() ? "(空)" : user.c_str());
      return;
    }

    sessions.note_success(ip);
    if (opt.audit) {
      opt.audit(AuditWho{ip, "web:" + sessions.user()}, "login.ok",
                "登录成功，签发会话（有效期 " +
                    std::to_string(SessionStore::kTtlSeconds) + " 秒）");
    }
    std::string const token = sessions.issue(t);
    // httpOnly：中间那个 XSS 想偷 cookie 就偷不到了，而前端本来也不需要读
    // 它（要问"我登录了吗"就调 /api/session）。
    // secure=false：这个服务跑在明文 HTTP 上，置 true 会让浏览器直接不收
    // 这个 cookie —— 症状是"登录成功、下一个请求又未登录"。
    resp.set_cookie(kSessionCookie, token, "/",
                    static_cast<long>(SessionStore::kTtlSeconds), /*http_only=*/true,
                    /*secure=*/false, "Lax");
    uvcpp_json j;
    j["ok"] = true;
    j["user"] = sessions.user();
    j["expires_in_s"] = static_cast<std::uint64_t>(SessionStore::kTtlSeconds);
    send_json(resp, j);
    D2O_INFO("Web 控制台登录成功：%s（用户 %s）。", ip.c_str(), sessions.user().c_str());
  });

  // GET /api/session —— "我登录了吗"。前端进任何页面前先问这一句。
  a.get("/api/session", [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                               uvcpp_web_next) {
    bool const required = sessions.effective();
    bool const ok = has_session(req, sessions);
    uvcpp_json j;
    j["auth_required"] = required;
    j["authenticated"] = ok;
    // **没登录时不报用户名。** 它对登录框没有用（前端默认填 admin），
    // 却等于把一个"这台机器上有个叫 X 的账号"白送给任何扫端口的人。
    if (ok) j["user"] = sessions.user();
    send_json(resp, j);
  });

  // POST /api/logout —— 让浏览器丢掉 cookie。
  //
  // 服务端**不需要**做任何事：令牌是无状态的，这里"登出"就是让客户端忘掉它。
  // 说白了，一个拿着旧令牌的人照样能用 —— 真正的吊销只有重启进程（换签名
  // 密钥）。这一点写在文档里，而不是假装登出做了更多。
  a.post("/api/logout", [](uvcpp_web_request&, uvcpp_web_response& resp,
                           uvcpp_web_next) {
    resp.clear_cookie(kSessionCookie, "/");
    uvcpp_json j;
    j["ok"] = true;
    send_json(resp, j);
  });
}

void WebServer::Impl::register_routes() {
  uvcpp_web_app& a = *app;

  // ---------------------------------------------------------------------
  // 登录守卫。**必须是第一个注册的中间件** —— 中间件的顺序就是执行顺序，
  // 而这是在链的最外层，任何 /api/ 的处理器都在它后面。
  // ---------------------------------------------------------------------
  //
  // 为什么是中间件而不是在每条路由里各写一句：这里要覆盖的是"所有 /api/"
  // 这一个**集合**，而集合会随着后面加接口而变大。逐条写的结果是新加的接口
  // 默认**不设防**，而且不会有任何迹象 —— 漏掉的那一条和"故意公开"的那一条
  // 长得一模一样。
  //
  // 注意 404/405 那两条特殊链**也带中间件**（见 uvcpp_web_app 的
  // build_special_chains），所以未登录访问一个不存在的 /api/ 路径得到的是
  // 401 而不是 404 —— 这反而是对的：别把"有没有这个接口"告诉没登录的人。
  a.use([this](uvcpp_web_request& req, uvcpp_web_response& resp,
               uvcpp_web_next next) {
    if (!gate_covers(req.path()) || has_session(req, sessions)) {
      next();
      return;
    }
    // 不调 next() 就是链的正常终点（框架里 body_limit / CORS 预检也是这么
    // 短路的），不是错误用法。
    uvcpp_json j;
    j["ok"] = false;
    j["error"] = "未登录，或登录已过期。";
    j["auth_required"] = true;
    send_json(resp, j, 401);
  });

  register_auth_routes();

  // GET /api/status —— 一次性快照。WS 是持续推送，这个是"没有 WS 时也能看"。
  a.get("/api/status", [this](uvcpp_web_request&, uvcpp_web_response& resp,
                              uvcpp_web_next) {
    uvcpp_json j;
    {
      std::lock_guard<std::mutex> lk(mu);
      j = uvcpp_json::parse(last_stats, nullptr, false);
      if (j.is_discarded()) j = uvcpp_json::object();
      // 补一次而不是直接用 `last_stats` 里那份：那是上一次广播留下的文本，
      // 最多一秒前。这批字段是"现在"的状态，不该跟着上一帧走。
      patch_live_counters(j);
    }
    send_json(resp, j);
  });

  // WebSocket：/ws/stats。注册必须在 start() 之前。
  a.websocket("/ws/stats", [this](uvcpp_web_ws_request& ws) { on_stats_socket(ws); });

  // GET /api/config —— 当前生效配置。密码字段由 `config_to_json` 换成哨兵，
  // 所以这里可以直接把它的输出原样发出去（**不能**在 Web 层再做一次脱敏：
  // 两处脱敏规则迟早会不一致，而不一致的那一侧就是在泄露口令）。
  if (opt.config_get) {
    a.get("/api/config", [this](uvcpp_web_request&, uvcpp_web_response& resp,
                                uvcpp_web_next) {
      std::string text = opt.config_get();
      // 挂两样**关于配置的元信息**：还没生效的那几项，和画质档位表。
      // **这一步失败不改变响应本身** —— 一份前端读得懂的配置，远好过因为
      // 加不上一个附加字段而整条 500。
      //
      // 档位表**从后端发**，而不是让前端写死一份。那份表在 `config.cpp` 的
      // `quality_tiers()` 里，每个档位的 fps / 码率 / 分辨率上限都在那儿；前端
      // 照抄的后果是改了表而界面上写的还是旧数值 —— 而界面上那几句话正是用户
      // 判断"降到这一档会牺牲什么"的唯一依据。`QualityTier::label` 的注释从
      // 一开始就写着"界面直接显示"，那就得真的发出去。
      {
        uvcpp_json j = uvcpp_json::parse(text, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
          if (opt.config_pending) {
            uvcpp_json arr = uvcpp_json::array();
            for (std::string const& f : opt.config_pending()) arr.push_back(f);
            j["pending"] = arr;
          }
          uvcpp_json tiers = uvcpp_json::array();
          for (QualityTier const& t : quality_tiers()) {
            uvcpp_json e;
            e["key"] = t.key;
            e["label"] = t.label;
            e["fps"] = t.fps;
            e["bitrate_kbps"] = t.bitrate_kbps;
            e["max_height"] = t.max_height;
            e["sub_enabled"] = t.sub_enabled;
            tiers.push_back(e);
          }
          j["quality_tiers"] = tiers;

          // 延时档位表，**同理从后端发**。
          //
          // 前端那三张卡片上写的每一句话几乎都是算出来的：流水线几帧、
          // 在某个帧率下约等于多少毫秒、关键帧几秒一个。现算自这张表，
          // 界面就不可能和 `latency_tiers()` 漂移 —— 而"界面上那个数字"
          // 恰好是用户唯一能拿去对账的东西（陷阱 90 那一族：档位选下去、
          // 界面显示已生效、每一处代码都做到了它承诺的事，只有用户关心的
          // 那个数字没动）。
          //
          // ⚠️ **表里没有 `custom`，这里也不许补。** `custom` 的语义是
          // "谁都不动"，而它的四个数字（当前 threads / gop）是**手填的**、
          // 不是表里的常数 —— 补进这张表就等于让 `find_latency_tier("custom")`
          // 命中，展开那一段随后会拿表里的值把用户手填的盖掉。
          // 界面上第四张卡是前端自己补的（`ConfigView.vue` 的 `customLatencyNote`）。
          uvcpp_json ltiers = uvcpp_json::array();
          for (LatencyTier const& t : latency_tiers()) {
            uvcpp_json e;
            e["key"] = t.key;
            e["label"] = t.label;
            e["threads"] = t.threads;
            e["gop_seconds"] = t.gop_seconds;
            ltiers.push_back(e);
          }
          j["latency_tiers"] = ltiers;
          text = j.dump(2);
        }
      }
      resp.status(200);
      resp.set_content_type("application/json; charset=utf-8");
      resp.body(text);
      resp.end();
    });
  }

  // POST /api/config —— 校验 + 落盘 + **逐字段**说明哪些当场生效、哪些要重启。
  //
  // 热生效的边界是"改它要不要动别的东西"：ONVIF 的身份与凭据只在循环线程上
  // 被读，改它就是把几个字符串换掉；而改分辨率要重建编码器和 DXGI 会话、
  // 改端口要重新 bind —— 那些做一半的失败模式是"网页上显示改了、实际还在用
  // 旧的"，而用户没有任何办法看出来。所以不追求"尽量多热生效"，只把真正
  // 免费的那一块热掉，其余如实列进 pending。
  //
  // **关键在于逐字段，而不是给一个布尔值。** 一次保存里两种字段常常同时存在，
  // 只回一个 `restart_required` 时前端只能二选一，两种情况里必有一种是骗人。
  if (opt.config_set) {
    a.post("/api/config", [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                                 uvcpp_web_next) {
      // 和 SOAP 一样用 body_data()/body_size()：content_length() 是声明值。
      std::string const body(req.body_data(), req.body_size());
      ConfigApplyResult res;
      std::string err;
      if (!opt.config_set(body, res, err)) {
        uvcpp_json j;
        j["ok"] = false;
        j["error"] = err.empty() ? "配置未通过校验。" : err;
        send_json(resp, j, 400);
        return;
      }
      uvcpp_json arr_applied = uvcpp_json::array();
      for (std::string const& f : res.applied) arr_applied.push_back(f);
      uvcpp_json arr_pending = uvcpp_json::array();
      for (std::string const& f : res.pending) arr_pending.push_back(f);

      // ---- 操作日志（Phase V）：改配置这一条 -------------------------------
      //
      // **细节里只有字段名，一个值都没有。** 这不是顺手省略，是这一段的全部
      // 要点：`applied`/`pending` 里装的恰好就是"改过哪些字段"，而"改了哪些
      // 字段"已经足够回答用户那句"谁在什么时候改了什么"。值里会有口令
      // （`web.password` / `onvif.password` 都在可改字段集合里），一旦记进去
      // 就是事故 —— 而且是一份要留 180 天、界面上还提供"复制一行"的事故。
      //
      // 只在**成功**这条路上写：上面 `config_set` 返回 false 时已经 400 返回，
      // 那次什么都没改，记一条"改过什么"反而是假的。
      //
      // 来源与身份走 `who` 的两列，与审计行 `时间 | 来源 | 身份 | 动作 | 细节`
      // 一一对应（IP 用 `req.peer_ip()`，与登录风控同一个取法）。
      if (opt.audit) {
        std::string detail;
        if (res.applied.empty() && res.pending.empty()) {
          detail = "没有字段发生变化";
        } else {
          auto const join = [](std::vector<std::string> const& v) {
            std::string s;
            for (std::string const& f : v) {
              if (!s.empty()) s += ", ";
              s += f;
            }
            return s;
          };
          if (!res.applied.empty()) detail = "已生效[" + join(res.applied) + "]";
          if (!res.pending.empty()) {
            if (!detail.empty()) detail += " ";
            detail += "待重启[" + join(res.pending) + "]";
          }
        }
        opt.audit(AuditWho{req.peer_ip(), "web:" + sessions.user()}, "config.set",
                  detail);
      }

      // 回读一次而不是把入参回显回去：回读的是**真的落盘并被接受**的那份，
      // 而回显只是"我以为写进去了什么"。前端拿它刷新表单也就自然是准的。
      uvcpp_json j;
      j["ok"] = true;
      j["restart_required"] = !res.pending.empty();
      j["applied"] = arr_applied;
      j["pending"] = arr_pending;
      if (res.pending.empty()) {
        j["note"] = res.applied.empty()
                        ? "配置已保存（这次没有改动任何字段）。"
                        : "配置已保存，且已生效 —— 不需要重启。";
      } else if (res.applied.empty()) {
        j["note"] = "配置已保存。这些项需要重启程序才会生效。";
      } else {
        j["note"] = "配置已保存。applied 里的已经生效，pending 里的需要重启程序。";
      }
      // 被拒绝的字段（目前只有硬件码那三个）。**拼进同一句话**而不是新加一个
      // 字段：`note` 已经是这条路上"给用户看的那句话"，前端也已经在渲染它 ——
      // 再加一个平行的 `notes` 数组，就会出现"有的提示在前端、有的没人显示"。
      for (std::string const& n : res.notes) {
        j["note"] = j["note"].get<std::string>() + " " + n;
      }
      if (opt.config_get) {
        uvcpp_json c = uvcpp_json::parse(opt.config_get(), nullptr, false);
        if (!c.is_discarded()) j["config"] = c;
      }
      send_json(resp, j);
    });
  }

  // GET /api/sources —— 本机**有什么可以当通道**（显示器 + 摄像头）。
  //
  // 配置页的「通道」那一段要摆出"能加哪些"，所以得先知道有哪些。这份数据
  // 只能由主程序给：枚举显示输出要 DXGI、枚举摄像头要 Media Foundation，而
  // Web 层对这两个东西一无所知，也不该知道。
  //
  // ⚠️ **它回的是启动时算好的一份快照，不是现枚举。** 详见 `WebOptions::
  // sources_json` 的注释：探"这块屏能不能抓"要真建一个 `IDXGIOutputDuplication`，
  // 而一块屏同时只允许一个 —— 现枚举会让正在采集的那一路失败。
  //
  // 恒注册、`sources_json` 为空就回两张空表：前端于是不需要处理 404，
  // 单通道/测试构建下那个界面只会显示"没发现可加的通道"，这是个准确的描述。
  a.get("/api/sources", [this](uvcpp_web_request&, uvcpp_web_response& resp,
                               uvcpp_web_next) {
    std::string text = opt.sources_json ? opt.sources_json() : std::string();
    uvcpp_json j = uvcpp_json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) j = uvcpp_json::object();
    if (!j.contains("displays")) j["displays"] = uvcpp_json::array();
    if (!j.contains("cameras")) j["cameras"] = uvcpp_json::array();
    send_json(resp, j);
  });

  // 网页预览"现在看第几路" —— GET 读、POST 写。
  //
  // 为什么是**全机一份**的状态而不是每个连接各一份：控制台只有一个（绑
  // `web.port` 那个），画面有 N 路。而 `/api/snapshot.jpg` 是一条无状态的
  // GET，界面上按的是"通道 2 的快门"、存下来的却是通道 1 —— 只可能是这个
  // 状态没被两边共用。所以它落在进程里，Web 层只负责转手。
  //
  // 校验权**在后端**（`preview_channel_set` 回 false）：哪几个通道现在真的
  // 存在，是运行期才知道的事（配置里关着的、被挡住的都不算），Web 层手里
  // 只有一份字符串。
  a.get("/api/preview-channel", [this](uvcpp_web_request&, uvcpp_web_response& resp,
                                       uvcpp_web_next) {
    uvcpp_json j;
    j["channel"] = opt.preview_channel_get ? opt.preview_channel_get() : 1;
    send_json(resp, j);
  });

  a.post("/api/preview-channel", [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                                        uvcpp_web_next) {
    std::string const body(req.body_data(), req.body_size());
    uvcpp_json in = uvcpp_json::parse(body, nullptr, false);
    // 只认 JSON 里的数字。`"channel": "2"` 这种字符串**故意不收** ——
    // 它多半意味着前端把某个 DOM 的 `value` 原样发了过来，而那种错误
    // 静默接受一次，就会在下一个字段上变成"看着像 2、其实是 NaN"。
    int want = 0;
    if (!in.is_discarded() && in.is_object() && in.contains("channel") &&
        in["channel"].is_number_integer()) {
      want = in["channel"].get<int>();
    }
    if (want <= 0) {
      uvcpp_json j;
      j["ok"] = false;
      j["error"] = "请求体要是 {\"channel\": <正整数>}。";
      send_json(resp, j, 400);
      return;
    }
    bool const ok = opt.preview_channel_set && opt.preview_channel_set(want);
    uvcpp_json j;
    j["ok"] = ok;
    j["channel"] = opt.preview_channel_get ? opt.preview_channel_get() : 1;
    if (!ok) {
      j["error"] = "没有这个通道 —— 它可能没建起来（配置里关着、或者被别的"
                   "通道挡住了）。";
    }
    send_json(resp, j, ok ? 200 : 400);
  });

  // 快照 —— 最近一帧的 JPEG。**两条路径、两套凭据，故意分开。**
  //
  // 分开的唯一理由是**别让浏览器弹出原生的用户名/口令框**。401 带
  // `WWW-Authenticate` 时 Chrome/Firefox 会对子资源加载弹一个系统级的对话框，
  // 那个框和我们的登录页是两套东西：用户在里面输的口令进不了我们的会话，
  // 输错了还会再弹，而他完全不知道发生了什么。
  //
  // 所以：
  //   - `/api/snapshot.jpg` —— **给网页用**。只认会话 cookie，由上面那个
  //     中间件统一守卫，永远不发 Digest 挑战。前端拿它当普通图片加载。
  //   - `<opt.onvif.snapshot_path>`（默认 `/snapshot.jpg`）—— **给 NVR 用**，
  //     也就是 `GetSnapshotUri` 报出去的那个地址。认两种凭据：会话 cookie
  //     （有人直接在浏览器里打开这个 URL）或 ONVIF 的 Digest。
  //
  // 后一条**必须发 Digest 挑战**：ONVIF 客户端的标准流程就是"先裸请求拿 401
  // + 挑战，再带着 Authorization 重来"，不发挑战等于永远走不到第二步。真实的
  // IP 摄像机全都要求快照鉴权，NVR 就是按那个前提写的，所以这不是"更严"，
  // 而是它们唯一被测过的形状。
  if (opt.snapshot_source) {
    a.get("/api/snapshot.jpg", [this](uvcpp_web_request&, uvcpp_web_response& resp,
                                      uvcpp_web_next) { serve_snapshot(resp); });

    a.get(opt.onvif.snapshot_path,
          [this](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next) {
            if (!has_session(req, sessions)) {
              if (opt.onvif_enabled) {
                // **挑战串必须从 `onvif` 那一个实例上取**，绝不能就地新建一个
                // 校验器 —— nonce 是用每进程一份的随机密钥签的，重建就等于把
                // 已经发出去的 nonce 全部作废，客户端会陷进
                // "401 → 换新 nonce → 还是 401"（陷阱 30）。
                std::string challenge;
                if (!onvif.verify_http_digest(req, challenge)) {
                  resp.unauthorized(challenge);
                  resp.body("需要凭据：请用 ONVIF 的用户名与口令。",
                            "text/plain; charset=utf-8");
                  resp.end();
                  return;
                }
              } else {
                // ONVIF 关着 = 没有 Digest 这一套可认，只剩"先登录网页"。
                // **不发挑战**：这里没有能验的凭据，发出去只会让客户端把
                // 同一个请求重试到天荒地老。
                resp.status(401).text("未登录。ONVIF 未启用，快照只对已登录的会话开放。").end();
                return;
              }
            }
            serve_snapshot(resp);
          });
  }

  // WS /ws/preview —— fMP4 分片，给浏览器的 MSE 播。
  //
  // **无条件注册**，不像快照那样看回调在不在：这条通道的数据来源是采集会话，
  // 而会话在哪一刻开始是运行期的事（命令行没给 --record/--rtsp 时也会跑）。
  // 没数据的时候客户端连上来只会拿到一条安静的连接，前端据此报"预览流未就绪"
  // —— 那是个准确的描述。注册成"没有就 404"反而会让前端把它当成后端缺少功能。
  a.websocket("/ws/preview", [this](uvcpp_web_ws_request& ws) { on_preview_socket(ws); });

  // POST /api/onvif/restart —— 重新广播 WS-Discovery（Bye + Hello）。
  //
  // 名字里的 restart 只指"让 NVR 重新收录这台设备"，**不重启进程**：用户改完
  // 设备名/端口之后，NVR 手里那份还是旧的，而它不会主动重新 Probe 一台它已
  // 经认识的设备。这一下的作用就是把它踢回"不认识"。
  a.post("/api/onvif/restart", [this](uvcpp_web_request&, uvcpp_web_response& resp,
                                      uvcpp_web_next) {
    bool const was_running = wsd.running();
    wsd.reannounce();
    uvcpp_json j;
    j["ok"] = true;
    j["wsd_running"] = was_running;
    if (!was_running) {
      j["note"] = "WS-Discovery 没在跑（未启动或被配置关闭），没有可广播的内容。";
    } else {
      j["note"] = "已发出 Bye + Hello。NVR 会在数秒后重新收录本设备。";
    }
    send_json(resp, j);
  });

  // POST /api/reload —— 用**当前保存的配置**重开采集/推流管线。
  //
  // **和上一条不是一回事，名字像而已。** `/api/onvif/restart` 只重播一次
  // WS-Discovery 组播（NVR 那边重新收录设备，画面一秒都不中断）；这条是真的
  // 把采集链拆了重建（录制分段、RTSP 客户端断开重连）。所以路径也刻意不叫
  // `restart` —— 前端两个按钮挨着放的时候，`reload` 和 `restart` 至少还能
  // 让人停下来看一眼。
  //
  // **回 202 而不是 200。** 重开这件事发生在采集线程上，响应发出去的这一刻
  // 它还没发生 —— 回 200 等于宣称"已经重开了"，而客户端下一毫秒去拉流可能还
  // 连着旧的那一条。202 的意思是"收到了、正在做"，这是实话，前端也正好据此
  // 开始转圈。
  //
  // **不需要定时器**（不像重启进程那条要等响应发完）：这里只是置一个原子位，
  // 采集线程下一拍自己会看到，没有任何理由把响应往后拖。
  if (opt.reload) {
    a.post("/api/reload", [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                                 uvcpp_web_next) {
      bool const live = opt.reload();
      // 这里**同步写**、不进 `on_sent`：`opt.reload()` 只是置一个原子位，
      // 记一行日志既不会拖住响应，也不像重启那样会让进程消失。
      if (opt.audit) {
        opt.audit(AuditWho{req.peer_ip(), "web:" + sessions.user()}, "reload",
                  live ? "按当前配置重开采集管线（录制分段、RTSP 客户端重连）"
                       : "当前没有在录制，没有可重开的管线");
      }
      uvcpp_json j;
      j["ok"] = true;
      j["live"] = live;
      if (live) {
        j["note"] =
            "正在按当前配置重开采集管线：录制会分段，已连上的 RTSP 客户端会断开、"
            "数秒后重连。";
      } else {
        j["note"] =
            "当前没有在录制，没有可重开的管线；配置会在下一次开始录制时生效。";
      }
      send_json(resp, j, 202);
    });
  }

  // POST /api/restart —— **真的重启这个进程**。
  //
  // 三个名字的分工在前端是挨着放的，所以这里再说一遍：
  //   * `/api/onvif/restart` —— 只重播一次 WS-Discovery 组播（画面一秒不断）
  //   * `/api/reload`        —— 重开采集管线（录制分段、RTSP 重连，进程还在）
  //   * `/api/restart`（这条）—— 进程本身退出再起来，上面两样都会发生
  //
  // **先回 202，再用 `resp.on_sent` 在响应真的发出去之后动手。** 在 handler 里
  // 退进程的话这个 202 永远到不了前端，用户看到的是"网络错误"，而它和"服务崩
  // 了"长得一模一样（陷阱 73）。框架对**整包**响应是在 body 入队之后同步触发
  // `on_sent` 的（`uvcpp_web_app.cpp` 里 `send_response()` 之后紧跟那句
  // `notify_sent(info)`），而真正把它写上线是 libuv 的事，几十微秒。主线程至少
  // 要等一拍采集循环（40 ms）才会看到那个原子位 —— 余量是这么来的，不是猜的。
  // 需要知道边界的话："入队"不等于"已上网络"，所以严格地说这一刻是"响应已经
  // 交给网络层"，不是"客户端已经收到"。测试里那条"客户端真的收到了一个能解析
  // 的 202"就是这件事的实测判据。
  //
  // **回调里只置位。** 它跑在 web 循环线程上，而收尾是主线程那条分段循环的活
  // （陷阱 14：`app.post` 在循环线程上是就地执行的，"投递"本身就不可靠）。
  if (opt.restart) {
    a.post("/api/restart", [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                                  uvcpp_web_next) {
      // **这一行写在 `on_sent` 外面，是有意的。** 上面刚写过：客户端提前走了
      // 也照样重启。既然如此，"谁在什么时候点了重启"这件事在请求到达并过了
      // 鉴权的那一刻就已经成立，把它挂在"响应送达"上会让一次真的重启没有记录
      // —— 而重启恰恰是排查"那天几点断过流"要对着看的那一行。
      if (opt.audit) {
        opt.audit(AuditWho{req.peer_ip(), "web:" + sessions.user()}, "restart",
                  "重启服务进程（录制中断并分段、RTSP 客户端断开、所有网页会话失效）");
      }
      resp.on_sent([this](uvcpp_web_sent_info const& info) {
        // **客户端提前走了也照样重启。** 请求已经到达、也已经过了鉴权，那两件
        // 事都发生过；"响应没送达"改变不了它。反过来（因为对方断线就不重启）
        // 会让一次点击石沉大海，而用户看到的只是"点了没反应"。
        if (!info.ok) {
          D2O_WARN("重启请求的 202 没能送达（连接已断），照常执行。");
        }
        opt.restart();
      });
      uvcpp_json j;
      j["ok"] = true;
      j["restarting"] = true;
      // 三条代价都要写出来，而不是一个干巴巴的"重启"：用户点之前必须知道
      // 自己会失去什么。
      j["note"] =
          "正在重启服务：当前录制会中断并分段、已连上的 RTSP 客户端会断开重连、"
          "所有网页会话失效（会话令牌是每进程一份的随机密钥，重启必然踢掉所有人），"
          "稍后需要重新登录。";
      send_json(resp, j, 202);
    });
  }

  // POST /api/factory-reset —— 回到全新状态（删掉 config.json）然后重启。
  //
  // **body 必须恰好是 `{"confirm":"RESET"}`**，形状对不上就 400 什么都不做。
  // 只认这一个字面量：前端那个二次确认框是**给人看的**，服务端不能信"我点了
  // 确认"这种说法 —— 发出这个请求的可以是任何东西，包括一个手滑的脚本。
  //
  // 动作本身（备份 `.bak`、删配置、清日志、复位设备状态）**不在这一层**：
  // Web 层够不到配置文件路径、`DeviceState`、`Log` 的文件句柄，而且那些事必须
  // 在主线程做（和重启同一条收尾路径）。这里只认形状、回 202、置位。
  if (opt.factory_reset) {
    a.post("/api/factory-reset", [this](uvcpp_web_request& req,
                                        uvcpp_web_response& resp, uvcpp_web_next) {
      // 和 SOAP 一样用 body_data()/body_size()：`content_length()` 是声明值。
      std::string const body(req.body_data(), req.body_size());
      uvcpp_json const c = uvcpp_json::parse(body, nullptr, false);
      if (c.is_discarded() || !c.is_object() ||
          c.value("confirm", std::string()) != "RESET") {
        uvcpp_json j;
        j["ok"] = false;
        j["error"] =
            "恢复出厂设置需要请求体恰好是 {\"confirm\":\"RESET\"}。"
            "这是防手滑的第二道闸，不是给人看的确认框。";
        send_json(resp, j, 400);
        return;
      }
      // **「谁点的」要在这里取出来、并且按值捕获。** 两件事都不能省：
      //   * 取的时刻 —— 请求对象 `req` 只在这次 handler 里有效，而 `on_sent`
      //     是几十微秒之后才跑的；那时再去问 `req.peer_ip()` 是碰一个已经收了
      //     的对象。所以先算成 `std::string` 再进 lambda。
      //   * 按值捕获 —— 恢复出厂会把**整份审计日志**清空（这正是用户要的），
      //     而主线程清完还要写一行「谁恢复了出厂」。这一行只能是**清空之前**
      //     截下来的那份 who，所以它必须已经在手上。
      //
      // 身份写成 `web:<用户名>`，与审计行的第二列同一个惯例（`onvif:<用户名>` /
      // `system` 是另外两种来源）。口令**永远不在这里**，也不需要 —— 能走到这一
      // 行的请求已经过了会话校验，用户名就是"是谁"的全部答案。
      std::string const ip = req.peer_ip();
      std::string const ident = "web:" + sessions.user();
      resp.on_sent([this, ip, ident](uvcpp_web_sent_info const& info) {
        if (!info.ok) {
          D2O_WARN("恢复出厂请求的 202 没能送达（连接已断），照常执行。");
        }
        opt.factory_reset(ip, ident);
      });
      uvcpp_json j;
      j["ok"] = true;
      j["resetting"] = true;
      // **诚实边界写在回应里**，不只写在文档里：恢复出厂恢复的是**配置文件**。
      j["note"] =
          "正在恢复出厂设置：config.json 会先备份成 config.json.bak 再删除，"
          "日志会被清空，设备时间偏移归零，然后服务重启。"
          "注意：恢复的是配置文件 —— 命令行上带的参数（端口、口令等）重启后照样生效，"
          "因为重启用的是同一条命令行。";
      send_json(resp, j, 202);
    });
  }

  // ---- 时间设置（Phase U）--------------------------------------------------
  //
  // 四条路与 `opt.time_*` 一一对应，**每一条都按 `if (opt.time_*)` 注册**
  // （同 `/api/config` 那条纪律：注册了一条功能不可用的路，请求会落到静态兜底
  // 上，SPA 模式下拿到 200 + 一份 index.html，前端 `JSON.parse` 报的错和真正的
  // 原因毫无关系）。
  //
  // **这里没有一条会改系统时钟。** 改的是 `DeviceState` 里那个"设备时间"
  // （= 主机时间 + 偏移），也就是水印上和 ONVIF 报出去的那个时间。理由见
  // `TimeConfig` 的注释与 README：动 Windows 的系统时钟要管理员权限、会连累
  // 这台机器上别的程序，而且那是用户的钟不是我们的。
  //
  // 三条 POST 的分工也值得写在这里，因为它们长得像而代价完全不同：
  //   * `/api/time`（手动校准）—— 同步做完：写 `DeviceState` + 落盘 + 重算
  //     pending。它慢不到哪去（一次写盘），所以**不用** 202。
  //   * `/api/time/reset`     —— 同上，但多一步 `reset_time()`。
  //   * `/api/time/sync`      —— **回 202**，因为真正的 UDP 查询在后台线程上
  //     （最长 2 秒 × 地址数）。这一条与 `/api/reload` 同源，不是新发明的形状。
  if (opt.time_get) {
    a.get("/api/time", [this](uvcpp_web_request&, uvcpp_web_response& resp,
                              uvcpp_web_next) {
      // 回调给的是**字符串**（时间与对时状态在 main.cpp 那边，它同时握着
      // `DeviceState` 和 `NtpSyncer`），这里解一遍再发 —— 解不动说明那份 JSON
      // 是坏的，而"坏"必须在**这里**变成一句能读的错，不能原样发给前端：
      // 前端 `JSON.parse` 抛出来的异常落在读字段那一行上，报的错和真正的原因
      // 毫无关系（陷阱 39 的形状，与 `/api/audio/devices` 那条同一个理由）。
      uvcpp_json j = uvcpp_json::parse(opt.time_get(), nullptr, false);
      if (j.is_discarded()) {
        D2O_ERROR("GET /api/time：时间的 JSON 自己就是坏的，发出去只会让前端"
                  "在一个解析错误上找原因。");
        uvcpp_json e;
        e["ok"] = false;
        e["error"] = "服务端组装的时间信息不是合法 JSON，这是本程序的缺陷。";
        send_json(resp, e, 500);
        return;
      }
      send_json(resp, j);
    });
  }

  if (opt.time_set) {
    a.post("/api/time", [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                               uvcpp_web_next) {
      // 和 SOAP 一样用 `body_data()`/`body_size()`：`content_length()` 是声明值。
      std::string const body(req.body_data(), req.body_size());
      std::string err;
      if (!opt.time_set(body, err)) {
        uvcpp_json j;
        j["ok"] = false;
        j["error"] = err.empty() ? std::string("手动校准被拒绝了，但没有给出原因。")
                                 : err;
        send_json(resp, j, 400);
        return;
      }
      uvcpp_json j;
      j["ok"] = true;
      j["note"] = "设备时间已校准。这是**设备时间**（水印与 ONVIF 报出去的那个），"
                  "本机的 Windows 系统时钟没有被改动。";
      // 顺带把新状态一起回去：不这样的话前端还得再拉一次 `/api/time`，而那一拍
      // 里它显示的还是旧偏移 —— 用户会看到"保存成功了，但数字没变"。
      bool have_offset = false;
      long long new_offset = 0;
      if (opt.time_get) {
        uvcpp_json const t = uvcpp_json::parse(opt.time_get(), nullptr, false);
        if (!t.is_discarded()) {
          j["time"] = t;
          // 审计记的是**结果**（落盘之后那个偏移），不是请求体。请求体可以是
          // `offset_s` 也可以是 `device_now`，而后者要服务端算一遍才知道偏移是
          // 多少 —— 记请求体的话，唯一能回答"到底调成了几点"的原始数字就丢了。
          if (auto const it = t.find("offset_s");
              it != t.end() && it->is_number_integer()) {
            new_offset = it->get<long long>();
            have_offset = true;
          }
        }
      }
      if (opt.audit) {
        std::string detail = "手动校准设备时间";
        detail += have_offset ? "，新偏移 " + std::to_string(new_offset) + " 秒"
                              : "（读回新偏移失败，见日志）";
        opt.audit(AuditWho{req.peer_ip(), "web:" + sessions.user()}, "time.set",
                  detail);
      }
      send_json(resp, j);
    });
  }

  if (opt.time_reset) {
    a.post("/api/time/reset", [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                                     uvcpp_web_next) {
      std::string err;
      if (!opt.time_reset(err)) {
        uvcpp_json j;
        j["ok"] = false;
        j["error"] = err.empty() ? std::string("归零被拒绝了，但没有给出原因。") : err;
        send_json(resp, j, 400);
        return;
      }
      if (opt.audit) {
        opt.audit(AuditWho{req.peer_ip(), "web:" + sessions.user()}, "time.reset",
                  "设备时间归零（= 主机时间），偏移写回配置");
      }
      uvcpp_json j;
      j["ok"] = true;
      j["note"] = "设备时间已恢复成主机时间（偏移归零）。";
      if (opt.time_get) {
        uvcpp_json const t = uvcpp_json::parse(opt.time_get(), nullptr, false);
        if (!t.is_discarded()) j["time"] = t;
      }
      send_json(resp, j);
    });
  }

  if (opt.time_sync) {
    a.post("/api/time/sync", [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                                    uvcpp_web_next) {
      // **不需要 `resp.on_sent`**（和 `/api/restart` 那条的差别就在这里）：
      // 这个回调只是往对时线程上放一个位（一把锁 + 一次 notify），不等任何
      // 网络、也不会让进程消失。所以"先回 202 再动手"那套在这里没有对象 ——
      // 照抄那个形状只会多一个解释不清的延迟。
      opt.time_sync();
      // 这一行记的是**请求**，不是结果 —— 结果还没发生（查询在后台线程上，
      // 最长 2 秒）。真对完之后由 main.cpp 那边再补一条 `ntp.sync`，两条一起
      // 回答"谁点的"和"对上了没有"。指望在这一行里写出偏移，就只能去猜。
      if (opt.audit) {
        opt.audit(AuditWho{req.peer_ip(), "web:" + sessions.user()}, "time.sync",
                  "请求立刻发起一次 NTP 对时查询（结果见随后的 ntp.sync 一行）");
      }
      uvcpp_json j;
      j["ok"] = true;
      j["syncing"] = true;
      // **两种模式的区别写进回应里**，因为"点了一下、什么都没变"和"对时失败"
      // 在前端长得一样：手动模式下这次查询只报结果、**不改设备时间**（否则
      // 配置里那个 `offset_s` 立刻就对不上，而下次重启又弹回去），NTP 模式下
      // 才会同时把设备时间对齐。界面照这句话说话，用户就不会去查一个不存在
      // 的故障。结果去 `GET /api/time` 的 `last_sync_state` / `last_sync_error` 看。
      j["note"] =
          "正在向 NTP 服务器发起一次对时查询（UDP 查询最长 2 秒）。"
          "当前是手动模式时，这次只报结果、不会改动设备时间；"
          "NTP 模式下会同时把设备时间对齐。结果见 GET /api/time 的 "
          "last_sync_state / last_sync_error。";
      send_json(resp, j, 202);
    });
  }

  // GET /api/audio/devices —— 音频设备列表（给配置页那个麦克风下拉框）。
  //
  // **注册与否由调用方决定**（`opt.audio_devices` 为空就是不注册），和
  // `/api/config` / `/api/reload` 是同一条纪律：注册了一条功能不可用的路，
  // 请求会落到静态兜底上，SPA 模式下拿到 200 + 一份 index.html，前端
  // `JSON.parse` 报的错和真正的原因毫无关系。
  if (opt.audio_devices) {
    a.get("/api/audio/devices", [this](uvcpp_web_request&, uvcpp_web_response& resp,
                                       uvcpp_web_next) {
      // 回调给的是**字符串**（枚举在 main.cpp 那边做，它才有 COM 的线程亲和），
      // 这里解一遍再发 —— 解不动就说明那份 JSON 是坏的，而"坏"必须在**这里**
      // 变成两个空列表，不能原样发给前端：前端 `JSON.parse` 抛出来的异常
      // 落在下拉框那一段代码上，报的错和真正的原因毫无关系（陷阱 39 的形状）。
      uvcpp_json j = uvcpp_json::parse(opt.audio_devices(), nullptr, false);
      if (j.is_discarded()) {
        D2O_ERROR("音频设备列表不是合法 JSON，已按空列表返回（下拉框会是空的）。");
        j = uvcpp_json{{"capture", uvcpp_json::array()},
                       {"render", uvcpp_json::array()}};
      }
      // 插拔一个耳机列表就变，缓存住的话界面会一直显示已经拔掉的设备。
      // 顺序照 `serve_snapshot`：先 status、再头、最后 body —— 那个顺序是被
      // 快照那条路验过的，反过来写没有理由，只是多一种没验过的组合。
      resp.status(200);
      resp.set_header("Cache-Control", "no-store");
      resp.json(j);
      resp.end();
    });
  }

  // GET /api/audit?days=7&limit=500&q=关键字 —— 操作日志（只读）。
  //
  // **这一层不认识 `AuditLog`。** 回调给的是一整份 JSON 文本（在 main.cpp 那边
  // 组装，它才握着 `AuditLog`），这里只管解一遍、校验一遍、发出去 —— 与
  // `/api/audio/devices` 是同一个形状，理由也一样：让"按天分组、倒序、关键字
  // 过滤、留存策略那句话"这些**呈现**上的决定集中在一处，而不是让 Web 层和
  // 日志层各持一半、将来漂开（陷阱 52 的形状）。
  //
  // **坏 JSON 回 500，不是回空列表。** 空列表在这里有确切含义（"这台设备还没
  // 发生过任何事"，很可能刚恢复出厂），把组装缺陷报成空列表，等于用一句话回答
  // 了两种完全不同的问题 —— 用户会去查"为什么没有记录"，而真正该查的是程序
  // 自己。和 `/api/time` 那条同一个取舍。
  if (opt.audit_read) {
    a.get("/api/audit", [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                               uvcpp_web_next) {
      // `days` / `limit` 缺失、写成 `abc`、写成 `-3`，全都退到默认值而不是让
      // 这次请求失败 —— 界面上那两个框是给人填的，填错了应该还能看见日志。
      // **上下界在 main.cpp 那边夹**（它要把夹过之后的值回报给分页用），
      // 这里只做"能不能整串转成整数"这一层。
      //
      // ⚠️ `query()` 回的是**指针**（`nullptr` = 没有这个键），不是带默认值的
      // 字符串 —— 没有这个键和"这个键的值是空串"是两件事，框架特意分开。
      auto str_param = [&req](char const* name) -> std::string {
        std::string const* const v = req.query(name);
        return v == nullptr ? std::string() : *v;
      };
      auto int_param = [&str_param](char const* name, int fallback) {
        std::string const v = str_param(name);
        if (v.empty()) return fallback;
        try {
          std::size_t used = 0;
          int const n = std::stoi(v, &used);
          // `"12abc"` 也解析得动，但那显然不是用户想输入的 —— 整串都得是数字。
          return used == v.size() ? n : fallback;
        } catch (std::exception const&) {
          return fallback;
        }
      };
      int const days = int_param("days", 7);
      int const limit = int_param("limit", 500);
      std::string const keyword = str_param("q");

      uvcpp_json j =
          uvcpp_json::parse(opt.audit_read(days, limit, keyword), nullptr, false);
      if (j.is_discarded()) {
        D2O_ERROR("GET /api/audit：操作日志的 JSON 自己就是坏的，发出去只会让"
                  "前端在一个解析错误上找原因。");
        uvcpp_json e;
        e["ok"] = false;
        e["error"] = "服务端组装的操作日志不是合法 JSON，这是本程序的缺陷。";
        send_json(resp, e, 500);
        return;
      }
      // 日志是只增的，但界面会反复刷新它，缓存住只会让人以为"新写的那几条
      // 没进去"。与音频设备列表同一个头，理由更直白。
      resp.status(200);
      resp.set_header("Cache-Control", "no-store");
      resp.json(j);
      resp.end();
    });
  }

  // ONVIF 的两个 SOAP 端点。**分成两条路由，而不是一条按 Body 里的命名空间
  // 分派**：`GetServiceCapabilities` 在两个服务里同名，而"客户端把请求发到
  // 哪个 URL"是它自己明确表达的意图，比从报文体里猜可靠得多。
  //
  // `onvif.set_options()` 在这里调是安全的：此刻循环线程还没起来。
  if (opt.onvif_enabled) {
    onvif.set_options(opt.onvif);
    // 接上设备状态。名称**不在这里推**：启动时那一次由 main.cpp 写（它还要
    // 覆盖"ONVIF 关着、但水印仍然要显示名称"那条路），运行中那一次由
    // `set_identity` 写。两个写入方各有明确的分工，这里再来一个的话，将来
    // 三处里漏掉一处就会表现为"某一条路上名字是旧的"。
    onvif.set_device_state(opt.device_state);
    a.post("/onvif/device_service",
           [this](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next) {
             onvif.handle(req, resp, false);
           });
    a.post("/onvif/media_service",
           [this](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next) {
             onvif.handle(req, resp, true);
           });
    D2O_INFO("ONVIF 已启用：%s / %s",
             "/onvif/device_service", "/onvif/media_service");
  }

  // 静态资源挂最后。它注册的是 `/` 和 `/*path` 两条**兜底**路由，
  // 永不调 next()；字面量路由（/api/...、/onvif/...）优先级天然高于通配符，
  // 所以注册顺序其实无所谓 —— 挂最后只是读起来更清楚。
  if (!opt.static_root.empty()) {
    uvcpp_web_static_options so;
    // Vue Router 用 history 模式时，/config 这类前端路由在后端是没有对应
    // 文件的，要靠回落到 index.html。少了这个，刷新页面就是 404。
    so.spa_fallback = true;
    so.spa_file = "index.html";
    // 不放 Cache-Control：前端是带 hash 的产物，让浏览器自己按 ETag 走，
    // 免得开发时改了 dist 却一直拿到旧文件。
    auto st = a.serve_static("/", opt.static_root, so);
    if (st) {
      D2O_INFO("静态资源根目录：%s", st->root_real().c_str());
    } else {
      D2O_WARN("静态资源根目录挂不上：%s（Web UI 会 404，API 不受影响）",
               opt.static_root.c_str());
    }
  }
}

void WebServer::init_runtime() {
  int const rc = uvcpp::uvcpp_set_threadpool_size(0);
  if (rc != 0) {
    // 走到这里说明有别的东西抢先用了线程池。不致命（功能都在，只是慢），
    // 但要知道 —— 它意味着"静态文件读取和别的东西在抢 4 个线程"。
    D2O_WARN("uvcpp_set_threadpool_size 返回 %d —— 线程池大小没设上，"
             "libuv 仍是默认的 4 个线程。",
             rc);
  }
}

WebServer::WebServer() : impl_(std::make_unique<Impl>()) {}
WebServer::~WebServer() { stop(); }

bool WebServer::start(WebOptions const& opt, std::string& err) {
  Impl& d = *impl_;
  if (d.app) {
    err = "WebServer 已经启动过。一个 uvcpp_web_app 实例只能启动一次，"
          "要重启请新建一个 WebServer。";
    return false;
  }

  d.opt = opt;
  // 登录状态先装好再注册路由：`register_routes()` 里那两个 WS 处理器会当场
  // 读它。此刻循环线程还没起来，在这里写没有任何并发问题。
  d.sessions.set_credentials(opt.auth_user, opt.auth_password);
  d.sessions.set_enabled(opt.auth_enabled);
  d.app = std::make_unique<uvcpp_web_app>();

  uvcpp_web_app& a = *d.app;
  a.set_host(opt.host)
      .set_port(opt.port)
      .set_server_header("Desk2ONVIF")
      // 访问日志默认关：NVR 会周期性地刷 SOAP，一秒几行会把我们自己的
      // 状态行冲出屏幕。要排查时用 --verbose 打开。
      .set_access_log(Log::level() <= LogLevel::Debug)
      .set_log_level(Log::level() <= LogLevel::Debug ? log_level::DEBUG
                                                     : log_level::WARN);

  // 单循环。理由见头文件 —— 多循环下"从别的线程推给所有 WS 客户端"
  // 没有正确实现（拿不到会话属于哪条循环）。
  if (a.loop_count() != 1) {
    D2O_WARN("webapp 的循环数是 %d，Desk2ONVIF 只支持 1（理由见 web_server.h）。"
             "已按 1 条循环继续。",
             a.loop_count());
  }

  d.register_routes();

  int const rc = d.app->start_background();
  if (rc != 0) {
    err = fmt("HTTP 服务起不来（%s:%d，libuv 错误码 %d）。"
              "端口被占是最常见的原因；换个 --http-port 试试。",
              opt.host.c_str(), opt.port, rc);
    d.app.reset();
    return false;
  }

  d.port.store(d.app->bound_port(), std::memory_order_relaxed);

  // ONVIF 的 `GetSnapshotUri` 要报真实的 HTTP 端口，而它只有 bind 之后才
  // 知道（配置里写 0 表示由系统分配）。**投到循环线程去改**：选项结构体只
  // 归循环线程，从这里直接写就是一条路由请求和一次写入抢同一个 std::string。
  if (opt.onvif_enabled) {
    int const actual = d.port.load(std::memory_order_relaxed);
    d.app->post([this, actual]() { impl_->onvif.set_http_port(actual); });
  }

  // 端口定下来了才可以起 WSD —— XAddrs 里要写真实端口。
  d.start_wsd(opt);

  D2O_INFO("HTTP 服务已启动：http://%s:%d/（API 在 /api/，WS 在 /ws/stats）",
           opt.host == "0.0.0.0" ? "本机地址" : opt.host.c_str(),
           d.port.load(std::memory_order_relaxed));

  // 登录状态要**如实**报出来。这条日志是用户判断"我这台机器现在敞不敞"的
  // 唯一依据，含糊过去等于让他以为安全。
  if (d.sessions.effective()) {
    D2O_INFO("Web 控制台需要登录：用户名 %s（口令在 web.password，默认 admin）。",
             d.sessions.user().c_str());
  } else if (opt.auth_enabled) {
    // 开关开着但口令是空的 —— 这不是"不鉴权"，而是"口令就是空"。区别在于
    // 前者前端会跳过登录页，后者会摆一个按一下回车就进去的登录框。它比明说
    // 没鉴权更坏，所以这一条是 WARN 而不是 INFO。
    D2O_WARN("web.auth_enabled 开着，但 web.password 是空的 —— 登录框照常出现，"
             "而任何人按一下回车就能进去。请设一个口令，或显式关掉它（--no-http-auth）。");
  } else {
    D2O_INFO("Web 控制台未启用登录（web.auth_enabled=false）。");
  }

  if (opt.host == "0.0.0.0") {
    // **这条警告的内容随登录走**：以前它只会说"谁都能访问"，而现在关掉登录
    // 才是那样。一条永远不变的警告只会训练人忽略它。
    if (d.sessions.effective()) {
      D2O_INFO("HTTP 绑在 0.0.0.0 —— 同一局域网的机器都能访问 Web UI，"
               "但需要登录（想只给本机用就 --http-host 127.0.0.1）。");
    } else {
      D2O_WARN("HTTP 绑在 0.0.0.0 且没有登录 —— 同一局域网的任何机器都能读写配置、"
               "看你的桌面画面。改用 --http-host 127.0.0.1，或在 web 里设一个口令。");
    }
  }
  return true;
}

void WebServer::stop() {
  if (!impl_) return;
  impl_->shutdown();
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->stats_conns.clear();
    impl_->stats_clients.store(0, std::memory_order_relaxed);
    // 预览的连接表也清掉。**不清就是悬垂指针**：app 已经回收了所有会话对象，
    // 表里留下的那些指针在下一块分片广播时会被写。
    impl_->preview_conns.clear();
    impl_->preview_clients.store(0, std::memory_order_relaxed);
  }
  impl_->port.store(0, std::memory_order_relaxed);
}

void WebServer::publish(SessionStats const& s) {
  Impl& d = *impl_;
  if (!d.app) return;

  // 持 post_mu 只是为了和 stop() 会合：stop() 拿到这把锁时，就能确定
  // "已经没有任何一次 publish 停在投递和返回之间了"。之后 publish 会看到
  // stopping 直接返回。没有这一步，一次恰到好处的 publish 会把任务投给
  // 一条正在被释放的循环。
  std::lock_guard<std::mutex> lk(d.post_mu);
  if (d.stopping || !d.app->loop_started()) return;

  std::string text = stats_json(s).dump();
  int const w = s.video_width;
  int const h = s.video_height;
  int const sw = s.sub_width;
  int const sh = s.sub_height;
  d.app->post([&d, text = std::move(text), w, h, sw, sh]() mutable {
    // 画面尺寸从采集线程跟着统计一起过来。放在**这条已经存在的通道**上
    // 而不是另开一个钩子：它和统计一样是"每秒一份的采集侧真实状态"，
    // 多一条管道只是多一处要记得接。
    d.apply_video_size(w, h, sw, sh);
    d.broadcast_stats(std::move(text));
  });
}

void WebServer::publish_video_size(int w, int h, int sub_w, int sub_h) {
  Impl& d = *impl_;
  if (!d.app) return;
  // 和 `publish()` 同一套停机会合：`stop()` 拿到这把锁之后就能确定没有任何
  // 一次投递停在"投出去"和"返回"之间了（见 `publish()` 顶上那段）。
  std::lock_guard<std::mutex> lk(d.post_mu);
  if (d.stopping || !d.app->loop_started()) return;
  d.app->post([&d, w, h, sub_w, sub_h]() { d.apply_video_size(w, h, sub_w, sub_h); });
}

int WebServer::port() const { return impl_->port.load(std::memory_order_relaxed); }

std::size_t WebServer::stats_clients() const {
  return impl_->stats_clients.load(std::memory_order_relaxed);
}

std::size_t WebServer::preview_clients() const {
  return impl_->preview_clients.load(std::memory_order_relaxed);
}

void WebServer::publish_preview(bool is_init,
                                std::shared_ptr<std::vector<std::uint8_t> const> bytes) {
  Impl& d = *impl_;
  if (!bytes || bytes->empty()) return;

  // 和 publish() 同一套会合逻辑：拿到 post_mu 之后看到的 stopping 是最终值，
  // 所以不会有一次投递落进正在被释放的循环里。
  std::lock_guard<std::mutex> lk(d.post_mu);
  if (d.stopping || !d.app || !d.app->loop_started()) return;

  d.app->post([&d, is_init, bytes = std::move(bytes)]() {
    if (is_init) {
      // 留存一份给之后连上来的客户端。**每次会话开始时都会覆盖它** ——
      // 换了分辨率/重开会话之后，老的初始化段配上新的分片是解不出来的，
      // 而且不会报错，只是浏览器一直黑着。
      d.preview_init = bytes;
      D2O_INFO("预览初始化段已就绪（%zu 字节），当前 %zu 个预览客户端。", bytes->size(),
               d.preview_conns.size());
    } else {
      ++d.preview_fragments;
    }

    if (d.preview_conns.empty()) {
      if (!is_init) ++d.preview_unwatched;
      return;
    }
    // **先拷一份表再遍历**：send_preview_block 在积压超限时会把自己从
    // preview_conns 里摘掉，边遍历边改容器会踩掉迭代器。
    std::vector<std::shared_ptr<PreviewConn>> const snapshot = d.preview_conns;
    for (auto const& pc : snapshot) d.send_preview_block(pc, bytes);
  });
}

void WebServer::publish_preview_t0(std::int64_t t0_wall_ms) {
  Impl& d = *impl_;
  if (t0_wall_ms <= 0) return;  // 采集侧没给（预览没封装，或第一个包的 wall_ms 是 0）

  // 和 publish_preview 同一套会合逻辑，理由也同一句：拿到 post_mu 之后看到的
  // stopping 是最终值，所以不会有一次投递落进正在被释放的循环里。
  std::lock_guard<std::mutex> lk(d.post_mu);
  if (d.stopping || !d.app || !d.app->loop_started()) return;

  d.app->post([&d, t0_wall_ms]() {
    // **存一份 + 广播，两件都要做，缺一件就有一半客户端拿不到。**
    //
    // 存：0 点是采集侧第一个包进封装器那一刻定的，而它几乎总在第一个浏览器
    // 连上来**之前**（首页一加载就连，但那之前采集早跑起来了）。没有这一份，
    // 之后每一个连上来的客户端都要等到下一次会话重开才有原点。
    //
    // 广播：首页一加载就连，那时 0 点可能还没定（采集刚起来、第一个包还没进
    // 封装器）。只存不广播的话，这批"第 0 秒就连上"的客户端 —— 也就是最盯着
    // 这个数字看的人 —— 反而永远收不到。
    //
    // 覆盖而不比较：一次会话只有一个 0 点，重开会话会再报一次（新的 0 点）。
    // 让"同一次会话重复报同一个值"走正常路径，比在这里判"变了没有"少一个
    // 能算错的地方 —— 这个钩子按设计每次会话只调一次，没有抖动的可能。
    d.preview_t0_ms = t0_wall_ms;

    std::vector<std::shared_ptr<PreviewConn>> const snapshot = d.preview_conns;
    for (auto const& pc : snapshot) d.send_preview_t0(pc);
  });
}

unsigned long long WebServer::wsd_probes_seen() const {
  return impl_->wsd.probes_seen();
}

unsigned long long WebServer::wsd_probes_answered() const {
  return impl_->wsd.probes_answered();
}

unsigned long long WebServer::onvif_requests() const {
  return impl_->onvif.requests();
}

unsigned long long WebServer::onvif_auth_failures() const {
  return impl_->onvif.auth_failures();
}

bool WebServer::apply_onvif_identity(OnvifIdentity const& id) {
  // 身份**总是**推下去，哪怕 ONVIF 没开。理由：这几个字段不只是 SOAP 的字段 ——
  // `device_name` 同时是画面水印上那个名称（经 `DeviceState`），而水印不依赖
  // ONVIF 开不开。ONVIF 关着却不让改名，会用的人第一反应是"改名坏了"。
  //
  // 这**不是**"记下来等以后用"（头文件里禁止的那个）：`DeviceState` 此刻就是
  // 活的，采集线程下一帧就读到新名字。ONVIF 自己那份 `options()` 跟着变了也没
  // 关系 —— 它没有路由，是惰性的；而返回值仍然是 false，于是调用方不会把
  // `onvif.*` 记进 `applied`，正确地说"要重启才有 SOAP 上的新名字"。
  impl_->onvif.set_identity(id);
  return impl_->opt.onvif_enabled;
}

void WebServer::apply_osd_config(OsdConfig const& c) {
  // 没有返回值，理由（以及从前那个恒 false 的 bool 为什么被拿掉）写在头文件里。
  // 这里只补一句实现上的：`set_osd` **不会**回调 `on_osd_patch`，所以这条路上
  // 不存在"保存 → 回调 → 再保存"的递归（`app.post` 在 loop 线程上是就地执行的，
  // 那种递归会当场栈溢出而不是死循环，表现完全不同）。
  impl_->onvif.set_osd(c);
}

bool WebServer::apply_web_credentials(bool enabled, std::string const& user,
                                      std::string const& password) {
  // Web 服务没起来 = 没有任何东西在被拦着，也就不存在"生效"。这时若返回
  // true，调用方会把 web.* 记进 `applied`，而实际效果是"下次启动才生效" ——
  // 正是这个功能要防的那句谎。参数照收不误：`sessions` 留着新值，对下次
  // start() 无害（start 会再设一遍）。
  bool const live = impl_->app != nullptr;
  impl_->sessions.set_credentials(user, password);
  impl_->sessions.set_enabled(enabled);
  if (live) {
    // 这里**只改拦不拦人**，不动已发出的令牌（签名密钥没变）。所以改口令
    // 不会把人踢下线，改开关也不会 —— 那是刻意的，见 web_session.h。
    D2O_INFO("Web 控制台的登录设置已更新：%s，用户名 %s。",
             impl_->sessions.effective() ? "需要登录" : "无需登录",
             impl_->sessions.user().c_str());
  }
  return live;
}

unsigned long long WebServer::onvif_faults() const {
  return impl_->onvif.faults();
}

}  // namespace d2o
