#pragma once

/**
 * @file src/audio/wasapi_capture.h
 * @brief 一路 WASAPI 采集：环回（"系统声音"）或麦克风，统一成 48 kHz 立体声 float32。
 *
 * **这一层只负责"把设备上的样本搬到环形缓冲里"**，不认识增益、不认识混音、
 * 不认识编码器。时间轴的主人也不是它 —— 见 `audio_mixer.h` 里"为什么由编码线程
 * 驱动"那一段。这样分开的好处是：设备时钟快一点慢一点，只影响"环里攒了多少"，
 * 不会顺着链路一路漂到 MP4 的时长上。
 *
 * 三条它必须自己扛住的现实（都不是假想的，见陷阱 67 / 68 那两条）：
 *
 *  1. **事件驱动不保证能用**。环回 + `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` 在有些
 *     驱动上 `Initialize` 直接失败；更坏的一种是**初始化成功、事件永不触发** ——
 *     那会得到一条永远静止的音轨，而 SDP、`ffprobe`、退出码全都正常。
 *     所以：初始化失败退回轮询（记一行日志），**运行中事件长时间不来也退回轮询**。
 *  2. **`AUDCLNT_BUFFERFLAGS_SILENT` 时 `pData` 的内容是未定义的**。系统不出声时
 *     环回会持续给这个标志。必须自己造零，否则混进去的是一段垃圾（听起来是噪音，
 *     会被归因到编码器头上）。
 *  3. **设备会中途消失**（拔麦克风、切默认设备、独占模式被别的程序抢走），
 *     此时 `GetBuffer` 给 `AUDCLNT_E_DEVICE_INVALIDATED`。要退避重开，
 *     **绝不阻塞编码线程、绝不崩**，期间输出静音并把状态置成 `NoSignal`。
 *
 * 还有一条**实测出来的、和直觉相反**的行为，它是"混音必须由编码线程按帧数驱动"
 * 这个设计的直接依据（见 `audio_mixer.h`）：
 *
 * > **环回在系统不出声时一个包都不来。**
 * > 实测（`tests/wasapi_check.cpp`）：没有声音在放的那 2 秒是 **0 包 0 帧** ——
 * > 不是"来了带 `SILENT` 标志的静音"，而是播放引擎空闲就**根本不产数据**。
 * > 而一旦真的在放声音（`PlaySound` 循环一段 WAV），环回是 **100.0%** 实时、
 * > 零欠载、零丢弃，和麦克风一模一样；起播第一秒 72%（引擎起转的补积压，
 * > 不是缺陷）。
 *
 * 所以"系统声音"这一路在没人出声的时候**是空的**，而且这是正常现象、不是故障。
 * 音频的时间轴因此绝不能建立在"从源端收到了多少"上 —— 那样系统一安静，音轨就
 * 会短一截。时间轴只能由编码线程按视频拍数出来，源端不够就补零。
 *
 * ⚠️ **量这一层的时候别用 `Beep()` 放测试音**：它不是一条连续的音频流，同一个
 * 采集层配它只能量到 94%~97%（放一次 3 秒的）乃至 30.3%（连放 15 次 400 ms 的），
 * 而那看起来活像采集层的缺陷。用 `PlaySound` 循环播一段真 WAV。
 */

#include <cstdint>
#include <future>
#include <memory>
#include <string>

#include "audio/audio_device.h"

namespace d2o {

/// `QueryPerformanceCounter` 的当前值。音频时间戳（`pu64QPCPosition`）就是它。
std::uint64_t qpc_now();

/// 每秒多少个 QPC 计数。同一台机器上恒定，进程内缓存。
std::uint64_t qpc_frequency();

class WasapiCapture {
 public:
  /// 采集的状态。给状态页和日志用。
  enum class State {
    Idle,      ///< 没启动，或者已经停了
    Ok,        ///< 正常在出样本（**静音也是 Ok** —— 系统不出声不是故障）
    NoSignal,  ///< 开着但出不来东西：设备没了、正在退避重开
    Failed,    ///< `start()` 就没成功（设备选错了、格式不支持）
  };

