#pragma once

/**
 * @file src/encoder/aac_encoder.h
 * @brief AAC 音频编码（FFmpeg 原生 `aac` 编码器）。
 *
 * 选原生 aac 而不是 libfdk_aac：gyan.dev 的 full_build 里没有 fdk（许可证原因），
 * 而原生那个在 64~128 kbps 这一档对监控用途完全够用。
 *
 * ## 三个不显眼但会咬人的地方
 *
 * **一、`AV_CODEC_FLAG_GLOBAL_HEADER` 是必须的，不是可选的。**
 * RTSP 的 SDP 里那句 `a=fmtp:97 config=<hex>` 要的是 **AudioSpecificConfig**，
 * 而它只在 GLOBAL_HEADER 打开时才进 `extradata`。不开的话 `extradata` 是空的，
 * SDP 里就没有 `config=` —— 而**收端照样能连上、`ffprobe` 照样报得出 48000 Hz
 * 立体声 AAC**，只是解出来是噪音。所以这里无条件打开，不暴露成选项。
 *
 * ⚠️ **而 `extradata` 不是 2 字节，是 5 字节**（实测 `11 90 56 E5 00`）。
 * 前 2 字节 `11 90` 才是 AudioSpecificConfig 本身（对象类型 2 = AAC-LC、
 * 采样率索引 3 = 48000、声道配置 2 = 立体声）；后面 `56 E5 00` 是 ISO 14496-3
 * 的 **sync extension**（11 位同步字 `0x2B7` + extensionAudioObjectType 5 = SBR +
 * `sbrPresentFlag = 0` + `psPresentFlag = 0`），FFmpeg 的 `aac_encode_init`
 * **无条件**写它，表示"没有 SBR"。所以两个消费者要的东西**不一样**：
 *   - MP4 的 `codecpar->extradata` 要**全部 5 字节**（那是 esds 里的 DecoderSpecificInfo）；
 *   - SDP 的 `config=` 要**前 2 字节**（那才是 AudioSpecificConfig，真实摄像机
 *     放进去的就是这个长度）。
 * 所以这里**不提供**一个含糊的「extradata 的十六进制」，而是分成
 * `extradata()`（原样）与 `asc()` / `asc_hex()`（前 2 字节）两个名字 ——
 * 一个叫 `..._hex()` 却在偷偷截断的函数，正是将来有人拿它去填 codecpar 的坑。
 *
 * **二、输出是裸 AU，`ADTS` 头一个字节都不要加。** 见陷阱 76：Live555 的
 * `ADTSAudioStreamDiscreteFramer` 干的是**反事** —— 它给裸帧**加** ADTS 头。
 * 而 FFmpeg 的 aac 编码器吐出来的本来就没有 ADTS 头（那是 `adts` 封装器的活）。
 * 再套一层等于每个包多一个假头，收端解出来是噪音，**而 SDP 完全正常**。
 * 所以 Live555 那边要接 `MPEG4GenericRTPSink`，不是 `ADTSAudioStreamDiscreteFramer`。
 *
 * **三、`initial_padding` 必须搬到 `Mp4Writer` 的 `codecpar` 上。**
 * AAC 编码器有 1024 样本（21 ms）的启动填充，`ffmpeg` 已实测会把**输出包的时间戳**
 * 整体前移 `-initial_padding`（`avcodec.h` 上 `initial_padding` 那一段的原文），
 * 也就是第一个包的 pts 是 **-1024** 而不是 0。mov 要靠
 * `codecpar->initial_padding` 写 edit list 才对得齐，而
 * **`avcodec_parameters_from_context` 不搬运这个字段**（陷阱 74）。
 * 不搬的话音画恒定差 21 ms —— 肉耳一般听不出来，但"音轨比视频短 21 ms"
 * 这类断言会一直打架。
 *
 * ## 输入格式
 *
 * **交错 float32**，和 `AudioMixer::fill()` 的输出是同一个格式 —— 上游只认一种
 * 格式，去交错（→ FLTP）在这一层内部做掉。采样率 48000、声道 2 是混音器唯一会
 * 给的组合，但这一层不假设它。
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVAudioFifo;

namespace d2o {

/// 和 `video_encoder.h` 里那个**故意是同一个类型**（别名重声明到同一类型是合法的）。
/// 不 include 那个头是为了让两个编码器互不依赖：链 aac_check 时不必把
/// `capture/frame.h`（连带 D3D 那一堆）拖进来。
using PacketSink = std::function<void(AVPacket& pkt)>;

struct AacConfig {
  int sample_rate = 48000;
  int channels = 2;
  int bitrate_kbps = 64;
};

class AacEncoder {
 public:
  AacEncoder() = default;
  ~AacEncoder();

  AacEncoder(AacEncoder const&) = delete;
  AacEncoder& operator=(AacEncoder const&) = delete;

  bool open(AacConfig const& cfg, std::string& err);
  void close();
  bool is_open() const { return ctx_ != nullptr; }

  /// 喂 `frames` 个**交错** float32 样本，产出 0..n 个包。
  ///
  /// **调用节奏与输出包数无关**：不足一帧（1024 样本）的先攒在 FIFO 里。
  /// 25 fps 下一拍是 1920 样本，所以有时出 1 包、有时出 2 包 —— 调用方按拍喂，
  /// 不要按包数反推时间轴（音频的时间轴归 `AudioMixer::fill`，见那里的说明）。
  bool encode(float const* interleaved, int frames, PacketSink const& sink,
              std::string& err);

  /// 冲刷。**FIFO 里不足一帧的尾巴会补零凑满一帧编出去** —— 不补的话最后
  /// 不到 21 ms 会被静默丢掉，而它恰好在音轨结尾（"音轨比视频短一截"）。
  bool flush(PacketSink const& sink, std::string& err);

  /// 给封装器读流参数用（`Mp4Writer` 从它身上取 codecpar）。
  AVCodecContext const* codec_context() const { return ctx_; }

  /// 编码器给的**全部** extradata 字节。实测 5 字节，见文件头：MP4 的
  /// `codecpar->extradata` 要的是这个（esds 里的 DecoderSpecificInfo）。
  std::vector<std::uint8_t> const& extradata() const { return extradata_; }

  /// 前 2 字节 —— 那才是严格意义上的 **AudioSpecificConfig**，SDP 的
  /// `a=fmtp:<pt> config=` 要的就是它。extradata 不足 2 字节时返回空。
  std::vector<std::uint8_t> asc() const;

  /// `asc()` 的十六进制（大写 —— RFC 3640 里十六进制不分大小写）。
  /// 实测 `"1190"`：AAC-LC / 48000 Hz / 立体声。
  std::string asc_hex() const;

  /// **不打开编码器**就能算出来的那份 AudioSpecificConfig 十六进制。
  ///
  /// 为什么需要它：RTSP 服务器现在由 `cmd_run` 在**任何采集会话起来之前**起
  /// （一台服务器服务全部通道），而 `ServerMediaSession` 是在那一刻一次性建好
  /// 的 —— 音频 subsession 要挂上去就必须在那时知道 ASC，可 AAC 编码器那时还
  /// 不存在（它的 extradata 要 `avcodec_open2` 之后才有）。
  ///
  /// ASC 只由三个数决定：object type（AAC-LC）、采样率索引、声道配置。而这三
  /// 个数在这条链路里是**常量** —— 采集层把每一路都统一成 48 kHz 立体声
  /// （见 `AudioMixer::Options` 的说明），`AacConfig` 里也没有这两项。
  /// 于是「预测」与「真值」必然一致，而**话不能只说一半**：
  /// `asc_hex()` 仍然是权威，会话起来之后 `capture_session` 会拿真值核对一次，
  /// 不一致就 WARN（那正是"连得上、能报格式、解出来是噪音"的先兆）。
  ///
  /// 采样率不在表里（或者声道 > 2 的奇怪组合）时返回**空串** —— 调用方据此
  /// 宁可不开音频，也不要报一个错的 config。
  static std::string asc_hex_for(int sample_rate, int channels);

  int frame_size() const { return frame_size_; }
  /// 编码器报的启动填充（实测 1024 样本）。**要搬到 codecpar 上**，见文件头。
  int initial_padding() const { return initial_padding_; }

  int sample_rate() const;
  int channels() const;

  std::uint64_t packets_out() const { return packets_out_; }
  std::uint64_t bytes_out() const { return bytes_out_; }
  /// 喂进去的样本总帧数（不含 flush 时补的零）。
  std::uint64_t frames_in() const { return frames_in_; }

 private:
  bool receive_all(PacketSink const& sink, std::string& err);
  /// 攒够一帧就编，直到 FIFO 里不足一帧为止。
  bool drain_fifo(PacketSink const& sink, std::string& err);

  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVAudioFifo* fifo_ = nullptr;

  AacConfig cfg_{};
  int frame_size_ = 0;
  int initial_padding_ = 0;
  std::int64_t next_pts_ = 0;   ///< 以**样本**为单位，时基 1/sample_rate
  /// 去交错用的暂存（平面布局：先 L 的 n 个，再 R 的 n 个）。
  std::vector<float> planar_;
  std::vector<std::uint8_t> extradata_;

  std::uint64_t packets_out_ = 0;
  std::uint64_t bytes_out_ = 0;
  std::uint64_t frames_in_ = 0;
};

}  // namespace d2o
