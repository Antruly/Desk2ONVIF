#pragma once

/**
 * @file src/encoder/mjpeg_encoder.h
 * @brief 单帧 JPEG 快照编码（HTTP GET /snapshot.jpg 用）。
 *
 * 和 X264Encoder 是两种东西，别混：那一个是"持续编一条流"，状态跨帧累积
 * （P 帧参考、码率控制窗口、GOP 位置）；这一个每帧独立、编完即弃，只求
 * "把当前这一张桌面压成能塞进 HTTP 响应体的 JPEG"。
 *
 * 为什么单独用 FFmpeg 的 mjpeg 编码器而不是自己塞 libjpeg-turbo：
 *   - 项目已经链接了 FFmpeg，再加一个库要多带一份依赖和构建脚本；
 *   - 编码器还是大概率能直接吃 BGRA（DXGI 桌面复制的原生格式）的，
 *     能吃就整条链路上一次色彩转换都不用做。
 *
 * **但"能吃 BGRA"不能写死**：mjpeg 编码器在 FFmpeg 8 之前支持一批 RGB
 * 格式，libavcodec 63（本项目 third_party/ffmpeg 的 FFmpeg 9.0.2）把它们
 * 全删了，只剩 yuvj420p/yuvj422p/yuvj444p 和对应的 yuv*。写死 BGRA 的话
 * avcodec_open2 直接 EINVAL，快照功能整个起不来（实测过）。
 * 所以 init() 会问 `avcodec_get_supported_config()` 拿支持的像素格式：
 * 支持 BGRA 就直通，不支持就退到 yuvj444p（4:4:4，桌面文字不糊）走一次
 * swscale。两条路 encode() 都只认调用方的 stride。
 *
 * **不能用 `codec->pix_fmts` 去问** —— libavcodec 63 把这个成员删了，
 * 这么写是编译错误（不是运行期才发现）。而且
 * `avcodec_get_supported_config()` 返回"configs 为空"时含义是**全支持**，
 * 不是"一个都不支持"，判反了会在老版本 FFmpeg 上退化成"永远走 swscale"。
 *
 * 上下文跨调用保留：init() 里 avcodec_open2 的开销不小，而快照是按 HTTP
 * 请求触发的（可能一秒好几次，也可能几分钟一次），每次重开纯属浪费。
 */

#include <cstdint>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace d2o {

class MjpegEncoder {
 public:
  MjpegEncoder();
  ~MjpegEncoder();
  MjpegEncoder(MjpegEncoder const&) = delete;
  MjpegEncoder& operator=(MjpegEncoder const&) = delete;

  /// 设定分辨率/质量。分辨率变了要重新调。quality 是 FFmpeg 的 qscale
  /// （2..31，越小越清晰），默认 5。
  ///
  /// 重复调用是安全的：会先把上一次的上下文完整释放掉，不会累积泄漏。
  /// NVR 在 ONVIF 里改分辨率（SetVideoEncoderConfiguration）之后就是走这条路。
  bool init(int width, int height, int quality = 5);

  /// BGRA（每行 stride 字节，可能 > width*4）→ 一帧完整 JPEG（含 SOI/EOI）。
  /// 失败返回空串。**线程约定：只在 web 循环线程上调用，内部不加锁。**
  std::string encode(std::uint8_t const* bgra, int stride);

  int width() const;
  int height() const;
  bool ready() const;

 private:
  /// 释放全部 FFmpeg 对象。init() 开头和析构都走它，保证只有一处清理逻辑。
  void close();

  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  /// 只有编码器不吃 BGRA 时才建（本项目的 FFmpeg 就是这种）。
  /// 非空表示 encode() 要先做一次 BGRA→YUV 的转换。
  SwsContext* sws_ = nullptr;
  /// 只增不减的假时间戳。JPEG 里不存时间，但给每帧一个不同的 pts 更省心：
  /// 有些版本会因为 pts 不变而走重排/去重逻辑，白白 EAGAIN 一次。
  std::int64_t pts_ = 0;
};

}  // namespace d2o
