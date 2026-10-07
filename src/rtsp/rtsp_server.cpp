#include "rtsp/rtsp_server.h"

#include "common/log.h"
#include "rtsp/h264_live_source.h"
#include "rtsp/nalu.h"
#include "rtsp/nalu_queue.h"

#include "BasicUsageEnvironment.hh"
#include "H264VideoRTPSink.hh"
#include "H264VideoStreamDiscreteFramer.hh"
#include "MediaSink.hh"
#include "OnDemandServerMediaSubsession.hh"
#include "RTSPServer.hh"
#include "ServerMediaSession.hh"

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

}  // namespace

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
                                   std::vector<std::uint8_t> sps,
                                   std::vector<std::uint8_t> pps,
                                   std::atomic<unsigned>* clients,
                                   std::atomic<H264LiveSource*>* source_slot) {
    return new LiveSubsession(env, std::move(queue), opt, std::move(sps),
                              std::move(pps), clients, source_slot);
  }

 protected:
  LiveSubsession(UsageEnvironment& env, std::shared_ptr<NaluQueue> queue,
                 RtspStreamOptions const& opt, std::vector<std::uint8_t> sps,
                 std::vector<std::uint8_t> pps,
                 std::atomic<unsigned>* clients,
                 std::atomic<H264LiveSource*>* source_slot)
      : OnDemandServerMediaSubsession(env, True /* reuseFirstSource */),
        queue_(std::move(queue)),
        opt_(opt),
        sps_(std::move(sps)),
        pps_(std::move(pps)),
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
    return H264VideoRTPSink::createNew(
        envir(), rtpGroupsock, rtpPayloadTypeIfDynamic, sps_.data(),
        static_cast<unsigned>(sps_.size()), pps_.data(),
        static_cast<unsigned>(pps_.size()));
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
  std::vector<std::uint8_t> sps_;
  std::vector<std::uint8_t> pps_;
  std::atomic<unsigned>* clients_ = nullptr;
  std::atomic<H264LiveSource*>* source_slot_ = nullptr;
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
  std::vector<std::uint8_t> sps;
  std::vector<std::uint8_t> pps;
  ServerMediaSession* sms = nullptr;
  std::atomic<unsigned> clients{0};
  /// 由 LiveSubsession 维护，不持有所有权（真正的属主是 framer 链）。
  std::atomic<H264LiveSource*> source{nullptr};
  /// 多 slice 只告警一次，免得刷屏。只在编码线程上碰。
  bool warned_multislice = false;
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
  if (extradata.size() != opt_.streams.size()) {
    err = fmt("RTSP 有 %zu 路码流，但拿到 %zu 份 extradata —— 调用方把编码器和"
              "码流的对应关系接错了。",
              opt_.streams.size(), extradata.size());
    return false;
  }
  for (RtspStreamOptions& s : opt_.streams) {
    if (s.fps == 0) s.fps = 30;
  }

  streams_.reserve(opt_.streams.size());
  for (std::size_t i = 0; i < opt_.streams.size(); ++i) {
    auto st = std::make_unique<Stream>();
    st->opt = opt_.streams[i];
    if (!extract_sps_pps(extradata[i].data(), extradata[i].size(), st->sps,
                         st->pps)) {
      err = fmt("第 %zu 路码流（/%s）的 extradata 里没有 SPS/PPS。SDP 的 "
                "sprop-parameter-sets 要靠它们，缺了很多 NVR 加进来就是黑屏。"
                "检查编码器是不是打开了 global_header。",
                i, st->opt.path.c_str());
      streams_.clear();
      return false;
    }
    D2O_DEBUG("第 %zu 路码流 /%s：SPS %zu 字节，PPS %zu 字节", i,
              st->opt.path.c_str(), st->sps.size(), st->pps.size());
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
        *env_, st.queue, st.opt, st.sps, st.pps, &st.clients, &st.source));
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
      Nalu s;
      s.data = st.sps;
      s.pts = pts;
      s.keyframe = true;
      st.queue->push(std::move(s));

      Nalu p;
      p.data = st.pps;
      p.pts = pts;
      p.keyframe = true;
      st.queue->push(std::move(p));
    }
    st.queue->push(std::move(n));
  }

  // 只在全部入队之后唤醒一次。wake() 自己有门闩，连着调也无害，
  // 但一次就够 —— 醒来之后 deliver() 会把队列里的数据连着投完。
  if (auto* src = st.source.load(std::memory_order_acquire)) src->wake();
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
  return st ? st->sps.size() : 0;
}

std::size_t RtspServer::pps_size(std::size_t stream) const {
  Stream const* st = at(stream);
  return st ? st->pps.size() : 0;
}

}  // namespace d2o
