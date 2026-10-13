#pragma once

/**
 * @file src/app/capture_session.h
 * @brief 把采集、编码、封装/推流几条线接起来，跑一段桌面会话。
 *
 * 线程模型：
 *
 *   [调用方线程]  采集 + 定速 —— DXGI 的会话状态只有这里碰
 *        │  BoundedQueue<CapturedFrame>（drop-oldest，深度默认 4）
 *        ▼
 *   [编码线程]    BGRA→YUV、x264，然后把包**依次**交给下游
 *        │                              ├─ RTSP  → Live555 调度线程
 *        │                              └─ MP4   → 文件
 *        ▼
 *   [Live555 调度线程]  只在起了 RTSP 时有
 *
 * 为什么采集循环跑在调用方线程而不是再起一个：采集循环本来就是"一直转直到
 * 有人叫停"，它天然就是主循环。多起一个线程只会多出一层 join 和一组停止标志。
 *
 * 队列深度是 4 而不是 1：深 1 时编码线程只要慢一拍就丢帧，抖动会被放大成可见
 * 的卡顿；深 4 能吸收约 130 ms 的抖动。再深就变成"延迟"而不是"缓冲"了。
 *
 * 录制和推流共用同一条采集/编码链路 —— 这是刻意的：NVR 在拉流的同时本地留一份
 * 存证是常见用法，两套采集不仅浪费，还会因为两条 DXGI 会话互相抢帧。
 */

#include "app/channel_plan.h"
#include "capture/frame.h"
#include "capture/frame_source.h"
#include "common/config.h"
#include "common/live_osd.h"
#include "rtsp/rtsp_server.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace d2o {

class DeviceState;

struct SessionOptions {
  /// 用哪个画面来源。**桌面是默认**，`--camera` 或配置里 `source:"webcam"`
  /// 才会换成摄像头。
  ///
  /// 从前这里是裸的 `unsigned display_index`。换成 `SourceSpec` 不是因为
  /// "以后可能还有别的源"，而是因为摄像头那一格逼出来的：一个下标说不清
  /// "第一台摄像头"，而"空字符串 = 第一台"这条规则必须和 `--list-cameras`
  /// 报的清单对得上（否则用户按清单上第 2 台的名字写进配置，程序按"第一个
  /// 非空字符串"去理解）。
  SourceSpec source;
  int fps = 25;
  int bitrate_kbps = 2048;
  int gop_seconds = 2;
  /// 编码线程数。<= 0 交给编码器按"延迟优先"自动选（见 EncoderConfig::threads）。
  int threads = 0;
  std::size_t queue_capacity = 4;
  std::size_t pool_size = 6;    ///< 帧缓冲池，要 > 队列深度 + 1
  std::string preset = "ultrafast";
  std::string tune = "zerolatency";
  std::string profile = "main";

  /// 主码流的高度上限。**0 = 用桌面原生尺寸**（不缩放）。
  ///
  /// 只写上限：桌面本来就比它小的话不放大。缩放靠编码器本来就有的
  /// src≠dst 能力（子码流走的就是它），这里只要把**编码尺寸**和**采集尺寸**
  /// 分开就行 —— 采集永远是原生尺寸，否则鼠标指针的坐标会错位。
  int max_height = 0;

  /// 水印。合成点只有一个（见 osd_overlay.h），所以主/子/MP4/RTSP/预览/快照
  /// 六路输出自动全都带上，不可能漂移。
  ///
  /// **这是兜底值**：`live_osd` 非空时每拍取的是那一位手上的当期快照。留着它
  /// 是为了手搓 `SessionOptions` 的调用方（`tests/` 与各种 harness）一行都不用
  /// 改 —— 同时它也是 `live_osd` 为空的唯一一种合理情形：没有人在热改水印。
  OsdConfig osd;

