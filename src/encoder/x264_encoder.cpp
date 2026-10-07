#include "encoder/video_encoder.h"

#include "common/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <thread>

namespace d2o {

namespace {

/// FFmpeg 的错误码要转成人话，否则只会看到一个负数。
std::string av_error(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  if (av_strerror(err, buf, sizeof buf) < 0) {
    return fmt("错误码 %d", err);
  }
  return std::string(buf) + fmt("（%d）", err);
}

}  // namespace

X264Encoder::~X264Encoder() { close(); }

void X264Encoder::close() {
  if (sws_) {
    sws_freeContext(sws_);
    sws_ = nullptr;
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
  sws_w_ = sws_h_ = 0;
  src_w_ = src_h_ = 0;
  have_converted_ = false;
  packets_out_ = bytes_out_ = keyframes_out_ = 0;
}

bool X264Encoder::open(EncoderConfig const& cfg, std::string& err) {
  close();
  cfg_ = cfg;

  if (cfg_.width <= 0 || cfg_.height <= 0 || cfg_.fps <= 0) {
    err = fmt("编码参数不合法：%dx%d @ %d fps", cfg_.width, cfg_.height, cfg_.fps);
    return false;
  }
  // H.264 的宽高必须是偶数（4:2:0 的色度是半分辨率）。
  // 桌面尺寸基本总是偶数，但外接奇异的显示器时不是，所以这里挡一下。
  if ((cfg_.width % 2) != 0 || (cfg_.height % 2) != 0) {
    err = fmt("H.264 要求宽高为偶数，实际是 %dx%d", cfg_.width, cfg_.height);
    return false;
  }

  AVCodec const* codec = avcodec_find_encoder_by_name("libx264");
  if (!codec) {
    err = "找不到 libx264 编码器 —— 需要带 libx264 的 FFmpeg 构建"
          "（gyan.dev 的 full_build；essentials 包不含）";
    return false;
  }

  ctx_ = avcodec_alloc_context3(codec);
  if (!ctx_) {
    err = "avcodec_alloc_context3 失败";
    return false;
  }

  ctx_->width = cfg_.width;
  ctx_->height = cfg_.height;
  ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx_->time_base = AVRational{1, cfg_.fps};
  ctx_->framerate = AVRational{cfg_.fps, 1};
  ctx_->gop_size = cfg_.gop_size();
  ctx_->max_b_frames = 0;

  // 码率控制用 ABR（平均码率）而不是 CRF：NVR 和网络更希望码率可预测。
  // max_b 给到 1.5 倍，让画面剧烈变化时（滚动、视频播放）不至于糊成一片。
  ctx_->bit_rate = static_cast<std::int64_t>(cfg_.bitrate_kbps) * 1000;
  ctx_->rc_max_rate = static_cast<std::int64_t>(cfg_.bitrate_kbps) * 1500;
  ctx_->rc_buffer_size = static_cast<int>(cfg_.bitrate_kbps) * 1000;  // 约 1 秒

  if (cfg_.global_header) {
    ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  // --- 线程数默认按"延迟优先"选，不是按吞吐优先 -------------------------
  //
  // 既然必须用帧级并行（见下面 slices 那一段，sliced_threads 被禁掉了），
  // 就有一个绕不开的代价：**x264 的帧级并行每多一个线程就多缓冲一帧才吐出
  // 来**，线程数几乎直接等于编码延迟的帧数。而 x264 的"自动"取的是
  // `逻辑核数 * 3 / 2` —— 本机 18 逻辑核 → 27 线程 → 实测启动后要
  // 约 0.9 秒才平衡（10.1s 时吐出 276 帧，理论值 303，差 27 帧，正好对上）。
  // 对一个直播推流来说是白送的延迟。
  //
  // 所以不写就封顶到 6。ultrafast + zerolatency 编 2880x1920 用不到更多，
  // 6 线程的吞吐余量已经很宽，而延迟降到约 0.2 秒。真需要更高吞吐
  // （4K60 之类）就用 --threads 显式调大，代价是延迟按帧数线性增加。
  int const cores = static_cast<int>(std::thread::hardware_concurrency());
  int const wanted = cfg_.threads > 0 ? cfg_.threads : 6;
  ctx_->thread_count = std::max(1, cores > 0 ? std::min(wanted, cores) : wanted);

  // preset / tune / profile 是 libx264 私有的 AVOption，只能这样设。
  av_opt_set(ctx_->priv_data, "preset", cfg_.preset.c_str(), 0);
  av_opt_set(ctx_->priv_data, "tune", cfg_.tune.c_str(), 0);
  av_opt_set(ctx_->priv_data, "profile", cfg_.profile.c_str(), 0);

  // ultrafast 把 CABAC、8x8dct、weightp 全关了，编出来的码流满足不了 Main，
  // x264 会把实际档位降到 Constrained Baseline —— **不管 --profile 写的什么**。
  // （实测：ultrafast + --profile high 出来就是 Constrained Baseline。）
  //
  // 这对 NVR 兼容性反而是最好的（Baseline 是覆盖面最广的一档），但它确实是
  // "选项被无声忽略"，所以这里说一句，免得后面有人对着 --profile high 查半天。
  if (cfg_.preset == "ultrafast" && cfg_.profile != "baseline" &&
      cfg_.profile != "constrained_baseline") {
    D2O_INFO("注意：ultrafast preset 关掉了 CABAC/8x8dct/weightp，实际档位会是 "
             "Constrained Baseline（对 NVR 兼容性最好）。"
             "要 main/high 请用 --preset veryfast 起步");
  }

  // --- 一帧必须只有一个 slice。这是硬约束，不是调优。 -------------------
  //
  // Live555 的 H264VideoStreamDiscreteFramer 判"这个 NALU 是不是结束了一个访问
  // 单元"（决定 RTP 的 M 位）靠的是 `nalUnitEndsAccessUnit(type)`，而那个函数的
  // 实现就是 `isVCL(type)` —— 每一个 VCL NALU 都被当成一帧的结尾。它没有别的
  // 信息可用（离散 NALU 模式下它看不到时间戳）。
  //
  // 于是多 slice 的后果是：一帧里的 18 个 slice 各自带 M 位出去，收端的 RTP
  // 解包器（ffmpeg 的 rtpdec_h264）按 M 位切分访问单元，**把每个 slice 当成一帧**。
  // 解码器拿到一帧只有 1/18 的宏块，其余全部"conceal"，日志里是
  // `concealing 20340 DC, 20340 AC, 20340 MV errors in P frame`（21600 宏块
  // 里藏了 20340 个 —— 正好是 17/18），画面糊成一片但**流是通的、SDP 是对的、
  // ffprobe 能解出正确的分辨率帧率**。不看到解码器日志根本发现不了。
  //
  // 而 `slices=1` 单独写是**不够的**：`tune=zerolatency` 会打开 x264 的
  // sliced-threads（slice 数 = 线程数），ffmpeg 的 libx264 封装在
  // `avctx->thread_type` 含 FF_THREAD_SLICE 时也会把它打开，两者都排在
  // x264-params 之前/之后不好确定，谁赢取决于版本。所以两边都堵：
  //   1. thread_type 只留 FF_THREAD_FRAME —— 从 ffmpeg 这一侧不让它要求切片线程；
  //   2. x264-params 里显式 `sliced-threads=0` —— 从 x264 这一侧再关一次。
  // 帧级多线程仍然有效（x264 会同时编多帧），编码吞吐不受影响。
  ctx_->thread_type = FF_THREAD_FRAME;
  av_opt_set(ctx_->priv_data, "x264-params", "slices=1:sliced-threads=0", 0);

  int ret = avcodec_open2(ctx_, codec, nullptr);
  if (ret < 0) {
    err = "avcodec_open2 失败：" + av_error(ret);
    close();
    return false;
  }

  // extradata 是 Annex-B 的 SPS+PPS（GLOBAL_HEADER 打开时才有）。
  if (ctx_->extradata && ctx_->extradata_size > 0) {
    extradata_.assign(ctx_->extradata,
                      ctx_->extradata + ctx_->extradata_size);
  } else if (cfg_.global_header) {
    err = "编码器没有产出 extradata（SPS/PPS）—— 后续的 SDP 会缺参数集";
    close();
    return false;
  }

  frame_ = av_frame_alloc();
  if (!frame_) {
    err = "av_frame_alloc 失败";
    close();
    return false;
  }
  frame_->format = ctx_->pix_fmt;
  frame_->width = ctx_->width;
  frame_->height = ctx_->height;
  // 16 字节对齐：swscale 和 x264 都偏好对齐的缓冲，未对齐时性能会掉。
  ret = av_frame_get_buffer(frame_, 16);
  if (ret < 0) {
    err = "av_frame_get_buffer 失败：" + av_error(ret);
    close();
    return false;
  }

  pkt_ = av_packet_alloc();
  if (!pkt_) {
    err = "av_packet_alloc 失败";
    close();
    return false;
  }

  D2O_INFO("编码器就绪：libx264 %dx%d @%dfps %dkbps gop=%d profile=%s preset=%s",
           cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrate_kbps, ctx_->gop_size,
           cfg_.profile.c_str(), cfg_.preset.c_str());
  if (!extradata_.empty()) {
    D2O_DEBUG("extradata（SPS/PPS）%zu 字节", extradata_.size());
  }
  return true;
}

bool X264Encoder::receive_all(PacketSink const& sink, std::string& err) {
  for (;;) {
    int const ret = avcodec_receive_packet(ctx_, pkt_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
    if (ret < 0) {
      err = "avcodec_receive_packet 失败：" + av_error(ret);
      return false;
    }
    ++packets_out_;
    bytes_out_ += static_cast<std::uint64_t>(pkt_->size);
    if ((pkt_->flags & AV_PKT_FLAG_KEY) != 0) ++keyframes_out_;
    if (sink) sink(*pkt_);
    av_packet_unref(pkt_);
  }
}

bool X264Encoder::encode(BgraFrame const& src, std::int64_t pts,
                         bool content_changed, PacketSink const& sink,
                         std::string& err) {
  if (!ctx_) {
    err = "编码器未打开";
    return false;
  }
  // 锁住第一帧的**源**尺寸。比的是源尺寸而不是 `ctx_->width/height`：
  // 后者是目标尺寸，两者不等恰恰是缩放编码器的正常状态（子码流就是这样，
  // 主码流 2880x1920 进来、540x360 出去）。拿它们比会把每一个缩放的编码器
  // 在第一帧就打死，而报出来的话是"分辨率变了"，和真正的原因南辕北辙。
  if (src_w_ == 0 && src_h_ == 0) {
    src_w_ = src.width;
    src_h_ = src.height;
  } else if (src.width != src_w_ || src.height != src_h_) {
    err = fmt("分辨率变了：第一帧是 %dx%d，送来的是 %dx%d。"
              "录制中途改分辨率需要重开会话",
              src_w_, src_h_, src.width, src.height);
    return false;
  }
  if (!content_changed && !have_converted_) {
    // 调用方说"没变"，但还没有任何一帧做过转换 —— 那是调用方的记账错了，
    // 照它说的做会得到一块全零的 YUV 缓冲（纯绿/纯黑画面）。
    content_changed = true;
  }

  int ret = av_frame_make_writable(frame_);
  if (ret < 0) {
    err = "av_frame_make_writable 失败：" + av_error(ret);
    return false;
  }

  if (content_changed) {
    // swscale 的上下文按输入尺寸缓存；尺寸没变就不重建。
    if (!sws_ || sws_w_ != src.width || sws_h_ != src.height) {
      if (sws_) sws_freeContext(sws_);
      sws_ = sws_getContext(src.width, src.height, AV_PIX_FMT_BGRA,
                            ctx_->width, ctx_->height, ctx_->pix_fmt,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!sws_) {
        err = "sws_getContext 失败（BGRA → YUV420P）";
        return false;
      }
      sws_w_ = src.width;
      sws_h_ = src.height;
    }

    // 源用紧凑行距（我们就是这么存的，但这里不假设），目标由 FFmpeg 的
    // linesize 决定 —— 它可能大于 width，所以必须逐平面传各自的指针。
    std::uint8_t const* src_data[4] = {src.pixels.data(), nullptr, nullptr, nullptr};
    int src_linesize[4] = {src.stride, 0, 0, 0};
    sws_scale(sws_, src_data, src_linesize, 0, src.height, frame_->data,
              frame_->linesize);
    have_converted_ = true;
  }
  // else：画面没变，frame_ 里还是上一帧的 YUV，直接送进去再编一次。
  // 时间戳仍在推进，所以输出仍是恒定帧率。

  frame_->pts = pts;

  int const sret = avcodec_send_frame(ctx_, frame_);
  if (sret < 0) {
    err = "avcodec_send_frame 失败：" + av_error(sret);
    return false;
  }
  return receive_all(sink, err);
}

bool X264Encoder::flush(PacketSink const& sink, std::string& err) {
  if (!ctx_) return true;
  int const ret = avcodec_send_frame(ctx_, nullptr);  // nullptr = 冲刷
  if (ret < 0 && ret != AVERROR_EOF) {
    err = "冲刷编码器失败：" + av_error(ret);
    return false;
  }
  return receive_all(sink, err);
}

}  // namespace d2o
