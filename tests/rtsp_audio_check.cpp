/**
 * @file tests/rtsp_audio_check.cpp
 * @brief RTSP 的音频 subsession：一个会话里是不是真的有 h264 + aac 两条轨。
 *
 * ## 为什么这个测试非要用真的 ffprobe/ffmpeg 去拉一次
 *
 * 因为这一路上**三种典型的错都表现为"SDP 完全正常"**：
 *
 *   1. **裸 AU 外面又套了一层 ADTS 头**（陷阱 76）。收端拿到的每个包都多一个
 *      假头，解出来是噪音 —— 而 `a=rtpmap:97 MPEG4-GENERIC/48000/2`、
 *      `a=fmtp:97 config=1190` 一模一样，`ffprobe` 照样报得出 aac / 48000 /
 *      2 声道。**唯一能分开的是"解出来有没有那个 1 kHz 的正弦"**。
 *   2. **锚点用负 pts 当哨兵**（见 `aac_live_source.h`）：每个 AU 的呈现时间都
 *      等于投递时刻 ⇒ 收端的音轨要么没声音、要么一顿一顿。同样不影响 SDP。
 *   3. **音频挂成了第二个 ServerMediaSession**（陷阱 77）：NVR 拉一个 URL 只会
 *      看到视频，而且**不会报错**（它压根没要音频）。判据是"**同一个会话**里
 *      报告出了两个流" —— 只看 `ffprobe -i` 的输出行数是看不出来的，
 *      得让它把两条流都列出来。
 *
 * 所以本进程只做"产生 + 推"，判据全在 `rtsp_audio_check.ps1` 里（ffprobe 的
 * 双流断言、ffmpeg 拉成 WAV、`rtsp_audio_probe.py` 量 1 kHz）。这里负责把
 * **真的** x264 + 真的 aac 编码器接上真的 RtspServer，而不是喂一段假数据。
 *
 * ## 为什么音频不能用"生成多少样本就推多少"来驱动
 *
 * 这里每拍喂 `48000/fps` 个样本（1920 @25fps），而编码器内部按 1024 攒 FIFO ——
 * 于是有时一拍出 1 包、有时出 2 包。这正是真实链路的形状（陷阱 81：
 * 时间轴归编码线程按拍数出来，不归源端）。
 *
 * 编译（在项目根目录下跑）：
 *   g++ -std=c++17 -Isrc -Ithird_party/ffmpeg/include tests/rtsp_audio_check.cpp \
 *       -o build/rtsp_audio_check.exe \
 *       build/lib/libdesk2onvif_core.a \
 *       -Lthird_party/ffmpeg/lib -lavformat -lavcodec -lswscale -lswresample -lavutil \
 *       -Wl,--start-group third_party/live555/lib/libliveMedia.a \
 *           third_party/live555/lib/libgroupsock.a \
 *           third_party/live555/lib/libBasicUsageEnvironment.a \
 *           third_party/live555/lib/libUsageEnvironment.a -Wl,--end-group \
 *       -Lthird_party/uvcpp/lib -luvcpp -luv \
 *       C:/msys64/mingw64/lib/libssl.dll.a C:/msys64/mingw64/lib/libcrypto.dll.a \
 *       -ld3d11 -ldxgi -liphlpapi -lgdi32 -ladvapi32 -lwtsapi32 -luserenv -lshell32 \
 *       -lole32 -lmmdevapi -lavrt -lws2_32 -lwinmm -loleaut32 -luuid -lcomdlg32
 *
 * 运行前把 `third_party\ffmpeg\bin` 和 `C:\msys64\mingw64\bin` 放进 PATH
 * （av*.dll 在前者、DLL 运行库在后者）：
 *   ./build/rtsp_audio_check.exe 8559 30
 *
 * 参数：`[端口] [秒数] [off]`。第三个参数写 `off` 就是**关掉音频**那一路 ——
 * ps1 会拿它验陷阱 78 的另一半："关掉时不建 subsession"（会话里应当**只有
 * 视频一条流**）。"有音轨但是静音"和"没有音轨"对 NVR 是两回事，而这两半
 * 必须分别验：只验开着的那一半，会漏掉"其实一直挂着一条空音轨"这种实现。
 */

#include "common/log.h"
#include "encoder/aac_encoder.h"
#include "encoder/video_encoder.h"
#include "rtsp/rtsp_server.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace d2o;

