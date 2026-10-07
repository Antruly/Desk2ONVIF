#pragma once

/**
 * @file src/capture/frame.h
 * @brief 采集线程交给编码线程的那一帧的表示。
 *
 * 为什么不是"一个 vector + 一把锁"：采集线程和编码线程要能真正并行 —— 采集
 * 线程往缓冲 A 里写下一帧的时候，编码线程正在读缓冲 B。共享指针的引用计数天然
 * 表达这件事，不需要编解码双方去协商谁在用哪块内存。
 *
 * 另一个要点是 content_id：桌面静止时（绝大多数时间）画面根本没变，但为了让
 * NVR 和播放器看到的是恒定帧率的流，我们**仍然要按目标帧率产出帧**。这时不
 * 重新拷贝像素、也不重新做一次 BGRA→YUV 转换，只把同一个 image 再入队一次，
 * content_id 不变 —— 编码线程看到 id 没变就复用上次的 YUV 缓冲。
 * 省下的是每帧 8 MB 的拷贝和一次全画面色彩转换。
 */

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace d2o {

/// 一帧 BGRA 桌面图像。BGRA 是 DXGI 桌面复制唯一给的格式
/// （DXGI_FORMAT_B8G8R8A8_UNORM），不做任何转换直接收下来。
struct BgraFrame {
  int width = 0;                       ///< 像素
  int height = 0;                      ///< 像素
  int stride = 0;                      ///< 每行字节数，可能 > width*4
  std::vector<std::uint8_t> pixels;    ///< size >= stride * height
  std::uint64_t content_id = 0;        ///< 内容版本号；像素没变时不变

  std::size_t byte_size() const {
    return static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
  }
};

/// 队列里传的元素。
///
/// image 可以跨多个 tick 复用（桌面静止时是常态），tick 则是每帧递增的序号 ——
/// 时间戳必须由 tick 决定而不是由"第几次拿到新内容"决定，否则静止的桌面会
/// 让 MP4 的时间轴停在原地，播放器看到的是 0 秒的片段。
struct CapturedFrame {
  std::shared_ptr<const BgraFrame> image;
  std::uint64_t tick = 0;
};

/// 定长的 BGRA 帧缓冲池。
///
/// 存在的理由：1080p 一帧是 8 MB，@30fps 就是 240 MB/s 的分配/释放。
/// 池子里固定几块缓冲循环用，稳态下零分配。
///
/// 池子空了的处理方式是**跳过这一帧**，不是等待 —— 等待等于把编码的延迟
/// 传回采集线程，正是前面说要避免的事。
///
/// 必须用 std::shared_ptr<FramePool> 持有：publish() 出来的帧的 deleter 持有
/// 池子的一份引用，保证"采集线程已经退出、编码线程还在处理最后一帧"的时序安全。
class FramePool : public std::enable_shared_from_this<FramePool> {
 public:
  FramePool(int width, int height, std::size_t count)
      : w_(width), h_(height), count_(count ? count : 1) {
    build();
  }

  FramePool(FramePool const&) = delete;
  FramePool& operator=(FramePool const&) = delete;

  /// 取一块空闲缓冲。池子空了返回 nullptr。
  BgraFrame* take() {
    std::lock_guard<std::mutex> lk(m_);
    if (free_.empty()) return nullptr;
    BgraFrame* f = free_.back();
    free_.pop_back();
    return f;
  }

  /// 包装成共享指针交出去；最后一个引用消失时缓冲自动回池。
  std::shared_ptr<BgraFrame> publish(BgraFrame* f) {
    std::shared_ptr<FramePool> self = shared_from_this();
    return std::shared_ptr<BgraFrame>(f, [self](BgraFrame* p) {
      std::lock_guard<std::mutex> lk(self->m_);
      self->free_.push_back(p);
    });
  }

  std::size_t available() const {
    std::lock_guard<std::mutex> lk(m_);
    return free_.size();
  }
  std::size_t total() const { return buffers_.size(); }
  int width() const { return w_; }
  int height() const { return h_; }

  /// 桌面分辨率变了要换池子。调用方必须保证此刻没有帧在外面（都归还了），
  /// 否则旧缓冲被回收后指向的是已经被释放的 vector。
  void rebuild(int width, int height) {
    std::lock_guard<std::mutex> lk(m_);
    w_ = width;
    h_ = height;
    build();
  }

 private:
  void build() {
    buffers_.clear();
    free_.clear();
    std::size_t const stride = static_cast<std::size_t>(w_) * 4;
    std::size_t const bytes = stride * static_cast<std::size_t>(h_);
    buffers_.reserve(count_);
    free_.reserve(count_);
    for (std::size_t i = 0; i < count_; ++i) {
      auto f = std::make_unique<BgraFrame>();
      f->width = w_;
      f->height = h_;
      f->stride = static_cast<int>(stride);
      f->pixels.assign(bytes, 0);
      free_.push_back(f.get());
      buffers_.push_back(std::move(f));
    }
  }

  int w_;
  int h_;
  std::size_t count_;
  mutable std::mutex m_;
  std::vector<std::unique_ptr<BgraFrame>> buffers_;
  std::vector<BgraFrame*> free_;
};

}  // namespace d2o
