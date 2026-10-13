/**
 * @file tests/audio_mixer_check.cpp
 * @brief 混音器：时间轴归谁、安静时音轨会不会短一截、增益和 headroom 对不对。
 *
 * 这个测试存在的理由是 **`audio_mixer.h` 的那条设计** —— 时间轴由编码线程按
 * 视频拍数出来、源端不够补零 —— 有几条**看起来完全多余的**不变量，而它们
 * 每一条都能被一个"顺手优化"悄悄破坏，破坏之后症状又都很远：
 *
 *   * `[1]`/`[2]`：**没有任何声音进来的时候，音轨照样要按拍长出来。** 这是
 *     整个设计的目的。谁要是把 `fill()` 改成"只按收到的样本数推进时间轴"
 *     （那看起来更"省零"、更"诚实"），系统一安静音轨就短一截 —— 而用户看到的
 *     是"录像里声音比画面早结束"，谁也想不到去查混音器。这两节就是钉这一条的。
 *     **分工要分清**：`[1]` 一个源都不开（不碰设备），确定性地覆盖"源端一个样本
 *     都不来"；`[2]` 开真环回，覆盖"设备在、但没人出声"。`[2]` **不判**源端有没有
 *     来数据 —— 那取决于这台机器的播放引擎当时停没停（实测两种状态都有过），
 *     是环境而不是产品契约。见 `[2]` 里那段注释。
 *   * `[5]`：**两路是真的相加，不是后一路把前一路覆盖掉。** `dst[i] +=` 写成
 *     `dst[i] =` 时只有一个源能听见（后处理的那一路），而**单源测试全绿**
 *     —— 只有两路同开才露馅。这一节用"第二路增益为 0"把这件事变成一个
 *     确定性的断言：覆盖式写法在那时会输出全零。
 *   * `[1]`：`stop()` 之后状态必须回 `off`，不能是 `error`。靠 `running` 反推
 *     状态时，一个正常停掉的混音器会报"坏了"，而且**只在停过一次之后**才这样。
 *
 * `[3]`/`[4]`/`[6]` 才需要真的设备：出声时采得到、增益乘得对、电平表会释放。
 *
 * ⚠️ **`[4]`/`[5]` 量的是两个混音器**并排**拉的比值，而不是「先量一遍增益 1.0、
 * 再量一遍增益 0.5、除一下」。** 原因是这台机器的播放端点在**冷引擎起播**时有一段
 * 随时间衰减的增益：实测逐 40 ms 的形状（`out/audio-probe/burst_probe.cpp`）是从
 * 0.8936 **单调平滑**地掉到稳态 0.2032、跨 80 拍连续，而 `tests/wasapi_check.cpp`
 * 那个进程里**一个混音器都没有**，读到的还是同一个 0.8936 ⇒ 它在采集层之下、
 * 在端点的 DSP 里，不是我们的任何一行代码。
 *
 * 拿它当分母，比值就成了"这台机器当时稳没稳"的函数 —— 陷阱 87 的形状（判据依赖了
 * 一个没写进前提的环境条件）。两个混音器在**同一个 40 ms 拍**里各 `fill()` 一次，
 * 端点给的是同一份增益，于是它在比值里**精确约掉**：冷启动瞬态、稳态各是多少都不
 * 再参与，连"要等多久才算稳"这个前提都不需要。想把 `[4]`/`[5]` 改回"先后各量一次"
 * 之前，请先读 `[3]` 里那两行实测读数 —— 它们正是这条注释的由来。
 *
 * 编译（在项目根目录下）：
 *   g++ -std=c++17 -Isrc -Ithird_party/ffmpeg/include tests/audio_mixer_check.cpp \
 *       src/audio/audio_mixer.cpp src/audio/wasapi_capture.cpp src/audio/audio_device.cpp \
 *       src/common/log.cpp src/common/text.cpp -o build/audio_mixer_check.exe \
 *       -Lthird_party/ffmpeg/lib -lswresample -lavutil -lole32 -lmmdevapi -lwinmm
 */

#include <windows.h>

