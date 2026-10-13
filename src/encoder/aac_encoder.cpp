#include "encoder/aac_encoder.h"

#include "common/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <cstdio>

namespace d2o {

namespace {

/// FFmpeg 的错误码要转成人话，否则只会看到一个负数。
/// （和 x264_encoder.cpp 里那个是同一份，各自留一份是刻意的：两个编码器
/// 互不依赖，一个文件里改坏了不会连累另一个。）
std::string av_error(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  if (av_strerror(err, buf, sizeof buf) < 0) {
    return fmt("错误码 %d", err);
  }
  return std::string(buf) + fmt("（%d）", err);
}

}  // namespace

AacEncoder::~AacEncoder() { close(); }

void AacEncoder::close() {
  if (fifo_) {
    av_audio_fifo_free(fifo_);
    fifo_ = nullptr;
  }
  if (pkt_) {
    av_packet_free(&pkt_);
    pkt_ = nullptr;
  }
  if (frame_) {
    av_frame_free(&frame_);
    frame_ = nullptr;
  }
  if (ctx_) {
    avcodec_free_context(&ctx_);
    ctx_ = nullptr;
  }
  extradata_.clear();
  planar_.clear();
  // cfg_ 留着不清：close() 之后 sample_rate()/channels() 还答得出来。
  frame_size_ = 0;
  initial_padding_ = 0;
  next_pts_ = 0;
  packets_out_ = bytes_out_ = frames_in_ = 0;
}

bool AacEncoder::open(AacConfig const& cfg, std::string& err) {
  close();
  cfg_ = cfg;

  if (cfg_.sample_rate <= 0) {
    err = fmt("采样率不合法：%d", cfg_.sample_rate);
    return false;
  }
  if (cfg_.channels < 1 || cfg_.channels > 2) {
    // 混音器只出立体声，而这一层不打算支持它用不上的东西 —— 与其写一条
    // 从来没人走过的多声道路径，不如当场说清楚（同 audio_mixer.cpp 那条）。
    err = fmt("AAC 这一层只支持 1 或 2 声道（拿到 %d）", cfg_.channels);
    return false;
  }
  if (cfg_.bitrate_kbps <= 0) {
    err = fmt("码率不合法：%d kbps", cfg_.bitrate_kbps);
    return false;
  }

  AVCodec const* codec = avcodec_find_encoder_by_name("aac");
  if (!codec) {
    err = "找不到 aac 编码器 —— 这个 FFmpeg 构建里没有原生 AAC 编码器";
    return false;
  }

  ctx_ = avcodec_alloc_context3(codec);
  if (!ctx_) {
    err = "avcodec_alloc_context3 失败";
    return false;
  }

  ctx_->sample_rate = cfg_.sample_rate;
  // FLTP：AAC 原生就是按平面浮点收的。用 S16P 会多一次量化，而我们的上游
  // （AudioMixer）本来就是 float —— 在 float 域里一路走到编码器，中间不落地。
  ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
  // FFmpeg 7 起声道信息统一走 ch_layout，`channels`/`channel_layout` 已经没了。
  av_channel_layout_default(&ctx_->ch_layout, cfg_.channels);
  ctx_->bit_rate = static_cast<std::int64_t>(cfg_.bitrate_kbps) * 1000;
  // 时基取 1/采样率，于是 pts 的单位就是**样本数** —— 后面对齐音频时间轴
  // 时不用再乘除任何东西。这与视频那边（时基 1/fps、pts 是帧号）是同一套思路。
  ctx_->time_base = AVRational{1, cfg_.sample_rate};

  // 见文件头第一条：SDP 的 config= 全靠它，无条件打开，不暴露成选项。
  ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  int const ret = avcodec_open2(ctx_, codec, nullptr);
  if (ret < 0) {
    err = "avcodec_open2 失败：" + av_error(ret);
    close();
    return false;
  }

  if (ctx_->frame_size <= 0) {
    err = fmt("编码器没有给出帧长（frame_size=%d）—— 没法按帧喂它",
              ctx_->frame_size);
    close();
    return false;
  }
  frame_size_ = ctx_->frame_size;
  // 由 libavcodec 填，不是我们设的。搬给 codecpar 是 Mp4Writer 的活（陷阱 74）。
  initial_padding_ = ctx_->initial_padding;

  if (ctx_->extradata && ctx_->extradata_size > 0) {
    extradata_.assign(ctx_->extradata, ctx_->extradata + ctx_->extradata_size);
  } else {
    // 没有 extradata 就没有 SDP 的 config=，收端解出来是噪音而**一切看着正常**
    // （能连上、ffprobe 报得出正确的采样率和声道）。宁可起不来。
    err = "AAC 编码器没有产出 extradata（AudioSpecificConfig）—— "
          "SDP 会缺 config= 参数，收端解出来是噪音";
    close();
    return false;
  }

  frame_ = av_frame_alloc();
  if (!frame_) {
    err = "av_frame_alloc 失败";
    close();
    return false;
  }
  frame_->format = ctx_->sample_fmt;
  frame_->sample_rate = ctx_->sample_rate;
  av_channel_layout_copy(&frame_->ch_layout, &ctx_->ch_layout);
  frame_->nb_samples = frame_size_;
  int const bret = av_frame_get_buffer(frame_, 0);
  if (bret < 0) {
    err = "av_frame_get_buffer 失败：" + av_error(bret);
    close();
    return false;
  }

  // FIFO 的初始容量给一帧就够（它会自己涨），给大了只是白占内存。
  fifo_ = av_audio_fifo_alloc(ctx_->sample_fmt, cfg_.channels, frame_size_);
  if (!fifo_) {
    err = "av_audio_fifo_alloc 失败";
    close();
    return false;
  }

  pkt_ = av_packet_alloc();
  if (!pkt_) {
    err = "av_packet_alloc 失败";
    close();
    return false;
  }

  D2O_INFO("AAC 编码器就绪：%d Hz %d 声道 %d kbps，帧长 %d 样本（%.1f ms），"
           "启动填充 %d 样本（%.1f ms），extradata %zu 字节",
           cfg_.sample_rate, cfg_.channels, cfg_.bitrate_kbps, frame_size_,
           1000.0 * frame_size_ / cfg_.sample_rate, initial_padding_,
           1000.0 * initial_padding_ / cfg_.sample_rate, extradata_.size());
  return true;
}

bool AacEncoder::receive_all(PacketSink const& sink, std::string& err) {
  for (;;) {
    int const ret = avcodec_receive_packet(ctx_, pkt_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
    if (ret < 0) {
      err = "avcodec_receive_packet 失败：" + av_error(ret);
      return false;
    }
    ++packets_out_;
    bytes_out_ += static_cast<std::uint64_t>(pkt_->size);
    if (sink) sink(*pkt_);
    av_packet_unref(pkt_);
  }
}

bool AacEncoder::drain_fifo(PacketSink const& sink, std::string& err) {
  while (av_audio_fifo_size(fifo_) >= frame_size_) {
    // 编码器可能在上一轮 send 之后还持有 frame_ 的引用（它自己 av_frame_ref
    // 过一次），所以下一轮往里写之前必须先确认这块缓冲是我们独占的。
    int const mret = av_frame_make_writable(frame_);
    if (mret < 0) {
      err = "av_frame_make_writable 失败：" + av_error(mret);
      return false;
    }
    int const got = av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame_->data),
                                       frame_size_);
    if (got < frame_size_) {
      // 上面刚判过 size() >= frame_size_，读不满足以说明 FIFO 的状态不对了。
      err = fmt("音频 FIFO 读取不足：要 %d 帧，拿到 %d",
                frame_size_, got);
      return false;
    }
    frame_->nb_samples = got;
    frame_->pts = next_pts_;
    next_pts_ += got;

    int const sret = avcodec_send_frame(ctx_, frame_);
    if (sret < 0) {
      err = "avcodec_send_frame 失败：" + av_error(sret);
      return false;
    }
    if (!receive_all(sink, err)) return false;
  }
  return true;
}

