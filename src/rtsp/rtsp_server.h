#pragma once

/**
 * @file src/rtsp/rtsp_server.h
 * @brief Live555 RTSP 服务器 + 自己的调度线程。
 *
 * 线程模型：
 *
 *   [编码线程]  push_annex_b()   —— 切分 NALU、插参数集、入队、唤醒
 *        │  NaluQueue（有界，drop-oldest）
 *        ▼
 *   [Live555 调度线程]  BasicTaskScheduler::doEventLoop()
 *        H264LiveSource::doGetNextFrame()  ← 客户端拉的时候才被调用
 *        EventTrigger 唤醒 ← 编码线程唯一能碰的东西
 *
 * 音频（可选）是**并列的第二条**同样形状的路径，不是上面那条的一部分：
 *
 *   [编码线程]  push_audio_au()  —— 一个裸 AU 原样入队、唤醒
 *        │  AuQueue（有界，drop-oldest）
 *        ▼
 *   [Live555 调度线程]  AacLiveSource::doGetNextFrame() → MPEG4GenericRTPSink
 *
 * 两条路径共用调度器、**各自一个队列和一个 armed 门闩**，唯一的交叉点是音频的
 * subsession 挂在主码流的 ServerMediaSession 上（见 RtspAudioOptions）。
 *
 * **停机顺序是有语义的**：必须先让编码线程停掉并 join，再调 stop()。
 * Live555 的对象全是单线程的，stop() 会在调用方线程上析构
 * RTSPServer / ServerMediaSession / H264LiveSource，这时候编码线程要是还在
 * push，就是往一个正在析构的对象上打 triggerEvent。CaptureSession 里
 * 保证了这一点（先 join 再 stop）—— 改那边的时候要留意。
 */

#include "rtsp/nalu.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class UsageEnvironment;
class TaskScheduler;
class RTSPServer;
class ServerMediaSession;

namespace d2o {

class NaluQueue;
class H264LiveSource;

/// 一路码流的参数。
///
/// 主码流和子码流共用这一个结构：除了路径、码率和队列深度，两者没有任何区别
/// —— 编出它们的是两个各自独立的编码器，推给客户端的是两条各自独立的
/// ServerMediaSession。刻意**不做**成"主码流 + 可选子码流"两个不同的形状，
/// 是因为那样每处代码都要分叉成"主怎么走、子怎么走";做成数组之后，
/// 加第三路（比如给手机看的第三档）不需要动任何一处逻辑。
struct RtspStreamOptions {
  std::string path = "live";
  unsigned fps = 30;
  /// 进 SDP 的带宽估计（`b=AS:`）和 Live555 的 estBitrate。**不影响编码**
  /// —— 实际码率由编码器决定，这个值只用来告诉客户端"大概要多少带宽"。
  int bitrate_kbps = 4000;
  /// 队列深度（按 NALU 计）。90 个约等于 3 秒 @30fps —— 一帧一个 VCL NALU，
  /// 加上每个关键帧前插的两个参数集。
  std::size_t queue_capacity = 90;
};

/// 音频那一路的参数。
///
/// **它不是 `streams` 里的第三项，而是单独一条 source/sink/queue 路径**
/// （陷阱 77）。区别是硬的：`streams` 里每一项都是一个独立的
/// `ServerMediaSession`（各自一个 URL），而音频必须和视频待在**同一个会话**里
/// —— NVR 拉一个 URL，期望音频和视频都在那一个会话里。分两个 session 的话它
/// 只看到视频，**而且不会报错**（它压根没要音频）。
///
/// 于是音频的 sink 挂在**主码流**（`streams[0]`）那个 ServerMediaSession 上，
/// 作为它的第二个 subsession。动态载荷的编号是"会话内第几个 subsession"
/// 决定的（`96 + trackNumber() - 1`），所以音频自动拿到 **97**。
struct RtspAudioOptions {
  /// 默认**关闭**（陷阱 79）。打开它会改变每一台设备的 SDP，而真实 NVR 的
  /// 兼容性只能靠实测；开了之后 NVR 不接受的症状是"加了音频就没画面"，
  /// 而归因会指向编码器。这个决定不该替用户做。
  bool enabled = false;
  unsigned sample_rate = 48000;
  unsigned channels = 2;
  /// 只进 SDP 的 `b=AS:` 与 Live555 的 estBitrate，**不影响编码**
  /// （真正的码率由 AacEncoder 决定）。
  int bitrate_kbps = 64;
  /// AudioSpecificConfig 的十六进制 —— `AacEncoder::asc_hex()`，实测 `"1190"`
  /// （AAC-LC / 48000 / 立体声）。
  ///
  /// ⚠️ **不是** extradata 的全部 5 字节（那多出来的 3 字节是 FFmpeg 无条件
  /// 写的"没有 SBR"sync extension），**更不是** ADTS 头。见 aac_encoder.h。
  std::string config_hex;
  /// 一个 AU 里的样本数（`AacEncoder::frame_size()`，实测 1024）。只用来算
  /// RTP 的发送节奏。
  unsigned samples_per_au = 1024;
  /// 队列深度（按 AU 计）。一个 AU = 1024 样本 = 21.3 ms，所以 96 个约 2 秒。
  std::size_t queue_capacity = 96;
};

struct RtspOptions {
  bool enabled = false;
  std::uint16_t port = 554;
  /// **至少一路，而且顺序即语义**：`streams[i]` 对应编码器 i，
  /// `push_annex_b(i, …)` 就是按这个下标找路的。主码流在最前，子码流在后。
  ///
  /// 空数组 + enabled=true 是调用方的记账错误（编码器不知道往哪推），
  /// `start()` 会直接判失败而不是宽容地什么都不做 —— 后者表现为
  /// "NVR 连得上、没有画面"，是最难查的一类症状。
  std::vector<RtspStreamOptions> streams;

