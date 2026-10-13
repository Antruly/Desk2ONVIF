#include "rtsp/rtsp_server.h"

#include "common/log.h"
#include "rtsp/aac_live_source.h"
#include "rtsp/au_queue.h"
#include "rtsp/h264_live_source.h"
#include "rtsp/nalu.h"
#include "rtsp/nalu_queue.h"

#include "BasicUsageEnvironment.hh"
#include "H264VideoRTPSink.hh"
#include "H264VideoStreamDiscreteFramer.hh"
#include "MPEG4GenericRTPSink.hh"
#include "MediaSink.hh"
#include "OnDemandServerMediaSubsession.hh"
#include "RTSPServer.hh"
#include "ServerMediaSession.hh"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <type_traits>

namespace d2o {

// doEventLoop() 收的是 EventLoopWatchVariable*，而那个 typedef 在"有标准库"的
// 构建下就是 std::atomic_char。rtsp_server.h 为了不把 Live555 拖进来只前向
// 声明了 UsageEnvironment，所以在这里钉一下，免得哪天它对不上了还没人发现。
static_assert(std::is_same<EventLoopWatchVariable, std::atomic_char>::value,
              "EventLoopWatchVariable 不是 std::atomic_char —— "
              "rtsp_server.h 里 watch_ 的类型要跟着改");

namespace {

/// H264or5Fragmenter 会把**整个 NALU** 先收进一个 `OutPacketBuffer::maxSize`
/// 大小的缓冲，然后才切片发 RTP。默认只有 100000 字节 —— 2880x1920 的
/// ultrafast 关键帧很容易越过它，而越过的后果是**静默截断**：不报错、不告警，
/// 只是那一帧画面破掉，下一帧又好了。
///
/// 这种"偶尔糊一下"是最难反查的一类症状，所以直接把上限抬到 2 MB
/// （每个流一个缓冲，内存代价可以忽略），并且把见过的最大 NALU 记下来备查。
constexpr unsigned kFragmentBufferBytes = 2u * 1024u * 1024u;

/// 首次 DESCRIBE 等参数集的上限。
///
/// DESCRIBE 会在 Live555 的**调度线程**上走到 `createNewRTPSink()`（基类会建一个
/// "假"的 source/sink 对来取 `getAuxSDPLine()`），所以在这里等待 = 把整个 RTSP
/// 事件循环按住。多通道下更要命：一条通道还没开好会把别的通道的客户端一起卡住。
///
/// 3 秒这个数是从两头夹出来的：往上，编码器打开实测在几百毫秒量级（DXGI 建会话
/// + x264 打开），3 秒是它的十倍，正常情况绝无可能等满；往下，NVR 的连接超时通常
/// 是 5~10 秒，3 秒之后我们**放弃等待、用空参数集把 SDP 发出去**，而不是让客户端
/// 挂着。
constexpr int kParamWaitMs = 3000;

}  // namespace

/// 一路码流的参数集槽位。
///
/// ## 为什么是"槽位"而不是两个 vector 成员
///
/// 参数集是**后到的**：`start()` 建会话时编码器还没打开（见头文件的说明）。
/// 而读它的那一刻在别的线程上 —— Live555 调度线程的 `createNewRTPSink()`。
///
/// ## 为什么是 shared_ptr 而不是裸指针
///
/// 读写两端分属两个对象（`Stream` 和每个客户端各自的 `LiveSubsession`），而它们的
/// 析构顺序**不由我们决定**（`deleteServerMediaSession` 会一路析构到 subsession，
/// 那之后 `streams_` 才 clear）。裸指针 + "析构时要小心顺序"就是一道靠记性维持的
/// 约束 —— 而忘记它的症状是 use-after-free，崩在别的地方。shared_ptr 让这件事
/// 变成不需要记的。
struct StreamParams {
  mutable std::mutex mu;
  /// mutable 而不是把 wait_ready() 写成非 const：`wait_for` 在
  /// `condition_variable` 上是非 const 的（它要改内部等待队列），而这里
  /// "等一会儿"在调用方眼里完全是读操作。把方法写成非 const 的话，
  /// `createNewRTPSink()` 那条 const 路径就得把 params_ 也变成非 const。
  mutable std::condition_variable cv;
  std::vector<std::uint8_t> sps;
  std::vector<std::uint8_t> pps;
  bool ready = false;

  /// 等参数集到齐，最多 `ms`。返回 false = 等超时（调用方要用空参数集继续，
  /// **不能**失败整条 DESCRIBE，见 kParamWaitMs 的说明）。
  bool wait_ready(int ms) const {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, std::chrono::milliseconds(ms),
                       [this] { return ready; });
  }

  void fill(std::vector<std::uint8_t> s, std::vector<std::uint8_t> p) {
    {
      std::lock_guard<std::mutex> lk(mu);
      sps = std::move(s);
      pps = std::move(p);
      ready = true;
    }
    cv.notify_all();
  }

