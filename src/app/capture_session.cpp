#include "app/capture_session.h"

#include "app/audio_track.h"
#include "capture/frame_source.h"
#include "capture/display_info.h"
#include "capture/frame.h"
#include "capture/osd_overlay.h"
#include "common/adaptive.h"
#include "common/bounded_queue.h"
#include "common/device_state.h"
#include "common/log.h"
#include "encoder/fmp4_muxer.h"
#include "encoder/mp4_writer.h"
#include "encoder/video_encoder.h"
#include "rtsp/rtsp_server.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#ifdef _WIN32
#  include <windows.h>
#endif

namespace d2o {

namespace {

using SteadyClock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

/// 连续重建失败多少次之后把告警降频。**这不是"放弃"**：显示器被拔掉、系统
/// 休眠、切换用户都会让会话在一段时间里根本建不起来，而"过一会儿插回来"是
/// 常态。放弃等于交给用户一台"显示器碰一下就死"的摄像机，而它连一句"我放弃
/// 了"都只说一次。改成降频重试之后，插回来就自动接着录。
constexpr int kNoisyRecoverAttempts = 10;

/// 降频之后每隔这么多秒再报一次，免得挂一整夜刷出几万行日志。
constexpr int kRecoverWarnIntervalSeconds = 60;

/// 给网页横幅用的中文提示。空串 = 没什么好说的（性能正常）。
///
/// **在这里拼而不是让前端按 `perf_state` 去拼**：前端拼的话，改一边忘一边，
/// 屏幕上就会出现"overloaded 但提示写着一切正常"。文案只有一份真相。
std::string perf_hint_for(AdaptiveController const& a, std::uint64_t drops_recent,
                          int base_fps) {
  if (a.duty() <= 1) {
    // 刚恢复过来的一小段时间里也别急着报"一切正常" —— 那会让横幅刚亮起
    // 就消失，比不亮更让人摸不着头脑。
    if (drops_recent > 0) {
      return fmt("已恢复实时（最近丢弃 %llu 帧）",
                 static_cast<unsigned long long>(drops_recent));
    }
    return std::string();
  }

  if (a.state() == PerfState::Overloaded) {
    return fmt("本设备性能不足，已经自动把帧率降到 %d fps（原 %d fps）仍然跟不上。"
               "建议降低画质：清晰度会让一点，但画面能重新变得流畅。",
               a.target_fps(), base_fps);
  }

  return fmt("性能不足，已自动把帧率降到 %d fps（原 %d fps）以保持实时 —— "
             "画面不会卡顿或积压，只是帧数少了。降低画质可以换回完整帧率。",
             a.target_fps(), base_fps);
}

/// 会话失效之后隔多久做第一次重建。显示模式切换要几帧才稳定，立刻重建多半
/// 再失败一次；但也不能久 —— 这段时间画面是停住的。
constexpr int kFirstRecoverDelayMs = 150;

/// 重建失败的重试间隔上限。退让到这个值就封顶：再长的话显示器插回来了却要
/// 等半天才恢复。
constexpr int kMaxRecoverDelayMs = 2000;

/// 落后超过这么多秒就重设时间基准，见 run() 里的说明。
constexpr int kMaxLagSeconds = 2;

/// 编码线程判断"队列空了"用的超时。**它只是轮询间隔，不是停机信号** ——
/// 停机走 close()。见 BoundedQueue::PopStatus。
using QueuePopStatus = BoundedQueue<CapturedFrame>::PopStatus;

/// Ctrl+C。默认行为是直接终止进程 —— 那样 MP4 的索引写不完，文件是坏的。
std::atomic<bool> g_interrupted{false};

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      g_interrupted.store(true, std::memory_order_relaxed);
      return TRUE;  // 自己处理了，不要走默认的立即结束
    default:
      return FALSE;
  }
}
#endif

/// 编码出来的包的一个下游。
struct PacketConsumer {
  virtual ~PacketConsumer() = default;
  /// 失败时填 err 并返回 false，整条管线就停。
  virtual bool on_packet(AVPacket& pkt, std::string& err) = 0;

  std::string error;  ///< 每个下游各持一份，互不覆盖
  bool ok = true;
};

/// 分发给多个下游。
///
/// **consumers 的顺序是有语义的**：会"拿走"包内容的下游必须排在最后。
/// `av_interleaved_write_frame` 写完之后包就被清空了（详见 Mp4Writer::write），
/// 所以 Mp4Consumer 一旦排在 RtspConsumer 前面，后面那位拿到的就是一堆空包 ——
/// 而且不会报错，只表现为"有连接、无画面"。
struct PacketTap {
  std::vector<PacketConsumer*> consumers;
  std::atomic<std::uint64_t>* packets = nullptr;
  std::atomic<std::uint64_t>* keyframes = nullptr;
  std::atomic<std::uint64_t>* bytes = nullptr;
  std::string error;
  bool ok = true;

  void operator()(AVPacket& pkt) {
    if (!ok) return;
    // 顺序要紧：下面任何一个下游都可能把包清空，所以大小和关键帧标志先读出来。
    std::uint64_t const size = static_cast<std::uint64_t>(pkt.size);
    bool const key = (pkt.flags & AV_PKT_FLAG_KEY) != 0;

    for (PacketConsumer* c : consumers) {
      if (!c->ok) continue;
      if (!c->on_packet(pkt, c->error)) {
        c->ok = false;
        ok = false;
        error = c->error;
        return;
      }
    }
    if (packets) packets->fetch_add(1, std::memory_order_relaxed);
    if (key && keyframes) keyframes->fetch_add(1, std::memory_order_relaxed);
    if (bytes) bytes->fetch_add(size, std::memory_order_relaxed);
  }
};

/// 下游：写 MP4。**会拿走包的内容**，必须排在最后。
///
/// `stream_index` 由**这里**填，不是由封装器改写（`Mp4Writer::write` 只认它，
/// 见那里的说明）。视频这一路现在恒为 0；音频接进来之后音频的下游填 1。
struct Mp4Consumer : PacketConsumer {
  Mp4Writer* writer = nullptr;
  int stream_index = 0;
  bool on_packet(AVPacket& pkt, std::string& err) override {
    pkt.stream_index = stream_index;
    return writer->write(pkt, err);
  }
};

/// 下游：切分 NALU 推 RTSP。只读，不碰包的内容。
///
/// `stream` 是这一路在 RtspServer 里的下标。主码流和子码流各有一个这个对象，
/// 各推各的 —— 它们拿到的 AVPacket 来自两个独立的编码器，NALU 边界、时间戳
/// 和参数集都不同，混在一起就会互相打乱对方的 GOP。
struct RtspConsumer : PacketConsumer {
  RtspServer* server = nullptr;
  std::size_t stream = 0;
  bool on_packet(AVPacket& pkt, std::string& err) override {
    (void)err;  // 推流失败不致命：队列自己会丢，客户端那边重连即可
    server->push_annex_b(stream, pkt.data, static_cast<std::size_t>(pkt.size),
                         (pkt.flags & AV_PKT_FLAG_KEY) != 0, pkt.pts);
    return true;
  }
};

/// 下游：把裸 AU 推给 RTSP 的音频那一路。只读，不碰包的内容。
///
/// **注意这里不走 `push_annex_b`**：音频的队列元素是 AU 不是 NALU，没有起始码
/// 要剥、没有参数集要插、也没有"等关键帧"这回事（`AuQueue` 与 `NaluQueue` 是
/// 两个类，理由见 rtsp_server.h）。包里的字节就是 AAC 的裸 AU，原样进去。
///
/// `pkt.pts` 的单位是**样本**（编码器的时基是 1/采样率），`AacLiveSource` 拿它
/// 换算呈现时间 —— 这里**不要**做任何单位转换。
struct RtspAudioConsumer : PacketConsumer {
  RtspServer* server = nullptr;
  bool on_packet(AVPacket& pkt, std::string& err) override {
    (void)err;
    server->push_audio_au(pkt.data, static_cast<std::size_t>(pkt.size), pkt.pts);
    return true;
  }
};

/// 浏览器预览那一路的封装器 + 出口钩子。
///
/// 跨线程边界只有一处：把封装好的字节交给钩子，由 Web 层投递到它自己的循环上。
/// **不要在这里碰任何 WS 连接** —— 这段代码在编码线程上跑。
struct PreviewFeed {
  Fmp4Muxer muxer;
  int fps = 30;
  CaptureSession::PreviewHook hook;
  /// 每帧复用，免得每片一次 malloc。write() 是"追加"语义，所以每轮先清。
  std::vector<std::uint8_t> buf;

  /// 每写进去多少帧就**主动切一刀**（`muxer.flush()`）。0 = 一切照旧，分片只跟
  /// 关键帧走。
  ///
  /// 这是预览真实延迟里**最后一层**、也是唯一由我们说了算的那一层：分片只在
  /// 关键帧处落地时，第 N 个 GOP 的分片要等到第 N+1 个关键帧才发得出去 ——
  /// 于是"画面现在在哪"和"浏览器手上有什么"之间永远隔着平均半个 GOP
  /// （`gop_seconds = 2` 时约 1 秒、最坏 2 秒）。这一项和采集快慢、网络、浏览器
  /// 都没有关系，只跟"什么时候切"有关，所以它是可以单独量、单独改的。
  ///
  /// **切出来的分片不以关键帧开头**，这是这个功能的全部意义，也是它唯一的风险
  /// （MSE 收不收，见 README 里那段）；`.cpp` 的接线处按 fps/4 给值 ≈ 250 ms。
  int flush_every = 0;
  /// 距离这一刀已经写进去多少帧。只用于数 `flush_every` 的节拍。
  int since_flush = 0;
  /// 切分片失败只报一次。理由与 `warned` 相同：每秒一次的 ERROR 会把别的线索冲掉。
  bool flush_warned = false;

  /// 时间原点的出口。见 `CaptureSession::PreviewT0Hook`。
  CaptureSession::PreviewT0Hook t0_hook;
  /// **媒体时间 0 点对应的墙钟 = 这个基准 + 该样本的 pts**。由采集循环在 tick 0
  /// 播种一次（`note_t0_base_ms`），之后每帧只读。
  ///
  /// 为什么一个**常数**就够：采集侧的 PTS 就是拍号、每拍 +1（自适应降载改的是
  /// "每 d 拍入队一次"，拍号本身的量纲没变），所以任意一对 (墙钟, 拍号) 都能定出
  /// 同一个常数。取 tick 0 是为了不额外引入"第一帧实际在哪一拍"的抖动。
  ///
  /// 原子只是为了读起来不必推敲内存序：写发生在采集线程、读也发生在采集线程
  /// （同一拍内先后），跨线程根本用不上。换成普通成员照样对。
  std::atomic<std::int64_t> t0_base_ms{0};
  /// 报告过没有。**不能拿 `muxer.has_t0()` 兼职**：那个说的是"封装器定过 0 点了"，
  /// 这个说的是"我们已经把 0 点发出去了"，两件事会在"钩子为空"时分开。
  bool t0_reported = false;

  /// 由采集循环在 tick 0 调一次。**必须是墙钟**（`system_clock::now()`），
  /// **不是设备时间** —— 设备时间是主机时间加 NVR 下发的偏移，拿它去和浏览器的
  /// `Date.now()` 相减会平白多出一个偏移量（可能几个小时）。
  void note_t0_base_ms(std::int64_t ms) {
    t0_base_ms.store(ms, std::memory_order_relaxed);
  }

  /// **必须在编码线程上、且在任何 on_packet 之前调。**
  ///
  /// 初始化段的顺序是 MSE 的硬要求，而"同一条线程顺序调用"是唯一能保证它的
  /// 办法 —— 换个线程发就得靠 post() 的投递顺序，而跨线程的 post() 没有顺序
  /// 保证（这里两个调用都在编码线程上，所以有）。
  void begin() {
    if (!hook || !muxer.ready()) return;
    hook(/*is_init=*/true,
         std::make_shared<std::vector<std::uint8_t> const>(muxer.init_segment()));
  }

  /// 0 点定了就报一次。**在 `muxer.write()` 成功之后调** —— 定 0 点的是封装器
  /// 把包真的交出去那一刻，不是我们喂进去那一刻（见 `fmp4_muxer.cpp` 里
  /// write_nal() 那段注释）。
  void maybe_report_t0() {
    if (t0_reported || !t0_hook) return;
    if (!muxer.has_t0()) return;
    t0_reported = true;
    t0_hook(muxer.t0_wall_ms());
  }