  /// 热生效的水印配置（见 common/live_osd.h）。**借用**，必须活得比这一趟
  /// `run()` 久；可以为空（那时用上面那份常量 `osd`）。
  ///
  /// 采集循环每拍取一次快照，所以网页上保存、NVR 下发 `SetOSD`，画面**下一帧**
  /// 就变 —— 不重建管线、不切分录像。代价是同一段录像里可能有两种水印位置，
  /// 这是明说的取舍（README「已知取舍」）。
  LiveOsd const* live_osd = nullptr;

  /// 水印里 `name` 那一项读的是它。**借用**，必须活得比这一趟 run() 久；
  /// 可以为空 —— 那时名称项画不出东西，但不报错。
  DeviceState const* device = nullptr;

  /// 性能不足时的降载策略。
  AdaptiveConfig adaptive;

  /// 空 = 不写文件。录制和推流可以同时开。
  std::string output_path;
  int seconds = 0;              ///< 0 = 一直跑到 Ctrl+C

  /// 子码流。**第二路编码器**（不是"编一次再抽帧"——那要先解码，纯浪费）。
  ///
  /// 只有开了 RTSP 才有意义：子码流存在的唯一理由是给 NVR 的多画面网格用，
  /// 而那条路走的是 RTSP。`rtsp.enabled` 为假时这个开关被忽略。
  ///
  /// 尺寸按**高度**给、宽度按主码流的宽高比推 —— NVR 的界面是按高度分档的
  /// （D1 / 720p / 360p），而 16:10、21:9 这些比例下写死宽度会变形。
  bool sub_enabled = false;
  int sub_height = 360;
  int sub_bitrate_kbps = 512;

  /// 音频（AAC）。
  ///
  /// **直接用 `AudioConfig`，不再另立一个形状相同的结构。** 水印和自适应也是
  /// 这么做的（`o.osd = c.osd`），理由是同一个：两份字段表迟早会不一致 ——
  /// 加一个字段只改了一边，而这里两边的类型不同，**编译器一个字都不会说**。
  /// 症状是"网页上改了增益，重载之后又跳回 1.0"，而两种方向（存进配置、
  /// 读回选项）各错一半，看起来像随机的。
  ///
  /// 音轨一旦起来就**同时**进 MP4 与 RTSP 主码流（同一个会话里的第二个
  /// subsession，见 `RtspAudioOptions`）。想让 NVR 那侧回到纯视频，就把
  /// `enabled` 关掉 —— 那也同时关掉了录像里的音轨，这是刻意的：一个开关，
  /// 一条语义，不是"关了一半"。
  AudioConfig audio;

  /// 这一路**想要**什么样的码流：路径、fps、进 SDP 的带宽估计、队列深度。
  ///
  /// ⚠️ **本阶段起它不再描述"要不要起 RTSP 服务"。** 服务器现在是**一台**，
  /// 由 `cmd_run` 持有、服务全部通道（见 `rtsp_server` 与 `rtsp_stream_base`）。
  /// 这里留着的意义是"这一路叫什么、队列多深"—— `cmd_run` 把各通道的这些
  /// 拼成一份 `RtspOptions` 再起那一台服务器。
  ///
  /// `rtsp.enabled` / `rtsp.port` 这两个字段在**配置那一层**仍然是权威的
  /// （`sync_options_to_config` / `apply_config_to_options` 还在用它们），
  /// 只是会话不再拿它们去起服务器。
  RtspOptions rtsp;

  /// **共享的** RTSP 服务器。空 = 这一路不推流（配置里 rtsp 关着、
  /// 或者通道数超过服务器建好的路数）。
  ///
  /// 借用，必须活得比这一趟 `run()` 久。它的所有权在 `cmd_run`。
  RtspServer* rtsp_server = nullptr;

  /// 本通道在共享服务器上的**主码流**下标；子码流（如果有）是它 +1。
  ///
  /// "通道 N 的主码流在 `stream_base`、子码流在 `stream_base + 1`"这条约定
  /// 由 `cmd_run` 建服务器时的顺序保证 —— 会话只按这个下标推，不认识通道编号。
  std::size_t rtsp_stream_base = 0;