  /// 取一份**拷贝**。拷贝是必要的：sink 构造之后 Live555 会一直持有它自己的
  /// 那份（`H264VideoRTPSink` 把 sps/pps 存进 `fSPS`/`fPPS`），所以不能把
  /// vector 的 `data()` 交出去之后就撒手 —— 下一次 `fill()` 会把它挪走。
  void snapshot(std::vector<std::uint8_t>& s, std::vector<std::uint8_t>& p) const {
    std::lock_guard<std::mutex> lk(mu);
    s = sps;
    p = pps;
  }
};

/// RTSP 那一侧的 ServerMediaSubsession。
///
/// 每个客户端连接都会在这上面走一遍 startStream/deleteStream，但**只有一份
/// source**（`reuseFirstSource=True`）：每一路码流只有一个编码器，多个客户端
/// 共享它。示例代码里常写 False，那是给文件源用的 —— 直播照抄会变成每个
/// 客户端各起一份源，而我们的源是同一个队列，一帧数据只会被其中一个客户端
/// 取走，其余客户端全部饿死。
class LiveSubsession : public OnDemandServerMediaSubsession {
 public:
  static LiveSubsession* createNew(UsageEnvironment& env,
                                   std::shared_ptr<NaluQueue> queue,
                                   RtspStreamOptions const& opt,
                                   std::shared_ptr<StreamParams> params,
                                   std::atomic<unsigned>* clients,
                                   std::atomic<H264LiveSource*>* source_slot) {
    return new LiveSubsession(env, std::move(queue), opt, std::move(params),
                              clients, source_slot);
  }

 protected:
  LiveSubsession(UsageEnvironment& env, std::shared_ptr<NaluQueue> queue,
                 RtspStreamOptions const& opt,
                 std::shared_ptr<StreamParams> params,
                 std::atomic<unsigned>* clients,
                 std::atomic<H264LiveSource*>* source_slot)
      : OnDemandServerMediaSubsession(env, True /* reuseFirstSource */),
        queue_(std::move(queue)),
        opt_(opt),
        params_(std::move(params)),
        clients_(clients),
        source_slot_(source_slot) {}

  virtual ~LiveSubsession() {
    // 先把槽位清掉，再让基类去析构 source。反过来的话，窗口期内
    // RtspServer::push_annex_b 会拿到一个已经死掉的指针。
    if (source_slot_) source_slot_->store(nullptr, std::memory_order_release);
  }

  virtual FramedSource* createNewStreamSource(unsigned /*clientSessionId*/,
                                              unsigned& estBitrate) {
    estBitrate = static_cast<unsigned>(opt_.bitrate_kbps);

    auto* src = H264LiveSource::createNew(envir(), queue_, opt_.fps);
    if (source_slot_) source_slot_->store(src, std::memory_order_release);

    // 必须夹一层 DiscreteFramer 才能接 RTPSink，两个原因都是硬性的：
    //   - H264VideoRTPSink::sourceIsCompatibleWithUs() 要求上游是 H264 framer，
    //     裸 FramedSource 会被拒；
    //   - H264or5VideoRTPSink::doSpecialFrameHandling() 会把 fragmenter 的上游
    //     **盲目强转**成 H264or5VideoStreamFramer* 去读 pictureEndMarker()。
    //     上游不是 framer 的话，那就是对着一块无关内存做虚函数调用。
    //
    // framer 会接管 src 的生命周期（FramedFilter 析构时 close 掉输入源），
    // 所以上面存进 source_slot_ 的是**非持有**指针，只是拿来唤醒用。
    return H264VideoStreamDiscreteFramer::createNew(envir(), src);
  }

  virtual RTPSink* createNewRTPSink(Groupsock* rtpGroupsock,
                                    unsigned char rtpPayloadTypeIfDynamic,
                                    FramedSource* /*inputSource*/) {
    // 构造时就把 SPS/PPS 交给 sink：这样 auxSDPLine() 第一次调用就有内容，
    // **不需要**覆写 getAuxSDPLine()，也不需要文件源那套"先预读一帧"的把戏。
    // SDP 里的 sprop-parameter-sets（base64）Live555 自己生成。
    //
    // 参数集是**后填的**（见 rtsp_server.h 上 start() 的说明），所以这里要等。
    // 等不到就照样建 sink —— 只是 SDP 里没有 sprop-parameter-sets。这**不是**
    // 致命错误：`push_annex_b` 会在每个 IDR 前补发带内参数集，所以最坏情况是
    // "第一个关键帧之前花屏"，而不是黑屏到底。反过来让 DESCRIBE 失败的话，
    // NVR 那边看到的是"添加失败"，那才是真的没救。
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
    if (params_) {
      if (!params_->wait_ready(kParamWaitMs)) {
        D2O_WARN("等参数集超时（%d ms）—— 这一路的 SDP 里没有 "
                 "sprop-parameter-sets，客户端要靠带内参数集才能出画。",
                 kParamWaitMs);
      }
      params_->snapshot(sps, pps);
    }
    return H264VideoRTPSink::createNew(
        envir(), rtpGroupsock, rtpPayloadTypeIfDynamic, sps.data(),
        static_cast<unsigned>(sps.size()), pps.data(),
        static_cast<unsigned>(pps.size()));
  }

