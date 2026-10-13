#include "app/audio_track.h"

#include "common/log.h"

namespace d2o {

AudioTrack::~AudioTrack() { stop(); }

bool AudioTrack::start(AudioTrackOptions const& opt, std::string& err) {
  err.clear();
  stop();  // 幂等：重载会拿同一个对象再 start 一次

  if (opt.fps <= 0) {
    err = fmt("音频要按视频的拍驱动，但帧率是 %d", opt.fps);
    return false;
  }
  if (opt.mixer.rate <= 0 || opt.mixer.channels <= 0) {
    err = fmt("音频格式不合法：%d Hz / %d 声道", opt.mixer.rate, opt.mixer.channels);
    return false;
  }
  // **两路都没勾 = 音频没开，不是错误配置**，但也不该悄悄产出一条数字静音的
  // 音轨（陷阱 78："有音轨但是静音"和"没有音轨"对 NVR 是两回事）。所以这里
  // 直接拒绝，调用方按纯视频走。
  if (!opt.mixer.system.enabled && !opt.mixer.mic.enabled) {
    err = "音频开着，但「系统声音」和「麦克风」都没有勾选";
    return false;
  }

  opt_ = opt;
  // 容量按名义每拍 + 1。差值法（见文件头）每拍最多比名义多要一个样本。
  std::size_t const per_tick =
      static_cast<std::size_t>(opt.mixer.rate / opt.fps + 1);
  block_.assign(per_tick * static_cast<std::size_t>(opt.mixer.channels), 0.0f);

  if (!mixer_.start(opt.mixer, err)) return false;

  AacConfig ac;
  ac.sample_rate = opt.mixer.rate;
  ac.channels = opt.mixer.channels;
  ac.bitrate_kbps = opt.bitrate_kbps;
  if (!encoder_.open(ac, err)) {
    // 编码器起不来就别把采集线程留着空转 —— 每留下一路设备，就多一份
    // "状态页上显示着 ok 而音轨其实一个字节都没有"。
    mixer_.stop();
    return false;
  }

  ticks_ = 0;
  encode_failed_ = false;
  warned_encode_ = false;
  active_ = true;
  D2O_INFO("音频已就绪：%d Hz %d 声道 %d kbps，每拍 %d 帧（对齐 %d fps 的视频）",
           opt.mixer.rate, opt.mixer.channels, opt.bitrate_kbps, frames_per_tick(),
           opt.fps);
  return true;
}

void AudioTrack::stop() {
  mixer_.stop();
  ticks_ = 0;
  active_ = false;
}

int AudioTrack::frames_per_tick() const {
  if (opt_.fps <= 0) return 0;
  return opt_.mixer.rate / opt_.fps;
}

std::uint64_t AudioTrack::frames_out() const { return mixer_.frames_out(); }

double AudioTrack::seconds() const {
  int const rate = opt_.mixer.rate;
  return rate > 0 ? static_cast<double>(frames_out()) / static_cast<double>(rate)
                  : 0.0;
}

void AudioTrack::tick(PacketSink const& sink) {
  if (!active_) return;

  ++ticks_;
  // **累计应得减去已产出**，不是"每拍固定 rate/fps"。见文件头：整除的余数
  // 逐拍累加不会丢，48k/7 这种情况下一小时也不会短掉 51 秒。
  std::int64_t const want_total =
      static_cast<std::int64_t>(ticks_) * opt_.mixer.rate / opt_.fps;
  std::int64_t const produced = static_cast<std::int64_t>(mixer_.frames_out());
  std::int64_t const want = want_total - produced;
  if (want <= 0) return;  // 到不了（除非 fps > rate），但别让它变成负数

  std::size_t const need = static_cast<std::size_t>(want) *
                           static_cast<std::size_t>(opt_.mixer.channels);
  if (need > block_.size()) block_.resize(need, 0.0f);

  // `fill` **一定会写满**（源端不够就补零）—— 所以这里不需要、也不该自己清零。
  // 补零是正常路径：环回在系统不出声时本来就一个样本都不给。
  mixer_.fill(block_.data(), static_cast<int>(want));

  std::string err;
  if (!encoder_.encode(block_.data(), static_cast<int>(want), sink, err)) {
    encode_failed_ = true;
    if (!warned_encode_) {
      warned_encode_ = true;
      D2O_WARN("音频编码失败：音轨从这里断掉，视频不受影响。%s", err.c_str());
    }
    return;
  }
  encode_failed_ = false;
}

bool AudioTrack::flush(PacketSink const& sink, std::string& err) {
  if (!active_) {
    err.clear();
    return true;
  }
  return encoder_.flush(sink, err);
}

}  // namespace d2o