  /// **本通道占了几路**：1 = 只有主码流，2 = 主 + 子。由 `cmd_run` 填。
  ///
  /// ## 为什么不写成 `stream_base + 1 < server->stream_count()`
  ///
  /// 那个写法**挡不住真正的危险**。越界会被 `push_annex_b` 静默忽略（它的契约
  /// 就是这样），但 `stream_base + 1` 通常**不越界** —— 它正好是**下一个通道的
  /// 主码流**。于是通道 1 的子码流地址上播的是通道 2 的画面，两路流各自都
  /// "正常"，只有画面是错的。
  ///
  /// 这是整个共享服务器设计里唯一一种"错得完全安静"的失败方式，所以它不该
  /// 靠一次算术去挡，而该由一个**说得清的数字**决定："这台服务器上给这一路
  /// 排了几个槽"。填错了是 `cmd_run` 的错，而那里的信息是完整的。
  unsigned rtsp_stream_slots = 0;
};

/// 把「这一趟实际生效」的采集与 RTSP 选项收回 `AppConfig`。
///
/// 命令行的每一处覆盖都是"以 cfg 为默认值再写回 opt"，所以开机时 `cfg` 里
/// 装的是**文件写的值**、`opt` 里装的是**真正生效的值**。这两个函数就是
/// 在这两个世界之间搭桥，**必须成对使用、字段表必须一致**：
///
///   * `sync_options_to_config()` 开机时跑一次（命令行解析完、Web 起来之前），
///     让 `GET /api/config` 报的是生效值而不是文件里的那份 —— 不然
///     `--fps 60` 之后网页上显示的还是 25，看起来像命令行没生效。
///   * `apply_config_to_options()` 重载时跑（`POST /api/reload`），把用户刚存的
///     配置推回 opt。
///
/// **为什么成对放在这里而不是各写各的**：这两张字段表一旦漂移，症状是
/// "某个字段改了但重载之后没变"或"重载之后它自己变回去了" —— 而两次运行
/// （开机 / 重载）里只有一次是错的，看起来像随机行为。放在同一个文件的相邻
/// 位置，改一边时另一边就在眼前。
///
/// 注意 `sync` 那侧**不碰 `capture.quality`**：用户手改了 fps 之后档位该显示
/// "自定义"，而那是前端按 diff 决定的；在这里猜一个档位回去会让"我明明改成
/// 30 帧，它却报均衡"变成两个互相矛盾的数字。
void sync_options_to_config(SessionOptions const& o, AppConfig& c);

/// `sync_options_to_config()` 的逆运算。**只动采集与 RTSP 那批字段**，
/// `output_path` / `seconds` / `device` 这些运行期字段原样保留 —— 重载不该
/// 换掉正在录的那个文件名。
void apply_config_to_options(AppConfig const& c, SessionOptions& o);

/// 通道 2 起的选项：从**通道 1 已经生效的那一份**继承，只覆盖计划里说了算的
/// 字段。
///
/// ## 为什么是"从 `base` 继承"而不是"从 `AppConfig` 重建"
///
/// `channels.items[]` 里只有 `quality` / `fps` / `bitrate_kbps` / `max_height` /
/// 子码流 / 路径这几项。水印、自适应、GOP、预设、编码线程数、时间预算**都没
/// 有**通道级的对应字段 —— 从 `AppConfig` 重建一份的话，`--osd-name`、
/// `--no-osd`、`--adaptive off`、`--gop 1` 这些命令行覆盖在通道 2 上会静默
/// 失效。而"通道 1 有水印、通道 2 没有"这种症状没有任何一处会报出来。
///
/// ## 只在这一处和通道 1 不一样：音频
///
/// `audio.enabled` **无条件关掉**。多开一路 WASAPI 环回 = 同一份系统声音被抓
/// 两遍、编两遍，白烧一份 CPU，而 NVR 那边两路听起来一模一样（还有回声般的
/// 相位差）。它是这一份里唯一一个"故意不继承"的字段，所以写在最显眼的地方。
///
/// @param record_path 这一路要写的文件；空 = 不录。
SessionOptions options_for_channel(ChannelPlan const& plan,
                                   SessionOptions const& base,
                                   std::string const& record_path);