  virtual void startStream(
      unsigned clientSessionId, void* streamToken, TaskFunc* rtcpRRHandler,
      void* rtcpRRHandlerClientData, unsigned short& rtpSeqNum,
      unsigned& rtpTimestamp,
      ServerRequestAlternativeByteHandler* serverRequestAlternativeByteHandler,
      void* serverRequestAlternativeByteHandlerClientData) {
    OnDemandServerMediaSubsession::startStream(
        clientSessionId, streamToken, rtcpRRHandler, rtcpRRHandlerClientData,
        rtpSeqNum, rtpTimestamp, serverRequestAlternativeByteHandler,
        serverRequestAlternativeByteHandlerClientData);
    if (clients_) clients_->fetch_add(1, std::memory_order_relaxed);
    // 有人开始看了：队列开始收数据，并进入"等下一个关键帧"状态。
    if (queue_) queue_->set_armed(true);
  }

  virtual void deleteStream(unsigned clientSessionId, void*& streamToken) {
    OnDemandServerMediaSubsession::deleteStream(clientSessionId, streamToken);
    if (clients_ && clients_->load(std::memory_order_relaxed) > 0) {
      clients_->fetch_sub(1, std::memory_order_relaxed);
    }
    if (queue_ &&
        (!clients_ || clients_->load(std::memory_order_relaxed) == 0)) {
      queue_->set_armed(false);
    }
  }

 private:
  std::shared_ptr<NaluQueue> queue_;
  RtspStreamOptions opt_;
  /// 参数集槽位。**共享**（不是拷贝）：`createNewRTPSink` 每次都要读当期值 ——
  /// 通道中途换分辨率之后，新连上来的客户端要拿到新的那份 SDP。
  std::shared_ptr<StreamParams> params_;
  std::atomic<unsigned>* clients_ = nullptr;
  std::atomic<H264LiveSource*>* source_slot_ = nullptr;
};

/// 音频那一路的 ServerMediaSubsession。和 LiveSubsession 同构，差别都来自
/// "音频不需要视频那些东西"：
///
///   - **不夹 framer**。`MPEG4GenericRTPSink` 没有覆写
///     `sourceIsCompatibleWithUs()`（覆写它的是 AMR/DV/H264/H265/JPEG/
///     MPEG1or2/MPEG4ES/HLSSegmenter 那几个），于是落到 `MediaSink` 的默认实现
///     —— 返回 True。裸 FramedSource 可以直接接。视频那一路不行，除了兼容性检查，
///     H264 的 sink 还会把上游**盲目强转**成 framer 去读 pictureEndMarker()。
///     所以这里**不要**"为了对称"也套一层 framer：那会变成给裸 AU 加 ADTS 头
///     （陷阱 76）的同一种错。
///   - sink 是 `MPEG4GenericRTPSink`，`config_hex` 在**构造时**就交进去，
///     `auxSDPLine()` 因此第一次调用就有内容（`fFmtpSDPLine` 在构造函数里就拼好了，
///     见 MPEG4GenericRTPSink.cpp:53-71）⇒ **不需要**覆写 `getAuxSDPLine()`。
///
/// `source_slot_` 存的是**非持有**指针，只用来唤醒 —— 真属主是 sink 链。
class AudioSubsession : public OnDemandServerMediaSubsession {
 public:
  static AudioSubsession* createNew(UsageEnvironment& env,
                                    std::shared_ptr<AuQueue> queue,
                                    RtspAudioOptions const& opt,
                                    std::atomic<unsigned>* clients,
                                    std::atomic<AacLiveSource*>* source_slot) {
    return new AudioSubsession(env, std::move(queue), opt, clients, source_slot);
  }

 protected:
  AudioSubsession(UsageEnvironment& env, std::shared_ptr<AuQueue> queue,
                  RtspAudioOptions const& opt, std::atomic<unsigned>* clients,
                  std::atomic<AacLiveSource*>* source_slot)
      : OnDemandServerMediaSubsession(env, True /* reuseFirstSource */),
        queue_(std::move(queue)),
        opt_(opt),
        clients_(clients),
        source_slot_(source_slot) {}

  virtual ~AudioSubsession() {
    // 先清槽位再让基类析构 source。反过来的话窗口期内 push_audio_au 会拿到
    // 一个已经死掉的指针。
    //
    // 槽位的**生命周期**由"没有人看时空着"这件事兜底：它只在 startStream 之后
    // 到 deleteStream 之前被 push_audio_au 读到，而那段时间里 source 一定活着
    // （reuseFirstSource 让它活到最后一个客户端断开）。SDP 生成时 Live555 也会
    // 建一个"假"的 source 再关掉（OnDemandServerMediaSubsession.cpp:83/102），
    // 但那个时刻队列还没被武装，所以读不到它。
    if (source_slot_) source_slot_->store(nullptr, std::memory_order_release);
  }

