#pragma once

/**
 * @file src/common/log.h
 * @brief 极简的分级日志。
 *
 * 只有三个要求：带时间戳、带级别、多线程不串行。所以自己写，不引第三方库 ——
 * 采集线程和编码线程会同时打日志，printf 的两三次 write 之间会被插进去，
 * 表现是半行日志和半行日志粘在一起。这里整行拼好再一次性 fwrite。
 *
 * 不用 libuvcpp 自带的 uvcpp_log：那套要等 webapp 起来才初始化，而采集/编码
 * 这条链路在 Phase 1 根本还没有 webapp。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace d2o {

/// 把 printf 风格的参数拼成一个 std::string。
///
/// 故意**不加** __attribute__((format(printf,1,2)))：那个属性要求第二项指的是
/// 真正的可变参数列表（`...`），而这里是参数包模板，GCC 会直接报
/// "argument 3 value '2' does not refer to a variable argument list"。
///
/// 少掉的格式检查由语言本身兜住：把 std::string 传进 `...` 是非良构的，
/// 编译当场就过不去 —— 所以 `%s` 必须配 `.c_str()` 这件事仍然会在编译期暴露。
template <typename... Args>
std::string fmt(char const* f, Args... args) {
  int const n = std::snprintf(nullptr, 0, f, args...);
  if (n <= 0) return std::string();
  std::string s(static_cast<std::size_t>(n), '\0');
  std::snprintf(&s[0], static_cast<std::size_t>(n) + 1, f, args...);
  return s;
}

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Off };

class Log {
 public:
  static void set_level(LogLevel lv) {
    level_.store(static_cast<int>(lv), std::memory_order_relaxed);
  }
  static LogLevel level() {
    return static_cast<LogLevel>(level_.load(std::memory_order_relaxed));
  }
  static bool enabled(LogLevel lv) {
    return static_cast<int>(lv) >= level_.load(std::memory_order_relaxed);
  }

  /// 拼好整行再输出，避免多线程互相插入。
  static void write(LogLevel lv, std::string const& msg, char const* file, int line);

  /// 除 stderr 之外**再**写一份到文件。
  ///
  /// 长驻形态（`--console` / `--worker`）需要它，而且两边的理由不同：
  ///
  ///   * 控制台形态：用户双击起来，用完了直接把窗口一关 —— stderr 跟着消失，
  ///     出过什么问题一点痕迹都不留。
  ///   * 服务形态：worker 是 `CreateProcessAsUserW` 拉起来的，**根本没有控制台**，
  ///     它的 stderr 落在虚空里。没有这一份，那种形态出问题时能拿到的只有
  ///     "进程还在、端口开着"，任何原因都查不到（服务自己的日志只记它拉起了谁）。
  ///
  /// 打不开**不当错误**：只往 stderr 说一句就算。日志写不出来不该让程序起不来。
  /// 文件超过 4 MB 自动轮转一份 `.old`（一次轮转，不留历史备份）。
  static void set_file(std::string const& path);
  /// 按值返回：它由一把锁保护着，而调用方拿它只是为了打印一行。
  static std::string file_path();

  /// 关掉文件那一份（幂等，没开过也照样成功）。**给"恢复出厂要删掉日志"用。**
  ///
  /// 存在的唯一理由是 Windows 的 `fopen` 不带 `FILE_SHARE_DELETE`：句柄开着
  /// 的时候 `std::remove` 直接失败（Permission denied），而症状恰好是那件事最
  /// 不该出错的地方 —— "恢复出厂之后旧的日志还在"。所以顺序只能是
  /// **先 `close_file()`、再删、最后 `set_file(原路径)` 开回来**。
  ///
  /// 关掉之后文件那一份就没有了（只剩下 stderr），所以删完必须开回来，
  /// 否则从那以后到进程退出之间的日志一行都不落盘。
  static void close_file();

 private:
  static std::atomic<int> level_;
  static std::mutex mutex_;
};

/// 这个名字是不是**我们自己的**日志文件。
///
/// 剪枝（审计日志按天留 N 天）、清空（恢复出厂）、以及验收里"不该删的那个还在
/// 不在"三处都调它 —— 一个判据三处用。写两遍的话它们迟早漂移，而漂移的症状
/// 是"恢复出厂之后旧日志还在"，正好是审计最不该出问题的地方。
///
/// 认三种：
///   * `desk2onvif.log`        —— 调试日志（`Log::set_file`）
///   * `desk2onvif.log.old`    —— 它 4 MB 轮转出来的那一份
///   * `audit-YYYY-MM-DD.log`  —— 审计日志按天一份（`audit_log`）
///
/// **只认名字，不认识路径**：调用方只把文件名喂进来，所以它对
/// `..\..\windows\system32\x.log` 这种输入天然没有反应 —— 这个窄化是刻意的，
/// 它让"删错了目录"这件事在这个函数里不可能发生。
bool is_our_log_file(std::string const& name);

}  // namespace d2o

// 级别判断包在宏里，关闭的级别连参数都不求值 —— D2O_DEBUG("%d", expensive())
// 在 Info 级别下不会调用 expensive()。
#define D2O_LOG_AT(lv, ...)                                              \
  do {                                                                   \
    if (::d2o::Log::enabled(lv)) {                                       \
      ::d2o::Log::write(lv, ::d2o::fmt(__VA_ARGS__), __FILE__, __LINE__); \
    }                                                                    \
  } while (0)

#define D2O_TRACE(...) D2O_LOG_AT(::d2o::LogLevel::Trace, __VA_ARGS__)
#define D2O_DEBUG(...) D2O_LOG_AT(::d2o::LogLevel::Debug, __VA_ARGS__)
#define D2O_INFO(...)  D2O_LOG_AT(::d2o::LogLevel::Info, __VA_ARGS__)
#define D2O_WARN(...)  D2O_LOG_AT(::d2o::LogLevel::Warn, __VA_ARGS__)
#define D2O_ERROR(...) D2O_LOG_AT(::d2o::LogLevel::Error, __VA_ARGS__)