  /// 音频这一路。它**不在 `streams` 里**，理由见 `RtspAudioOptions` 上面那段。
  RtspAudioOptions audio;
};

class RtspServer {
 public:
  /// **构造函数不能在类里写 `= default`。** 类里 default 的那个版本会当场
  /// 计算异常规格，于是要实例化每个成员的析构函数 —— 而 `streams_` 里的
  /// `Stream` 在这里还是不完整类型（它定义在 .cpp 里，那样才不用把
  /// ServerMediaSession / NaluQueue 的头文件拖进这个头）。GCC 报的是一大串
  /// `unique_ptr.h` 里的 `sizeof` 不完整类型，看不出和构造函数有关。
  /// 挪到 .cpp 里定义，那里 `Stream` 已经完整了。
  RtspServer();
  ~RtspServer();

  RtspServer(RtspServer const&) = delete;
  RtspServer& operator=(RtspServer const&) = delete;

  /// 起服务器：按 `opt.streams` 把**每一条流的 ServerMediaSession 一次建好**，
  /// 但**不等 SPS/PPS**。
  ///
  /// ## 为什么参数集是后填的（多通道逼出来的形状）
  ///
  /// 一路会话从前是这么起的：`CaptureSession::run()` 打开编码器 → 拿到
  /// extradata → 才 `start()`。多通道下这个顺序不成立了 —— 现在是**一台服务器
  /// 服务 N 个通道**，而通道是各自异步起来的（某个通道的显示器没插、某个摄像头
  /// 被占用，都不该让别的通道的 RTSP 起不来）。
  ///
  /// 所以：`start()` 只建会话，**SPS/PPS 留一个可后填的槽**
  /// （`set_stream_extradata()`），由**首次 DESCRIBE 构造 sink 的那一刻**读。
  ///
  /// 本阶段**风险最高的一条是运行期 `addServerMediaSession()`** —— 它会往
  /// `streams_` 里加元素，而 Live555 的 subsession 持有 `&st.clients` /
  /// `&st.source` 的地址（`streams_` 里装的是 `unique_ptr`，地址本身稳定，
  /// 但"服务器跑起来之后还能改注册表"这条路上有 Live555 自己的锁语义要照顾）。
  /// 用"一次建好 + 后填槽位"就完全绕开了它：**注册表在 start() 之后一次都不改**。
  bool start(RtspOptions const& opt, std::string& err);

  /// 老形状：起服务器并立刻把每路的 extradata 填进去。给"所有编码器都已经
  /// 开好了"的调用方（测试、单通道工具）用，等价于 start() + N 次
  /// set_stream_extradata()，只是失败时会把服务器收掉，语义和从前一样。
  bool start(RtspOptions const& opt,
             std::vector<std::vector<std::uint8_t>> const& extradata,
             std::string& err);

  /// 给第 `stream` 路填上参数集（Annex-B，含起始码的 SPS+PPS，就是编码器的
  /// `extradata()`）。
  ///
  /// **可以早于 start() 调**（那时只存进槽里），也可以在跑的过程中调任意多次
  /// —— 通道中途换了分辨率、编码器重建，就再填一次。填完之后后续的
  /// DESCRIBE/SETUP 用的是**新的**那份。
  ///
  /// 下标越界、或者 anneb 里没有 SPS/PPS，返回 false 并把理由写进 err。
  bool set_stream_extradata(std::size_t stream,
                            std::vector<std::uint8_t> const& annexb,
                            std::string& err);

  /// 某一路的参数集**填上了没有**。多通道的启动日志和 `/api/status` 要用它回答
  /// "这个通道的 NVR 现在拉得到画面吗" —— 没填上时 SDP 里没有
  /// `sprop-parameter-sets`，很多 NVR 加进来就是黑屏（见 `push_annex_b` 的说明：
  /// 带内参数集会在下一个 IDR 前补发，所以它不是致命的，但值得报出来）。
  bool stream_params_ready(std::size_t stream) const;

  /// 见文件头的停机顺序说明：调用前编码线程必须已经停了。
  void stop();

  bool running() const { return running_.load(std::memory_order_relaxed); }

  /// 起了几路。start() 之后 = opt.streams.size()。
  std::size_t stream_count() const { return streams_.size(); }