  virtual FramedSource* createNewStreamSource(unsigned /*clientSessionId*/,
                                              unsigned& estBitrate) {
    estBitrate = static_cast<unsigned>(opt_.bitrate_kbps);
    auto* src = AacLiveSource::createNew(envir(), queue_, opt_.sample_rate,
                                        opt_.samples_per_au);
    if (source_slot_) source_slot_->store(src, std::memory_order_release);
    return src;  // 不套 framer，见类注释
  }

  virtual RTPSink* createNewRTPSink(Groupsock* rtpGroupsock,
                                    unsigned char rtpPayloadTypeIfDynamic,
                                    FramedSource* /*inputSource*/) {
    // 裸 AU + AudioSpecificConfig。**不要**用 ADTSAudioStreamDiscreteFramer，
    // 它干的是反事：给裸帧**加** ADTS 头（陷阱 76）。
    // sdpMediaTypeString 传 "audio"，mpeg4Mode 必须是 "AAC-hbr"（大小写不敏感，
    // 构造里会转小写比对，写错了只会在 stderr 上打一行 error 然后照样跑）。
    return MPEG4GenericRTPSink::createNew(
        envir(), rtpGroupsock, rtpPayloadTypeIfDynamic, opt_.sample_rate,
        "audio", "AAC-hbr", opt_.config_hex.c_str(), opt_.channels);
  }

  virtual void startStream(
      unsigned clientSessionId, void* streamToken, TaskFunc* rtcpRRHandler,
      void* rtcpRRHandlerClientData, unsigned short& rtpSeqNum,
      unsigned& rtpTimestamp,
      ServerRequestAlternativeByteHandler* serverRequestAlternativeByteHandler,
      void* serverRequestAlternativeByteHandlerClientData) {
    OnDemandServerMediaSubsession::startStream(
        clientSessionId, streamToken, rtcpRRHandler, rtcpRRHandlerClientData,
        rtpSeqNum, rtpTimestamp, serverRequestAlternativeByteHandler,
        serverRequestAlternativeByteHandlerClientData);
    if (clients_) clients_->fetch_add(1, std::memory_order_relaxed);
    // 有人开始看了。音频**没有**"等关键帧"那一步 —— 每个 AU 独立可解。
    if (queue_) queue_->set_armed(true);
  }

  virtual void deleteStream(unsigned clientSessionId, void*& streamToken) {
    OnDemandServerMediaSubsession::deleteStream(clientSessionId, streamToken);
    if (clients_ && clients_->load(std::memory_order_relaxed) > 0) {
      clients_->fetch_sub(1, std::memory_order_relaxed);
    }
    if (queue_ &&
        (!clients_ || clients_->load(std::memory_order_relaxed) == 0)) {
      queue_->set_armed(false);
    }
  }

 private:
  std::shared_ptr<AuQueue> queue_;
  RtspAudioOptions opt_;
  std::atomic<unsigned>* clients_ = nullptr;
  std::atomic<AacLiveSource*>* source_slot_ = nullptr;
};

/// 一路码流的全部状态。
///
/// 用一个 `std::vector<std::unique_ptr<Stream>>` 而不是 `vector<Stream>`：
/// 里面有 `std::atomic`，既不能拷也不能移，而 vector 在扩容时要移元素。
/// 指针的稳定性在这里还有第二个用处 —— LiveSubsession 存的是这些 atomic 的
/// **地址**，所以这个 vector 在 start() 之后绝不能再动。
struct RtspServer::Stream {
  RtspStreamOptions opt;
  std::shared_ptr<NaluQueue> queue;
  /// 参数集槽位。**可后填、可重填**（见 rtsp_server.h 上 start() 的说明）。
  /// 每个客户端各自的 LiveSubsession 也持一份，所以它的生命周期不依赖这个
  /// `Stream` 活多久 —— 那是 shared_ptr 而不是引用的理由。
  std::shared_ptr<StreamParams> params;
  ServerMediaSession* sms = nullptr;
  std::atomic<unsigned> clients{0};
  /// 由 LiveSubsession 维护，不持有所有权（真正的属主是 framer 链）。
  std::atomic<H264LiveSource*> source{nullptr};
  /// 多 slice 只告警一次，免得刷屏。只在编码线程上碰。
  bool warned_multislice = false;
};

/// 音频那一路的全部状态。
///
/// 和 `Stream` 分开而不是做成它的一个变体：它**没有 sps/pps、没有 sms**
/// （挂在主码流那个会话下）、而且只有一个。做成变体的话，"主码流那一路的
/// audio 成员"这种没有意义的东西就会出现在类型里。
struct RtspServer::AudioTrack {
  RtspAudioOptions opt;
  std::shared_ptr<AuQueue> queue;
  std::atomic<unsigned> clients{0};
  /// 由 AudioSubsession 维护，不持有所有权。
  std::atomic<AacLiveSource*> source{nullptr};
};

