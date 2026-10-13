#include "audio/audio_mixer.h"

#include "common/log.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace d2o {

namespace {

/// 电平表的释放时间常数（秒）。
///
/// 峰值表要"**立刻上、慢慢下**"：只取每块的峰值的话，25 fps 下每 40 ms 跳一次，
/// 数字根本读不出来；而不做释放的话，一个瞬态过去之后读数会当场掉到 0。
/// 0.4 秒是"看得清跳变、又不至于把 1 秒前那一下一直挂在屏幕上"的量。
///
/// ⚠️ **衰减一律按时间算，不按调用次数算。** `fill()` 的调用频率就是视频帧率，
/// 而帧率是可改的（25 / 30 / 自适应降载也改拍数）。按次数衰减的话，把帧率从
/// 30 调到 15，电平表的释放时间就跟着慢一倍 —— 而"改个帧率电平表就变迟钝了"
/// 这种事没人会想到去查混音器。所以这里用 `target_frames / rate` 当 dt，
/// 它正好是这一块**音轨**代表的时间长度，与调用频率无关。
constexpr float kPeakReleaseTau = 0.4f;

/// RMS 的平滑时间常数（秒）。比峰值长一点 —— RMS 本身就已经是块内的平均了，
/// 再用短时常数平滑等于没平滑。
constexpr float kRmsTau = 0.3f;

/// 一路源的运行时状态。把"配了"和"起来了"分开记：配了但没起来要报 error，
/// 而"没配"是 Off，两者都不是故障。
struct Source {
  bool enabled = false;
  bool running = false;  ///< start() 真的成功了
  AudioFlow flow = AudioFlow::Render;
  bool loopback = false;
  char const* label = "";
  AudioMixer::SourceOptions opt;
  WasapiCapture cap;
  std::vector<float> scratch;
};

}  // namespace

struct AudioMixer::Impl {
  Options opt;
  Source system;
  Source mic;

  /// "现在是不是在混音"。**必须单独记一个位**，不能靠 `source.running`
  /// 反推：`stop()` 之后 `running` 变成 false 而 `opt.*.enabled` 还是 true
  /// （配置没变，只是停了），于是状态会报成 `error` —— 一个正常停掉的混音器
  /// 显示成"坏了"。而这个错只在"停过一次"之后才出现，典型的越用越像出故障。
  bool active = false;

  /// 多路同时开时各乘它。`1/√n`，两路时即 `1/√2 ≈ 0.707`（"各乘 0.7"）。
  /// 按**真正起来了**几路算，不是按"配了几路" —— 麦克风没起来的时候
  /// 系统声音不该白白低 3 dB。
  float headroom = 1.0f;

  /// 下面这几个给别的线程读（状态页 / stats），所以要有锁。
  /// **`fill()` 是本类里唯一写它们的调用**，而且它只在编码线程上跑。
  mutable std::mutex mu;
  AudioLevels lv;
  std::uint64_t frames_out = 0;
  std::uint64_t zero_filled = 0;
};

AudioMixer::AudioMixer() : impl_(new Impl()) {
  impl_->system.flow = AudioFlow::Render;
  impl_->system.loopback = true;
  impl_->system.label = "系统声音";
  impl_->mic.flow = AudioFlow::Capture;
  impl_->mic.loopback = false;
  impl_->mic.label = "麦克风";
}

AudioMixer::~AudioMixer() { stop(); }

