#pragma once

/**
 * @file src/rtsp/rtsp_server.h
 * @brief Live555 RTSP 服务器 + 自己的调度线程。
 *
 * 线程模型：
 *
 *   [编码线程]  push_annex_b()  —— 切分 NALU、插参数集、入队、唤醒
 *        │  NaluQueue（有界，drop-oldest）
 *        ▼
 *   [Live555 调度线程]  BasicTaskScheduler::doEventLoop()
 *        H264LiveSource::doGetNextFrame()  ← 客户端拉的时候才被调用
 *        EventTrigger 唤醒 ← 编码线程唯一能碰的东西
 *
 * **停机顺序是有语义的**：必须先让编码线程停掉并 join，再调 stop()。
 * Live555 的对象全是单线程的，stop() 会在调用方线程上析构
 * RTSPServer / ServerMediaSession / H264LiveSource，这时候编码线程要是还在
 * push，就是往一个正在析构的对象上打 triggerEvent。CaptureSession 里
 * 保证了这一点（先 join 再 stop）—— 改那边的时候要留意。
 */

#include "rtsp/nalu.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class UsageEnvironment;
class TaskScheduler;
class RTSPServer;
class ServerMediaSession;

namespace d2o {

class NaluQueue;
class H264LiveSource;

/// 一路码流的参数。
///
/// 主码流和子码流共用这一个结构：除了路径、码率和队列深度，两者没有任何区别
/// —— 编出它们的是两个各自独立的编码器，推给客户端的是两条各自独立的
/// ServerMediaSession。刻意**不做**成"主码流 + 可选子码流"两个不同的形状，
/// 是因为那样每处代码都要分叉成"主怎么走、子怎么走";做成数组之后，
/// 加第三路（比如给手机看的第三档）不需要动任何一处逻辑。
struct RtspStreamOptions {
  std::string path = "live";
  unsigned fps = 30;
  /// 进 SDP 的带宽估计（`b=AS:`）和 Live555 的 estBitrate。**不影响编码**
  /// —— 实际码率由编码器决定，这个值只用来告诉客户端"大概要多少带宽"。
  int bitrate_kbps = 4000;
  /// 队列深度（按 NALU 计）。90 个约等于 3 秒 @30fps —— 一帧一个 VCL NALU，
  /// 加上每个关键帧前插的两个参数集。
  std::size_t queue_capacity = 90;
};

struct RtspOptions {
  bool enabled = false;
  std::uint16_t port = 554;
  /// **至少一路，而且顺序即语义**：`streams[i]` 对应编码器 i，
  /// `push_annex_b(i, …)` 就是按这个下标找路的。主码流在最前，子码流在后。
  ///
  /// 空数组 + enabled=true 是调用方的记账错误（编码器不知道往哪推），
  /// `start()` 会直接判失败而不是宽容地什么都不做 —— 后者表现为
  /// "NVR 连得上、没有画面"，是最难查的一类症状。
  std::vector<RtspStreamOptions> streams;
};

class RtspServer {
 public:
  /// **构造函数不能在类里写 `= default`。** 类里 default 的那个版本会当场
  /// 计算异常规格，于是要实例化每个成员的析构函数 —— 而 `streams_` 里的
  /// `Stream` 在这里还是不完整类型（它定义在 .cpp 里，那样才不用把
  /// ServerMediaSession / NaluQueue 的头文件拖进这个头）。GCC 报的是一大串
  /// `unique_ptr.h` 里的 `sizeof` 不完整类型，看不出和构造函数有关。
  /// 挪到 .cpp 里定义，那里 `Stream` 已经完整了。
  RtspServer();
  ~RtspServer();

  RtspServer(RtspServer const&) = delete;
  RtspServer& operator=(RtspServer const&) = delete;

  /// extradata 每路一份 Annex-B SPS+PPS，顺序必须和 opt.streams 一致。
  /// **每一份都必须非空** —— SDP 里的 `sprop-parameter-sets` 靠它，
  /// 缺了那一路的 NVR 加进来就是黑屏。
  bool start(RtspOptions const& opt,
             std::vector<std::vector<std::uint8_t>> const& extradata,
             std::string& err);

  /// 见文件头的停机顺序说明：调用前编码线程必须已经停了。
  void stop();

  bool running() const { return running_.load(std::memory_order_relaxed); }

  /// 起了几路。start() 之后 = opt.streams.size()。
  std::size_t stream_count() const { return streams_.size(); }

  /// **编码线程**调用。data 是一个 AVPacket 的内容（Annex-B，带起始码）。
  /// 关键帧会在 IDR 前自动补发 SPS/PPS。
  ///
  /// 下标越界是**静默忽略**（不崩、不报错）：这条路在编码线程上，而它唯一的
  /// 失败方式是调用方把编码器和流的对应关系搞错了 —— 那种错在启动时就该
  /// 被发现，不该等到录到一半从编码线程上去抛。
  void push_annex_b(std::size_t stream, std::uint8_t const* data,
                    std::size_t size, bool keyframe, std::int64_t pts);

  /// 实际可用的播流地址（含 Live555 自己挑的本机 IP 和真实端口）。
  std::string url(std::size_t stream) const;

  /// 某一路的观众数。
  unsigned client_count(std::size_t stream) const;
  /// 所有路加起来的观众数。
  unsigned client_count() const;
  std::size_t queue_capacity(std::size_t stream) const;
  std::size_t queue_size(std::size_t stream) const;
  std::uint64_t nalu_dropped(std::size_t stream) const;
  /// 累计入队 NALU 数。和 nalu_dropped() 一起看才能分清"产出过快"和"没人消费"。
  std::uint64_t nalu_pushed(std::size_t stream) const;
  std::size_t max_nalu_bytes(std::size_t stream) const;
  std::size_t sps_size(std::size_t stream) const;
  std::size_t pps_size(std::size_t stream) const;

 private:
  /// 一路码流的全部状态。定义在 .cpp 里 —— 它要碰 ServerMediaSession 和
  /// NaluQueue，而那些头文件没有理由出现在这里。
  struct Stream;

  void teardown();
  /// 越界返回 nullptr。所有按路取数的接口都过它，于是"下标写错"这件事
  /// 只有一处需要处理。const 与非 const 两个重载的差别就是"要不要往里写"。
  Stream* at(std::size_t stream);
  Stream const* at(std::size_t stream) const;

  RtspOptions opt_{};
  std::vector<std::unique_ptr<Stream>> streams_;

  /// 切分用的暂存，避免每帧都分配。只在编码线程上碰。
  /// 两路共用一份是安全的：push 是串行的，每次进来从头填。
  std::vector<Nalu> scratch_;

  TaskScheduler* scheduler_ = nullptr;
  UsageEnvironment* env_ = nullptr;
  RTSPServer* server_ = nullptr;

  /// doEventLoop 的停止标志。类型必须和 Live555 的 EventLoopWatchVariable 一致，
  /// .cpp 里有 static_assert 盯着。
  std::atomic_char watch_{0};

  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace d2o
