/**
 * @file tests/aac_check.cpp
 * @brief AAC 编码器：编出来的到底是不是能听的 AAC，以及那 2 字节 extradata 对不对。
 *
 * 这一层坏掉的样子**非常不像坏掉**：SDP 里有 `m=audio`、`ffprobe` 报得出
 * 48000 Hz 立体声 AAC、时长也对、NVR 也接受，**只有听的时候**才发现是噪音或者
 * 静音。三条独立的路径都能造成这个结果，而它们各自的"正常"表现完全一样：
 *
 *   1. **没有 extradata** ⇒ SDP 缺 `config=` ⇒ 收端按默认假设解，出来是噪音；
 *   2. **给裸 AU 套了 ADTS 头**（陷阱 76）⇒ 每个包多一层假头，出来是噪音；
 *   3. **initial_padding 没搬给 codecpar**（陷阱 74）⇒ 音画恒定差 21 ms，
 *      这个听不出来，但和"音轨比视频短 21 ms"这类断言会一直打架。
 *
 * 所以这个测试**把包解回来按波形量**（过零率数频率），而不是只看
 * "编出了东西、大小不为零"。后者在上面三种情况下全部通过。
 *
 * 编译（在项目根目录下）：
 *   g++ -std=c++17 -Isrc -Ithird_party/ffmpeg/include tests/aac_check.cpp \
 *       src/encoder/aac_encoder.cpp src/common/log.cpp \
 *       -o build/aac_check.exe -Lthird_party/ffmpeg/lib -lavcodec -lavutil
 */

#include "common/log.h"
#include "encoder/aac_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace d2o;

