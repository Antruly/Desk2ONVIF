/**
 * @file tests/mp4_dual_check.cpp
 * @brief `Mp4Writer` 的双轨：真的写出两条轨，且两轨的时间轴各自正确。
 *
 * 这个测试盯的是**两类不报错但结果全错**的缺陷，都是"单路改两路"时才会出现的：
 *
 *   1. **源时基只存了一份**（陷阱 75）。`avformat_write_header` 会按封装格式的
 *      要求改写 `stream->time_base`（mov 换成 1/15360 这种），所以"包进来时用的
 *      是哪个时基"只能提前记下来 —— 而且**每路各记一份**。拿视频那份
 *      （1/fps）去 rescale 音频的包（pts 单位是样本数），音频的时间戳会被放大
 *      48000/25 倍：`ffprobe` 报得出 48000 Hz 立体声 AAC、时长那一栏却是一
 *      个多小时的荒谬值，而**没有任何一步报错**。
 *   2. **`write()` 改写 `pkt.stream_index`**。旧版那句
 *      `pkt.stream_index = stream_->index;` 单路时等于"永远是 0"，两路时它会把
 *      音频的包静默当成视频的包写进去 —— 文件能播、`ffprobe` 报两路都在，
 *      只是音轨是空的。
 *
 * 所以判据必须是**读回来按流对账**（流数、编码、几何、每流包数、每流时长），
 * 而不是"文件生成了、大小不为零"—— 上面两种情况下后者全部通过。
 *
 * 另外顺带盯住陷阱 74：`avcodec_parameters_from_context` 不搬 `initial_padding`，
 * 而 mov 要靠它写 edit list 才把音画对齐。**但那个字段本身量不出来** —— 实测
 * 把 `mp4_writer.cpp` 里那句搬运注释掉、重编，两个 mp4 **逐字节相同**，读回来的
 * `initial_padding` 也都是 1024（FFmpeg 的 aac 编码器把首包 pts 整体前移
 * −1024，mov 的 muxer 是从那个负 dts 推的 edit list，不是从这个字段）。所以这里
 * **不拿它当断言**，改判那个真正可观测的东西：**首包 pts 是 −1024 样本**。
 * 详见下面那条 note。
 *
 * 编译（在项目根目录下）：
 *   g++ -std=c++17 -Isrc -Ithird_party/ffmpeg/include tests/mp4_dual_check.cpp \
 *       src/encoder/mp4_writer.cpp src/encoder/aac_encoder.cpp \
 *       src/encoder/x264_encoder.cpp src/common/log.cpp \
 *       -o build/mp4_dual_check.exe \
 *       -Lthird_party/ffmpeg/lib -lavformat -lavcodec -lswscale -lavutil
 *
 * 运行前把 `third_party\ffmpeg\bin` 放进 PATH（那三个 av*.dll 在那里），
 * 或者从 build\ 目录下跑：
 *   PATH=third_party/ffmpeg/bin:$PATH ./build/mp4_dual_check.exe
 */

#include "common/log.h"
#include "encoder/aac_encoder.h"
#include "encoder/mp4_writer.h"
#include "encoder/video_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace d2o;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, std::string const& what) {
  if (ok) {
    ++g_pass;
    std::printf("[OK]   %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("[FAIL] %s\n", what.c_str());
  }
}

void note(std::string const& what) { std::printf("       %s\n", what.c_str()); }

std::string av_error(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  if (av_strerror(err, buf, sizeof buf) < 0) return std::to_string(err);
  return std::string(buf) + "(" + std::to_string(err) + ")";
}

// 参数刻意取小：这个测试验的是**时间轴与分路**，不是画质。320x240@25 一秒的
// 编码耗时可以忽略，而帧数少也让"每流包数"这种断言一眼看得懂。
constexpr int kW = 320;
constexpr int kH = 240;
constexpr int kFps = 25;
constexpr int kSeconds = 1;
constexpr int kFrames = kFps * kSeconds;