bool AacEncoder::encode(float const* interleaved, int frames,
                        PacketSink const& sink, std::string& err) {
  if (!ctx_) {
    err = "AAC 编码器未打开";
    return false;
  }
  if (frames <= 0) return true;
  if (!interleaved) {
    err = "AAC 编码器拿到空指针";
    return false;
  }

  int const ch = cfg_.channels;
  // 交错 → 平面。AAC 要的是 FLTP（每个声道一段连续的 float），而混音器给的是
  // 交错 float。这一层转换放在这里，是为了让上游只认一种格式。
  planar_.resize(static_cast<std::size_t>(ch) * static_cast<std::size_t>(frames));
  for (int c = 0; c < ch; ++c) {
    float* dst = planar_.data() + static_cast<std::size_t>(c) * frames;
    for (int i = 0; i < frames; ++i) {
      float const v = interleaved[static_cast<std::size_t>(i) * ch + c];
      // 兜底钳位。`AudioMixer::fill()` 已经钳过一次，但这是进编码器之前的
      // **最后一站**：float 过冲一点点，编码器那边不会报错，只会削顶 ——
      // 也就是"多了一点听不出来的失真"，没有任何人会去查。一次比较换这个，
      // 值。
      dst[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    }
  }

  void* ptrs[8] = {nullptr, nullptr, nullptr, nullptr,
                   nullptr, nullptr, nullptr, nullptr};
  for (int c = 0; c < ch; ++c) {
    ptrs[c] = planar_.data() + static_cast<std::size_t>(c) * frames;
  }
  int const written = av_audio_fifo_write(fifo_, ptrs, frames);
  if (written < frames) {
    err = fmt("音频 FIFO 写入不足：要 %d 帧，写进 %d（通常是内存不足）",
              frames, written);
    return false;
  }
  frames_in_ += static_cast<std::uint64_t>(frames);
  return drain_fifo(sink, err);
}

bool AacEncoder::flush(PacketSink const& sink, std::string& err) {
  if (!ctx_) return true;

  // 把 FIFO 里不足一帧的尾巴**补零凑满一帧**编出去。
  //
  // 不补的话最后那不到 21 ms 会被静默丢掉 —— 而它恰好在音轨的结尾，症状是
  // "音轨比视频短一点点"，正是最容易被归因到别处的那一类。代价是末尾多出
  // 不到 21 ms 的静音，对监控录像无所谓。
  int const left = av_audio_fifo_size(fifo_);
  if (left > 0) {
    int const mret = av_frame_make_writable(frame_);
    if (mret < 0) {
      err = "av_frame_make_writable 失败：" + av_error(mret);
      return false;
    }
    // 先整块清零再读 —— `av_audio_fifo_read` 只覆盖 left 个样本，
    // 剩下那部分数据是上一次编码留下的，不清就是一段**上一次的音频**被
    // 当成尾部补进去（听起来是结尾处一声突兀的重复）。
    for (int c = 0; c < cfg_.channels; ++c) {
      std::fill(frame_->data[c], frame_->data[c] + frame_->linesize[0], 0);
    }
    int const got = av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame_->data),
                                       frame_size_);
    if (got != left) {
      err = fmt("冲刷音频 FIFO 时读到 %d 帧，预期 %d", got, left);
      return false;
    }
    D2O_DEBUG("音频结尾补零 %d 个样本（%.1f ms）凑满一帧",
              frame_size_ - left, 1000.0 * (frame_size_ - left) / cfg_.sample_rate);

    frame_->nb_samples = frame_size_;
    frame_->pts = next_pts_;
    next_pts_ += frame_size_;
    int const sret = avcodec_send_frame(ctx_, frame_);
    if (sret < 0) {
      err = "avcodec_send_frame 失败：" + av_error(sret);
      return false;
    }
    if (!receive_all(sink, err)) return false;
  }

  int const ret = avcodec_send_frame(ctx_, nullptr);  // nullptr = 冲刷
  if (ret < 0 && ret != AVERROR_EOF) {
    err = "冲刷 AAC 编码器失败：" + av_error(ret);
    return false;
  }
  return receive_all(sink, err);
}