  void on_packet(AVPacket& pkt) {
    if (!hook || !muxer.ready()) return;
    if (pkt.data == nullptr || pkt.size <= 0) return;

    Fmp4Sample s;
    s.data = pkt.data;
    s.size = static_cast<std::size_t>(pkt.size);
    // 编码器的 pts 是**帧序号**，时基 1/fps（见 video_encoder.h）。
    // 直接当微秒用会让整条时间轴快/慢 fps 倍 —— 而现象是"画面在放，速度不对"，
    // 很难往时间戳上想。
    s.pts_us = fps > 0 ? pkt.pts * 1000000 / fps : pkt.pts * 33333;
    s.dts_us = s.pts_us;  // bframes=0，dts 恒等于 pts
    s.keyframe = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
    // 墙钟：基准 + 媒体时间。**样本自己在采集那一刻的墙钟**也要对得上这条式子，
    // 否则前端算出来的"这一帧是什么时候拍的"就是错的 —— 而它看起来只是个
    // 稍微大一点/小一点的数。
    {
      std::int64_t const base = t0_base_ms.load(std::memory_order_relaxed);
      if (base > 0) s.wall_ms = base + s.pts_us / 1000;
    }

    buf.clear();
    // 单个样本没写进去不致命：这是实时流，下一个关键帧就会把分片补齐。
    // 报一次就不再刷屏 —— 每秒一次的错误日志会把别的线索全冲掉。
    if (!muxer.write(s, buf)) {
      if (!warned) {
        warned = true;
        D2O_WARN("有样本没能进入 fMP4 封装器，浏览器预览可能出现跳帧。");
      }
      return;
    }

    // **必须在下面那个 `buf.empty()` 提前返回之前报 t0。**
    // 喂进去的样本**远多于**分片：分片只在关键帧处（或者主动切分处）产生，
    // 而 t0 在第一个样本上就定了。把报 t0 挪到"有分片了"之后，前端在那之前
    // 只能显示"未知"，而它恰好是用户刚打开页面、最盯着看的那一会儿。
    //
    // 排在主动切分**之前**：切不切得成都不影响 t0，而它越早出去，前端越早
    // 显示得出"这一帧是什么时候拍的"。
    maybe_report_t0();

    // **主动切一刀，别等下一个关键帧。** 见 `flush_every` 那段：不加这一步，
    // 真实延迟里就永远含着"平均半个 GOP"。
    //
    // 切在 `write()` **之后**：刚喂进去的这一帧要落在这一刀里。切在它之前等于
    // 把最新的一帧留在封装器里，等下一次再切 —— 那正好又晚了一整拍，而这个
    // 功能的全部意义就是不再晚。
    //
    // 切出来的分片和 `write()` 自己吐出来的分片**拼在同一个 `buf` 里一起发**
    // （两处都是"追加"语义，所以这里不用再分一次消息）。分成两条发也可以，
    // 但那样一次 append 会变成两次，而 MSE 那边除了多一次调用什么也换不来。
    if (flush_every > 0 && ++since_flush >= flush_every) {
      since_flush = 0;
      if (!muxer.flush(buf) && !flush_warned) {
        flush_warned = true;
        D2O_WARN("fMP4 主动切分片失败，浏览器预览的延迟可能会变大。");
      }
    }

    // **空是常态**：关掉主动切分时只有关键帧才产出分片，开着时也要攒够
    // `flush_every` 帧才有一次非空。两种配置下"这一次 write() 一个字节都没
    // 吐出来"都是正常的一帧，不能当错误 —— 上面那句 WARN 管的是"写失败"。
    if (buf.empty()) return;

    hook(/*is_init=*/false,
         std::make_shared<std::vector<std::uint8_t> const>(std::move(buf)));
  }

  bool warned = false;
};

/// 下游：把访问单元喂给预览封装器。只读，不碰包的内容。
struct PreviewConsumer : PacketConsumer {
  PreviewFeed* feed = nullptr;
  bool on_packet(AVPacket& pkt, std::string& err) override {
    (void)err;  // 预览出问题不该拖垮录制/推流，所以这里没有失败路径
    feed->on_packet(pkt);
    return true;
  }
};

/// 编码线程。
///
/// 主码流和子码流是**两个独立的编码器**，跑在同一条线程上、依次处理同一帧
/// BGRA。不是"主码流编完再解码缩一份"——那样要先解一次 H.264，比直接再缩一次
/// 还贵。共用一条线程而不是各起一条，是因为瓶颈是 x264 本身而不是线程调度，
/// 两条线程只会多出一层同步，而且两路的帧序还得分头保证。
struct Pipeline {
  BoundedQueue<CapturedFrame>* queue = nullptr;
  X264Encoder* encoder = nullptr;
  PacketTap* tap = nullptr;
  /// 子码流。nullptr = 没开。开了之后它和主码流**同生共死**：任一路编码失败
  /// 整条管线都停 —— 半条流活着比全死更难查。
  X264Encoder* sub_encoder = nullptr;
  PacketTap* sub_tap = nullptr;
  /// 音轨。nullptr = 音频关着（或者没起来）。
  ///
  /// **它是这一层唯一按"拍"推进的东西**：每弹出一帧视频就 `tick()` 一次，
  /// 音轨于是和视频共用同一个原点（见 audio_track.h）。不这么做的话，音频的
  /// 时间轴就得靠"从设备收到了多少"来推 —— 而系统不出声时环回一个包都不来。
  AudioTrack* audio = nullptr;
  /// 音轨的下游。和主/子码流各自一份 tap 是同一个理由：音频和视频的计数器
  /// 量纲完全不同（AU vs NALU、样本 vs 帧），混在一起两个数都失去意义。
  PacketTap* audio_tap = nullptr;
  /// 不参与 tap 的扇出：它是"发初始化段"这个一次性动作的入口，见 run() 第一行。
  /// 浏览器预览只跟主码流走（子码流是给 NVR 的多画面网格用的，不是给人看的）。
  /// **预览也不带声音**（`Fmp4Muxer` 是单流 + H.264 专用），这是取舍，不是遗漏。
  PreviewFeed* preview = nullptr;

  std::atomic<std::uint64_t> packets{0};
  std::atomic<std::uint64_t> keyframes{0};
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<std::uint64_t> frames{0};
  /// 子码流的计数**单独一套**。合在一起的话，两路的分辨率差 8 倍，
  /// 平均码率变成一个没有意义的数，而"子码流是不是出问题了"就看不出来了。
  std::atomic<std::uint64_t> sub_packets{0};
  std::atomic<std::uint64_t> sub_bytes{0};
  /// 音轨的 AU 计数。**它不进主码流的 packets/bytes** —— 一个 AU 是一帧音频、
  /// 一个包是一帧视频，加起来得到的"帧率"是个没有意义的数。
  std::atomic<std::uint64_t> audio_packets{0};
  std::atomic<std::uint64_t> audio_bytes{0};
  std::atomic<bool> failed{false};
  std::string error;