constexpr int kRate = 48000;
constexpr int kCh = 2;
constexpr int kFreq = 1000;
constexpr float kAmp = 0.4f;
constexpr int kChunk = kRate / kFps;  // 每拍 1920 样本 —— 和真实采集同一节奏

constexpr char const* kOutPath = "out/mp4_dual_check.mp4";

/// 一帧会动的画面。内容必须**每帧不同** —— 全黑的常量帧在 ultrafast 下小得
/// 近乎为零，虽然不影响时间轴，但"每流包数"这类断言会变得不像在测真东西。
void fill_frame(BgraFrame& f, int i) {
  for (int y = 0; y < f.height; ++y) {
    std::uint8_t* row = f.pixels.data() + static_cast<std::size_t>(y) * f.stride;
    for (int x = 0; x < f.width; ++x) {
      row[x * 4 + 0] = static_cast<std::uint8_t>((x + i * 7) & 0xFF);  // B
      row[x * 4 + 1] = static_cast<std::uint8_t>((y + i * 5) & 0xFF);  // G
      row[x * 4 + 2] = static_cast<std::uint8_t>((x + y + i * 3) & 0xFF);  // R
      row[x * 4 + 3] = 0xFF;                                           // A
    }
  }
  f.content_id = static_cast<std::uint64_t>(i) + 1;
}

/// 交错立体声正弦，相位从 start_frame 连续算（分块各自从 0 开始会在块边界
/// 上多出跳变 —— aac_check 里踩过）。
void fill_sine(std::vector<float>& out, int start_frame, int frames) {
  out.resize(static_cast<std::size_t>(frames) * kCh);
  for (int i = 0; i < frames; ++i) {
    double const t = static_cast<double>(start_frame + i) / kRate;
    float const v =
        static_cast<float>(std::sin(2.0 * 3.14159265358979 * kFreq * t) * kAmp);
    out[static_cast<std::size_t>(i) * kCh + 0] = v;
    out[static_cast<std::size_t>(i) * kCh + 1] = v;
  }
}

/// 读回来的一个流的摘要。
struct StreamInfo {
  AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
  AVCodecID codec = AV_CODEC_ID_NONE;
  int width = 0;
  int height = 0;
  int sample_rate = 0;
  int channels = 0;
  int extradata_size = 0;
  int initial_padding = 0;
  double start_time = 0.0;   ///< 秒
  double duration = 0.0;     ///< 秒
  double first_pts = 0.0;    ///< 首包的 pts，秒（读回来之前是 0）
  bool have_first = false;
  int packets = 0;
  bool saw_negative_dts = false;
};

}  // namespace

