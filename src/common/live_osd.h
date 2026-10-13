#pragma once

/**
 * @file src/common/live_osd.h
 * @brief 水印配置的"当期那一份"，让改水印**下一帧就生效**。
 *
 * 为什么需要它，而不是让采集线程直接读 `AppConfig`：写入方是 Web 循环线程
 * （`POST /api/config`）和 SOAP handler（NVR 的 `SetOSD`），读取方是采集线程
 * （每帧画水印）。直接共享 `AppConfig` 就是两个线程读写同一个对象 —— 而
 * `OsdConfig` 里有一个 `std::vector`，改它的同时另一边正在遍历它，读到的是
 * 已经释放的缓冲区。这里换的是**整份不可变快照**：读的人拿到一个
 * `shared_ptr<OsdConfig const>`，那份东西在他手里期间不会被任何人改，所以
 * 不需要读锁，也不可能读到半个状态。
 *
 * 为什么不是"改水印就重建管线"（这正是本阶段之前的行为）：重建意味着录制
 * 分段、RTSP 客户端全部断开重连 —— 为了改一行水印文字付这个代价，用户不会
 * 接受。代价是**同一段录像里可能出现两种水印位置**，这个取舍是明说的
 * （README「已知取舍」），不是意外。
 *
 * 与 `DeviceState` 的关系：那一位管"水印上写什么时间、什么名字"（值），
 * 这一位管"水印怎么画、画在哪"（形状）。两者都在被改的那一刻生效。
 */

#include <memory>
#include <mutex>
#include <utility>

#include "common/config.h"

namespace d2o {

/// 一份可以随时被换掉、但换出去的每一份都不可变的水印配置。
///
/// 指针（`LiveOsd const*`）交给 `SessionOptions`，值（`snapshot()`）交给
/// 采集循环 —— 采集循环每拍取一次，所以"改完下一帧就变"是这个接口的直接
/// 后果，而不是某处额外做的同步。
class LiveOsd {
 public:
  LiveOsd() = default;
  explicit LiveOsd(OsdConfig const& c) : cur_(std::make_shared<OsdConfig const>(c)) {}

  LiveOsd(LiveOsd const&) = delete;
  LiveOsd& operator=(LiveOsd const&) = delete;

  /// 换一份。可以在任意线程调用（Web 循环 / SOAP handler / 启动线程）。
  void set(OsdConfig const& c) {
    auto next = std::make_shared<OsdConfig const>(c);
    std::lock_guard<std::mutex> lk(m_);
    cur_ = std::move(next);
  }

  /// 取当期那一份。**永远不返回空**：构造时就装了一份，`set()` 也不会装空 ——
  /// 所以采集循环不需要判空（那会变成一个"忘了判"就崩的地方）。
  std::shared_ptr<OsdConfig const> snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    return cur_;
  }

 private:
  mutable std::mutex m_;
  std::shared_ptr<OsdConfig const> cur_{std::make_shared<OsdConfig const>()};
};

}  // namespace d2o
