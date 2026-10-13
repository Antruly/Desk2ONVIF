/**
 * @file tests/play_tone.cpp
 * @brief 让这台机器**真的在放声音**，持续 N 秒 —— 给 `tests/audio_e2e.ps1` 当信号源。
 *
 * ## 为什么不是脚本里直接放，也不是复用 `wasapi_check.exe`
 *
 * **一、不用 PowerShell 的 `Add-Type` 现写 P/Invoke。** 那样得手搓一份 RIFF 头，
 * 而项目里已经有一份**验过的**信号源（`tests/tone_wav.h`）—— 它存在的全部理由就是
 * "别让两个测试用不同的信号"。第二份 WAV 会立刻分叉：一份改了幅值/频率，另一份
 * 没跟上，于是两个测试量出来的不再是同一件事，而这种事坏起来是**安静的**。
 *
 * **二、不复用 `wasapi_check.exe`。** 它的 `main()` 不收参数（那是它自己的采集
 * 测量，跑一次十几秒而且会去开设备），既不能"只放声音 10 秒"，也不能和被测程序
 * 同时跑 —— 它自己会去抓环回，等于把被测的那一路抢走。
 *
 * **三、必须是真的一条连续流。** 陷阱 82 量过：`Beep()` 一次 3 秒得到 94%~97%
 * 的实时率，6 秒里放 15 次 400ms 的 `Beep` 只有 30.3%，而 `PlaySound` 循环播一段
 * 真 WAV 是 **100.0%**。差别全在 `Beep` 这个 API 自己。**本测试要判"采到的是
 * 非静音"，信号源一旦一顿一顿，判出来的就是采集层的假缺陷。**
 *
 * 所以这里只做一件事：`tone_wav.h` 写文件 → `PlaySound(SND_FILENAME|SND_ASYNC|
 * SND_LOOP)` → 睡 N 秒 → 停。**进程活着声音才在**，所以 N 秒必须由这个进程自己睡满，
 * 不能"起完就退"。
 *
 * ## 编译
 *
 * ```
 *   g++ -std=c++17 -Wall -Wextra tests/play_tone.cpp -o build/play_tone.exe -lwinmm
 * ```
 *
 * 不需要 `-Isrc`：它只 include 同目录的 `tone_wav.h`，不碰项目里的任何东西 ——
 * 这是刻意的，一个"放测试音"的小工具不该依赖被测程序的头文件。
 *
 * ## 用法
 *
 * ```
 *   build/play_tone.exe --seconds 10                 # 放 10 秒，然后自己退出
 *   build/play_tone.exe --seconds 30 --tag rtsp      # 换一个 WAV 文件名（tag 不同互不干扰）
 *   build/play_tone.exe --seconds 30 --ready C:\x.rdy # 声音真的响了就写下这个文件
 * ```
 *
 * `--ready` 是给脚本用的**握手**：`PlaySound` 返回 TRUE 之后立刻写出这个文件，
 * 进程退出时删掉。脚本轮询它就能知道"声音已经在响了"，而不是"起完睡 2 秒再看看"
 * —— 猜短了录到的是一段安静的头，判"非静音"就可能假失败；猜长了白等。
 * 这个项目里同类的地方（如 `tests\rtsp_sub_e2e.ps1` 的端口等待循环）一律是
 * "等条件成立"而不是"等一个拍脑袋的秒数"，这里沿用同一条纪律。
 *
 * **会被听见。** 它放的是 1 kHz 正弦、16 位、幅值 9000/32767（约 −11 dBFS）——
 * 不响但也不轻。跑测试之前先把系统音量调下来，或者戴上耳机。
 *
 * 退出码：0 = 正常放完；2 = 参数错；1 = 起不来（写不出 WAV，或者播放引擎拒绝）。
 */

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "tone_wav.h"