  /// **编码线程**调用。data 是一个 AVPacket 的内容（Annex-B，带起始码）。
  /// 关键帧会在 IDR 前自动补发 SPS/PPS。
  ///
  /// 下标越界是**静默忽略**（不崩、不报错）：这条路在编码线程上，而它唯一的
  /// 失败方式是调用方把编码器和流的对应关系搞错了 —— 那种错在启动时就该
  /// 被发现，不该等到录到一半从编码线程上去抛。
  void push_annex_b(std::size_t stream, std::uint8_t const* data,
                    std::size_t size, bool keyframe, std::int64_t pts);

  /// **编码线程**调用：一个**裸 AU**（`AacEncoder` 吐出的 AVPacket 的全部内容，
  /// 一包就是一个 AU，**不带 ADTS 头**）。`pts` 的单位是样本。
  ///
  /// 音频没配、服务器没跑、没人在看（音频 subsession 没被 SETUP）时**静默忽略**
  /// —— 和 push_annex_b 同一个道理：这是编码线程上的调用，唯一的失败方式是调用方
  /// 接错了，而那种错在启动时就该被发现。
  void push_audio_au(std::uint8_t const* data, std::size_t size,
                     std::int64_t pts);

  /// 实际可用的播流地址（含 Live555 自己挑的本机 IP 和真实端口）。
  std::string url(std::size_t stream) const;

  /// 某一路的观众数。
  unsigned client_count(std::size_t stream) const;
  /// 所有路加起来的观众数。
  unsigned client_count() const;
  std::size_t queue_capacity(std::size_t stream) const;
  std::size_t queue_size(std::size_t stream) const;
  std::uint64_t nalu_dropped(std::size_t stream) const;
  /// 累计入队 NALU 数。和 nalu_dropped() 一起看才能分清"产出过快"和"没人消费"。
  std::uint64_t nalu_pushed(std::size_t stream) const;
  std::size_t max_nalu_bytes(std::size_t stream) const;
  std::size_t sps_size(std::size_t stream) const;
  std::size_t pps_size(std::size_t stream) const;

  // ---- 音频那一路的诊断。没开音频时全是 0 / false。 ----

  /// 音频 subsession 建起来了没有。**只反映"这条 subsession 存在"**，
  /// 不是"编码器在推音频" —— 后者要靠 config 里的开关和 stats 判断。
  bool audio_enabled() const { return audio_ != nullptr; }
  /// 音频实际用的 payload type。它是**会话内 subsession 的序号**算出来的
  /// （`96 + trackNumber() - 1`，OnDemandServerMediaSubsession 的两处都是这个
  /// 公式），音频是主码流会话里的第二个 ⇒ 97。没开音频时是 0。
  unsigned audio_payload_type() const { return audio_payload_type_; }
  /// **SDP 里报出去的那份** AudioSpecificConfig（十六进制）。空串 = 没开音频。
  ///
  /// 给采集会话对账用：这一份是 `cmd_run` 在 AAC 编码器还没打开时**预测**出来
  /// 的（见 `AacEncoder::asc_hex_for`），而编码器起来之后手上才有一份真值。
  /// 两个不一致时，客户端照样连得上、照样报得出采样率与声道，**只是解出来是
  /// 噪音** —— 所以那一下必须有人比。
  std::string audio_config_hex() const;
  unsigned audio_client_count() const;
  std::size_t audio_queue_capacity() const;
  std::size_t audio_queue_size() const;
  std::uint64_t audio_au_dropped() const;
  std::uint64_t audio_au_pushed() const;
  std::size_t audio_max_au_bytes() const;

 private:
  /// 一路码流的全部状态。定义在 .cpp 里 —— 它要碰 ServerMediaSession 和
  /// NaluQueue，而那些头文件没有理由出现在这里。
  struct Stream;
  /// 音频那一路的状态。同样是**单数**，所以不是 `Stream` 的一种。
  struct AudioTrack;

  void teardown();
  /// 越界返回 nullptr。所有按路取数的接口都过它，于是"下标写错"这件事
  /// 只有一处需要处理。const 与非 const 两个重载的差别就是"要不要往里写"。
  Stream* at(std::size_t stream);
  Stream const* at(std::size_t stream) const;

  RtspOptions opt_{};
  std::vector<std::unique_ptr<Stream>> streams_;
  /// 音频这一路。null 表示没开（陷阱 78：那时**一条 subsession 都不建**）。
  std::unique_ptr<AudioTrack> audio_;
  unsigned audio_payload_type_ = 0;

  /// 切分用的暂存，避免每帧都分配。只在编码线程上碰。
  /// 两路共用一份是安全的：push 是串行的，每次进来从头填。
  std::vector<Nalu> scratch_;

  TaskScheduler* scheduler_ = nullptr;
  UsageEnvironment* env_ = nullptr;
  RTSPServer* server_ = nullptr;

  /// doEventLoop 的停止标志。类型必须和 Live555 的 EventLoopWatchVariable 一致，
  /// .cpp 里有 static_assert 盯着。
  std::atomic_char watch_{0};

  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace d2o
