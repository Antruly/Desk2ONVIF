#include "common/adaptive.h"

#include <algorithm>

namespace d2o {

namespace {

/// 升档（开始丢帧）的门槛：这一秒丢了配置帧率的这么多分之一以上。
///
/// 取 1/8 而不是 1：偶尔丢一两帧是正常抖动（一次磁盘刷写、一次调度延迟），
/// 为它升档会让画面在"好好的"和"少了三分之一帧率"之间来回跳。而丢到 1/8
/// 以上就不是抖动了，是**持续**跟不上。
constexpr int kUpDutyDivisor = 8;

/// 降档（少丢一点）要求连续多少秒一帧不丢。
///
/// 10 秒是刻意的慢：升档是"立刻"的，两侧不对称才能防住边界抖动
/// （见 adaptive.h 的文件头）。慢一点的代价是"性能恢复了之后画质要 10 秒才
/// 回来"，而快一点的代价是画面帧率一秒一变 —— 后者在监控画面上明显得多。
constexpr int kDownDutyCleanSeconds = 10;

/// 判定 Overloaded 需要连续多少秒"丢到下限还跟不上"。
///
/// 2 秒：一秒是抖动的量级，而 Overloaded 会弹横幅提示用户降画质 ——
/// 弹一条"建议降低画质"然后两秒后自己消失，比不弹更让人困惑。
constexpr int kOverloadedSeconds = 2;

}  // namespace

char const* perf_state_name(PerfState s) {
  switch (s) {
    case PerfState::Ok:
      return "ok";
    case PerfState::Throttled:
      return "throttled";
    case PerfState::Overloaded:
      return "overloaded";
  }
  return "ok";
}

void AdaptiveController::configure(AdaptiveConfig const& cfg, int target_fps) {
  cfg_ = cfg;
  base_fps_ = std::max(1, target_fps);

  // duty 的上限由 min_fps 决定：duty = base_fps / min_fps 时帧率正好是下限。
  // 夹到至少 1 —— min_fps 配得比 base_fps 还大时（手改配置）不能得到 0，
  // 那会让 `tick % duty` 变成除零。
  int const min_fps = std::max(1, cfg_.min_fps);
  max_duty_ = std::max(1, base_fps_ / min_fps);

  duty_ = 1;
  state_ = PerfState::Ok;
  clean_seconds_ = 0;
  at_cap_bad_seconds_ = 0;

  // `off` 时把上限压成 1：状态机还在跑，但它不可能改变 duty。这样
  // "关掉自适应"和"自适应恰好没触发"在 stats 里是**看得出区别**的
  // （前者的 duty 永远是 1，后者的 duty 会动）。
  if (cfg_.mode != "throttle" && cfg_.mode != "auto") max_duty_ = 1;
}

void AdaptiveController::observe(std::uint64_t dropped, bool pool_starved) {
  std::uint64_t const up_threshold =
      static_cast<std::uint64_t>(std::max(1, base_fps_ / kUpDutyDivisor));

  // 池子见底**一律**算"这一秒跟不上"，不看数字。
  //
  // 理由是它常常不伴随任何队列丢弃：池子空的时候采集循环直接 `continue`
  // （连入队那一句都走不到），所以 `dropped` 可能是 0 而实际上编码器已经落后
  // 了整整一个池子。只看 dropped 的话，最严重的那个工况恰好是唯一看不见的。
  bool const bad = pool_starved || dropped >= up_threshold;

  if (bad) {
    clean_seconds_ = 0;
    if (duty_ < max_duty_) ++duty_;
  } else {
    ++clean_seconds_;
    // 只在**确实丢过帧**（duty > 1）时才需要恢复，否则每次干净的秒都在做
    // 无意义的归零。而且要求连续干净：见 kDownDutyCleanSeconds。
    if (duty_ > 1 && clean_seconds_ >= kDownDutyCleanSeconds) {
      --duty_;
      clean_seconds_ = 0;
    }
  }

  // Overloaded 的口径是「**已经在上限上**、还在跟不上」。
  //
  // 计数单开一个是必须的：`bad_seconds_` 是"连续跟不上的秒数"，而它在爬坡
  // 途中就涨到 2 了 —— 拿它判 Overloaded 的话，duty 刚到上限的那一刻就会被
  // 报成"丢到下限还跟不上"，而实际上还一秒都没在上限上待过。
  if (bad && duty_ >= max_duty_) {
    ++at_cap_bad_seconds_;
  } else {
    at_cap_bad_seconds_ = 0;
  }

  if (duty_ > 1 && at_cap_bad_seconds_ >= kOverloadedSeconds) {
    state_ = PerfState::Overloaded;
  } else if (duty_ > 1) {
    state_ = PerfState::Throttled;
  } else {
    state_ = PerfState::Ok;
  }
}

int AdaptiveController::target_fps() const {
  return std::max(1, base_fps_ / std::max(1, duty_));
}

}  // namespace d2o
