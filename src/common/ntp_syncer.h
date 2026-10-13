// 周期对时的执行者：一条自己的线程 + 一个 condition_variable。
//
// 它拿 `ntp_client` 问一次时间，把结果落成 `DeviceState` 的偏移。
//
// ---------------------------------------------------------------------------
// 与实施方案的一处偏离（刻意，理由是量过的）
//
// 计划里写的是"周期对时用 `uvcpp_timer`，挂在 `app.loop()` 上"。**改成了
// 一条专用线程**，三条理由，缺一不可：
//
//   1. **裸形态根本没有那个事件循环。** `--record --seconds N`、以及任何
//      `--no-http` 的跑法走的是 `cmd_run(parsed, RunForm::Bare, …)`
//      （`main.cpp`），那条路自己注释写着"没有单实例锁、没有日志文件" ——
//      它**也不建 web 循环**。定时器挂在 `app.loop()` 上的话，
//      `time.mode=ntp` 在这些形态下会**静默地永远不对时**：不报错、不写日志、
//      界面上一切正常（界面本身也不在）。这正是本项目最恨的那类失败。
//   2. **后台线程本来就要有。** UDP 查询是阻塞的（最长 `timeout_ms` × 地址数），
//      不能占住 web 循环 —— 它同时管着 WS 推送和 SOAP。计划自己也写了
//      "回调里只置位，真正的查询交给一个后台线程"。既然那个线程无论如何都要
//      存在，定时器就只是多一跳、少不掉任何东西。
//   3. **收尾是确定的**：`stop()` 置位 + `notify` + `join`，一行不多。而且
//      `condition_variable` 让 `request_now()`（`POST /api/time/sync` 那条路）
//      免费拿到 —— 用定时器的话还得再造一条从 web 循环到工作线程的唤醒路径。
//
// ---------------------------------------------------------------------------
// 它**不写配置文件**（这一条比上面那条更容易被"顺手改掉"）
//
// 进程里另外两个写 `config.json` 的地方 —— `apply_config`（`main.cpp`）与
// `--save-config` —— 都跑在 web 循环线程 / 主线程上，它们之间靠"同一条线程"
// 天然串行。`save_config` 的动作是"写 `<path>.tmp` → `remove` → `rename`"，
// 从**第二条线程**插进去就会和那两次写叠在一起：最坏的结果是拿到半个文件，
// 而那份文件是下次启动的全部依据。所以 NTP 得来的偏移只活在 `DeviceState`
// 与下面这份 `Status` 里。
//
// 这不是遗漏，是**语义划清**，而且要能自圆其说：
//
//   `config.time.offset_s` 的含义 = "重启后要恢复的那个偏移"。
//   手动校准（网页 / NVR 下发）走的是 `apply_config`，它存盘 —— 因为手动那条路
//   重启之后不会自己再对一次。
//   NTP 那条路重启后**会**自己再对一次（起来就对，见下），所以它不需要存盘，
//   存了反而会让"配置里的值"和"设备此刻的值"长期地对不上。
//
// ---------------------------------------------------------------------------
// 手动模式下 `request_now()` 只问、不落
//
// `POST /api/time/sync` 是个按钮，用户在手动模式下也会点（想试试填的服务器通不通）。
// 这时我们**照常发起查询**并如实报出结果，但**不改设备时间**：手动模式下让一个
// 后台线程把钟挪走，和配置里那个 `offset_s` 立刻就对不上，而下次重启又弹回去 ——
// 那才是真的"改了没反应"。界面照这个结果说话：对上了就告诉用户"服务器可用，
// 当前是手动模式，设备时间未改动；切到 NTP 才会采用"。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/config.h"

namespace d2o {

class DeviceState;

class NtpSyncer {
 public:
  /// 对时状态的一份快照。给 `/api/time`、`/api/status`、`/ws/stats` 用。
  struct Status {
    /// 来自配置。**是发起时那一刻的配置**，不是"此刻的" —— 但它只用于显示。
    std::string mode = "manual";
    std::string server;
    int interval_s = 0;

    /// 此刻是否有一次查询在飞（`POST /api/time/sync` 之后轮询它就知道完没完）。
    bool syncing = false;

