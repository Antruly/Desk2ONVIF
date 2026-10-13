/**
 * @file tests/audio_track_check.cpp
 * @brief `AudioTrack`：**音轨的样本总数只由拍数决定**，与设备、与此刻有没有声音无关。
 *
 * ## 为什么这条要单独测，而且必须在"源端一个样本都不给"的条件下测
 *
 * 音频时间轴的主人只能是编码线程（依据见 `audio_track.h` 的文件头：环回在系统
 * 不出声时**一个包都不来**，实测 0 包 0 帧，不是"来了带 SILENT 标志的静音"）。
 * 而这条链路上最容易写错、写错之后症状最远的一件事，恰恰是把 `tick()` 实现成
 * "每拍给 `rate/fps` 个样本"：
 *
 *   - 48000/25 = 1920 **整除**，所以默认档位下它一次都不会露馅；
 *   - 而帧率是可改的（画质档位里有 12/20/25/30），`48000/7 = 6857.14` ——
 *     每拍取 6857，**一小时短掉 51 秒**。症状是"录像越长声音跑得越前"，
 *     没有任何一个字指向一次整除。
 *
 * 所以 `[3]` 专门用 7 fps 跑 70 拍，断言 `frames_out() == 70*48000/7`（整数除法的
 * 精确值）。把实现换回 `frames_per_tick()` 那种每拍固定值，这一条**当场变红**
 * （479990 vs 480000）—— 除此之外没有任何断言看得出来。
 *
 * ## 为什么"这一整段跑得飞快"本身就是一个判据
 *
 * `[1]` 推 25 拍只花几毫秒，却要求音轨长到 1.000 秒。这是**物理上不可能**由
 * 源端供出来的（采集环最深 300 ms = 14400 帧），所以它顺带证明了"时间轴不是
 * 按收到了多少样本推的"：断言 `frames_out() - zero_filled() <= 环深`。
 * 换成设备驱动的实现，这一个数就等于音轨长度本身，两条断言同时红。
 *
 * ## 编译（在项目根目录下）
 *   g++ -std=c++17 -Wall -Wextra -Isrc -Ithird_party/ffmpeg/include \
 *       tests/audio_track_check.cpp \
 *       src/app/audio_track.cpp src/audio/audio_mixer.cpp \
 *       src/audio/wasapi_capture.cpp src/audio/audio_device.cpp \
 *       src/encoder/aac_encoder.cpp src/common/log.cpp src/common/text.cpp \
 *       -o build/audio_track_check.exe \
 *       -Lthird_party/ffmpeg/lib -lavcodec -lavutil -lswresample \
 *       -lole32 -lmmdevapi -lwinmm
 *
 * 运行（FFmpeg 的 DLL 在 build/bin 下，所以 PATH 上要有它和 mingw64\bin）：
 *   ./build/audio_track_check.exe
 */

#include <windows.h>

#include "app/audio_track.h"
#include "audio/audio_device.h"
#include "common/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstdint>
#include <cstdio>
#include <string>
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
constexpr int kCh = 2;
/// 和 `AudioMixer::Options::ring_frames` 的默认值一致。`[1]` 拿它当"源端最多
/// 能供出多少"的上界。
constexpr std::uint64_t kRingFrames = 14400;

bool has_endpoint(AudioFlow f) {
  std::string e;
  return !list_audio_endpoints(f, e).empty();
}

/// 收 AU 的桩。记下每个包的 pts 与大小 —— 只记总数的话，"时间轴上有个洞"
/// 和"一切正常"长得一模一样，而那听起来是咔一下。
struct AuLog {
  std::vector<std::int64_t> pts;
  std::vector<int> sizes;
  std::uint64_t bytes = 0;

  PacketSink sink() {
    return [this](AVPacket& p) {
      pts.push_back(p.pts);
      sizes.push_back(p.size);
      bytes += static_cast<std::uint64_t>(p.size);
    };
  }

