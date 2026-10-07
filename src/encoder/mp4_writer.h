#pragma once

/**
 * @file src/encoder/mp4_writer.h
 * @brief 把编码出来的包写成 MP4 文件。
 *
 * Phase 1 只有一个用处：让 `ffprobe` 和 VLC 能独立地验证编码链路的产物 ——
 * 分辨率、帧率、编码格式、以及"画面到底是不是黑的"。用文件而不是直接看内存，
 * 是为了让验证方（包括人）不依赖我们自己的任何代码。
 *
 * 用 libavformat 的 mp4 封装器，不自己写：桌面录制的时间戳处理、avcC 里的
 * SPS/PPS、faststart 的重写索引，这些都不值得重写一遍。
 */

#include <cstdint>
#include <string>

struct AVFormatContext;
struct AVStream;
struct AVCodecContext;
struct AVPacket;

namespace d2o {

class Mp4Writer {
 public:
  Mp4Writer() = default;
  ~Mp4Writer();

  Mp4Writer(Mp4Writer const&) = delete;
  Mp4Writer& operator=(Mp4Writer const&) = delete;

  /// enc 必须在写入期间一直有效 —— 流的参数是从它身上取的。
  ///
  /// faststart=true 会把索引挪到文件开头，边下边播时不用等整个文件。
  /// 代价是收尾时要重写一遍文件（几 MB 的桌面录屏可以忽略）。
  bool open(std::string const& path, AVCodecContext const* enc,
            std::string& err, bool faststart = true);

  /// 写一个包。**会拿走包的所有权**（内容被清空），这是 av_interleaved_write_frame
  /// 的约定。调用方之后不必再 unref，unref 了也无害。
  bool write(AVPacket& pkt, std::string& err);

  /// 收尾并落盘。不调它文件是坏的 —— MP4 的索引在末尾（除非 faststart）。
  bool close(std::string& err);

  std::uint64_t frames_written() const { return frames_written_; }
  std::uint64_t bytes_written() const { return bytes_written_; }

 private:
  /// 时基，就是一对整数。这里不引 <libavutil/rational.h> 的 AVRational：
  /// 那个头要包在 extern "C" 里，会把 FFmpeg 的头文件依赖从 .cpp 漏到头文件，
  /// 而这里需要的只是两个 int。
  struct TimeBase {
    int num = 0;
    int den = 1;
  };

  AVFormatContext* fmt_ = nullptr;
  AVStream* stream_ = nullptr;
  /// 包进来时用的时基（= 编码器的 1/fps）。见 write() 里的说明。
  TimeBase src_time_base_{1, 30};
  std::string path_;
  std::uint64_t frames_written_ = 0;
  std::uint64_t bytes_written_ = 0;
};

}  // namespace d2o