struct SessionStats {
  double elapsed_s = 0.0;
  std::uint64_t ticks = 0;            ///< 按目标帧率应该产出的帧数
  std::uint64_t content_frames = 0;   ///< 其中画面真的变了的
  std::uint64_t queue_dropped = 0;    ///< 编码跟不上而丢掉的
  std::uint64_t pool_exhausted = 0;   ///< 池子空而跳过采集的次数
  /// 自适应降载主动丢掉的拍数。**和 queue_dropped 分开计**：一个是"我们
  /// 故意降了帧率"，另一个是"编码器出问题了"，混成一个数字就没法归因。
  std::uint64_t adaptive_dropped = 0;
  /// **桌面没动、只有水印的文字变了**而额外产出的帧数。
  ///
  /// 它是"水印上的钟在走"的证据：恒等于 0 而水印明明开着，说明静止桌面上
  /// 秒数冻住了（编码器复用了旧 YUV），那比没有水印更糟。
  std::uint64_t osd_refresh_frames = 0;
  /// **水印配置变了、桌面没动**，于是从"没有水印的那份桌面图"重新合成的帧数。
  ///
  /// 和上面那个是两件事：`osd_refresh_frames` 是钟在走（每秒一次，自动发生），
  /// 这个是**有人改了配置**（网页保存 / NVR 下发 `SetOSD`）才发生一次。它存在的
  /// 理由是让"水印改成热生效"这句话可观测 —— 网页上改一下位置，这个数应该
  /// 在下一拍就 +1，而进程没有重启、`pending` 里也没有 `osd.*`。
  std::uint64_t osd_rebuilt_frames = 0;
  /// **有人改了水印配置、采集侧真的把它换上了**的次数（网页保存 / NVR 下发
  /// `SetOSD`）。和上面那个是**两个不同的量**，而且这个才是"热生效"的证据：
  ///
  ///   * `osd_rebuilt_frames` 只在**桌面静止**时才涨（桌面本来就在动的话，
  ///     下一帧自然会带上新水印，不需要重新合成）。所以它等于 0 **不代表**
  ///     热生效没发生 —— 在一台一直在动的桌面上它永远是 0（陷阱 87 的形状：
  ///     宾语是"这台机器的桌面动没动"，不是产品）。
  ///   * 这一个与环境无关：**每一次真正的配置变更恰好 +1**，不管桌面动没动。
  ///
  /// 它存在的理由是把 `pending` 里那条**否定式**证据（"`osd.*` 不在 pending 里"）
  /// 配上一条**肯定式**的：`applied` / `pending` 由调用方写 `running_cfg.osd`
  /// 决定，而"写了 `running_cfg` 却忘了 `live_osd.set()`"完全写得出来 ——
  /// 那时界面照样说已生效，只有这个数不涨（陷阱 91 的同一条纪律：否定式断言
  /// 必须有一个肯定式的伴）。
  std::uint64_t osd_hot_applied = 0;

  /// 自适应降载的当前状态。`perf_hint` 是**给人看的中文**，前端直接显示 ——
  /// 让前端拿 state 去拼文案会变成两份真相（改一边忘一边）。
  std::string perf_state = "ok";
  int perf_duty = 1;                  ///< 每几拍出一帧，1 = 不丢
  int perf_target_fps = 0;            ///< 实际目标帧率
  std::string perf_hint;
  /// `perf_hint` 里那句建议对应的**画质档位名**（`high`/`balanced`/…），空串 =
  /// 没有可一键应用的档位。
  ///
  /// **由调用方（main.cpp 的 stats hook）填，会话自己不填**：会话不认识"画质
  /// 档位"这个概念（它只知道 fps / 码率 / 上限高度），而 `quality` 这个标签
  /// 住在 `AppConfig` 里。让会话去读配置就等于把配置的权威副本拖进采集线程。
  std::string perf_suggested_quality;
  /// 最近 10 秒内的丢弃数。**累计值会让人以为一直在丢**，而它多半只是启动
  /// 那一下造成的。
  std::uint64_t drops_recent = 0;
  std::uint64_t packets = 0;
  std::uint64_t keyframes = 0;
  std::uint64_t bytes = 0;
  std::uint64_t access_lost = 0;
  double effective_fps = 0.0;         ///< packets / elapsed

