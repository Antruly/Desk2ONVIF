#include "audio/wasapi_capture.h"

#include <windows.h>

#include <mmreg.h>   // WAVE_FORMAT_* / WAVEFORMATEXTENSIBLE（WIN32_LEAN_AND_MEAN
                     // 把 mmsystem.h 挡在外面了，这里显式要）
#include <audioclient.h>
#include <mmdeviceapi.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "common/com_ptr.h"
#include "common/log.h"   // d2o::fmt
#include "common/text.h"

namespace d2o {

std::uint64_t qpc_now() {
  LARGE_INTEGER v;
  ::QueryPerformanceCounter(&v);
  return static_cast<std::uint64_t>(v.QuadPart);
}

std::uint64_t qpc_frequency() {
  // 一台机器上恒定，没必要每次问一次系统调用。
  static std::uint64_t const freq = [] {
    LARGE_INTEGER v;
    ::QueryPerformanceFrequency(&v);
    return static_cast<std::uint64_t>(v.QuadPart ? v.QuadPart : 1);
  }();
  return freq;
}

namespace {

/// 事件连续多少次不来就判"这条路的事件是死的"。200 ms × 15 ≈ 3 秒。
/// 选 3 秒是因为**环回在系统不出声时也会持续给包**（给的是带 SILENT 标志的静音），
/// 所以"3 秒一个包都没有"不可能是"没声音"，只能是事件没派发。
constexpr int kIdleWaitsBeforePolling = 15;

std::string hr_text(HRESULT hr) {
  return fmt("0x%08lX", static_cast<unsigned long>(hr));
}

/// 采集线程睡着的时候也要能退出。用小块睡 + 查停止位，而不是一次 sleep_for 到底。
void sleep_interruptible(std::atomic<bool> const& stopping, int ms) {
  int left = ms;
  while (left > 0 && !stopping.load(std::memory_order_relaxed)) {
    int const chunk = left > 50 ? 50 : left;
    std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
    left -= chunk;
  }
}

/// 把 `WAVEFORMATEX` 的采样格式映射成 FFmpeg 的。
///
/// **EXTENSIBLE 的子格式只看 `Data1`。** 那三个子格式 GUID 只在前 4 字节上不同
/// （PCM = 1、IEEE_FLOAT = 3，剩下的 12 字节全是 `0000-0010-8000-00aa00389b71`），
/// 而 `KSDATAFORMAT_SUBTYPE_*` 这两个符号在 MinGW 下和 MMDevice 那几个 GUID 是
/// 同一类问题（`DEFINE_GUIDSTRUCT` 展开成裸声明，库里没有）—— 见陷阱 66。
/// 读一个结构体字段既不用符号也不用抄字节。认不出来的格式**如实报错**，
/// 不要猜：猜错的后果是一条从头到尾的噪音，而不是一句能看懂的话。
bool map_sample_format(WAVEFORMATEX const* wfx, AVSampleFormat& out, std::string& err) {
  WORD tag = wfx->wFormatTag;
  if (tag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
    auto const* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE const*>(wfx);
    switch (ext->SubFormat.Data1) {
      case 1: tag = WAVE_FORMAT_PCM; break;
      case 3: tag = WAVE_FORMAT_IEEE_FLOAT; break;
      default:
        err = fmt("设备的共享模式格式不认识（EXTENSIBLE 子格式 Data1=%lu）",
                  static_cast<unsigned long>(ext->SubFormat.Data1));
        return false;
    }
  }
  if (tag == WAVE_FORMAT_IEEE_FLOAT) {
    if (wfx->wBitsPerSample != 32) {
      err = fmt("device reports %u-bit float, only 32-bit float is handled",
                wfx->wBitsPerSample);
      return false;
    }
    out = AV_SAMPLE_FMT_FLT;
    return true;
  }
  if (tag == WAVE_FORMAT_PCM) {
    if (wfx->wBitsPerSample == 16) {
      out = AV_SAMPLE_FMT_S16;
      return true;
    }
    if (wfx->wBitsPerSample == 32) {
      // 24-in-32 也走这一支：容器是 4 字节，低位补零，swr 按 S32 读是对的。
      out = AV_SAMPLE_FMT_S32;
      return true;
    }
    if (wfx->wBitsPerSample == 24) {
      // 打包 24 位（3 字节一样本）swr 不认。真遇到再说，先如实报错。
      err = "设备是打包 24 位 PCM，这一路暂不支持";
      return false;
    }
    err = fmt("设备是 %u 位 PCM，这一路只支持 16/32 位", wfx->wBitsPerSample);
    return false;
  }
  err = fmt("设备的共享模式格式不支持（wFormatTag=0x%04X，%u 位，%u 声道）",
            static_cast<unsigned>(tag), static_cast<unsigned>(wfx->wBitsPerSample),
            static_cast<unsigned>(wfx->nChannels));
  return false;
}

/// 交错立体声 float32 的环形缓冲。
///
/// 单生产者（采集线程）单消费者（编码线程），一把锁就够：两边都是每秒几十次，
/// 而锁里干的只是 memcpy。**不做无锁**：那种代码的正确性要靠内存序推理，
/// 而这里的收益是零。
///
/// 满了**丢最旧的**：和视频那条路同一个哲学 —— 宁可丢一段，绝不排队积压。
/// 积压的后果是声音永远慢几秒，而且再也追不回来。
class FloatRing {
 public:
  void reset(int capacity_frames) {
    cap_ = capacity_frames > 0 ? capacity_frames : 1;
    buf_.assign(static_cast<std::size_t>(cap_) * 2, 0.0f);
    read_ = 0;
    size_ = 0;
    dropped_ = 0;
  }

