#pragma once

/**
 * @file src/common/adaptive.h
 * @brief 性能不足时的降载决策。**纯逻辑，不碰任何系统 API，可单测。**
 *
 * 这一层只回答一个问题：「这一秒丢了这么多帧，下一拍该每几拍才出一帧？」
 *
 * ## 为什么唯一的手段是"丢帧"
 *
 * 编码跟不上时有三条路：降码率、降分辨率、降帧率。前两条在这条链路上都
 * **改不了运行中的东西** —— 码率要重建编码器（FFmpeg 的 libx264 封装层不认
 * 运行中改 `bit_rate`，`x264_encoder_reconfig` 在导出表里出现 0 次），分辨率
 * 更是把 SPS/PPS、MP4 轨道头、SDP 全绑死了。重建编码器意味着录制分段、
 * RTSP 客户端掉线 —— 那不该是"这台机器有点卡"的反应。
 *
 * 而帧率是**采集循环的定速器**决定的：`period = 1e9/fps`、`pts = tick`。
 * 于是"每第 d 拍才入队一次"就等于"帧率降到 fps/d"，而 PTS 每帧前进 d、
 * 墙钟与画面严格对应 —— **无损**，代价是调用方一句 `tick % duty == 0`。
 *
 * ## 为什么要两侧滞回
 *
 * 只用"丢了就升档、没丢就降档"会在边界上以秒为周期来回抖：升档之后负载轻了，
 * 立刻降档，负载又重了。所以两个方向的门槛和确认时长**刻意不同**：
 * 升档看"这一秒丢得够多"（立刻），降档要"连续 N 秒一帧不丢"（慢）。
 * 不对称是有意的：降档（画质变好）比升档（丢帧）的代价小，但**来回抖**比
 * 一直停在略保守的那一档糟得多。
 */

#include "common/config.h"

#include <cstdint>

namespace d2o {

/// 给人看的状态。字符串形式直接进 stats / 网页横幅。
enum class PerfState {
  Ok,          ///< 跟得上，duty == 1
  Throttled,   ///< 正在丢帧，但丢得动（画面还在实时）
  Overloaded,  ///< 已经丢到帧率下限**仍然**跟不上 —— 只能提示降档
};

char const* perf_state_name(PerfState s);

class AdaptiveController {
 public:
  /// `target_fps` 是**用户配的**帧率，不是当前帧率。`cfg.mode == "off"` 时
  /// 整个控制器退化成恒等（duty 恒为 1、状态恒为 Ok）。
  void configure(AdaptiveConfig const& cfg, int target_fps);

  /// 喂一秒的观测。
  ///
  /// `dropped` = 这一秒里"本该产出却没入队"的拍数（队列丢弃 + 池子耗尽的
  /// 增量）。`pool_starved` = 这一秒里池子见底过 —— 它和 dropped 一起看，
  /// 因为池子见底说明**编码器长期落后**，而队列丢弃只是抖动。
  void observe(std::uint64_t dropped, bool pool_starved);

  /// 每几拍入队一次。恒 >= 1；1 = 不丢。
  int duty() const { return duty_; }

  /// 实际目标帧率 = 配置帧率 / duty。
  int target_fps() const;

  PerfState state() const { return state_; }

  /// 配置帧率。configure() 时定下，用于算 duty 上限。
  int base_fps() const { return base_fps_; }

 private:
  AdaptiveConfig cfg_;
  int base_fps_ = 25;
  int max_duty_ = 1;
  int duty_ = 1;
  PerfState state_ = PerfState::Ok;

  /// 连续"干净"的秒数，降档靠它。升档不看它 —— 见文件头那段。
  int clean_seconds_ = 0;
  /// 连续「**已经在上限上**还跟不上」的秒数。Overloaded 靠它。
  ///
  /// 单独一个计数器不是啰嗦：爬坡途中每次都算"跟不上"，拿一个通用的坏秒数
  /// 去判的话，duty 刚到上限那一刻就会被报成 Overloaded —— 而它一秒都还没在
  /// 上限上待过（见 .cpp 里那段）。
  int at_cap_bad_seconds_ = 0;
};

}  // namespace d2o
