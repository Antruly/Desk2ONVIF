#pragma once

/**
 * @file src/rtsp/h264_live_source.h
 * @brief 把 NaluQueue 包装成 Live555 能拉的 FramedSource。
 *
 * **Live555 是拉模型，而且框架自己不带队列** —— 这是写这个类时最容易搞反的
 * 地方，三条规则都是硬性的：
 *
 * 1. `doGetNextFrame()` 是**调度线程**在客户端要数据时调过来的。什么时候要
 *    由 sink 的节奏和 RTCP 反馈决定，不由我们决定。
 * 2. 队列空的时候必须**什么都不做就返回**，绝不能调 `afterGetting()` ——
 *    那等于投递了一帧不存在的数据。之后靠编码线程唤醒。
 * 3. `afterGetting()` 只能在调度线程上调。编码线程碰它是未定义行为。
 *
 * 队列本身在 `NaluQueue` 里（头文件同目录），这里只负责"拉"这一侧。
 */

#include "FramedSource.hh"
#include "UsageEnvironment.hh"

#include <atomic>
#include <cstdint>
#include <memory>

namespace d2o {

class NaluQueue;

class H264LiveSource : public FramedSource {
 public:
  static H264LiveSource* createNew(UsageEnvironment& env,
                                   std::shared_ptr<NaluQueue> queue,
                                   unsigned fps);

  /// **编码线程**调用：队列里进新数据了，把调度线程叫醒。
  /// 这是全 Live555 里唯一允许跨线程调用的东西。
  void wake();

 protected:
  H264LiveSource(UsageEnvironment& env, std::shared_ptr<NaluQueue> queue,
                 unsigned fps);
  virtual ~H264LiveSource();

  virtual void doGetNextFrame();

 private:
  static void on_trigger(void* clientData);
  void deliver();

  std::shared_ptr<NaluQueue> queue_;
  unsigned fps_;
  EventTriggerId trigger_ = 0;

  /// `triggerEvent()` 的契约（`UsageEnvironment.hh` 原文）：
  /// "once triggerEvent() is called with one event trigger id, it should not be
  ///  called again with the same event trigger id until after its event has been
  ///  handled."  30 fps 下编码线程会想连着触发，所以必须自己加一道门闩，
  /// 只在 false→true 的那一次真的调 triggerEvent。
  std::atomic<bool> pending_{false};

  /// 呈现时间的锚点：把编码器的帧号换算成挂钟时间。
  /// 队列里可能压着几百毫秒的数据，直接用"投递时刻"当时间戳会把它们全挤在
  /// 同一瞬间，客户端看到的是一顿一顿的。
  struct timeval anchor_{};
  std::int64_t anchor_pts_ = -1;
};

}  // namespace d2o