  /// 采集画面的真实尺寸。ONVIF 的 `GetProfiles` 要报它 —— 报成一个
  /// "典型"的 1920x1080 而实际推的是别的尺寸，有些 NVR 会拒绝这个 profile，
  /// 而症状只是"添加失败"，没有任何线索指向分辨率。
  int video_width = 0;
  int video_height = 0;

  /// 子码流编码器的实际尺寸。**0 = 没开子码流**。宽度是按主码流的宽高比
  /// 推出来再取偶的，所以不能由调用方自己算 —— ONVIF 的 GetProfiles 要报
  /// 真实值，报错了有些 NVR 会直接拒绝这个 profile。
  int sub_width = 0;
  int sub_height = 0;

  /// 音频。`audio_state` 取值 `off` / `ok` / `nosignal` / `error`
  /// （`ok` 时**安静也是正常** —— 系统不出声不是故障）。
  ///
  /// `audio_frames` 是音轨已有的**样本帧数**，除以 48000 就是音轨时长。它是
  /// "音轨和视频一样长"这件事唯一的依据（包数是编码器的节奏，不是时长）。
  /// `audio_zero_filled` 是其中由我们补零的帧数 —— **系统安静时它本来就涨**，
  /// 不是故障指示。
  std::string audio_state = "off";
  float audio_level_l = 0.0f;         ///< 峰值（0..1），已经过释放
  float audio_level_r = 0.0f;
  std::uint64_t audio_frames = 0;
  std::uint64_t audio_packets = 0;
  std::uint64_t audio_zero_filled = 0;
  /// 采样率。**只为了把 `audio_frames` 换成人能读的秒数**（0 = 音频没开）。
  /// 上面那句注释里"除以 48000"写死了，而采样率是配置项 —— 这里给出来，
  /// 免得读的人（或下一个写脚本的人）自己猜一个数。
  int audio_sample_rate = 0;

  /// 鼠标指针合成。`cursor_frames` 是落过指针像素的帧；`pointer_only_frames`
  /// 是其中**只因为鼠标动了**才产出的那些 —— 桌面静止时光标还在动，靠的就是
  /// 它们。后者恒等于 0 而鼠标明明在动，说明指针那一套没生效。
  std::uint64_t cursor_frames = 0;
  std::uint64_t pointer_only_frames = 0;

  /// 每一路 RTSP 码流各一份。**下标和 RtspOptions::streams、编码器一一对应**：
  /// [0] 主码流，[1] 子码流。没开 RTSP 时整个数组是空的。
  struct RtspStreamStats {
    std::string path;                 ///< 不含前导斜杠，和 RtspStreamOptions 一致
    std::string url;                  ///< 完整播流地址，含本机 IP 和端口
    unsigned clients = 0;
    std::uint64_t nalu_dropped = 0;   ///< 该路推流队列满了丢掉的 NALU
    std::size_t max_nalu_bytes = 0;   ///< 该路见过的最大 NALU，用来盯截断风险
    std::uint64_t packets = 0;        ///< 该路编码器产出的包数
    std::uint64_t bytes = 0;
  };
  std::vector<RtspStreamStats> rtsp_streams;

  std::string summary() const;
};

class CaptureSession {
 public:
  /// run() 的三种结局。**刻意不是 bool**："跑完了"和"这条链得整个重建一遍"
  /// 是完全不同的两件事，而后者是要接着干活的信号 —— 混成一个 false 的话，
  /// 调用方要么把它当失败退出，要么就得再去看一个布尔成员，两种写法都容易漏。
  enum class RunResult {
    Finished,  ///< 正常结束（到时间、Ctrl+C、被 request_stop）
    Restart,   ///< 采集尺寸中途变了，调用方应当**再调一次 run()**（新文件、新 RTSP）
    Failed,    ///< 出错，err 里是人话
  };