  int size() const { return size_; }
  std::uint64_t dropped() const { return dropped_; }

  void push(float const* src, int frames) {
    if (frames <= 0) return;
    if (frames >= cap_) {
      // 一次就超过整个环（长时间没被消费）：只留最后 cap_ 帧，其余算丢掉。
      src += static_cast<std::size_t>(frames - cap_) * 2;
      dropped_ += static_cast<std::uint64_t>(frames - cap_);
      frames = cap_;
      read_ = 0;
      size_ = cap_;
      std::memcpy(buf_.data(), src, static_cast<std::size_t>(frames) * 2 * sizeof(float));
      return;
    }
    int const overflow = size_ + frames - cap_;
    if (overflow > 0) {
      read_ = (read_ + overflow) % cap_;
      size_ -= overflow;
      dropped_ += static_cast<std::uint64_t>(overflow);
    }
    int const write = (read_ + size_) % cap_;
    int const first = std::min(frames, cap_ - write);
    std::memcpy(&buf_[static_cast<std::size_t>(write) * 2], src,
                static_cast<std::size_t>(first) * 2 * sizeof(float));
    if (first < frames) {
      std::memcpy(buf_.data(), src + static_cast<std::size_t>(first) * 2,
                  static_cast<std::size_t>(frames - first) * 2 * sizeof(float));
    }
    size_ += frames;
  }

  int pop_add(float* dst, int frames) {
    int const n = std::min(frames, size_);
    if (n <= 0) return 0;
    int const first = std::min(n, cap_ - read_);
    float const* p = &buf_[static_cast<std::size_t>(read_) * 2];
    for (std::size_t i = 0; i < static_cast<std::size_t>(first) * 2; ++i) dst[i] += p[i];
    if (first < n) {
      float const* q = buf_.data();
      for (std::size_t i = 0; i < static_cast<std::size_t>(n - first) * 2; ++i) {
        dst[static_cast<std::size_t>(first) * 2 + i] += q[i];
      }
    }
    read_ = (read_ + n) % cap_;
    size_ -= n;
    return n;
  }

