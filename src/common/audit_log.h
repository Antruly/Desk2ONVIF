#pragma once

/**
 * @file src/common/audit_log.h
 * @brief 操作日志（审计）：谁、从哪来、改了什么。
 *
 * ## 为什么不复用 `Log`
 *
 * 三条理由缺一不可，而三条 `Log` 都做不到 —— 它是个**静态类**（全进程一份、
 * 一个文件、4 MB 轮转一份 `.old`）：
 *
 *   1. **留存期不同**：审计要按天留半年（用户明确要求「本机默认存半年」），
 *      而调试日志是「写到 4 MB 就翻掉」。
 *   2. **内容纪律不同**：审计里**绝不能出现口令**（见 `note()` 的注释）。
 *      两者混在一个文件里，那条纪律就没法单独执行。
 *   3. **恢复出厂要清的集合必须明确可枚举**：「恢复出厂清空日志」这个动作
 *      删的是哪几个文件，得能一句话说清。混在一起时它是「日志文件里的一部分」，
 *      而那不是个能写进判据的集合。
 *
 * ## 行格式（固定，字段内不许出现 `|`）
 *
 * ```
 * 2026-10-08 15:30:12 | 192.168.1.23 | web:admin | config.set | rtsp.port=8554 (pending)
 *      时间            来源          身份         动作          细节
 * ```
 *
 * 按天一个文件：`<配置目录>\logs\audit-YYYY-MM-DD.log`。
 * 分隔符选 `|` 而不是逗号或制表符：细节里出现逗号是常事（列表），而 `|` 在
 * 我们写的所有细节串里都不出现 —— 并且 `sanitize()` 会保证它**不可能**出现。
 */

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace d2o {

/// 「谁干的」。
///
/// 拆成两个字段而不是拼成一个字符串，是因为它们**来源不同**：来源在 HTTP 层
/// 手边就有（`req.peer_ip()`），而身份是会话里那个用户名 / NVR 用的那个。
/// 拼起来写（`"192.168.1.23/web:admin"`）的话，读日志的人要自己猜分隔符，
/// 而写日志的人迟早会写出一个既不像 IP 也不像用户名的东西。
struct AuditWho {
  /// 来源。网页那条路是来源 IP；NVR 那条路是 NVR 的 IP；系统自己写的是
  /// `"local"` 或 `"system"`。**不许为空** —— 空来源在审计里等于没有来源。
  std::string source;
  /// 身份。惯例是 `<通道>:<用户名>`（`web:admin` / `onvif:admin`），
  /// 系统自己写的是 `system`。**口令永远不在这里，也不在 detail 里。**
  std::string identity;
};

/// 一条读回来的记录。给 `GET /api/audit` 用。
struct AuditEntry {
  std::string day;       ///< `YYYY-MM-DD`
  std::string time;      ///< `HH:MM:SS`
  std::string source;
  std::string identity;
  std::string action;
  std::string detail;
  std::string raw;       ///< 整行原文（界面上「复制一行」要的是它，不是拼回去的那份）
};

/// 审计日志的规模，给界面顶部那行「保留 N 天，共 X 个文件 / Y KB」用。
struct AuditStats {
  bool enabled = false;
  int keep_days = 0;
  int files = 0;
  long long bytes = 0;
  std::string dir;
};

/// 操作日志。**实例类**（不是 `Log` 那样的静态类），一进程一份。
class AuditLog {
 public:
  AuditLog() = default;
  ~AuditLog();
  AuditLog(AuditLog const&) = delete;
  AuditLog& operator=(AuditLog const&) = delete;

  /// 建目录、算今天的文件名、**启动时剪一次枝**。
  ///
  /// `keep_days == 0` 表示永不剪枝。`enabled == false` 时只记住路径（界面要
  /// 显示它落在哪）而一行都不写、也不剪枝 —— 「关掉」应当是**什么都不做**，
  /// 而不是「照写但换个地方」。
  ///
  /// 打不开目录**不当致命错误**：返回 false 并把原因写进 `err`，调用方打一行
  /// WARN 就够了。审计写不出来不该让程序起不来（与 `Log::set_file` 同一个取舍）。
  bool open(std::string const& dir, int keep_days, bool enabled, std::string& err);

  /// 关掉文件句柄（幂等）。**给「恢复出厂要删日志」用** —— Windows 的
  /// `fopen` 不带 `FILE_SHARE_DELETE`，句柄开着就删不掉，理由与
  /// `Log::close_file()` 逐字相同。
  void close();

  bool enabled() const;
  std::string dir() const;
  int keep_days() const;

  /// 写一条。**可以在任意线程上调**（自己的互斥量 + 每行 fflush）。
  ///
  /// ## 口令纪律（硬性）
  ///
  /// `detail` 里**永远不出现**口令、`Authorization` 头、nonce、token。
  /// 改口令类字段时只记「已修改」，不记改成了什么。
  /// 这条纪律唯一的执行者是验收脚本里那条「拿真口令在整份日志里 grep，
  /// 必须零命中」—— 没有它，这一条就只是一句注释。
  ///
  /// 判据：**这一行如果被贴进工单，会不会泄露什么？** 会，就别写进去。
  ///
  /// 另外 `detail` 里也**不要写配置值**，写**字段名**：字段名不是值，
  /// 躲开的是整类问题（口令恰好住在某个字段的「值」里）。
  void note(AuditWho const& who, std::string const& action, std::string const& detail);

  /// 删掉**我们自己的**全部审计文件。返回删掉了几个。目录本身不动。
  ///
  /// 与剪枝**共用同一个判据**（`is_our_log_file`，见 `log.h`）：写两遍的话
  /// 它们迟早漂移，而漂移的症状是「恢复出厂之后旧日志还在」—— 正好是审计
  /// 最不该出问题的地方。
  int clear();

  /// 剪枝：删掉日期早于 `今天 - keep_days` 的那些。返回删掉了几个。
  /// **不递归、不按大小、不碰别的文件。**
  int prune();

  AuditStats stats() const;

  /// 读回最近 `days` 天的记录，**倒序**（新的在前），最多 `limit` 条。
  ///
  /// `keyword` 非空时只保留「原文里含它」的行（大小写敏感，与界面上的过滤一致）。
  /// **只读我们自己的 `audit-*.log`** —— 这个窄化与 `is_our_log_file` 是同一个。
  std::vector<AuditEntry> read(int days, int limit, std::string const& keyword,
                               std::string& err) const;

  // ---------------------------------------------------------------------
  // 【本类的并发纪律】下面每一个**公开**方法都自己拿 `mutex_`，所以
  // **公开方法之间不许互相调用** —— `mutex_` 是非递归的 `std::mutex`，
  // 在它上面重复加锁是当场死锁。
  //
  // 这不是理论风险：`open()` 一开始就写成「拿锁 → 调 `close()` → 调 `prune()`」，
  // 而这两个也各自拿锁。症状是本项目最难查的那一种 —— 进程**一声不响地停住**：
  // 0.05 秒 CPU、没有输出、没有退出码、没有目录被建、连 `--seconds` 都不会到期。
  // 它编得过、`--selfcheck` 也过（自检不走 `cmd_run`），只有真的跑起来才发作。
  //
  // 规矩：**需要「持锁地做某件事」时，把实现写成 `xxx_locked()`，公开的那个
  // `xxx()` 只是「拿锁 + 转发」。** 与既有的 `reopen_for_today_locked()`
  // 是同一个形状（`note()` 也那样调它）。
  // ---------------------------------------------------------------------

  /// 关掉文件句柄。**调用方必须已经持有 mutex_**。
  void close_locked();

  /// 剪枝的实现。**调用方必须已经持有 mutex_**。
  int prune_locked();

  /// 把字段里不该出现的字符换掉。**调用方必须已经持有 mutex_**。
  ///
  /// 换掉三类：`|`（会伪造出多一个字段的行）、`\r` 和 `\n`（**会伪造出整行** ——
  /// NVR 把设备名设成 `"name\n2026-01-01 00:00:00 | ..."` 就能往审计里塞一条
  /// 假记录，而「设备名」正是我们记进 detail 的东西之一），以及所有 < 0x20
  /// 的控制字符。
  ///
  /// 这**不是**洁癖：审计的全部价值在于「事后能信它」，而一条能被写入者伪造的
  /// 审计记录，比没有审计更坏。
  static std::string sanitize(std::string const& s);

  /// 换日：文件名按天算，跨零点要换一个新文件。**调用方必须已经持有 mutex_**。
  bool reopen_for_today_locked();

  mutable std::mutex mutex_;
  std::string dir_;
  std::string path_;        ///< 当前写的那一份
  std::string day_;         ///< 它对应的 `YYYY-MM-DD`
  int keep_days_ = 180;
  bool enabled_ = false;
  std::FILE* file_ = nullptr;
};

/// `audit-YYYY-MM-DD.log` 这种名字里的日期部分；不是这种名字就返回空串。
///
/// 先过 `is_our_log_file`（唯一那个「是不是我们的文件」的判据），再把日期抠出来
/// —— 两道都走同一个函数，所以「剪枝认得的名字」和「清空删的名字」不可能不一致。
std::string audit_day_of(std::string const& name);

/// 本地时间的 `YYYY-MM-DD`。剪枝的基准和文件名的算法是同一个。
std::string today_string();

}  // namespace d2o
