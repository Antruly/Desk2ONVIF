#include "encoder/fmp4_muxer.h"

#include "common/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include <cstring>

namespace d2o {

namespace {

std::string av_error(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  if (av_strerror(err, buf, sizeof buf) < 0) return fmt("错误码 %d", err);
  return std::string(buf) + fmt("（%d）", err);
}

/// 流时基。**故意不用 1/fps**：
///
/// 调用方给的是微秒，用 1/fps 的话每次都要按帧长四舍五入；真实采集的时间戳
/// 不是帧长的整数倍（抖动、丢帧、录屏暂停），一旦两个相邻样本被舍进同一格，
/// mov 就会判定"非单调"，轻则打警告重则写出浏览器拒收的 tfdt。
/// 1/1000000 让这次换算成为恒等变换，抖动原样保留 —— 这正是我们想要的，
/// 时间戳本来就不该被封装层"修正"。
///
/// 代价是 track 的 timescale 变成 1000000（mov 默认是 15360 这种），
/// 对 mp4/CMAF 完全合法，Chrome 和 ffmpeg 都照单全收。
constexpr AVRational kMicro{1, 1000000};

/// 扫一段 Annex-B，看里面有没有 IDR（NALU type 5）。
///
/// 关键帧标志本来应该由 av_parser_parse2 填（它确实会填），但**只有它自己找
/// 帧边界的那套状态机跑起来时才填**。而那套状态机要求"下一帧的起始码已经到
/// 了"才肯结束当前帧，也就是每帧要压后一次调用才吐出来 —— 实测喂 300 个 AU
/// 只出来 299 个，最后一帧永远卡在解析器里出不来。**这一句原来跟着的下一句是
/// "本模块又没有 flush 的位置" —— 那个位置现在有了（`flush()`），但它管的是 mov
/// 的**分片边界**，不会替解析器吐出扣着的那一帧**，所以上面这个结论一个字都没变：
/// 别拿 `flush()` 当它的解药，也别因为"现在有 flush 了"就以为解析器会多吐一帧。
/// 所以解析器被切到 COMPLETE_FRAMES 模式（见 init()），代价是它不再判帧界，
/// 于是 s->key_frame 只剩第一帧是对的（实测 300 帧只标出 1 个）。
///
/// 关键帧判错的后果不是"画质差一点"，而是 frag_keyframe 再也不会触发：
/// 整个流只有一个分片，浏览器 append 完 init 段之后就再也没有东西可播。
/// 这么要紧的一件事，不能只押在一个模式开关上，所以这里自己扫一遍。
///
/// 起始码有 3 字节和 4 字节两种写法，必须都认：4 字节起始码的前三个字节就是
/// 一个合法的 3 字节起始码，只看 3 字节版本也能对上，但反过来 —— 把
/// `00 00 00 01` 整体跳过 —— 会连带跳过后面那个 NALU 头，type 就读成了
/// 后面数据的第一个字节。
bool has_idr(std::uint8_t const* d, std::size_t n) {
  std::size_t i = 0;
  while (i + 3 <= n) {
    std::size_t sc = 0;
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
      sc = 3;
    } else if (i + 4 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 &&
               d[i + 3] == 1) {
      sc = 4;
    }
    if (sc == 0) {
      ++i;
      continue;
    }
    if (i + sc >= n) return false;  // 只有起始码、没有头，后面不用看了
    if ((d[i + sc] & 0x1F) == 5) return true;
    i += sc + 1;
  }
  return false;
}

}  // namespace

int Fmp4Muxer::io_write(void* opaque, std::uint8_t const* data, int size) {
  auto* st = static_cast<IoState*>(opaque);
  // 返回 0 而不是负数：avio 把返回值当"写进去了多少字节"，负数会被当成错误码，
  // 而错误码 -32 之类在 size 的语境下毫无意义。小于等于 0 就什么都不做。
  if (!st || !st->buf || size <= 0) return 0;
  if (st->pos > st->buf->size()) st->buf->resize(st->pos, 0);
  if (st->pos + static_cast<std::size_t>(size) > st->buf->size()) {
    st->buf->resize(st->pos + static_cast<std::size_t>(size));
  }
  std::memcpy(st->buf->data() + st->pos, data, static_cast<std::size_t>(size));
  st->pos += static_cast<std::size_t>(size);
  return size;
}

