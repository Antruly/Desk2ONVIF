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

  /// pop() 的三种结局。**必须是三种，不能是 bool。**
  ///
  /// 原先这里返回 bool，"等超时了队列还空着"和"队列已经关了"是同一个 false。
  /// 消费端拿它当循环条件（`while (q.pop(f, 200ms))`）时，两者就变成了同一件
  /// 事 —— 而它们的意思正好相反：一个说"暂时没数据，接着等"，一个说"永远没
  /// 数据了，收工"。
  ///
  /// 后果不是"偶尔少一帧"，是**整条管线静默死掉**：采集侧只要停顿超过超时
  /// （切分辨率/锁屏/系统卡顿都会，实测是 DXGI 重建会话时那 200 ms 退让），
  /// 编码线程就把"队列空了一下"当成"该退出了"，于是不设错误标志、不打一行
  /// 日志地退出循环。之后采集继续往一个没人读的队列里丢帧，进程照常跑到
  /// 时间结束，收尾报表和退出码全是成功 —— 实测：26 秒的录制只写出 5.8 秒的
  /// 文件、175 个包、RTSP 一个 NALU 都没有，退出码 0。
  ///
  /// 所以调用方**只应把 Closed 当停机**，Timeout 一律 continue。
  enum class PopStatus {
    Got,      ///< 拿到了，out 有效
    Timeout,  ///< 超时且队列仍空 —— 生产端只是暂时没东西
    Closed,   ///< 队列已关闭且已取空 —— 这才是停机信号
  };

  /// 等一个元素。取到了返回 Got；超时返回 Timeout；队列已关闭且取空返回
  /// Closed。**后两者不要混为一谈**，见 PopStatus 的说明。
  PopStatus pop(T& out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, timeout, [this] { return !q_.empty() || closed_; });
    if (q_.empty()) return closed_ ? PopStatus::Closed : PopStatus::Timeout;
    out = std::move(q_.front());
    q_.pop_front();
    return PopStatus::Got;
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
