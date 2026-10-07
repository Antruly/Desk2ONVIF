#include "encoder/mjpeg_encoder.h"

#include "common/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

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

/// 这个编码器肯不肯吃这种像素格式。
///
/// 必须真的去查，不能凭印象写死 AV_PIX_FMT_BGRA：mjpeg 编码器在 FFmpeg 8
/// 之前支持一批 RGB 格式（BGRA/BGR24/RGB24/ARGB/RGBA），libavcodec 63
/// （本项目的 third_party/ffmpeg，FFmpeg 9.0.2）把它们删掉了，只剩
/// yuvj420p/yuvj422p/yuvj444p 和对应的三个 yuv*。写死 BGRA 的后果是
/// avcodec_open2 直接 EINVAL（编码器自己只打印一句 "Specified pixel format
/// bgra is not supported by the mjpeg encoder."），快照功能整个起不来 ——
/// 实测就是这么失败的。
///
/// 查的方式也踩过一次：老写法 `codec->pix_fmts[i]` 在 libavcodec 63 上
/// **编译不过**（AVCodec 结构体里已经没有 pix_fmts 这个成员了，FF_API 一次
/// 大清理时挪走的），得用新的 avcodec_get_supported_config()。
bool supports_pix_fmt(AVCodecContext const* ctx, AVCodec const* codec,
                      AVPixelFormat fmt) {
  void const* configs = nullptr;
  int count = 0;
  if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                   &configs, &count) < 0) {
    return false;
  }
  // 注意这条：返回 NULL 是"什么格式都支持"，不是"什么都不支持"。
  // 当成"不支持"会让我们白白多建一个 swscale。
  if (!configs) return true;
  auto const* list = static_cast<AVPixelFormat const*>(configs);
  for (int i = 0; i < count; ++i) {
    if (list[i] == fmt) return true;
  }
  return false;
}

/// 不吃 BGRA 时退而求其次要用的 YUV 格式，按优先级挑第一个支持的。
///
/// 排序理由是**桌面内容**而不是通用视频：截图里全是小字号文字和彩色 UI，
/// 4:2:0 会把色度抽到四分之一，彩色文字的边缘当场糊掉（灰底彩色字尤其明显，
/// 亮度是对的、颜色溢出到旁边）。4:4:4 的体积代价对"一次 HTTP 请求一张图"
/// 完全可以接受。
///
/// 优先取 yuvj* 而不是 yuv*：J 系列是 full range（0..255），正是 JPEG/JFIF
/// 期望的范围；普通的 yuv444p 是 limited range（16..235），喂给编码器会得到
/// 一张发灰的图 —— 除非另外设 color_range，而那样还得赌编码器认不认。
/// 另外 4:4:4 没有色度下采样，奇数宽高（外接显示器常见）也不会被拒绝，
/// 而 4:2:0 要求宽高都是偶数。
///
/// 用 J 系列的代价是 swscale 每次初始化都会往 stderr 打一句
/// "deprecated pixel format used, make sure you did set range correctly"。
/// 那是 FFmpeg 在提醒"这个格式已经废弃、range 是自动替你定的"，不是错误：
/// 实测编出来的白是 255 而不是 235，颜色是对的。别看到这句话就去改成
/// yuv444p + color_range，那反而要自己拿 sws_setColorspaceDetails 把目的
/// range 设成 full，少设一处就得到一张发灰的图（而且体积、能否解码都正常，
/// 只有颜色不对）。
AVPixelFormat pick_yuv_format(AVCodecContext const* ctx, AVCodec const* codec) {
  AVPixelFormat const prefer[] = {
      AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUVJ422P,
      AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV420P,
  };
  for (AVPixelFormat f : prefer) {
    if (supports_pix_fmt(ctx, codec, f)) return f;
  }
  return AV_PIX_FMT_NONE;
}

}  // namespace

MjpegEncoder::MjpegEncoder() = default;

MjpegEncoder::~MjpegEncoder() { close(); }

void MjpegEncoder::close() {
  // 释放顺序：转换器 → 包 → 帧 → 上下文。AVPacket/AVFrame 里的缓冲是
  // 引用计数的，先放掉它们再放上下文，免得 free_context 去碰一个还被
  // 我们持有的引用。
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
  // 时间戳回到起点：重开之后就是一个全新的编码会话，
  // 承接上一个会话的计数值没有意义。
  pts_ = 0;
}

