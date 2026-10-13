#pragma once

/**
 * @file src/common/device_state.h
 * @brief ONVIF 层和采集层**同时读写**的那两样东西：设备时钟、摄像头名称。
 *
 * 为什么需要它，而不是各自存一份：这两样都必须在被改的**那一刻**生效。
 *
 *   - NVR 调 `SetSystemDateAndTime` 之后，画面上水印的时间要立刻跟着走；
 *   - NVR 改名（`SetVideoEncoderConfiguration` 的 `Name`）之后，水印上的
 *     名称要立刻变，而不是等下次重启。
 *
 * 两者的写入方是 Web 循环线程（SOAP handler），读取方是采集线程（每帧画水印）。
 * 各自拷一份的话，那份拷贝永远不会更新 —— 而症状是"改了没反应"。所以状态放在
 * 这里，由两边共享。
 *
 * 它刻意**不认识**配置文件和 ONVIF 的类型：写盘和发 SOAP 都是调用方的事。
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace d2o {

class DeviceState {
 public:
  /// 设备时间 = 主机时间 + NVR 下发的偏移。
  ///
  /// **绝不去改系统时钟**：那要管理员权限，而且会连累这台机器上所有别的程序
  /// （以及日志的时间戳）。NVR 要的是"画面上的时间和我的时间轴对齐"，那只需
  /// 要改画面上的那个数字。
  std::chrono::system_clock::time_point now() const;

  std::chrono::seconds time_offset() const {
    return std::chrono::seconds{offset_s_.load(std::memory_order_relaxed)};
  }

  /// 时钟是否被 NVR 设过（用来决定状态页要不要显示偏移、以及
  /// `GetSystemDateAndTime` 报 `Manual` 还是别的）。
  bool time_synced() const { return synced_.load(std::memory_order_relaxed); }

  /// NVR 在**主机时间**的哪一刻设的。状态页用它说明"这是什么时候被谁改的"。
  std::chrono::system_clock::time_point synced_at() const {
    return std::chrono::system_clock::time_point{
        std::chrono::seconds{synced_at_s_.load(std::memory_order_relaxed)}};
  }

  /// 把设备时间设成 `device_now`（由调用方从 SOAP 的
  /// `UTCDateTime`/`LocalDateTime` 解析出来）。偏移就是它和主机时间之差。
  void set_device_time(std::chrono::system_clock::time_point device_now);

  /// 复位：设备时间回到主机时间。状态页那个"恢复主机时间"按钮走这里。
  void reset_time();

  /// 摄像头名称。**空串是合法的**（水印那一项就画不出来，但不报错）。
  std::string channel_name() const;

  /// 改名称。ONVIF 的 `Set*`、Web 改 `onvif.device_name`、启动初始化三处都走它。
  void set_channel_name(std::string s);

 private:
  std::atomic<std::int64_t> offset_s_{0};
  std::atomic<bool> synced_{false};
  std::atomic<std::int64_t> synced_at_s_{0};

  mutable std::mutex mu_;
  std::string name_;
};

}  // namespace d2o