std::int64_t Fmp4Muxer::io_seek(void* opaque, std::int64_t offset, int whence) {
  auto* st = static_cast<IoState*>(opaque);
  if (!st || !st->buf) return AVERROR(ENOSYS);

  int64_t const size = static_cast<int64_t>(st->buf->size());
  // AVSEEK_SIZE/AVSEEK_FORCE 是 avio 塞在高位上的标志，不是 SEEK_* 的值，
  // 先剥掉再 switch，否则永远落进 default。
  if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) return size;

  int64_t target = 0;
  switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: target = offset; break;
    case SEEK_CUR: target = static_cast<int64_t>(st->pos) + offset; break;
    case SEEK_END: target = size + offset; break;
    default: return AVERROR(ENOSYS);
  }
  if (target < 0) return AVERROR(EINVAL);
  // 允许 seek 到末尾之后：下一次写会把中间补齐成 0。分片封装正常情况下用不到，
  // 但让 pos 越过 size 是合法的（avio 自己就会这么干），拒绝它反而会误伤。
  st->pos = static_cast<std::size_t>(target);
  return target;
}

Fmp4Muxer::Fmp4Muxer() { io_state_.buf = &buffer_; }

Fmp4Muxer::~Fmp4Muxer() { teardown(); }

void Fmp4Muxer::teardown() {
  ready_ = false;
  have_base_ = false;
  have_last_ = false;
  // t0 是**每个会话一份**的：init() 会走这里，所以换一次会话就重新记一次。
  // 不重置的话，第二次会话报出去的 t0 会是上一次那个墙钟，于是浏览器算出来的
  // "真实延迟"平白多出一整段会话的时长 —— 而它看起来只是"延迟很大"。
  have_t0_ = false;
  t0_wall_ms_ = 0;
  last_dts_ = 0;
  first_duration_ = 0;
  // 分片那两个也是**每个会话一份**的。`pending_samples_` 不清的话，第二次会话
  // 的第一次 `flush()` 会看到上一次会话留下的计数、于是落在一个空封装器上；
  // `fragment_on_flush_` 不清的话，一次"开着 frag_custom 的会话"之后紧接着
  // 一次不开的会话，第二次会在 movflags 之外还留着"这个开关开着"这个印象 ——
  // 而 init() 的 doc 明说关掉时 movflags 与加它之前逐字节相同，那是个承诺。
  pending_samples_ = 0;
  fragment_on_flush_ = false;

  if (pkt_) av_packet_free(&pkt_);

  // 解析器的上下文是独立的（见 init()），先于解析器释放也无妨，但顺序反过来
  // 更符合直觉：先关解析器，再放它用过的上下文。
  if (parser_) av_parser_close(parser_);
  if (parser_ctx_) avcodec_free_context(&parser_ctx_);

  if (fmt_) {
    // 先 av_freep 掉 AVIO 的缓冲、再 avio_context_free，是为了让缓冲区的归属
    // 在两种解释下都成立：avio_context_free 的文档说它会释放"与之相关的一切"，
    // 但 avio_alloc_context 的文档又说缓冲区"之后要用 av_free 释放"。
    // av_freep 顺手把指针置空，后一步无论释不释放都不会二次释放。
    if (fmt_->pb) {
      av_freep(&fmt_->pb->buffer);
      // 这里传的是 &fmt_->pb，avio_context_free 会把 fmt_->pb 写成 nullptr，
      // 于是后面的 avformat_free_context 不可能再碰到同一个 pb。
      // 不这么做的话，两条释放路径的重叠与否要取决于 AVFMT_FLAG_CUSTOM_IO
      // 在各版本里的具体实现，那是没必要赌的东西。
      avio_context_free(&fmt_->pb);
    }
    avformat_free_context(fmt_);
    fmt_ = nullptr;
  }
  stream_ = nullptr;
  io_state_.buf = &buffer_;
  io_state_.pos = 0;
  buffer_.clear();
  init_segment_.clear();
}

