#pragma once

/**
 * @file src/common/bounded_queue.h
 * @brief 有界队列，满了丢最旧的（drop-oldest）。
 *
 * 为什么是丢最旧的而不是阻塞生产者：采集端一旦被阻塞，DXGI 那边积累的帧会让
 * AcquireNextFrame 一直拿到旧帧，而且桌面复制会话持有时间变长，最终更容易
 * 走到 ACCESS_LOST 重建 —— 代价比丢一帧大得多。屏幕录制里"丢掉过期的帧、
 * 保留最新的帧"本来就是正确策略：看直播的人宁可画面跳一下，也不要延迟累积。
 *
 * 编码与推流解耦也靠它：Phase 2 起编码线程和 Live555 调度线程之间是同一个
 * 结构，只是元素类型不同。
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

namespace d2o {

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : cap_(capacity ? capacity : 1) {}

  BoundedQueue(BoundedQueue const&) = delete;
  BoundedQueue& operator=(BoundedQueue const&) = delete;

  /// 入队。队列满时丢掉最旧的一个，返回本次丢掉的个数（0 或 1）。
  std::size_t push(T v) {
    std::size_t dropped = 0;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (closed_) return 0;  // 已经停了，静默丢弃：调用方多半正在退出
      while (q_.size() >= cap_) {
        q_.pop_front();
        ++dropped;
        ++total_dropped_;
      }
      q_.push_back(std::move(v));
    }
    cv_.notify_one();
    return dropped;
  }

  /// 等一个元素。取到了返回 true；超时、或队列已关闭且取空，返回 false。
  bool pop(T& out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, timeout, [this] { return !q_.empty() || closed_; });
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  /// 关掉：唤醒所有等待者，之后 pop 要么取到剩余元素，要么立刻返回 false。
  void close() {
    {
      std::lock_guard<std::mutex> lk(m_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lk(m_);
    return closed_;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size();
  }

  std::size_t capacity() const { return cap_; }

  /// 累计丢弃数。这是判断"编码是否跟不上采集"的唯一硬指标。
  std::uint64_t total_dropped() const {
    std::lock_guard<std::mutex> lk(m_);
    return total_dropped_;
  }

 private:
  std::size_t const cap_;
  std::deque<T> q_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  bool closed_ = false;
  std::uint64_t total_dropped_ = 0;
};

}  // namespace d2o