 private:
  std::vector<float> buf_;
  int cap_ = 0;
  int read_ = 0;
  int size_ = 0;
  std::uint64_t dropped_ = 0;
};

}  // namespace

struct WasapiCapture::Impl {
  Options opt;

  std::thread thread;
  std::atomic<bool> stopping{false};

  // —— 下面这一组只有采集线程碰 ——
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IMMDevice> device;
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  HANDLE event = nullptr;
  SwrContext* swr = nullptr;
  std::string device_name;      ///< 实际打开的是哪个设备（设备 id 空时它才有意义）
  int in_rate = 0;
  int in_bytes_per_frame = 0;
  int out_cap_frames = 0;
  std::vector<float> conv;      ///< swr 的输出缓冲（交错立体声 float）
  std::vector<std::uint8_t> zeros;  ///< SILENT 时拿来当"全零输入"
  bool opened = false;
  bool use_event = false;       ///< 走的是事件驱动还是轮询
  int pace_ms = 10;             ///< 轮询路的节奏：半个引擎周期
  int idle_waits = 0;
  std::uint64_t buffer_frames = 0;  ///< 引擎给的一块有多大（选轮询节奏用）
  std::uint64_t first_qpc = 0;
  std::uint64_t delivered_frames = 0;
  bool rate_checked = false;

  // —— 下面这一组跨线程 ——
  mutable std::mutex mu;
  FloatRing ring;
  std::atomic<int> state{static_cast<int>(State::Idle)};
  std::string text;
  std::string pacing;
  std::atomic<std::uint64_t> reopen_count{0};
  std::atomic<std::uint64_t> discontinuity_count{0};
  std::atomic<std::uint64_t> audible_packets{0};
  std::atomic<std::uint64_t> packets{0};
  double rate_ratio = 0.0;      ///< 设备时钟 / QPC，只有 rate_checked 之后才有意义

  void set_state(State s, std::string t) {
    state.store(static_cast<int>(s), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mu);
    text = std::move(t);
  }
  void set_pacing(std::string p) {
    std::lock_guard<std::mutex> lk(mu);
    pacing = std::move(p);
  }
  std::string get_text() const {
    std::lock_guard<std::mutex> lk(mu);
    return text;
  }
  std::string get_pacing() const {
    std::lock_guard<std::mutex> lk(mu);
    return pacing;
  }
};

WasapiCapture::WasapiCapture() : impl_(new Impl) {}

WasapiCapture::~WasapiCapture() { stop(); }

// ---------------------------------------------------------------------------
// 打开 / 关闭（只在采集线程上跑）
// ---------------------------------------------------------------------------

namespace {

/// 打开过程中的中途失败：把已经建出来的东西收干净。
/// 不能顺手调 `WasapiCapture::stop()` 那一套 —— 那是给外部用的。
void release_stream(ComPtr<IAudioCaptureClient>& capture, ComPtr<IAudioClient>& client,
                    SwrContext*& swr, HANDLE& ev) {
  capture.reset();
  if (client) client->Stop();
  client.reset();
  if (swr) swr_free(&swr);
  if (ev) {
    ::CloseHandle(ev);
    ev = nullptr;
  }
}

}  // namespace

bool WasapiCapture::start(Options const& opt, std::string& err) {
  err.clear();
  if (impl_->thread.joinable()) {
    err = "这一路已经在跑了";
    return false;
  }
  impl_->opt = opt;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->ring.reset(opt.ring_frames);
    impl_->text = "正在启动";
    impl_->pacing.clear();
  }
  impl_->stopping.store(false, std::memory_order_relaxed);

  std::promise<std::string> init;
  std::future<std::string> result = init.get_future();
  impl_->thread = std::thread([this, p = std::move(init)]() mutable { thread_main(std::move(p)); });
  err = result.get();

  if (!err.empty()) {
    impl_->stopping.store(true, std::memory_order_relaxed);
    impl_->thread.join();
    impl_->set_state(State::Failed, err);
    return false;
  }
  return true;
}