  struct Options {
    /// 哪一路。决定去问 `eRender` 还是 `eCapture`。
    AudioFlow flow = AudioFlow::Render;
    /// 设备 id（`IMMDevice::GetId()`）。**空 = 默认端点** ——
    /// 而且在设备被拔掉之后"默认"会跟着系统走，这正是用户想要的。
    std::string device_id;
    /// 采集方式。只有渲染端点能环回；采集端点写 true 会被忽略。
    bool loopback = false;
    /// 输出采样率 / 声道数。AAC 那条路固定 48 kHz 立体声。
    int rate = 48000;
    int channels = 2;
    /// 环形缓冲能装多少帧 —— **它就是这一路的延迟上界**。
    ///
    /// 满了丢最旧的（和视频那条路同一个哲学），所以这个数等于"声音最多能比
    /// 画面晚多久"。300 ms：够吃掉编码线程的排程抖动，又不至于让"系统声音"
    /// 的起播听着明显滞后。
    ///
    /// 别以为它大一点更安全。环回在系统开始出声时会**补一段积压**（实测第一秒
    /// 送到 67% 实时、第二秒 94%），环越大，这段积压被抽完之前声音就越滞后 ——
    /// 而超出这一刻之后的数据本来也不该要了。
    int ring_frames = 14400;
    /// 日志里怎么称呼这一路（"系统声音" / "麦克风"）。
    /// **每一行日志都要带上它** —— 两路同时开时，不带就分不清是谁在说话。
    std::string label = "音频";
  };

  WasapiCapture();
  ~WasapiCapture();
  WasapiCapture(WasapiCapture const&) = delete;
  WasapiCapture& operator=(WasapiCapture const&) = delete;

  /// 起一条采集线程并把设备打开。
  ///
  /// **阻塞到"打开成功或失败"有结论为止**（COM 只能在其所属线程上初始化，
  /// 所以整条 OpenDevice 的流程都在采集线程上跑，这里等那个结果）。
  /// 失败时填 `err` 并返回 false，调用方据此决定"这一路没戏了"还是"整条音频关掉"。
  bool start(Options const& opt, std::string& err);

  /// 停采集线程并 join。可重复调用。设备失效的重试循环会在这期间退出。
  void stop();

  State state() const;
  /// 一句话说明现在什么情况（"正在采集" / "设备没了，正在重试"），给网页用。
  std::string status_text() const;
  /// "走的是事件驱动还是轮询" —— 排查"这台机器音轨是死的"时必须能看见这一条。
  std::string pacing_text() const;

  /// 环形缓冲里现在有多少帧还没被取走。
  int available() const;

  /// 把最多 `frames` 帧**累加**进 `dst`（交错立体声，`dst` 长度 2*frames）。
  ///
  /// 是"累加"不是"覆盖"：混音就是把几路加在一起。**调用方负责先把 dst 清零** ——
  /// 把这件事留给调用方，是因为"源不够补零"和"根本没启用这一路"在数值上都是
  /// 零，由调用方一次做掉比这里每路各清一遍清楚。
  ///
  /// 返回真正来自设备的帧数（可能小于 `frames`，那就是欠载）。
  int pull_add(float* dst, int frames);

  /// 被环形缓冲丢掉的帧数（源端产出快过消费端时才会涨）。
  std::uint64_t dropped_frames() const;
  /// 设备失效后自动重开的次数。**不为 0 说明这台机器的音频不稳**，要能看见。
  std::uint64_t reopen_count() const;
  /// 收到过多少个包（`GetBuffer` 里有 frames > 0）。
  ///
  /// 和"采样数"分开记是有原因的：**环回在系统完全不出声时一个包都不来**
  /// （播放引擎空闲就不产数据），而麦克风永远按采样率出包。所以
  /// "`packets == 0`" 和 "`packets > 0` 但全是 SILENT" 是两种不同的现实：
  /// 前者是"没人出声"，后者是"引擎在跑、这一路确实静音"。混成一个数就分不清了。
  std::uint64_t packets() const;
  /// 没有带 `AUDCLNT_BUFFERFLAGS_SILENT` 的包数。**为 0 说明这一路到现在
  /// 一个非静音样本都没收到过** —— 麦克风被静音、或者环回根本没接上，
  /// 都长这样。和采样数分开记，是为了让"有数据"和"有声音"这两件事可分辨。
  std::uint64_t audible_packets() const;

 private:
  /// 采集线程的本体。整个 OpenDevice 流程都在这里跑：`CoInitializeEx` 是**按线
  /// 程**的，而 `IMMDevice` 这些接口不该跨线程用，所以"在哪个线程上建的就一直在
  /// 那个线程上用"这条最简单的规则就能避免一整类难查的问题。
  /// 它通过 `init` 把"打开成功/失败"告诉 `start()`，然后一直抽水到 `stopping`。
  void thread_main(std::promise<std::string> init);

  bool open_device(std::string& err);   ///< 只在采集线程上调用
  void close_device();                  ///< 只在采集线程上调用
  void pump();                          ///< 等一次（事件或睡一觉）然后抽空
  void drain();                         ///< GetNextPacketSize 循环
  void deliver(unsigned char const* data, unsigned frames, std::uint64_t qpc);
  void handle_stream_error(long hr);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace d2o