  /// 从第一个 AU 的起点算起，AU 序列一共覆盖了多少样本。
  /// **不依赖"首包 pts 是 −启动填充"这个绝对值**（虽然 `[1]` 也顺手盯着它）：
  /// 只数相邻差值，这样"个位数的偏移"不会把它变成一个脆断言。
  std::int64_t span(int frame_size) const {
    if (pts.empty()) return 0;
    return pts.back() - pts.front() + frame_size;
  }
};

/// 按帧率推 `n` 拍。
void pump(AudioTrack& t, AuLog& log, int n) {
  PacketSink s = log.sink();
  for (int i = 0; i < n; ++i) t.tick(s);
}

AudioTrackOptions with_source(int sys, int mic, int fps) {
  AudioTrackOptions o;
  o.fps = fps;
  o.mixer.system.enabled = sys != 0;
  o.mixer.mic.enabled = mic != 0;
  return o;
}

}  // namespace

int main() {
  Log::set_level(LogLevel::Warn);  // 测试输出只留判定，噪音交给 -v 去看

  bool const have_sys = has_endpoint(AudioFlow::Render);
  bool const have_mic = has_endpoint(AudioFlow::Capture);
  std::printf("本机端点：播放 %s / 录音 %s\n", have_sys ? "有" : "无",
              have_mic ? "有" : "无");
  if (!have_sys && !have_mic) {
    skip("这台机器上一个音频端点都没有 —— 本节全部依赖真实采集");
    std::printf("\n通过 %d 项，失败 %d 项，跳过 %d 项。\n", g_pass, g_fail, g_skip);
    return 0;
  }

  // ============ [1] 25 fps：一拍 = 一帧的时间，样本数只由拍数决定 ============
  std::printf("\n[1] 推 25 拍 = 1.000 秒的音轨，而源端**供不出这么多**（环深只有 300 ms）\n");
  if (!have_sys) {
    skip("没有播放端点（[1] 用环回，因为它在系统安静时一个包都不来 —— "
         "那正是这条设计要对付的现实）");
  } else {
    AudioTrack t;
    std::string err;
    AudioTrackOptions const o = with_source(/*sys=*/1, /*mic=*/0, /*fps=*/25);
    if (!t.start(o, err)) {
      check(false, "start() 失败：" + err);
    } else {
      AuLog log;
      bool every_tick_exact = true;
      for (int n = 1; n <= 25; ++n) {
        t.tick(log.sink());
        if (t.frames_out() != static_cast<std::uint64_t>(n) * kRate / 25) {
          every_tick_exact = false;
        }
      }
      check(every_tick_exact, "每一拍之后 frames_out() 都等于 ticks*48000/25");
      check(t.frames_out() == 48000,
            d2o::fmt("25 拍之后音轨恰好 48000 样本 = 1.000 秒（实测 %llu）",
                     static_cast<unsigned long long>(t.frames_out())));
      check(t.frames_per_tick() == 1920, "名义每拍 1920（48000/25 整除，所以这一档看不出差值法的必要）");

      std::uint64_t const real = t.frames_out() - t.mixer().zero_filled_frames();
      check(real <= kRingFrames,
            d2o::fmt("源端真正供上的只有 %llu 帧（<= 环深 %llu = 300 ms），"
                     "多出来的 %llu 帧全是按拍补的零",
                     static_cast<unsigned long long>(real),
                     static_cast<unsigned long long>(kRingFrames),
                     static_cast<unsigned long long>(t.frames_out() - real)));
      check(t.mixer().zero_filled_frames() > 0,
            "这段时间确实在补零（几毫秒里不可能真采到 1 秒的音频）");
      check(t.mixer().state_text() == "ok",
            "状态仍是 ok —— **环回不出声不是故障**（实测 " + t.mixer().state_text() + "）");

      // AU 的时间轴：相邻两包的 pts 差必须**恰好是一帧**。有洞 = 声音里少一段，
      // 有重叠 = 同一段被播两遍，两者在"总字节数"上完全看不出来。
      int const fs = t.encoder().frame_size();
      bool contiguous = !log.pts.empty();
      for (std::size_t i = 1; i < log.pts.size(); ++i) {
        if (log.pts[i] - log.pts[i - 1] != fs) contiguous = false;
      }
      check(contiguous,
            d2o::fmt("%zu 个 AU 的 pts 逐个相差恰好 %d 个样本（无洞、无重叠）",
                     log.pts.size(), fs));
      check(!log.pts.empty() && log.pts.front() == -t.encoder().initial_padding(),
            d2o::fmt("首个 AU 的 pts 是 %lld = −启动填充 %d（编码器把整条时间轴前移）",
                     log.pts.empty() ? 0LL : static_cast<long long>(log.pts.front()),
                     t.encoder().initial_padding()));
      check(t.encoder().sample_rate() == kRate && t.encoder().channels() == kCh,
            d2o::fmt("编码器用的是 %d Hz / %d 声道 —— 采样率的权威在 mixer 那一份",
                     t.encoder().sample_rate(), t.encoder().channels()));

      // ==================== [2] 冲刷：结尾那不足一帧的尾巴不能丢 ====================
      std::printf("\n[2] 冲刷编码器：悄悄丢掉的那一段**恰好在音轨结尾**\n");
      std::int64_t const span_before = log.span(fs);
      std::size_t const n_before = log.pts.size();
      std::string ferr;
      bool const flushed = t.flush(log.sink(), ferr);
      check(flushed, "flush() 成功" + (flushed ? std::string() : "：" + ferr));
      check(log.pts.size() > n_before,
            d2o::fmt("冲刷真的多编出 %zu 个 AU（FIFO 里那不足一帧的尾巴）",
                     log.pts.size() - n_before));
      check(t.frames_out() == 48000,
            "冲刷**不改变**音轨长度 —— 它只把已经产出的样本编出去，不新增样本");
      std::int64_t const span_after = log.span(fs);
      note(d2o::fmt("覆盖样本：冲前 %lld（音轨 %llu），冲后 %lld",
                    static_cast<long long>(span_before),
                    static_cast<unsigned long long>(t.frames_out()),
                    static_cast<long long>(span_after)));
      check(span_after >= static_cast<std::int64_t>(t.frames_out()),
            "冲完之后覆盖到全部样本 —— 结尾那不到 21 ms 没有丢");
      // 上界给两帧而不是一帧：结尾多出来的是"补零凑满的那一帧"**加上**编码器
      // 自己攒着的粒度，实测多出 1152 个样本（1.1 帧）。判据要卡的是"没有多出
      // 一大段静音"，不是精确到帧。
      check(span_after < static_cast<std::int64_t>(t.frames_out()) + 2 * fs,
            "而且只是补零凑满的那点，没有多出一段静音");
      bool cont_after = true;
      for (std::size_t i = n_before; i < log.pts.size(); ++i) {
        if (log.pts[i] - log.pts[i - 1] != fs) cont_after = false;
      }
      check(cont_after, "冲刷出来的那个 AU 的 pts 接着上一个（时间轴没在这里断）");

      t.stop();
    }
  }

  // ============ [3] 除不尽的帧率：余数不能被丢掉（这条是"每拍固定值"的判据） ============
  std::printf("\n[3] fps=7（48000/7 = 6857.14）：70 拍必须是**恰好** 480000 个样本\n");
  if (!have_sys) {
    skip("没有播放端点");
  } else {
    AudioTrack t;
    std::string err;
    AudioTrackOptions const o = with_source(1, 0, /*fps=*/7);
    if (!t.start(o, err)) {
      check(false, "start() 失败：" + err);
    } else {
      AuLog log;
      pump(t, log, 70);
      std::uint64_t const want = 70ULL * kRate / 7;  // = 480000，10.000 秒
      check(t.frames_out() == want,
            d2o::fmt("70 拍之后是 %llu 个样本（正是 10.000 秒）",
                     static_cast<unsigned long long>(t.frames_out())));
      std::uint64_t const nominal =
          static_cast<std::uint64_t>(t.frames_per_tick()) * 70;
      check(t.frames_per_tick() == 6857, "名义每拍是 6857（48000/7 的商）");
      note(d2o::fmt("按名义每拍算是 %llu 个样本，差 %lld —— 逐拍丢掉的那点余数，"
                    "一小时就是 51 秒",
                    static_cast<unsigned long long>(nominal),
                    static_cast<long long>(want) -
                        static_cast<long long>(nominal)));
      // 判据卡在**一个样本**上（不是 1 ms）：余数丢掉的 10 个样本是 208 µs，
      // 用毫秒级的容差看就是"通过"，而它正是这条链路上最该被抓住的那个量。
      double const sec = t.seconds();
      check(sec > 10.0 - 1.0 / kRate && sec < 10.0 + 1.0 / kRate,
            d2o::fmt("seconds() = %.6f 秒（与 70 拍 / 7 fps 相差不超过一个样本）", sec));
      // 顺带盯住"每拍单独看都对"的那种写法：它在前 7 拍上就已经差了 1 个样本。
      AudioTrack t2;
      AuLog log2;
      if (t2.start(o, err)) {
        pump(t2, log2, 7);
        check(t2.frames_out() == 48000,
              d2o::fmt("连 7 拍这种小数目也必须是整 48000（实测 %llu）—— "
                       "它在第 7 拍上就分出对错了",
                       static_cast<unsigned long long>(t2.frames_out())));
        t2.stop();
      } else {
        check(false, "第二次 start() 失败：" + err);
      }
      t.stop();
    }
  }

  // ============ [4] 参数校验：不该悄悄产出一条数字静音的音轨（陷阱 78） ============
  std::printf("\n[4] 参数校验\n");
  {
    AudioTrack t;
    std::string err;
    AudioTrackOptions none;  // 两路都没勾
    bool const ok = t.start(none, err);
    check(!ok, "两路都没勾时 start() 返回 false");
    check(!ok && (err.find("系统声音") != std::string::npos &&
                  err.find("麦克风") != std::string::npos),
          "错误消息点名了是哪两个开关：" + err);
    check(!t.active(), "失败之后 active() 是 false（调用方按纯视频继续）");

    // 注意：`start()` **必须先调、再拼消息**。写成
    // `check(!t.start(o, err), "…" + err)` 时两个实参的求值顺序是未指定的
    // （GCC 是从右往左），于是消息里的 `err` 是**上一次**留下的那句话 ——
    // 断言照样对，打印出来的人话却是错的，而错的那句读起来完全合理。
    AudioTrackOptions nofps = with_source(1, 0, 0);
    bool const nofps_ok = t.start(nofps, err);
    std::string const nofps_msg = err;
    check(!nofps_ok, "fps=0 时拒绝：" + nofps_msg);
    check(!nofps_ok && nofps_msg.find("帧率") != std::string::npos,
          "而且说的是帧率，不是别的：" + nofps_msg);

    AudioTrackOptions norate = with_source(1, 0, 25);
    norate.mixer.rate = 0;
    bool const norate_ok = t.start(norate, err);
    std::string const norate_msg = err;
    check(!norate_ok, "采样率为 0 时拒绝：" + norate_msg);
    check(!norate_ok && norate_msg.find("48000") == std::string::npos &&
              norate_msg.find("Hz") != std::string::npos,
          "而且说的是格式不合法：" + norate_msg);

    if (have_sys) {
      // 麦克风那一路指到一个不存在的设备：**整条音频照样要起来**（环回是好的），
      // 而状态报 error 把"我那一路上真坏了"露出来。这两件事在两层上分别成立，
      // 合起来才是"麦克风被拔掉不该让摄像机起不来"。
      AudioTrackOptions broken = with_source(1, 1, 25);
      broken.mixer.mic.device_id =
          "{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}";
      if (t.start(broken, err)) {
        check(true, "两路里坏了一路（麦克风设备不存在）时 start() 仍然成功");
        check(t.mixer().state_text() == "error",
              "而状态报 error —— 好的一路还在跑，坏的那一路没被藏起来（实测 " +
                  t.mixer().state_text() + "）");
        AuLog broken_log;
        t.tick(broken_log.sink());  // 不该崩、也不该不产出
        check(t.frames_out() == 1920, "坏了一路也照样按拍产出：一拍 1920");
      } else {
        check(false, "两路里坏了一路时 start() 不该失败：" + err);
      }
      t.stop();
    }
  }

  // ==================== [5] 重载：同一个对象再 start 一次，时间轴从头开始 ====================
  std::printf("\n[5] 重载（分段循环会拿同一个对象再 start 一次）\n");
  if (!have_sys) {
    skip("没有播放端点");
  } else {
    AudioTrack t;
    std::string err;
    AudioTrackOptions const o = with_source(1, 0, 25);
    if (!t.start(o, err)) {
      check(false, "第一次 start() 失败：" + err);
    } else {
      AuLog log;
      pump(t, log, 25);
      check(t.frames_out() == 48000, "第一趟 1.000 秒");
      check(t.start(o, err), "同一个对象可以再 start() 一次" + (err.empty() ? "" : "：" + err));
      check(t.frames_out() == 0, "重载之后时间轴从零开始（不是接着上一趟涨）");
      AuLog log2;
      pump(t, log2, 25);
      check(t.frames_out() == 48000,
            d2o::fmt("第二趟还是恰好 1.000 秒（实测 %llu）",
                     static_cast<unsigned long long>(t.frames_out())));
      check(!log2.pts.empty() && log2.pts.front() == -t.encoder().initial_padding(),
            "第二趟的首包 pts 照样从 −启动填充开始");

      // ==================== [6] stop() 之后不再产出 ====================
      std::printf("\n[6] stop() 之后 tick() 什么都不做\n");
      t.stop();
      check(!t.active(), "active() 是 false");
      std::uint64_t const frozen = t.frames_out();
      std::size_t const n = log2.pts.size();
      t.tick(log2.sink());
      check(t.frames_out() == frozen && log2.pts.size() == n,
            "tick() 一个样本都不产出、一个包都不发");
      std::string ferr;
      check(t.flush(log2.sink(), ferr), "flush() 在停掉之后返回成功（幂等，析构里还会调一次）");
      check(log2.pts.size() == n, "冲刷也不产出（编码器已经关了）");
    }
  }

  // ==================== [7] 两路同开：时间轴照样只由拍数决定 ====================
  std::printf("\n[7] 系统声音 + 麦克风同时开\n");
  if (!have_sys || !have_mic) {
    skip("两个端点没有同时具备");
  } else {
    AudioTrack t;
    std::string err;
    AudioTrackOptions const o = with_source(1, 1, 25);
    if (!t.start(o, err)) {
      check(false, "start() 失败：" + err);
    } else {
      AuLog log;
      pump(t, log, 25);
      check(t.frames_out() == 48000,
            d2o::fmt("两路同开时音轨依然是 1.000 秒（实测 %llu）—— 增益与 "
                     "headroom 都不参与时间轴",
                     static_cast<unsigned long long>(t.frames_out())));
      check(!log.pts.empty(), "而且 AU 照样产出");
      auto const srcs = t.mixer().sources();
      check(srcs.size() == 2 && srcs[0].enabled && srcs[1].enabled,
            "sources() 两项都在、都标着启用");
      t.stop();
    }
  }

  std::printf("\n通过 %d 项，失败 %d 项，跳过 %d 项。\n", g_pass, g_fail, g_skip);
  return g_fail == 0 ? 0 : 1;
}