void WasapiCapture::stop() {
  if (!impl_) return;
  impl_->stopping.store(true, std::memory_order_relaxed);
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->set_state(State::Idle, "已停止");
  impl_->set_pacing(std::string());
}

// 采集线程的本体。声明成 Impl 的成员函数不方便（要访问 WasapiCapture 的一堆
// 私有成员），所以写成一个自由函数 + 友元式的私有成员。这里用最直白的写法：
// 把线程体也放成类里没有的静态函数，靠 Impl 指针操作。
void WasapiCapture::thread_main(std::promise<std::string> init) {
  ComApartment com;
  if (!com.ok()) {
    std::string const e = "这个线程的 COM 起不来（CoInitializeEx " +
                          hr_text(static_cast<HRESULT>(com.hr())) + "）";
    init.set_value(e);
    impl_->set_state(State::Failed, e);
    return;
  }

  std::string err;
  if (!open_device(err)) {
    init.set_value(err);
    impl_->set_state(State::Failed, err);
    return;
  }
  init.set_value(std::string());

  int backoff_ms = 500;
  while (!impl_->stopping.load(std::memory_order_relaxed)) {
    if (!impl_->opened) {
      // 设备没了。退避重开 —— 期间**照常输出静音**，编码线程完全不知道出过事。
      // 重试不设上限：用户把耳机插回去的那一刻就该自己接上，而不是要求他重启程序。
      sleep_interruptible(impl_->stopping, backoff_ms);
      if (impl_->stopping.load(std::memory_order_relaxed)) break;
      backoff_ms = std::min(backoff_ms * 2, 5000);
      std::string e;
      if (open_device(e)) {
        backoff_ms = 500;
        impl_->reopen_count.fetch_add(1, std::memory_order_relaxed);
        D2O_INFO("%s：设备回来了（%s），继续采集", impl_->opt.label.c_str(),
                 impl_->device_name.c_str());
      } else {
        impl_->set_state(State::NoSignal, e);
      }
      continue;
    }
    pump();
  }
  close_device();
}