bool Fmp4Muxer::init(int width, int height,
                     std::vector<std::uint8_t> const& extradata_annexb, int fps,
                     bool fragment_on_flush) {
  teardown();

  // 先落到成员上再往下走：下面第 4 段算 movflags 时读它，而那是**唯一**一处
  // 读它的地方。`flush()` 自己不回头看这个标志 —— 标志关掉时 mov 怎么对待那次
  // 强制 flush 是它的事（我们不替它断言），我们只保证"没开这个开关时 movflags
  // 与加它之前逐字节相同"。
  fragment_on_flush_ = fragment_on_flush;

  if (width <= 0 || height <= 0 || extradata_annexb.empty()) {
    D2O_ERROR("fMP4 初始化失败：参数不合法（%dx%d，extradata %zu 字节）", width,
              height, extradata_annexb.size());
    return false;
  }

  // --- 1. 借解码器把 Annex-B 的 extradata 变成一份可用的 codecpar -------------
  //
  // 看着绕，但这是唯一可靠的路子：mov 需要的是 avcC 形式的参数集（NALU 长度
  // 前缀 + SPS/PPS），而编码器给的是 Annex-B。avformat 里没有一个"把 Annex-B
  // 转成 codecpar"的公开入口，但解码器有 —— avcodec_open2 会解析 SPS，把
  // profile/level/尺寸填进 codecpar，extradata 则交给 mov 封装器在写 avcC 时
  // 转换（ff_isom_write_avcc 认起始码）。
  //
  // 这里失败基本只有一个原因：extradata 不是 SPS/PPS。那必须当错误处理 ——
  // 少了它，init 段里的 avcC 是空的，浏览器只会给一句含糊的
  // "DEMUXER_ERROR_COULD_NOT_PARSE"，然后什么都不播。
  AVCodec const* dec_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!dec_codec) {
    D2O_ERROR("fMP4 初始化失败：这个 FFmpeg 构建里没有 H.264 解码器");
    return false;
  }
  AVCodecContext* dec = avcodec_alloc_context3(dec_codec);
  if (!dec) {
    D2O_ERROR("fMP4 初始化失败：avcodec_alloc_context3 返回空");
    return false;
  }
  // extradata 后面必须留 AV_INPUT_BUFFER_PADDING_SIZE 个 0 字节：解码器和
  // 解析器都会按机器字长一次读过头几个字节，不填充就是越界读 —— Release 下
  // 大概率不炸，只是偶尔读到垃圾，属于最难查的一类。av_mallocz 顺手清零。
  dec->extradata = static_cast<std::uint8_t*>(
      av_mallocz(extradata_annexb.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!dec->extradata) {
    D2O_ERROR("fMP4 初始化失败：extradata 分配 %zu 字节失败",
              extradata_annexb.size());
    avcodec_free_context(&dec);
    return false;
  }
  std::memcpy(dec->extradata, extradata_annexb.data(), extradata_annexb.size());
  dec->extradata_size = static_cast<int>(extradata_annexb.size());

  int ret = avcodec_open2(dec, dec_codec, nullptr);
  if (ret < 0) {
    D2O_ERROR("fMP4 初始化失败：打开 H.264 解码器失败：%s",
              av_error(ret).c_str());
    avcodec_free_context(&dec);
    return false;
  }

  // --- 2. 输出上下文与视频流 -------------------------------------------------
  ret = avformat_alloc_output_context2(&fmt_, nullptr, "mp4", nullptr);
  if (ret < 0 || !fmt_) {
    D2O_ERROR("fMP4 初始化失败：创建 mp4 封装上下文失败：%s",
              av_error(ret).c_str());
    avcodec_free_context(&dec);
    return false;
  }

  stream_ = avformat_new_stream(fmt_, nullptr);
  if (!stream_) {
    D2O_ERROR("fMP4 初始化失败：avformat_new_stream 返回空");
    avcodec_free_context(&dec);
    teardown();
    return false;
  }

  ret = avcodec_parameters_from_context(stream_->codecpar, dec);
  if (ret < 0) {
    D2O_ERROR("fMP4 初始化失败：复制编码参数失败：%s", av_error(ret).c_str());
    avcodec_free_context(&dec);
    teardown();
    return false;
  }
  // 尺寸以调用方给的为准。解码器只有在真正解过一帧之后才会把 SPS 里的尺寸填进
  // 自己的上下文；只经手过 extradata 时那里可能是 0，照抄就会写出一个 0x0
  // 的 stsd —— 浏览器把它当成"没有视频轨"，同样什么都不播。
  stream_->codecpar->width = width;
  stream_->codecpar->height = height;
  // 时基必须在 avformat_write_header **之前**设：写头时 mov 用它决定 track 的
  // timescale，之后再改就只是改一个没人再读的变量了。
  stream_->time_base = kMicro;

  // 解码器到此为止：codecpar 已经把需要的东西深拷贝走了。
  avcodec_free_context(&dec);

  // --- 3. 自定义 AVIO --------------------------------------------------------
  //
  // 4KB 够用：真正决定延迟的不是它，而是每次写完都 avio_flush（见 write()）。
  std::uint8_t* avio_buf = static_cast<std::uint8_t*>(av_malloc(4096));
  if (!avio_buf) {
    D2O_ERROR("fMP4 初始化失败：AVIO 缓冲分配失败");
    teardown();
    return false;
  }
  io_state_.buf = &buffer_;
  io_state_.pos = 0;
  fmt_->pb = avio_alloc_context(avio_buf, 4096, 1 /*write*/, &io_state_, nullptr,
                                &Fmp4Muxer::io_write, &Fmp4Muxer::io_seek);
  if (!fmt_->pb) {
    // avio_alloc_context 失败时不会接手这块缓冲，得自己放掉。
    av_free(avio_buf);
    D2O_ERROR("fMP4 初始化失败：avio_alloc_context 返回空");
    teardown();
    return false;
  }
  // 告诉 avformat 这个 pb 是我们的：它的关闭路径（avio_closep/avformat_free_context）
  // 会因此绕开它，由 teardown() 统一收尾。
  fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;

  // --- 4. 分片开关 -----------------------------------------------------------
  //
  // 三个一个都不能少，少哪个都能编出一个"能播但没法直播"的文件：
  //   frag_keyframe  每个关键帧起一个新分片。MSE 的 SourceBuffer 要靠它才能
  //                  在任意时刻 append 并从分片头开始解码；关掉的话整个流是
  //                  一个巨大的分片，浏览器要等全部到齐才出画。
  //   empty_moov     让 moov 在写头的时候就出来（内容是空的）。默认行为是
  //                  把 moov 留到收尾才写 —— 直播流根本没有"收尾"那一刻，
  //                  于是 init 段永远是空的，init_segment() 会返回 0 字节。
  //   default_base_moof  trun 里的 data_offset 以 moof 为基准（CMAF/DASH 的
  //                  要求）。这个是给更严格的解析器准备的，浏览器两种都吃，
  //                  但既然 CMAF 规定了就没必要省。
  //
  // frag_custom 是**第四个**，默认**不加**，只在预览那条路上由 fragment_on_flush_
  // 打开。⚠️ **它的作用与它的名字给人的印象不一样**（量过的结论，读数在头文件
  // `flush()` 那段注释里）：真正切分片的是 `flush()` 里那句
  // `av_write_frame(fmt_, nullptr)`，**不是这个标志**。`tests/fmp4_check.cpp` 的
  // 「切碎不等于切坏」拿开关关着的对象、同样调 43 次 flush()：分片数一样
  // （47/47）、样本一样（300/300），两份输出逐字节相同。
  //
  // 所以"不加它时上面这三个的组合与加它之前逐字节相同"这句话是对的，但它守的
  // 不是"切不切"，只是"默认行为不变"。这一段原先把后半句写成"于是延迟收益可以
  // 拿两次构建对减" —— 逐字节相同这件事恰好否掉了那个做法：差是 0。要 A/B 的是
  // 调用不调用 flush()，那一处在 capture_session.cpp。
  //
  // 字符串在这里拼好、下面 logging 那一行直接复用它：**分片策略只有一份来源**，
  // 于是"日志里写的"和"实际设进去的"不可能漂移 —— 而这两者漂移的症状是排查的人
  // 按日志去查一个根本没开着的开关。
  std::string movflags = "+frag_keyframe+empty_moov+default_base_moof";
  if (fragment_on_flush_) movflags += "+frag_custom";
  av_opt_set(fmt_->priv_data, "movflags", movflags.c_str(), 0);

  ret = avformat_write_header(fmt_, nullptr);
  if (ret < 0) {
    D2O_ERROR("fMP4 初始化失败：写 init 段失败：%s", av_error(ret).c_str());
    teardown();
    return false;
  }
  // avio 是带缓冲的：不 flush 的话 moov 可能还躺在那个 4KB 的缓冲里，
  // 下面 assign 出来的 init 段就是半截的 —— 而这种"少了最后几十字节"的 init
  // 段浏览器照样会收下，然后在 append 第一个分片时才翻脸。
  avio_flush(fmt_->pb);

  // 帧率不进时基（见 kMicro 的说明），只在这里用一次：给第一个样本一个时长。
  // 其余样本的时长是相邻 dts 之差，第一个没有"上一个"可减 —— 留 0 的话 trun 里
  // 会出现一个零时长的样本。放在写头**之后**算，是因为 mov 可能按自己的规矩
  // 改写过 stream_->time_base，时长得跟着它最后的那个来。
  first_duration_ =
      fps > 0 ? av_rescale_q(1000000 / fps, kMicro, stream_->time_base) : 0;

  init_segment_.assign(buffer_.begin(), buffer_.end());
  buffer_.clear();
  io_state_.pos = 0;

  if (init_segment_.empty()) {
    D2O_ERROR("fMP4 初始化失败：init 段是空的（moov 没出来）");
    teardown();
    return false;
  }

  // --- 5. 解析器 -------------------------------------------------------------
  parser_ = av_parser_init(AV_CODEC_ID_H264);
  parser_ctx_ = avcodec_alloc_context3(nullptr);
  if (!parser_ || !parser_ctx_) {
    D2O_ERROR("fMP4 初始化失败：H.264 解析器分配失败");
    teardown();
    return false;
  }
  // 给解析器一个"空"的上下文，而不是上面那个解码器：解码器身上带着 Annex-B 的
  // extradata，而解析器看到 avctx->extradata_size > 0 就会往"这是 AVC
  // （长度前缀）"那条路走。我们要它老老实实按起始码找 NALU。
  parser_ctx_->extradata = nullptr;
  parser_ctx_->extradata_size = 0;

  // 把解析器自己的日志压掉。
  //
  // 它会**每帧**打一行 `non-existing PPS 0 referenced`（ERROR 级，约 30 行/秒），
  // 原因正是上面那句"不给 extradata"：解析器看不到 SPS/PPS，于是每遇到一个
  // slice 都报一次"这个 PPS 我没见过"。这是我们的**刻意配置**造成的，不是故障
  // 信号 —— 参数集在 init 段里（avcC），本来就不该在这条路上出现。
  //
  // 压掉它是安全的：解析失败我们看的是 `av_parser_parse2` 的**返回值**
  // （write() 里 `used < 0` 就报错退出），不依赖它自己打印什么。
  //
  // 取值是偏移量，不是级别：av_log 判的是 `level + offset > av_log_level`，
  // 而级别是 PANIC 0 / FATAL 8 / ERROR 16 / WARNING 24 / INFO 32。
  // 偏移 24 把 ERROR 抬到 40、WARNING 抬到 48，都超过默认阈值 INFO(32) 而被丢弃；
  // FATAL 落在 32，仍然打得出来 —— 留下"真的坏了"那种，丢掉"每帧都来一条"那种。
  parser_ctx_->log_level_offset = 24;  // AV_LOG_WARNING

  // 一次 write() 正好一个完整访问单元 —— 这是 Fmp4Sample 的约定，直接告诉解析器，
  // 让它别再去猜边界。理由见 has_idr() 的说明：它猜边界的那套状态机会把每帧压后
  // 一次调用，最后一帧则永远出不来。
  parser_->flags |= PARSER_FLAG_COMPLETE_FRAMES;

  pkt_ = av_packet_alloc();
  if (!pkt_) {
    D2O_ERROR("fMP4 初始化失败：av_packet_alloc 失败");
    teardown();
    return false;
  }

  ready_ = true;
  D2O_INFO("fMP4 就绪：%dx%d，init 段 %zu 字节，时基 1/%d，分片策略 %s",
           width, height, init_segment_.size(), kMicro.den, movflags.c_str());
  return true;
}

