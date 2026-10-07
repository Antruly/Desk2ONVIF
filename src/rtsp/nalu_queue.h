#pragma once

/**
 * @file src/rtsp/nalu_queue.h
 * @brief 编码线程 → Live555 调度线程的单向有界队列。
 *
 * 这是把两条线程解耦的全部内容。Live555 是**拉模型且自身没有队列** ——
 * 客户端什么时候要数据，由 sink 的节奏和 RTCP 反馈决定，不由我们决定。
 * 所以缓冲必须自己维护，队列就是那个缓冲。
 *
 * 满了丢**最老的**（drop-oldest）：直播里延迟比完整性重要，而且丢掉的都是
 * 已经过时的画面。代价要说清楚 —— 丢掉中间几个 NALU 之后解码器会一直出花屏
 * 到下一个 IDR，所以 `--gop` 别设太大，而且每个 IDR 前都要重发 SPS/PPS。
 *
 * `armed` 是"有没有人在看"。没人连的时候 push 直接丢弃，免得白占内存。
 * 从没人看到有人看的瞬间会进入"等关键帧"状态：新连上的解码器从半个 GOP
 * 中间开始是解不出画面的，与其喂它一堆花屏，不如静默到下一个 IDR
 * （最多等一个 GOP，默认 2 秒，客户端完全接受）。
 */

#include "rtsp/nalu.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

namespace d2o {

class NaluQueue {
 public:
  explicit NaluQueue(std::size_t capacity)
      : capacity_(capacity ? capacity : 1) {}

  NaluQueue(NaluQueue const&) = delete;
  NaluQueue& operator=(NaluQueue const&) = delete;

  /// 生产者：编码线程。返回本次因此丢弃的旧 NALU 数。
  std::size_t push(Nalu&& n) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!armed_) return 0;

    if (waiting_key_) {
      // 等到参数集或 IDR 才开始收。SPS 一定排在 IDR 前面（是我们在
      // RtspServer 里插的），所以从 SPS 开始收正好是完整的解码起点。
      if (!n.is_sps() && !n.is_idr()) return 0;
      waiting_key_ = false;
    }

    if (n.data.size() > max_nalu_) max_nalu_ = n.data.size();

    std::size_t dropped = 0;
    while (q_.size() >= capacity_) {
      q_.pop_front();
      ++dropped;
    }
    q_.push_back(std::move(n));
    dropped_ += dropped;
    ++pushed_;
    return dropped;
  }

  /// 消费者：Live555 调度线程。
  bool pop(Nalu& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  /// 切换"有没有人在看"。两个方向都会清空队列：变成有人在看时要丢掉旧的
  /// （新客户端不该收到连接之前的画面），变成没人看时留着也只是占内存。
  void set_armed(bool armed) {
    std::lock_guard<std::mutex> lk(mu_);
    if (armed_ == armed) return;
    armed_ = armed;
    waiting_key_ = armed;
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

  /// 累计入队成功（= 被武装期间真的收下的）NALU 数。
  ///
  /// 这个数和 dropped() 是一对：dropped 说的是"丢了多少"，它说的是"来了多少"。
  /// 只有 dropped 的时候，一个"每秒丢 500 个"的数字既可能是编码器产出异常，
  /// 也可能是队列根本没被消费 —— 加上它就能一眼分开。实测中正是靠它定位了
  /// 消费端不拉取的故障。
  std::uint64_t pushed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pushed_;
  }

  /// 见过的最大 NALU 字节数。这个数是有用的诊断：Live555 的
  /// `H264or5Fragmenter` 把整个 NALU 先收进一个 `OutPacketBuffer::maxSize`
  /// 大小的缓冲，超了就被**截断**（画面破但不报错）。所以这个值必须始终
  /// 明显小于我们设的 `OutPacketBuffer::maxSize`。
  std::size_t max_nalu_bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return max_nalu_;
  }

 private:
  mutable std::mutex mu_;
  std::deque<Nalu> q_;
  std::size_t const capacity_;
  bool armed_ = false;
  bool waiting_key_ = false;
  std::uint64_t dropped_ = 0;
  std::uint64_t pushed_ = 0;
  std::size_t max_nalu_ = 0;
};

}  // namespace d2o
