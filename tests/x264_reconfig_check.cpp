/**
 * @file tests/x264_reconfig_check.cpp
 * @brief 实验：**运行中改 `AVCodecContext::bit_rate`，libx264 会不会真的听？**
 *
 * 为什么值得单独一个实验：这决定了 `/api/config` 改码率要不要重启进程。
 * 而两种做法都有明确的坏处 ——
 *
 *   不热生效：用户盯着画面调码率，每调一次流就断一次。而"码率"恰恰是唯一
 *             一个人们会一边看一边调的参数（画面糊了 / 太占带宽了）。
 *   假装热生效：改了 `avctx->bit_rate` 但 FFmpeg 的 libx264 封装层并不把它
 *             转给 `x264_encoder_reconfig`，于是**输出码率纹丝不动**，而网页
 *             上、日志里、配置里全都是新值。这比不热生效糟得多。
 *
 * gyan 的预编译包里既没有 `x264.h` 也没有 `libx264.dll.a`（x264 是静态编进
 * avcodec 里的），所以**没有**直接调 `x264_encoder_reconfig` 这条路。能试的
 * 只有"改 avctx 的字段，看封装层认不认"。
 *
 * 判据是**实测码率**，不是"有没有报错"：编码器对不认识的新值完全可能一声不吭。
 * 所以这里编三段，每段 60 帧，中间只改 bit_rate，然后比三段的实际字节数 ——
 * 画面内容用同一个伪随机序列生成，**三段的内容统计特性完全一样**，
 * 于是字节数的差别只能来自码率设置。
 *
 * 编译（只需要 FFmpeg）：
 *   g++ -std=c++17 -Ithird_party/ffmpeg/include tests/x264_reconfig_check.cpp \
 *       -Lthird_party/ffmpeg/lib -lavcodec -lavutil -o build/x264_reconfig_check.exe
 */

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace {

/// 确定性噪声 + 缓慢移动的方块。**每段都用同样的种子重来一遍**，这样三段喂
/// 进去的画面序列逐字节相同，字节数的差别就只能归因于码率。
void fill_frame(AVFrame* f, int w, int h, int i) {
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // 一块在动的亮块 + 每帧都变的伪随机噪声（噪声逼着编码器真的花码率，
      // 否则静止画面下三段的字节数都会趋近于 0，比不出任何东西）。
      bool const block = ((x + i * 4) / 40 + (y / 40)) % 2 == 0;
      std::uint32_t const n = (static_cast<std::uint32_t>(x) * 2654435761u) ^
                              (static_cast<std::uint32_t>(y) * 40503u) ^
                              (static_cast<std::uint32_t>(i) * 2246822519u);
      std::uint8_t const v = static_cast<std::uint8_t>((n >> 16) & 0x7F);
      f->data[0][y * f->linesize[0] + x] = static_cast<std::uint8_t>(block ? 200 : 40 + v / 2);
      f->data[1][(y / 2) * f->linesize[1] + x / 2] = 128;
      f->data[2][(y / 2) * f->linesize[2] + x / 2] = 128;
    }
  }
}

/// 编一段，返回这段产出的总字节数和包数。
struct Segment {
  std::int64_t bytes = 0;
  int packets = 0;
  int keyframes = 0;
};

Segment encode_segment(AVCodecContext* ctx, int w, int h, int first_i, int count) {
  Segment s;
  AVFrame* f = av_frame_alloc();
  f->format = AV_PIX_FMT_YUV420P;
  f->width = w;
  f->height = h;
  av_frame_get_buffer(f, 32);

  AVPacket* pkt = av_packet_alloc();
  for (int i = 0; i < count; ++i) {
    av_frame_make_writable(f);
    fill_frame(f, w, h, first_i + i);
    f->pts = first_i + i;
    if (avcodec_send_frame(ctx, f) < 0) break;
    while (avcodec_receive_packet(ctx, pkt) == 0) {
      s.bytes += pkt->size;
      s.packets++;
      if (pkt->flags & AV_PKT_FLAG_KEY) s.keyframes++;
      av_packet_unref(pkt);
    }
  }
  av_packet_free(&pkt);
  av_frame_free(&f);
  return s;
}

}  // namespace