std::vector<std::uint8_t> const& Fmp4Muxer::init_segment() const {
  return init_segment_;
}

bool Fmp4Muxer::write_nal(std::uint8_t const* data, int size,
                          Fmp4Sample const& s) {
  // 时间戳：先减去会话 0 点。调用方完全可能用绝对时钟（QPC/steady_clock），
  // 直接把它当 tfdt 写出去的话，1e6 的 timescale 乘上那个量级会溢出 int64，
  // 而 mov 对 tfdt 是 64 位无符号，溢出的结果是浏览器收到一个天文数字的
  // baseMediaDecodeTime，然后安静地不播。
  std::int64_t const dts_us = s.dts_us - base_dts_us_;
  std::int64_t const pts_us = s.pts_us - base_dts_us_;

  std::int64_t dts = av_rescale_q(dts_us, kMicro, stream_->time_base);
  std::int64_t pts = av_rescale_q(pts_us, kMicro, stream_->time_base);

  // 严格递增。timescale 变成 1000000 之后这里基本不会触发，但采集侧一旦出现
  // 两个样本同一个时间戳（重复帧、时钟回拨），mov 会报 "Non-monotonous DTS"
  // 并把两个样本写进同一个 tick —— 分片里的 sample duration 变成 0，
  // 表现是画面卡住或整段被浏览器丢掉。宁可把重复的那个往后推一格。
  if (have_last_ && dts <= last_dts_) {
    D2O_WARN("fMP4：时间戳没有前进（dts=%lld 上一个=%lld），已推到下一个刻度",
             static_cast<long long>(dts), static_cast<long long>(last_dts_));
    dts = last_dts_ + 1;
  }
  if (pts < dts) pts = dts;  // pts 早于 dts 是非法组合，B 帧场景下 mov 会另算 ctts

  // 关键帧三个来源取或：调用方说的、解析器认出来的、自己扫出来的。
  // 前两个都可能缺席（解析器在 COMPLETE_FRAMES 下几乎永远不填，调用方也可能
  // 懒得标），而漏标一个关键帧的后果是分片少一个 —— 不致命但会让延迟变大；
  // 多标一个只是多一个分片。所以这里偏保守。
  bool const key = s.keyframe || (parser_->key_frame > 0) || has_idr(data, size);

  av_packet_unref(pkt_);
  int const aret = av_new_packet(pkt_, size);
  if (aret < 0) {
    D2O_ERROR("fMP4：为 %d 字节的样本分配包失败：%s", size, av_error(aret).c_str());
    return false;
  }
  std::memcpy(pkt_->data, data, static_cast<std::size_t>(size));
  pkt_->pts = pts;
  pkt_->dts = dts;
  pkt_->stream_index = stream_->index;
  // 自己给时长，别让封装器去猜：mov 对手里最后一个样本是没法算时长的
  // （没有"下一个"了），留给它填 0 的话，分片里最后一帧的呈现时长为 0，
  // 播放器可能直接跳过这一帧。恒帧率下用上一帧的间隔就是对的，第一个样本
  // 没有上一帧，用 init() 里按 fps 算出来的那个。
  pkt_->duration = have_last_ ? (dts - last_dts_) : first_duration_;
  if (key) pkt_->flags |= AV_PKT_FLAG_KEY;

  // --- 媒体时间 0 点对回墙钟（浏览器那把"真实延迟"尺子的锚）-------------------
  //
  // **定这个 0 点的是封装器，不是上面那两行减法。** 这是量出来的，不是推的：
  // 把 `base_dts_us_ = s.dts_us;` 改成 `= 0;`（也就是把我们那次减法整个拿掉）、
  // 再把交出去的 dts 整体平移 74 秒重新编译，输出里的 tfdt 仍然从 0 起、
  // **与基线逐字节相同**（`out/fmp4-t0-sweep2.txt` 的 M1）。libavformat 的 mov
  // 封装器按"这条轨看到的第一个 dts"重新定基准，所以我们减不减它都无所谓
  // （上面那次减法的真正职责是别让 1e6 的 timescale 撞上 int64，见本函数开头）。
  //
  // 于是锚点就是**第一个落到 av_interleaved_write_frame 的样本**，而它必须
  // 在这里取，不能在 write() 入口取：这个函数的 `s` 是**当前**这个样本的，
  // 解析器正常模式下无法在没有下一个起始码的情况下切出输入里的最后一个访问
  // 单元 ⇒ 第一个落到封装器的包可能带着**第 1 个样本的 dts、装的却是第 0 个
  // 样本的像素**。差一个帧间隔（30fps 下 33 ms）—— 在一把 150 ms 容差的尺子上
  // 看不出来，但那是"差不多对"，正是这一阶段要消灭的东西。
  //
  // `wall_ms <= 0` 表示调用方没提供墙钟（测试、以及不用预览的路径）。这时**不记**，
  // `has_t0()` 保持为假、界面上显示"未知" —— 比报一个看起来合理的假数好：
  // 拿 0 当 t0 的话前端算出来的是 1970 年到现在，而它会被当成"延迟巨大"去查。
  if (!have_t0_ && s.wall_ms > 0) {
    t0_wall_ms_ = s.wall_ms;
    have_t0_ = true;
  }

  // av_interleaved_write_frame 会拿走包的内容（返回后 pkt_ 是空的），
  // 这正是我们要的：pkt_ 结构体留着复用，内容本来也要丢。
  int const wret = av_interleaved_write_frame(fmt_, pkt_);
  if (wret < 0) {
    D2O_ERROR("fMP4：写样本失败（%d 字节，key=%d）：%s", size,
              key ? 1 : 0, av_error(wret).c_str());
    return false;
  }

  last_dts_ = dts;
  have_last_ = true;
  // 只有**真的落进封装器**才计数（上面那条 return false 的路径不经过这里）：
  // `flush()` 拿它当"有没有东西可以切"的闸门，一个写失败的样本不该让那道闸门
  // 打开 —— 否则 flush() 会落到一个没有任何样本的封装器上，而 mov 在强制 flush
  // 之下会照样吐一个**零样本**的 moof+mdat，那是 MSE 的 SourceBuffer 当场拒收的
  // 东西（`AppendError`）。这条闸门的位置就是这么定下来的。
  ++pending_samples_;
  return true;
}