bool WasapiCapture::open_device(std::string& err) {
  release_stream(impl_->capture, impl_->client, impl_->swr, impl_->event);
  impl_->enumerator.reset();
  impl_->device.reset();
  impl_->opened = false;

  HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(impl_->enumerator.put()));
  if (FAILED(hr)) {
    err = "拿不到设备枚举器（CoCreateInstance " + hr_text(hr) + "）";
    return false;
  }

  EDataFlow const flow = (impl_->opt.flow == AudioFlow::Render) ? eRender : eCapture;
  if (impl_->opt.device_id.empty()) {
    hr = impl_->enumerator->GetDefaultAudioEndpoint(flow, eConsole, impl_->device.put());
    if (FAILED(hr)) {
      err = "这台机器上没有默认的" +
            std::string(impl_->opt.flow == AudioFlow::Render ? "播放" : "录音") +
            "设备（" + hr_text(hr) + "）";
      return false;
    }
  } else {
    std::wstring const wid = from_utf8(impl_->opt.device_id);
    hr = impl_->enumerator->GetDevice(wid.c_str(), impl_->device.put());
    if (FAILED(hr)) {
      err = "配置里指定的设备已经不在了（" + hr_text(hr) + "），可以改用默认设备或重新挑一个";
      return false;
    }
  }

  {  // 记下实际打开的是谁 —— device_id 为空时用户根本不知道选中的是哪一个
    LPWSTR wid = nullptr;
    if (SUCCEEDED(impl_->device->GetId(&wid)) && wid) {
      impl_->device_name = endpoint_name_by_id(impl_->opt.flow, to_utf8(wid, -1));
      ::CoTaskMemFree(wid);
    }
    if (impl_->device_name.empty()) impl_->device_name = "（未命名设备）";
  }

  hr = impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(impl_->client.put()));
  if (FAILED(hr)) {
    err = "打不开这个设备的音频客户端（Activate " + hr_text(hr) + "）";
    return false;
  }

  WAVEFORMATEX* wfx = nullptr;
  hr = impl_->client->GetMixFormat(&wfx);
  if (FAILED(hr) || !wfx) {
    err = "问不到设备的共享模式格式（GetMixFormat " + hr_text(hr) + "）";
    return false;
  }
  // 用一个 RAII 把它收干净：下面每一条 return false 的路都要 CoTaskMemFree。
  struct MixFormatGuard {
    WAVEFORMATEX* p;
    ~MixFormatGuard() { if (p) ::CoTaskMemFree(p); }
  } guard{wfx};

  AVSampleFormat in_fmt = AV_SAMPLE_FMT_NONE;
  if (!map_sample_format(wfx, in_fmt, err)) return false;

  // 环回只对渲染端点成立。用户传了 loopback 但选的是麦克风时**不当错误**，
  // 只是忽略它 —— 麦克风本来就不需要环回，报错只会让人以为配置错了。
  bool loopback = impl_->opt.loopback && impl_->opt.flow == AudioFlow::Render;

  DWORD base = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
  // 先试事件驱动，失败退回轮询（陷阱 67）。**两条路都要留痕** ——
  // 不然"这台机器上音轨是死的"这件事没有任何线索可查。
  DWORD flags = base | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, wfx, nullptr);
  if (FAILED(hr)) {
    D2O_WARN("%s：事件驱动初始化失败（%s），退回轮询", impl_->opt.label.c_str(),
             hr_text(hr).c_str());
    HRESULT const first = hr;
    flags = base;
    hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, wfx, nullptr);
    if (FAILED(hr)) {
      err = "音频客户端初始化失败（事件驱动 " + hr_text(first) + "，轮询 " + hr_text(hr) + "）";
      return false;
    }
  }
  impl_->use_event = (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;

  if (impl_->use_event) {
    impl_->event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->event) {
      err = "建不出音频事件的句柄";
      return false;
    }
    hr = impl_->client->SetEventHandle(impl_->event);
    if (FAILED(hr)) {
      // 走到这里说明 Initialize 认了 EVENTCALLBACK 但 SetEventHandle 不认 ——
      // 只能整个重来一遍走轮询，不能在已经 Initialize 过的 client 上改标志。
      D2O_WARN("%s：SetEventHandle 失败（%s），这一轮先按轮询跑", impl_->opt.label.c_str(),
               hr_text(hr).c_str());
      release_stream(impl_->capture, impl_->client, impl_->swr, impl_->event);
      hr = impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(impl_->client.put()));
      if (FAILED(hr)) {
        err = "重新打开音频客户端失败（" + hr_text(hr) + "）";
        return false;
      }
      hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, base, 0, 0, wfx, nullptr);
      if (FAILED(hr)) {
        err = "退回轮询之后仍然初始化失败（" + hr_text(hr) + "）";
        return false;
      }
      impl_->use_event = false;
    }
  }

  UINT32 buf_frames = 0;
  hr = impl_->client->GetBufferSize(&buf_frames);
  if (FAILED(hr)) {
    err = "问不到缓冲区大小（GetBufferSize " + hr_text(hr) + "）";
    return false;
  }
  impl_->buffer_frames = buf_frames;
  // 轮询的节奏取半个引擎周期：比一整个快一倍，但不会空转。
  // 引擎周期 = buf_frames / 采样率，所以半周期 = buf_frames * 1000 / (2 * rate) ms。
  impl_->pace_ms = static_cast<int>(std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(buf_frames) * 1000 / (2ull * impl_->opt.rate)));
  // 事件驱动的等待上限：取 4 个引擎周期，下限 50 ms。事件丢了也能靠这个兜住。
  impl_->idle_waits = 0;

  hr = impl_->client->GetService(__uuidof(IAudioCaptureClient),
                                 reinterpret_cast<void**>(impl_->capture.put()));
  if (FAILED(hr)) {
    err = "拿不到采集接口（GetService " + hr_text(hr) + "）";
    return false;
  }

  // swr：设备的共享模式格式 → 我们要的 48 kHz 立体声 float。
  {
    AVChannelLayout out_layout;
    ::av_channel_layout_default(&out_layout, impl_->opt.channels);
    AVChannelLayout in_layout;
    std::uint32_t mask = 0;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
      mask = reinterpret_cast<WAVEFORMATEXTENSIBLE const*>(wfx)->dwChannelMask;
    }
    // WASAPI 的声道掩码和 FFmpeg 的 AV_CH_* 是同一套位序，可以直接当布局用。
    if (mask == 0 || ::av_channel_layout_from_mask(&in_layout, mask) < 0) {
      ::av_channel_layout_default(&in_layout, static_cast<int>(wfx->nChannels));
    }
    int const r = ::swr_alloc_set_opts2(
        &impl_->swr, &out_layout, AV_SAMPLE_FMT_FLT, impl_->opt.rate, &in_layout, in_fmt,
        static_cast<int>(wfx->nSamplesPerSec), 0, nullptr);
    ::av_channel_layout_uninit(&out_layout);
    ::av_channel_layout_uninit(&in_layout);
    if (r < 0 || !impl_->swr || ::swr_init(impl_->swr) < 0) {
      err = "建不出音频重采样器（设备 " + fmt("%u Hz / %u 声道", wfx->nSamplesPerSec,
                                             wfx->nChannels) + "）";
      return false;
    }
  }
  impl_->in_rate = static_cast<int>(wfx->nSamplesPerSec);
  impl_->in_bytes_per_frame = static_cast<int>(wfx->nBlockAlign);
  if (impl_->in_rate <= 0 || impl_->in_bytes_per_frame <= 0) {
    err = "设备报的格式有问题（采样率或块对齐是 0）";
    return false;
  }

  hr = impl_->client->Start();
  if (FAILED(hr)) {
    err = "启动音频流失败（Start " + hr_text(hr) + "）";
    return false;
  }

  impl_->opened = true;
  impl_->set_pacing(impl_->use_event ? "事件驱动" : "轮询");
  impl_->set_state(State::Ok, fmt("正在采集（%s，%s）", impl_->device_name.c_str(),
                                 impl_->use_event ? "事件驱动" : "轮询"));
  D2O_INFO("%s：已开始采集 —— 设备「%s」，%u Hz %u 声道，%s，缓冲 %llu 帧",
           impl_->opt.label.c_str(), impl_->device_name.c_str(), wfx->nSamplesPerSec,
           wfx->nChannels, impl_->use_event ? "事件驱动" : "轮询",
           static_cast<unsigned long long>(buf_frames));
  return true;
}

