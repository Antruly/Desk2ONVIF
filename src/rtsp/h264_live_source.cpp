#include "rtsp/h264_live_source.h"

#include "common/log.h"
#include "rtsp/nalu_queue.h"

// gettimeofday() 在 Windows 上是 Live555 自己实现的（groupsock/GroupsockHelper.cpp），
// 声明就在这个头里。别去用 MinGW 的，那不是同一个。
#include "GroupsockHelper.hh"

#include <cstring>

namespace d2o {

H264LiveSource* H264LiveSource::createNew(UsageEnvironment& env,
                                          std::shared_ptr<NaluQueue> queue,
                                          unsigned fps) {
  return new H264LiveSource(env, std::move(queue), fps);
}

H264LiveSource::H264LiveSource(UsageEnvironment& env,
                               std::shared_ptr<NaluQueue> queue, unsigned fps)
    : FramedSource(env), queue_(std::move(queue)), fps_(fps ? fps : 30) {
  // 全 Live555 里唯一允许跨线程调用的入口。用尽 32 个触发器时返回 0，
  // 这个必须判 —— 否则 wake() 会往一个不存在的触发器上打事件，
  // 结果是"偶尔卡住不推流"，而且完全没有报错。
  trigger_ =
      envir().taskScheduler().createEventTrigger(&H264LiveSource::on_trigger);
  if (trigger_ == 0) {
    envir() << "H264LiveSource: createEventTrigger 失败（32 个触发器已用尽）——"
               "跨线程唤醒不可用\n";
  }
  ::gettimeofday(&anchor_, nullptr);
}

H264LiveSource::~H264LiveSource() {
  if (trigger_ != 0) {
    envir().taskScheduler().deleteEventTrigger(trigger_);
    trigger_ = 0;
  }
}

void H264LiveSource::wake() {
  if (trigger_ == 0) return;
  // 门闩：只在 false→true 的那一次真的调 triggerEvent。
  // 契约见头文件里的说明。后面进来的数据已经在队列里了，处理时会被一起取走。
  if (pending_.exchange(true, std::memory_order_acq_rel)) return;
  envir().taskScheduler().triggerEvent(trigger_, this);
}

void H264LiveSource::on_trigger(void* clientData) {
  auto* self = static_cast<H264LiveSource*>(clientData);
  // 门闩必须在**开头**清，不能在结尾 —— 这一条是看调度器的实现定下来的：
  // BasicTaskScheduler.cpp:191-198 是"先清自己的标志位，再调 handler"。
  // 所以 handler 执行期间到达的 triggerEvent 会在下一个 tick 被正常派发；
  // 如果门闩留到结尾才清，那次派发进来会看到 pending_ 还是 true，
  // 而这里又不区分"谁触发的"，结果就是这次唤醒被整个吞掉。
  self->pending_.store(false, std::memory_order_release);
  self->deliver();
}

void H264LiveSource::doGetNextFrame() {
  // 拉模型：只有在 sink 要数据时（调度线程）才会走到这里。
  // 队列空就**什么都不做地返回**，之后靠 wake() 把我们叫回来。
  // 这里绝不能调 afterGetting() —— 那等于投递了一帧不存在的数据。
  deliver();
}

void H264LiveSource::deliver() {
  if (!isCurrentlyAwaitingData()) return;  // 没人在等，别投

  Nalu n;
  if (!queue_->pop(n)) return;  // 暂时没数据，等唤醒

  auto const nalu_size = static_cast<unsigned>(n.data.size());
  if (nalu_size > fMaxSize) {
    // 缓冲不够只能截断。正常不该发生：上游是 H264or5Fragmenter，它的输入缓冲
    // 就是 OutPacketBuffer::maxSize（我们在 RtspServer::start 里调大了）。
    // 真发生了要能看见 —— 症状是"关键帧偶尔破一下"，不看日志根本联想不到。
    fNumTruncatedBytes = nalu_size - fMaxSize;
    fFrameSize = fMaxSize;
    D2O_WARN("NALU %u 字节超过下游缓冲 %u 字节，被截断（画面会破）", nalu_size,
             fMaxSize);
  } else {
    fNumTruncatedBytes = 0;
    fFrameSize = nalu_size;
  }
  std::memmove(fTo, n.data.data(), fFrameSize);

  // 呈现时间从编码器帧号换算，而不是取"投递时刻的挂钟时间"。
  // 队列里可能压着几百毫秒的数据，用挂钟时间会把它们全挤到同一瞬间，
  // 客户端那边看到的就是一顿一顿的（而且 RTCP 的 SR 也会算错）。
  if (anchor_pts_ < 0) anchor_pts_ = n.pts;
  std::int64_t const base_us =
      static_cast<std::int64_t>(anchor_.tv_sec) * 1'000'000 + anchor_.tv_usec;
  std::int64_t us = base_us + (n.pts - anchor_pts_) * 1'000'000 /
                                 static_cast<std::int64_t>(fps_);
  if (us < 0) us = 0;
  fPresentationTime.tv_sec = static_cast<long>(us / 1'000'000);
  fPresentationTime.tv_usec = static_cast<long>(us % 1'000'000);
  fDurationInMicroseconds = 1'000'000u / fps_;

  FramedSource::afterGetting(this);
}

}  // namespace d2o