  CaptureSession() = default;
  ~CaptureSession();

  CaptureSession(CaptureSession const&) = delete;
  CaptureSession& operator=(CaptureSession const&) = delete;

  /// 阻塞直到跑完（到时间或 Ctrl+C）。失败时 err 填入人话。
  ///
  /// 返回 Restart 时这一趟已经**完整收尾**了：MP4 关了（能播）、RTSP 停了、
  /// 统计是齐的。调用方拿新的 SessionOptions 再调一次即可 —— 之所以要重来
  /// 一趟而不是就地改，是因为编码器的 SPS/PPS、MP4 的轨道头、SDP 里的
  /// `sprop-parameter-sets` 都在创建时就把尺寸写死了。
  ///
  /// `opt` 是**非常量引用**：这一趟会把它里面那些"由桌面决定"的字段写回成
  /// 实际生效的值（目前是 `max_height` 被夹到桌面高度以下时的结果）。调用方
  /// 拿同一份 opt 反复调（分段循环就是这么做的），写回是它唯一能看到
  /// "实际用的是哪个尺寸"的途径 —— 让它自己去猜等于两份真相。
  RunResult run(SessionOptions& opt, std::string& err);

  /// 每秒一次的统计回调，**在采集线程上**调用（就是跑 `run()` 的那个线程）。
  ///
  /// 给 Web 层推状态用。用回调而不是"让别的线程去读 `stats()`"是刻意的：
  /// `stats_` 是采集线程在写的普通结构体，另一个线程去读它是数据竞争 ——
  /// 而它的表现（偶尔读到半新半旧的字节）恰好是最难查的那一类。
  /// 走回调就不存在共享：回调里拿到的是采集线程自己填好的一份。
  ///
  /// 回调**不得阻塞**（它卡住的就是采集循环）。WebServer::publish 只是把
  /// 一个任务投出去，符合这个要求。必须在 `run()` 之前设置。
  using StatsHook = std::function<void(SessionStats const&)>;
  void set_stats_hook(StatsHook h) { stats_hook_ = std::move(h); }

  /// 浏览器预览的 fMP4 字节流。**在编码线程上调用，不得阻塞**
  /// （WebServer::publish_preview 只做投递，符合这个要求）。必须在 run() 之前设置。
  ///
  /// `is_init = true` 的那一份是初始化段（ftyp+moov），**必定先于任何分片出现**
  /// —— 顺序是这里保证的：两者都在编码线程上发，而"MSE 只认第一个 append 进去
  /// 的初始化段"是个晚一个字节就整条流解不出来的硬要求。
  ///
  /// 字节用 shared_ptr 交出去而不是拷一份：这条路每两秒才走一次、一块几百 KB，
  /// 但拷贝发生在**编码线程**上；让引用计数代替 memcpy 是白拿的。
  ///
  /// 分片一律从关键帧开始（封装器按关键帧断片），所以中途连上来的客户端拿到
  /// init + 下一个分片就能出画，不需要回放历史。
  ///
  /// 钩子为空 = 不做这一路封装（省掉每秒半次 mux 的开销）。
  using PreviewHook =
      std::function<void(bool is_init, std::shared_ptr<std::vector<std::uint8_t> const>)>;
  void set_preview_hook(PreviewHook h) { preview_hook_ = std::move(h); }

