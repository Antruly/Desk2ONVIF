#include "rtsp/aac_live_source.h"

#include "common/log.h"
#include "rtsp/au_queue.h"

// gettimeofday() 在 Windows 上是 Live555 自己实现的（groupsock/GroupsockHelper.cpp），
// 声明就在这个头里。别去用 MinGW 的，那不是同一个。
#include "GroupsockHelper.hh"

#include <cstring>

namespace d2o {

AacLiveSource* AacLiveSource::createNew(UsageEnvironment& env,
                                        std::shared_ptr<AuQueue> queue,
                                        unsigned sample_rate,
                                        unsigned samples_per_au) {
  return new AacLiveSource(env, std::move(queue), sample_rate, samples_per_au);
}

AacLiveSource::AacLiveSource(UsageEnvironment& env,
                             std::shared_ptr<AuQueue> queue,
                             unsigned sample_rate, unsigned samples_per_au)
    : FramedSource(env),
      queue_(std::move(queue)),
      sample_rate_(sample_rate ? sample_rate : 48000),
      samples_per_au_(samples_per_au ? samples_per_au : 1024) {
  // 全 Live555 里唯一允许跨线程调用的入口，用尽 32 个触发器时返回 0，
  // 必须判 —— 否则 wake() 会往一个不存在的触发器上打事件，表现为"偶尔不出声"，
  // 而且完全没有报错。视频那一路会先用掉一个，音频这一个通常还剩。
  trigger_ =
      envir().taskScheduler().createEventTrigger(&AacLiveSource::on_trigger);
  if (trigger_ == 0) {
    envir() << "AacLiveSource: createEventTrigger 失败（32 个触发器已用尽）——"
               "跨线程唤醒不可用\n";
  }
  ::gettimeofday(&anchor_, nullptr);
}

AacLiveSource::~AacLiveSource() {
  if (trigger_ != 0) {
    envir().taskScheduler().deleteEventTrigger(trigger_);
    trigger_ = 0;
  }
}

void AacLiveSource::wake() {
  if (trigger_ == 0) return;
  // 门闩：只在 false→true 的那一次真的调 triggerEvent。契约见头文件。
  if (pending_.exchange(true, std::memory_order_acq_rel)) return;
  envir().taskScheduler().triggerEvent(trigger_, this);
}

void AacLiveSource::on_trigger(void* clientData) {
  auto* self = static_cast<AacLiveSource*>(clientData);
  // 门闩必须在**开头**清，不能在结尾：BasicTaskScheduler 是"先清自己的标志位、
  // 再调 handler"（BasicTaskScheduler.cpp:191-198），所以 handler 执行期间到达的
  // triggerEvent 会在下一个 tick 被正常派发。留到结尾才清的话那次派发会被整个吞掉。
  self->pending_.store(false, std::memory_order_release);
  self->deliver();
}

void AacLiveSource::doGetNextFrame() {
  // 拉模型：队列空就什么都不做地返回，之后靠 wake() 叫回来。
  // 绝不能在这里调 afterGetting() —— 那等于投递了一帧不存在的数据。
  deliver();
}

void AacLiveSource::deliver() {
  if (!isCurrentlyAwaitingData()) return;  // 没人在等，别投

  Au au;
  if (!queue_->pop(au)) return;  // 暂时没数据，等唤醒

  auto const au_size = static_cast<unsigned>(au.data.size());
  if (au_size > fMaxSize) {
    // 缓冲不够只能截断。正常不该发生：上游是 MPEG4GenericRTPSink 的输入缓冲，
    // 也就是我们在 RtspServer::start 里抬到 2 MB 的那个。真发生了要能看见 ——
    // 症状是"偶尔一声咔"，不看日志联想不到是缓冲。
    fNumTruncatedBytes = au_size - fMaxSize;
    fFrameSize = fMaxSize;
    D2O_WARN("AU %u 字节超过下游缓冲 %u 字节，被截断（声音会咔一下）", au_size,
             fMaxSize);
  } else {
    fNumTruncatedBytes = 0;
    fFrameSize = au_size;
  }
  std::memmove(fTo, au.data.data(), fFrameSize);

  // 呈现时间从编码器的样本号换算，不是取"投递时刻的挂钟时间"。
  // 差值语义：anchor_pts_ 是**第一个投递出去的** AU 的 pts，实测是 −1024
  // （编码器把首包整体前移了启动填充），所以第一个 AU 落在 anchor 那一瞬间，
  // 后面的每个 AU 比它晚 (Δ样本 / 48000) 秒。负 pts 因此是无害的。
  //
  // 判"定没定过锚"用的是 anchored_ 而不是 `anchor_pts_ < 0` —— 见头文件里的
  // 说明：首包本来就是负的，拿负数当哨兵的话**前两个 AU 会并到同一时刻**
  // （首个把锚点定到 −1024，下一个 pts = 0 时哨兵仍然成立）。那 21 ms 的错位
  // 在端到端里量不出来，只有 tests\aac_live_source_check.cpp 会红。
  if (!anchored_) {
    anchored_ = true;
    anchor_pts_ = au.pts;
  }
  std::int64_t const base_us =
      static_cast<std::int64_t>(anchor_.tv_sec) * 1'000'000 + anchor_.tv_usec;
  std::int64_t us = base_us + (au.pts - anchor_pts_) * 1'000'000 /
                                 static_cast<std::int64_t>(sample_rate_);
  if (us < 0) us = 0;  // 理论上到不了，但时间戳落在 1970 之前会让 RTCP 发疯
  fPresentationTime.tv_sec = static_cast<long>(us / 1'000'000);
  fPresentationTime.tv_usec = static_cast<long>(us % 1'000'000);
  fDurationInMicroseconds = static_cast<unsigned>(
      static_cast<std::int64_t>(samples_per_au_) * 1'000'000 / sample_rate_);

  FramedSource::afterGetting(this);
}

}  // namespace d2o
