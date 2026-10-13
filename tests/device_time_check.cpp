/**
 * @file tests/device_time_check.cpp
 * @brief `DeviceState::set_device_time` 落下来的偏移**必须恰好**是调用方给的那个数。
 *
 * 为什么值得单独一个测试：调用方给的是一句「主机现在 + N 秒」，而
 * `set_device_time` 内部要**再读一次**主机时刻来算差值，于是它算出来的是
 * `N 秒 − ε`（ε = 两次读钟之间真实流逝的时间，纳秒级）。原来那句
 * `duration_cast<seconds>` 是**向零截断**的 —— 只要 ε > 0，正偏移就少一秒。
 * 症状是「NVR 对完时，水印比录像机慢一秒」，或者「网页手动校准 +90 秒，
 * `/api/time` 报 89」；两个都会被归到别处去查。
 *
 * ## 为什么必须循环 500 次，而不是写一条断言就完事
 *
 * 这个缺陷能不能露出来**取决于时钟的分辨率**，不是每次都看得见。实测
 * （同一台机器，诊断在 `out\clockgran\probe.cpp`）：`system_clock::now()`
 * 的分辨率约 **104 ns**，于是两次 `now()` 差 0 还是几十纳秒是随机的 ——
 * 两万次里「恰好 90」14992 次、「掉到 89」5008 次，即 **25%**。
 * 也就是说单跑一次只抓得到四分之一：`check(offset == 90)` 在一份**有缺陷的**
 * 实现上有 75% 的概率打出 [OK]。这正是陷阱 85 的形状 —— 一条只有选对了
 * 时刻才生效的断言，和一条真断言长得一模一样。500 轮里一次都撞不上的
 * 概率是 0.75^500 ≈ 1e-62，所以循环之后它是确定性的。
 *
 * ## 判据里那个「ε 必须远小于半秒」的前提是被量出来的（陷阱 87）
 *
 * `round` 与截断只在 ε ∈ (0, 0.5s) 之内分得开：真被操作系统抢占半秒以上时
 * **两个写法都会给出 N−1**，那时红的不是产品。所以每一轮都顺手量一次 ε 的
 * 上界（`set` 调用前后各读一次钟），**ε ≥ 0.5 秒的那些轮单独计数、不参与判定**，
 * 并把「被抢占几轮」与「ε 最大多少」一起打在结果行里。前提写进判据，
 * 不留给读的人去猜。
 *
 * ## 负方向也测，而且期望是「两个写法都对」
 *
 * 向零截断对负数是往**大**里跑（−90 − ε → −90），所以负偏移本来就没坏。
 * 留着这一档是给它当**对照**：修好之后它必须仍然绿，而且绿的原因不能是
 * 「产品被改坏了才碰巧对上」。
 *
 * 编译（不需要第三方库；陷阱 18：PATH 里要先有 `mingw64\bin`，走
 * `scripts\build.ps1` 的环境即可）：
 *   g++ -std=c++17 -Isrc tests/device_time_check.cpp \
 *       src/common/device_state.cpp -o build/device_time_check.exe
 */

#include "common/device_state.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace d2o;
using SysClock = std::chrono::system_clock;

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, std::string const& what) {
  ++g_checks;
  if (ok) {
    std::printf("[ OK ] %s\n", what.c_str());
  } else {
    ++g_failed;
    std::printf("[FAIL] %s\n", what.c_str());
  }
}

/// 每一档跑多少轮。见文件头：单轮只有 25% 能看到那个缺陷。
constexpr int kIters = 500;

/// `round` 与截断的分界线。ε 一旦到了这里，两个写法给出的都是 N−1，
/// 这一轮判的就不是产品而是这台机器当时忙不忙（陷阱 87）。
constexpr std::int64_t kStallNs = 500'000'000;

struct Round {
  int exact = 0;          ///< 读回来恰好是给的那个数
  int minus1 = 0;         ///< 比期望少 1 —— 向零截断的签名
  int plus1 = 0;          ///< 比期望多 1
  int other = 0;          ///< 差得更离谱
  int stalled = 0;        ///< ε ≥ 0.5 秒，不参与判定
  long long worst_eps_ns = 0;
};

/// 把第 `n` 秒落值 `kIters` 次，数一数每次读回来是多少。
///
/// `eps` 取的是 `set_device_time` **调用前后**两次读钟之差 —— 它比真正那个 ε
/// （调用内部的两次读钟）略大，正好是一个**上界**：上界只要小于半秒，
/// 就不能拿「被抢占」来解释读回来的偏差。
Round run_value(DeviceState& dev, int n) {
  Round r;
  for (int i = 0; i < kIters; ++i) {
    auto const t0 = SysClock::now();
    dev.set_device_time(t0 + std::chrono::seconds{n});
    auto const t1 = SysClock::now();
    auto const eps =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (eps > r.worst_eps_ns) r.worst_eps_ns = eps;
    if (eps >= kStallNs) {
      ++r.stalled;
      continue;
    }
    long long const got = static_cast<long long>(dev.time_offset().count());
    if (got == n) {
      ++r.exact;
    } else if (got == n - 1) {
      ++r.minus1;
    } else if (got == n + 1) {
      ++r.plus1;
    } else {
      ++r.other;
    }
  }
  return r;
}

