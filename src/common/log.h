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

 private:
  static std::atomic<int> level_;
  static std::mutex mutex_;
};

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
