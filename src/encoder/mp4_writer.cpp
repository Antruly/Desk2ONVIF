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

bool Mp4Writer::open(std::string const& path, AVCodecContext const* enc,
                     std::string& err, bool faststart) {
  path_ = path;

  int ret = avformat_alloc_output_context2(&fmt_, nullptr, "mp4", path.c_str());
  if (ret < 0 || !fmt_) {
    err = "创建 MP4 封装上下文失败：" + av_error(ret);
    return false;
  }

  stream_ = avformat_new_stream(fmt_, nullptr);
  if (!stream_) {
    err = "avformat_new_stream 失败";
    abandon(fmt_);
    return false;
  }

  ret = avcodec_parameters_from_context(stream_->codecpar, enc);
  if (ret < 0) {
    err = "复制编码参数到流失败：" + av_error(ret);
    abandon(fmt_);
    stream_ = nullptr;
    return false;
  }
  // 时基跟随编码器（1/fps）。不自己换算成 mov 惯用的 1/90000：让 libavformat
  // 去算，我们只需要记住"进来的包用的是哪个时基"。
  //
  // 这一句和下面 avformat_write_header 之后**必须**留存 src_time_base_ 是有
  // 原因的，见 write() 里的说明。
  stream_->time_base = enc->time_base;
  src_time_base_ = TimeBase{enc->time_base.num, enc->time_base.den};

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
    stream_ = nullptr;
    return false;
  }

  ret = avformat_write_header(fmt_, nullptr);
  if (ret < 0) {
    err = "写 MP4 文件头失败：" + av_error(ret);
    abandon(fmt_);
    stream_ = nullptr;
    return false;
  }

  D2O_INFO("输出文件已就绪：%s", path.c_str());
  return true;
}

bool Mp4Writer::write(AVPacket& pkt, std::string& err) {
  if (!fmt_ || !stream_) {
    err = "MP4 封装器未打开";
    return false;
  }
  pkt.stream_index = stream_->index;

  // 源时基要用**开文件头之前**记下来的那个（src_time_base_ = 1/fps），
  // 不能图省事写成 av_packet_rescale_ts(&pkt, stream_->time_base,
  // stream_->time_base)。写入文件头时 libavformat 会按封装格式的要求改写
  // stream_->time_base（mov 会把它变成 1/15360 这种），此时"换算到自己"
  // 是个空操作，帧号就被当成 15360 分之一秒来解释。
  //
  // 症状很有辨识度：ffprobe 报 nb_frames 正确、分辨率正确，但 duration 和
  // avg_frame_rate 荒谬（300 帧 0.019 秒），文件能播但一闪而过。
  AVRational const src{src_time_base_.num, src_time_base_.den};
  av_packet_rescale_ts(&pkt, src, stream_->time_base);

  std::uint64_t const size = static_cast<std::uint64_t>(pkt.size);
  // av_interleaved_write_frame 会拿走包的内容（写完后 pkt 被清空）。
  // 这正是我们要的：编码器那边拿回去的包本来也要 unref。
  int const ret = av_interleaved_write_frame(fmt_, &pkt);
  if (ret < 0) {
    err = "写视频包失败：" + av_error(ret);
    return false;
  }
  ++frames_written_;
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
  stream_ = nullptr;
  return ok;
}

}  // namespace d2o
