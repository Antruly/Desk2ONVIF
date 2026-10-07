#include "common/log.h"

#include <cstdio>
#include <io.h>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace d2o {

std::atomic<int> Log::level_{static_cast<int>(LogLevel::Info)};
std::mutex Log::mutex_;

namespace {

char const* level_tag(LogLevel lv) {
  switch (lv) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    default:              return "?????";
  }
}

char const* level_color(LogLevel lv) {
  switch (lv) {
    case LogLevel::Trace: return "\033[90m";   // 灰
    case LogLevel::Debug: return "\033[36m";   // 青
    case LogLevel::Info:  return "\033[0m";    // 默认
    case LogLevel::Warn:  return "\033[33m";   // 黄
    case LogLevel::Error: return "\033[31m";   // 红
    default:              return "\033[0m";
  }
}

/// 只在写终端时上色。重定向到文件时 ANSI 转义会变成一堆乱码。
bool stdout_is_tty() {
  static bool const tty = [] {
    if (!_isatty(_fileno(stdout))) return false;
#ifdef _WIN32
    // Windows 10 之前控制台不认 ANSI，要先打开 VT 处理。
    // 打不开就当不是终端处理，退化到无色输出，而不是打出一堆 ^[[31m。
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return false;
    DWORD mode = 0;
    if (!::GetConsoleMode(h, &mode)) return false;
    if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
      if (!::SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return false;
    }
    return true;
#else
    return true;
#endif
  }();
  return tty;
}

/// 从 __FILE__ 里取文件名部分。CMake 传的是绝对路径，整条打出来太长。
char const* base_name(char const* path) {
  if (!path) return "?";
  char const* last = path;
  for (char const* p = path; *p; ++p) {
    if (*p == '/' || *p == '\\') last = p + 1;
  }
  return last;
}

}  // namespace

void Log::write(LogLevel lv, std::string const& msg, char const* file, int line) {
  // 时间戳精确到毫秒。Phase 1 要靠它看帧间隔，秒级不够用。
  using clock = std::chrono::system_clock;
  auto const now = clock::now();
  auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()).count() % 1000;
  std::time_t const t = clock::to_time_t(now);
  std::tm tmv{};
#ifdef _WIN32
  ::localtime_s(&tmv, &t);
#else
  ::localtime_r(&t, &tmv);
#endif

  std::string line_buf;
  line_buf.reserve(msg.size() + 64);
  if (stdout_is_tty()) line_buf += level_color(lv);
  char head[48];
  std::snprintf(head, sizeof head, "%02d:%02d:%02d.%03d [%s] ",
                tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                static_cast<int>(ms), level_tag(lv));
  line_buf += head;
  if (lv <= LogLevel::Debug) {
    // Trace/Debug 才带位置：定级为 Info 以上的信息都该是"看消息就知道在哪"。
    line_buf += base_name(file);
    line_buf += ':';
    line_buf += std::to_string(line);
    line_buf += ' ';
  }
  line_buf += msg;
  if (stdout_is_tty()) line_buf += "\033[0m";
  line_buf += '\n';

  std::lock_guard<std::mutex> lk(mutex_);
  std::fwrite(line_buf.data(), 1, line_buf.size(), stderr);
  std::fflush(stderr);
}

}  // namespace d2o