void WasapiCapture::close_device() {
  release_stream(impl_->capture, impl_->client, impl_->swr, impl_->event);
  impl_->enumerator.reset();
  impl_->device.reset();
  impl_->opened = false;
}

// ---------------------------------------------------------------------------
// 抽水
// ---------------------------------------------------------------------------

void WasapiCapture::pump() {
  if (impl_->use_event) {
    DWORD const w = ::WaitForSingleObject(impl_->event, 200);
    if (w == WAIT_FAILED) {
      D2O_WARN("%s：等音频事件出错（%lu），改用轮询", impl_->opt.label.c_str(),
               static_cast<unsigned long>(::GetLastError()));
      impl_->use_event = false;
      impl_->set_pacing("轮询（事件报错后）");
    } else if (w == WAIT_TIMEOUT) {
      // 事件等不到有**两种完全不同的原因**，而它们的处置一样、说法不一样
      // （实测数据见 tests\wasapi_check.cpp）：
      //
      //   * **环回在系统不出声时一个包都不来。** 实测：没声音的 2 秒是
      //     **0 包 0 帧** —— 不是"来了带 SILENT 标志的静音"，是播放引擎空闲就
      //     根本不产数据。所以对环回来说"等不到事件"是**常态**，报 WARN 就是
      //     每天在喊狼来了。
      //   * **麦克风永远按采样率出包**（内容全零时也出）。实测稳定 100.0% 实时，
      //     事件不该不来；这台机器上不来就是真有事。
      //
      // 两种情况都改用轮询节奏：真坏了能救回来，本来就没数据也只是白等一圈。
      // 另一条独立的理由是 LOOPBACK+EVENTCALLBACK 在有些驱动上**初始化成功但
      // 事件永不触发** —— 不兜住的话得到的是一条永远静止的音轨，而一切都"正常"。
      if (++impl_->idle_waits >= kIdleWaitsBeforePolling) {
        if (impl_->opt.loopback) {
          D2O_INFO("%s：%d 秒没有数据（环回在系统不出声时本来就不产数据），"
                   "改用轮询节奏继续等",
                   impl_->opt.label.c_str(), impl_->idle_waits * 200 / 1000);
          impl_->set_pacing("轮询（暂时没在出声）");
        } else {
          D2O_WARN("%s：事件连续 %d 次（约 %d 秒）没来，改用轮询节奏 —— "
                   "采集端点本该一直在出包，有些驱动上事件回调就是不发事件",
                   impl_->opt.label.c_str(), impl_->idle_waits,
                   kIdleWaitsBeforePolling * 200 / 1000);
          impl_->set_pacing("轮询（事件一直不来）");
        }
        impl_->use_event = false;
      }
    } else {
      impl_->idle_waits = 0;
    }
  } else {
    sleep_interruptible(impl_->stopping, impl_->pace_ms);
  }
  if (impl_->stopping.load(std::memory_order_relaxed)) return;
  drain();
}

