#include "encoder/mp4_writer.h"

#include "common/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace d2o {

namespace {

std::string av_error(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  if (av_strerror(err, buf, sizeof buf) < 0) return fmt("错误码 %d", err);
  return std::string(buf) + fmt("（%d）", err);
}

/// 打开过程中失败时的清理。不能顺手调 close()：那会把 err 覆盖成
/// "写 MP4 索引失败"，把真正的原因（比如路径打不开）盖掉。
void abandon(AVFormatContext*& fmt) {
  if (!fmt) return;
  if (fmt->pb) avio_closep(&fmt->pb);
  avformat_free_context(fmt);
  fmt = nullptr;
}

}  // namespace

Mp4Writer::~Mp4Writer() {
  if (fmt_) {
    std::string ignored;
    close(ignored);  // 忘了调 close 时至少别泄漏，但文件多半是坏的
  }
}

bool Mp4Writer::add_stream(AVCodecContext const* enc, std::string& err) {
  AVStream* st = avformat_new_stream(fmt_, nullptr);
  if (!st) {
    err = "avformat_new_stream 失败";
    return false;
  }

  int const ret = avcodec_parameters_from_context(st->codecpar, enc);
  if (ret < 0) {
    err = "复制编码参数到流失败：" + av_error(ret);
    return false;
  }

  // 陷阱 74：上面那一句**不搬运**这个字段，而 mov 的 edit list 语义上要靠它，
  // 所以这里显式搬一次。
  //
  // ⚠️ **但搬不搬在这条链路上量不出来**（tests/mp4_dual_check.cpp 实测）：把这句
  // 注释掉、重编，两个 mp4 逐字节相同，读回来的 initial_padding 也都是 1024。
  // 因为 FFmpeg 的 aac 编码器把首包 pts 整体前移 -initial_padding（实测首包
  // dts = −1024），mov 的 muxer 是从**那个负 dts** 推的 edit list。所以这一句是
  // "按契约写对"而不是"修一个看得见的毛病"。
  //
  // 两路都搬（视频编码器这个字段实测是 0，搬了没有副作用）：只给音频搬的话，
  // 下一个加进来的人会以为这个字段"只对音频有意义"，而那是个错误的印象。
  st->codecpar->initial_padding = enc->initial_padding;

  // 时基跟随编码器（视频是 1/fps、音频是 1/采样率）。不自己换算成 mov 惯用的
  // 1/90000：让 libavformat 去算，我们只需要记住"进来的包用的是哪个时基"。
  st->time_base = enc->time_base;

  // **这一份必须在 avformat_write_header 之前记下来，而且每一路各记一份。**
  // 写文件头时 libavformat 会按封装格式的要求改写 st->time_base（mov 把它换成
  // 1/15360 这种），之后就读不到原值了；而拿视频那份去 rescale 音频的包，
  // 两路各自的 ffprobe 输出看着都正常，只有时间轴错位（陷阱 75）。
  src_time_base_.push_back(TimeBase{enc->time_base.num, enc->time_base.den});

  D2O_DEBUG("MP4 第 %zu 路：时基 %d/%d，启动填充 %d 样本",
            src_time_base_.size() - 1, enc->time_base.num, enc->time_base.den,
            enc->initial_padding);
  return true;
}