bool Fmp4Muxer::write(Fmp4Sample const& s, std::vector<std::uint8_t>& out) {
  if (!ready_ || !fmt_ || !stream_ || !parser_ || !parser_ctx_ || !pkt_) {
    D2O_ERROR("fMP4：还没初始化就收到了样本");
    return false;
  }
  if (!s.data || s.size == 0) {
    D2O_WARN("fMP4：空样本（size=%zu），跳过", s.size);
    return false;
  }

  if (!have_base_) {
    base_dts_us_ = s.dts_us;
    have_base_ = true;
  }

  // 每次调用前清空：mov 的分片是"整块写出"的 —— 要么一个 moof+mdat 一起落下来，
  // 要么什么都不写（样本还在它自己的 cluster 里攒着，等下一个关键帧）。
  // 不存在"写到一半跨调用"的情况，所以 buffer_ 里的东西一定属于这一次调用。
  buffer_.clear();
  io_state_.pos = 0;

  bool ok = true;
  std::uint8_t const* p = s.data;
  int remaining = static_cast<int>(s.size);
  while (remaining > 0) {
    std::uint8_t* nal = nullptr;
    int nal_size = 0;
    int const used = av_parser_parse2(parser_, parser_ctx_, &nal, &nal_size, p,
                                      remaining, s.pts_us, s.dts_us,
                                      AV_NOPTS_VALUE);
    if (used < 0) {
      D2O_ERROR("fMP4：H.264 解析失败：%s", av_error(used).c_str());
      ok = false;
      break;
    }
    p += used;
    remaining -= used;

    if (nal_size > 0 && !write_nal(nal, nal_size, s)) ok = false;

    // 解析器一个字节都没吃下去就说明它卡住了（正常模式下它会等下一帧的起始码，
    // COMPLETE_FRAMES 下不会发生）。再转一圈就是死循环，宁可丢一个样本。
    if (used <= 0) {
      if (remaining > 0) {
        D2O_WARN("fMP4：解析器没有前进，丢弃剩下的 %d 字节", remaining);
        ok = false;
      }
      break;
    }
    if (!ok) break;
  }

  // 把这次产生的字节整块交给调用方。注意 out 是**追加**的：调用方可能把
  // init 段和若干分片拼在同一个 vector 里发出去。
  if (!buffer_.empty()) {
    out.insert(out.end(), buffer_.begin(), buffer_.end());
    buffer_.clear();
    io_state_.pos = 0;
    // 这次 write() 吐了字节出来 ⇒ 封装器刚刚切了一刀（`frag_keyframe` 那一刀，
    // 它落在哪个关键帧上我们看不见），攒着的样本已经落盘了。清零点就在这里，
    // 依据是"有输出"这个**可观测**的事实，而不是去猜 mov 内部的状态。
    // 不清的话 `flush()` 会在下一个强制切分点上把上一刀之后那些已经写出去的
    // 样本再数一遍 —— 数多了不会写坏东西（闸门只是"非零即开"），但那个数就
    // 不再是"攒着多少"，而是"喂过多少"，两个名字里只有一个是真的。
    pending_samples_ = 0;
  }
  return ok;
}