RtspServer::RtspServer() = default;   // 见头文件里的说明：必须在 Stream 完整之后

RtspServer::~RtspServer() { stop(); }

RtspServer::Stream* RtspServer::at(std::size_t i) {
  return i < streams_.size() ? streams_[i].get() : nullptr;
}

RtspServer::Stream const* RtspServer::at(std::size_t i) const {
  return i < streams_.size() ? streams_[i].get() : nullptr;
}

bool RtspServer::start(RtspOptions const& opt,
                       std::vector<std::vector<std::uint8_t>> const& extradata,
                       std::string& err) {
  if (!start(opt, err)) return false;
  if (extradata.size() != opt_.streams.size()) {
    err = fmt("RTSP 有 %zu 路码流，但拿到 %zu 份 extradata —— 调用方把编码器和"
              "码流的对应关系接错了。",
              opt_.streams.size(), extradata.size());
    stop();
    return false;
  }
  for (std::size_t i = 0; i < extradata.size(); ++i) {
    if (!set_stream_extradata(i, extradata[i], err)) {
      stop();
      return false;
    }
  }
  return true;
}

bool RtspServer::set_stream_extradata(std::size_t stream,
                                      std::vector<std::uint8_t> const& annexb,
                                      std::string& err) {
  Stream* st = at(stream);
  if (!st) {
    err = fmt("要填参数集的是第 %zu 路码流，但服务器上一共只有 %zu 路。",
              stream, streams_.size());
    return false;
  }
  std::vector<std::uint8_t> sps;
  std::vector<std::uint8_t> pps;
  if (!extract_sps_pps(annexb.data(), annexb.size(), sps, pps)) {
    err = fmt("第 %zu 路码流（/%s）的 extradata 里没有 SPS/PPS。SDP 的 "
              "sprop-parameter-sets 要靠它们，缺了很多 NVR 加进来就是黑屏。"
              "检查编码器是不是打开了 global_header。",
              stream, st->opt.path.c_str());
    return false;
  }
  D2O_DEBUG("第 %zu 路码流 /%s：SPS %zu 字节，PPS %zu 字节", stream,
            st->opt.path.c_str(), sps.size(), pps.size());
  if (!st->params) st->params = std::make_shared<StreamParams>();
  st->params->fill(std::move(sps), std::move(pps));
  return true;
}

bool RtspServer::stream_params_ready(std::size_t stream) const {
  Stream const* st = at(stream);
  if (!st || !st->params) return false;
  std::lock_guard<std::mutex> lk(st->params->mu);
  return st->params->ready;
}

