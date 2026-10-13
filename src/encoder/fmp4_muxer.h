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
 *     默认配置下分片**只在关键帧处切**（movflags 里的 `frag_keyframe`），所以
 *     "第 N 个 GOP 的分片"是在第 N+1 个关键帧喂进来的那一刻才落到 out 里的。
 *     这正是直播预览那把"真实延迟"的尺子里**多出来的半个 GOP**（最坏一整个）的
 *     来源 —— `TARGET_LATENCY` 那 0.5 秒之外，还有"分片要等下一个关键帧"。
 *     `flush()` 能在我们指定的位置切一刀（**不必是关键帧，这一条已量过** ——
 *     见 `flush()` 那段），那一项于是从"跟着 GOP 走"变成"跟着调用方的
 *     切分节奏走"。**关键帧处仍然会照切**（`frag_keyframe` 一直在 movflags 里），
 *     所以打开之后的分片边界是"调用方的节奏"与"关键帧"两套的并集。
 *     **一次 write() 完全没有输出在两种配置下都是常态**，不是错误信号。
 *   - **最后一个 GOP 要靠 `flush()` 收尾。** 不调的话它一直压在封装器里（mov 要等
 *     "下一个关键帧"才知道上一片在哪结束），下一次 `init()` 就跟着没了。直播流里
 *     确实没有"最后一个 GOP"，但**录制结束**与**预览断开**这两处有。
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
  /// 这个样本被**喂进封装器**那一刻的墙钟（Unix 毫秒）。默认 0 = 调用方没提供
  /// （测试与其它封装器使用者都不需要它，所以它是默认值而不是必填项）。
  ///
  /// 它唯一的作用是把**媒体时间轴**对回**墙钟**：封装器把第一个落到
  /// `av_interleaved_write_frame` 的样本当成 tfdt = 0（见 .cpp 里 write_nal()
  /// 的说明），于是"媒体时间 T 毫秒"对应的墙钟就是 `t0_wall_ms + T`
  /// —— 前提是媒体时间与墙钟 1:1 推进（本项目的采集侧正是如此：PTS 就是拍号）。
  /// 浏览器预览的那把"真实延迟"尺子量的是这个差值，而不是封装器自己的缓冲。
  ///
  /// **必须是墙钟，不是设备时间**：水印用的 `device_now` 是主机时刻加 NVR 下发
  /// 的偏移，拿它去和浏览器那边的 `Date.now()` 相减会平白多出一个偏移量。
  std::int64_t wall_ms = 0;
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
  /// @param fragment_on_flush 在 movflags 里加上 `+frag_custom`。默认 **false**。
  ///
  ///        ⚠️ **这个参数不是"能不能在任意位置切分片"的开关。** 真正切分片的是
  ///        `flush()` 里那句 `av_write_frame(fmt_, nullptr)` —— 量出来的读数与
  ///        推理过程都写在下面 `flush()` 那段注释里（开关取真/假两态：47/47 个
  ///        分片、300/300 个样本，两份输出**逐字节相同**）。这一段原先写着
  ///        "打开 `+frag_custom`，让 `flush()` 能在任意位置切分片"，**那是推出来的，
  ///        不是量出来的，而且推反了**。
  ///
  ///        留着它的两个理由：
  ///        (a) 默认 false，预览之外的使用者（测试、将来别的封装消费者）拿到的
  ///            是"和加这个参数之前逐字节一样"的行为；
  ///        (b) 它是为"主动切分"这件事提供的那条路。这一版 FFmpeg 不需要它，
  ///            而"哪天换一版变成不带它就只在关键帧处切"是一条会**无声**退化的
  ///            路（症状是预览延迟悄悄涨回去，没有一行日志），开着它成本是 0。
  ///
  ///        ⚠️ **别拿"开/关这个参数编两个构建相减"去量延迟收益。** 上面那句
  ///        "逐字节相同"就是这个 A/B 的结论：差恰好是 0，量出来的是噪声。
  ///        要 A/B 的是**调用不调用 `flush()`**，那一处在 `capture_session.cpp`
  ///        的切分条件上，不在这个参数上 —— 探针 ② 量的是它（相邻分片间隔
  ///        222ms vs 把那一行关掉后的 2001ms）。
  bool init(int width, int height, std::vector<std::uint8_t> const& extradata_annexb,
            int fps, bool fragment_on_flush = false);

  /// 初始化段（ftyp+moov）。init() 成功后即可取；**必须先发给浏览器**，
  /// 发晚一个字节 MSE 就会拒绝后面所有分片。
  std::vector<std::uint8_t> const& init_segment() const;

  /// 封装一个访问单元。产生了完整分片时**追加**到 out（不清空 out）。
  /// 返回 false 表示这个样本没能写进去（调用方应记一次错误，但不该崩）。
  bool write(Fmp4Sample const& s, std::vector<std::uint8_t>& out);

  /// 把攒在封装器里的样本立刻切一个分片出来，**追加**到 out（不清空 out）。
  ///
  /// **没有新样本时是空操作**（返回 true、一个字节都不写）。这条闸门放在这里、
  /// 不指望调用方自己数，理由很具体：mov 在强制 flush 之下会照样写出一个
  /// **零样本**的 moof+mdat，而那是 MSE 的 SourceBuffer 会当场拒绝的东西
  /// （`AppendError`）—— "空分片"是这个接口唯一容易犯的错，堵在源头最省事。
  ///
  /// 它**只动分片边界，不产生也不丢任何样本**：切碎之后每个分片照样能被
  /// libavformat 顺序读回、样本一个不少、时间戳不乱 —— 这一条由
  /// `tests/fmp4_check.cpp` 的「切碎不等于切坏」盯着。
  ///
  /// **这一刀切在哪、切了几刀，已经量过了**（`tests/fmp4_check.cpp` 的
  /// 「切碎不等于切坏」，2026-10-08）。读数如下，两个都记下来，因为第二个是
  /// 一个反直觉的结果：
  ///
  ///   1. **不必是关键帧。** 同一段 300 帧的码流、每 7 帧调一次 `flush()`：
  ///      切出 47 个分片、`trun` 的 sample_count 之和恰好 300、每个分片的 tfdt
  ///      严格递增、libavformat 顺序读回解出 300 帧且整段解码零错误输出。
  ///      （47 而不是 43 是因为关键帧处 `frag_keyframe` 仍在切，见文件头。）
  ///   2. ⚠️ **`+frag_custom` 在这件事上没有可观测的差别。** 同一个馈入序列、
  ///      同一个 flush 节奏、`init()` 的 `fragment_on_flush` 取真/假各跑一遍：
  ///      **两态都是 47 个分片 / 300 个样本**，而且**关掉的那一侧也**是 tfdt
  ///      严格递增、解出 300/300 帧、解码干净。
  ///      **再往前一步是逐字节比对**：两个文件的字节**完全相同**（5,139,234 字节，
  ///      测试里就地 `memcmp`，读数是"两态输出逐字节相同"那一行）。这一条比
  ///      "分片数相同"硬得多 —— 后者还能读成"两边都切了 47 刀，只是一边走运写对了"，
  ///      而字节相同没有第二种读法。也就是说这一版 FFmpeg 里
  ///      `av_write_frame(fmt_, nullptr)` 本身就切分片，不是有了 `frag_custom` 才切。
  ///      这一条与随包发布的头文件是一致的：`av_write_frame` 的文档里"`pkt` 可以是
  ///      NULL，用来立即冲刷封装器内部缓冲"**是无条件的**，而 `frag_custom`
  ///      那个选项在发布的头文件里一个字都没有（它只在 movenc 的源码里，那不在
  ///      这棵树里）。
  ///
  ///   所以：**不要**再写下"能切在任意位置是 `+frag_custom` 才有的"这句话，
  ///   它是错的（这个位置原先写着它，是推出来的不是量出来的）。开关仍然保留，
  ///   理由是它就是为这件事提供的那条路，且开与不开的行为在这套组合下一致；
  ///   但**这个功能的成立不依赖它**。
  ///
  ///   两态读数相同这件事**只作报告、不作判据**：它量的是 libavformat 的行为，
  ///   换个 FFmpeg 版本就可能变，而红的原因与我们的代码无关。
  bool flush(std::vector<std::uint8_t>& out);

  bool ready() const { return ready_; }

  /// 媒体时间 0 点对应的**墙钟**（Unix 毫秒）：第一个真正落到封装器里的样本
  /// 带着的那份 `Fmp4Sample::wall_ms`。`has_t0()` 为假时这个值没有意义
  /// （固定是 0）。
  ///
  /// 「落到封装器里」而不是「喂进来」是刻意的：定 0 点的是封装器，不是我们
  /// —— `write_nal()` 那段注释记着这条是量出来的。
  std::int64_t t0_wall_ms() const { return t0_wall_ms_; }
  bool has_t0() const { return have_t0_; }

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

  /// `+frag_custom` 开没开。**只影响 movflags 那个字符串** —— `flush()` 的语义
  /// 与它无关（量过：两态切出来的分片一模一样，见上面 `flush()` 那段）。
  bool fragment_on_flush_ = false;
  /// 上一次分片边界之后**真正落进封装器**的样本数。`flush()` 拿它当闸门。
  ///
  /// 「真正落进去」而不是「喂进来」：`write_nal()` 里写失败那条路不该让这个数
  /// 增长，否则 `flush()` 会被放到一个没有任何样本的封装器上。
  ///
  /// **它不跟着 `write()` 清零**，而是跟着"分片真的写出去了"清零 —— 所以
  /// 清零点在两处：`flush()` 成功之后，以及 `movflags` 自己切的那一刀之后。
  /// 后者我们看不见，只能**从"这次 write() 有输出"推**：有输出就说明封装器
  /// 刚吐了一个分片，攒着的样本已经落盘了。
  std::size_t pending_samples_ = 0;

  /// 媒体时间 0 点对应的墙钟，以及"定过没有"。
  ///
  /// **不能拿 `have_last_` 兼职**：`have_last_` 在写失败时不会被置上（见
  /// write_nal 的结尾），于是封装的第一个包失败之后，下一个包会**再记一次**
  /// t0 —— 而它已经不是 0 点了，报出去的墙钟会平白多出一个帧间隔。
  /// 一个标志只回答一个问题。
  std::int64_t t0_wall_ms_ = 0;
  bool have_t0_ = false;
};

}  // namespace d2o