namespace {

constexpr int kW = 320;        ///< 故意小：这个测试验的是协议，不是画质
constexpr int kH = 240;
constexpr int kFps = 25;
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr double kToneHz = 1000.0;   ///< 环回那一段的测试音；探针按它判频率
constexpr double kAmplitude = 0.4;   ///< 留足余量：低码率 AAC 会把峰值抬高（陷阱 84）

/// 一帧会动的画面。**每帧都真的变**（一条横扫的白条 + 斜纹理），
/// 这样 `content_changed` 恒为 true —— 不然 x264 会复用上一帧的 YUV，
/// 而"没画面"和"编码器不起作用"在收端看起来是一样的。
void fill_frame(BgraFrame& f, int tick) {
  int const bar_x = (tick * 6) % (kW - 24);
  for (int y = 0; y < kH; ++y) {
    std::uint8_t* row = f.pixels.data() + static_cast<std::size_t>(y) * f.stride;
    for (int x = 0; x < kW; ++x) {
      std::uint8_t const v =
          (x > bar_x && x < bar_x + 24)
              ? 255
              : static_cast<std::uint8_t>((x + y * 2 + tick * 3) % 200);
      row[x * 4 + 0] = v;
      row[x * 4 + 1] = v;
      row[x * 4 + 2] = v;
      row[x * 4 + 3] = 255;
    }
  }
  f.content_id = static_cast<std::uint64_t>(tick) + 1;
}

}  // namespace