bool MjpegEncoder::init(int width, int height, int quality) {
  // 先清干净再建。这里是"重复 init 不泄漏"的唯一保证点 —— 如果哪天有人在
  // 下面加了 return false 的提前退出，也必须已经 close 过。
  close();

  if (width <= 0 || height <= 0) {
    D2O_ERROR("快照编码参数不合法：%dx%d", width, height);
    return false;
  }
  // qscale 的有效区间是 2..31（1 是数学上合法但没意义的档，FFmpeg 拿它当
  // 边界；给到 1 或 0 会被静默夹掉，给到 32 以上同理）。夹在这里而不是
  // 交给 FFmpeg 内部：夹完之后日志里能看出"你要的值没被采纳"，
  // 而内部夹是完全无声的。
  if (quality < 2) {
    D2O_WARN("快照 qscale 请求 %d 太小，夹到 2（jpg 已经很大了）", quality);
    quality = 2;
  } else if (quality > 31) {
    D2O_WARN("快照 qscale 请求 %d 超出上限，夹到 31（再大就是马赛克）", quality);
    quality = 31;
  }

  AVCodec const* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (!codec) {
    D2O_ERROR("找不到 MJPEG 编码器 —— 这个 FFmpeg 构建里没有 mjpeg");
    return false;
  }

  ctx_ = avcodec_alloc_context3(codec);
  if (!ctx_) {
    D2O_ERROR("avcodec_alloc_context3 失败");
    return false;
  }

  // 能吃 BGRA 就直接吃（零转换），吃不下就换 YUV 并建一个转换器。
  // 见 supports_pix_fmt() 上面那段：本项目的 FFmpeg 9.0.2 属于后者。
  // 注意查询要在 ctx_ 建好之后：avcodec_get_supported_config 允许带上
  // 上下文（strict_std_compliance 之类的选项会影响结果），传 NULL 只是
  // 拿默认值。查完再改 ctx_ 的字段，顺序不能反。
  AVPixelFormat dst_fmt = AV_PIX_FMT_NONE;
  bool const bgra_direct = supports_pix_fmt(ctx_, codec, AV_PIX_FMT_BGRA);
  if (bgra_direct) {
    dst_fmt = AV_PIX_FMT_BGRA;
  } else {
    dst_fmt = pick_yuv_format(ctx_, codec);
    if (dst_fmt == AV_PIX_FMT_NONE) {
      D2O_ERROR("这个 MJPEG 编码器既不吃 BGRA 也没有可用的 YUV 格式");
      close();
      return false;
    }
  }

  ctx_->width = width;
  ctx_->height = height;
  ctx_->pix_fmt = dst_fmt;

  // --- time_base 可以不填是错觉。 ---------------------------------------
  //
  // mjpeg 编解码器是搭在 mpegvideo 那一套上的（ff_mpv_encode_init），而
  // 那里有一句硬检查：time_base.num/den 只要有一个 <= 0 就直接
  // `The encoder timebase is not set.` 返回 EINVAL，**avcodec_open2 当场失败**
  // （实测：注释掉这一行，日志里就是这句话 + Invalid argument（-22））。
  // 新分配的 AVCodecContext 默认就是 0/1，所以"单帧快照用不到时间基、
  // 干脆不填"是编不过去的。
  //
  // 填什么值对输出没有影响：一帧一个包，JPEG 里也不存时间戳。
  // 25 只是给个看得过去的默认值，免得别处读到 0 又去做除法。
  ctx_->time_base = AVRational{1, 25};

  // 固定质量（VBR 的反面）：快照要的是"这一张看着清楚"，
  // 不需要为了平均码率去牺牲某几帧。
  ctx_->flags |= AV_CODEC_FLAG_QSCALE;

  // --- 单线程，这不是保守，是正确性。 -----------------------------------
  //
  // FFmpeg 的帧级多线程内部排队：send_frame() 收下的是第 N 帧，
  // receive_packet() 吐出来的可能是第 N-1 帧（或者干脆 EAGAIN）。对快照来说
  // 这意味着 HTTP 响应里装的是**上一次请求那时**的画面，而且这个延迟
  // 随线程数增长 —— 拿到的是"过了期的桌面"，在监控场景里属于那种
  // 看起来完全正常、对不上时序才会发现的 bug。
  //
  // 而且这里也换不到吞吐：mjpeg 每帧独立（没有 P 帧参考），本来也不会
  // 因为多线程而快多少，而快照又是一个 HTTP 请求才编一帧。
  ctx_->thread_count = 1;

  int ret = avcodec_open2(ctx_, codec, nullptr);
  if (ret < 0) {
    D2O_ERROR("avcodec_open2(MJPEG %dx%d) 失败：%s", width, height,
              av_error(ret).c_str());
    close();
    return false;
  }

  if (!bgra_direct) {
    // 只用一次就丢的转换上下文没有意义重开，跟着编码器一起活着。
    // SWS_BILINEAR 在这里其实是"别做缩放"：输入输出同尺寸，
    // 真正干的是色彩空间转换。用更贵的 bicubic 在这个场景下没有区别。
    sws_ = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height,
                          dst_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) {
      D2O_ERROR("sws_getContext 失败（BGRA → %s）",
                av_get_pix_fmt_name(dst_fmt));
      close();
      return false;
    }
  }

  frame_ = av_frame_alloc();
  if (!frame_) {
    D2O_ERROR("av_frame_alloc 失败");
    close();
    return false;
  }
  frame_->format = ctx_->pix_fmt;
  frame_->width = ctx_->width;
  frame_->height = ctx_->height;
  // 32 字节对齐：mjpeg 内部的 DCT 和 SIMD 路径偏好对齐的行，而且对齐后
  // linesize 通常也更大，写起来不会和输入行重叠。注意我们**不**假设
  // linesize == width*4，encode() 里读的是 frame_->linesize[0]。
  ret = av_frame_get_buffer(frame_, 32);
  if (ret < 0) {
    D2O_ERROR("av_frame_get_buffer 失败：%s", av_error(ret).c_str());
    close();
    return false;
  }

  // --- frame->quality 的单位不是 qscale，是 lambda。--------------------
  //
  // 这是本模块最容易踩的一个坑：AVFrame.quality 的文档写的是
  // "quality (between 1 (good) and FF_LAMBDA_MAX (bad))"，也就是
  // **qscale * FF_QP2LAMBDA**（FF_QP2LAMBDA = 118），不是 qscale 本身。
  //
  // 直接把 qscale（比如 5）写进去，编码器算出来的实际 qscale 是 5/118 → 0，
  // 被夹到最小档。后果不是"画质不是我要的那一档"，而是**所有 2..31 的请求
  // 都塌成同一档**：传 2 和传 31 编出来的 jpg 一个字节都不差 —— 看起来
  // "质量参数生效了"（确实能编出图），只有把三档摆在一起比大小才会发现
  // qscale 根本没接上。实测（本机、本项目的 FFmpeg 9.0.2）：把 quality 原样
  // 写进去，qscale=2/10/28 编出来的都是 26377 字节，一个字节不差；
  // 乘上 FF_QP2LAMBDA 之后才是 26377 / 13796 / 9650。
  // tests/mjpeg_check.cpp 里那条"三档质量体积必须不同"的断言就是钉这个的。
  frame_->quality = quality * FF_QP2LAMBDA;

  pkt_ = av_packet_alloc();
  if (!pkt_) {
    D2O_ERROR("av_packet_alloc 失败");
    close();
    return false;
  }

  D2O_INFO("快照编码器就绪：MJPEG %dx%d 输入 BGRA→%s qscale=%d（%s）", width,
           height, av_get_pix_fmt_name(dst_fmt), quality,
           bgra_direct ? "直通，无转换" : "经 swscale 转换");
  return true;
}