bool AudioMixer::start(Options const& opt, std::string& err) {
  err.clear();
  stop();  // 幂等：重配（POST /api/reload）会先 start 再 start

  if (opt.channels != 2) {
    // 见头文件：采集层已经把每一路统一成立体声了，这里再来一套下混只会多出
    // 一条没人走的路径。与其支持它，不如当场说清楚。
    err = fmt("混音器只支持立体声（拿到 %d 声道）", opt.channels);
    return false;
  }

  impl_->opt = opt;
  impl_->system.enabled = opt.system.enabled;
  impl_->system.opt = opt.system;
  impl_->mic.enabled = opt.mic.enabled;
  impl_->mic.opt = opt.mic;

  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->lv = AudioLevels{};
    impl_->frames_out = 0;
    impl_->zero_filled = 0;
  }

  bool const any_enabled = opt.system.enabled || opt.mic.enabled;
  if (!any_enabled) {
    // **不是失败。** "音频关着"是一个完全正常的、被成功落实了的配置 ——
    // 把它做成错误会让调用方不得不去区分"音频没配"和"音频起不来"，
    // 而那正是 `State::Off` 存在的理由。
    impl_->headroom = 1.0f;
    impl_->active = false;
    D2O_INFO("音频：没有启用任何源，整条音频关着");
    return true;
  }

  int started = 0, attempted = 0;
  std::string first_err;

  auto bring_up = [&](Source& s) {
    if (!s.enabled) return;
    ++attempted;
    WasapiCapture::Options co;
    co.flow = s.flow;
    co.loopback = s.loopback;
    co.device_id = s.opt.device_id;
    co.rate = opt.rate;
    co.channels = opt.channels;
    co.ring_frames = opt.ring_frames;
    co.label = s.label;
    std::string e;
    if (s.cap.start(co, e)) {
      s.running = true;
      ++started;
    } else if (first_err.empty()) {
      first_err = e;
    }
  };
  bring_up(impl_->system);
  bring_up(impl_->mic);

  if (started == 0) {
    err = first_err.empty() ? std::string("没有任何一路音频起得来") : first_err;
    return false;
  }
  // 一路坏一路好：**保留好的那一路**。麦克风坏了不该把系统声音一起弄没，
  // 而用户仍然能从状态页上看见是哪一路坏了（`sources()` 里那一项的 state）。
  if (started < attempted) {
    D2O_WARN("音频：%d 路里有 %d 路没起来，其余照常；坏掉的那一路在状态页上是 error",
             attempted, attempted - started);
  }

  impl_->headroom = 1.0f / std::sqrt(static_cast<float>(started));
  impl_->active = true;
  std::string which;
  if (impl_->system.running) which = "系统声音";
  if (impl_->mic.running) which += which.empty() ? "麦克风" : " + 麦克风";
  D2O_INFO("音频：%d 路已开始（%s），混音时各乘 %.3f", started, which.c_str(),
           static_cast<double>(impl_->headroom));
  return true;
}

void AudioMixer::stop() {
  auto& im = *impl_;
  im.system.cap.stop();
  im.mic.cap.stop();
  im.system.running = false;
  im.mic.running = false;
  im.active = false;
}