bool RtspServer::start(RtspOptions const& opt, std::string& err) {
  if (running_.load(std::memory_order_relaxed)) {
    err = "RTSP 服务器已经在跑了";
    return false;
  }
  opt_ = opt;

  // 空数组 + enabled 是调用方的记账错误。宽容地什么都不做的话，现象是
  // "NVR 连得上、永远没有画面" —— 而这条链路上每一个环节看上去都是好的。
  if (opt_.streams.empty()) {
    err = "RTSP 开着但一路码流都没配（RtspOptions::streams 是空的）。";
    return false;
  }
  for (RtspStreamOptions& s : opt_.streams) {
    if (s.fps == 0) s.fps = 30;
  }

  // 音频这一路要在这里就把参数定下来：它的 subsession 在下面那个循环里挂到
  // 主码流的会话上，而那个会话在同一个循环里就注册给服务器了。
  if (opt_.audio.enabled) {
    // SDP 里的 `a=fmtp:97 …config=` 就是这两字节。缺了的话**收端照样能连上、
    // ffprobe 照样报得出 48000 Hz 立体声 AAC**，只是解出来是噪音 —— 所以这里
    // 宁可让 RTSP 起不来，也不要建一条"看着有、实际没声"的音轨。
    if (opt_.audio.config_hex.empty()) {
      err = "RTSP 开了音频，但 AudioSpecificConfig（config_hex）是空的 —— "
            "SDP 的 a=fmtp:…config= 靠它。它来自 AacEncoder::asc_hex()。";
      streams_.clear();
      return false;
    }
    if (opt_.audio.sample_rate == 0) opt_.audio.sample_rate = 48000;
    if (opt_.audio.channels == 0) opt_.audio.channels = 1;
    if (opt_.audio.samples_per_au == 0) opt_.audio.samples_per_au = 1024;
    if (opt_.audio.queue_capacity == 0) opt_.audio.queue_capacity = 96;

    audio_ = std::make_unique<AudioTrack>();
    audio_->opt = opt_.audio;
    audio_->queue = std::make_shared<AuQueue>(audio_->opt.queue_capacity);
  }

  streams_.reserve(opt_.streams.size());
  for (std::size_t i = 0; i < opt_.streams.size(); ++i) {
    auto st = std::make_unique<Stream>();
    st->opt = opt_.streams[i];
    // 槽位**现在就建**（空的），不等到 set_stream_extradata：万一参数集一直
    // 没来（某个通道开不起来），subsession 那边也还有一个合法的 shared_ptr
    // 可以读，而不是一个要处处判空的空指针。
    st->params = std::make_shared<StreamParams>();
    streams_.push_back(std::move(st));
  }

  // 见 kFragmentBufferBytes 的说明。
  OutPacketBuffer::increaseMaxSizeTo(kFragmentBufferBytes);

  // createNew 的默认参数是 10000 微秒，也就是 triggerEvent 的派发延迟上限。
  // 不能传 0 —— 0 的含义是"select 不设超时"，那样事件永远不会被派发，
  // 整个推流会卡死在第一帧（源码注释也明说了 0 只在不用事件触发器时才行）。
  // 30 fps 下一帧 33 ms，10 ms 的派发延迟完全够用。
  scheduler_ = BasicTaskScheduler::createNew();
  if (!scheduler_) {
    err = "创建 Live555 调度器失败";
    return false;
  }
  env_ = BasicUsageEnvironment::createNew(*scheduler_);
  if (!env_) {
    err = "创建 Live555 运行环境失败";
    teardown();
    return false;
  }

  server_ = RTSPServer::createNew(*env_, Port(opt_.port));
  if (!server_) {
    err = fmt("创建 RTSP 服务器失败：端口 %u 可能已被占用，或者被防火墙拦了。"
              "换个端口试试（--rtsp-port）。",
              static_cast<unsigned>(opt_.port));
    teardown();
    return false;
  }

  // 每一路一个队列 + 一个 ServerMediaSession。两路之间**不共享任何东西**：
  // 主码流的关键帧和子码流的关键帧不是同时产生的（两个编码器各自计数），
  // 共用队列的话，一路的关键帧会把另一路的 GOP 边界搅乱。
  for (std::size_t i = 0; i < streams_.size(); ++i) {
    Stream& st = *streams_[i];
    st.queue = std::make_shared<NaluQueue>(st.opt.queue_capacity);

    // 会话名进 SDP 的 `s=` 行和 `a=x-qt-text-nam`。**必须纯 ASCII**：Live555
    // 是原样写进去的，而 NVR 那边解析 SDP 的多半是个手写的窄字符解析器，
    // 非 ASCII 字节轻则显示成乱码，重则让整个 SDP 解析失败（那就连流都拉不起来）。
    // 这里没有任何理由冒这个险。
    st.sms = ServerMediaSession::createNew(
        *env_, st.opt.path.c_str(), nullptr,
        i == 0 ? "Desk2ONVIF desktop live" : "Desk2ONVIF desktop live (sub)");
    if (!st.sms) {
      err = fmt("创建第 %zu 路（/%s）的 ServerMediaSession 失败", i,
                st.opt.path.c_str());
      teardown();
      return false;
    }

    // &st.clients / &st.source 的地址必须长期稳定 —— 所以 streams_ 在 start()
    // 之后一次都不能再改（上面已经 reserve 过了）。
    st.sms->addSubsession(LiveSubsession::createNew(
        *env_, st.queue, st.opt, st.params, &st.clients, &st.source));

    // 音频挂在**主码流**这个会话上，而且必须**排在视频之后**加：动态载荷的
    // 编号是 `96 + trackNumber() - 1`，而 trackNumber 是"会话里第几个
    // subsession"（ServerMediaSession.cpp:105 的 `fTrackNumber = ++fSubsessionCounter`）。
    // 所以音频自动拿到 97 —— 不要自己硬编码，也别在这里改顺序。
    if (i == 0 && audio_) {
      auto* ass = AudioSubsession::createNew(*env_, audio_->queue, audio_->opt,
                                            &audio_->clients, &audio_->source);
      st.sms->addSubsession(ass);
      audio_payload_type_ = 96 + ass->trackNumber() - 1;
      D2O_INFO("RTSP 音频已挂到主码流会话上：payload type %u，%u Hz %u 声道，"
               "config=%s，队列 %zu 个 AU",
               audio_payload_type_, audio_->opt.sample_rate,
               audio_->opt.channels, audio_->opt.config_hex.c_str(),
               audio_->opt.queue_capacity);
    }

    server_->addServerMediaSession(st.sms);
  }

  running_.store(true, std::memory_order_relaxed);
  // doEventLoop 在 TaskScheduler 上，不在 UsageEnvironment 上 ——
  // 这两个类声明在同一个头文件里，很容易看串。
  thread_ = std::thread([this] { scheduler_->doEventLoop(&watch_); });
  return true;
}

void RtspServer::stop() {
  if (!running_.exchange(false, std::memory_order_relaxed)) return;
  // 让 doEventLoop 返回。等待上限是一个调度 tick（10 ms）。
  watch_.store(1, std::memory_order_relaxed);
  if (thread_.joinable()) thread_.join();
  teardown();
}

