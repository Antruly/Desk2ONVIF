#pragma once

/**
 * @file src/audio/audio_mixer.h
 * @brief 把勾选的音频源混成一路 48 kHz 立体声 float32，交给编码器。
 *
 * ## 为什么时间轴的主人是编码线程，而不是这里自己起一条线程
 *
 * 直觉上混音该有一条自己的线程：有源就往里推，够 1024 个样本就叫编码器来取。
 * **在这台设备上那是错的**，而且错得很隐蔽。实测（`tests\wasapi_check.cpp`）：
 *
 * | | 系统不出声时 | 出声时（稳态） | 起播第一秒 |
 * |---|---|---|---|
 * | 环回（系统声音） | **0 包 0 帧** | **100.0%** 实时 | 72% |
 * | 麦克风 | 照常出包（内容全零） | **100.0%** 实时 | 94% |
 *
 * 也就是说，"系统声音"这一路在没人出声的时候**一个样本都不来** —— 不是来了带
 * `AUDCLNT_BUFFERFLAGS_SILENT` 的静音包，而是播放引擎空闲就根本不产数据。
 * 所以按"从源端收到了多少"推时间轴的写法，**系统一安静音轨就短一截**：
 * 用户看到的是"录像里声音比画面早结束"，或者更糟 —— 音轨时长对不上、
 * 整个文件的时间轴被拖歪。而安静是常态，不是故障。
 *
 * 因此：`fill(target_frames)` 由**编码线程按视频拍数**调用，一次要多少给多少，
 * 源端不够就补零。时间轴于是只有一个主人（编码线程），和视频用同一个原点，
 * A/V **在结构上不可能漂**，而不是"漂了我们再想办法对齐"。
 *
 * ## 两个后果，都是刻意的
 *
 *  1. **源端多了就丢最旧的**（`WasapiCapture` 的环本来就是 drop-oldest）。
 *     廉价声卡和麦克风不同源时钟，长期必然有一路快一点 —— 丢最旧把这点漂移
 *     **结构性地**吃掉，代价是偶发一次咔哒声。对监控用途，一次咔哒远比
 *     "声音慢三秒"可接受。
 *  2. **补零是正常路径，不是兜底。** `zero_filled_frames()` 在系统安静时本来就
 *     会涨，它不是故障指示 —— 别拿它去报警。
 *
 * ## 增益
 *
 * 每一路先按自己的增益缩放**再**求和（顺序不能反：反了就没法给两路设不同的
 * 增益）。两路同时开时再各乘 `1/√2` —— 等功率定律，等价于"各乘 0.7"，
 * 免得两路都接近满幅时恒定削顶。写 `√2` 而不是 `0.7` 是因为它是个**理由**
 * 而不是一个魔数。
 *
 * 这个系数按"**开着**几路"算，不按"这一刻**真的有数据**的几路"算：后者会在
 * 系统安静/出声之间来回切增益，听起来是一抽一抽的响度跳变，比恒定小 3 dB
 * 难听得明确。代价是系统明明安静时麦克风也白低 3 dB —— 这是取舍，不是疏漏。
 */

#include "audio/wasapi_capture.h"

#include <cstdint>
#include <string>
#include <vector>

namespace d2o {

/// 电平表读数。**L/R 分开**：混成单个数就看不出"只有一边有声音"
/// （单声道的麦克风上混、或者某一路的接线只通了一边，都是这个形状）。
struct AudioLevels {
  float peak_l = 0.0f;
  float peak_r = 0.0f;
  float rms_l = 0.0f;
  float rms_r = 0.0f;
};

class AudioMixer {
 public:
  /// 混音器的状态。**和 `WasapiCapture::State` 不是一回事** —— 那个说的是
  /// "某一路设备怎么样"，这个说的是"整条音轨怎么样"，给状态页和 stats 用。
  enum class State {
    Off,       ///< 没有任何一路启用（音频配置是关的）
    Ok,        ///< 启用的那几路都在正常出样本（**安静也是 Ok**）
    NoSignal,  ///< 开着但采不到东西：设备没了、正在退避重开
    Error,     ///< 一路都没起来（设备选错了、格式不支持）
  };