#include "audio/audio_device.h"
#include "audio/audio_mixer.h"
#include "common/log.h"
#include "tone_wav.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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
constexpr int kPullMs = 40;                          // 25 fps，和默认采集一致
constexpr int kFrames = kRate * kPullMs / 1000;      // 1920

/// 按拍喂 `fill()`，顺便量输出。**按拍睡到整点**而不是 sleep_for：
/// 后者会把每拍自己的耗时累加进去，于是"墙钟"比"拍数×40ms"长。
struct Run {
  int ticks = 0;
  int full_blocks = 0;      ///< 一次就供满的拍数（没补零）
  float dst_peak = 0.0f;    ///< 直接量 `dst` —— 这才是"进了录像的东西"
  float dst_rms = 0.0f;
  AudioLevels lv;           ///< 混音器自己报的电平表
  double wall = 0.0;
};

Run run_for(AudioMixer& m, int seconds) {
  Run r;
  std::vector<float> buf(static_cast<std::size_t>(kFrames) * 2);
  double sq = 0.0;
  long long n = 0;
  auto const t0 = std::chrono::steady_clock::now();
  r.ticks = seconds * 1000 / kPullMs;
  for (int i = 0; i < r.ticks; ++i) {
    int const got = m.fill(buf.data(), kFrames);
    if (got >= kFrames) ++r.full_blocks;
    for (float v : buf) {
      float const a = std::fabs(v);
      if (a > r.dst_peak) r.dst_peak = a;
      sq += static_cast<double>(v) * v;
      ++n;
    }
    std::this_thread::sleep_until(
        t0 + std::chrono::milliseconds(static_cast<long long>(i + 1) * kPullMs));
  }
  r.wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  r.dst_rms = n > 0 ? static_cast<float>(std::sqrt(sq / n)) : 0.0f;
  r.lv = m.levels();
  std::printf("       %d 秒：输出峰值 %.4f 有效值 %.4f，电平表 peak %.4f/%.4f rms %.4f/%.4f，"
              "供满 %d/%d 拍\n",
              seconds, r.dst_peak, r.dst_rms, r.lv.peak_l, r.lv.peak_r, r.lv.rms_l,
              r.lv.rms_r, r.full_blocks, r.ticks);
  return r;
}

/// 只按拍推 `fill()`，不测量。**电平表的衰减是沿音轨时间走的**，所以量它的
/// 释放必须一边量一边继续喂 —— 不喂的话它停在那儿不动，而那**不是保持器行为**，
/// 是"没有音轨在前进"（第一版测试就是在停声之后只 sleep 不 fill，于是把一条
/// 正确的实现报成了失败）。
AudioLevels pump_for(AudioMixer& m, int ms) {
  std::vector<float> buf(static_cast<std::size_t>(kFrames) * 2);
  auto const t0 = std::chrono::steady_clock::now();
  int const n = ms / kPullMs;
  for (int i = 0; i < n; ++i) {
    m.fill(buf.data(), kFrames);
    std::this_thread::sleep_until(
        t0 + std::chrono::milliseconds(static_cast<long long>(i + 1) * kPullMs));
  }
  return m.levels();
}

/// 并排拉两个混音器，返回各自的峰值。
///
/// **为什么是并排，而不是「先量 A 再量 B」**：见文件头那段 ⚠️ —— 这台机器的播放
/// 端点在冷引擎起播时有一段约 3 秒的衰减增益（0.8936 → 0.2032，逐 40 ms 单调平滑，
/// 而没有任何混音器的 `wasapi_check` 读到的是同一个 0.8936）。测两次就是把这个
/// 瞬态放进分母，比值于是成了"这台机器当时稳没稳"的函数。
///
/// 两个混音器在**同一个 40 ms 拍**里各 `fill()` 一次，端点给的是同一份增益，
/// 它在比值里**精确约掉**。这比"等它稳了再量"更强：连"要等多久"都不需要假设。
struct Pair {
  float a_peak = 0.0f;
  float b_peak = 0.0f;
  int a_full = 0;   ///< A 供满的拍数
  int b_full = 0;   ///< B 供满的拍数
  int ticks = 0;
};