void WasapiCapture::drain() {
  for (;;) {
    UINT32 packet = 0;
    HRESULT hr = impl_->capture->GetNextPacketSize(&packet);
    if (FAILED(hr)) {
      handle_stream_error(hr);
      return;
    }
    if (packet == 0) return;

    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    UINT64 pos = 0;
    UINT64 qpc = 0;
    hr = impl_->capture->GetBuffer(&data, &frames, &flags, &pos, &qpc);
    if (hr == AUDCLNT_S_BUFFER_EMPTY) return;  // 轮询路上正常会撞到
    if (FAILED(hr)) {
      handle_stream_error(hr);
      return;
    }
    if (frames > 0) {
      if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
        impl_->discontinuity_count.fetch_add(1, std::memory_order_relaxed);
      }
      impl_->packets.fetch_add(1, std::memory_order_relaxed);
      if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
        impl_->audible_packets.fetch_add(1, std::memory_order_relaxed);
      }
      // **SILENT 的 pData 内容未定义**（陷阱 68）。传 nullptr 进来，deliver 自己造零。
      deliver((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data, frames,
              (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ? 0 : qpc);
    }
    impl_->capture->ReleaseBuffer(frames);
  }
}

void WasapiCapture::deliver(BYTE const* data, unsigned frames, std::uint64_t qpc) {
  int const cap = static_cast<int>(frames) * impl_->opt.rate / impl_->in_rate + 1024;
  if (static_cast<int>(impl_->conv.size()) < cap * 2) {
    impl_->conv.resize(static_cast<std::size_t>(cap) * 2);
  }

  uint8_t const* in_ptr[1] = {nullptr};
  if (data == nullptr) {
    std::size_t const need = static_cast<std::size_t>(frames) * impl_->in_bytes_per_frame;
    if (impl_->zeros.size() < need) impl_->zeros.resize(need);
    std::memset(impl_->zeros.data(), 0, need);
    in_ptr[0] = impl_->zeros.data();
  } else {
    in_ptr[0] = data;
  }
  uint8_t* out_ptr[1] = {reinterpret_cast<uint8_t*>(impl_->conv.data())};

  int const got = ::swr_convert(impl_->swr, out_ptr, cap, in_ptr, static_cast<int>(frames));
  if (got <= 0) return;

  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->ring.push(impl_->conv.data(), got);
  }

  // 设备时钟 vs 系统时钟：一次性的体检。
  //
  // 这条不是洁癖。廉价声卡的时钟和主机差个千分之几是常态，而这条链路上音频的
  // 时间轴是**按帧数**走的（见 audio_mixer 那段），所以差异不会让音画漂 ——
  // 它会表现为"攒得比取得快"或"取得比攒得快"，一路把环形缓冲顶满或抽干。
  // 攒满 5 秒之后比一次，差得多就报一声，省得将来有人对着"音轨时长对不上"猜。
  if (qpc != 0) {
    if (impl_->first_qpc == 0) impl_->first_qpc = qpc;
    impl_->delivered_frames += static_cast<std::uint64_t>(got);
    if (!impl_->rate_checked &&
        impl_->delivered_frames > static_cast<std::uint64_t>(impl_->opt.rate) * 5) {
      double const secs =
          static_cast<double>(qpc - impl_->first_qpc) / static_cast<double>(qpc_frequency());
      if (secs > 0.1) {
        impl_->rate_ratio =
            (static_cast<double>(impl_->delivered_frames) / impl_->opt.rate) / secs;
        impl_->rate_checked = true;
        if (impl_->rate_ratio < 0.95 || impl_->rate_ratio > 1.05) {
          D2O_WARN("%s：设备时钟和系统时钟差了 %.1f%%（这台设备的时钟不太准）。"
                   "音画不会因此漂（音频的时间轴按帧数走），但环里的水位会慢慢偏",
                   impl_->opt.label.c_str(), (impl_->rate_ratio - 1.0) * 100.0);
        } else {
          D2O_DEBUG("%s：设备时钟/系统时钟 = %.4f", impl_->opt.label.c_str(),
                    impl_->rate_ratio);
        }
      }
    }
  }
}