namespace {

/// 从 `--k v` / `--k=v` 两种写法里取一个带值的参数。没给返回空串。
/// **两种写法都要认**：项目里别的程序（`src/main.cpp` 的 `parse_args`）认前者，
/// 而手敲的时候后者更顺手；只认一种的话，另一种会被当成"这个开关没给值"，
/// 然后那个值就变成了下一个位置参数 —— 命令行看上去完全正常（陷阱 27）。
std::string opt_value(int argc, char** argv, char const* key) {
  std::string const k = key;
  for (int i = 1; i < argc; ++i) {
    std::string const a = argv[i];
    if (a == k && i + 1 < argc) return argv[i + 1];
    if (a.rfind(k + "=", 0) == 0) return a.substr(k.size() + 1);
  }
  return std::string();
}

/// 这个开关在命令行上出现过吗（哪怕后面没跟值）。
bool opt_seen(int argc, char** argv, char const* key) {
  std::string const k = key;
  for (int i = 1; i < argc; ++i) {
    std::string const a = argv[i];
    if (a == k || a.rfind(k + "=", 0) == 0) return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  // 认参数。**白名单之外一律报错退出**，不忽略 —— 敲错一个开关而程序照跑，
  // 得到的是"测试通过了"而实际什么都没测（陷阱 27 那一族的形状）。
  for (int i = 1; i < argc; ++i) {
    std::string const a = argv[i];
    bool const has_eq = a.find('=') != std::string::npos;
    std::string const key = has_eq ? a.substr(0, a.find('=')) : a;
    if (key == "--seconds" || key == "--tag" || key == "--ready") {
      if (!has_eq) ++i;  // 空格写法：把值跳过去
      continue;
    }
    std::fprintf(stderr, "不认识的参数：「%s」（只认 --seconds / --tag / --ready）\n",
                 a.c_str());
    return 2;
  }

  int const seconds = [&] {
    std::string const s = opt_value(argc, argv, "--seconds");
    return s.empty() ? 10 : std::atoi(s.c_str());
  }();
  if (seconds <= 0 || seconds > 3600) {
    std::fprintf(stderr, "--seconds 要在 1..3600 之间，收到的是 %d\n", seconds);
    return 2;
  }

  std::string const ready = opt_value(argc, argv, "--ready");
  if (opt_seen(argc, argv, "--ready") && ready.empty()) {
    std::fprintf(stderr, "--ready 后面要给一个文件路径（脚本靠它知道声音已经响了）\n");
    return 2;
  }

  std::string const tag = [&] {
    std::string const t = opt_value(argc, argv, "--tag");
    return t.empty() ? std::string("e2e") : t;
  }();
  std::string const wav = d2o_test::write_tone_wav(tag);
  if (wav.empty()) {
    // 写不出来就说清楚是**写文件**这一步失败的。只报"起不来"的话，读日志的人
    // 会去查播放引擎，而真正的原因（磁盘满 / %TEMP% 不可写）在别处。
    std::fprintf(stderr, "写不出测试音的 WAV（%%TEMP%% 不可写？）\n");
    return 1;
  }

  d2o_test::Tone tone(wav);
  if (!tone.start()) {
    std::fprintf(stderr, "PlaySound 拒绝播放「%s」—— 这台机器上没有可用的播放设备？\n",
                 wav.c_str());
    return 1;
  }

  // **这一行是给人看的锚点**，`--ready` 那个文件是给脚本看的。两个都给：
  // 手动跑的时候能一眼看出声音起来了。
  std::printf("TONE playing %d s\n", seconds);
  std::fflush(stdout);

  if (!ready.empty()) {
    HANDLE f = ::CreateFileA(ready.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD w = 0;
      ::WriteFile(f, "1", 1, &w, nullptr);
      ::CloseHandle(f);
    } else {
      // 写不出来**不当致命错误**：脚本那边等不到文件会超时，而超时信息里
      // 会带上是哪个路径 —— 比在这里直接退出更好查（退出的话脚本看到的是
      // "进程没了"，而它刚刚才报过"正在放声音"）。
      std::fprintf(stderr, "警告：写不出就绪文件「%s」，脚本会一直等到超时\n",
                   ready.c_str());
    }
  }

  // 睡觉。**不能用 Sleep 之外的办法**：这个进程一退，PlaySound 的声音就没了
  // （ASYNC 的播放线程随进程走）。
  ::Sleep(static_cast<DWORD>(seconds) * 1000);

  tone.stop();
  std::printf("TONE stopped\n");
  std::fflush(stdout);

  // 收尾删掉临时 WAV。**删在 stop 之后** —— 播着的时候删文件在 Windows 上会
  // 失败（文件被播放引擎持有），而那个失败完全无害，只会留下一个垃圾文件。
  ::DeleteFileA(wav.c_str());
  if (!ready.empty()) ::DeleteFileA(ready.c_str());
  return 0;
}