Pair run_pair(AudioMixer& a, std::string const& a_label, AudioMixer& b,
              std::string const& b_label, int seconds) {
  Pair p;
  std::vector<float> bufa(static_cast<std::size_t>(kFrames) * 2);
  std::vector<float> bufb(static_cast<std::size_t>(kFrames) * 2);
  p.ticks = seconds * 1000 / kPullMs;
  auto const t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < p.ticks; ++i) {
    if (a.fill(bufa.data(), kFrames) >= kFrames) ++p.a_full;
    if (b.fill(bufb.data(), kFrames) >= kFrames) ++p.b_full;
    for (float v : bufa) p.a_peak = std::max(p.a_peak, std::fabs(v));
    for (float v : bufb) p.b_peak = std::max(p.b_peak, std::fabs(v));
    std::this_thread::sleep_until(
        t0 + std::chrono::milliseconds(static_cast<long long>(i + 1) * kPullMs));
  }
  std::printf("       并排 %d 秒：%s 峰值 %.4f（供满 %d/%d），%s 峰值 %.4f（供满 %d/%d）\n",
              seconds, a_label.c_str(), p.a_peak, p.a_full, p.ticks, b_label.c_str(),
              p.b_peak, p.b_full, p.ticks);
  return p;
}

bool has_endpoint(AudioFlow f) {
  std::string e;
  return !list_audio_endpoints(f, e).empty();
}

AudioMixer::Options opts(bool sys, bool mic) {
  AudioMixer::Options o;
  o.system.enabled = sys;
  o.mic.enabled = mic;
  return o;
}

}  // namespace