std::vector<std::uint8_t> AacEncoder::asc() const {
  // 前 2 字节才是 AudioSpecificConfig。后面那 3 个字节是 sync extension
  // （"没有 SBR"的声明），SDP 里放不放都行，而真实摄像机不放 —— 见文件头。
  if (extradata_.size() < 2) return {};
  return std::vector<std::uint8_t>(extradata_.begin(), extradata_.begin() + 2);
}

std::string AacEncoder::asc_hex() const {
  auto const a = asc();
  std::string s;
  s.reserve(a.size() * 2);
  char buf[3];
  for (std::uint8_t b : a) {
    std::snprintf(buf, sizeof buf, "%02X", b);
    s += buf;
  }
  return s;
}

std::string AacEncoder::asc_hex_for(int sample_rate, int channels) {
  // ISO/IEC 14496-3 的 samplingFrequencyIndex 表。**只列这条链路可能出现的
  // 那几个**也行，但整张表一共 13 项、抄全了反而没有"漏了哪一项"的问题。
  // 0x0F 是"显式给 24 bit 采样率"的转义，这里不支持。
  static constexpr int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                   24000, 22050, 16000, 12000, 11025, 8000, 7350};
  int freq_idx = -1;
  for (int i = 0; i < static_cast<int>(sizeof kRates / sizeof kRates[0]); ++i) {
    if (kRates[i] == sample_rate) {
      freq_idx = i;
      break;
    }
  }
  if (freq_idx < 0 || channels < 1 || channels > 7) {
    // 宁可返回空：空的 config= 会让 `RtspServer::start()` 直接判失败（并且
    // 说清楚原因），比报一个错的 ASC 好得多 —— 后者收端照样连得上、照样报得出
    // "48000 Hz 立体声"，只是解出来是噪音。
    return std::string();
  }
  // 5 bit object type（AAC-LC = 2）、4 bit 采样率索引、4 bit 声道配置。
  unsigned const bits = (2u << 11) | (static_cast<unsigned>(freq_idx) << 7) |
                        (static_cast<unsigned>(channels) << 3);
  char buf[5];
  std::snprintf(buf, sizeof buf, "%02X%02X", (bits >> 8) & 0xFFu, bits & 0xFFu);
  return std::string(buf);
}

int AacEncoder::sample_rate() const {
  return ctx_ ? ctx_->sample_rate : cfg_.sample_rate;
}

int AacEncoder::channels() const {
  return ctx_ ? ctx_->ch_layout.nb_channels : cfg_.channels;
}

}  // namespace d2o
