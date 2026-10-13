#include "common/log.h"

#include <cstdio>
#include <filesystem>
#include <io.h>
#include <system_error>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace d2o {

std::atomic<int> Log::level_{static_cast<int>(LogLevel::Info)};
std::mutex Log::mutex_;

namespace {

/// 日志文件。**由 mutex_ 保护**（和 stderr 那一份共用同一把锁，所以文件和
/// 终端上看到的顺序一致）。进程退出时故意不关：正常退出会 flush，而崩溃时
/// 关不关都一样 —— 每行都 fflush 过了。
std::FILE* g_file = nullptr;
std::string g_file_path;
std::size_t g_file_bytes = 0;
constexpr std::size_t kFileRotateBytes = 4u * 1024u * 1024u;

/// 超过 4 MB 就轮转一份 .old。**一次轮转，不留编号历史**：这不是审计日志，
/// 是一份排障用的现场，而一个会自己长到几百 MB 的文件放在系统盘上比丢历史坏。
void rotate_if_needed() {
  if (g_file == nullptr || g_file_bytes < kFileRotateBytes) return;
  std::fclose(g_file);
  g_file = nullptr;
  std::string const old = g_file_path + ".old";
  std::remove(old.c_str());
  std::rename(g_file_path.c_str(), old.c_str());
  g_file = std::fopen(g_file_path.c_str(), "ab");
  g_file_bytes = 0;
}


/// 关掉日志文件。**调用方必须已经持有 mutex_**（和 rotate_if_needed 一样）。
///
/// 抽成一个函数而不是在两处各写三行：`set_file`（换文件）和 `close_file`
/// （恢复出厂要删掉它）都走这里，而"忘了把 g_file 置空"这类漏项只在一个地方
/// 就只可能漏一次。
void close_locked() {
  if (g_file != nullptr) {
    std::fclose(g_file);
    g_file = nullptr;
  }
}

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

  // **先拼不带颜色的那一行**，颜色只在往终端写的时候才加。
  // 反过来的话（先拼带颜色的、再往文件里写），日志文件里会是一堆
  // `^[[33m` —— 而它只在 stderr 是终端时才发作，也就是"在我机器上看不出来"。
  std::string line_buf;
  line_buf.reserve(msg.size() + 64);
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
  line_buf += '\n';

  std::lock_guard<std::mutex> lk(mutex_);
  // 光标颜色和这一行分开写：整行仍然是一次 fwrite，多线程之间不会互相插入。
  bool const tty = stdout_is_tty();
  char const* const color = tty ? level_color(lv) : "";
  if (tty) std::fwrite(color, 1, 5, stderr);
  std::fwrite(line_buf.data(), 1, line_buf.size(), stderr);
  if (tty) std::fwrite("\033[0m", 1, 4, stderr);
  std::fflush(stderr);

  if (g_file != nullptr) {
    rotate_if_needed();
    if (g_file != nullptr) {
      std::fwrite(line_buf.data(), 1, line_buf.size(), g_file);
      std::fflush(g_file);
      g_file_bytes += line_buf.size();
    }
  }
}

void Log::set_file(std::string const& path) {
  std::lock_guard<std::mutex> lk(mutex_);
  close_locked();
  g_file_path = path;
  g_file_bytes = 0;
  // 先看现存的有多大：上一次运行留下的文件也该轮到它轮转，否则"日志写满了"
  // 这件事要等到这一次自己再写满 4 MB 才发生。
  std::error_code ec;
  auto const sz = std::filesystem::file_size(path, ec);
  if (!ec) g_file_bytes = static_cast<std::size_t>(sz);
  rotate_if_needed();
  // "ab"：追加，且**二进制** —— 文本模式下 Windows 会把每个 '\n' 写成 "\r\n"，
  // 而 stderr 那条路不经过它，于是文件里和终端上的行尾不一样。
  g_file = std::fopen(path.c_str(), "ab");
  if (g_file == nullptr) {
    std::fprintf(stderr, "日志文件打不开（%s），只写终端。\n", path.c_str());
  }
}

std::string Log::file_path() {
  std::lock_guard<std::mutex> lk(mutex_);
  return g_file_path;
}

void Log::close_file() {
  std::lock_guard<std::mutex> lk(mutex_);
  close_locked();
  // **g_file_path 故意不清。** 调用方（恢复出厂）的顺序是
  // file_path() → close_file() → 删文件 → set_file(刚问到的那个路径)，
  // 而"原来写在哪"只有这里知道。清掉它等于逼调用方自己再记一份，
  // 两份迟早不一致 —— 症状是"恢复出厂之后日志写到了别处"。
  g_file_bytes = 0;
}

bool is_our_log_file(std::string const& name) {
  if (name == "desk2onvif.log" || name == "desk2onvif.log.old") return true;

  // audit-YYYY-MM-DD.log。手写逐字符判断，**不用 <regex>**：这个判据在
  // 恢复出厂的清空路径上，而 <regex> 会往二进制里再塞一份几百 KB 的模板
  // 实例化 —— 为了比对一个 20 字符的文件名不值当。
  constexpr char const kPrefix[] = "audit-";
  constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;   // 6
  constexpr std::size_t kDateLen = 10;                      // YYYY-MM-DD
  constexpr std::size_t kTotal = kPrefixLen + kDateLen + 4; // + ".log"
  if (name.size() != kTotal) return false;
  if (name.compare(0, kPrefixLen, kPrefix) != 0) return false;
  if (name.compare(kPrefixLen + kDateLen, 4, ".log") != 0) return false;
  for (std::size_t i = 0; i < kDateLen; ++i) {
    char const c = name[kPrefixLen + i];
    bool const is_digit = (c >= '0' && c <= '9');
    bool const is_dash = (i == 4 || i == 7) && c == '-';
    if (!is_digit && !is_dash) return false;
  }
  return true;
}

}  // namespace d2o