int main() {
  Log::set_level(LogLevel::Info);

  std::string const wav = d2o_test::write_tone_wav("mixer");
  bool const have_sys = has_endpoint(AudioFlow::Render);
  bool const have_mic = has_endpoint(AudioFlow::Capture);
  std::printf("测试音：%s\n", wav.empty() ? "(写不出来)" : wav.c_str());

  // ------------------------- [1] 一路都不开：fill 照样要写满、时间轴照样要涨
  std::printf("\n[1] 音频整个关着时，fill() 仍然必须写满并推进时间轴\n");
  {
    AudioMixer m;
    std::string err;
    bool const ok = m.start(opts(false, false), err);
    // **"音频关着"不是失败。** 它是一个被成功落实了的配置，调用方不该被迫去
    // 区分"没配"和"起不来"。
    check(ok, "start() 在没有任何源时返回成功" + (ok ? std::string() : "：" + err));
    check(m.state_text() == "off", "状态是 off（实测 " + m.state_text() + "）");

    std::vector<float> buf(static_cast<std::size_t>(kFrames) * 2, 0.5f);  // 哨兵
    int const got = m.fill(buf.data(), kFrames);
    check(got == 0, "没有源在供数据，返回 0");
    bool all_zero = true;
    for (float v : buf) {
      if (v != 0.0f) all_zero = false;
    }
    // 是**覆盖**不是累加（和 `WasapiCapture::pull_add` 的约定正好相反），
    // 所以哨兵必须被整个抹掉 —— 留着 0.5 就是"没写满"，会变成恒定直流。
    check(all_zero, "dst 被完整覆盖成零（哨兵 0.5 一个不剩）");

    for (int i = 0; i < 49; ++i) m.fill(buf.data(), kFrames);
    check(m.frames_out() == 50ULL * kFrames,
          d2o::fmt("时间轴照涨：50 拍 = %llu 帧（%d 秒的音频）",
                   static_cast<unsigned long long>(m.frames_out()), kFrames * 50 / kRate));
    check(m.zero_filled_frames() == 50ULL * kFrames,
          "这 50 拍全部记成补零（源端一个样本都没来）");
    auto const srcs = m.sources();
    check(srcs.size() == 2, "sources() 恒为两项（关掉的那路也在，state 是 off）");
    check(!srcs.empty() && srcs[0].state == "off" && srcs[1].state == "off",
          "两项都报 off");

    // 停掉之后**不能**变成 error：配置里 enabled 还是 true（配置没变，只是停了），
    // 靠 running 反推状态的话这里会报"坏了"。
    m.stop();
    check(m.state_text() == "off", "stop() 之后状态回 off（实测 " + m.state_text() + "）");
  }

  // ------------------- [2] 环回开着但系统安静：**音轨时长不能短一截**（核心）
  std::printf("\n[2] 环回开着、系统完全不出声 —— 音轨照样按拍长出来（这是本设计的全部目的）\n");
  if (!have_sys) {
    skip("没有播放端点");
  } else {
    AudioMixer m;
    std::string err;
    if (!m.start(opts(true, false), err)) {
      check(false, "start() 失败：" + err);
    } else {
      // 先把可能还在响的声音等掉，再开始量。
      std::this_thread::sleep_for(std::chrono::seconds(2));
      std::uint64_t const before = m.frames_out();
      Run const r = run_for(m, 4);
      std::uint64_t const grew = m.frames_out() - before;

      // ★ 这一条是整个测试的重点：**时间轴的长度只由拍数决定**。源端给不给数据、
      //   给多少，都不参与。所以它断言的是 `ticks * kFrames`，不是"大约 4 秒"。
      check(grew == static_cast<std::uint64_t>(r.ticks) * kFrames,
            d2o::fmt("4 秒里时间轴长了 %llu 帧 = 恰好 %d 秒的音频",
                     static_cast<unsigned long long>(grew), r.ticks * kFrames / kRate));
      // 安静时"输出里没有声音"才是可观测的那一面。它比"源端没来数据"稳 ——
      // 见下面那段注释。
      check(r.dst_peak < 0.001f,
            d2o::fmt("输出是纯静音（峰值 %.4f）—— 录进去的确实是安静，不是杂音", r.dst_peak));
      check(m.state_text() == "ok",
            "状态仍然是 ok —— **系统安静不是故障**（实测 " + m.state_text() + "）");

      // ⚠️ **不要断言 `zero_filled_frames() > 0`。** 这条原来是这么写的（理由是
      //    陷阱 81 那个实测：环回在系统不出声时一个包都不来），它在测这台机器
      //    **当时的**环回行为，不是在测混音器。实测（2026-10-08）：同一台机器上
      //    `--record --audio system` 全程不放声音，8 秒里照样收到 **376 个包、
      //    补零 0 帧** —— 播放引擎根本没停，只是每一包的内容全零。于是这条断言
      //    在自己完全健康的机器上变红，而产品侧的每一条不变量都还是绿的。
      //    这正是陷阱 82 的形状：拿"被测对象在某个环境下的表现"当判据。
      //
      //    "源端到底给没给数据"这件事本身**不影响任何一条产品契约**（上面那条
      //    时间轴断言两种情形下都成立）。所以这里只**报告**，不判。真正要验
      //    "源端一个样本都不来"时怎么办，由 [1] 确定性地覆盖（一个源都不开，
      //    不碰设备，因此不受这台机器的环回状态影响）。
      if (m.zero_filled_frames() > 0) {
        note(d2o::fmt("这一跑源端确实没数据：补零 %llu 帧 —— 补零路径被走到了",
                      static_cast<unsigned long long>(m.zero_filled_frames())));
      } else {
        note(d2o::fmt("这一跑的环回一直在出包（%d/%d 拍一次供满），所以**补零路径没被"
                      "走到** —— 这台机器的播放引擎不会因为没人出声就停下来。"
                      "补零那条由 [1] 确定性地覆盖，不在这里重复。",
                      r.full_blocks, r.ticks));
      }
      m.stop();
    }
  }

  // ------------------------------------------------- [3] 出声时采得到、电平表涨
  std::printf("\n[3] 出声时输出里真的有声音，电平表跟着涨\n");
  if (!have_sys || wav.empty()) {
    skip("没有播放端点（或写不出 WAV）");
  } else {
    d2o_test::Tone tone(wav);
    AudioMixer m;
    auto o = opts(true, false);
    o.system.gain = 1.0f;
    std::string err;
    if (!m.start(o, err) || !tone.start()) {
      check(false, "起不来：" + err);
    } else {
      // 头一秒单独量一次，**只为把冷引擎那个起播瞬态印在日志上** —— 它比后面那个
      // 窗口高是正常的（见文件头那段 ⚠️ 与 [4] 上面那段），这里不判它。
      // 少了这一行，下次看到日志里两个不一样的峰值时又得从头查一遍。
      Run const w1 = run_for(m, 1);
      Run const r = run_for(m, 2);
      check(r.dst_peak > 0.01f,
            d2o::fmt("输出里有声音（峰值 %.4f）—— 增益 1.0", r.dst_peak));
      check(r.lv.peak_l > 0.01f,
            d2o::fmt("电平表跟着涨（peak_l %.4f）", r.lv.peak_l));
      check(r.full_blocks > 0, d2o::fmt("有拍被供满（%d 拍）", r.full_blocks));
      // **只报告，不判。** 它是这台机器端点的行为，不是混音器的契约（陷阱 87）。
      if (w1.dst_peak > r.dst_peak * 1.1f) {
        note(d2o::fmt("起播头一秒 %.4f 比之后两秒 %.4f 高 —— 这台机器的播放端点在"
                      "冷引擎起播时有一段随时间衰减的增益（实测约 3 秒）。不是缺陷；"
                      "[4]/[5] 的比值不受它影响（端点增益在比值里约掉了）。",
                      w1.dst_peak, r.dst_peak));
      }
      m.stop();
    }
  }

  // --------------------------- [4] 增益：两个混音器并排，端点增益在比值里约掉
  std::printf("\n[4] 增益 0.5 时输出峰值应当约等于增益 1.0 时的一半\n");
  if (!have_sys || wav.empty()) {
    skip("没有播放端点（或写不出 WAV）");
  } else {
    d2o_test::Tone tone(wav);
    // 并排的两套配置**只差增益这一个变量**。两个混音器在同一个 40 ms 拍里各 fill
    // 一次，端点给的是同一份（随时间变的）增益，于是它精确约掉 —— 断言因此是
    // 机器无关的。详见 run_pair() 与文件头那段。
    AudioMixer a, b;
    auto oa = opts(true, false);
    oa.system.gain = 1.0f;
    auto ob = opts(true, false);
    ob.system.gain = 0.5f;
    std::string err;
    if (!a.start(oa, err) || !b.start(ob, err)) {
      check(false, "并排起不来：" + err);
    } else if (!tone.start()) {
      check(false, "测试音起不来");
    } else {
      Pair const p = run_pair(a, "增益 1.0", b, "增益 0.5", 2);
      // **先确认两路都真的接到了信号。** 并排拉的第二个环回客户端要是拿不到数据，
      // b_peak 会是 0 而比值变成 0 —— 那同样落在区间外，报出来却是"增益没乘对"，
      // 指向完全错误的方向。这条守卫把那种情形单独挑出来。
      check(p.a_peak > 0.01f && p.b_peak > 0.01f,
            d2o::fmt("并排两路都接到了信号（%.4f / %.4f）", p.a_peak, p.b_peak));
      double const ratio = p.a_peak > 0.0f ? p.b_peak / p.a_peak : 0.0;
      check(ratio > 0.49 && ratio < 0.51,
            d2o::fmt("峰值比 %.4f（期望 0.500，实测 %.4f / %.4f）", ratio, p.b_peak,
                     p.a_peak));
      a.stop();
      b.stop();
    }
  }

  // ------------------------------- [5] 两路同开：各乘 1/√2，而且是真的**相加**
  std::printf("\n[5] 两路同开时各乘 1/√2 —— 而且第二路不是把第一路覆盖掉\n");
  if (!have_sys || wav.empty()) {
    skip("没有播放端点（或写不出 WAV）");
  } else if (!have_mic) {
    skip("没有采集端点 —— 这一节要的就是麦克风那一路也在");
  } else {
    d2o_test::Tone tone(wav);
    // 并排的两套配置：**只系统**（1 路，×1.0）与**系统 + 麦克风**（2 路，各 ×0.7071）。
    // 麦克风的增益设成 0，于是它贡献恒为 0，比值里只剩 headroom 这一个变量 ——
    // 断言就能是精确的 0.7071 而不是一个含糊的区间。
    //
    // 顺带这一条把"相加 vs 覆盖"也钉死了：`dst[i] += ` 写成 `dst[i] = ` 的话，
    // 麦克风是**后**处理的那一路，于是整个输出会变成 0 × headroom = 全零。
    // 单源测试对这件事完全无感 —— 只有这里看得出来。
    AudioMixer a, b;
    auto oa = opts(true, false);
    oa.system.gain = 1.0f;
    auto ob = opts(true, true);
    ob.system.gain = 1.0f;
    ob.mic.gain = 0.0f;
    std::string err;
    if (!a.start(oa, err) || !b.start(ob, err)) {
      check(false, "并排起不来：" + err);
    } else if (!tone.start()) {
      check(false, "测试音起不来");
    } else {
      Pair const p = run_pair(a, "只系统", b, "系统+麦克风(增益 0)", 2);
      // 这条守卫的失败信息必须把**两种可能**都写出来：B 输出全零既是"第二个环回
      // 客户端拿不到数据"，也**正是**覆盖式写法的症状，这两者在这一点上分不开。
      // 只写一种的话，下一个看到这条红的人会顺着错的那个方向查下去。
      check(p.a_peak > 0.01f && p.b_peak > 0.01f,
            d2o::fmt("并排两路都接到了信号（%.4f / %.4f）—— B 若是 0，要么是第二个"
                     "环回客户端拿不到数据，要么是 `dst[i] = ` 把前一路覆盖掉了",
                     p.a_peak, p.b_peak));
      double const ratio = p.a_peak > 0.0f ? p.b_peak / p.a_peak : 0.0;
      check(std::fabs(ratio - 0.7071) < 0.02,
            d2o::fmt("峰值比 %.4f（期望 1/√2 = 0.7071，实测 %.4f / %.4f）", ratio,
                     p.b_peak, p.a_peak));
      auto const srcs = b.sources();
      check(srcs.size() == 2 && srcs[0].enabled && srcs[1].enabled,
            "sources() 里两路都报 enabled");
      a.stop();
      b.stop();
    }
  }

  // ------------------------------------------------- [6] 电平表的释放是往下走的
  std::printf("\n[6] 声音停了之后电平表要往下走（不然它只是一块峰值保持器）\n");
  if (!have_sys || wav.empty()) {
    skip("没有播放端点");
  } else {
    AudioMixer m;
    std::string err;
    if (!m.start(opts(true, false), err)) {
      check(false, "start() 失败：" + err);
    } else {
      {
        d2o_test::Tone tone(wav);
        tone.start();
        run_for(m, 1);
        Run const loud = run_for(m, 2);
        tone.stop();
        note(d2o::fmt("停声前 peak_l %.4f", loud.lv.peak_l));

        // 先确认"没有音轨在前进时电平表就停住"这件事是**设计**而不是缺陷：
        // 它由 `fill()` 驱动（时间轴的主人），不是一块自己的墙钟计时器。
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        check(std::fabs(m.levels().peak_l - loud.lv.peak_l) < 1e-3f,
              "不喂 fill() 时电平表静止 —— 它走的是音轨时间，不是墙钟");

        // 停声之后连着推 4 段，peak_l 必须一步一步往下。
        float prev = loud.lv.peak_l;
        bool monotone = true;
        float last = prev;
        for (int i = 0; i < 4; ++i) {
          AudioLevels const lv = pump_for(m, 400);
          if (lv.peak_l > prev + 1e-6f) monotone = false;
          prev = lv.peak_l;
          last = lv.peak_l;
        }
        check(monotone, "停声后 peak_l 一路单调下降（释放，不是保持）");
        check(last < loud.lv.peak_l * 0.5f,
              d2o::fmt("1.6 秒后掉到一半以下（%.4f → %.4f）", loud.lv.peak_l, last));
      }
      m.stop();
    }
  }

  std::printf("\n通过 %d 项，失败 %d 项，跳过 %d 项。\n", g_pass, g_fail, g_skip);
  return g_fail == 0 ? 0 : 1;
}