bool Mp4Writer::open(std::string const& path, AVCodecContext const* venc,
                     AVCodecContext const* aenc, std::string& err,
                     bool faststart) {
  if (!venc) {
    err = "MP4 至少要有一路视频（venc 是空的）";
    return false;
  }
  path_ = path;
  // 复用同一个对象再 open 一次时，别留着上一次的时基 —— 它会正好落在
  // write() 那个范围检查的**内侧**，于是错的那一路被静默写进去。
  src_time_base_.clear();

  int ret = avformat_alloc_output_context2(&fmt_, nullptr, "mp4", path.c_str());
  if (ret < 0 || !fmt_) {
    err = "创建 MP4 封装上下文失败：" + av_error(ret);
    return false;
  }

  // 顺序固定：**0 = 视频，1 = 音频**（见文件头）。调用方按这个编号填
  // pkt.stream_index。aenc 为空就不建音频流 —— "有音轨但是静音"和"没有音轨"
  // 对 NVR 与播放器是两回事（陷阱 78）。
  if (!add_stream(venc, err)) {
    abandon(fmt_);
    src_time_base_.clear();
    return false;
  }
  if (aenc && !add_stream(aenc, err)) {
    abandon(fmt_);
    src_time_base_.clear();
    return false;
  }

  if (faststart) {
    // 失败不算致命 —— 只是索引留在文件末尾而已。
    av_opt_set(fmt_->priv_data, "movflags", "+faststart", 0);
  }

  // FFmpeg 在 Windows 上会把 UTF-8 路径转成宽字符再 open，中文路径没问题，
  // 不必自己去转。
  ret = avio_open(&fmt_->pb, path.c_str(), AVIO_FLAG_WRITE);
  if (ret < 0) {
    err = fmt("打不开输出文件 %s：%s", path.c_str(), av_error(ret).c_str());
    abandon(fmt_);
    src_time_base_.clear();
    return false;
  }

  ret = avformat_write_header(fmt_, nullptr);
  if (ret < 0) {
    err = "写 MP4 文件头失败：" + av_error(ret);
    abandon(fmt_);
    src_time_base_.clear();
    return false;
  }

  D2O_INFO("输出文件已就绪：%s（%zu 路：视频%s）", path.c_str(),
           src_time_base_.size(), aenc ? " + 音频" : "");
  return true;
}

bool Mp4Writer::write(AVPacket& pkt, std::string& err) {
  if (!fmt_) {
    err = "MP4 封装器未打开";
    return false;
  }

  // stream_index 由调用方填，这里只认它（见文件头）：旧版那句
  // `pkt.stream_index = stream_->index;` 单路时等于"永远是 0"，两路时它会把
  // 音频的包静默当成视频的包写进去，而封装器一声不吭 —— 文件能播、ffprobe
  // 报得出两路都在，只是音轨是空的。越界当场报出来。
  int const si = pkt.stream_index;
  if (si < 0 || static_cast<std::size_t>(si) >= src_time_base_.size()) {
    err = fmt("包带着 stream_index=%d，但这个文件里只有 %zu 路", si,
              src_time_base_.size());
    return false;
  }

  // 源时基要用**开文件头之前**记下来的那一份，而且要用**这一路自己的**。
  // 不能图省事写成 av_packet_rescale_ts(&pkt, st->time_base, st->time_base)：
  // 写文件头时 libavformat 会按封装格式的要求改写 stream->time_base（mov 把
  // 它变成 1/15360 这种），此时"换算到自己"是个空操作，帧号就被当成 15360
  // 分之一秒来解释。
  //
  // 症状很有辨识度：ffprobe 报 nb_frames 正确、分辨率正确，但 duration 和
  // avg_frame_rate 荒谬（300 帧 0.019 秒），文件能播但一闪而过。
  AVRational const src{src_time_base_[si].num, src_time_base_[si].den};
  av_packet_rescale_ts(&pkt, src, fmt_->streams[si]->time_base);

  std::uint64_t const size = static_cast<std::uint64_t>(pkt.size);
  // av_interleaved_write_frame 会拿走包的内容（写完后 pkt 被清空）。
  // 这正是我们要的：编码器那边拿回去的包本来也要 unref。
  int const ret = av_interleaved_write_frame(fmt_, &pkt);
  if (ret < 0) {
    err = fmt("写第 %d 路的包失败：%s", si, av_error(ret).c_str());
    return false;
  }
  ++packets_written_;
  bytes_written_ += size;
  return true;
}

bool Mp4Writer::close(std::string& err) {
  if (!fmt_) return true;
  bool ok = true;

  if (fmt_->pb) {
    int const ret = av_write_trailer(fmt_);
    if (ret < 0) {
      err = "写 MP4 索引失败：" + av_error(ret);
      ok = false;
    }
    if (avio_closep(&fmt_->pb) < 0 && ok) {
      err = "关闭输出文件失败";
      ok = false;
    }
  }

  avformat_free_context(fmt_);
  fmt_ = nullptr;
  src_time_base_.clear();  // stream_count() 因此诚实地回到 0
  return ok;
}

}  // namespace d2o