  /// 一路源的配置。对应 `audio.system` / `audio.microphone` 那两组。
  struct SourceOptions {
    bool enabled = false;
    /// 设备 id，**空 = 默认端点**。设备被拔掉之后"默认"会跟着系统走。
    std::string device_id;
    /// 增益。1.0 = 原样。
    float gain = 1.0f;
  };

  struct Options {
    int rate = 48000;
    /// **只支持 2**。采集层已经把每一路统一成 48 kHz 立体声了，这里再来一套
    /// 单声道下混只会多出一条没人走的路径 —— 而没人走的路径等于没被测过。
    int channels = 2;

    /// 环回，即"系统声音"。
    SourceOptions system;
    /// 麦克风。
    SourceOptions mic;

    /// 每一路采集环的深度（帧）。**它就是这一路的延迟上界**，见
    /// `WasapiCapture::Options::ring_frames`。
    int ring_frames = 14400;
  };

  AudioMixer();
  ~AudioMixer();
  AudioMixer(AudioMixer const&) = delete;
  AudioMixer& operator=(AudioMixer const&) = delete;

  /// 按配置起那几路采集。**阻塞到每一路都有结论**（设备是同步开的）。
  ///
  /// 返回 false 只在**一路都没起来**的时候（那时 `err` 是人话，音频整条关掉）。
  /// 两路里坏了一路不算失败：**保留好的那一路**、打一行 WARN、把它标成
  /// `error` —— 麦克风坏了不该把系统声音一起弄没，而用户还是能从状态页上
  /// 看见是哪一路坏了。
  bool start(Options const& opt, std::string& err);

  /// 停掉所有采集并 join。可重复调用。
  void stop();

  /// 编码线程调：把**恰好** `target_frames` 帧交错立体声 float32 写进 `dst`
  /// （`dst` 长度是 `2 * target_frames`）。
  ///
  /// **一定会写满**：源端不够就补零（见文件头）。所以调用方不需要、也不该
  /// 自己清零 `dst` —— 和 `WasapiCapture::pull_add` 的约定**正好相反**，
  /// 那个是"累加、调用方先清零"，这里是"覆盖、一定写满"。
  ///
  /// 返回这一块里**至少有一路真的供上了数据**的帧数。等于 `target_frames`
  /// 就是"没饿着"；小于它说明补了零 —— 而这在系统安静时是正常的。
  ///
  /// 电平表就在这一步更新（混完之后算，即"真正进了音轨的东西"）。
  int fill(float* dst, int target_frames);

  State state() const;
  /// 线上用的字符串：`off` / `ok` / `nosignal` / `error`。给 stats 用。
  std::string state_text() const;

  /// 混完之后（含增益）的电平表读数。带释放时间，见 `fill()` 的实现。
  AudioLevels levels() const;

  /// 已经交出去的帧数。**它是"音轨有多长"的唯一依据** —— 除以 rate 就是
  /// 音轨时长，和有没有声音、设备稳不稳都无关。
  std::uint64_t frames_out() const;
  /// 其中**一路都没供上、由我们补零**的帧数。系统安静时它本来就涨，
  /// **不是故障指示**。
  std::uint64_t zero_filled_frames() const;

  /// 每一路的一份快照，给状态页用（`GET /api/audio/devices` 那类）。
  ///
  /// 做成快照而不是"暴露 Capture 对象"：状态页要在**别的线程**上读，
  /// 而一次拿一份拼好的值，就不存在"读到半新半旧"这种最难查的问题。
  struct SourceStats {
    std::string label;                 ///< "系统声音" / "麦克风"
    bool enabled = false;
    std::string state;                 ///< 同 `state_text()` 的取值
    std::string pacing;                ///< "事件驱动" / "轮询（…）"
    std::string device;                ///< 设备名 + 格式那一行
    std::uint64_t packets = 0;
    std::uint64_t audible_packets = 0;  ///< **为 0 = 这一路到现在一个非静音样本都没有**
    std::uint64_t dropped_frames = 0;
    std::uint64_t reopen_count = 0;     ///< **不为 0 说明这台机器的音频不稳**
    int buffered_frames = 0;            ///< 环里还没被取走的
  };
  std::vector<SourceStats> sources() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace d2o