  void run() {
    // 初始化段**必须在任何分片之前**发出去，而这里是与所有分片同一条线程的
    // 最早位置。放到 run() 外面（主线程）发是不行的：那样两个调用来自两条
    // 线程，投递顺序就得看 post() 的调度，而 MSE 对顺序是零容忍的。
    if (preview) preview->begin();

    CapturedFrame f;
    std::uint64_t last_content_id = 0;
    bool have_last = false;

    // **只有"队列关了"才是收工信号。** 空队列等超时只是"采集侧暂时没帧"，
    // 必须接着等 —— 把它也当成收工就会让编码线程在一次 200 ms 的采集停顿之后
    // 静默退出（不设 failed、不打日志），录制和推流从此全停而进程照常跑到
    // 时间结束、退出码 0。成因与后果见 BoundedQueue::PopStatus 的注释。
    //
    // 采集侧真正会停顿的地方不少：DXGI 重建会话的退让、锁屏、切分辨率、
    // 显示器休眠、系统卡顿。也就是说这不是一条罕见的边界，而是**任何一次
    // 停顿都会踩**的路。
    for (;;) {
      auto const st = queue->pop(f, milliseconds(200));
      if (st == QueuePopStatus::Closed) break;
      if (st == QueuePopStatus::Timeout) continue;

      if (!f.image) continue;

      // 画面自上一帧以来变了没有。没变时编码器跳过整帧色彩转换，
      // 只把上一份 YUV 再编一次 —— 时间戳照样推进，输出仍是恒定帧率。
      bool const changed = !have_last || f.image->content_id != last_content_id;
      auto const pts = static_cast<std::int64_t>(f.tick);

      std::string err;
      if (!encoder->encode(*f.image, pts, changed, std::ref(*tap), err) ||
          !tap->ok) {
        error = tap->ok ? err : tap->error;
        failed.store(true, std::memory_order_relaxed);
        return;
      }

      if (sub_encoder) {
        // 同一个 `changed`：两路看的是同一帧 BGRA，变没变是同一个事实。
        // 各自的编码器会自己决定要不要重做颜色转换。
        std::string serr;
        if (!sub_encoder->encode(*f.image, pts, changed, std::ref(*sub_tap),
                                 serr) ||
            !sub_tap->ok) {
          error = sub_tap->ok ? serr : sub_tap->error;
          failed.store(true, std::memory_order_relaxed);
          return;
        }
      }

      // 音频**在视频之后**推一拍。顺序只是为了让视频的入队不被混音那几步
      // （混 + 编，几百微秒）拖后；时间轴不受影响 —— 两边都用同一个 tick 当原点。
      //
      // **一拍一次是这里唯一允许的节奏**：`AudioTrack::tick` 的产出量由累计拍数
      // 决定，所以帧率一改（画质档位、自适应降载）音轨的时长仍然严格跟着视频走。
      if (audio) {
        audio->tick(std::ref(*audio_tap));
        if (!audio_tap->ok) {
          error = audio_tap->error;
          failed.store(true, std::memory_order_relaxed);
          return;
        }
      }

      if (changed) {
        last_content_id = f.image->content_id;
        have_last = true;
      }
      frames.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

/// 把 RTSP 的实时计数灌进统计结构的每一路。
///
/// 单独抽出来是因为有三处在填（每秒报表、正常收尾、以及采集循环里那条
/// "编码线程挂了"的出口）。复制三份的话，加一个字段必然漏掉其中一份 ——
/// 而漏掉的那份表现为"某一路上这个数永远是 0"，看上去像功能没生效。
void fill_rtsp_stats(SessionStats& s, RtspServer const& rtsp, std::size_t base,
                     Pipeline const& pipeline) {
  // `s.rtsp_streams` 装的是**本通道自己那几路**（下标 0/1），而服务器上的下标
  // 是 `base + i`。这两个差一个 `base` —— 从前它们是同一个数（一台服务器只有
  // 一个通道），所以 `base` 是这一轮多通道带进来的唯一一处新算术。
  for (std::size_t i = 0; i < s.rtsp_streams.size(); ++i) {
    SessionStats::RtspStreamStats& st = s.rtsp_streams[i];
    std::size_t const idx = base + i;
    st.clients = rtsp.client_count(idx);
    st.nalu_dropped = rtsp.nalu_dropped(idx);
    st.max_nalu_bytes = rtsp.max_nalu_bytes(idx);
  }
  // 包数/字节**从 s 本身取**，不从 pipeline 的原子量取：顶层 packets/bytes 是
  // 同一个数的权威版本，而且在收尾路径上它已经加过冲刷的那些包。从 pipeline 取
  // 会让逐路统计比顶层少掉冲刷的那几帧 —— 两个数出现在同一份报表里却对不上，
  // 是最容易让人怀疑统计整体不可信的那种不一致。
  if (!s.rtsp_streams.empty()) {
    s.rtsp_streams[0].packets = s.packets;
    s.rtsp_streams[0].bytes = s.bytes;
  }
  if (s.rtsp_streams.size() > 1) {
    s.rtsp_streams[1].packets =
        pipeline.sub_packets.load(std::memory_order_relaxed);
    s.rtsp_streams[1].bytes = pipeline.sub_bytes.load(std::memory_order_relaxed);
  }
}

/// 把音轨的实时状态灌进统计。
///
/// 和 `fill_rtsp_stats` 是同一个理由单独抽出来：填的地方有四处（每秒报表、
/// 正常收尾、以及编码线程挂了/采集出错那两条出口），复制四份必然漏掉其中一份，
/// 而漏掉的那份表现为"音轨的时长永远是 0"。
void fill_audio_stats(SessionStats& s, AudioTrack const& audio,
                      Pipeline const& pipeline) {
  // 编码器挂了要报 error。**混音器的状态答不了这个问题** —— 它那边一切正常
  // （设备在出样本），坏的是编码这一步。
  s.audio_state = audio.encode_failed() ? "error" : audio.mixer().state_text();
  AudioLevels const lv = audio.mixer().levels();
  s.audio_level_l = lv.peak_l;
  s.audio_level_r = lv.peak_r;
  s.audio_frames = audio.frames_out();
  s.audio_packets = pipeline.audio_packets.load(std::memory_order_relaxed);
  s.audio_zero_filled = audio.mixer().zero_filled_frames();
  s.audio_sample_rate = audio.encoder().sample_rate();
}

}  // namespace

// --- 配置 ↔ 会话选项：成对的两个方向 ---------------------------------------
//
// 这两张字段表**必须一致**（多一项少一项都会让某个字段的重载行为不对），
// 所以刻意挨着放。上一处实现见 main.cpp 开机时的命令行解析：每一处覆盖都是
// "以 cfg 为默认值再写回 opt"，于是开机之后 cfg 是"文件里的值 + 命令行"、
// opt 是"真正生效的值" —— 这两个函数就是在那两个世界之间搭桥。
//
// 什么时候各自被调用、以及为什么不在开机路径上也用 `apply_config_to_options`
// 的原因是同一件事：**开机要走命令行，重载不能再走一遍命令行**。重载时若把
// `--fps 60` 重新盖上去，用户在网页上选的「流畅（20 fps）」就会被无声地推翻，
// 而 `pending` 会因为 `running_cfg = cfg` 而被清空、界面上显示"已生效"。
// 好在开机的"生效值回写"已经把命令行折算进 cfg 了，所以重载只读 cfg 就是对的。

void sync_options_to_config(SessionOptions const& o, AppConfig& c) {
  // 只回写**桌面**那一种来源的下标。摄像头通道的 `--camera` 不回写：
  // `channels.items[].camera` 才是它的权威，而这个函数管的是通道 1 的
  // `capture.*` —— 把摄像头选择塞进 `capture.display_index` 会让
  // `GET /api/config` 报出一个"显示器序号"，而用户明明用的是摄像头。
  if (o.source.kind == "display") {
    c.capture.display_index = static_cast<int>(o.source.display_index);
  }
  c.capture.fps = o.fps;
  c.capture.bitrate_kbps = o.bitrate_kbps;
  c.capture.gop_seconds = o.gop_seconds;
  c.capture.threads = o.threads;
  c.capture.preset = o.preset;
  c.capture.tune = o.tune;
  c.capture.profile = o.profile;
  c.capture.max_height = o.max_height;
  // 水印与自适应整段回写：它们**只能**从配置来（外加几个开关），所以
  // `--no-osd` 之后 `GET /api/config` 必须显示 `enabled: false` —— 否则网页上
  // 那个勾还亮着，而画面上没有水印，看起来像水印坏了。
  c.osd = o.osd;
  c.adaptive = o.adaptive;
  // 音频同理，而且比水印更需要：`--no-audio` 之后 `GET /api/config` 必须显示
  // `enabled: false`。不回写的话网页上那个总开关还亮着、而 SDP 里没有音频，
  // 看起来像"音频开了但没生效"。
  c.audio = o.audio;
  c.rtsp.enabled = o.rtsp.enabled;

  // 端口和路径**只在 RTSP 真开着的时候回写**。关着的时候 `o.rtsp` 里装的是
  // RtspOptions 的默认值（554 / 空 streams），回写就等于把用户配好但暂时关掉的
  // 端口/路径抹成默认 —— 下次开起来时他会发现端口自己变了。
  if (o.rtsp.enabled && !o.rtsp.streams.empty()) {
    c.rtsp.port = o.rtsp.port;
    // 存回带斜杠的形式（`/live`）：配置文件和 NVR 界面里这样写更自然，而
    // 读的时候带不带斜杠都认。`o.rtsp.streams[0].path` 里是没有斜杠的那份。
    c.rtsp.path = "/" + o.rtsp.streams[0].path;
    c.rtsp.sub_enabled = o.sub_enabled;
    c.rtsp.sub_height = o.sub_height;
    c.rtsp.sub_bitrate_kbps = o.sub_bitrate_kbps;
    // 子码流路径**只在它真开着的时候回写**：关着时 `o.rtsp.streams` 只有一路，
    // 从这里取第二路会取到不存在的东西。
    if (o.sub_enabled && o.rtsp.streams.size() > 1) {
      c.rtsp.sub_path = "/" + o.rtsp.streams[1].path;
    }
  }
}

namespace {

/// 去掉开头的斜杠。和 main.cpp 开机时那个 `strip_slash` 是同一件事：
/// 命令行上写 `live`、配置文件和 NVR 界面里写 `/live`，是同一个东西。
std::string strip_leading_slash(std::string s) {
  while (!s.empty() && s.front() == '/') s.erase(0, 1);
  return s;
}

}  // namespace

void apply_config_to_options(AppConfig const& c, SessionOptions& o) {
  // `kind` **原样留着**，不在这里重算：它是由"这一路是怎么来的"决定的
  // （命令行 `--camera`、或者 `channels.items[].source`），而那两个地方都在
  // 这个函数的外面。在这里猜一个回去的话，一次重载就会把用户选的摄像头
  // 悄悄换回桌面 —— 而 `sync_options_to_config` 那侧只看 display 分支，
  // 于是下一次"生效值回写"也不会纠正它。
  // 直接赋值就好：`capture.display_index` 本来就是 `unsigned`，那条
  // "< 0 就取 0" 的夹取写在这里是**恒假**的（编译器的 -Wtype-limits 会当场
  // 指出来），而它读起来像在防一个负数 —— 那种"看起来在防一个不存在的
  // 情况"的分支比没有更糟：它会让下一个读代码的人以为这里有符号约定。
  o.source.display_index = c.capture.display_index;
  o.fps = c.capture.fps;
  o.bitrate_kbps = c.capture.bitrate_kbps;
  o.gop_seconds = c.capture.gop_seconds;
  o.threads = c.capture.threads;
  o.preset = c.capture.preset;
  o.tune = c.capture.tune;
  o.profile = c.capture.profile;
  o.max_height = c.capture.max_height;
  o.osd = c.osd;
  o.adaptive = c.adaptive;
  // 音频**无条件整段读**，不像下面 RTSP 那样分"开着才读"。音频没有"只在开着
  // 时才有意义的字段" —— 每一个字段（两个源、设备、码率、增益）在关着的时候
  // 也都是有意义的，因为用户就是在关着的状态下把它们配好的。
  o.audio = c.audio;
  o.rtsp.enabled = c.rtsp.enabled;

  if (!o.rtsp.enabled) {
    // **不碰 sub_* 和 streams**，理由和 sync 那侧的"关着不回写"完全对称：
    // 关着的时候 opt 里那几项是上次生效过的值，抹掉它们就等于"关一次 RTSP
    // 顺手把端口和路径清了"。
    return;
  }

  o.rtsp.port = c.rtsp.port;
  o.sub_enabled = c.rtsp.sub_enabled;
  o.sub_height = c.rtsp.sub_height;
  o.sub_bitrate_kbps = c.rtsp.sub_bitrate_kbps;

  // 队列深度**从现有选项里继承**，不从配置重算。
  //
  // `rtsp.queue` 不是配置字段（它由 `--rtsp-queue` 给，默认 `fps * 3`），所以
  // 重载时无从查起。继承是这里唯一说得通的选择：显式给过 `--rtsp-queue 500`
  // 的人不该因为点了一次重载就回到 75。
  //
  // streams 为空（第一次调、或者从没开过 RTSP）时退回默认公式。
  std::size_t const queue_capacity =
      o.rtsp.streams.empty()
          ? static_cast<std::size_t>(o.fps > 0 ? o.fps * 3 : 90)
          : o.rtsp.streams[0].queue_capacity;

  // 路径为空就**保留现有的那一条**，不报错也不退回 "live"。
  // 空路径在开机时是人话报错（`--rtsp-path 写 live`），但重载时它是
  // "用户存了一份空路径"—— 而配置层不拦这个。这时把流挂到一个凭空的
  // "live" 上比什么都不做更糟：NVR 那边地址会变，而没有任何一处解释过。
  std::string main_path = strip_leading_slash(c.rtsp.path);
  if (main_path.empty() && !o.rtsp.streams.empty()) {
    main_path = o.rtsp.streams[0].path;
  }

  // 两路同名是开机时挡下来的（Live555 让哪一个生效都是"一条能播的流"，
  // 分不出主次）。重载这条路上**退回单路**：能继续推流、而且现象是
  // "子码流没了"，比两条流悄悄同地址要好查得多。
  std::string sub_path = strip_leading_slash(c.rtsp.sub_path);
  bool const sub_ok =
      o.sub_enabled && !sub_path.empty() && sub_path != main_path;

  o.rtsp.streams.clear();
  // 两路的 `fps` 都是主码流的帧率（子码流不独立定帧，它吃的是同一拍）。
  unsigned const stream_fps = static_cast<unsigned>(o.fps > 0 ? o.fps : 25);
  RtspStreamOptions main_stream;
  main_stream.path = main_path;
  main_stream.fps = stream_fps;
  main_stream.bitrate_kbps = o.bitrate_kbps;
  main_stream.queue_capacity = queue_capacity;
  o.rtsp.streams.push_back(std::move(main_stream));

  if (sub_ok) {
    RtspStreamOptions sub;
    sub.path = std::move(sub_path);
    sub.fps = stream_fps;
    sub.bitrate_kbps = o.sub_bitrate_kbps;
    // 队列深度按 NALU 计，两路每帧都是 1 个 VCL NALU —— "3 秒的量"在两路
    // 就是同一个数，照搬即可（按**字节**等比缩小反而让子码流更早丢包）。
    sub.queue_capacity = queue_capacity;
    o.rtsp.streams.push_back(std::move(sub));
  } else if (o.sub_enabled) {
    o.sub_enabled = false;
    D2O_WARN("重载：子码流路径（%s）为空或与主码流（%s）相同，这一趟先只开"
             "主码流。两路挂同一个地址时 Live555 只会让其中一条生效，而现象"
             "是一条能正常播的流 —— 分不出它是主还是子。",
             c.rtsp.sub_path.c_str(), main_path.c_str());
  }
}

SessionOptions options_for_channel(ChannelPlan const& plan,
                                   SessionOptions const& base,
                                   std::string const& record_path) {
  SessionOptions o = base;

  // 画面来源。**三个字段一起给**：`kind` 决定另两个里哪个有用，而配置项里
  // 那个"用不上的"字段是留着给用户切回来用的（见 ChannelConfig 的说明），
  // 所以这里照抄、不猜。
  o.source.kind = plan.source;
  o.source.display_index = plan.display_index;
  o.source.camera = plan.camera;

  // 下面这几个就是 `channels.items[]` 里**有**对应字段的全部。
  o.fps = plan.fps;
  o.bitrate_kbps = plan.bitrate_kbps;
  o.max_height = plan.max_height;
  o.sub_enabled = plan.sub_enabled;
  o.sub_height = plan.sub_height;
  o.sub_bitrate_kbps = plan.sub_bitrate_kbps;

  // **只走通道 1**（见头文件那段）。这是这一份里唯一一个故意不继承的字段。
  o.audio.enabled = false;

  o.output_path = record_path;

  // 队列深度**继承通道 1 的**。它不是配置字段（`--rtsp-queue` 给，默认
  // `fps * 3`），而"够装几个 GOP"在每一路上是同一个道理；按这一路的 fps 重算
  // 反而会让通道 2 的队列比通道 1 浅，而没有任何一处解释过为什么。
  std::size_t const queue_capacity =
      base.rtsp.streams.empty()
          ? static_cast<std::size_t>(o.fps > 0 ? o.fps * 3 : 90)
          : base.rtsp.streams[0].queue_capacity;

  auto strip_slash = [](std::string s) {
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    return s;
  };
  std::string main_path = strip_slash(plan.rtsp_path);
  if (main_path.empty()) main_path = fmt("ch%u", plan.index);
  std::string sub_path = strip_slash(plan.rtsp_sub_path);

  unsigned const stream_fps = static_cast<unsigned>(o.fps > 0 ? o.fps : 25);

  o.rtsp.streams.clear();
  RtspStreamOptions main_stream;
  main_stream.path = main_path;
  main_stream.fps = stream_fps;
  main_stream.bitrate_kbps = o.bitrate_kbps;
  main_stream.queue_capacity = queue_capacity;
  o.rtsp.streams.push_back(std::move(main_stream));

  bool const sub_ok =
      o.sub_enabled && !sub_path.empty() && sub_path != main_path;
  if (sub_ok) {
    RtspStreamOptions sub;
    sub.path = std::move(sub_path);
    sub.fps = stream_fps;
    sub.bitrate_kbps = o.sub_bitrate_kbps;
    // 队列按 NALU 计，两路每帧都是 1 个 VCL NALU —— "3 秒的量"在同一个数上，
    // 照搬即可（按字节等比缩小反而让子码流更早丢包）。与 `apply_config_to_options`
    // 里那条一字不差。
    sub.queue_capacity = queue_capacity;
    o.rtsp.streams.push_back(std::move(sub));
  } else if (o.sub_enabled) {
    o.sub_enabled = false;
    // 和重载那条路同一个现象、同一句话：两路挂同一个地址时 Live555 只会让
    // 其中一条生效，而它是一条能正常播的流 —— 分不出是主还是子。
    D2O_WARN("通道 %u 的子码流路径（%s）为空或与主码流（%s）相同，"
             "这一路先只开主码流。",
             plan.index, plan.rtsp_sub_path.c_str(),
             plan.rtsp_path.c_str());
  }

  // **`rtsp.enabled` / `rtsp.port` 原样留着**（继承自 `base`）。它们描述的是
  // "这一台机器要不要开 RTSP、开在哪个端口"，服务器只有一台，由 `cmd_run`
  // 决定；通道 2 起在这里改它只会让某一路的记账和实际不符。
  return o;
}

std::string SessionStats::summary() const {
  std::string s = fmt(
      "时长 %.1fs，产出 %llu 包（%llu 关键帧，%.1f fps），%.2f MB，"
      "画面变化 %llu 次 / 共 %llu 帧，队列丢弃 %llu，缓冲池耗尽 %llu，"
      "会话重建 %llu 次",
      elapsed_s, static_cast<unsigned long long>(packets),
      static_cast<unsigned long long>(keyframes), effective_fps,
      static_cast<double>(bytes) / (1024.0 * 1024.0),
      static_cast<unsigned long long>(content_frames),
      static_cast<unsigned long long>(ticks),
      static_cast<unsigned long long>(queue_dropped),
      static_cast<unsigned long long>(pool_exhausted),
      static_cast<unsigned long long>(access_lost));
  s += fmt("\n  鼠标合成 %llu 帧（其中 %llu 帧只因为鼠标动了才产出）",
           static_cast<unsigned long long>(cursor_frames),
           static_cast<unsigned long long>(pointer_only_frames));
  // 水印那一行**只在开着的时候打**：没开时它恒为 0，打出来是纯噪音。
  // 而它在开着的时候必须打 —— 这个数是"静止桌面上水印的钟还在走"唯一的
  // 证据（那条路径产出的帧不计入"画面变化"），没有它就只能靠肉眼去视频里
  // 找秒数，而那个判据既不能自动化也不可靠。
  if (osd_refresh_frames > 0) {
    s += fmt("\n  水印刷新 %llu 帧（桌面没动、只有水印文字变了）",
             static_cast<unsigned long long>(osd_refresh_frames));
  }
  // 水印热生效那两行**各自只在真的发生过时打**，而且它们答的是两个不同的问题：
  //
  //   水印热生效 N 次 —— "有人改了配置、采集侧真的换上了"（与桌面动没动无关）
  //   水印重建   N 帧 —— 其中有多少帧是**因为桌面静止**才需要重新合成的
  //
  // 只有后者的话，在一台一直在动的桌面上它恒为 0，读起来像"热生效没发生"
  // —— 而热生效其实次次都发生了（下一帧本来就带新水印）。
  if (osd_hot_applied > 0) {
    s += fmt("\n  水印热生效 %llu 次（改完下一帧就变，没有重建管线）",
             static_cast<unsigned long long>(osd_hot_applied));
  }
  if (osd_rebuilt_frames > 0) {
    s += fmt("\n  水印重建 %llu 帧（其中桌面静止、需要从无水印的桌面图重合成）",
             static_cast<unsigned long long>(osd_rebuilt_frames));
  }
  // 子码流这里只报尺寸，码率交给下面逐路那一段 —— 两处都报的话，同一份报表里
  // 会出现两个"子码流码率"，而它们的口径（是否含冲刷、按哪路算）一旦不一致，
  // 读的人就得先决定信哪个。
  if (sub_width > 0) {
    s += fmt("\n  子码流尺寸 %dx%d", sub_width, sub_height);
  }
  // 音频那一行**只在开着的时候打**（`off` 时下面每个数都是 0，纯噪音）。
  // 这一行存在的理由是**音轨时长**：它和视频是同一串拍数算出来的（见
  // `AudioTrack` 的文件头），所以两者天然该一样长；而"包数 × 1024"看不出
  // 这件事 —— AAC 按 1024 样本攒包，末尾那一小段会多算/少算到 21 ms。
  if (audio_state != "off") {
    double const a_sec =
        audio_sample_rate > 0
            ? static_cast<double>(audio_frames) / static_cast<double>(audio_sample_rate)
            : 0.0;
    s += fmt("\n  音频 %s  音轨 %.3fs（%llu 样本帧 @ %d Hz），%llu 包，"
             "补零 %llu 帧，电平 L %.3f / R %.3f",
             audio_state.c_str(), a_sec,
             static_cast<unsigned long long>(audio_frames), audio_sample_rate,
             static_cast<unsigned long long>(audio_packets),
             static_cast<unsigned long long>(audio_zero_filled),
             static_cast<double>(audio_level_l),
             static_cast<double>(audio_level_r));
  }
  // 逐路列出来。只报一路的话，"哪一路在丢"这个问题就没法回答了，而两路的
  // 码率差着近一个数量级，合起来的平均数什么也说明不了。
  for (std::size_t i = 0; i < rtsp_streams.size(); ++i) {
    RtspStreamStats const& st = rtsp_streams[i];
    double const kbps = elapsed_s > 0
                            ? static_cast<double>(st.bytes) * 8.0 / elapsed_s / 1000.0
                            : 0.0;
    s += fmt("\n  推流[%zu]  %s  %.0f kbps，观众 %u，NALU 丢弃 %llu，"
             "最大 NALU %zu 字节",
             i, st.url.c_str(), kbps, st.clients,
             static_cast<unsigned long long>(st.nalu_dropped),
             st.max_nalu_bytes);
  }
  return s;
}

CaptureSession::~CaptureSession() = default;

std::shared_ptr<BgraFrame const> CaptureSession::snapshot() const {
  std::lock_guard<std::mutex> lk(snapshot_mu_);
  return snapshot_;
}

CaptureSession::RunResult CaptureSession::run(SessionOptions& opt,
                                              std::string& err) {
  stop_.store(false, std::memory_order_relaxed);
  // 重配请求**在这里就清掉**，而不是留到收尾时。理由不是"反正是同一件事"，
  // 而是：调用方（main.cpp 的分段循环）**每一趟都用当前配置重算一遍选项**，
  // 所以"没有管线在跑"的时候到达的请求本来就是空操作 —— 即将开始的那一趟
  // 已经带上新配置了。留到收尾清的话，它反而会在下一趟开头把刚从配置算好的
  // 参数再重开一次（多一个空段）。
  reconfigure_.store(false, std::memory_order_relaxed);
  stats_ = SessionStats{};

  // 没有任何下游时也算"有事可做"的情况只有一种：Web 服务在跑。
  // 那时编码链照常转，数据被丢弃，但每秒的统计要喂给 /ws/stats 和 /api/status
  // —— 否则 Web 页面上全是 0，看起来像坏了。
  // 谁都没要的时候仍然拒绝：那种调用一定是命令行写错了，早报比空转强。
  // 有没有下游现在看的是**共享服务器给没给**，不是 `opt.rtsp.enabled`。
  // 那两个从本轮起是两件事：`rtsp.enabled` 是配置层的"用户想不想要 RTSP"，
  // 而"这一路能不能推"取决于 `cmd_run` 有没有把这一路排进服务器（比如
  // 通道数超过上限、或者那一台的端口被占）。
  bool const rtsp_on = opt.rtsp_server != nullptr;
  if (opt.output_path.empty() && !rtsp_on && !stats_hook_) {
    err = "既没有指定输出文件也没有开推流，那就没有任何事可做。"
          "给个 --record <文件> 或者 --rtsp。";
    return RunResult::Failed;
  }

#ifdef _WIN32
  ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
  struct HandlerGuard {
    ~HandlerGuard() { ::SetConsoleCtrlHandler(console_ctrl_handler, FALSE); }
  } handler_guard;
#endif

  // --- 1+2. 建源（挑哪一个 + 建会话）--------------------------------------
  //
  // **"挑哪一个"整个搬进了 `open()`。** 桌面那边要枚举输出、探一次可捕获性；
  // 摄像头那边要按名字找设备、协商格式。这两套都是"这个源到底是哪一个"，
  // 而 `run()` 认识的只是四个动词（open / acquire / reemit / close）——
  // 于是下一路来源加进来时，这个文件一行都不用改。
  std::unique_ptr<IFrameSource> source = make_frame_source(opt.source);
  if (!source->open(err)) return RunResult::Failed;
  D2O_INFO("画面来源：%s", source->describe().c_str());

  // 指针那套诊断**只有桌面源有**（见 `ICursorDiagnostics` 的说明）。取一次，
  // 取不到就是"这一路根本没有指针这回事"—— 那是结构上的答案，不是读出来的 0。
  ICursorDiagnostics* cursor_diag = dynamic_cast<ICursorDiagnostics*>(source.get());

  int const w = source->width();
  int const h = source->height();
  if (w <= 0 || h <= 0) {
    err = fmt("拿到的桌面尺寸不合法：%dx%d", w, h);
    return RunResult::Failed;
  }
  if ((w % 2) != 0 || (h % 2) != 0) {
    // H.264 的 4:2:0 色度是半分辨率，宽高必须是偶数。外接奇异分辨率的显示器
    // 会碰到（历史上有一些 1366x768 之外的非标准模式）。
    err = fmt("桌面尺寸 %dx%d 不是偶数，H.264 编不了。"
              "改一下显示分辨率再录（Phase 6 会加裁剪处理）", w, h);
    return RunResult::Failed;
  }

  // --- 2b. 编码尺寸（主码流可能比桌面小）-----------------------------------
  //
  // 采集永远是**桌面原生尺寸**：鼠标指针的坐标是 DXGI 按原生尺寸给的，
  // 采集时就缩放的话指针位置会整体偏掉，而症状是"指针和它点的东西差一点"，
  // 很容易被当成驱动问题。缩放交给编码器 —— 它本来就支持 src ≠ dst
  // （子码流走的就是这条路），不需要任何新代码。
  //
  // `max_height` 只写**上限**：桌面本来就比它小就不放大（放大既费 CPU 又不会
  // 多出信息）。
  int main_w = w;
  int main_h = h;
  if (opt.max_height > 0 && opt.max_height < h) {
    long const want = (static_cast<long>(w) * opt.max_height + h / 2) / h;
    main_h = opt.max_height;
    main_w = static_cast<int>(want);
    // **缩放之后要重做一次偶数校验。** 桌面尺寸那一关（上面）只保证了
    // w/h 是偶数，按比例缩下来完全可能是奇数：2560x1600 缩到高 900 得
    // 1440x900（没事），但缩到 1080 得 1728x1080，缩到 720 得 1152x720，
    // 而 1680x1050 → 高 900 得 1440x900 没事、高 700 得 1120x700 也没事，
    // 可 2880x1620 → 高 1080 得 1920x1080 又是没事 —— 只要分子是奇数就会出
    // 奇数。而 x264 对奇数边长是**直接拒绝**，报的还是 "width not divisible
    // by 2"，看不出是缩放引入的。
    main_w = (main_w + 1) / 2 * 2;
    main_h = (main_h + 1) / 2 * 2;
    // 取偶可能把高度顶到上限之上（比如上限 901 → 902），夹回来。
    // 上限本身就是奇数的话就往下取，反正只差一像素。
    if (main_h > opt.max_height && opt.max_height > 1) {
      main_h = (opt.max_height / 2) * 2;
    }
    if (main_w > w || main_h > h) {
      // 夹完之后反而比桌面大了 —— 某些极端比例下会碰上。那就干脆不缩。
      main_w = w;
      main_h = h;
    }
    if (main_w != w || main_h != h) {
      D2O_INFO("主码流按画质档位缩放到 %dx%d（桌面原生 %dx%d）", main_w, main_h,
               w, h);
    }
  }
  if (main_w <= 0 || main_h <= 0 || (main_w % 2) != 0 || (main_h % 2) != 0) {
    err = fmt("算出来的主码流尺寸 %dx%d 不合法（桌面 %dx%d，max_height %d）",
              main_w, main_h, w, h, opt.max_height);
    return RunResult::Failed;
  }

  // --- 7c. 自适应降载（控制器） -------------------------------------------
  //
  // ⚠️ **控制器必须建在下面那"立刻推一帧"之前**，这不是摆放习惯问题。
  //
  // 那一帧是 `/api/status` 与 `/ws/stats` 在会话头一秒里唯一能看到的东西，
  // 而 `perf_duty` / `perf_state` 的**初值**是 1 / "ok"（见 `SessionStats`），
  // 看上去"一切正常"，却没带上对应的 `perf_target_fps`（初值 0）。于是那一秒
  // 里三个字段互相矛盾：duty = 1 而 target = 0，可按定义 target 就是 fps/duty。
  //
  // 这不是纸面上的整齐：`tests\web_contract.py` 的 [5] 段正是拿
  // `perf_target_fps == fps // perf_duty` 对账的（这两个数由两段不同的代码
  // 算出来 —— 一个来自自适应控制器、一个来自配置 —— 对不上就说明有一段没
  // 接上）。探针跑得比第一个整秒报表还快时就会读到那组初值，然后报红。
  // 2026-10-08 安装包验收红的就是这一条（`perf_target_fps = 0`），而当时
  // 报出来的话只有"退出码 = 1"，查了很久。
  //
  // 修法是**在唯一的构造点上把这三个字段一次填齐**，而不是让读的人自己去
  // 分辨"这是初值还是真值" —— 后者是每一处读取点都要重复一遍的负担，还总会
  // 有人漏掉。
  AdaptiveController adaptive;
  adaptive.configure(opt.adaptive, opt.fps);

  // 尺寸要在**采集循环开始之前**就填好：ONVIF 的 GetProfiles 可能在这之前
  // 就被 NVR 调到了（Web 服务比采集先起），而那一次回答里的分辨率得是真的。
  stats_.video_width = main_w;
  stats_.video_height = main_h;
  // 自适应那三个字段同理，而且必须是**一组**：只填 target 不填另两个，
  // 又能拼出一帧自相矛盾的读数。
  stats_.perf_duty = adaptive.duty();
  stats_.perf_target_fps = adaptive.target_fps();
  stats_.perf_state = perf_state_name(adaptive.state());
  // 立刻推一帧出去，别等第一个整秒。这一帧里除尺寸与上面这组状态外全是 0，
  // 那是**真实的** —— 此刻确实一帧都没编。让它等一秒的代价是：NVR 恰好在这
  // 一秒里问 GetProfiles，就会拿到一个默认的 1920x1080，而那是编造的。
  if (stats_hook_) stats_hook_(stats_);

  // --- 3. 编码器 ----------------------------------------------------------
  EncoderConfig ec;
  ec.width = main_w;
  ec.height = main_h;
  ec.fps = opt.fps;
  ec.bitrate_kbps = opt.bitrate_kbps;
  ec.gop_seconds = opt.gop_seconds;
  ec.threads = opt.threads;
  ec.preset = opt.preset;
  ec.tune = opt.tune;
  ec.profile = opt.profile;
  // SPS/PPS 进 extradata：MP4 的 avcC 要用，RTSP 的 SDP
  // （sprop-parameter-sets）也要用。两个下游都靠它。
  ec.global_header = true;

  X264Encoder encoder;
  if (!encoder.open(ec, err)) return RunResult::Failed;

  // --- 3b. 子码流编码器 ---------------------------------------------------
  //
  // 只有开了 RTSP 才建。子码流存在的唯一理由是给 NVR 的多画面网格用，而那条
  // 路走的是 RTSP —— 只录文件或者只开 Web 的时候多编一路纯粹的浪费。
  //
  // **尺寸按高度给、宽度按宽高比推**：NVR 界面是按高度分档的（D1/720p/360p），
  // 而写死宽度在 16:10、21:9 这些比例下会把画面拉变形。
  X264Encoder sub_encoder;
  // 子码流要同时满足三件事：用户想要（`sub_enabled`）、有服务器可推（`rtsp_on`）、
  // 而且 `cmd_run` **真的给这一路排了第二个槽**（`rtsp_stream_slots >= 2`）。
  // 第三件事不能省 —— 少了它就是"往下一个通道的主码流上推子码流"，两路流各自
  // 都正常，只有画面是错的。见 `SessionOptions::rtsp_stream_slots`。
  bool sub_on = opt.sub_enabled && rtsp_on && opt.rtsp_stream_slots >= 2;
  int sub_w = 0;
  int sub_h = 0;
  if (sub_on) {
    sub_h = opt.sub_height > 0 ? opt.sub_height : 360;
    // 取偶：YUV420P 的色度平面是 2x2 子采样，奇数边长 x264 直接拒绝
    // （报 "width not divisible by 2"）。四舍五入到最近的偶数。
    sub_h = (sub_h + 1) / 2 * 2;
    long const want_w = (static_cast<long>(main_w) * sub_h + main_h / 2) / main_h;
    sub_w = static_cast<int>((want_w + 1) / 2 * 2);

    if (sub_w > 0 && sub_h > 0 && (sub_w < main_w || sub_h < main_h)) {
      EncoderConfig sc = ec;
      sc.width = sub_w;
      sc.height = sub_h;
      sc.bitrate_kbps = opt.sub_bitrate_kbps;
      // 子码流的线程数**单独封顶**：它的像素只有主码流的几十分之一，按核数
      // 开线程纯属浪费，还会和主码流抢 CPU。
      sc.threads = std::min(opt.threads > 0 ? opt.threads : 6, 2);
      if (!sub_encoder.open(sc, err)) return RunResult::Failed;
      D2O_INFO("子码流已启用：%dx%d（%.0f kbps）", sub_w, sub_h,
               static_cast<double>(opt.sub_bitrate_kbps));
    } else {
      // 尺寸没比主码流小 —— 再编一遍同样的画面没有任何意义。说清楚为什么
      // 没开，而不是安静地少一路（那会被当成"子码流坏了"）。
      D2O_WARN("子码流高度 %d 在主码流 %dx%d 下推不出更小的尺寸，已跳过子码流",
               opt.sub_height, main_w, main_h);
      sub_on = false;
    }
  }
  stats_.sub_width = sub_on ? sub_w : 0;
  stats_.sub_height = sub_on ? sub_h : 0;

  // --- 3c. 音频 -----------------------------------------------------------
  //
  // 位置是硬的：**必须在 MP4 与 RTSP 之前**。两边都要用到编码器出来的东西
  // —— MP4 那边的 `codecpar->extradata`（5 字节，esds 用）和 RTSP 那边的
  // `asc_hex()`（2 字节，SDP 的 `config=` 用），而它们只有编码器真的开着才有。
  //
  // 起不来**不是**致命错误：按纯视频继续录。麦克风被拔掉不该让摄像机起不来 ——
  // 而这一趟的报表和状态页上会写着音频是 `off`，用户看得出来。
  AudioTrack audio_track;
  bool audio_on = false;
  if (opt.audio.enabled) {
    AudioTrackOptions aopt;
    aopt.fps = opt.fps;
    aopt.bitrate_kbps = opt.audio.bitrate_kbps;
    aopt.mixer.system.enabled = opt.audio.system;
    aopt.mixer.system.gain = opt.audio.system_gain;
    aopt.mixer.mic.enabled = opt.audio.microphone;
    aopt.mixer.mic.device_id = opt.audio.device;
    aopt.mixer.mic.gain = opt.audio.mic_gain;
    std::string aerr;
    if (audio_track.start(aopt, aerr)) {
      audio_on = true;
    } else {
      D2O_WARN("音频没起来，这一趟按纯视频录：%s", aerr.c_str());
    }
  }

  // --- 4. 下游 ------------------------------------------------------------
  Mp4Writer writer;
  Mp4Consumer mp4;
  if (!opt.output_path.empty()) {
    // 音频关着/没起来时传 nullptr ⇒ **只有一条视频轨**，而不是"有一条静音
    // 音轨"（陷阱 78）—— 后者会让播放器和 NVR 都给文件画上音频图标。
    if (!writer.open(opt.output_path, encoder.codec_context(),
                     audio_on ? audio_track.encoder().codec_context() : nullptr,
                     err)) {
      return RunResult::Failed;
    }
    mp4.writer = &writer;
    mp4.stream_index = 0;  // 视频
  }

  RtspConsumer rtsp_consumer;
  RtspConsumer rtsp_sub_consumer;
  RtspAudioConsumer rtsp_audio_consumer;
  if (rtsp_on) {
    // **本阶段最大的重构：服务器不在这里起了。**
    //
    // 从前这一格是 `RtspServer rtsp; rtsp.start(rtsp_opt, extras, err);`，
    // 外加一个"退出时把服务器收掉"的守卫。多通道下那不成立了 —— 一台服务器
    // 要服务 N 个通道，而通道是各自异步起来的（某个通道的屏没插、某个摄像头
    // 被占用，都不该让别的通道的 RTSP 起不来）。所以服务器归 `cmd_run` 持有，
    // 这里只是**往共享的流上推**。
    //
    // 会话这一侧因此多出来的责任只有一件：**把参数集填进共享的槽位**。
    // 填的时机是这里（编码器刚打开），而读它的是首次 DESCRIBE —— 见
    // `RtspServer::set_stream_extradata()` 的说明。
    //
    // ⚠️ **子码流的 SPS/PPS 是不同的**（分辨率不同），所以不能拿主码流那份
    // 去喂子码流 —— 那样 SDP 里报的分辨率是对的、解出来的画面却是错的。
    {
      std::string perr;
      if (!opt.rtsp_server->set_stream_extradata(opt.rtsp_stream_base,
                                                 encoder.extradata(), perr)) {
        // 填不上**不是**致命错误：`push_annex_b` 会在每个 IDR 前补发带内参数集，
        // 所以最坏情况是"第一个关键帧之前花屏"，不是黑屏到底。而这里若直接
        // 判失败，症状会变成"某个通道的编码器参数集有点特别，于是整台机器
        // 一个通道都不推流了"—— 代价和收益完全不成比例。
        D2O_WARN("主码流参数集没填上：%s（客户端要靠带内参数集才能出画）",
                 perr.c_str());
      }
      if (sub_on) {
        if (!opt.rtsp_server->set_stream_extradata(opt.rtsp_stream_base + 1,
                                                   sub_encoder.extradata(), perr)) {
          D2O_WARN("子码流参数集没填上：%s", perr.c_str());
        }
      }
    }

    rtsp_consumer.server = opt.rtsp_server;
    rtsp_consumer.stream = opt.rtsp_stream_base;
    if (sub_on) {
      rtsp_sub_consumer.server = opt.rtsp_server;
      rtsp_sub_consumer.stream = opt.rtsp_stream_base + 1;
    }
    // 音频**只跟通道 1**（见 README 的"已知取舍"）：它挂在通道 1 主码流的那个
    // ServerMediaSession 上，而那个会话是 `cmd_run` 在第 0 路上建的。别的通道
    // 想推音频也没有会话可挂 —— 推过去会被静默忽略（`push_audio_au` 的契约），
    // 所以这里必须挡住，否则"通道 2 录的视频看起来有音轨"会成为一个查不出的谜。
    if (audio_on && opt.rtsp_stream_base == 0) {
      rtsp_audio_consumer.server = opt.rtsp_server;

      // **拿真值核对一次 SDP 里那份预测值。** 服务器起在这一趟之前，那时 AAC
      // 编码器还不存在，`config=` 只能由 `AacEncoder::asc_hex_for()` 按
      // "48 kHz 立体声"这个约定算出来；现在编码器开了，手上有一份真的。
      //
      // 不一致的后果**不是**报错，而是：客户端连得上、ffprobe 报得出
      // "48000 Hz 立体声 AAC"，**解出来是噪音**。这种故障没有任何一处会响，
      // 所以必须在这里显式比一次。
      //
      // 比出来不一致**只说不改**（也改不了）：SDP 里那一份是在 Live555 调度
      // 线程上建 RTP sink 时读的，而这一句跑在采集线程上 —— 从这边去改它是
      // 一场数据竞争，为了收拾一个"本来就不该发生"的状态去引入一个真的 UB，
      // 不划算。这句话本身就是排查入口：日志里有它，等于知道了噪音从哪来。
      std::string const asc_real = audio_track.encoder().asc_hex();
      std::string const asc_sdp = opt.rtsp_server->audio_config_hex();
      if (!asc_real.empty() && !asc_sdp.empty() && asc_real != asc_sdp) {
        D2O_WARN("RTSP 的 SDP 里报的 AudioSpecificConfig 是预测的 %s，而编码器"
                 "实际给的是 %s —— 客户端会连得上、却解出噪音。"
                 "两者不一致说明这条链路上 48 kHz 立体声那个前提被改动了"
                 "（见 AacEncoder::asc_hex_for 的说明）。",
                 asc_sdp.c_str(), asc_real.c_str());
      }
    }

    // URL 要等 Live555 挑完本机 IP 才知道，所以在这里就取出来填进统计 ——
    // 采集还没开始，但端口和地址已经定了。
    //
    // `stats_.rtsp_streams` 装的是**本通道自己那两路**（下标 0/1），不是
    // 服务器上全部的路：这样通道 1 的 `/api/status` 和从前一字不差，而通道 2
    // 的读数也不会莫名其妙地带上通道 1 的数字。
    std::size_t const n = sub_on ? 2u : 1u;
    stats_.rtsp_streams.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      stats_.rtsp_streams[i].path = opt.rtsp.streams[i].path;
      stats_.rtsp_streams[i].url = opt.rtsp_server->url(opt.rtsp_stream_base + i);
      D2O_INFO("RTSP 第 %zu 路已就绪：%s", opt.rtsp_stream_base + i,
               stats_.rtsp_streams[i].url.c_str());
    }
  }

  // 浏览器预览。封装器**在这里就 init**，而不是等第一个客户端连上来：
  // extradata 只有编码器开着的时候才有，而初始化段要跟分片同线程按顺序发
  // （见 PreviewFeed::begin），在采集线程里现 init 就得处理"init 赶不上
  // 第一帧"的竞态。提前准备好就完全没有这个问题。
  PreviewFeed preview;
  PreviewConsumer preview_consumer;
  if (preview_hook_) {
    // 主动切分片的节奏：**每 ~250 ms 一刀**。
    //
    // 250 ms 这个数是从两头夹出来的，不是拍的：往下，它要明显小于"关键帧间隔"
    // 这个默认的产出节奏（均衡档 2 秒），否则切了跟没切一样；往上，它要小到
    // 播放器那 0.5 秒的目标缓冲能吃得住 —— 一次 append 送进来的东西超过
    // `TARGET_LATENCY`，播放头就只能往后让。`fps / 4` 在 25/30 fps 上分别是
    // 6/7 帧、240/233 ms，两边都落在区间里。
    //
    // fps 不为正时（配置坏了）退回 1：除以 0 会得到 0，而 0 在这个接口上是
    // "关掉这个功能"—— 一个坏配置**静默地把功能关掉**，比切得密一点难查得多。
    int const flush_every = opt.fps / 4 > 0 ? opt.fps / 4 : 1;
    // `fragment_on_flush` 与它同开关。**不是因为这个标志是必需的**（这一版
    // FFmpeg 里 `av_write_frame(fmt_, nullptr)` 本来就切，两个都量过、见
    // fmp4_muxer.h），而是因为它是为这件事提供的那条路，开着不花钱；而
    // "什么时候切"这件事只有一个旋钮，比两个各自能关的开关好推理。
    if (!preview.muxer.init(main_w, main_h, encoder.extradata(), opt.fps,
                            flush_every > 0)) {
      // 不是致命错误：录制、RTSP、ONVIF 都不受影响，坏的只有浏览器预览。
      D2O_WARN("fMP4 封装器初始化失败，浏览器预览不可用（录制与 RTSP 不受影响）。");
    } else {
      preview.fps = opt.fps;
      preview.flush_every = flush_every;
      preview.hook = preview_hook_;
      // 时间原点这一路**跟着预览走**：预览不封装的话，"媒体时间 0 点"根本不存在，
      // 报出去只会让前端以为有流可算。
      preview.t0_hook = preview_t0_hook_;
      preview_consumer.feed = &preview;
    }
  }

  // **顺序即语义**：只读的下游在前，会拿走包的下游在后。
  PacketTap tap;
  if (rtsp_on) tap.consumers.push_back(&rtsp_consumer);
  if (preview_consumer.feed) tap.consumers.push_back(&preview_consumer);
  if (!opt.output_path.empty()) tap.consumers.push_back(&mp4);

  // 子码流**只有 RTSP 一个下游**，所以它自己的 tap 里只有一个消费者。
  // 也正因如此它不会碰主码流的计数器 —— 两个 tap 各绑各的原子量。
  PacketTap sub_tap;
  if (sub_on) sub_tap.consumers.push_back(&rtsp_sub_consumer);

  // 音轨的下游。**顺序同上面那条规则**：RTSP（只读）在前、MP4（拿走包）在后。
  // 这里踩它的症状比视频那一路更隐蔽：包被 `av_interleaved_write_frame` 清空
  // 之后，后面那位推到 RTSP 的是一堆零长度的 AU —— NVR 那边"有音轨、没有声音"，
  // 而 SDP 完全正常、编码器那边也一切正常。
  //
  // `mp4_audio` 必须和 `audio_tap` 同作用域：tap 里存的是它的地址，而管线要到
  // 这个块结束之后才跑。放进 `if` 里就成了悬垂指针。
  Mp4Consumer mp4_audio;
  PacketTap audio_tap;
  if (audio_on) {
    if (rtsp_on) audio_tap.consumers.push_back(&rtsp_audio_consumer);
    if (!opt.output_path.empty()) {
      mp4_audio.writer = &writer;
      mp4_audio.stream_index = 1;  // 音频（视频那一路恒为 0）
      audio_tap.consumers.push_back(&mp4_audio);
    }
  }

  // --- 5. 起编码线程 ------------------------------------------------------
  auto pool = std::make_shared<FramePool>(w, h, opt.pool_size);
  BoundedQueue<CapturedFrame> queue(opt.queue_capacity);

  Pipeline pipeline;
  pipeline.queue = &queue;
  pipeline.encoder = &encoder;
  pipeline.tap = &tap;
  pipeline.preview = &preview;
  if (sub_on) {
    pipeline.sub_encoder = &sub_encoder;
    pipeline.sub_tap = &sub_tap;
    sub_tap.packets = &pipeline.sub_packets;
    sub_tap.bytes = &pipeline.sub_bytes;
  }
  if (audio_on) {
    pipeline.audio = &audio_track;
    pipeline.audio_tap = &audio_tap;
    audio_tap.packets = &pipeline.audio_packets;
    audio_tap.bytes = &pipeline.audio_bytes;
  }
  // 计数器绑在 tap 上。这三行漏掉的话编码照跑、推流照推，只有报表永远是 0 ——
  // 而"队列满了在丢"和"已编码 0 帧"同时出现，正是这个漏绑的特征。
  tap.packets = &pipeline.packets;
  tap.keyframes = &pipeline.keyframes;
  tap.bytes = &pipeline.bytes;

  std::thread encoder_thread([&pipeline] { pipeline.run(); });

  // --- 6. 采集 + 定速 -----------------------------------------------------
  //
  // 定速循环的形状：每个 tick 有一个截止时刻，在截止之前不断向 DXGI 要帧
  // （要到的帧会覆盖同一个缓冲，所以最终留下的永远是这一拍里最新的画面），
  // 到点就把当前缓冲入队。
  //
  // 为什么不是"来一帧编一帧"：桌面是事件驱动的，静止时一帧都不来，动起来时
  // 一秒钟可能来几百帧。直接转发的后果是 MP4 的时间轴和真实时间对不上，
  // 而且播放器会看到一顿一顿的画面。定速之后输出是恒定帧率，静止的桌面
  // 也是连续的画面 —— 这正是 NVR 想要的。
  nanoseconds const period(1'000'000'000LL / opt.fps);
  SteadyClock::time_point start = SteadyClock::now();

  std::uint64_t tick = 0;
  std::uint64_t content_id = 0;
  std::shared_ptr<const BgraFrame> last;
  /// 连续重建失败次数。成功取到一帧就归零 —— 它驱动的是"多久再试一次"和
  /// "还要不要每次都报"，不是"什么时候放弃"（**没有放弃这回事**）。
  int recover_attempts = 0;
  /// 会话已经失效、正等着重建。为真时这一拍不采集，画面沿用上一帧。
  bool session_down = false;
  SteadyClock::time_point next_recover_at{};
  SteadyClock::time_point next_recover_warn{};
  bool warned_pool = false;
  bool warned_no_content = false;
  bool logged_cursor = false;
  bool fatal = false;
  /// 采集尺寸变了，需要调用方重建整条链。见 RunResult::Restart。
  bool restart = false;
  SteadyClock::time_point next_report = start + std::chrono::seconds(1);

  // --- 7b. 水印 -----------------------------------------------------------
  //
  // **合成点只有这一个**：池子里那一份 BGRA 主/子编码器共用，MP4、RTSP、
  // 浏览器预览、快照吃的也都是从它派生的东西。在这里画一次，六路输出全都
  // 带上，而且不可能漂移 —— 分开画的话"哪一路的文字晚了一个版本"是查不清的。
  OsdOverlay osd;
  /// 装一份水印配置进 `osd`（**原始那份**，未缩放的，日志用它报数）。
  ///
  /// 字号是按**主码流**给的像素数（配置注释这么说的），而它是画在**桌面原生
  /// 尺寸**的缓冲上、再让编码器缩下去的。所以原生尺寸下的字号要按比例放大，
  /// 缩完才正好是用户要的那个大小。不补这一下的症状是"选了流畅档之后水印
  /// 莫名其妙变小了"，而它和编码参数看起来毫无关系。
  OsdConfig cur_osd;
  auto install_osd = [&](OsdConfig const& src) {
    OsdConfig scaled = src;
    if (main_h > 0 && main_h != h) {
      double const k = static_cast<double>(h) / static_cast<double>(main_h);
      for (auto& it : scaled.items) {
        if (it.font_size > 0) {
          it.font_size = static_cast<int>(static_cast<double>(it.font_size) * k + 0.5);
        }
      }
    }
    osd.configure(scaled, opt.device);
    cur_osd = src;
  };
  /// 已经装进去的那一份（**只是 live 那条路的记账**）。指针换了就把新的比一次
  /// 序列化文本 —— NVR 会周期性重复下发同一份 OSD，内容没变就不该重配：重配会
  /// 把 OsdOverlay 里按 (文字, 字号) 缓存的字形掩码全丢掉，白重画一次。
  std::shared_ptr<OsdConfig const> applied_osd;
  std::string applied_osd_text;
  {
    auto const seed = opt.live_osd ? opt.live_osd->snapshot()
                                   : std::make_shared<OsdConfig const>(opt.osd);
    install_osd(*seed);
    applied_osd = seed;
    applied_osd_text = osd_to_json(*seed);
  }
  /// 上一帧水印上的文字。变了就说明**即使桌面没动**也得出一帧新的 ——
  /// 否则桌面静止时编码器复用上一帧的 YUV（见 x264_encoder 里那条
  /// content_changed 短路），水印上的秒会冻住。
  std::string last_osd_key;
  /// 水印**配置**刚换过，下一帧必须重画一次。
  ///
  /// `key()` 只是那几段文字（见 osd_overlay.cpp），所以"把水印从左上拖到右下"
  /// 这种改动**一个字符都没变** —— 光靠 `osd_changed` 的话，桌面静止时（真的
  /// 一动不动，比如锁屏、停在某个静态页面上）新位置要等到下一次桌面变化才出现。
  /// 那不是"不生效"，是"生效得看运气"，比不生效更难查。
  bool osd_force_redraw = false;
  bool logged_osd = false;

  // --- 7c'. 自适应降载的运行时状态 ----------------------------------------
  /// （控制器 `adaptive` 本身建在上面「立刻推一帧」之前 —— 理由见那一段。）
  /// 上一次统计报表时的两个累计值，用来算"这一秒丢了几个"。
  /// **必须看增量**：累计值从开机起就没归过零，拿它判"现在卡不卡"会在录制
  /// 跑了一小时之后永远为真（哪怕那 300 个丢弃全发生在启动的那一秒）。
  std::uint64_t last_drop_sample = 0;
  std::uint64_t last_exhaust_sample = 0;
  /// 这一秒里池子见底过没有。见 AdaptiveController::observe 里那段说明。
  bool starved_this_second = false;
  /// 上一次**打日志**时的 duty，只用来"变化时才报一行"。
  int last_logged_duty = 1;

  while (!stop_.load(std::memory_order_relaxed) &&
         !g_interrupted.load(std::memory_order_relaxed)) {
    if (pipeline.failed.load(std::memory_order_relaxed)) break;
    // 用户点了「重载」（`POST /api/reload`）：体面收尾，让调用方按新配置再来
    // 一趟。和下面那个"尺寸变了"走的是同一条出口，**收尾是完整的** —— 文件有
    // trailer、编码器冲干净了，所以这不是"中断录制"而是"分段"。
    if (reconfigure_.load(std::memory_order_relaxed)) {
      D2O_INFO("收到重载请求：这一段落盘收尾，下一段按新配置重开"
               "（录制会分段、RTSP 客户端会断开重连）。");
      restart = true;
      break;
    }
    if (opt.seconds > 0 &&
        tick >= static_cast<std::uint64_t>(opt.seconds) *
                    static_cast<std::uint64_t>(opt.fps)) {
      break;
    }

    // 水印上的"现在"取的是**设备时钟**（主机时刻 + NVR 下发的偏移），不是
    // 主机时刻 —— 这样 NVR 校过时之后水印会和录像机的时间轴对齐。
    // 没有 DeviceState（单跑录制、没有 ONVIF）时退回主机时刻。
    std::chrono::system_clock::time_point const device_now =
        opt.device ? opt.device->now() : std::chrono::system_clock::now();

    // --- 浏览器预览的时间原点（只播一次种）----------------------------------
    //
    // 前端要报"真实延迟"，就得把 `<video>.currentTime`（媒体时间）换算成**墙钟**：
    // `这一帧是什么时候拍的 ≈ t0_wall_ms + currentTime*1000`。这个 t0 就是这里播的。
    //
    // **必须是墙钟，不能拿上面那个 `device_now`。** 设备时间是主机时刻加 NVR 下发的
    // 偏移（可能几个小时），而浏览器那边的参照是它自己的 `Date.now()` —— 用一个带
    // 偏移的钟去减，算出来的"延迟"会平白多出那个偏移量，而它看起来只是个偏大或
    // 偏小的正常数字（陷阱 63 的同一族：尺子的宾语错了，报出来的数照样合理）。
    //
    // 只播一次（tick 0），因为一个常数就够：PTS 就是拍号、每拍 +1（自适应降载改的
    // 是"每 d 拍入队一次"，拍号本身的量纲没变），所以任意一对 (墙钟, 拍号) 定出的
    // 都是同一个常数。取第 0 拍是为了不额外引入"第一帧实际在哪一拍"的抖动。
    if (tick == 0 && pipeline.preview) {
      pipeline.preview->note_t0_base_ms(static_cast<std::int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()));
    }

    // --- 水印的热生效（每拍一次）-------------------------------------------
    //
    // 网页上保存、NVR 下发 SetOSD/CreateOSD/DeleteOSD，都在**别的线程**上把一份
    // 新配置换进 LiveOsd（见 common/live_osd.h）。这里每拍取一次当期那一份，
    // 取到新的就装进去 —— 于是画面**下一帧**就变，不重建管线、不切分录像。
    //
    // 判据分两层，都是必要的：指针不同说明"有人 set 过"（零开销的一次比较，
    // 是常态），序列化文本不同才说明"内容真的变了"（NVR 周期性重复下发同一份
    // OSD 是常态，那时不该重配 —— 重配会把字形掩码缓存全丢掉）。
    if (opt.live_osd) {
      auto live = opt.live_osd->snapshot();
      if (live.get() != applied_osd.get()) {
        std::string const text = osd_to_json(*live);
        if (text != applied_osd_text) {
          install_osd(*live);
          applied_osd_text = text;
          osd_force_redraw = true;
          ++stats_.osd_hot_applied;
          D2O_INFO("水印配置已热生效：%zu 项（%s）", cur_osd.items.size(),
                   cur_osd.adaptive_color ? "自适应配色" : "固定配色");
        }
        applied_osd = std::move(live);
      }
    }

    SteadyClock::time_point deadline = start + period * (tick + 1);

    // 落后太多（锁屏、系统卡顿、编码线程被抢）时重设时间基准。
    //
    // 不这么做的话有两种坏结果：要么疯狂空转把落后追回来（瞬间编几百帧），
    // 要么让 tick 落后于真实时间，文件时长比实际录制时间长。重设基准等于
    // "这段停滞不录进文件" —— 这是最容易解释的行为。
    {
      SteadyClock::time_point const now = SteadyClock::now();
      if (now > deadline + period * static_cast<std::uint64_t>(opt.fps * kMaxLagSeconds)) {
        D2O_WARN("采集落后超过 %d 秒，重设时间基准（这段停滞不会出现在文件里）",
                 kMaxLagSeconds);
        start = now - period * tick;
        deadline = start + period * (tick + 1);
      }
    }

    // --- 会话恢复 ---------------------------------------------------------
    //
    // 重建**不提在 acquire 的重试循环里做**，而是放到这一层。理由有二：
    //
    //   * 重建失败的退让是秒级的（显示器拔了要等它插回来），睡在"这一拍剩下的
    //     时间里多要几次帧"那个循环里，等于让重试节奏去决定采集的时间轴；
    //     放在这里，恢复期间每一拍照常推进 tick、照常出报表。
    //   * 会话没建起来的时候 acquire() 会返回 Fatal（dup_ 是空的），而它没法
    //     区分"没有会话"和"别的致命错"。句柄空着就不去调它，是唯一稳妥的做法。
    //
    // 恢复期间照常把上一帧入队：NVR 那边看到的是画面停住，而不是流断掉。
    if (session_down) {
      SteadyClock::time_point const now = SteadyClock::now();
      if (now >= next_recover_at) {
        std::string rerr;
        if (source->reopen(rerr)) {
          D2O_INFO("捕获会话已重建（累计 %llu 次）",
                   static_cast<unsigned long long>(stats_.access_lost));
          if (recover_attempts > 0) {
            D2O_INFO("（连续失败 %d 次之后重建成功，录制继续）", recover_attempts);
          }
          session_down = false;
          if (source->width() != w || source->height() != h) {
            // 尺寸变了。**就地改不了**：编码器的 SPS/PPS、MP4 的轨道头、
            // SDP 里的 sprop-parameter-sets 和新尺寸的 avcC 全都绑死在旧值上，
            // 硬换只会得到一个解不出来的流。交给调用方把整条链重建一遍
            // （见 CaptureSession::RunResult::Restart）。
            D2O_WARN("采集尺寸从 %dx%d 变成 %dx%d：编码器、MP4 轨道和 SDP 里"
                     "都写死了旧尺寸，就地换不了。这一段落盘收尾，下一段按新"
                     "尺寸重开。",
                     w, h, source->width(), source->height());
            restart = true;
            break;
          }
        } else {
          ++recover_attempts;
          // 前几次每次都报（好定位），之后每分钟报一次（免得刷屏）。**不放弃。**
          if (recover_attempts <= kNoisyRecoverAttempts || now >= next_recover_warn) {
            D2O_WARN("重建捕获会话失败（第 %d 次）：%s —— 继续重试，显示器接回来"
                     "之后会自动接着录",
                     recover_attempts, rerr.c_str());
            next_recover_warn =
                now + std::chrono::seconds(kRecoverWarnIntervalSeconds);
          }
          next_recover_at =
              now + milliseconds(std::min(150 * recover_attempts, kMaxRecoverDelayMs));
        }
      }
    }

    BgraFrame* buf = pool->take();
    if (!buf) {
      // 池子空了：编码 + 写出已经落后一整个池子的量。跳过这一帧。
      // 注意 tick 照样要推进 —— 否则时间轴会被拉长。
      ++stats_.pool_exhausted;
      // 这一秒里池子见底过。自适应**必须**知道这件事：池子空时我们直接
      // continue，根本走不到入队那一句，所以它可能完全不伴随队列丢弃 ——
      // 只看丢弃数的话，最严重的那个工况恰好是唯一看不见的。
      starved_this_second = true;
      if (!warned_pool) {
        warned_pool = true;
        D2O_WARN("帧缓冲池已耗尽，开始跳帧（编码跟不上采集）");
      }
      ++tick;
      ++stats_.ticks;
      std::this_thread::sleep_until(deadline);
      continue;
    }

    bool got_content = false;
    for (;;) {
      // 会话正等着重建（见上面那一段）。句柄是空的，acquire 只会回 Fatal。
      if (session_down) break;

      SteadyClock::time_point const now = SteadyClock::now();
      if (now >= deadline) break;

      auto const remain =
          std::chrono::duration_cast<milliseconds>(deadline - now).count();
      auto const timeout = static_cast<std::uint32_t>(
          std::min<std::int64_t>(std::max<std::int64_t>(remain, 1), 100));

      bool changed = false;
      std::string aerr;
      SourceStatus const st = source->acquire(timeout, *buf, changed, aerr);

      if (st == SourceStatus::Ok) {
        got_content = changed;
        recover_attempts = 0;
      } else if (st == SourceStatus::Timeout) {
        continue;  // 桌面没动，看看这一拍剩下的时间里会不会动
      } else if (st == SourceStatus::AccessLost) {
        // **不做原地重建，也不睡觉**：标一下，这一拍就到此为止，下一拍由上面
        // 那段统一处理。在这里重建会睡 150~2000 ms，而采集循环一停，编码队列
        // 就见底 —— 以前这一停正是把编码线程"劝退"的原因（见 PopStatus）。
        ++stats_.access_lost;
        session_down = true;
        next_recover_at =
            SteadyClock::now() + milliseconds(kFirstRecoverDelayMs);
        break;
      } else {
        err = aerr;
        fatal = true;
        break;
      }
    }
    if (fatal) break;

    // 这一帧水印上会写什么。**在写像素之前就要算**，因为下面要用它判
    // "桌面没动但水印变了没有"。
    bool const osd_wanted = osd.enabled();
    std::string const osd_key = osd_wanted ? osd.key(device_now) : std::string();
    bool const osd_changed = osd_wanted && osd_key != last_osd_key;
    // 两件不同的事，合成一路给下面的判断用：文字变了（每秒一次，自动），
    // 或者**配置变了**（网页保存 / NVR 下发 SetOSD，见上面那段热生效刷新）。
    bool const osd_redraw = osd_changed || osd_force_redraw;

    if (got_content || (osd_redraw && last)) {
      // 桌面没动、只是水印变了：把上一帧拷过来重画。
      //
      // **为什么要拷而不是直接复用上一帧的内存**：`last` 指向的那块已经
      // publish 出去了，快照、正在解码的编码线程都可能还持有它；就地写它会
      // 改到别人正在读的像素（快照拍出半新半旧的画面）。拷一份是唯一正确的
      // 做法，而它每秒只发生一次（水印的粒度是秒），代价可以忽略。
      //
      // ⚠️ **但"水印配置变了"不能走这条路**：`last` 里烧着的是**旧**配置画出
      // 来的水印（旧位置、旧字号，关掉水印时则是整块旧水印），从它拷过来再
      // 重画，等于把旧的又画了一遍 —— 症状正是"改了没反应、关也关不掉"。
      // 配置变了要从**没有水印的那份桌面图**重新合成（reemit）。
      if (!got_content) {
        bool rebuilt = false;
        if (osd_force_redraw) {
          rebuilt = source->reemit(*buf) == SourceStatus::Ok;
          if (rebuilt) ++stats_.osd_rebuilt_frames;
        }
        if (!rebuilt && last) {
          std::memcpy(buf->pixels.data(), last->pixels.data(), last->byte_size());
        }
      }
      if (osd_wanted) {
        // 像素是**桌面原生尺寸**的（缩放交给编码器），所以这里传的就是池子里
        // 那块缓冲的真实宽高与步长 —— 传主码流的尺寸会让水印画错位置。
        osd.compose(buf->pixels.data(), w, h,
                    static_cast<std::size_t>(buf->stride), device_now);
        last_osd_key = osd_key;
        if (!logged_osd) {
          logged_osd = true;
          D2O_INFO("水印已启用：%zu 项（%s）", cur_osd.items.size(),
                   cur_osd.adaptive_color ? "自适应配色" : "固定配色");
        }
      }
      osd_force_redraw = false;
      buf->content_id = ++content_id;
      last = pool->publish(buf);  // shared_ptr<BgraFrame> → shared_ptr<const BgraFrame>
      if (got_content) {
        ++stats_.content_frames;
      } else {
        // 桌面没动，只是水印的文字变了。这个计数器是"钟在走"的唯一证据。
        ++stats_.osd_refresh_frames;
      }
      // 第一张带指针的画面出来时报一次形状/位置。**只报一次**：指针每动一下
      // 都报的话就是每秒几十行，而这里要回答的只是"它到底合成上了没有、
      // 合成的是什么形状"。
      if (!logged_cursor && cursor_diag && cursor_diag->composited_frames() > 0) {
        logged_cursor = true;
        D2O_INFO("鼠标指针已合成进画面：%s",
                 cursor_diag->pointer_description().c_str());
      }
    } else {
      // 画面没变：缓冲原样还回池子，这一拍复用上一帧（不重新拷贝像素）。
      std::shared_ptr<BgraFrame> recycle = pool->publish(buf);
      recycle.reset();
    }

    // **降帧率在这里落地。** `tick % duty != 0` 的拍不进队，于是帧率降到
    // fps/duty；而 tick 仍然每拍 +1，PTS 每帧前进 duty，墙钟与画面严格对应
    // —— 这就是它无损的原因（见 adaptive.h）。
    //
    // 水印刷新那一帧不受 duty 限制：让时钟停在上一秒比多编一帧重要得多，
    // 而它每秒最多发生一次。落在非 duty 倍数上也无所谓 —— tick 的量纲没变，
    // 只是那一处间距略不匀。
    bool const duty_ok = (tick % static_cast<std::uint64_t>(adaptive.duty())) == 0;
    if (last && (duty_ok || osd_redraw)) {
      stats_.queue_dropped += queue.push(CapturedFrame{last, tick});
    } else if (last) {
      // 这一拍被自适应丢掉了。**单独计数**：它和 queue_dropped（编码跟不上）
      // 是完全不同的两件事，混在一起的话"画质档位降了帧率"会被看成"编码器
      // 出问题了"。
      ++stats_.adaptive_dropped;
    } else if (!warned_no_content) {
      // 一帧内容都没拿到过。理论上只在"会话建立后桌面一次都没刷新过"时发生，
      // 实际几乎不会 —— 连鼠标指针闪烁都算一次刷新。
      warned_no_content = true;
      D2O_WARN("还没取到任何桌面画面，等待桌面发生变化…");
    }

    ++tick;
    ++stats_.ticks;

    SteadyClock::time_point const now = SteadyClock::now();
    if (now >= next_report) {
      // 用 now 而不是累加：累加在长时间卡顿之后会连着打好几行相同的报表。
      next_report = now + std::chrono::seconds(1);
      double const elapsed = std::chrono::duration<double>(now - start).count();
      std::uint64_t const pk = pipeline.packets.load(std::memory_order_relaxed);
      std::uint64_t const by = pipeline.bytes.load(std::memory_order_relaxed);

      // --- 自适应：把**这一秒的增量**喂进去 -------------------------------
      //
      // 用增量而不是累计值。累计值从开机起就没归过零，拿它判"现在卡不卡"
      // 会在录制跑了一小时之后永远为真 —— 哪怕那几百个丢弃全发生在启动的
      // 那一秒。这是"计数器名字与实际语义相反"那一类错的近亲。
      std::uint64_t const total_dropped = stats_.queue_dropped;
      std::uint64_t const total_exhausted = stats_.pool_exhausted;
      std::uint64_t const drop_delta = total_dropped - last_drop_sample;
      std::uint64_t const exhaust_delta = total_exhausted - last_exhaust_sample;
      last_drop_sample = total_dropped;
      last_exhaust_sample = total_exhausted;
      adaptive.observe(drop_delta + exhaust_delta, starved_this_second);
      starved_this_second = false;

      // 最近 10 秒的丢弃数（指数衰减近似）：既不用维护环形缓冲，也不会像
      // 累计值那样"永远显得在丢"。
      stats_.drops_recent = stats_.drops_recent - stats_.drops_recent / 10 + drop_delta;
      stats_.perf_duty = adaptive.duty();
      stats_.perf_target_fps = adaptive.target_fps();
      stats_.perf_state = perf_state_name(adaptive.state());
      stats_.perf_hint = perf_hint_for(adaptive, stats_.drops_recent, opt.fps);

      int const old_duty = last_logged_duty;
      if (adaptive.duty() != old_duty) {
        last_logged_duty = adaptive.duty();
        if (adaptive.duty() > old_duty) {
          D2O_INFO("性能不足，自适应降帧：目标 %d fps（原 %d fps，每 %d 拍出 1 帧）",
                   adaptive.target_fps(), opt.fps, adaptive.duty());
        } else {
          D2O_INFO("性能已恢复，自适应升帧：目标 %d fps（每 %d 拍出 1 帧）",
                   adaptive.target_fps(), adaptive.duty());
        }
      }

      std::string tail;
      // 逐路打。**入队/丢弃必须分开看**：只报丢弃时，"每秒丢 500 个"既可能是
      // 编码器产出过快，也可能是消费端根本不拉，分不清方向（见陷阱 26）。
      //
      // 只打**本通道自己那几路**（`stats_.rtsp_streams` 是它），服务器下标再加
      // `rtsp_stream_base`。多通道下这两者不再相等，照着服务器全长遍历会让每个
      // 通道都打一遍所有通道的推流状态 —— 日志里看着正常，内容全是别人的。
      for (std::size_t i = 0; i < stats_.rtsp_streams.size(); ++i) {
        std::size_t const idx = opt.rtsp_stream_base + i;
        tail += fmt("  推流[%zu] 观众 %u  队列 %zu/%zu 入队 %llu 丢弃 %llu",
                    i, opt.rtsp_server->client_count(idx),
                    opt.rtsp_server->queue_size(idx),
                    opt.rtsp_server->queue_capacity(idx),
                    static_cast<unsigned long long>(
                        opt.rtsp_server->nalu_pushed(idx)),
                    static_cast<unsigned long long>(
                        opt.rtsp_server->nalu_dropped(idx)));
      }
      // 自适应没触发时不打那几个字段：常态下每行都多一截"自适应 关"是纯噪音，
      // 而"它到底动过没有"要看的是**变化**，不是每秒的存在感。
      std::string perf;
      if (adaptive.duty() > 1) {
        perf = fmt("  自适应 %d fps（1/%d） 丢 %llu", adaptive.target_fps(),
                   adaptive.duty(),
                   static_cast<unsigned long long>(stats_.adaptive_dropped));
      }
      D2O_INFO("%5.1fs  已编码 %llu 帧（%.1f fps）  %.0f kbps  队列 %zu/%zu "
               "丢弃 %llu  缓存池空闲 %zu/%zu%s%s",
               elapsed, static_cast<unsigned long long>(pk),
               elapsed > 0 ? static_cast<double>(pk) / elapsed : 0.0,
               elapsed > 0 ? static_cast<double>(by) * 8.0 / elapsed / 1000.0 : 0.0,
               queue.size(), queue.capacity(),
               static_cast<unsigned long long>(stats_.queue_dropped),
               pool->available(), pool->total(), perf.c_str(), tail.c_str());

      // 顺手把实时字段填进 stats_，再交给 Web 层的钩子。
      // 填在**这里**而不是等收尾：不填的话，运行期间 `stats()` 里的
      // packets/bytes 一直是 0，Web 上就是"帧率 0、码率 0"而画面明明是通的。
      stats_.elapsed_s = elapsed;
      stats_.packets = pk;
      stats_.keyframes = pipeline.keyframes.load(std::memory_order_relaxed);
      stats_.bytes = by;
      stats_.effective_fps = elapsed > 0 ? static_cast<double>(pk) / elapsed : 0.0;
      stats_.cursor_frames = cursor_diag ? cursor_diag->composited_frames() : 0;
      stats_.pointer_only_frames =
          cursor_diag ? cursor_diag->pointer_only_frames() : 0;
      if (rtsp_on) fill_rtsp_stats(stats_, *opt.rtsp_server, opt.rtsp_stream_base, pipeline);
      if (audio_on) fill_audio_stats(stats_, audio_track, pipeline);
      if (stats_hook_) stats_hook_(stats_);

      // 快照留档，跟着这一秒的报表一起更新。**放在这里而不是每帧都换**：
      // 每帧换等于每秒 30 次持锁 + 一次 shared_ptr 原子增减，而快照端点
      // 是人/NVR 偶尔拉一张，一秒的陈旧完全无害。
      //
      // 只在真的有帧时替换：`last` 为空说明桌面一次都没刷新过，那就不该
      // 用一个空指针把上一张好图盖掉。
      if (last) {
        std::lock_guard<std::mutex> lk(snapshot_mu_);
        snapshot_ = last;
      }
    }

    std::this_thread::sleep_until(deadline);
  }

  // --- 7. 收尾 ------------------------------------------------------------
  // 顺序是硬性的：
  //   关队列 → join 编码线程 → 冲刷编码器 → 关文件 → 停 RTSP
  // 编码线程必须**先**停下，RtspServer::stop() 才能安全地析构 Live555 的那些
  // 对象（见 rtsp_server.h 的文件头说明）。中间的冲刷也会往 RTSP 推最后几帧，
  // 所以它必须排在 stop() 前面。
  SteadyClock::time_point const end = SteadyClock::now();
  stats_.elapsed_s = std::chrono::duration<double>(end - start).count();

  queue.close();
  encoder_thread.join();

  stats_.packets = pipeline.packets.load(std::memory_order_relaxed);
  stats_.keyframes = pipeline.keyframes.load(std::memory_order_relaxed);
  stats_.bytes = pipeline.bytes.load(std::memory_order_relaxed);
  stats_.cursor_frames = cursor_diag ? cursor_diag->composited_frames() : 0;
  stats_.pointer_only_frames =
      cursor_diag ? cursor_diag->pointer_only_frames() : 0;

  if (pipeline.failed.load(std::memory_order_relaxed)) {
    // 编码线程挂了。已经把写出来的部分收尾成一个能播的文件，但整体算失败。
    std::string const werr = pipeline.error;
    std::string ignored;
    if (!opt.output_path.empty()) writer.close(ignored);
    // 这两条失败路径跳过了下面的冲刷，也就是跳过了那次 fill_rtsp_stats。
    // 在这里补一次：出错时"到底推出去多少、有没有观众"恰好是最需要知道的，
    // 而漏掉它会让收尾报表上每一路的数字全是 0。音频同理 —— 而且它比视频更
    // 需要：`audio_state` 是"音轨到底起没起来"的唯一答案，其中 `error` 这一档
    // 只有走到这里才报得出来。
    if (rtsp_on) fill_rtsp_stats(stats_, *opt.rtsp_server, opt.rtsp_stream_base, pipeline);
    if (audio_on) fill_audio_stats(stats_, audio_track, pipeline);
    err = fmt("编码线程出错：%s（已写出的部分仍会保存）", werr.c_str());
    return RunResult::Failed;
  }
  if (fatal) {
    // 采集侧出错，err 里已经是原因了。同样把已写出的部分收尾。
    std::string ignored;
    if (!opt.output_path.empty()) writer.close(ignored);
    if (rtsp_on) fill_rtsp_stats(stats_, *opt.rtsp_server, opt.rtsp_stream_base, pipeline);
    if (audio_on) fill_audio_stats(stats_, audio_track, pipeline);
    return RunResult::Failed;
  }

  // 冲刷编码器里剩下的帧。少这一步最后几帧会丢 —— 它们还在 x264 的内部缓冲里。
  std::atomic<std::uint64_t> flush_packets{0};
  std::atomic<std::uint64_t> flush_keyframes{0};
  std::atomic<std::uint64_t> flush_bytes{0};
  PacketTap flush_tap;
  flush_tap.consumers = tap.consumers;
  flush_tap.packets = &flush_packets;
  flush_tap.keyframes = &flush_keyframes;
  flush_tap.bytes = &flush_bytes;

  std::string ferr;
  if (!encoder.flush(std::ref(flush_tap), ferr)) {
    D2O_WARN("冲刷编码器失败（末尾几帧可能丢失）：%s", ferr.c_str());
  }
  if (!flush_tap.ok) {
    D2O_WARN("写出剩余帧失败：%s", flush_tap.error.c_str());
  }
  stats_.packets += flush_packets.load();
  stats_.keyframes += flush_keyframes.load();
  stats_.bytes += flush_bytes.load();

  // 子码流同样要冲。zerolatency 下 x264 内部不留帧，这一步通常是空转 ——
  // 但"通常"不是"总是"（换 preset 就不是了），而少这一步的代价是子码流末尾
  // 缺帧，在 NVR 的多画面网格上表现为"最后一下卡住"。计数器直接记进
  // pipeline 的原子量，这样下面 fill_rtsp_stats 拿到的就是含冲刷的总数。
  if (sub_on) {
    PacketTap sub_flush_tap;
    sub_flush_tap.consumers = sub_tap.consumers;
    sub_flush_tap.packets = &pipeline.sub_packets;
    sub_flush_tap.bytes = &pipeline.sub_bytes;
    std::string sferr;
    if (!sub_encoder.flush(std::ref(sub_flush_tap), sferr)) {
      D2O_WARN("冲刷子码流编码器失败（末尾几帧可能丢失）：%s", sferr.c_str());
    }
    if (!sub_flush_tap.ok) {
      D2O_WARN("写出子码流剩余帧失败：%s", sub_flush_tap.error.c_str());
    }
  }

  // 音频同样要冲，而且**必须在 writer.close() 之前**：AAC 编码器的 FIFO 里
  // 可能还攒着不到 21 ms（一帧 1024 样本）的尾音，晚于关文件就会被静默丢掉。
  // 它在音轨的**结尾**，症状是"音轨比视频短一截"—— 最容易被归因到别处
  // （同步、NVR、封装），所以这个顺序是硬的，不是"顺手放这儿"。
  //
  // 复用 `audio_tap` 的消费者表，**顺序也一样**（会拿走包内容的下游排在最后）：
  // 冲出来的 AU 与正常那一路走完全相同的去向，否则症状是"MP4 里有音轨而 RTSP
  // 没有声音"或者反过来，而两条路单独看都正常。
  if (audio_on) {
    PacketTap audio_flush_tap;
    audio_flush_tap.consumers = audio_tap.consumers;
    // 计数器直接记进 pipeline 的原子量（和子码流那次一样），这样下面
    // fill_audio_stats 读到的是含冲刷的总数，不用再加一次。
    audio_flush_tap.packets = &pipeline.audio_packets;
    audio_flush_tap.bytes = &pipeline.audio_bytes;
    std::string aferr;
    if (!audio_track.flush(std::ref(audio_flush_tap), aferr)) {
      D2O_WARN("冲刷音频编码器失败（音轨末尾不到 21 ms 会丢）：%s", aferr.c_str());
    }
    if (!audio_flush_tap.ok) {
      D2O_WARN("写出剩余音频失败：%s", audio_flush_tap.error.c_str());
    }
  }

  // 放在冲刷**之后**：顶层 packets/bytes 到这里才是最终值，而 fill_rtsp_stats
  // 对主码流那一路就是照抄它们。早一步调用，两处会差着冲刷的那几帧。
  if (rtsp_on) fill_rtsp_stats(stats_, *opt.rtsp_server, opt.rtsp_stream_base, pipeline);
  // 音频那一组同理：`audio_frames` 要等混音器把最后几拍吐完才是最终值。
  if (audio_on) fill_audio_stats(stats_, audio_track, pipeline);

  if (!opt.output_path.empty() && !writer.close(err)) return RunResult::Failed;

  stats_.effective_fps = stats_.elapsed_s > 0
                             ? static_cast<double>(stats_.packets) / stats_.elapsed_s
                             : 0.0;

  // 走到这里说明这一趟**已经完整收尾**：编码线程 join 了、编码器冲干净了、
  // 文件关了（能播）、RTSP 停了。所以 Restart 和 Finished 的差别只是"要不要
  // 再来一趟"，不是"成没成功"。
  return restart ? RunResult::Restart : RunResult::Finished;
}

}  // namespace d2o