void RtspServer::teardown() {
  // 走到这里时调度线程必须已经停了（见头文件的停机顺序说明）。
  //
  // 音频的槽位要**先**清：它的 subsession 是主码流那个 ServerMediaSession 的
  // 第二个 subsession，下面那句 deleteServerMediaSession 会一路析构到
  // AacLiveSource。先清槽，窗口期内 push_audio_au 才不会拿到死指针。
  if (audio_) audio_->source.store(nullptr, std::memory_order_release);
  for (auto& st : streams_) {
    if (st) st->source.store(nullptr, std::memory_order_release);
  }

  for (auto& st : streams_) {
    if (!st || !st->sms) continue;
    if (!server_) break;   // 服务器都没了，sms 也跟着没了
    // deleteServerMediaSession 是基类 GenericMediaServer 上的，不在 RTSPServer
    // 自己的声明里 —— 所以光看 RTSPServer.hh 会以为这个 API 没了。
    // 它等价于 closeAllClientSessionsForServerMediaSession + remove，并在最后
    // 一个引用消失时把 sms 删掉，也就是一路析构到 H264LiveSource。
    server_->deleteServerMediaSession(st->sms);
    st->sms = nullptr;
  }
  streams_.clear();
  // 音频的 queue 和两个 atomic 跟着一起走。subsession 本身已经随上面那个 sms
  // 析构了（这里不持有它），所以只需要把状态清掉。
  audio_.reset();
  audio_payload_type_ = 0;

  if (server_) {
    Medium::close(server_);
    server_ = nullptr;
  }
  if (env_) {
    if (!env_->reclaim()) {
      // 返回 False 说明还有 source/sink 持有库状态。多半是某条析构路径没走完，
      // 继续下去会带着一堆悬挂的 Medium 去 delete scheduler。
      D2O_WARN("Live555 还有对象没归还（reclaim 返回 False）——有 source/sink 泄漏");
    }
    env_ = nullptr;
  }
  if (scheduler_) {
    delete scheduler_;
    scheduler_ = nullptr;
  }
  scratch_.clear();
}

void RtspServer::push_annex_b(std::size_t stream, std::uint8_t const* data,
                              std::size_t size, bool keyframe, std::int64_t pts) {
  if (!running_.load(std::memory_order_relaxed)) return;
  Stream* pst = at(stream);
  if (!pst) return;  // 下标写错：启动时就该发现，这里不崩也不喊
  Stream& st = *pst;
  if (!st.queue || !st.queue->armed()) return;  // 没人在看，切分都省了

  scratch_.clear();
  split_annex_b(data, size, pts, keyframe, scratch_);
  if (scratch_.empty()) return;

  // 多 slice 的守门人。见 nalu.h 和 x264_encoder.cpp 的说明：Live555 的
  // nalUnitEndsAccessUnit() 把**每个** VCL NALU 都当成一帧的结尾，所以一帧里
  // 有多个 VCL NALU 时 RTP 的 M 位就是错的，收端会把每个 slice 当成独立一帧。
  //
  // 这一路故障的可怕之处在于它**全是绿灯**：客户端连得上、SDP 正确、ffprobe
  // 报出正确的分辨率和帧率、ffmpeg 也能解出整整 30 fps 的帧 —— 只有解码器日志
  // 里那行 `concealing N DC, N AC, N MV errors` 和糊掉的画面能看出来。
  // 编码器那边已经堵死了（thread_type / sliced-threads=0），这里再放一个哨兵：
  // 万一哪天换了 FFmpeg 版本、或者有人改了 preset，让它至少**喊一声**。
  //
  // 每路各喊一次：两路是两个编码器，一路的参数被人改坏了不代表另一路也是。
  if (!st.warned_multislice) {
    int vcl = 0;
    for (Nalu const& n : scratch_) {
      if (n.is_vcl()) ++vcl;
    }
    if (vcl > 1) {
      st.warned_multislice = true;
      D2O_WARN(
          "第 %zu 路（/%s）一个访问单元里有 %d 个 VCL NALU（多 slice）。"
          "Live555 会把每个 VCL NALU 都当成一帧的结尾，RTP 的 M 位因此出错，"
          "收端把每个 slice 解成独立一帧 —— 现象是能连上、有画面，但大面积花屏。"
          "查编码器的 sliced-threads / thread_type。",
          stream, st.opt.path.c_str(), vcl);
    }
  }

  // 关键帧前重发参数集。SDP 里的 sprop-parameter-sets 只覆盖"从头开始看"的
  // 客户端；中途接入的、或者丢了 SDP 的（部分 NVR 就是这样）得靠带内参数集
  // 才能起画面。这一步顺带给 NaluQueue 的"等关键帧"逻辑一个确定的起点 ——
  // 它等的就是这里的 SPS。
  bool injected = false;
  for (Nalu& n : scratch_) {
    if (!injected && n.is_idr()) {
      injected = true;
      // 从槽位取一份。**每个 IDR 取一次**（不是启动时取一次）：通道中途换了
      // 分辨率、编码器重建，槽位里就是新的那一份，而这里自动跟上。代价是每个
      // GOP 两次 vector 拷贝（SPS/PPS 合起来几十字节），可以忽略。
      std::vector<std::uint8_t> sps;
      std::vector<std::uint8_t> pps;
      if (st.params) st.params->snapshot(sps, pps);
      if (!sps.empty()) {
        Nalu s;
        s.data = std::move(sps);
        s.pts = pts;
        s.keyframe = true;
        st.queue->push(std::move(s));
      }
      if (!pps.empty()) {
        Nalu p;
        p.data = std::move(pps);
        p.pts = pts;
        p.keyframe = true;
        st.queue->push(std::move(p));
      }
    }
    st.queue->push(std::move(n));
  }

  // 只在全部入队之后唤醒一次。wake() 自己有门闩，连着调也无害，
  // 但一次就够 —— 醒来之后 deliver() 会把队列里的数据连着投完。
  if (auto* src = st.source.load(std::memory_order_acquire)) src->wake();
}

