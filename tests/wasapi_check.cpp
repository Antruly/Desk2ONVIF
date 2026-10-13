/**
 * @file tests/wasapi_check.cpp
 * @brief WASAPI 采集这一层到底在出样本吗 —— 以及环回和麦克风的**本质差别**。
 *
 * 为什么要有这个：这一层坏掉的样子**特别不像坏掉**。事件驱动在有些驱动上
 * "初始化成功但事件永不触发"，得到的是**一条永远静止的音轨** —— SDP 里有
 * `m=audio`、`ffprobe` 报得出 48000 Hz 立体声 AAC、时长也对，只有听的时候
 * 才发现是死的。同样地，"设备失效没重开"表现为中途一直静音，而不是任何报错。
 * 这两件事都不是"看一眼代码"能排除的。
 *
 * ## 这一跑量出来的两张表，都必须写下来
 *
 * **一、环回和麦克风不是同一种东西。**
 *
 * | | 系统不出声时 | 出声时（稳态） | 起播第一秒 |
 * |---|---|---|---|
 * | 环回（系统声音） | **0 包 0 帧**（播放引擎空闲就不产数据） | **100.0%** 实时，0 欠载 | 72% |
 * | 麦克风 | 照常出包（内容全零） | **100.0%** 实时，一秒不差 | 94% |
 *
 * 所以判据也不同：麦克风可以硬判"样数量 ≈ 墙钟"（实测误差 0.01%）；环回**只在出声
 * 期间**能硬判，安静时必须放过 —— 而"这一路到底有没有跟上"和"有没有声音"是两件事，
 * 前者只在后者成立时才问得出来。
 *
 * 而"系统安静时环回是空的"**不是缺陷**：音频的时间轴由编码线程按视频拍数出来
 * （见 `audio_mixer.h`），源端没有就补零，音轨时长照样一秒不差。反过来，把时间轴
 * 建立在"从源端收到了多少"上，系统一安静音轨就会短一截 —— 这个测试守的就是
 * 这两条路各自的正确行为，而不是某个统一的数字。
 *
 * **二、`Beep()` 不是一条连续的音频流，不能拿它当"系统正在放声音"。**
 *
 * 详细的实测表在 `tests\tone_wav.h`（测试音就放在那里，两个测试共用一份）。
 * 那条值得单独拎出来，是因为它**差点导出反的结论**：只用 `Beep` 测会看到
 * "环回只跟得上 30%"，那活像一个采集层缺陷，会让人去改一个根本没坏的模块；
 * 退一步用单次长 Beep 又得到"环回只能到 94%"这个**看起来像真实上界**的数，
 * 于是判据门槛设在 85% —— 门槛设在那里的理由是错的，虽然这次结论碰巧对。
 * **一个用来量东西的工具，本身就是最需要先被量一遍的那个东西。**
 *
 * 编译（在项目根目录下）：
 *   g++ -std=c++17 -Isrc -Ithird_party/ffmpeg/include tests/wasapi_check.cpp \
 *       src/audio/wasapi_capture.cpp src/audio/audio_device.cpp \
 *       src/common/log.cpp src/common/text.cpp -o build/wasapi_check.exe \
 *       -Lthird_party/ffmpeg/lib -lswresample -lavutil -lole32 -lmmdevapi -lwinmm
 */

#include <windows.h>

#include "audio/audio_device.h"
#include "audio/wasapi_capture.h"
#include "common/log.h"
#include "tone_wav.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace d2o;