namespace {

int g_pass = 0;
int g_fail = 0;

// 这个测试**没有跳过分支**：原生 aac 编码器/解码器在任何一份 FFmpeg 构建里都有，
// 起不来就是真失败，不是"这台机器上没有"。（wasapi_check 那种才需要 skip ——
// 它依赖这台机器上有没有录音端点。）
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

constexpr int kRate = 48000;
constexpr int kCh = 2;
constexpr int kFreq = 1000;
constexpr float kAmp = 0.5f;
constexpr int kChunk = 1920;  // 40 ms @ 48k —— 和 25 fps 的一拍一致

std::string av_error(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  if (av_strerror(err, buf, sizeof buf) < 0) return std::to_string(err);
  return std::string(buf) + "(" + std::to_string(err) + ")";
}

/// 交错立体声正弦。相位是**从 0 连续算**的（不是每块从 0 开始），
/// 否则每块边界上都有一次跳变，过零率会多出 25 个假周期。
void fill_sine(std::vector<float>& out, int start_frame, int frames) {
  out.resize(static_cast<std::size_t>(frames) * kCh);
  for (int i = 0; i < frames; ++i) {
    double const t = static_cast<double>(start_frame + i) / kRate;
    float const v = static_cast<float>(std::sin(2.0 * 3.14159265358979 * kFreq * t) * kAmp);
    out[static_cast<std::size_t>(i) * kCh + 0] = v;
    out[static_cast<std::size_t>(i) * kCh + 1] = v;
  }
}

/// 收下来的包（sink 里的包是借用的，回调返回即失效，必须拷走）。
struct Captured {
  std::vector<std::vector<std::uint8_t>> data;
  std::vector<std::int64_t> pts;
  std::int64_t first_pts = 0;
  bool have_first = false;
  std::size_t bytes() const {
    std::size_t n = 0;
    for (auto const& d : data) n += d.size();
    return n;
  }
};

PacketSink collector(Captured& cap) {
  return [&cap](AVPacket& pkt) {
    cap.data.emplace_back(pkt.data, pkt.data + pkt.size);
    cap.pts.push_back(pkt.pts);
    if (!cap.have_first) {
      cap.first_pts = pkt.pts;
      cap.have_first = true;
    }
  };
}

/// 把全部包解回交错 float（单声道平均）。返回 false 表示解码器起不来。
bool decode_all(AacEncoder const& enc, Captured const& cap, std::vector<float>& out,
                std::string& why) {
  AVCodec const* dec = avcodec_find_decoder(AV_CODEC_ID_AAC);
  if (!dec) {
    why = "找不到 aac 解码器";
    return false;
  }
  AVCodecContext* d = avcodec_alloc_context3(dec);
  if (!d) {
    why = "avcodec_alloc_context3(解码器) 失败";
    return false;
  }
  // **extradata 必须给它**：AAC 的裸 AU 里没有任何采样率/声道信息，
  // 不给 extradata 它只能靠猜 —— 而"猜错了也照样解出东西"正是这一层
  // 最难查的地方。
  auto const& ed = enc.extradata();
  d->extradata = static_cast<std::uint8_t*>(
      av_mallocz(ed.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  std::memcpy(d->extradata, ed.data(), ed.size());
  d->extradata_size = static_cast<int>(ed.size());
  int const oret = avcodec_open2(d, dec, nullptr);
  if (oret < 0) {
    why = "解码器 avcodec_open2 失败：" + av_error(oret);
    avcodec_free_context(&d);
    return false;
  }

  AVFrame* f = av_frame_alloc();
  out.clear();
  for (auto const& p : cap.data) {
    AVPacket* pkt = av_packet_alloc();
    pkt->data = const_cast<std::uint8_t*>(p.data());
    pkt->size = static_cast<int>(p.size());
    if (avcodec_send_packet(d, pkt) < 0) {
      why = "avcodec_send_packet 失败";
      av_packet_free(&pkt);
      av_frame_free(&f);
      avcodec_free_context(&d);
      return false;
    }
    av_packet_free(&pkt);
    for (;;) {
      int const r = avcodec_receive_frame(d, f);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
      if (r < 0) {
        why = "avcodec_receive_frame 失败：" + av_error(r);
        av_frame_free(&f);
        avcodec_free_context(&d);
        return false;
      }
      if (f->format != AV_SAMPLE_FMT_FLTP) {
        why = "解码器给的不是 FLTP（是 " + std::to_string(f->format) + "）";
        av_frame_free(&f);
        avcodec_free_context(&d);
        return false;
      }
      for (int i = 0; i < f->nb_samples; ++i) {
        float s = 0.0f;
        for (int c = 0; c < f->ch_layout.nb_channels; ++c) {
          s += reinterpret_cast<float const*>(f->data[c])[i];
        }
        out.push_back(s / static_cast<float>(f->ch_layout.nb_channels));
      }
      av_frame_unref(f);
    }
  }
  av_frame_free(&f);
  avcodec_free_context(&d);
  return true;
}

/// 带施密特触发的上升沿计数 —— 直接数符号变化在解码出来的波形上会被
/// 零点附近的噪声灌爆（一个周期能数出七八次）。
int count_rising(std::vector<float> const& x, int from, int to, float thr) {
  int n = 0;
  bool high = false;
  for (int i = from; i < to; ++i) {
    float const v = x[static_cast<std::size_t>(i)];
    if (!high && v > thr) {
      high = true;
      ++n;
    } else if (high && v < -thr) {
      high = false;
    }
  }
  return n;
}

}  // namespace

int main() {
  Log::set_level(LogLevel::Info);

  AacEncoder enc;
  std::string err;
  AacConfig cfg;
  cfg.sample_rate = kRate;
  cfg.channels = kCh;
  cfg.bitrate_kbps = 64;

  if (!enc.open(cfg, err)) {
    std::printf("[FAIL] 打不开 AAC 编码器：%s\n", err.c_str());
    std::printf("\n通过 0 项，失败 1 项。\n");
    return 1;
  }

  // ------------------------------------------- [1] extradata 的形状与内容
  std::printf("\n[1] extradata（AudioSpecificConfig）—— SDP 的 config= 就是它\n");
  auto const& ed = enc.extradata();
  auto const asc = enc.asc();
  // **不是 2 字节，是 5 字节**（实测 `11 90 56 E5 00`）：前 2 个是
  // AudioSpecificConfig，后 3 个是"没有 SBR"的 sync extension，FFmpeg
  // 无条件写的。断言写成 `>= 2` 而不是 `== 2`，正是为了让下一个人看到
  // 这条注释而不是去"修"一个本来就对的字节数。
  note(d2o::fmt("extradata 实测 %zu 字节，asc() 取前 %zu 字节", ed.size(), asc.size()));
  check(ed.size() >= 2, d2o::fmt("至少 2 字节（实测 %zu）", ed.size()));
  check(asc.size() == 2, d2o::fmt("asc() 恰好 2 字节（实测 %zu）", asc.size()));
  if (asc.size() == 2) {
    check(asc[0] == ed[0] && asc[1] == ed[1], "asc() 就是 extradata 的头两个字节");
    // 5 bits 对象类型 | 4 bits 采样率索引 | 4 bits 声道配置 | 3 bits 填充
    int const object_type = (asc[0] >> 3) & 0x1F;
    int const srate_index = ((asc[0] & 0x07) << 1) | (asc[1] >> 7);
    int const chan_config = (asc[1] >> 3) & 0x0F;
    note(d2o::fmt("解析出来：对象类型 %d，采样率索引 %d，声道配置 %d，asc_hex=%s",
                  object_type, srate_index, chan_config, enc.asc_hex().c_str()));
    check(object_type == 2, d2o::fmt("对象类型是 2（AAC-LC，实测 %d）", object_type));
    check(srate_index == 3, d2o::fmt("采样率索引是 3（48000 Hz，实测 %d）", srate_index));
    check(chan_config == 2, d2o::fmt("声道配置是 2（立体声，实测 %d）", chan_config));
  }
  // SDP 里那句 config= 的**形状**：4 个大写十六进制字符。多一个字节
  // 收端有的能容忍有的不能，而"多带了 3 字节"正是最容易顺手写错的地方。
  check(enc.asc_hex().size() == 4, d2o::fmt("asc_hex() 是 4 个十六进制字符（实测 \"%s\"）",
                                            enc.asc_hex().c_str()));

  // ------------------------------- [1b] 预测出来的 ASC 必须等于编码器那一份
  //
  // `asc_hex_for()` 是**不打开编码器**算出来的那一份，而 RTSP 服务器就是靠它
  // 在采集会话起来之前把音频 subsession 挂上去的（服务器比编码器先起，见
  // rtsp_server.h 的 `audio_config_hex`）。所以这里要钉的是**两者相等**：
  //   * 断「预测 == 真值」而不是「预测 == "1190"」—— 后者是抄一个常量，
  //     编码器换了采样率/声道（或者 FFmpeg 换了写法）时它照样绿；
  //   * 也断几个别的采样率，免得那张索引表只对 48000 这一项是对的。
  // 不一致的后果是"客户端连得上、报得出格式、解出来是噪音"，没有任何一处
  // 会报错 —— 所以这条只能在单元测试里钉死。
  std::printf("\n[1b] asc_hex_for()：不打开编码器算的那份 ASC\n");
  check(d2o::AacEncoder::asc_hex_for(kRate, kCh) == enc.asc_hex(),
        d2o::fmt("48000/2 的预测值 == 编码器的真值（预测 \"%s\" / 真值 \"%s\"）",
                 d2o::AacEncoder::asc_hex_for(kRate, kCh).c_str(),
                 enc.asc_hex().c_str()));
  check(d2o::AacEncoder::asc_hex_for(48000, 2) == "1190",
        d2o::fmt("48000/2 就是文档里那个 1190（实测 \"%s\"）",
                 d2o::AacEncoder::asc_hex_for(48000, 2).c_str()));
  // 索引表**逐项**验：4 bits 的采样率索引一共 13 项，抄错一项的症状是
  // "某个采样率下解出噪音"，而这条链路上永远不会跑到那一项 —— 也就是永远不会
  // 被发现。用公式重新算一遍，不抄第二张表。
  bool rates_ok = true;
  int const kTable[] = {96000, 88200, 64000, 48000, 44100, 32000,
                        24000, 22050, 16000, 12000, 11025, 8000, 7350};
  for (int i = 0; i < 13; ++i) {
    unsigned const bits =
        (2u << 11) | (static_cast<unsigned>(i) << 7) | (2u << 3);
    char want[5];
    std::snprintf(want, sizeof want, "%02X%02X", (bits >> 8) & 0xFFu, bits & 0xFFu);
    if (d2o::AacEncoder::asc_hex_for(kTable[i], 2) != want) {
      rates_ok = false;
      note(d2o::fmt("  %d Hz 算出来是 \"%s\"，应当是 \"%s\"", kTable[i],
                    d2o::AacEncoder::asc_hex_for(kTable[i], 2).c_str(), want));
    }
  }
  check(rates_ok, "13 个采样率索引逐项对得上");
  check(d2o::AacEncoder::asc_hex_for(12345, 2).empty(),
        "表里没有的采样率返回空串（宁可不开音频，也不报一个错的 config）");
  check(d2o::AacEncoder::asc_hex_for(48000, 0).empty() &&
        d2o::AacEncoder::asc_hex_for(48000, 8).empty(),
        "声道数越界（0 / 8）也返回空串");

  // ---------------------------------------------------- [2] initial_padding
  std::printf("\n[2] initial_padding —— Mp4Writer 要靠它写 edit list（陷阱 74）\n");
  check(enc.initial_padding() == 1024,
        d2o::fmt("编码器报的启动填充是 1024 样本（实测 %d）", enc.initial_padding()));
  check(enc.frame_size() == 1024, d2o::fmt("帧长是 1024 样本（实测 %d）", enc.frame_size()));
  check(enc.sample_rate() == kRate && enc.channels() == kCh,
        d2o::fmt("采样率/声道是 %d/%d", enc.sample_rate(), enc.channels()));

  // ---------------------------------------------- [3] 编 1 秒 1 kHz 正弦
  std::printf("\n[3] 编 1 秒 1 kHz 正弦（按 40 ms 一拍喂，和真实调用节奏一致）\n");
  Captured cap;
  auto sink = collector(cap);
  constexpr int kTotal = kRate;  // 1 秒
  {
    std::vector<float> chunk;
    for (int done = 0; done < kTotal; done += kChunk) {
      int const n = std::min(kChunk, kTotal - done);
      fill_sine(chunk, done, n);
      if (!enc.encode(chunk.data(), n, sink, err)) {
        check(false, "encode() 失败：" + err);
        break;
      }
    }
  }
  if (!enc.flush(sink, err)) {
    check(false, "flush() 失败：" + err);
  }
  note(d2o::fmt("帧长 1024 ⇒ 48000 样本应是 %d 个包，实测 %d 个，共 %zu 字节（%.1f kbps）",
                kTotal / 1024, static_cast<int>(cap.data.size()), cap.bytes(),
                8.0 * cap.bytes() / 1.0 / 1000.0));
  check(cap.data.size() >= static_cast<std::size_t>(kTotal / 1024),
        "包数不少于 48000/1024 —— 一个都没少编");

  // **裸 AU：不能有 ADTS 同步字。** FFmpeg 的 aac 编码器本来不加（那是 adts
  // 封装器的活），但"多加了一层头"正是收端出噪音的头号成因，成本一行。
  bool any_adts = false;
  for (auto const& p : cap.data) {
    if (p.size() >= 2 && p[0] == 0xFF && (p[1] & 0xF0) == 0xF0) any_adts = true;
  }
  check(!any_adts, "没有包以 ADTS 同步字 0xFFF? 开头（裸 AU，陷阱 76）");

  // 时间戳整体前移 initial_padding —— avcodec.h 上那段原文就是这么写的，
  // 而 Mp4Writer 的 edit list 依赖它。
  check(cap.have_first && cap.first_pts == -enc.initial_padding(),
        d2o::fmt("第一个包的 pts 是 -%d（实测 %lld）", enc.initial_padding(),
                 static_cast<long long>(cap.first_pts)));

  // **证据转储**：把裸 AU 套上 ADTS 存一份，好让**外部**解码器交叉验证。
  // ADTS 只是给外人读的容器，产品里一个字都不用（裸帧进 MPEG4GenericRTPSink，
  // 见文件头第二条）—— 这里存在的理由是"我们自己解出来的波形"不能既当嫌疑
  // 又当法官：一旦怀疑编码器或怀疑解码循环，只有第三方能分开这两者。
  //
  //   ffmpeg -i out/aac_probe.aac -af astats -f null -
  //
  // 头字段从 `asc()` 现推，不写死 —— 写死的话改了采样率/声道它照旧吐一个
  // **语法合法但内容骗人**的文件，而这种东西骗的正是拿它当判据的人。
  {
    auto const asc = enc.asc();
    int const srate_index = asc.size() == 2 ? (((asc[0] & 0x07) << 1) | (asc[1] >> 7)) : 3;
    int const chans = enc.channels();
    FILE* f = std::fopen("out/aac_probe.aac", "wb");
    if (f) {
      for (auto const& p : cap.data) {
        int const len = static_cast<int>(p.size()) + 7;
        std::uint8_t h[7];
        h[0] = 0xFF;
        h[1] = 0xF1;  // 同步字 + MPEG-4 + layer 0 + 无 CRC
        // profile(2) = 对象类型-1 | 采样率索引(4) | private(1) | 声道配置高位(1)
        h[2] = static_cast<std::uint8_t>((1 << 6) | ((srate_index & 0x0F) << 2) |
                                         ((chans >> 2) & 0x01));
        h[3] = static_cast<std::uint8_t>(((chans & 0x03) << 6) | ((len >> 11) & 0x03));
        h[4] = static_cast<std::uint8_t>((len >> 3) & 0xFF);
        h[5] = static_cast<std::uint8_t>(((len & 7) << 5) | 0x1F);
        h[6] = 0xFC;
        std::fwrite(h, 1, 7, f);
        std::fwrite(p.data(), 1, p.size(), f);
      }
      std::fclose(f);
      note("已转储 out/aac_probe.aac（ADTS，仅供外部解码器交叉验证）");
    }
  }

  // --------------------------------------- [4] 解回来量波形（这才是判据）
  std::printf("\n[4] 解回来按波形量 —— 前三种失败模式在这里才现形\n");
  std::vector<float> pcm;
  std::string why;
  if (!decode_all(enc, cap, pcm, why)) {
    check(false, "解码失败：" + why);
  } else {
    note(d2o::fmt("解出 %zu 个样本（%.3f 秒）", pcm.size(),
                  static_cast<double>(pcm.size()) / kRate));
    check(pcm.size() >= static_cast<std::size_t>(kTotal),
          "解出的样本数不少于喂进去的 48000 —— 音轨没被截短");

    // 掐掉首尾：开头是编码器的启动填充、结尾是 flush 补的零，
    // 它们本来就不是那段正弦，量频率时把它们算进去只会得到假失败。
    int const from = std::min<int>(4096, static_cast<int>(pcm.size()));
    int const to = static_cast<int>(pcm.size()) - std::min<int>(4096, static_cast<int>(pcm.size()) / 4);
    if (to <= from + kRate / 10) {
      check(false, "解出来的可用样本太少，量不了频率");
    } else {
      int const cycles = count_rising(pcm, from, to, 0.05f);
      double const secs = static_cast<double>(to - from) / kRate;
      double const freq = cycles / secs;
      check(std::fabs(freq - kFreq) < kFreq * 0.02,
            d2o::fmt("解出来的频率是 %.1f Hz（送进去的是 %d Hz，误差 %.2f%%）", freq,
                     kFreq, 100.0 * std::fabs(freq - kFreq) / kFreq));

      float peak = 0.0f;
      int at = 0;
      double sq = 0.0;
      for (int i = from; i < to; ++i) {
        float const a = std::fabs(pcm[static_cast<std::size_t>(i)]);
        if (a > peak) { peak = a; at = i; }
        sq += static_cast<double>(pcm[static_cast<std::size_t>(i)]) * pcm[static_cast<std::size_t>(i)];
      }
      double const rms = std::sqrt(sq / (to - from));
      double const in_rms = kAmp / 1.41421356;
      // 这一行是本节的**核心测量**，不是装饰：它把"编码器把电平放大了多少"
      // 与"峰值被抬了多少"分开摆出来。两者的差额就是编解码器的过冲。
      note(d2o::fmt("有效值 %.4f（送进去 %.4f，差 %+.2f dB）｜峰值 %.4f（送进去 %.2f，"
                    "差 %+.2f dB，出现在第 %d 个样本）",
                    rms, in_rms, 20.0 * std::log10(rms / in_rms), peak, kAmp,
                    20.0 * std::log10(peak / kAmp), at));

      // ⚠️ **电平要断言有效值，不能断言峰值。** 有损编码器保的是能量，不是峰值：
      // FFmpeg 原生 aac 在 64 kbps 上编一条纯正弦，峰值会**过冲约 +3.9 dB**
      // （实测：送进去 0.50 → 解出来 0.784），而有效值只差 +0.4 dB。
      // 这不是我们的封装写错了 —— 同一条信号交给 `ffmpeg` 命令行编同样的
      // 64 kbps，`astats` 量出来是 -5.12 dB 对源 -9.03 dB，**同样是 +3.9 dB**；
      // 提到 128 kbps 降到 +0.9 dB。也就是说它是这个编码器在低码率上的固有
      // 行为，与谁调用它无关。
      //
      // 所以"峰值应当约等于送进去的幅值"是一条**不成立的断言** —— 它会把一个
      // 完全正确的实现报成失败。判据换成有效值（±1.5 dB 之内），它才是编码器
      // 真正保证的东西。
      check(std::fabs(20.0 * std::log10(rms / in_rms)) < 1.5,
            d2o::fmt("有效值和送进去的差 %.2f dB（判据 1.5 dB 之内）—— 电平没丢也没翻倍",
                     20.0 * std::log10(rms / in_rms)));
      // 峰值只判"在、而且没削顶"：上限 1.0 是因为任何超过它就说明解码输出
      // 已经在整数域里削平了，那才是真的坏了。
      check(peak > kAmp * 0.7f && peak <= 1.0f,
            d2o::fmt("峰值在 %.2f..1.0 之间（实测 %.4f）—— 有声音且未削顶",
                     kAmp * 0.7f, peak));
      // 波峰因数**只判下界**：它这一条存在是为了把"噪音"和"音调"分开
      // （白噪音约 0.25，正弦 0.707），而进了解码器之后的正弦被过冲抬到
      // 0.47 —— 判上界就又会变成那条不成立的断言。
      double const crest = peak > 0.0f ? rms / peak : 0.0;
      check(crest > 0.35, d2o::fmt("有效值/峰值 = %.3f —— 是音调不是噪音（白噪音约 0.25）",
                                   crest));
    }
  }

  // --------------------------------- [5] flush 会把不足一帧的尾巴补零编出去
  std::printf("\n[5] 尾巴不足一帧时 flush 要补零编出去，不能静默丢掉\n");
  {
    // **同样一段输入喂两个编码器，一个冲刷一个不冲刷。** 这是刻意的差分：
    // 断言"1124 个样本应当编出 N 个包"要先知道编码器的延迟补偿是几 ——
    // 而那个数实测是 `送进去的整帧数 + 1`（§3：47 帧 → 48 包；这里 2 帧 → 3 包），
    // 它是实现细节，写死之后 FFmpeg 换个版本就会变成一条假失败。
    // 差分只依赖"多冲刷不该少出包"这一条，而它正是要钉的性质本身。
    //
    // 顺带把"多冲刷也没用"这个反向错误也挡住了：省略 flush 的话那 100 个样本
    // **一个包都不会有**（FIFO 里不足一帧就不编），音轨的结尾悄悄短 2 ms。
    AacEncoder e2;  // 冲刷
    AacEncoder e3;  // 不冲刷
    std::string e;
    if (!e2.open(cfg, e) || !e3.open(cfg, e)) {
      check(false, "第二个/第三个编码器打不开：" + e);
    } else {
      int const tail = 100;                  // 故意不是 1024 的倍数
      int const n = e2.frame_size() + tail;  // 1124
      std::vector<float> chunk;
      fill_sine(chunk, 0, n);
      Captured c2;
      Captured c3;
      auto s2 = collector(c2);
      auto s3 = collector(c3);
      bool const ok = e2.encode(chunk.data(), n, s2, e) && e2.flush(s2, e) &&
                      e3.encode(chunk.data(), n, s3, e);
      if (!ok) {
        check(false, "第二个/第三个编码器失败：" + e);
      } else {
        note(d2o::fmt("%d 个样本（%d 整帧 + %d 零头）：冲刷出 %zu 个包，不冲刷出 %zu 个包",
                      n, e2.frame_size(), tail, c2.data.size(), c3.data.size()));
        check(c2.data.size() > c3.data.size(),
              d2o::fmt("冲刷的包数严格多于不冲刷的（%zu > %zu）—— 零头没被丢掉",
                       c2.data.size(), c3.data.size()));
        check(e2.frames_in() == static_cast<std::uint64_t>(n),
              d2o::fmt("frames_in() 记的是真喂进去的 %d 帧（不含补的零）", n));
        std::vector<float> p2;
        std::string w2;
        if (decode_all(e2, c2, p2, w2)) {
          // 解出来必须是帧长的整数倍，且覆盖得住喂进去的 1124 个样本。
          // （实测 3072 = 3 × 1024：2 帧来自我们送的两帧，多出来的那 1 帧是
          // 编码器自己的延迟补偿 —— 所以这里是 `>=` 而不是等号。）
          check(p2.size() % 1024 == 0 && p2.size() >= static_cast<std::size_t>(n),
                d2o::fmt("解出 %zu 个样本（%d 的整数倍，且不少于喂进去的 %d）",
                         p2.size(), 1024, n));
        } else {
          check(false, "第二个编码器的包解不开：" + w2);
        }
      }
    }
  }

  // ------------------------------------------------- [6] 关掉再开是一干二净
  std::printf("\n[6] close() 之后状态要干净（stop/start 会走到这条路上）\n");
  enc.close();
  check(!enc.is_open(), "close() 之后 is_open() 是 false");
  check(enc.packets_out() == 0 && enc.frames_in() == 0, "计数器归零");
  check(enc.extradata().empty(), "extradata 已清空");
  check(enc.sample_rate() == kRate,
        "关掉之后 sample_rate() 还答得出来（读的是配置不是上下文）");
  std::string e3;
  AacEncoder fresh;
  check(fresh.open(cfg, e3), "关掉之后还能重新打开：" + e3);
  check(fresh.frame_size() == 1024, "重开之后帧长仍然对");
  {
    // 一个样本都没喂过就 flush：FIFO 是空的，应当什么都不出、也不报错。
    Captured empty;
    check(fresh.flush(collector(empty), e3),
          "从没喂过数据时 flush() 不报错（空音频不是故障）");
    check(empty.data.empty(), "空 FIFO 的 flush 产出 0 个包");
  }

  std::printf("\n通过 %d 项，失败 %d 项。\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