void WasapiCapture::handle_stream_error(HRESULT hr) {
  if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
    // 拔了、切了默认设备、或者被独占模式抢走了。**不当致命**：关掉，让
    // thread_main 的循环退避重开。期间 Mixer 取不到数据，输出的是静音。
    close_device();
    impl_->set_state(State::NoSignal, "设备被拔掉或切走了，正在等它回来");
    D2O_WARN("%s：设备失效（AUDCLNT_E_DEVICE_INVALIDATED），关掉重开", impl_->opt.label.c_str());
    return;
  }
  // 别的错误也走同一条路：重开一次比在坏掉的 client 上继续转强。
  close_device();
  impl_->set_state(State::NoSignal, "音频流出错（" + hr_text(hr) + "），正在重开");
  D2O_WARN("%s：音频流出错（%s），关掉重开", impl_->opt.label.c_str(), hr_text(hr).c_str());
}

// ---------------------------------------------------------------------------
// 消费侧
// ---------------------------------------------------------------------------

WasapiCapture::State WasapiCapture::state() const {
  return static_cast<State>(impl_->state.load(std::memory_order_relaxed));
}

std::string WasapiCapture::status_text() const { return impl_->get_text(); }

std::string WasapiCapture::pacing_text() const { return impl_->get_pacing(); }

int WasapiCapture::available() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->ring.size();
}

int WasapiCapture::pull_add(float* dst, int frames) {
  if (frames <= 0) return 0;
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->ring.pop_add(dst, frames);
}

std::uint64_t WasapiCapture::dropped_frames() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->ring.dropped();
}

std::uint64_t WasapiCapture::reopen_count() const {
  return impl_->reopen_count.load(std::memory_order_relaxed);
}

std::uint64_t WasapiCapture::packets() const {
  return impl_->packets.load(std::memory_order_relaxed);
}

std::uint64_t WasapiCapture::audible_packets() const {
  return impl_->audible_packets.load(std::memory_order_relaxed);
}

}  // namespace d2o