int main(int argc, char** argv) {
  unsigned const port =
      argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 8559u;
  int const seconds = argc > 2 ? std::atoi(argv[2]) : 30;
  bool const audio_on = !(argc > 3 && std::string(argv[3]) == "off");
  if (seconds <= 0) {
    std::fprintf(stderr, "秒数必须是正数\n");
    return 2;
  }

  std::string err;

  // ---- 视频编码器：只是为了 SDP 里的 sprop-parameter-sets 是真的 ----
  X264Encoder venc;
  EncoderConfig vcfg;
  vcfg.width = kW;
  vcfg.height = kH;
  vcfg.fps = kFps;
  vcfg.bitrate_kbps = 800;
  vcfg.gop_seconds = 2;
  if (!venc.open(vcfg, err)) {
    std::fprintf(stderr, "x264 打不开：%s\n", err.c_str());
    return 1;
  }

  // ---- 音频编码器：config_hex 必须来自它自己 ----
  // 手抄一个 "1190" 也能让 SDP 看起来对，但那就绕过了"编码器真的按 48000/立体声
  // 初始化过"这件事 —— 而接收端是按 SDP 里那个 config 去解包的。
  AacEncoder aenc;
  AacConfig acfg;
  acfg.sample_rate = kSampleRate;
  acfg.channels = kChannels;
  acfg.bitrate_kbps = 64;
  if (audio_on && !aenc.open(acfg, err)) {
    std::fprintf(stderr, "AAC 编码器打不开：%s\n", err.c_str());
    return 1;
  }

  RtspServer rtsp;
  RtspOptions opt;
  opt.enabled = true;
  opt.port = static_cast<std::uint16_t>(port);
  RtspStreamOptions s0;
  s0.path = "live";
  s0.fps = kFps;
  s0.bitrate_kbps = 800;
  opt.streams.push_back(s0);
  opt.audio.enabled = audio_on;
  opt.audio.sample_rate = kSampleRate;
  opt.audio.channels = kChannels;
  opt.audio.bitrate_kbps = 64;
  opt.audio.config_hex = aenc.asc_hex();
  opt.audio.samples_per_au = static_cast<unsigned>(aenc.frame_size());

  std::vector<std::vector<std::uint8_t>> extras;
  extras.push_back(venc.extradata());

  if (!rtsp.start(opt, extras, err)) {
    std::fprintf(stderr, "RTSP 起不来：%s\n", err.c_str());
    return 1;
  }
  // 这两个方向都要判：**开着的没建**和**关着的建了**（陷阱 78）都是缺陷，
  // 而后者在收端表现为"一条永远静音的音轨"，NVR 会给它画上音频图标。
  if (rtsp.audio_enabled() != audio_on) {
    std::fprintf(stderr, "音频 subsession 的存在与否和请求相反：要求 %s，实际 %s\n",
                 audio_on ? "开" : "关", rtsp.audio_enabled() ? "开" : "关");
    rtsp.stop();
    return 1;
  }

  // 这几行是给 ps1 抓的：端口是从参数来的，地址得打出来才知道该拉哪个。
  std::printf("RTSP: %s\n", rtsp.url(0).c_str());
  if (audio_on) {
    std::printf("audio: payload_type=%u asc=%s frame_size=%d\n",
                rtsp.audio_payload_type(), aenc.asc_hex().c_str(),
                aenc.frame_size());
  } else {
    std::printf("audio: 关闭（不建 subsession，见陷阱 78）\n");
  }
  std::fflush(stdout);

  // ---- 按拍推进：视频一帧、音频 48000/fps 个样本 ----
  std::size_t const vframes = static_cast<std::size_t>(seconds) * kFps;
  int const frames_per_tick = kSampleRate / kFps;   // 1920 @48k/25fps
  std::size_t const total_samples =
      static_cast<std::size_t>(frames_per_tick) * vframes;

  BgraFrame frame;
  frame.width = kW;
  frame.height = kH;
  frame.stride = kW * 4;
  frame.pixels.resize(static_cast<std::size_t>(frame.stride) * kH);

  std::vector<float> mix(static_cast<std::size_t>(frames_per_tick) * kChannels);

  auto vsink = [&](AVPacket& pkt) {
    bool const key = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
    rtsp.push_annex_b(0, pkt.data, static_cast<std::size_t>(pkt.size), key,
                      pkt.pts);
  };
  auto asink = [&](AVPacket& pkt) {
    rtsp.push_audio_au(pkt.data, static_cast<std::size_t>(pkt.size), pkt.pts);
  };

  auto const start = std::chrono::steady_clock::now();
  std::int64_t audio_pos = 0;
  for (std::size_t i = 0; i < vframes; ++i) {
    auto const due =
        start + std::chrono::nanoseconds(static_cast<std::int64_t>(i) *
                                         1'000'000'000LL / kFps);
    std::this_thread::sleep_until(due);

    fill_frame(frame, static_cast<int>(i));
    if (!venc.encode(frame, static_cast<std::int64_t>(i), true, vsink, err)) {
      std::fprintf(stderr, "第 %zu 帧编码失败：%s\n", i, err.c_str());
      break;
    }

    if (audio_on) {
      // 相位按**绝对样本号**算，不按这一拍内的下标 —— 否则每一拍开头都会
      // 从 0 重新开始，接缝处是个相位跳变，听感上是每隔 40 ms 一声咔。
      for (int n = 0; n < frames_per_tick; ++n) {
        float const v = static_cast<float>(
            kAmplitude * std::sin(2.0 * 3.14159265358979 * kToneHz *
                                  static_cast<double>(audio_pos + n) /
                                  static_cast<double>(kSampleRate)));
        mix[static_cast<std::size_t>(n) * 2 + 0] = v;
        mix[static_cast<std::size_t>(n) * 2 + 1] = v;
      }
      if (!aenc.encode(mix.data(), frames_per_tick, asink, err)) {
        std::fprintf(stderr, "第 %zu 拍的音频编码失败：%s\n", i, err.c_str());
        break;
      }
      audio_pos += frames_per_tick;
    }

    if ((i + 1) % (kFps * 5) == 0) {
      std::printf("  [%2zu s] frame=%zu 观众(视/音)=%u/%u AU 入队=%llu 丢弃=%llu "
                  "队列=%zu\n",
                  (i + 1) / kFps, i + 1, rtsp.client_count(0),
                  rtsp.audio_client_count(),
                  static_cast<unsigned long long>(rtsp.audio_au_pushed()),
                  static_cast<unsigned long long>(rtsp.audio_au_dropped()),
                  rtsp.audio_queue_size());
      std::fflush(stdout);
    }
  }

  // 冲刷 FIFO 里那不到 21 ms 的尾巴，让音轨长度对得上 —— 不冲刷也能过，
  // 但少了这段就少了一个"结尾少一截"的现场。
  if (audio_on && !aenc.flush(asink, err)) {
    std::fprintf(stderr, "音频冲刷失败：%s\n", err.c_str());
  }

  // 收尾统计。ps1 抓 `音频统计` 这一行做交叉核对（比如"AU 一个都没入队"
  // 这种情况下，收端是绝对不可能有声音的，必须能被看出来）。
  std::printf("音频统计: 视频帧=%zu 音频样本=%lld AU 入队=%llu AU 丢弃=%llu "
              "最大 AU=%zu 队列=%zu/%zu payload=%u 音频观众=%u\n",
              vframes, static_cast<long long>(audio_pos),
              static_cast<unsigned long long>(rtsp.audio_au_pushed()),
              static_cast<unsigned long long>(rtsp.audio_au_dropped()),
              rtsp.audio_max_au_bytes(), rtsp.audio_queue_size(),
              rtsp.audio_queue_capacity(), rtsp.audio_payload_type(),
              rtsp.audio_client_count());
  std::printf("视频统计: NALU 入队=%llu 丢弃=%llu 最大 NALU=%zu 观众=%u\n",
              static_cast<unsigned long long>(rtsp.nalu_pushed(0)),
              static_cast<unsigned long long>(rtsp.nalu_dropped(0)),
              rtsp.max_nalu_bytes(0), rtsp.client_count(0));
  std::fflush(stdout);

  // 停机顺序：编码线程（就是本线程）已经不再 push 了，才轮到 stop()。
  venc.flush(vsink, err);
  rtsp.stop();
  if (audio_on) aenc.close();
  venc.close();
  (void)total_samples;
  return 0;
}