  /// 浏览器预览的"时间原点"：**媒体时间 0 点对应的墙钟**（Unix 毫秒）。
  /// **在编码线程上调用，不得阻塞**（WebServer 那侧只做投递）。必须在 run() 之前设置。
  ///
  /// 它存在的唯一理由：前端那把"真实延迟"尺子需要把 `<video>.currentTime`（媒体时间）
  /// 换算成墙钟才能和 `Date.now()` 相减。没有它，前端只能报"我们手上数据的末尾"
  /// 离播放头多远 —— 那个数稳定在它自己的追帧目标上，看着很合理，却不是延迟。
  ///
  /// **只报一次**（一个会话一个 0 点）。这个钩子为空 = 不报，前端显示"未知"
  /// —— 那正是我们要的：宁可说不知道，也不要报一个看起来合理的假数。
  ///
  /// 值来自封装器（`Fmp4Muxer::t0_wall_ms()`），不是我们自己记的时间：定 0 点的是
  /// 封装器 —— 这是量出来的（见 `fmp4_muxer.cpp` 里 write_nal() 那段注释）。
  using PreviewT0Hook = std::function<void(std::int64_t t0_wall_ms)>;
  void set_preview_t0_hook(PreviewT0Hook h) { preview_t0_hook_ = std::move(h); }

  /// 线程安全。Ctrl+C 的处理函数会调它。
  void request_stop() { stop_.store(true, std::memory_order_relaxed); }
  bool stop_requested() const { return stop_.load(std::memory_order_relaxed); }

  /// 线程安全。请求"这一趟体面收尾，然后按**新的** `SessionOptions` 再来一趟"。
  ///
  /// 和 `request_stop()` 是两条路，**不能合并成一个**：停止是"用户不要了"，
  /// 重配是"用户改了参数、要他立刻看到效果"。混成一个的话，调用方没法区分
  /// "跑完了该退出"和"跑完了该接着跑" —— 而这正是 `RunResult` 三态要回答的。
  ///
  /// 落点和 `RunResult::Restart` 完全一样（尺寸变化走的就是它），所以这里
  /// 只是多置一个位、让收尾时把返回值从 `Finished` 改成 `Restart`。
  /// **收起尾来是完整的**：编码器冲干净、MP4 写完 trailer、RTSP 停掉 ——
  /// 调用方拿到的是一段能播的文件，不是被砍断的那种。
  ///
  /// 调用方（`main.cpp` 的分段循环）在拿到 `Restart` 之后必须**重算
  /// `SessionOptions`**，否则新的一趟跑的还是一模一样的参数。
  void request_reconfigure() {
    reconfigure_.store(true, std::memory_order_relaxed);
  }
  bool reconfigure_requested() const {
    return reconfigure_.load(std::memory_order_relaxed);
  }

  /// 最近一帧 BGRA，供 `GET /snapshot.jpg` 用。线程安全；还没有帧时返回空。
  ///
  /// **返回的是帧池里那个 shared_ptr 本身，不是拷贝。** 两件事让这么做是
  /// 安全的：帧一旦 publish 就不可变；多持有一个 shared_ptr 等于告诉池子
  /// "这块还在用"，它不会回收去装新画面。于是拿到手的是零拷贝、且保证不会
  /// 再变的内存。
  ///
  /// 拷贝一份看似更"稳"，其实更糟：拖到共享的拷贝既慢（2880x1920 是 22 MB，
  /// 每秒一次），又仍要面对"拷到一半源被覆盖"的撕裂 —— 除非再加一把锁把它
  /// 挡住，而那把锁就架在采集循环的正中间。
  ///
  /// 代价是池子**永久少一格**（默认 6 → 5）。池深本来就是吸收抖动用，
  /// 少一格不影响采集，换来的是快照路径零拷贝。
  std::shared_ptr<BgraFrame const> snapshot() const;

  SessionStats const& stats() const { return stats_; }

 private:
  std::atomic<bool> stop_{false};
  /// 见 `request_reconfigure()`。
  std::atomic<bool> reconfigure_{false};
  SessionStats stats_{};
  StatsHook stats_hook_;
  PreviewHook preview_hook_;
  PreviewT0Hook preview_t0_hook_;

  /// 快照留档。采集线程写（每秒一次），web 循环线程读，所以要一把锁。
  /// 锁的临界区只有一个 shared_ptr 的赋值/拷贝 —— **绝不要在持锁时去编码
  /// JPEG**，那会把采集循环堵住。
  mutable std::mutex snapshot_mu_;
  std::shared_ptr<BgraFrame const> snapshot_;
};

}  // namespace d2o
