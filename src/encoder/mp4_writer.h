#pragma once

/**
 * @file src/encoder/mp4_writer.h
 * @brief 把编码出来的包写成 MP4 文件（一到两路：视频，可选音频）。
 *
 * Phase 1 只有一个用处：让 `ffprobe` 和 VLC 能独立地验证编码链路的产物 ——
 * 分辨率、帧率、编码格式、以及"画面到底是不是黑的"。用文件而不是直接写内存，
 * 是为了让验证方（包括人）不依赖我们自己的任何代码。
 *
 * 用 libavformat 的 mp4 封装器，不自己写：桌面录制的时间戳处理、avcC 里的
 * SPS/PPS、faststart 的重写索引，这些都不值得重写一遍。
 *
 * ## 从单路变两路时踩的两条（都是"不报错但结果全错"那一类）
 *
 * **一、源时基必须按流各存一份**（陷阱 75）。`avformat_write_header` 会按封装
 * 格式的要求**改写** `stream->time_base`（mov 把它换成 1/15360 这种），所以
 * "包进来时用的是哪个时基"只能在写文件头**之前**记下来 —— 而且**每一路各记
 * 一份**。拿视频那份去 rescale 音频的包，两路各自的 `ffprobe` 输出看着都正常，
 * 只有时间轴错位。这正是陷阱 19 在第二路上的原样复现。
 *
 * **二、`initial_padding` 要显式搬**（陷阱 74）。`avcodec_parameters_from_context`
 * **不搬运**这个字段，而 mov 的 edit list 语义上要靠它，所以这里显式搬一次。
 *
 * ⚠️ **但别以为搬不搬能在这条链路上量出来**（`tests/mp4_dual_check.cpp` 实测）：
 * 把下面那句注释掉、重编，两个 mp4 **逐字节相同**，读回来的
 * `codecpar->initial_padding` 也都是 1024。原因是 FFmpeg 的 aac 编码器把首包
 * pts 整体前移 `-initial_padding`（实测首包 dts = −1024），而 mov 的 muxer 是从
 * **那个负 dts** 推的 edit list。所以这一句是"按契约写对"而不是"修一个能看见的
 * 毛病"—— 它防的是"某个编码器设了 `initial_padding` 却不挪时间戳"那一天。
 * 相应的，测试**不判这个字段**（一条怎么都绿的断言等于没验），改判首包 pts 为负。
 *
 * ## 流编号是契约
 *
 * **0 = 视频，1 = 音频**，由 `open()` 按这个顺序创建，调用方按这个编号填
 * `pkt.stream_index`。`write()` **不再改写**这个字段（旧版那句
 * `pkt.stream_index = stream_->index;` 单路时等于"永远是 0"，两路时它会把音频
 * 的包静默当成视频的包写进去，而封装器一声不吭）。
 */

#include <cstdint>
#include <string>
#include <vector>

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

  /// 两个编码上下文都必须在写入期间一直有效 —— 流的参数是从它们身上取的。
  ///
  /// `aenc` 为 **nullptr 表示纯视频**：这时**不会**建音频流。音频关掉时
  /// 不要建一条空音轨 —— "有音轨但是静音"和"没有音轨"对 NVR 与播放器是两回事
  /// （陷阱 78）。`venc` 不能为空。
  ///
  /// 建的流按固定顺序编号：**0 = 视频，1 = 音频**。
  ///
  /// faststart=true 会把索引挪到文件开头，边下边播时不用等整个文件。
  /// 代价是收尾时要重写一遍文件（几 MB 的桌面录屏可以忽略）。
  bool open(std::string const& path, AVCodecContext const* venc,
            AVCodecContext const* aenc, std::string& err, bool faststart = true);

  /// 写一个包。**会拿走包的所有权**（内容被清空），这是 av_interleaved_write_frame
  /// 的约定。调用方之后不必再 unref，unref 了也无害。
  ///
  /// `pkt.stream_index` 由**调用方**填（0 = 视频，1 = 音频），这里只认它 ——
  /// 越界会当场失败而不是静默写错路。
  bool write(AVPacket& pkt, std::string& err);

  /// 收尾并落盘。不调它文件是坏的 —— MP4 的索引在末尾（除非 faststart）。
  bool close(std::string& err);

  /// 两路合起来的包数与字节数。**没有分路的计数** —— 现在没有任何地方读它们，
  /// 等真需要时再加；先加两个没人用的访问器只会让它们慢慢和现实脱节。
  std::uint64_t packets_written() const { return packets_written_; }
  std::uint64_t bytes_written() const { return bytes_written_; }

  /// 已经建了几路（0 表示没打开）。诊断与断言用。
  std::size_t stream_count() const { return src_time_base_.size(); }

 private:
  /// 建一路：拷参数、搬 `initial_padding`、记下**写文件头之前**的源时基。
  bool add_stream(AVCodecContext const* enc, std::string& err);

  /// 时基，就是一对整数。这里不引 <libavutil/rational.h> 的 AVRational：
  /// 那个头要包在 extern "C" 里，会把 FFmpeg 的头文件依赖从 .cpp 漏到头文件，
  /// 而这里需要的只是两个 int。
  struct TimeBase {
    int num = 0;
    int den = 1;
  };

  AVFormatContext* fmt_ = nullptr;
  /// **按流各一份**，下标 = `pkt.stream_index`。见文件头第一条。
  std::vector<TimeBase> src_time_base_;
  std::string path_;
  std::uint64_t packets_written_ = 0;
  std::uint64_t bytes_written_ = 0;
};

}  // namespace d2o