bool Fmp4Muxer::flush(std::vector<std::uint8_t>& out) {
  if (!ready_ || !fmt_ || !stream_) {
    D2O_ERROR("fMP4：还没初始化就要求切分片");
    return false;
  }
  // 没有新样本就是空操作。闸门放在这里、不指望调用方自己数，理由见头文件。
  if (pending_samples_ == 0) return true;

  buffer_.clear();
  io_state_.pos = 0;

  // **必须用 `av_write_frame`，不能用 `av_interleaved_write_frame`。** 两者在
  // `pkt == nullptr` 时的语义**不是**同一件事，这是逐字读随包发布的头文件定下来的
  // （不是回忆、也不是猜）：
  //   av_write_frame()            "This parameter can be NULL (at any time, not just
  //                                at the end), in order to immediately flush data
  //                                buffered within the muxer, for muxers that buffer
  //                                up data internally before writing it to the output."
  //   av_interleaved_write_frame() "This parameter can be NULL (at any time, not just
  //                                at the end), to flush the **interleaving queues**."
  // 我们要的是前者 —— 让 mov 把攒着的那一簇样本切出来。后者只清空交织队列，
  // 而我们是单流、包早就穿过它了，刷它等于什么都没做。
  // （两段文档里都**没有**"不要混用"的警告，所以在 write() 用交织版、这里用
  // 直接版是可以的；真要哪天合成一条路，得先把这个前提重新量一遍。）
  int const ret = av_write_frame(fmt_, nullptr);
  if (ret < 0) {
    D2O_ERROR("fMP4：切分片失败：%s", av_error(ret).c_str());
    // 不清 pending_samples_：这一刀没切成，样本还在封装器里，下一次 flush()
    // 应该再试一次。清掉就等于把它们扔了。
    return false;
  }
  // 和 write() 同一个理由：avio 是带缓冲的，不 flush 的话刚切出来的分片可能还
  // 躺在那个 4KB 的缓冲里没落进 buffer_ —— 于是"切了一刀"在调用方看来是零字节，
  // 而零字节和"没到该切的时候"长得一模一样。
  avio_flush(fmt_->pb);

  if (!buffer_.empty()) {
    out.insert(out.end(), buffer_.begin(), buffer_.end());
    buffer_.clear();
    io_state_.pos = 0;
    // 只有真的交出了字节才算这一刀切成了。封装器完全可能"收下了 flush 但一个分片
    // 都没写"（还没攒够、或者分片边界不在这里），那时清零就是把闸门重新关上，
    // 攒着的样本从此再没人去切 —— 而它在画面上只表现为"最后一段预览卡住"。
    pending_samples_ = 0;
  }
  return true;
}

}  // namespace d2o
