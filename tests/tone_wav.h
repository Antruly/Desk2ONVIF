#pragma once

/**
 * @file tests/tone_wav.h
 * @brief "系统正在放声音" —— 一段可以循环播放的 1 kHz 正弦 WAV。
 *
 * ## 为什么是 `PlaySound` 而不是 `Beep()`
 *
 * **`Beep()` 不是一条连续的音频流，拿它放测试音会量出假的上界。** 实测同一个
 * 采集层，三种放音方式的实时率差到离谱：
 *
 * | 放音方式 | 环回跟上的实时率 |
 * |---|---|
 * | `PlaySound` 循环播一段 WAV（**真的连续流**） | **100.0%** |
 * | 一次 `Beep(1000, 3000)` | 94%~97% |
 * | 6 秒里放 15 次 `Beep(1000, 400)` | **30.3%** |
 *
 * 差别全在 `Beep` 自己（主板蜂鸣器时代的产物，每次调用都有自己的起停）。
 * 这个坑**差点导出反的结论**：30% 那个数看起来活像一个采集层缺陷，会让人去改
 * 一个根本没坏的模块；而 94% 那个数**看起来像真实上界**，于是判据门槛设在 85%
 * 上 —— 理由完全是错的，只是碰巧结论对。详见 `tests/wasapi_check.cpp` 文件头。
 *
 * ## 为什么是一个共享头文件
 *
 * 两个测试都要它：`wasapi_check.cpp`（量采集层）和 `audio_mixer_check.cpp`
 * （量混音）。抄两份的话，哪天改进了一份而另一份没跟上，两边量出来的就不再是
 * 同一件事了 —— 而"两个测试用的不是同一个信号源"这种事，坏起来是安静的。
 *
 * 头文件形态而不是 .cpp：两个测试的编译命令都写在各自文件头里、各自独立编译，
 * 多一个需要一起编的 .cpp 只会让那两条命令行更容易写错。
 *
 * 只依赖 Win32，RIFF 头手写（44 字节，比引一个音频库便宜）。
 * 用到它的测试要链 `-lwinmm`。
 */

#include <windows.h>

#include <mmsystem.h>  // PlaySound（WIN32_LEAN_AND_MEAN 把 mmsystem.h 挡在外面了）

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace d2o_test {

/// 把一段 1 kHz 正弦写成 16 位单声道 WAV，返回路径。失败返回空串。
/// `tag` 用来让不同的测试各写各的文件，免得并行跑的时候互相盖。
inline std::string write_tone_wav(std::string const& tag = "") {
  std::string path;
  {
    char tmp[MAX_PATH] = {0};
    DWORD const n = ::GetTempPathA(MAX_PATH, tmp);
    path.assign(tmp, n);
  }
  path += "desk2onvif_test_tone" + (tag.empty() ? std::string() : "_" + tag) + ".wav";

  int const rate = 48000;
  int const secs = 2;
  int const samples = rate * secs;
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    double const t = static_cast<double>(i) / rate;
    pcm[static_cast<std::size_t>(i)] =
        static_cast<std::int16_t>(std::sin(2.0 * 3.14159265358979 * 1000.0 * t) * 9000.0);
  }

  std::vector<std::uint8_t> buf;
  auto put32 = [&buf](std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
  };
  auto put16 = [&buf](std::uint16_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
  };
  auto putstr = [&buf](char const* s) {
    for (; *s; ++s) buf.push_back(static_cast<std::uint8_t>(*s));
  };

  std::uint32_t const data_bytes = static_cast<std::uint32_t>(pcm.size() * 2);
  putstr("RIFF");
  put32(36 + data_bytes);
  putstr("WAVE");
  putstr("fmt ");
  put32(16);
  put16(1);                                     // PCM
  put16(1);                                     // 单声道
  put32(static_cast<std::uint32_t>(rate));
  put32(static_cast<std::uint32_t>(rate * 2));  // 字节率
  put16(2);                                     // 块对齐
  put16(16);                                    // 位深
  putstr("data");
  put32(data_bytes);
  for (std::int16_t s : pcm) put16(static_cast<std::uint16_t>(s));

  HANDLE f = ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return std::string();
  DWORD written = 0;
  ::WriteFile(f, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr);
  ::CloseHandle(f);
  return path;
}

/// 一声能持续响的"系统在放声音"。析构时自动停，免得测试中途失败把声音留在那儿。
struct Tone {
  std::string wav;
  bool playing = false;

  explicit Tone(std::string p) : wav(std::move(p)) {}

  bool start() {
    if (wav.empty()) return false;
    // SND_LOOP 必须和 SND_ASYNC 一起给：不给 ASYNC 的话 PlaySound 会**阻塞**
    // 整整一段的时长，量出来的时间全废。
    playing =
        ::PlaySoundA(wav.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_LOOP) != FALSE;
    return playing;
  }

  void stop() {
    ::PlaySoundA(nullptr, nullptr, 0);
    playing = false;
  }

  ~Tone() { stop(); }
  Tone(Tone const&) = delete;
  Tone& operator=(Tone const&) = delete;
};

}  // namespace d2o_test
