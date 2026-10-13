#include "common/ntp_syncer.h"

#include "common/device_state.h"
#include "common/log.h"
#include "common/ntp_client.h"

namespace d2o {
namespace {

/// **主机**时刻（自 1970 起的秒），不是设备时刻。
///
/// 对时的记录必须挂在主机时间轴上：它的用途是回答"这台机器上的什么时候试过
/// 对时"，而设备时间正是这个线程要去挪的那个东西 —— 拿它当刻度会得到一条
/// 每次对完时都往前跳一下的时间线，那还不如没有。
std::int64_t host_now_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

NtpSyncer::NtpSyncer() : next_(std::chrono::steady_clock::now()) {}

NtpSyncer::~NtpSyncer() { stop(); }

void NtpSyncer::start(DeviceState* dev) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    dev_ = dev;
    if (th_.joinable()) return;  // 已经起过了，只更新 dev_
    stop_ = false;
  }
  th_ = std::thread([this] { loop(); });
}

void NtpSyncer::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

void NtpSyncer::set_config(TimeConfig const& c) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    bool const was_ntp = cfg_.mode == "ntp";
    cfg_ = c;
    if (c.mode == "ntp" && !was_ntp) {
      // 刚从手动切到 NTP：立刻对一次。**不等一个周期**是对的 —— 用户刚把这个
      // 开关打开，等一小时才有动静的话他只会认为这个功能坏了。
      next_ = std::chrono::steady_clock::now();
    }
  }
  cv_.notify_all();
}

void NtpSyncer::request_now() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    requested_ = true;
  }
  cv_.notify_all();
}

NtpSyncer::Status NtpSyncer::snapshot_locked() const {
  Status s;
  s.mode = cfg_.mode;
  s.server = cfg_.ntp_server;
  s.interval_s = cfg_.ntp_interval_s;
  s.syncing = syncing_;
  s.last_at_s = last_at_s_;
  s.last_state = last_state_;
  s.last_error = last_error_;
  s.last_offset_s = last_offset_s_;
  s.last_stratum = last_stratum_;
  s.last_applied = last_applied_;
  s.ok_count = ok_count_;
  s.fail_count = fail_count_;
  return s;
}

NtpSyncer::Status NtpSyncer::status() const {
  std::lock_guard<std::mutex> lk(mu_);
  return snapshot_locked();
}

void NtpSyncer::latch_locked(TimeConfig const& c, int state, int offset_s,
                             int stratum, std::string const& error) {
  auto const st = static_cast<NtpState>(state);
  last_at_s_ = host_now_s();
  last_state_ = ntp_state_name(st);
  last_error_ = error;
  last_stratum_ = stratum;
  if (st == NtpState::kOk) {
    ++ok_count_;
    last_offset_s_ = offset_s;
  } else {
    ++fail_count_;
  }

  bool applied = false;
  if (st == NtpState::kOk && c.mode == "ntp" && dev_ != nullptr) {
    dev_->set_device_time(std::chrono::system_clock::now() +
                          std::chrono::seconds{offset_s});
    applied = true;
  }
  last_applied_ = applied;

  // 每次尝试都落一行，成败都要有 —— "对时到底试没试过"只有这条路能回答，
  // 而 UDP 到公网 123 在没有网络的环境里是**静默丢弃**的。
  if (st == NtpState::kOk) {
    D2O_INFO("NTP 对时成功：服务器 %s，偏移 %+d 秒（stratum %d）%s",
             c.ntp_server.c_str(), offset_s, stratum,
             applied ? "，已应用到设备时间"
                     : "，当前是手动模式，设备时间未改动（切到 NTP 模式才会采用）");
  } else {
    D2O_WARN("NTP 对时失败：服务器 %s，%s（%s）；累计失败 %d 次",
             c.ntp_server.c_str(), ntp_state_name(st), error.c_str(), fail_count_);
  }
}

void NtpSyncer::loop() {
  using clock = std::chrono::steady_clock;
  std::unique_lock<std::mutex> lk(mu_);
  for (;;) {
    if (stop_) return;
    TimeConfig const c = cfg_;
    // 到期条件有两路：有人按了"立刻对时"，或者自动排期到了。
    // **自动那一路要求 `mode == ntp`** —— 手动模式下这个线程除了被显式请求之外
    // 什么都不做（这正是"手动"这个词的意思）。
    bool const auto_due = (c.mode == "ntp") && (next_ <= clock::now());
    if (requested_ || auto_due) {
      requested_ = false;
      syncing_ = true;
      // 查询是阻塞的，而且要在**不持锁**的情况下跑：`status()` 是给 HTTP 线程
      // 调的，拿不到锁的话界面上那个"对时中"会一直转圈。
      lk.unlock();
      NtpResult const r = ntp_query(c.ntp_server);
      lk.lock();
      syncing_ = false;
      latch_locked(c, static_cast<int>(r.state), r.offset_s, r.stratum, r.error);
      // `ntp_interval_s == 0` = "只在启动时对一次"：把下一次排到无穷远，
      // 而不是拍脑袋给个默认周期（那会和配置里写的 0 直接矛盾）。
      next_ = c.ntp_interval_s > 0
                  ? clock::now() + std::chrono::seconds{c.ntp_interval_s}
                  : clock::time_point::max();

      // 操作日志（审计）：**在不持锁的地方调**。
      //
      // 回调里要做一次磁盘写（`AuditLog::note` 自带互斥量 + 每行 fflush），
      // 而 `mu_` 是 `status()` 要拿的那把锁 —— 每来一个网页请求 / 每一次 WS
      // 统计推送都会去拿它。持着它做磁盘 IO，症状是"对完时那一刻界面上卡一下"，
      // 而且这台机器的磁盘忙起来时那一"下"可以是几十毫秒。
      //
      // 快照与回调都在这里先取出来：`unlock()` 之后 `cfg_` / `last_*` 都可能
      // 被别的线程改（`set_config` 就在网页线程上），再读就不是"这一次的结果"了。
      Status const snap = snapshot_locked();
      auto const cb = on_sync;
      lk.unlock();
      if (cb) cb(snap);
      lk.lock();
      continue;  // 回去重算一次到期条件：等待期间可能又有人按了"立刻对时"
    }

    if (c.mode == "ntp" && next_ != clock::time_point::max()) {
      cv_.wait_until(lk, next_);
    } else {
      cv_.wait(lk);
    }
  }
}

}  // namespace d2o