int main() {
  Log::set_level(LogLevel::Warn);  // 这个测试的输出归它自己，日志别插进来

  std::string err;

  // --- 1. 两个编码器 ------------------------------------------------------
  X264Encoder venc;
  EncoderConfig vcfg;
  vcfg.width = kW;
  vcfg.height = kH;
  vcfg.fps = kFps;
  vcfg.bitrate_kbps = 600;
  vcfg.gop_seconds = 1;
  if (!venc.open(vcfg, err)) {
    std::printf("[FAIL] 视频编码器打不开：%s\n", err.c_str());
    return 1;
  }
  note(fmt("视频编码器就绪：%dx%d @%d，时基 %d/%d", kW, kH, kFps,
           venc.codec_context()->time_base.num,
           venc.codec_context()->time_base.den));

  AacEncoder aenc;
  AacConfig acfg;
  acfg.sample_rate = kRate;
  acfg.channels = kCh;
  acfg.bitrate_kbps = 64;
  if (!aenc.open(acfg, err)) {
    std::printf("[FAIL] 音频编码器打不开：%s\n", err.c_str());
    return 1;
  }
  note(fmt("音频编码器就绪：%d Hz %d 声道，时基 %d/%d，启动填充 %d 样本",
           kRate, kCh, aenc.codec_context()->time_base.num,
           aenc.codec_context()->time_base.den, aenc.initial_padding()));

  // --- 2. 双轨打开 --------------------------------------------------------
  Mp4Writer writer;
  if (!writer.open(kOutPath, venc.codec_context(), aenc.codec_context(), err)) {
    std::printf("[FAIL] Mp4Writer::open 失败：%s\n", err.c_str());
    return 1;
  }
  check(writer.stream_count() == 2,
        fmt("建了两路（实测 %zu）", writer.stream_count()));

  // --- 3. 越界 stream_index 必须被拒 --------------------------------------
  //
  // 这一条直接盯住"write() 不再改写 stream_index"这个契约。旧实现会把它改写成
  // 0 然后照写不误，于是音频包全进了视频轨 —— 而这里拿到的是明确的失败。
  {
    AVPacket* probe = av_packet_alloc();
    av_new_packet(probe, 16);
    probe->stream_index = 2;  // 只建了两路，合法下标是 0 和 1
    std::string perr;
    bool const ok = writer.write(*probe, perr);
    check(!ok, "第三路的包被拒绝（而不是静默写进视频轨）");
    if (!ok) note("拒绝理由：" + perr);
    av_packet_free(&probe);
  }

  // --- 4. 写一秒 ----------------------------------------------------------
  BgraFrame frame;
  frame.width = kW;
  frame.height = kH;
  frame.stride = kW * 4;
  frame.pixels.assign(static_cast<std::size_t>(frame.stride) * kH, 0);

  std::vector<float> audio;
  std::uint64_t vpkt = 0;
  std::uint64_t apkt = 0;
  int audio_frames_in = 0;
  bool step_ok = true;

  for (int i = 0; i < kFrames && step_ok; ++i) {
    fill_frame(frame, i);
    step_ok = venc.encode(
        frame, i, true,
        [&](AVPacket& p) {
          p.stream_index = 0;
          if (!writer.write(p, err)) step_ok = false;
          ++vpkt;
        },
        err);
    if (!step_ok) break;

    fill_sine(audio, audio_frames_in, kChunk);
    audio_frames_in += kChunk;
    step_ok = aenc.encode(
        audio.data(), kChunk,
        [&](AVPacket& p) {
          p.stream_index = 1;
          if (!writer.write(p, err)) step_ok = false;
          ++apkt;
        },
        err);
  }

  if (!step_ok) {
    std::printf("[FAIL] 写入过程中失败：%s\n", err.c_str());
    return 1;
  }

  // 两个编码器都要冲刷：x264 有帧级并行，AAC 有 FIFO 里不足一帧的尾巴
  // （aac_encoder::flush 会给它补零凑满一帧）。不冲刷的话时长对不齐，
  // 而那正是这个测试要判的东西。
  step_ok = venc.flush(
      [&](AVPacket& p) {
        p.stream_index = 0;
        if (!writer.write(p, err)) step_ok = false;
        ++vpkt;
      },
      err);
  if (step_ok) {
    step_ok = aenc.flush(
        [&](AVPacket& p) {
          p.stream_index = 1;
          if (!writer.write(p, err)) step_ok = false;
          ++apkt;
        },
        err);
  }
  if (!step_ok) {
    std::printf("[FAIL] 冲刷失败：%s\n", err.c_str());
    return 1;
  }

  note(fmt("写入：视频 %llu 包，音频 %llu 包，共 %llu 包 / %llu 字节（送进 %d 帧视频、"
           "%d 帧音频）",
           static_cast<unsigned long long>(vpkt),
           static_cast<unsigned long long>(apkt),
           static_cast<unsigned long long>(writer.packets_written()),
           static_cast<unsigned long long>(writer.bytes_written()), kFrames,
           audio_frames_in));
  check(writer.packets_written() == vpkt + apkt,
        "封装器记的包数 = 视频 + 音频两个下游报的和");
  check(vpkt > 0 && apkt > 0, "两路都真的产出了包");
  // AAC 编码器有延迟（见 aac_check §5）：送进去的整帧数 + 1。
  check(apkt >= static_cast<std::uint64_t>(kSeconds * kRate / aenc.frame_size()),
        fmt("音频包数不少于 %d（一秒钟的整帧数）",
            kSeconds * kRate / aenc.frame_size()));

  if (!writer.close(err)) {
    std::printf("[FAIL] Mp4Writer::close 失败：%s\n", err.c_str());
    return 1;
  }
  check(writer.stream_count() == 0, "关掉之后 stream_count() 回到 0");

  // --- 5. 读回来按流对账 --------------------------------------------------
  AVFormatContext* in = nullptr;
  int ret = avformat_open_input(&in, kOutPath, nullptr, nullptr);
  if (ret < 0) {
    std::printf("[FAIL] 打不开刚写出来的文件：%s\n", av_error(ret).c_str());
    return 1;
  }
  ret = avformat_find_stream_info(in, nullptr);
  if (ret < 0) {
    std::printf("[FAIL] 读不出流信息：%s\n", av_error(ret).c_str());
    avformat_close_input(&in);
    return 1;
  }

  check(in->nb_streams == 2,
        fmt("文件里恰好两路（实测 %u）", in->nb_streams));

  std::vector<StreamInfo> info;
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    AVStream* st = in->streams[i];
    StreamInfo si;
    si.type = st->codecpar->codec_type;
    si.codec = st->codecpar->codec_id;
    si.width = st->codecpar->width;
    si.height = st->codecpar->height;
    si.sample_rate = st->codecpar->sample_rate;
    si.channels = st->codecpar->ch_layout.nb_channels;
    si.extradata_size = st->codecpar->extradata_size;
    si.initial_padding = st->codecpar->initial_padding;
    if (st->start_time != AV_NOPTS_VALUE) {
      si.start_time = st->start_time * av_q2d(st->time_base);
    }
    if (st->duration != AV_NOPTS_VALUE) {
      si.duration = st->duration * av_q2d(st->time_base);
    }
    info.push_back(si);
  }

  // 逐包过一遍，数每流的包数、记首包的 pts，并盯住负的 dts。
  AVPacket* pkt = av_packet_alloc();
  while (av_read_frame(in, pkt) >= 0) {
    int const si = pkt->stream_index;
    if (si >= 0 && static_cast<std::size_t>(si) < info.size()) {
      StreamInfo& s = info[static_cast<std::size_t>(si)];
      if (!s.have_first) {
        s.have_first = true;
        s.first_pts = pkt->pts * av_q2d(in->streams[si]->time_base);
      }
      ++s.packets;
      if (pkt->dts < 0) s.saw_negative_dts = true;
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);

  char const* names[] = {"0", "1"};
  for (std::size_t i = 0; i < info.size(); ++i) {
    StreamInfo const& s = info[i];
    note(fmt("第 %s 路：codec=%s %dx%d %d Hz %d 声道 extradata %d 字节 "
             "启动填充 %d start=%.4f s 首包 pts=%.4f s 时长=%.4f s 包数 %d%s",
             names[i < 2 ? i : 0], avcodec_get_name(s.codec), s.width, s.height,
             s.sample_rate, s.channels, s.extradata_size, s.initial_padding,
             s.start_time, s.first_pts, s.duration, s.packets,
             s.saw_negative_dts ? "（有负 dts）" : ""));
  }
  note("注：读回来的 initial_padding 是**恒为 1024** 的 —— 把 mp4_writer.cpp 里"
       "那句搬运注释掉、重编再比，"
       "两个 mp4 逐字节相同（实测）。也就是说这个数来自 demuxer 对 AAC 的约定/"
       "首包负 dts 推出来的 edit list，**不是**我们搬过去的那一份，所以这里"
       "不拿它当断言（一条怎么都绿的断言等于没验）。真正有意义的是下面那条"
       "首包 pts 为负。");

  if (info.size() == 2) {
    check(info[0].codec == AV_CODEC_ID_H264,
          fmt("第 0 路是 H.264（实测 %s）", avcodec_get_name(info[0].codec)));
    check(info[0].width == kW && info[0].height == kH,
          fmt("第 0 路尺寸 %dx%d（期望 %dx%d）", info[0].width, info[0].height,
              kW, kH));

    check(info[1].codec == AV_CODEC_ID_AAC,
          fmt("第 1 路是 AAC（实测 %s）", avcodec_get_name(info[1].codec)));
    check(info[1].sample_rate == kRate,
          fmt("第 1 路采样率 %d（期望 %d）", info[1].sample_rate, kRate));
    check(info[1].channels == kCh,
          fmt("第 1 路声道数 %d（期望 %d）", info[1].channels, kCh));
    // 陷阱 74 的**可观测**后果在这里，不在 initial_padding 那个字段上（见上面
    // 那条 note）：AAC 编码器有 1024 样本（21 ms）的启动填充，它把自己的
    // 首包 pts 整体前移 −initial_padding（aac_encoder.h 文件头第三条）。
    // 那个负 pts 就是 mov 写 edit list 的依据 —— 它如果在搬运过程中被抹平
    // （比如某天有人"顺手"把负 pts 钳成 0），音画就会恒定差 21 ms。
    check(info[1].have_first &&
              std::fabs(info[1].first_pts + 1024.0 / kRate) < 1e-4,
          fmt("第 1 路首包 pts 是 %.4f s（期望 −1024/48000 = %.4f s，编码器启动填充）",
              info[1].first_pts, -1024.0 / kRate));
    check(info[0].have_first && std::fabs(info[0].first_pts) < 1e-6,
          fmt("第 0 路首包 pts 是 %.6f s（视频没有启动填充，应当是 0）",
              info[0].first_pts));

    // extradata 得是**全部 5 字节**（AudioSpecificConfig 2 字节 + sync
    // extension 3 字节），不是 asc() 那 2 字节 —— MP4 的 esds 要的是完整的
    // DecoderSpecificInfo。搬的是 aac_encoder 的 extradata()，不是 asc()。
    check(info[1].extradata_size == 5,
          fmt("第 1 路 extradata 是 5 字节（实测 %d）", info[1].extradata_size));

    check(info[0].packets > 0 && info[1].packets > 0,
          fmt("两路读回来都有包（视频 %d / 音频 %d）", info[0].packets,
              info[1].packets));

    // **陷阱 75 的判据。** 源时基只存一份的话，音频的 pts（单位是样本数）会被
    // 按 1/fps 解释，时长会变成 48000/25 ≈ 1900 倍 —— 而这个文件照样能打开、
    // 两路照样都在、编码与采样率照样全对。只有把两个时长摆在一起才看得出来。
    double const diff = std::fabs(info[0].duration - info[1].duration);
    check(diff < 0.1,
          fmt("音视频时长差 %.4f s < 0.1 s（视频 %.4f / 音频 %.4f）", diff,
              info[0].duration, info[1].duration));

    // 绝对量也要判：两路都该接近 1 秒。只判"两者相等"的话，两路**同时**错成
    // 1900 倍也会通过 —— 而那是同一种根因的另一种形态。
    check(std::fabs(info[0].duration - kSeconds) < 0.2,
          fmt("视频时长 %.4f s 接近 %d s", info[0].duration, kSeconds));
    check(std::fabs(info[1].duration - kSeconds) < 0.2,
          fmt("音频时长 %.4f s 接近 %d s", info[1].duration, kSeconds));
  }

  avformat_close_input(&in);

  std::printf("\n%d 项通过，%d 项失败\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