namespace {

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;

void check(bool ok, std::string const& what) {
  if (ok) {
    ++g_pass;
    std::printf("[OK]   %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("[FAIL] %s\n", what.c_str());
  }
}

void skip(std::string const& what) {
  ++g_skip;
  std::printf("[跳过] %s\n", what.c_str());
}

void note(std::string const& what) { std::printf("       %s\n", what.c_str()); }

constexpr int kRate = 48000;
constexpr int kPullMs = 40;                            // 25 fps，和默认采集一致
constexpr int kPullFrames = kRate * kPullMs / 1000;    // 1920

// ---------------------------------------------------------------------------

/// 一边按视频拍抽干环，一边量。调用方自己控制放不放声音。
struct Run {
  long long real_frames = 0;
  int max_available = 0;
  int ticks = 0;
  int underruns = 0;
  float peak = 0.0f;
  std::uint64_t packets = 0;
  std::uint64_t audible = 0;
  double wall = 0.0;
};

Run drain_for(WasapiCapture& cap, int seconds) {
  Run r;
  r.packets = cap.packets();
  r.audible = cap.audible_packets();

  std::vector<float> buf(static_cast<std::size_t>(kPullFrames) * 2);
  auto const t0 = std::chrono::steady_clock::now();
  r.ticks = seconds * 1000 / kPullMs;
  for (int i = 0; i < r.ticks; ++i) {
    std::fill(buf.begin(), buf.end(), 0.0f);
    int const got = cap.pull_add(buf.data(), kPullFrames);
    r.real_frames += got;
    r.max_available = std::max(r.max_available, cap.available());
    if (got < kPullFrames) ++r.underruns;
    for (float v : buf) {
      float const a = std::fabs(v);
      if (a > r.peak) r.peak = a;
    }
    // 按拍睡到下一个整拍，而不是 sleep_for(40ms) —— 后者会把每拍的耗时累加进去，
    // 于是"墙钟"比"拍数 × 40ms"长，判据就自己偏了。
    std::this_thread::sleep_until(t0 + std::chrono::milliseconds(
        static_cast<long long>(i + 1) * kPullMs));
  }
  r.wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  r.packets = cap.packets() - r.packets;
  r.audible = cap.audible_packets() - r.audible;

  std::printf("       %d 秒：取到 %lld 帧（墙钟 %.2f s ⇒ %.1f%% 实时）\n", seconds,
              r.real_frames, r.wall, 100.0 * r.real_frames / r.wall / kRate);
  std::printf("       包 %llu（非静音 %llu），峰值 %.4f，欠载 %d/%d 拍，环里最多积压 %d 帧（%.0f ms）\n",
              static_cast<unsigned long long>(r.packets),
              static_cast<unsigned long long>(r.audible), r.peak, r.underruns, r.ticks,
              r.max_available, 1000.0 * r.max_available / kRate);
  return r;
}

bool has_endpoint(AudioFlow f) {
  std::string e;
  return !list_audio_endpoints(f, e).empty();
}

}  // namespace

int main() {
  Log::set_level(LogLevel::Info);

  std::string const wav = d2o_test::write_tone_wav("wasapi");
  if (wav.empty()) {
    std::printf("写不出测试用的 WAV，后面的声音判据没法做\n");
  } else {
    std::printf("测试音：%s（1 kHz 正弦，循环播放）\n", wav.c_str());
  }

  // ---------------------------------------------------------------- [1] 枚举
  std::printf("\n[1] 端点是这一层的输入，先确认枚举得到\n");
  {
    std::string err;
    auto const render = list_audio_endpoints(AudioFlow::Render, err);
    check(err.empty(), "枚举播放端点不报错" + (err.empty() ? std::string() : "：" + err));
    check(!render.empty(), "这台机器上至少有一个播放端点");
    bool names_ok = !render.empty();
    bool dflt = false;
    for (auto const& ep : render) {
      if (ep.name.empty()) names_ok = false;
      if (ep.is_default) dflt = true;
    }
    // 名字这条必须**单独断言非空**：FriendlyName 的 PROPERTYKEY 字节抄错时的
    // 症状是"设备名是空的"，而"列出了几个设备"完全正常（见陷阱 66）。
    check(names_ok, "每个播放端点的名字都非空（PROPERTYKEY 抄对了）");
    check(dflt, "认得出哪个是默认播放端点");
  }

  // --------------------------------------------- [2] 环回：出声时采得到吗
  std::printf("\n[2] 环回 —— 系统正在放声音的这段时间里，采到了吗\n");
  if (!has_endpoint(AudioFlow::Render) || wav.empty()) {
    skip("没有播放端点（或写不出 WAV），[2] 跳过");
  } else {
    WasapiCapture cap;
    WasapiCapture::Options opt;
    opt.flow = AudioFlow::Render;
    opt.loopback = true;
    opt.label = "系统声音";
    std::string err;
    bool const started = cap.start(opt, err);
    check(started, "start() 成功" + (started ? std::string() : "：" + err));
    if (!started) {
      skip("起不来就没法继续量");
    } else {
      check(cap.state() == WasapiCapture::State::Ok, "状态是 Ok");
      check(!cap.pacing_text().empty(), "说得出自己走的是哪条路：" + cap.pacing_text());
      note("设备：" + cap.status_text());

      d2o_test::Tone tone(wav);
      check(tone.start(), "测试音开始播放");
      drain_for(cap, 1);          // 起播那一秒丢掉：引擎起转要时间，实测只有 67%
      Run const r = drain_for(cap, 4);
      tone.stop();

      check(cap.reopen_count() == 0, "这几秒里没有发生设备重开");
      check(cap.dropped_frames() == 0, "环没有溢出（按实时节奏取就不该丢）");

      // 环回的硬判据只有这一条：**出声期间必须真的收到了声音**。
      // 事件死了的时候这里是 0 包 0 帧，而其余一切都"正常"。
      check(r.packets > 0, d2o::fmt("出声期间收到了包（%llu 个）",
                                   static_cast<unsigned long long>(r.packets)));
      check(r.audible > 0, d2o::fmt("其中有非静音的包（%llu 个）—— 不是一条死音轨",
                                   static_cast<unsigned long long>(r.audible)));
      check(r.peak > 0.01f, d2o::fmt("采到的样本里有声音（峰值 %.4f）", r.peak));
      double const live = r.real_frames / r.wall / kRate;
      check(live > 0.85, d2o::fmt("稳态下跟得上 85%% 的实时（实测 %.1f%%）", live * 100));

      cap.stop();
      cap.stop();  // 幂等
      check(cap.state() == WasapiCapture::State::Idle, "stop() 之后是 Idle，重复调用不炸");
    }
  }

  // ------------------------------------- [3] 环回：不出声时**不该**有数据
  std::printf("\n[3] 环回在系统完全不出声时应该一个包都不来（这是行为契约，不是缺陷）\n");
  if (!has_endpoint(AudioFlow::Render)) {
    skip("没有播放端点，[3] 跳过");
  } else {
    // 等声音停了、播放引擎也转凉了再看。引擎有闲置超时，刚停就测会看到
    // 一段残余（实测过：停播后还能收到几百毫秒的尾音）。
    std::this_thread::sleep_for(std::chrono::seconds(3));
    WasapiCapture cap;
    WasapiCapture::Options opt;
    opt.flow = AudioFlow::Render;
    opt.loopback = true;
    opt.label = "系统声音";
    std::string err;
    if (!cap.start(opt, err)) {
      check(false, "start() 失败：" + err);
    } else {
      Run const r = drain_for(cap, 2);
      // **不能断言"一定为 0"**：跑测试的时候用户可能正在放音乐，那就该有数据。
      // 能断言的是它的**形状**：不崩、不丢、不把环顶爆，而且状态仍然是 Ok
      // —— 环回安静是正常现象，不该被报成故障。
      check(cap.state() == WasapiCapture::State::Ok, "不出声时状态仍然是 Ok（安静不是故障）");
      check(cap.dropped_frames() == 0, "不出声时环是空的，没有溢出");
      note(r.packets == 0
               ? "没有任何声音在播放，环回 0 包 0 帧 —— 和实测一致，正常"
               : d2o::fmt("这台机器上有别的声音在放（%llu 包，峰值 %.4f），"
                          "所以看不出「安静」那一态",
                          static_cast<unsigned long long>(r.packets), r.peak));
      cap.stop();
    }
  }

  // ----------------------------------------------- [4] 麦克风：硬判据在这
  std::printf("\n[4] 麦克风 —— 这一路「样数量 ≈ 墙钟」是硬判据（实测 100.0%%）\n");
  if (!has_endpoint(AudioFlow::Capture)) {
    skip("这台机器上没有录音端点");
  } else {
    WasapiCapture cap;
    WasapiCapture::Options opt;
    opt.flow = AudioFlow::Capture;
    opt.loopback = false;  // 麦克风不环回；就算误传了 true 也该被忽略
    opt.label = "麦克风";
    std::string err;
    bool const started = cap.start(opt, err);
    check(started, "麦克风 start() 成功" + (started ? std::string() : "：" + err));
    if (started) {
      // 忽略第一秒：流的启动延迟必然吃掉一截，那不是判据要管的事。
      drain_for(cap, 1);
      Run const r = drain_for(cap, 4);
      double const ratio = r.real_frames / r.wall / kRate;
      check(ratio > 0.95 && ratio < 1.05,
            d2o::fmt("稳态下和墙钟差在 5%% 以内（实测 %.2f%%）", ratio * 100));
      check(r.packets > 0, d2o::fmt("一直在出包（%llu 个）—— 采集端点不该有空窗",
                                    static_cast<unsigned long long>(r.packets)));
      check(cap.dropped_frames() == 0, "环没有溢出");
      cap.stop();
    }
  }

  // ------------------------------------------- [5] pull_add 的语义
  std::printf("\n[5] pull_add 是「累加」而不是「覆盖」（混音靠这条）\n");
  {
    WasapiCapture cap;  // 没 start()，环是空的
    float buf[8];
    for (float& v : buf) v = 0.25f;
    int const got = cap.pull_add(buf, 4);
    bool untouched = true;
    for (float v : buf) {
      if (std::fabs(v - 0.25f) > 1e-9f) untouched = false;
    }
    check(got == 0 && untouched, "没有数据时返回 0，且一个字节都不动 dst");
    check(cap.available() == 0, "available() 在没有数据时是 0");
  }

  std::printf("\n通过 %d 项，失败 %d 项，跳过 %d 项。\n", g_pass, g_fail, g_skip);
  return g_fail == 0 ? 0 : 1;
}