std::string fmt_round(int n, Round const& r) {
  char buf[384];
  std::snprintf(buf, sizeof(buf),
                "%+d 秒 × %d 轮：恰好命中 %d 轮、比期望少 1 的 %d 轮、"
                "多 1 的 %d 轮、差得更远的 %d 轮、被抢占 %d 轮；ε 最大 %lld ns",
                n, kIters, r.exact, r.minus1, r.plus1, r.other, r.stalled,
                r.worst_eps_ns);
  return std::string(buf);
}

void test_semantics() {
  DeviceState dev;

  check(dev.time_offset().count() == 0, "新建时偏移是 0");
  check(!dev.time_synced(), "新建时是「没被设过」");

  // 没设过时 now() 就是主机现在。落在两次读钟之间是**确定**成立的
  // （中间那次 now() 在时间上必然夹在这两次之间），所以这一条不靠运气。
  auto const before = SysClock::now();
  auto const seen = dev.now();
  auto const after = SysClock::now();
  check(seen >= before && seen <= after,
        "没设过时 now() 就落在两次读钟之间（= 主机现在）");

  // synced_at 存的是**整秒**（截断到秒，所以它 ≤ 真实时刻）。判据只能是
  // 「不晚于现在、且不过旧」—— 拿它跟毫秒级的 t0 比先后会假红。
  auto const t0 = SysClock::now();
  dev.set_device_time(t0 + std::chrono::seconds{90});
  auto const t1 = SysClock::now();
  auto const sa = dev.synced_at();
  check(dev.time_synced(), "设过之后 time_synced() 为真");
  check(sa <= t1 && (t1 - sa) < std::chrono::seconds{2},
        "synced_at 记的是「什么时候被改的」（整秒，落在刚才这两次读钟附近）");
  // 这一条**刻意不判精确**：精确那件事由 [2] 的 500 轮负责。这里只钉住
  // "偏差是 1 秒级、不是 1900 年级"。
  check(dev.time_offset() >= std::chrono::seconds{89} &&
            dev.time_offset() <= std::chrono::seconds{90},
        "偏移在 89~90 秒之间（粗判；精确判据在 [2]）");

  // 「把设备时间设成和主机一样」与「没被设过」是两件事：前者 NVR 确实下发了
  // 一个时刻，状态页该显示"已同步"。两者混淆的话，NVR 一对时反而看起来像没对。
  dev.set_device_time(SysClock::now());
  check(dev.time_synced(), "设成「和主机一样」也算设过了（与「没设过」分得清）");
  check(dev.time_offset().count() == 0, "……而这时偏移是 0");

  dev.reset_time();
  check(dev.time_offset().count() == 0, "reset_time() 之后偏移归零");
  check(!dev.time_synced(), "reset_time() 之后回到「没被设过」");
  check(dev.synced_at().time_since_epoch().count() == 0,
        "reset_time() 之后 synced_at 也归零");
  check(std::chrono::round<std::chrono::seconds>(dev.now() - SysClock::now())
            .count() == 0,
        "reset_time() 之后 now() 回到主机现在");
}

void test_now_follows_offset() {
  DeviceState dev;

  // 这一条是"偏移真的用上了"的正面证据：偏移存对了但 now() 没读它的话，
  // 水印上的时间不会动，而 /api/time 报的偏移却是对的。
  dev.set_device_time(SysClock::now() + std::chrono::seconds{3600});
  check(std::chrono::round<std::chrono::seconds>(dev.now() - SysClock::now())
            .count() == 3600,
        "设成 +3600 之后 now() 比主机快 3600 秒");

  dev.set_device_time(SysClock::now() - std::chrono::seconds{3600});
  check(std::chrono::round<std::chrono::seconds>(dev.now() - SysClock::now())
            .count() == -3600,
        "设成 -3600 之后 now() 比主机慢 3600 秒");

  dev.reset_time();
  check(std::chrono::round<std::chrono::seconds>(dev.now() - SysClock::now())
            .count() == 0,
        "reset 之后 now() 与主机同步");
}

}  // namespace

int main() {
  std::printf("== DeviceState 设备时钟 ==\n\n");

  std::printf("-- [1] 语义（偏移从哪来、synced 什么时候真）--\n");
  test_semantics();

  std::printf("\n-- [2] 反复落值：偏移必须恰好是给的那个数（每档 %d 轮）--\n",
              kIters);
  {
    DeviceState dev;
    // 0 那一档两个写法都对（−ε 的截断与四舍五入都是 0）—— 留着是给上面几条
    // 当对照，证明这一组不是"每一条都要求同一个方向"。
    int const values[] = {90, -90, 1, -1, 3600, 0};
    for (int const n : values) {
      Round const r = run_value(dev, n);
      check(r.exact + r.stalled == kIters && r.minus1 + r.plus1 + r.other == 0,
            fmt_round(n, r));
      if (r.stalled > 0) {
        std::printf("[NOTE] 上面这一档有 %d 轮进程被抢占了 ≥0.5 秒；"
                    "那些轮两个写法都对不上，判不出产品的对错（陷阱 87）。\n",
                    r.stalled);
      }
    }
  }

  std::printf("\n-- [3] now() 跟着偏移走 --\n");
  test_now_follows_offset();

  std::printf("\n%s\n", std::string(60, '=').c_str());
  std::printf("通过 %d 项，失败 %d 项。\n", g_checks - g_failed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
