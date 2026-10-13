#pragma once

/**
 * @file src/rtsp/aac_live_source.h
 * @brief 把 AuQueue 包装成 Live555 能拉的 FramedSource（音频那一路）。
 *
 * 拉模型的三条硬规则和视频那一路**完全一样**（`H264LiveSource` 的文件头里有
 * 完整说明，这里不重复）：队列空时什么都不做就返回、`afterGetting()` 只能在
 * 调度线程上、跨线程唤醒只有 `triggerEvent()`。
 *
 * 和 `H264LiveSource` 的差别只有两处：
 *
 *   - **呈现时间按采样率换算**（`Δpts * 1e6 / sample_rate`），而不是帧率。
 *   - **`fDurationInMicroseconds` 取一个 AU 的时长**（`samples_per_au` 个样本）。
 *     它不是装饰：`MultiFramedRTPSink` 每往包里放一帧就把它加到
 *     `fNextSendTime` 上（`MultiFramedRTPSink.cpp:329`），那是发送节奏的来源。
 *     给 0 的话整个队列会被一口气全推出去（对直播无害但没必要）。
 *
 * 首包 pts 是 **−1024**（编码器的启动填充，见 aac_encoder.h），所以这里必须
 * 按**差值**算、并且钳住负值 —— 直接把它当绝对时间会得到一个 1970 年之前的时间戳。
 */

#include "FramedSource.hh"
#include "UsageEnvironment.hh"

#include <atomic>
#include <cstdint>
#include <memory>

namespace d2o {

class AuQueue;

class AacLiveSource : public FramedSource {
 public:
  /// `samples_per_au` 是一个 AU 里的样本数（`AacEncoder::frame_size()`，实测 1024）。
  static AacLiveSource* createNew(UsageEnvironment& env,
                                  std::shared_ptr<AuQueue> queue,
                                  unsigned sample_rate,
                                  unsigned samples_per_au);

  /// **编码线程**调用：队列里进新数据了，把调度线程叫醒。
  void wake();

 protected:
  AacLiveSource(UsageEnvironment& env, std::shared_ptr<AuQueue> queue,
                unsigned sample_rate, unsigned samples_per_au);
  virtual ~AacLiveSource();

  virtual void doGetNextFrame();

 private:
  static void on_trigger(void* clientData);
  void deliver();

  std::shared_ptr<AuQueue> queue_;
  unsigned sample_rate_;
  unsigned samples_per_au_;
  EventTriggerId trigger_ = 0;

  /// 门闩。契约见 h264_live_source.h：同一个事件触发器在上一次事件被派发之前
  /// 不能再次触发。
  std::atomic<bool> pending_{false};

  /// 呈现时间的锚点。见 h264_live_source.h 里的说明：队列里可能压着几百毫秒的
  /// 数据，用"投递时刻"当时间戳会把它们挤到同一瞬间。
  ///
  /// ⚠️ "定没定过锚"必须是**单独一个 bool**，不能拿 `anchor_pts_ < 0` 当判据
  /// —— 视频那一路的首帧号是 0，所以 `= -1` 那个哨兵在那边刚好能用；而音频的
  /// 首包 pts 实测就是 **−1024**（编码器的启动填充，见 aac_encoder.h）。
  ///
  /// **后果是量出来的，不是推出来的**（2026-10-07，`tests\aac_live_source_check.cpp`
  /// 就是为此写的）：哨兵写法下，队列从 −1024 开始时**前两个 AU 拿到同一个呈现
  /// 时间**（实测两包都印 `1791387250.716109`）—— 首个 AU 把锚点定到 −1024，
  /// 下一个 AU 的 pts 是 0，`anchor_pts_ < 0` 仍然成立于是锚点又被重定到 0。
  /// 差距只有 21 ms、而且之后每包的间距都是对的，所以**症状很轻**：不是"没声音"
  /// 也不是"一顿一顿"，只是开头两个 RTP 包时间戳相同。
  ///
  /// 更要紧的是**它在端到端里量不出来**：`tests\rtsp_audio_check.ps1` 拿哨兵写法
  /// 重编之后整条照样全绿（拉流的是服务起来 ~0.5 秒之后才连上来的播放器，那时
  /// 第一个被投递的 pts 早就是正数了，哨兵没机会出错）。所以改这一段之后必须
  /// 重跑那个单元测试 —— 它是这条缺陷唯一会红的地方。
  struct timeval anchor_{};
  bool anchored_ = false;
  std::int64_t anchor_pts_ = 0;
};

}  // namespace d2o
