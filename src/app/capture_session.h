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

#include "capture/frame.h"
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

struct SessionOptions {
  unsigned display_index = 0;   ///< --list-displays 里的序号
  int fps = 30;
  int bitrate_kbps = 4000;
  int gop_seconds = 2;
  /// 编码线程数。<= 0 交给编码器按"延迟优先"自动选（见 EncoderConfig::threads）。
  int threads = 0;
  std::size_t queue_capacity = 4;
  std::size_t pool_size = 6;    ///< 帧缓冲池，要 > 队列深度 + 1
  std::string preset = "ultrafast";
  std::string tune = "zerolatency";
  std::string profile = "main";

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

  RtspOptions rtsp;             ///< rtsp.enabled 控制是否起 RTSP 服务
};

struct SessionStats {
  double elapsed_s = 0.0;
  std::uint64_t ticks = 0;            ///< 按目标帧率应该产出的帧数
  std::uint64_t content_frames = 0;   ///< 其中画面真的变了的
  std::uint64_t queue_dropped = 0;    ///< 编码跟不上而丢掉的
  std::uint64_t pool_exhausted = 0;   ///< 池子空而跳过采集的次数
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
  RunResult run(SessionOptions const& opt, std::string& err);

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

  /// 线程安全。Ctrl+C 的处理函数会调它。
  void request_stop() { stop_.store(true, std::memory_order_relaxed); }
  bool stop_requested() const { return stop_.load(std::memory_order_relaxed); }

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
  SessionStats stats_{};
  StatsHook stats_hook_;
  PreviewHook preview_hook_;

  /// 快照留档。采集线程写（每秒一次），web 循环线程读，所以要一把锁。
  /// 锁的临界区只有一个 shared_ptr 的赋值/拷贝 —— **绝不要在持锁时去编码
  /// JPEG**，那会把采集循环堵住。
  mutable std::mutex snapshot_mu_;
  std::shared_ptr<BgraFrame const> snapshot_;
};

}  // namespace d2o
