#include "common/device_state.h"

namespace d2o {

std::chrono::system_clock::time_point DeviceState::now() const {
  return std::chrono::system_clock::now() + time_offset();
}

void DeviceState::set_device_time(std::chrono::system_clock::time_point device_now) {
  auto const host = std::chrono::system_clock::now();
  // **四舍五入，不是向零截断。** 调用方给的 `device_now` 是它自己按
  // 「主机现在 + N 秒」算的，而这里又读了一次主机时刻，于是差值是 `N − ε`
  // （ε = 两次读钟之间真实流逝的时间）。`duration_cast` 向零截断 ⇒ 只要
  // ε > 0，**正**偏移就少一秒：NVR 对完时水印比录像机慢一秒，网页手动校准
  // +90 秒而 `/api/time` 报 89。
  //
  // 这个缺陷不是每次都能看见，所以当初没暴露 —— 它取决于时钟分辨率。
  // 实测（`out\clockgran\probe.cpp`，同一台机器）：`system_clock::now()`
  // 分辨率约 104 ns，于是 ε 是 0 还是几十纳秒随机；两万次里「恰好 90」
  // 14992 次、掉到 89 有 5008 次（**25%**）。单跑一次只有四分之一的机会
  // 撞上，这也是 `tests\device_time_check.cpp` 要循环 500 轮的理由。
  // 换成 round 之后同一组两万次是 20000/20000。
  //
  // 负方向本来就没事（向零截断对负数是往大里跑：−90 − ε → −90），改完之后
  // 仍然对 —— 那个测试里留着负偏移和 0 两档就是为了当这个对照。
  auto const off = std::chrono::round<std::chrono::seconds>(device_now - host);
  offset_s_.store(off.count(), std::memory_order_relaxed);
  synced_at_s_.store(std::chrono::duration_cast<std::chrono::seconds>(
                         host.time_since_epoch())
                         .count(),
                     std::memory_order_relaxed);
  // **最后再置 synced_**：读的人拿到 synced_ 为真时，偏移和时刻都已经写好了。
  // 顺序反过来的话，状态页可能显示"已同步，偏移 +0 秒" —— 一个看起来像
  // "NVR 设了个和主机一样的时间"的假象。
  synced_.store(true, std::memory_order_release);
}

void DeviceState::reset_time() {
  offset_s_.store(0, std::memory_order_relaxed);
  synced_at_s_.store(0, std::memory_order_relaxed);
  synced_.store(false, std::memory_order_release);
}

std::string DeviceState::channel_name() const {
  std::lock_guard<std::mutex> lk(mu_);
  return name_;
}

void DeviceState::set_channel_name(std::string s) {
  std::lock_guard<std::mutex> lk(mu_);
  name_ = std::move(s);
}

}  // namespace d2o
