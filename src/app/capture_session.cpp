#include "app/capture_session.h"

#include "capture/desktop_capture.h"
#include "capture/display_info.h"
#include "capture/frame.h"
#include "common/bounded_queue.h"
#include "common/log.h"
#include "encoder/fmp4_muxer.h"
#include "encoder/mp4_writer.h"
#include "encoder/video_encoder.h"
#include "rtsp/rtsp_server.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
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

/// 捕获会话连续重建失败多少次就放弃。会话失效是正常事件，但一直重建不起来
/// 说明是硬件层面的事（显示器拔了、驱动挂了），继续转没有意义。
constexpr int kMaxRecoverAttempts = 10;

/// 落后超过这么多秒就重设时间基准，见 run() 里的说明。
constexpr int kMaxLagSeconds = 2;

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
struct Mp4Consumer : PacketConsumer {
  Mp4Writer* writer = nullptr;
  bool on_packet(AVPacket& pkt, std::string& err) override {
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
    // **空是常态**：mov 封装器要等下一个关键帧才知道上一片在哪结束，
    // 所以每两秒才有一次非空。这里不能把"空"当错误。
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
  /// 不参与 tap 的扇出：它是"发初始化段"这个一次性动作的入口，见 run() 第一行。
  /// 浏览器预览只跟主码流走（子码流是给 NVR 的多画面网格用的，不是给人看的）。
  PreviewFeed* preview = nullptr;

  std::atomic<std::uint64_t> packets{0};
  std::atomic<std::uint64_t> keyframes{0};
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<std::uint64_t> frames{0};
  /// 子码流的计数**单独一套**。合在一起的话，两路的分辨率差 8 倍，
  /// 平均码率变成一个没有意义的数，而"子码流是不是出问题了"就看不出来了。
  std::atomic<std::uint64_t> sub_packets{0};
  std::atomic<std::uint64_t> sub_bytes{0};
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

    while (queue->pop(f, milliseconds(200))) {
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
void fill_rtsp_stats(SessionStats& s, RtspServer const& rtsp,
                     Pipeline const& pipeline) {
  for (std::size_t i = 0; i < s.rtsp_streams.size(); ++i) {
    SessionStats::RtspStreamStats& st = s.rtsp_streams[i];
    st.clients = rtsp.client_count(i);
    st.nalu_dropped = rtsp.nalu_dropped(i);
    st.max_nalu_bytes = rtsp.max_nalu_bytes(i);
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

}  // namespace

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
  // 子码流这里只报尺寸，码率交给下面逐路那一段 —— 两处都报的话，同一份报表里
  // 会出现两个"子码流码率"，而它们的口径（是否含冲刷、按哪路算）一旦不一致，
  // 读的人就得先决定信哪个。
  if (sub_width > 0) {
    s += fmt("\n  子码流尺寸 %dx%d", sub_width, sub_height);
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

bool CaptureSession::run(SessionOptions const& opt, std::string& err) {
  stop_.store(false, std::memory_order_relaxed);
  stats_ = SessionStats{};

  // 没有任何下游时也算"有事可做"的情况只有一种：Web 服务在跑。
  // 那时编码链照常转，数据被丢弃，但每秒的统计要喂给 /ws/stats 和 /api/status
  // —— 否则 Web 页面上全是 0，看起来像坏了。
  // 谁都没要的时候仍然拒绝：那种调用一定是命令行写错了，早报比空转强。
  if (opt.output_path.empty() && !opt.rtsp.enabled && !stats_hook_) {
    err = "既没有指定输出文件也没有开推流，那就没有任何事可做。"
          "给个 --record <文件> 或者 --rtsp。";
    return false;
  }

#ifdef _WIN32
  ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
  struct HandlerGuard {
    ~HandlerGuard() { ::SetConsoleCtrlHandler(console_ctrl_handler, FALSE); }
  } handler_guard;
#endif

  // --- 1. 选输出 ----------------------------------------------------------
  std::vector<OutputInfo> outputs = enumerate_outputs();
  if (outputs.empty()) {
    err = "没有找到任何显示输出";
    return false;
  }
  if (opt.display_index >= outputs.size()) {
    err = fmt("--display %u 超出范围，本机只有 %zu 个输出。用 --list-displays 看清单",
              opt.display_index, outputs.size());
    return false;
  }
  OutputInfo const& out = outputs[opt.display_index];
  D2O_INFO("目标输出：#%u %s", opt.display_index, describe(out).c_str());

  // 先真的探一下能不能录。这一步很值得：本机装着向日葵，它的虚拟屏在 DXGI
  // 上根本建不了复制会话，早点说清楚比录出十分钟黑屏好。
  CaptureProbe const probe = probe_capturable(out);
  if (!probe.ok) {
    err = fmt("这个输出无法做桌面复制：\n    %s\n  输出：%s\n"
              "  用 --list-displays 看全部输出，选一个标着 [可捕获] 的换 --display 序号",
              probe.message.c_str(), describe(out).c_str());
    return false;
  }

  // --- 2. 捕获会话 --------------------------------------------------------
  DesktopCapture capture;
  if (!capture.open(out, err)) return false;

  int const w = capture.width();
  int const h = capture.height();
  if (w <= 0 || h <= 0) {
    err = fmt("拿到的桌面尺寸不合法：%dx%d", w, h);
    return false;
  }
  if ((w % 2) != 0 || (h % 2) != 0) {
    // H.264 的 4:2:0 色度是半分辨率，宽高必须是偶数。外接奇异分辨率的显示器
    // 会碰到（历史上有一些 1366x768 之外的非标准模式）。
    err = fmt("桌面尺寸 %dx%d 不是偶数，H.264 编不了。"
              "改一下显示分辨率再录（Phase 6 会加裁剪处理）", w, h);
    return false;
  }

  // 尺寸要在**采集循环开始之前**就填好：ONVIF 的 GetProfiles 可能在这之前
  // 就被 NVR 调到了（Web 服务比采集先起），而那一次回答里的分辨率得是真的。
  stats_.video_width = w;
  stats_.video_height = h;
  // 立刻推一帧出去，别等第一个整秒。这一帧里除尺寸外全是 0，那是**真实的**
  // —— 此刻确实一帧都没编。让它等一秒的代价是：NVR 恰好在这一秒里问
  // GetProfiles，就会拿到一个默认的 1920x1080，而那是编造的。
  if (stats_hook_) stats_hook_(stats_);

  // --- 3. 编码器 ----------------------------------------------------------
  EncoderConfig ec;
  ec.width = w;
  ec.height = h;
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
  if (!encoder.open(ec, err)) return false;

  // --- 3b. 子码流编码器 ---------------------------------------------------
  //
  // 只有开了 RTSP 才建。子码流存在的唯一理由是给 NVR 的多画面网格用，而那条
  // 路走的是 RTSP —— 只录文件或者只开 Web 的时候多编一路纯粹的浪费。
  //
  // **尺寸按高度给、宽度按宽高比推**：NVR 界面是按高度分档的（D1/720p/360p），
  // 而写死宽度在 16:10、21:9 这些比例下会把画面拉变形。
  X264Encoder sub_encoder;
  bool sub_on = opt.sub_enabled && opt.rtsp.enabled;
  int sub_w = 0;
  int sub_h = 0;
  if (sub_on) {
    sub_h = opt.sub_height > 0 ? opt.sub_height : 360;
    // 取偶：YUV420P 的色度平面是 2x2 子采样，奇数边长 x264 直接拒绝
    // （报 "width not divisible by 2"）。四舍五入到最近的偶数。
    sub_h = (sub_h + 1) / 2 * 2;
    long const want_w = (static_cast<long>(w) * sub_h + h / 2) / h;
    sub_w = static_cast<int>((want_w + 1) / 2 * 2);

    if (sub_w > 0 && sub_h > 0 && (sub_w < w || sub_h < h)) {
      EncoderConfig sc = ec;
      sc.width = sub_w;
      sc.height = sub_h;
      sc.bitrate_kbps = opt.sub_bitrate_kbps;
      // 子码流的线程数**单独封顶**：它的像素只有主码流的几十分之一，按核数
      // 开线程纯属浪费，还会和主码流抢 CPU。
      sc.threads = std::min(opt.threads > 0 ? opt.threads : 6, 2);
      if (!sub_encoder.open(sc, err)) return false;
      D2O_INFO("子码流已启用：%dx%d（%.0f kbps）", sub_w, sub_h,
               static_cast<double>(opt.sub_bitrate_kbps));
    } else {
      // 尺寸没比主码流小 —— 再编一遍同样的画面没有任何意义。说清楚为什么
      // 没开，而不是安静地少一路（那会被当成"子码流坏了"）。
      D2O_WARN("子码流高度 %d 在主码流 %dx%d 下推不出更小的尺寸，已跳过子码流",
               opt.sub_height, w, h);
      sub_on = false;
    }
  }
  stats_.sub_width = sub_on ? sub_w : 0;
  stats_.sub_height = sub_on ? sub_h : 0;

  // --- 4. 下游 ------------------------------------------------------------
  Mp4Writer writer;
  Mp4Consumer mp4;
  if (!opt.output_path.empty()) {
    if (!writer.open(opt.output_path, encoder.codec_context(), err)) return false;
    mp4.writer = &writer;
  }

  RtspServer rtsp;
  RtspConsumer rtsp_consumer;
  RtspConsumer rtsp_sub_consumer;
  if (opt.rtsp.enabled) {
    // 每路一份 extradata，顺序和 streams 一致。**两路的 SPS/PPS 是不同的**
    // （分辨率不同），所以不能拿主码流那份去喂子码流 —— 那样 SDP 里报的
    // 分辨率是对的、解出来的画面却是错的。
    std::vector<std::vector<std::uint8_t>> extras;
    extras.push_back(encoder.extradata());
    if (sub_on) extras.push_back(sub_encoder.extradata());

    if (!rtsp.start(opt.rtsp, extras, err)) return false;

    rtsp_consumer.server = &rtsp;
    rtsp_consumer.stream = 0;
    if (sub_on) {
      rtsp_sub_consumer.server = &rtsp;
      rtsp_sub_consumer.stream = 1;
    }

    // URL 要等 Live555 挑完本机 IP 才知道，所以在这里就取出来填进统计 ——
    // 采集还没开始，但端口和地址已经定了。
    stats_.rtsp_streams.resize(rtsp.stream_count());
    for (std::size_t i = 0; i < rtsp.stream_count(); ++i) {
      stats_.rtsp_streams[i].path = opt.rtsp.streams[i].path;
      stats_.rtsp_streams[i].url = rtsp.url(i);
      D2O_INFO("RTSP 第 %zu 路已就绪：%s", i, stats_.rtsp_streams[i].url.c_str());
    }
  }
  // 停机交给这个守卫：不管从哪条路径退出，都会在**编码线程 join 之后**
  // （见下面第 6 步）把 RTSP 收掉。顺序反了就是往正在析构的对象上打事件。
  struct RtspGuard {
    RtspServer* server;
    ~RtspGuard() {
      if (server) server->stop();
    }
  } rtsp_guard{opt.rtsp.enabled ? &rtsp : nullptr};

  // 浏览器预览。封装器**在这里就 init**，而不是等第一个客户端连上来：
  // extradata 只有编码器开着的时候才有，而初始化段要跟分片同线程按顺序发
  // （见 PreviewFeed::begin），在采集线程里现 init 就得处理"init 赶不上
  // 第一帧"的竞态。提前准备好就完全没有这个问题。
  PreviewFeed preview;
  PreviewConsumer preview_consumer;
  if (preview_hook_) {
    if (!preview.muxer.init(w, h, encoder.extradata(), opt.fps)) {
      // 不是致命错误：录制、RTSP、ONVIF 都不受影响，坏的只有浏览器预览。
      D2O_WARN("fMP4 封装器初始化失败，浏览器预览不可用（录制与 RTSP 不受影响）。");
    } else {
      preview.fps = opt.fps;
      preview.hook = preview_hook_;
      preview_consumer.feed = &preview;
    }
  }

  // **顺序即语义**：只读的下游在前，会拿走包的下游在后。
  PacketTap tap;
  if (opt.rtsp.enabled) tap.consumers.push_back(&rtsp_consumer);
  if (preview_consumer.feed) tap.consumers.push_back(&preview_consumer);
  if (!opt.output_path.empty()) tap.consumers.push_back(&mp4);

  // 子码流**只有 RTSP 一个下游**，所以它自己的 tap 里只有一个消费者。
  // 也正因如此它不会碰主码流的计数器 —— 两个 tap 各绑各的原子量。
  PacketTap sub_tap;
  if (sub_on) sub_tap.consumers.push_back(&rtsp_sub_consumer);

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
  int recover_attempts = 0;
  bool warned_pool = false;
  bool warned_no_content = false;
  bool logged_cursor = false;
  bool fatal = false;
  SteadyClock::time_point next_report = start + std::chrono::seconds(1);

  while (!stop_.load(std::memory_order_relaxed) &&
         !g_interrupted.load(std::memory_order_relaxed)) {
    if (pipeline.failed.load(std::memory_order_relaxed)) break;
    if (opt.seconds > 0 &&
        tick >= static_cast<std::uint64_t>(opt.seconds) *
                    static_cast<std::uint64_t>(opt.fps)) {
      break;
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

    BgraFrame* buf = pool->take();
    if (!buf) {
      // 池子空了：编码 + 写出已经落后一整个池子的量。跳过这一帧。
      // 注意 tick 照样要推进 —— 否则时间轴会被拉长。
      ++stats_.pool_exhausted;
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
      SteadyClock::time_point const now = SteadyClock::now();
      if (now >= deadline) break;

      auto const remain =
          std::chrono::duration_cast<milliseconds>(deadline - now).count();
      auto const timeout = static_cast<std::uint32_t>(
          std::min<std::int64_t>(std::max<std::int64_t>(remain, 1), 100));

      bool changed = false;
      std::string aerr;
      DesktopCapture::Status const st = capture.acquire(timeout, *buf, changed, aerr);

      if (st == DesktopCapture::Status::Ok) {
        got_content = changed;
        recover_attempts = 0;
      } else if (st == DesktopCapture::Status::Timeout) {
        continue;  // 桌面没动，看看这一拍剩下的时间里会不会动
      } else if (st == DesktopCapture::Status::AccessLost) {
        ++stats_.access_lost;
        ++recover_attempts;
        if (recover_attempts > kMaxRecoverAttempts) {
          err = fmt("捕获会话连续 %d 次重建失败，放弃", kMaxRecoverAttempts);
          fatal = true;
          break;
        }
        // 切换分辨率/锁屏之后立刻重建多半会再失败一次，退让一下。
        std::this_thread::sleep_for(
            milliseconds(std::min(200 * recover_attempts, 1000)));
        std::string rerr;
        if (!capture.reopen(rerr)) {
          D2O_WARN("重建捕获会话失败（第 %d/%d 次）：%s", recover_attempts,
                   kMaxRecoverAttempts, rerr.c_str());
          continue;
        }
        D2O_INFO("捕获会话已重建（累计 %llu 次）",
                 static_cast<unsigned long long>(stats_.access_lost));
        if (capture.width() != w || capture.height() != h) {
          err = fmt("录制中途分辨率从 %dx%d 变成了 %dx%d。"
                    "流里换分辨率需要重开会话（Phase 6 的内容）",
                    w, h, capture.width(), capture.height());
          fatal = true;
          break;
        }
        recover_attempts = 0;
      } else {
        err = aerr;
        fatal = true;
        break;
      }
    }
    if (fatal) break;

    if (got_content) {
      buf->content_id = ++content_id;
      last = pool->publish(buf);  // shared_ptr<BgraFrame> → shared_ptr<const BgraFrame>
      ++stats_.content_frames;
      // 第一张带指针的画面出来时报一次形状/位置。**只报一次**：指针每动一下
      // 都报的话就是每秒几十行，而这里要回答的只是"它到底合成上了没有、
      // 合成的是什么形状"。
      if (!logged_cursor && capture.composited_frames() > 0) {
        logged_cursor = true;
        D2O_INFO("鼠标指针已合成进画面：%s", capture.pointer_description().c_str());
      }
    } else {
      // 画面没变：缓冲原样还回池子，这一拍复用上一帧（不重新拷贝像素）。
      std::shared_ptr<BgraFrame> recycle = pool->publish(buf);
      recycle.reset();
    }

    if (last) {
      stats_.queue_dropped += queue.push(CapturedFrame{last, tick});
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
      std::string tail;
      // 逐路打。**入队/丢弃必须分开看**：只报丢弃时，"每秒丢 500 个"既可能是
      // 编码器产出过快，也可能是消费端根本不拉，分不清方向（见陷阱 26）。
      for (std::size_t i = 0; i < rtsp.stream_count(); ++i) {
        tail += fmt("  推流[%zu] 观众 %u  队列 %zu/%zu 入队 %llu 丢弃 %llu",
                    i, rtsp.client_count(i), rtsp.queue_size(i),
                    rtsp.queue_capacity(i),
                    static_cast<unsigned long long>(rtsp.nalu_pushed(i)),
                    static_cast<unsigned long long>(rtsp.nalu_dropped(i)));
      }
      D2O_INFO("%5.1fs  已编码 %llu 帧（%.1f fps）  %.0f kbps  队列 %zu/%zu "
               "丢弃 %llu  缓存池空闲 %zu/%zu%s",
               elapsed, static_cast<unsigned long long>(pk),
               elapsed > 0 ? static_cast<double>(pk) / elapsed : 0.0,
               elapsed > 0 ? static_cast<double>(by) * 8.0 / elapsed / 1000.0 : 0.0,
               queue.size(), queue.capacity(),
               static_cast<unsigned long long>(stats_.queue_dropped),
               pool->available(), pool->total(), tail.c_str());

      // 顺手把实时字段填进 stats_，再交给 Web 层的钩子。
      // 填在**这里**而不是等收尾：不填的话，运行期间 `stats()` 里的
      // packets/bytes 一直是 0，Web 上就是"帧率 0、码率 0"而画面明明是通的。
      stats_.elapsed_s = elapsed;
      stats_.packets = pk;
      stats_.keyframes = pipeline.keyframes.load(std::memory_order_relaxed);
      stats_.bytes = by;
      stats_.effective_fps = elapsed > 0 ? static_cast<double>(pk) / elapsed : 0.0;
      stats_.cursor_frames = capture.composited_frames();
      stats_.pointer_only_frames = capture.pointer_only_frames();
      if (opt.rtsp.enabled) fill_rtsp_stats(stats_, rtsp, pipeline);
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
  stats_.cursor_frames = capture.composited_frames();
  stats_.pointer_only_frames = capture.pointer_only_frames();

  if (pipeline.failed.load(std::memory_order_relaxed)) {
    // 编码线程挂了。已经把写出来的部分收尾成一个能播的文件，但整体算失败。
    std::string const werr = pipeline.error;
    std::string ignored;
    if (!opt.output_path.empty()) writer.close(ignored);
    // 这两条失败路径跳过了下面的冲刷，也就是跳过了那次 fill_rtsp_stats。
    // 在这里补一次：出错时"到底推出去多少、有没有观众"恰好是最需要知道的，
    // 而漏掉它会让收尾报表上每一路的数字全是 0。
    if (opt.rtsp.enabled) fill_rtsp_stats(stats_, rtsp, pipeline);
    err = fmt("编码线程出错：%s（已写出的部分仍会保存）", werr.c_str());
    return false;
  }
  if (fatal) {
    // 采集侧出错，err 里已经是原因了。同样把已写出的部分收尾。
    std::string ignored;
    if (!opt.output_path.empty()) writer.close(ignored);
    if (opt.rtsp.enabled) fill_rtsp_stats(stats_, rtsp, pipeline);
    return false;
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

  // 放在冲刷**之后**：顶层 packets/bytes 到这里才是最终值，而 fill_rtsp_stats
  // 对主码流那一路就是照抄它们。早一步调用，两处会差着冲刷的那几帧。
  if (opt.rtsp.enabled) fill_rtsp_stats(stats_, rtsp, pipeline);

  if (!opt.output_path.empty() && !writer.close(err)) return false;

  stats_.effective_fps = stats_.elapsed_s > 0
                             ? static_cast<double>(stats_.packets) / stats_.elapsed_s
                             : 0.0;
  return true;
}

}  // namespace d2o
