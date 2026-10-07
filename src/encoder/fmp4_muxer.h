#pragma once

/**
 * @file src/encoder/fmp4_muxer.h
 * @brief 把 H.264 Annex-B 访问单元增量封装成 fragmented MP4（CMAF 风格），
 *        给浏览器用 Media Source Extensions 直接播放。
 *
 * 浏览器那边的用法是固定的，这个类就是照着它长的：
 *
 *   1. init() 之后取 init_segment()（ftyp+moov），**先** append 给 SourceBuffer，
 *      MIME 是 `video/mp4; codecs="avc1.42C033"`；
 *   2. 之后每拿到一个分片（moof+mdat）append 一次，SourceBuffer 自己就会播。
 *
 * 调用方必须记住的三件事（每一条踩过都只表现为"浏览器不出画面"，而不会报错）：
 *
 *   - **一次 write() 不一定产出一个分片，一个分片也不一定来自一次 write()。**
 *     mov 封装器要等下一个关键帧才知道上一个分片在哪结束，所以"第 N 个 GOP 的
 *     分片"是在第 N+1 个关键帧喂进来的那一刻才落到 out 里的。这也意味着**最后
 *     一个 GOP 的分片拿不到** —— 直播流里不存在"最后一个 GOP"，所以这不是缺陷；
 *     但真要收尾得再加一个显式的 flush，本模块没有（接口里没有这个位置）。
 *   - **init 段不能晚发，也不能和分片混着发。** MSE 只认第一个 append 进去的
 *     init 段，晚一个字节后面所有分片都会被拒，报错还指不到真正的原因。
 *   - **时间戳只要求相对单调，不要求从 0 开始。** 内部以第一个样本的 dts 为
 *     0 点，传绝对时钟（比如 QueryPerformanceCounter）进来也不会把 tfdt 撑爆。
 *     代价是同一个 muxer 里不能混两次会话的样本。
 *
 * 为什么不自己写 box：moof/trun/tfdt/tfhd 的字段顺序、sample flags、
 * data_offset 的基准（default-base-is-moof）、avcC 里 SPS/PPS 的封装与 NALU 的
 * 长度前缀化 —— 每一项都能写错，而写错之后文件"看起来是好的"（ffprobe 还能
 * 报出正确的分辨率帧率），只有浏览器安静地不播。libavformat 这套已经对了十几年，
 * 没有理由再来一遍。
 */

#include <cstddef>
#include <cstdint>
#include <vector>

struct AVFormatContext;
struct AVStream;
struct AVIOContext;
struct AVCodecContext;
struct AVCodecParserContext;
struct AVPacket;

namespace d2o {

/// 一个待封装的访问单元。data 指向 **Annex-B**（带起始码）的一段完整访问单元。
struct Fmp4Sample {
  std::uint8_t const* data = nullptr;
  std::size_t size = 0;
  std::int64_t pts_us = 0;   ///< 微秒
  std::int64_t dts_us = 0;   ///< 微秒
  bool keyframe = false;
};

class Fmp4Muxer {
 public:
  Fmp4Muxer();
  ~Fmp4Muxer();
  Fmp4Muxer(Fmp4Muxer const&) = delete;
  Fmp4Muxer& operator=(Fmp4Muxer const&) = delete;

  /// @param extradata_annexb 编码器的 extradata（Annex-B，含 SPS/PPS，可选带 AUD）。
  /// @param fps 标称帧率。它**不进时基**（时基固定用微秒，理由见 .cpp 里 kMicro
  ///        那段），只用来给第一个样本凑一个合理的时长 —— 其余样本的时长都是
  ///        相邻 dts 之差，只有第一个没有"上一个"可以减。
  bool init(int width, int height, std::vector<std::uint8_t> const& extradata_annexb, int fps);

  /// 初始化段（ftyp+moov）。init() 成功后即可取；**必须先发给浏览器**，
  /// 发晚一个字节 MSE 就会拒绝后面所有分片。
  std::vector<std::uint8_t> const& init_segment() const;

  /// 封装一个访问单元。产生了完整分片时**追加**到 out（不清空 out）。
  /// 返回 false 表示这个样本没能写进去（调用方应记一次错误，但不该崩）。
  bool write(Fmp4Sample const& s, std::vector<std::uint8_t>& out);

  bool ready() const { return ready_; }

 private:
  /// AVIOContext 的私有数据。定义在这里而不是 .cpp 的匿名命名空间里，是因为
  /// 回调（下面两个静态成员）要能碰到它；写成 POD、只含两个平凡成员，
  /// 是为了让头文件不至于把 <vector> 之外的东西再拖进来。
  struct IoState {
    std::vector<std::uint8_t>* buf = nullptr;
    std::size_t pos = 0;
  };

  /// 自定义 AVIOContext 的写回调：把数据落到 buffer_ 里。
  /// 声明成静态成员（而不是匿名命名空间里的自由函数）只为了一件事：能访问私有的
  /// IoState。
  static int io_write(void* opaque, std::uint8_t const* data, int size);
  /// 自定义 AVIOContext 的 seek 回调。底层是 vector，本来就可寻址，所以支持。
  static std::int64_t io_seek(void* opaque, std::int64_t offset, int whence);

  /// 释放全部 FFmpeg 资源并回到未初始化状态。init() 重入前和析构时都要走一遍。
  void teardown();

  /// 把一段（解析器吐出来的）Annex-B 数据当成一个样本写进封装器。
  bool write_nal(std::uint8_t const* data, int size, Fmp4Sample const& s);

  AVFormatContext* fmt_ = nullptr;
  /// 指向 fmt_ 内部的视频流，只借用不拥有。
  AVStream* stream_ = nullptr;
  AVCodecParserContext* parser_ = nullptr;
  /// 解析器专用的上下文，故意是"空"的 —— 见 init() 里的说明。
  AVCodecContext* parser_ctx_ = nullptr;
  /// 复用同一个 AVPacket，避免每帧一次 malloc。
  AVPacket* pkt_ = nullptr;

  /// write() 每次调用前清空、调用后整块交出去的字节。
  /// 不直接往调用方的 vector 里写是因为 avio 要能 seek 回来改写（见 .cpp）。
  std::vector<std::uint8_t> buffer_;
  std::vector<std::uint8_t> init_segment_;
  IoState io_state_;

  bool ready_ = false;
  /// 第一个样本的时长（流时基）。见 init() 里 fps 的用法。
  std::int64_t first_duration_ = 0;
  /// 第一个样本的 dts（微秒），作为整个会话的 0 点。
  std::int64_t base_dts_us_ = 0;
  bool have_base_ = false;
  /// 上一个样本在**流时基**下的 dts，用来保证严格递增。
  std::int64_t last_dts_ = 0;
  bool have_last_ = false;
};

}  // namespace d2o
