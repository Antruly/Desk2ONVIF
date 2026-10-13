#pragma once

/**
 * @file src/rtsp/au_queue.h
 * @brief 音频 AU（裸 AAC 帧）从编码线程到 Live555 调度线程的队列。
 *
 * 它是 `NaluQueue` **减去视频专有的三个概念**之后的形状，三个都是"音频不需要"
 * 而不是"暂时没做"：
 *
 *   - **没有"等关键帧"**（`waiting_key_`）。AAC 的每一个 AU 都是独立可解的
 *     —— AudioSpecificConfig 通过 SDP 的 `a=fmtp:…config=` 给全了，不存在
 *     "从半个 GOP 中间开始解不出画面"那回事。所以新连上的客户端从**下一个** AU
 *     就能出声，不必等到下一个 IDR。
 *   - **没有参数集插入**。视频要把 SPS/PPS 在带内重发（部分 NVR 丢 SDP），
 *     音频对应的是 SDP 里那两字节，带内没有对应的东西。
 *   - **没有访问单元的概念**。一个 AAC 包就是一个 AU（编码器固定一帧一包），
 *     不存在"一帧被切成多个 VCL NALU"那种需要在多个包里判断"这一帧完了没有"
 *     的问题。
 *
 * 刻意**不**把 `NaluQueue` 改造成"带一个音频开关的通用队列"：那样上面这三条会
 * 变成三个要在两处同时想明白的分支，而两边真正共用的只有"有界 + 丢最旧 + armed"
 * 这几行。分开写反而更短，也更好读。
 *
 * `armed` 的语义和视频那一路一致（有没有人在看）。**音频这一路的 armed 是
 * 独立的一份**：NVR 只 SETUP 视频时不武装音频队列，于是编码线程那份音频数据
 * 连拷贝都不会做。
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace d2o {

/// 一个访问单元 = `AacEncoder` 吐出的一个 AVPacket 的**全部内容**。
///
/// **不带 ADTS 头**（陷阱 76）：FFmpeg 的 aac 编码器给的就是裸 AU，Live555 的
/// `MPEG4GenericRTPSink` 要的也是裸 AU。中间谁"顺手"套一层 ADTS，收端解出来
/// 就是噪音，而 **SDP 完全正常、ffprobe 也报得出正确的采样率和声道数** ——
/// 这类故障只能靠把流拉下来解码（听或者量）才发现。
struct Au {
  std::vector<std::uint8_t> data;
  /// 单位是**样本**（时基 1/sample_rate），直接来自 `AVPacket::pts`。
  /// ⚠️ 首包的 pts 是 **−1024**（编码器的启动填充），不是 0 —— 见 aac_encoder.h。
  /// 呈现时间按**差值**算，所以负起点无害（AacLiveSource 里有说明）。
  std::int64_t pts = 0;
};

class AuQueue {
 public:
  explicit AuQueue(std::size_t capacity)
      : capacity_(capacity ? capacity : 1) {}

  AuQueue(AuQueue const&) = delete;
  AuQueue& operator=(AuQueue const&) = delete;

  /// 生产者：编码线程。返回本次因此丢弃的旧 AU 数。
  std::size_t push(Au&& au) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!armed_) return 0;

    if (au.data.size() > max_au_) max_au_ = au.data.size();

    std::size_t dropped = 0;
    while (q_.size() >= capacity_) {
      q_.pop_front();
      ++dropped;
    }
    q_.push_back(std::move(au));
    dropped_ += dropped;
    ++pushed_;
    return dropped;
  }

  /// 消费者：Live555 调度线程。
  bool pop(Au& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  /// 切换"有没有人在看"。两个方向都清空队列：变成有人看时要丢掉连接之前的
  /// （新客户端不该收到旧声音），变成没人看时留着只是占内存。
  ///
  /// 丢了中间几个 AU 之后时间轴**不会**被压缩 —— 呈现时间由 pts 差值算出来，
  /// 丢包表现为时间轴上多出一个跳变（收端补一段静音或者咔一下），这比"把声音
  /// 挤在一起"诚实得多。
  void set_armed(bool armed) {
    std::lock_guard<std::mutex> lk(mu_);
    if (armed_ == armed) return;
    armed_ = armed;
    q_.clear();
  }

  bool armed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return armed_;
  }

  void clear() {
    std::lock_guard<std::mutex> lk(mu_);
    q_.clear();
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
  }
  std::size_t capacity() const { return capacity_; }
  std::uint64_t dropped() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dropped_;
  }

  /// 累计入队成功（= 被武装期间真的收下的）AU 数。和 `dropped()` 是一对，
  /// 理由和 `NaluQueue::pushed()` 完全一样：只有一个"丢了多少"的数字时分不清
  /// "产出过快"和"消费端不拉"。
  std::uint64_t pushed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pushed_;
  }

  /// 见过的最大 AU 字节数。AAC 在 64 kbps 下一帧约 200 字节，这个数只是用来
  /// 佐证"没有从中途混进视频包"（真混进来的话它会突然变成几十万）。
  std::size_t max_au_bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return max_au_;
  }

 private:
  mutable std::mutex mu_;
  std::deque<Au> q_;
  std::size_t const capacity_;
  bool armed_ = false;
  std::uint64_t dropped_ = 0;
  std::uint64_t pushed_ = 0;
  std::size_t max_au_ = 0;
};

}  // namespace d2o