std::string MjpegEncoder::encode(std::uint8_t const* bgra, int stride) {
  if (!ctx_ || !frame_ || !pkt_) {
    D2O_ERROR("快照编码器还没 init，丢掉这次请求");
    return std::string();
  }
  if (!bgra) {
    D2O_ERROR("快照输入为空指针");
    return std::string();
  }
  // stride 比一行还小说明调用方的记账是错的（比如拿了另一帧的 stride）。
  // 这种错误如果放过去，下面按行读会读出缓冲之外的地址。
  int const row_bytes = ctx_->width * 4;
  if (stride < row_bytes) {
    D2O_ERROR("快照 stride(%d) 小于一行的字节数(%d)，输入缓冲描述有误", stride,
              row_bytes);
    return std::string();
  }

  // 必须走 make_writable，不能直接往 frame_->data[0] 里写：
  // avcodec_send_frame() 是拿引用而不是拷贝，上一帧的缓冲可能还挂在编码器
  // 手上。缓冲没被别人引用时这个调用是零拷贝的（只是把 writable 标志置上），
  // 所以没有性能代价，属于"不写就是定时炸弹"的那类防御。
  int ret = av_frame_make_writable(frame_);
  if (ret < 0) {
    D2O_ERROR("av_frame_make_writable 失败：%s", av_error(ret).c_str());
    return std::string();
  }

  if (sws_) {
    // --- 行距只能通过 srcStride 交代，不能自己算。-----------------------
    //
    // 输入行距是调用方给的（DXGI 桌面复制的行距按纹理对齐，可能大于
    // width*4，本机 1080p 上就见过 4352 而 width*4 = 4320）。sws_scale 收
    // 的是"每行多少字节"，把真实的 stride 传进去它就对；如果这里图省事传
    // width*4，多出来的那几十个字节会被当成下一行的开头，画面沿对角线斜切，
    // **而且不会报任何错**：JPEG 的头、尺寸、能否解码全都正常，只有肉眼看
    // 才发现图是歪的，很容易被当成采集侧的问题查到别处去。
    // tests/mjpeg_check.cpp 用同一张图 + stride 加 64 字节再编一次、
    // 要求两次输出逐字节相同，就是为了让这个错误当场暴露。
    std::uint8_t const* src_data[4] = {bgra, nullptr, nullptr, nullptr};
    int src_linesize[4] = {stride, 0, 0, 0};
    sws_scale(sws_, src_data, src_linesize, 0, ctx_->height, frame_->data,
              frame_->linesize);
  } else {
    // BGRA 直通：逐行拷贝，只拷 width*4。绝不能按 stride*height 整块 memcpy ——
    // 目标只有 linesize*height 那么大，而且多出来的填充字节会被当成像素。
    std::size_t const dst_line = static_cast<std::size_t>(frame_->linesize[0]);
    for (int y = 0; y < ctx_->height; ++y) {
      std::memcpy(frame_->data[0] + static_cast<std::size_t>(y) * dst_line,
                  bgra + static_cast<std::size_t>(y) *
                             static_cast<std::size_t>(stride),
                  static_cast<std::size_t>(row_bytes));
    }
  }

  // 快照没有时间轴，pts 只要单调即可；一直用 0 会让某些版本把这一帧当成
  // "和上一帧同一个时间点"，给一个递增的假时间戳省掉这类麻烦。
  frame_->pts = ++pts_;

  ret = avcodec_send_frame(ctx_, frame_);
  if (ret < 0) {
    D2O_ERROR("avcodec_send_frame 失败：%s", av_error(ret).c_str());
    return std::string();
  }

  ret = avcodec_receive_packet(ctx_, pkt_);
  if (ret == AVERROR(EAGAIN)) {
    // 送进去了但这次没吐包。对 MJPEG + thread_count=1 正常不该发生，
    // 所以按"异常但非致命"处理：返回空串让调用方回 5xx，
    // 而不是把这当成编码器坏了去重开上下文。
    D2O_WARN("MJPEG 编码器收下了帧但没有产出包（EAGAIN），本次快照跳过");
    return std::string();
  }
  if (ret < 0) {
    D2O_ERROR("avcodec_receive_packet 失败：%s", av_error(ret).c_str());
    return std::string();
  }

  std::string jpeg(reinterpret_cast<char const*>(pkt_->data),
                   static_cast<std::size_t>(pkt_->size));
  // 包缓冲是引用计数的，拷进 std::string 之后立刻放掉，下一次 receive 才能
  // 复用这块内存。忘了 unref 会一路涨到内存耗尽。
  av_packet_unref(pkt_);
  return jpeg;
}

int MjpegEncoder::width() const { return ctx_ ? ctx_->width : 0; }

int MjpegEncoder::height() const { return ctx_ ? ctx_->height : 0; }

bool MjpegEncoder::ready() const { return ctx_ != nullptr && frame_ != nullptr; }

}  // namespace d2o