    /// 最后一次**尝试**的主机时刻（自 1970 起的秒）。失败也会更新 —— 配合
    /// `last_state` 读：`last_at` 说"什么时候试的"，`last_state` 说"试成什么样"。
    std::int64_t last_at_s = 0;
    /// `ok` / `timeout` / `bad_reply` / `unreachable` / `never`（一次都没试过）。
    std::string last_state = "never";
    std::string last_error;   ///< 失败时的人话；成功时为空
    int last_offset_s = 0;    ///< 最后一次**成功**那次算出来的偏移
    int last_stratum = 0;     ///< 服务器自报的层数，诊断用
    /// 最后一次尝试**是否真的挪了设备时钟**。成功但当前是手动模式时为 false
    /// —— 那是 `request_now()` 在手动模式下的正常结局（见文件头），不是失败。
    ///
    /// 单独一个字段而不是让读的人拿 `last_state == "ok" && mode == "ntp"` 去推：
    /// 那个推导漏掉了 `dev_ == nullptr` 这一项，而且它把「算出来的偏移」和
    /// 「用上了没有」合成一个判断，正是审计那一行要分开说的两件事。
    bool last_applied = false;
    int ok_count = 0;
    int fail_count = 0;
  };

  NtpSyncer();
  ~NtpSyncer();

  NtpSyncer(NtpSyncer const&) = delete;
  NtpSyncer& operator=(NtpSyncer const&) = delete;

  /// 起工作线程。`dev` 必须是活的、且活得比本对象长（`main.cpp` 里那个
  /// `cmd_run` 一层的对象满足这两条）。空指针 = 只查不落，不会崩。
  ///
  /// `mode == "ntp"` 时**起来就对一次**（不等一个周期）。这正是
  /// `ntp_interval_s == 0`（"只在启动时对一次"）的含义来源。
  void start(DeviceState* dev);
  /// 停机并 join。幂等，可从任何线程调。
  void stop();

  /// 换一份配置。从手动切到 NTP 时**立刻排一次**（不等一个周期的空窗）；
  /// 从 NTP 切到手动时，已经排好的下一次就不再执行（`loop()` 每一轮都重读配置）。
  void set_config(TimeConfig const& c);

  /// 立刻对一次（不等下一次到期）。手动模式下也照做，但**不落**到设备时间上
  /// —— 理由见文件头。
  void request_now();

  /// 线程安全。
  Status status() const;

  /// **每完成一次尝试就调一次**（成功、失败都调），参数是刚落下的那份状态快照。
  ///
  /// 存在的理由是操作日志：`ntp.sync` 那一行要写「服务器 + 结果 + 偏移」，而这三样
  /// 只有这个线程知道 —— 轮询 `status()` 去猜的话，「这一次」和「上一次」在
  /// 两个连续成功、偏移还相同的对时之间根本分不开，那条日志就会时有时无。
  ///
  /// ⚠️ **它跑在对时线程上，不在 Web 循环线程上。** 实现里不要碰任何只有循环
  /// 线程才安全的东西（`AuditLog` 自带互斥量，所以它是安全的那个）。
  /// 调用时**不持 `mu_`**，理由见 `loop()` 里那段注释。
  ///
  /// 在 `start()` 之前设好即可；之后再改是没验证过的用法。
  std::function<void(Status const&)> on_sync;

 private:
  void loop();
  /// 把一次查询的结局落到 `last_*` 与（视模式而定的）`DeviceState` 上。
  /// 调用时必须已经持有 `mu_`。
  void latch_locked(TimeConfig const& c, int state, int offset_s, int stratum,
                    std::string const& error);

  /// 把 `last_*` / `cfg_` 拼成一份 `Status`。**调用时必须已经持有 `mu_`**。
  /// `status()` 与 `on_sync` 的回调参数都从这里出 —— 两处各拼一遍的话，
  /// 「界面上看到的那份」和「写进审计的那份」迟早会差一个字段。
  Status snapshot_locked() const;

  DeviceState* dev_ = nullptr;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::thread th_;

  TimeConfig cfg_;
  bool stop_ = false;
  bool requested_ = false;
  bool syncing_ = false;
  /// 下一次自动对时的时刻（steady 钟，不受系统时间被改的影响）。
  /// `time_point::max()` = 不再自动对（手动模式，或 `interval_s == 0` 且已经对过）。
  std::chrono::steady_clock::time_point next_;

  std::int64_t last_at_s_ = 0;
  std::string last_state_ = "never";
  std::string last_error_;
  int last_offset_s_ = 0;
  int last_stratum_ = 0;
  bool last_applied_ = false;
  int ok_count_ = 0;
  int fail_count_ = 0;
};

}  // namespace d2o