void RtspServer::push_audio_au(std::uint8_t const* data, std::size_t size,
                               std::int64_t pts) {
  if (!running_.load(std::memory_order_relaxed)) return;
  AudioTrack* at = audio_.get();
  if (!at || !at->queue) return;  // 没开音频
  if (!data || size == 0) return;
  // 没人在看就**连拷贝都不做**。和视频那一路同一个理由：没人看的时候白占内存。
  if (!at->queue->armed()) return;

  Au au;
  au.data.assign(data, data + size);
  au.pts = pts;
  at->queue->push(std::move(au));

  // 只在入队之后唤醒一次。wake() 自己有门闩，一次就够 ——
  // 醒来之后 deliver() 会把队列里的数据连着投完。
  if (auto* src = at->source.load(std::memory_order_acquire)) src->wake();
}

std::string RtspServer::url(std::size_t stream) const {
  Stream const* st = at(stream);
  if (!server_ || !st || !st->sms) return std::string();
  char* u = server_->rtspURL(st->sms);
  if (!u) return std::string();
  std::string s(u);
  delete[] u;  // rtspURL 的返回值是要调用方 delete[] 的
  return s;
}

unsigned RtspServer::client_count(std::size_t stream) const {
  Stream const* st = at(stream);
  return st ? st->clients.load(std::memory_order_relaxed) : 0u;
}

unsigned RtspServer::client_count() const {
  unsigned n = 0;
  for (auto const& st : streams_) {
    if (st) n += st->clients.load(std::memory_order_relaxed);
  }
  return n;
}

std::size_t RtspServer::queue_capacity(std::size_t stream) const {
  Stream const* st = at(stream);
  return st ? st->opt.queue_capacity : 0;
}

std::size_t RtspServer::queue_size(std::size_t stream) const {
  Stream const* st = at(stream);
  return st && st->queue ? st->queue->size() : 0;
}

std::uint64_t RtspServer::nalu_dropped(std::size_t stream) const {
  Stream const* st = at(stream);
  return st && st->queue ? st->queue->dropped() : 0;
}

std::uint64_t RtspServer::nalu_pushed(std::size_t stream) const {
  Stream const* st = at(stream);
  return st && st->queue ? st->queue->pushed() : 0;
}

std::size_t RtspServer::max_nalu_bytes(std::size_t stream) const {
  Stream const* st = at(stream);
  return st && st->queue ? st->queue->max_nalu_bytes() : 0;
}

std::size_t RtspServer::sps_size(std::size_t stream) const {
  Stream const* st = at(stream);
  if (!st || !st->params) return 0;
  std::lock_guard<std::mutex> lk(st->params->mu);
  return st->params->sps.size();
}

std::size_t RtspServer::pps_size(std::size_t stream) const {
  Stream const* st = at(stream);
  if (!st || !st->params) return 0;
  std::lock_guard<std::mutex> lk(st->params->mu);
  return st->params->pps.size();
}

unsigned RtspServer::audio_client_count() const {
  return audio_ ? audio_->clients.load(std::memory_order_relaxed) : 0u;
}

std::size_t RtspServer::audio_queue_capacity() const {
  return audio_ && audio_->queue ? audio_->queue->capacity() : 0;
}

std::size_t RtspServer::audio_queue_size() const {
  return audio_ && audio_->queue ? audio_->queue->size() : 0;
}

std::uint64_t RtspServer::audio_au_dropped() const {
  return audio_ && audio_->queue ? audio_->queue->dropped() : 0;
}

std::uint64_t RtspServer::audio_au_pushed() const {
  return audio_ && audio_->queue ? audio_->queue->pushed() : 0;
}

std::string RtspServer::audio_config_hex() const {
  return audio_ ? audio_->opt.config_hex : std::string();
}

std::size_t RtspServer::audio_max_au_bytes() const {
  return audio_ && audio_->queue ? audio_->queue->max_au_bytes() : 0;
}

}  // namespace d2o