int AudioMixer::fill(float* dst, int target_frames) {
  auto& im = *impl_;
  if (target_frames <= 0) return 0;

  int const ch = im.opt.channels;
  int const want = target_frames * ch;
  std::fill(dst, dst + want, 0.0f);

  // 至少有一路真的供上了数据的最长前缀。取 max 而不是 min：某一路只给了
  // 前 100 帧、另一路给满，那 100 帧之后**并非**没人管 —— 短的这一路只是
  // 欠载了，而"欠载"在这条链路上是正常的（见头文件）。
  int real = 0;

  auto mix_one = [&](Source& s) {
    if (!s.running) return;
    if (static_cast<int>(s.scratch.size()) < want) {
      s.scratch.resize(static_cast<std::size_t>(want));
    }
    std::fill(s.scratch.begin(), s.scratch.begin() + want, 0.0f);
    int const got = s.cap.pull_add(s.scratch.data(), target_frames);
    if (got > real) real = got;
    // 先按自己的增益缩放**再**求和 —— 顺序反了就没法给两路设不同的增益。
    float const g = s.opt.gain * im.headroom;
    for (int i = 0; i < want; ++i) dst[i] += s.scratch[static_cast<std::size_t>(i)] * g;
  };
  mix_one(im.system);
  mix_one(im.mic);

  // 钳位。headroom 已经把"两路都接近满幅"这个常见情况挡住了，这里兜住剩下的
  // （某一路上挂了失真插件、或者用户把增益拉到 4.0）。
  for (int i = 0; i < want; ++i) {
    float const v = dst[i];
    dst[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
  }

  // ---- 电平表。在**混完之后**算，也就是"真正进了音轨的东西"。
  // 分开算某一路的电平是另一个需求（"我的麦克风到底有没有在工作"），
  // 那个由 `sources()` 里的 audible_packets 回答 —— 它不依赖增益。
  float peak[2] = {0.0f, 0.0f};
  double sq[2] = {0.0, 0.0};
  for (int f = 0; f < target_frames; ++f) {
    for (int c = 0; c < ch; ++c) {
      float const v = dst[f * ch + c];
      float const a = std::fabs(v);
      if (a > peak[c]) peak[c] = a;
      sq[c] += static_cast<double>(v) * v;
    }
  }

  double const dt = static_cast<double>(target_frames) / im.opt.rate;
  float const release = static_cast<float>(std::exp(-dt / kPeakReleaseTau));
  float const rms_a = static_cast<float>(1.0 - std::exp(-dt / kRmsTau));

  AudioLevels lv;
  {
    std::lock_guard<std::mutex> lk(im.mu);
    lv = im.lv;
    // 峰值：立刻上、按时间慢慢下。
    lv.peak_l = std::max(peak[0], lv.peak_l * release);
    lv.peak_r = std::max(peak[1], lv.peak_r * release);
    // RMS：两侧对称的指数平均。
    float const br_l = static_cast<float>(std::sqrt(sq[0] / target_frames));
    float const br_r = static_cast<float>(std::sqrt(sq[1] / target_frames));
    lv.rms_l += (br_l - lv.rms_l) * rms_a;
    lv.rms_r += (br_r - lv.rms_r) * rms_a;

    im.lv = lv;
    im.frames_out += static_cast<std::uint64_t>(target_frames);
    if (real < target_frames) {
      // 补零的帧数。**系统安静时它本来就涨** —— 不是故障指示，见头文件。
      im.zero_filled += static_cast<std::uint64_t>(target_frames - real);
    }
  }
  return real;
}

AudioMixer::State AudioMixer::state() const {
  auto const& im = *impl_;
  // `active` 一起判：停掉之后 `opt` 里那些 enabled 还是 true（配置没变），
  // 只看它们会把一个正常停掉的混音器报成 error。
  if (!im.active) return State::Off;

  bool running = false, nosignal = false, broken = false;
  auto look = [&](Source const& s) {
    if (!s.enabled) return;
    if (!s.running) {
      broken = true;  // 起了但没起来（设备选错、格式不支持）
      return;
    }
    running = true;
    if (s.cap.state() == WasapiCapture::State::NoSignal) nosignal = true;
    if (s.cap.state() == WasapiCapture::State::Failed) broken = true;
  };
  look(im.system);
  look(im.mic);

  // 优先级：**坏 > 没有信号 > 好**。
  //
  // `broken` 排在 `running` 前面是刻意的：两路里坏了一路而另一路正常时，
  // 报 `ok` 会把"我的麦克风一直没声音"藏起来，而那是用户最需要看见的一条。
  // 具体是哪一路坏，`sources()` 里逐项写着。
  if (broken) return State::Error;
  if (nosignal) return State::NoSignal;
  if (running) return State::Ok;
  return State::Error;
}

std::string AudioMixer::state_text() const {
  switch (state()) {
    case State::Off: return "off";
    case State::Ok: return "ok";
    case State::NoSignal: return "nosignal";
    case State::Error: return "error";
  }
  return "error";
}

AudioLevels AudioMixer::levels() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->lv;
}

std::uint64_t AudioMixer::frames_out() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->frames_out;
}

std::uint64_t AudioMixer::zero_filled_frames() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->zero_filled;
}

std::vector<AudioMixer::SourceStats> AudioMixer::sources() const {
  auto const& im = *impl_;
  std::vector<SourceStats> out;

  auto build = [&](Source const& s) {
    SourceStats st;
    st.label = s.label;
    st.enabled = s.enabled && im.active;
    // 没启用、或者整个混音器已经停了 —— 两种都是"这一路没在工作"，都一样报 off。
    if (!st.enabled) {
      st.state = "off";
      return st;
    }
    if (!s.running) {
      st.state = "error";  // 配了但没起来（设备选错、格式不支持）
      return st;
    }
    switch (s.cap.state()) {
      case WasapiCapture::State::Ok: st.state = "ok"; break;
      case WasapiCapture::State::NoSignal: st.state = "nosignal"; break;
      case WasapiCapture::State::Failed: st.state = "error"; break;
      case WasapiCapture::State::Idle: st.state = "off"; break;
    }
    st.pacing = s.cap.pacing_text();
    st.device = s.cap.status_text();
    st.packets = s.cap.packets();
    st.audible_packets = s.cap.audible_packets();
    st.dropped_frames = s.cap.dropped_frames();
    st.reopen_count = s.cap.reopen_count();
    st.buffered_frames = s.cap.available();
    return st;
  };

  // **两路各一项，永远两项**（关了的那路也在，state 是 "off"）：
  // 数组长度稳定，前端不用去猜"这一项是不存在还是没用"。
  out.push_back(build(im.system));
  out.push_back(build(im.mic));
  return out;
}

}  // namespace d2o
