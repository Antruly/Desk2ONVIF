#pragma once

/**
 * @file src/app/audio_track.h
 * @brief 音轨的驱动：**每一拍**产出"这一拍该有的那么多"音频，交给 AAC 编码器。
 *
 * ## 为什么"一拍"是这一层的核心概念
 *
 * 音频的时间轴归**编码线程**（完整依据在 `AudioMixer::fill()` 的说明里：环回在
 * 系统不出声时一个包都不来 —— 实测 0 包 0 帧，不是"来了带 SILENT 标志的静音"。
 * 按"从源端收到多少"推时间轴的话，系统一安静音轨就短一截，而安静是常态）。
 * 于是编码线程每弹出一帧视频，就推一拍音频，`target = 采样率 / 帧率`：
 *
 *   - 音轨的样本总数 **= 拍数 × 采样率 / 帧率**，与设备稳不稳、有没有声音无关；
 *   - 音视频共用同一个原点（都是"第几拍"），A/V **在结构上不可能漂**。
 *
 * ## 为什么按"累计应得"算，而不是每拍固定给 `rate/fps` 个
 *
 * 48000/25 = 1920 整除，看起来"每拍 1920"就够了。但帧率是用户可改的（画质档位里
 * 有 12/20/25/30），而 48000/7 = 6857.14 —— 每拍取 6857 的话每小时短掉 51 秒，
 * 而症状是"录像越长、声音跑得越前"，没人会联想到一次整除。
 *
 * 所以这里记的是**累计应得**（`ticks * rate / fps`）再减去已产出的：差值法逐拍
 * 累加，余数不会被丢掉，整条音轨的误差不超过一个样本。
 *
 * ## 为什么单独一个文件（而不是塞进 capture_session.cpp 的匿名命名空间）
 *
 * 那样它就只能靠"真的开一次桌面采集"来验，而这条链路上最关键的那个不变量
 * —— **样本总数只由拍数决定** —— 恰恰是最不需要 DXGI 的：它要的是"源端一个样本
 * 都不给的时候，音轨照样长对"。见 `tests\audio_track_check.cpp`。
 */

#include "audio/audio_mixer.h"
#include "encoder/aac_encoder.h"

#include <cstdint>
#include <string>
#include <vector>

namespace d2o {

struct AudioTrackOptions {
  /// 视频的拍频。**音频的节拍跟着它走**：一拍 = 一帧的时间。
  int fps = 25;
  /// AAC 码率。只影响编码器；SDP 里那句 `b=AS:` 由 `RtspAudioOptions` 单独给。
  int bitrate_kbps = 64;
  /// 源、增益、采样率、声道、环深。
  ///
  /// **采样率与声道的权威在这里**（`mixer.rate` / `mixer.channels`），这一层不再
  /// 抄一份 —— 两份的话，改了配置里那处、忘了这处，症状是"sdp 说 48000 而实际
  /// 按 44100 编"，而两边单独看都对。
  AudioMixer::Options mixer;
};

class AudioTrack {
 public:
  AudioTrack() = default;
  ~AudioTrack();

  AudioTrack(AudioTrack const&) = delete;
  AudioTrack& operator=(AudioTrack const&) = delete;

  /// 起源（设备在这一步真的被打开）并起编码器。**阻塞**，见 `AudioMixer::start`。
  ///
  /// 返回 false 时 `err` 是人话，调用方应当**按纯视频继续**而不是整趟失败 ——
  /// 麦克风被拔掉不该让录制起不来。两路里坏了一路不算失败（那一份由
  /// `mixer().sources()` 逐项报出来）。
  bool start(AudioTrackOptions const& opt, std::string& err);

  /// 停采集并 join。**幂等**，析构里也会调一次。
  void stop();

  bool active() const { return active_; }

  /// **编码线程**调：推进一拍。`sink` 收这一拍编出来的 AU（0~2 个 —— 编码器内部
  /// 按 1024 样本攒 FIFO，而一拍是 1920，所以有时出 1 包有时出 2 包）。
  void tick(PacketSink const& sink);

  /// 冲刷编码器 FIFO 里不足一帧的尾巴（补零凑满，见 `AacEncoder::flush`）。
  ///
  /// 顺序是硬的：**最后一拍之后、关文件之前**。落在关文件之后的话那不到 21 ms
  /// 会被静默丢掉，而它在音轨的**结尾**（症状是"音轨比视频短一截"，最容易被
  /// 归因到别处）。
  bool flush(PacketSink const& sink, std::string& err);

  /// 已经交出去的样本帧数。**它是"音轨有多长"的唯一依据**（÷ `rate` 秒）。
  /// 包数是编码器的节奏，不是时长 —— 拿它算时长会在最后一拍上差最多 21 ms。
  std::uint64_t frames_out() const;
  double seconds() const;

  /// 名义上每拍多少帧。**只用于日志和缓冲区容量**；实际给多少由 `tick()` 按
  /// 累计数算（见文件头）。
  int frames_per_tick() const;

  /// 供状态统计读（电平表、每一路的 packets/audible_packets）。
  AudioMixer const& mixer() const { return mixer_; }
  /// 供 SDP 读（`asc_hex()`）和 MP4 读（`codec_context()` 取 codecpar）。
  AacEncoder const& encoder() const { return encoder_; }

  /// 编码器**最近一次**调用失败了。给 `audio_state` 用。
  ///
  /// 编码失败**不中断录制**：视频照走，音轨从这里断掉，而状态页上看得出来。
  /// 让视频跟着一起死才是更坏的选择 —— 用户要的是"画面还在，声音出了问题"。
  bool encode_failed() const { return encode_failed_; }

 private:
  AudioMixer mixer_;
  AacEncoder encoder_;
  /// 每拍交给混音器的暂存。按 `rate/fps + 1` 分配：差值法可能一次多要一个样本。
  std::vector<float> block_;
  AudioTrackOptions opt_{};
  std::uint64_t ticks_ = 0;
  bool active_ = false;
  bool encode_failed_ = false;
  bool warned_encode_ = false;
};

}  // namespace d2o