int main() {
  int failures = 0;
  auto check = [&](bool ok, std::string const& what) {
    std::printf(ok ? "  [ OK ] %s\n" : "  [FAIL] %s\n", what.c_str());
    if (!ok) failures++;
  };

  int const W = 1280, H = 720, FPS = 30, N = 60;
  int const kLow = 1000, kHigh = 12000;
  std::string const preset = "ultrafast";
  std::string const tune = "zerolatency";

  AVCodec const* codec = avcodec_find_encoder_by_name("libx264");
  if (codec == nullptr) {
    std::printf("找不到 libx264 编码器。\n");
    return 2;
  }

  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  ctx->width = W;
  ctx->height = H;
  ctx->time_base = AVRational{1, FPS};
  ctx->framerate = AVRational{FPS, 1};
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->gop_size = FPS * 2;
  ctx->max_b_frames = 0;
  ctx->bit_rate = static_cast<std::int64_t>(kLow) * 1000;
  ctx->thread_count = 4;
  ctx->thread_type = FF_THREAD_FRAME;
  av_opt_set(ctx->priv_data, "preset", preset.c_str(), 0);
  av_opt_set(ctx->priv_data, "tune", tune.c_str(), 0);
  av_opt_set(ctx->priv_data, "x264-params", "slices=1:sliced-threads=0", 0);

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    std::printf("打开编码器失败。\n");
    return 2;
  }

  std::printf("== x264 运行中改码率：%d kbps → %d kbps ==\n", kLow, kHigh);
  std::printf("   %dx%d @%dfps，每段 %d 帧，三段内容逐字节相同\n\n", W, H, FPS, N);

  // ---- 第一段：低码率 -----------------------------------------------------
  Segment const s1 = encode_segment(ctx, W, H, 0, N);
  double const kbps1 = s1.bytes * 8.0 / (N / static_cast<double>(FPS)) / 1000.0;
  std::printf("   第 1 段（bit_rate=%d）      %8lld 字节  %4d 包  → %8.1f kbps\n",
              kLow, static_cast<long long>(s1.bytes), s1.packets, kbps1);

  // ---- 改码率（就是这一步在验）-------------------------------------------
  ctx->bit_rate = static_cast<std::int64_t>(kHigh) * 1000;

  Segment const s2 = encode_segment(ctx, W, H, 0, N);
  double const kbps2 = s2.bytes * 8.0 / (N / static_cast<double>(FPS)) / 1000.0;
  std::printf("   第 2 段（bit_rate=%d）    %8lld 字节  %4d 包  → %8.1f kbps\n",
              kHigh, static_cast<long long>(s2.bytes), s2.packets, kbps2);

  // ---- 改回低码率：只往上调的话，"变高"也可能是别的原因（比如编码器内部
  //      状态变化），调回来能排掉这一类解释。--------------------------------
  ctx->bit_rate = static_cast<std::int64_t>(kLow) * 1000;
  Segment const s3 = encode_segment(ctx, W, H, 0, N);
  double const kbps3 = s3.bytes * 8.0 / (N / static_cast<double>(FPS)) / 1000.0;
  std::printf("   第 3 段（bit_rate=%d）      %8lld 字节  %4d 包  → %8.1f kbps\n\n",
              kLow, static_cast<long long>(s3.bytes), s3.packets, kbps3);

  std::printf("   参考：x264 是 ABR，单段 2 秒的短窗口会有偏差；这里要看的\n"
              "         不是绝对值，是**第 2 段相对第 1/3 段有没有跟着翻上去**。\n\n");

  check(s2.bytes > s1.bytes * 2, "调高 bit_rate 之后产出字节数确实涨了（>2 倍）");
  check(s3.bytes < s2.bytes / 2, "再调低之后又降回去了");

  bool const honored = s2.bytes > s1.bytes * 2 && s3.bytes < s2.bytes / 2;
  std::printf("\n=== 结论 ===\n");
  if (honored) {
    std::printf("FFmpeg 的 libx264 封装层**认**运行中改 ctx->bit_rate —— 码率可以热生效。\n");
  } else {
    std::printf("FFmpeg 的 libx264 封装层**不认**运行中改 ctx->bit_rate（改了没反应）。\n"
                "所以码率热生效必须走 x264_encoder_reconfig，而预编译包里没有 x264.h\n"
                "和 libx264 导入库 —— 结论：码率只能标成「重启后生效」。\n");
  }

  avcodec_free_context(&ctx);
  return failures ? 1 : 0;
}
