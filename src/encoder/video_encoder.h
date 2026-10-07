#pragma once

/**
 * @file src/encoder/video_encoder.h
 * @brief H.264 编码。
 *
 * 参数是按"给 NVR 看的实时流"选的，不是按"压得越小越好"选的：
 *
 *   preset=ultrafast     录制的是桌面，CPU 还要留给别的活；而且桌面内容
 *                        （文字、窗口边缘）在 slow preset 下收益很小
 *   tune=zerolatency     关掉前瞻和 B 帧排队，编完立刻出包。这一项对 RTSP 是
 *                        硬需求 —— 有帧重排就有额外的缓冲延迟
 *   bframes=0            同上，且 B 帧会让 RTP 的时间戳需要重排
 *   gop = fps * 2        关键帧太稀，NVR 断线重连要等很久才出画；太密则码率飙升
 *
 * 输出格式是 Annex-B（x264 的原生输出），SPS/PPS 走 extradata —— Phase 2 的
 * SDP 里那个 sprop-parameter-sets 就是从这里取的。
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "capture/frame.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace d2o {

struct EncoderConfig {
  int width = 0;
  int height = 0;
  int fps = 30;
  int bitrate_kbps = 4000;
  int gop_seconds = 2;          ///< 关键帧间隔（秒），实际值是 fps * 这个数
  std::string preset = "ultrafast";
  std::string tune = "zerolatency";
  std::string profile = "main"; ///< baseline | main | high
  /// 编码线程数。<= 0 表示按"延迟优先"自动选（封顶 6，见 x264_encoder.cpp
  /// 里那段说明）—— **不要**直接用 x264 的 auto，那会取 核数*1.5，
  /// 帧级并行下等于同等的帧数延迟。
  int threads = 0;
  bool global_header = true;    ///< SPS/PPS 放 extradata（MP4 需要）；RTSP 另有安排

  int gop_size() const { return fps > 0 && gop_seconds > 0 ? fps * gop_seconds : 0; }
};

/// 编码产出的一个包。回调返回后包即失效，要用就得自己拷走/写出。
using PacketSink = std::function<void(AVPacket& pkt)>;

class X264Encoder {
 public:
  X264Encoder() = default;
  ~X264Encoder();

  X264Encoder(X264Encoder const&) = delete;
  X264Encoder& operator=(X264Encoder const&) = delete;

  bool open(EncoderConfig const& cfg, std::string& err);
  void close();
  bool is_open() const { return ctx_ != nullptr; }

  /// 把一帧 BGRA 编码成 0..n 个包（bframes=0 时是 1 个）。
  /// pts 用帧序号，时基是 1/fps。
  ///
  /// **输入尺寸不必等于 `EncoderConfig::width/height`** —— 不相等时由 swscale
  /// 缩放，子码流就是这么做的（同一个 2880x1920 的 BGRA，主编码器的目标是
  /// 2880x1920、子编码器的目标是 540x360）。尺寸比例一旦定下就不能再变，
  /// 变了会返回 false 并说明原因（那意味着显示器换了分辨率，得重开会话）。
  ///
  /// @param content_changed  像素是否与上一次不同。桌面静止时传 false，
  ///        编码器会**跳过 BGRA→YUV 的整帧转换**，直接复用上次的 YUV 缓冲 ——
  ///        1080p 一次转换约 4 ms，静止桌面（很常见）能省下这部分 CPU。
  ///        传 false 之前必须至少成功编码过一次 changed=true 的帧。
  bool encode(BgraFrame const& src, std::int64_t pts, bool content_changed,
              PacketSink const& sink, std::string& err);

  /// 冲刷编码器里剩下的帧。停止录制前必须调，否则最后几帧会丢。
  bool flush(PacketSink const& sink, std::string& err);

  /// 底层 FFmpeg 上下文。给封装器读流参数用（Mp4Writer 从它身上取 codecpar）。
  /// 除了"把参数拷走"之外不要拿它做别的 —— 编码器的状态归本类管。
  AVCodecContext const* codec_context() const { return ctx_; }

  /// Annex-B 形式的 SPS/PPS（取自 libx264 的 extradata）。Phase 2 的 SDP 要用。
  std::vector<std::uint8_t> const& extradata() const { return extradata_; }

  /// 诊断用。
  std::uint64_t packets_out() const { return packets_out_; }
  std::uint64_t bytes_out() const { return bytes_out_; }
  std::uint64_t keyframes_out() const { return keyframes_out_; }

 private:
  bool receive_all(PacketSink const& sink, std::string& err);

  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  SwsContext* sws_ = nullptr;
  int sws_w_ = 0;
  int sws_h_ = 0;
  /// 第一帧的源尺寸。**记的是源，不是目标** —— 目标尺寸在 cfg_ 里，两者不等
  /// 是正常的（缩放），而源尺寸中途变了才是故障。拿它和 `ctx_->width/height`
  /// 比会把"配了一个缩放的编码器"误判成"分辨率变了"。
  int src_w_ = 0;
  int src_h_ = 0;
  bool have_converted_ = false;  ///< frame_ 里是否已经有有效的 YUV
  EncoderConfig cfg_{};
  std::vector<std::uint8_t> extradata_;
  std::uint64_t packets_out_ = 0;
  std::uint64_t bytes_out_ = 0;
  std::uint64_t keyframes_out_ = 0;
};

}  // namespace d2o
