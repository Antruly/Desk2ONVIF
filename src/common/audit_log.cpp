#include "common/audit_log.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "common/log.h"

namespace d2o {

namespace {

/// 日期 ↔ 天数序号的换算。
///
/// **不用 `mktime` 减秒数**：那条路会把夏令时算进来 —— 一个经过 DST 边界的
/// 差值会差一小时，而剪枝的判据正好落在「零点」上，于是 `keep_days = 1` 时
/// 会周期性多删一天（或者少删一天）。这个量必须与运行环境无关，所以用纯粹
/// 的日历算法（Howard Hinnant 的 `days_from_civil`，无分支无时区）。
long long days_from_civil(long long y, unsigned m, unsigned d) {
  y -= (m <= 2);
  long long const era = (y >= 0 ? y : y - 399) / 400;
  unsigned const yoe = static_cast<unsigned>(y - era * 400);
  unsigned const doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  unsigned const doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + static_cast<long long>(doe) - 719468;
}

void civil_from_days(long long z, long long& y, unsigned& m, unsigned& d) {
  z += 719468;
  long long const era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned const doe = static_cast<unsigned>(z - era * 146097);
  unsigned const yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  long long const yy = static_cast<long long>(yoe) + era * 400;
  unsigned const doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  unsigned const mp = (5u * doy + 2u) / 153u;
  d = doy - (153u * mp + 2u) / 5u + 1u;
  m = mp + (mp < 10u ? 3u : static_cast<unsigned>(-9));
  y = yy + (m <= 2 ? 1 : 0);
}

std::string two(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%02d", v);
  return b;
}

/// `YYYY-MM-DD` 用给定序号拼出来。
std::string day_string(long long ordinal) {
  long long y = 0;
  unsigned m = 0, d = 0;
  civil_from_days(ordinal, y, m, d);
  char b[16];
  std::snprintf(b, sizeof b, "%04lld-%02u-%02u", y, m, d);
  return b;
}

std::string file_name_of_day(std::string const& day) {
  return "audit-" + day + ".log";
}

/// 本地时间「现在」的日历三元组。
void local_now(std::tm& out) {
  std::time_t const t = std::time(nullptr);
  out = std::tm{};
#ifdef _WIN32
  ::localtime_s(&out, &t);
#else
  ::localtime_r(&t, &out);
#endif
}

/// 把一行拆成 5 段。**分隔符是 `" | "`（含两侧空格）**，不是裸的 `|` ——
/// 这样 detail 里万一真混进一个 `|`（比如手改过日志文件），它也只是字符，
/// 不会把后面所有字段整体挪位。拆不出 5 段就返回 false。
bool split_line(std::string const& line, std::string& day, std::string& time,
                std::string& source, std::string& identity, std::string& action,
                std::string& detail) {
  std::vector<std::string> parts;
  std::size_t pos = 0;
  for (int i = 0; i < 4; ++i) {
    std::size_t const at = line.find(" | ", pos);
    if (at == std::string::npos) return false;
    parts.push_back(line.substr(pos, at - pos));
    pos = at + 3;
  }
  parts.push_back(line.substr(pos));
  if (parts.size() != 5) return false;
  if (parts[0].size() < 19) return false;          // "YYYY-MM-DD HH:MM:SS"
  day = parts[0].substr(0, 10);
  time = parts[0].substr(11, 8);
  source = parts[1];
  identity = parts[2];
  action = parts[3];
  detail = parts[4];
  return true;
}

constexpr std::size_t kMaxLineBytes = 4096;

}  // namespace

std::string today_string() {
  std::tm tmv{};
  local_now(tmv);
  char b[16];
  std::snprintf(b, sizeof b, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                tmv.tm_mday);
  return b;
}

std::string audit_day_of(std::string const& name) {
  // 先过唯一那个「是不是我们的文件」的判据。剪枝与清空于是**不可能**对
  // 「哪些文件算我们的」有两种看法。
  if (!is_our_log_file(name)) return std::string();
  constexpr std::size_t kPrefixLen = 6;   // "audit-"
  constexpr std::size_t kDateLen = 10;
  if (name.size() != kPrefixLen + kDateLen + 4) return std::string();
  return name.substr(kPrefixLen, kDateLen);
}

AuditLog::~AuditLog() { close(); }

std::string AuditLog::sanitize(std::string const& s) {
  std::string out;
  out.reserve(s.size());
  for (char const c : s) {
    unsigned char const u = static_cast<unsigned char>(c);
    if (c == '|') {
      out += '/';                      // 会伪造出多一个字段
    } else if (u < 0x20 || u == 0x7f) {
      out += ' ';                      // 换行会**伪造出整行**
    } else {
      out += c;
    }
  }
  // 逐字节走的，而 UTF-8 的续字节都在 0x80 以上，所以中文不会被切坏。
  return out;
}

bool AuditLog::open(std::string const& dir, int keep_days, bool enabled, std::string& err) {
  std::lock_guard<std::mutex> lk(mutex_);
  // 换目录/换开关都走同一条清场路径 —— 但**必须走 `_locked` 那一份**：
  // 这里已经持着 `mutex_` 了，而公开的 `close()` 会再拿一次同一个非递归
  // 互斥量，当场死锁（见下面 `close_locked()` 上方那段）。
  close_locked();
  dir_ = dir;
  keep_days_ = keep_days < 0 ? 0 : keep_days;
  enabled_ = enabled;
  err.clear();
  if (!enabled_) return true;   // 关掉就是什么都不做，连目录都不建

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(dir_), ec);
  if (ec) {
    err = "建不了审计日志目录 " + dir_ + "：" + ec.message();
    return false;
  }
  // 启动时剪一次枝：一个常驻进程可能几天不重启，跨零点换文件那条路只管
  // 「今天的文件对不对」，管不了「昨天以前那些该不该还在」。
  prune_locked();   // 同上：持锁调 `_locked` 那一份
  if (!reopen_for_today_locked()) {
    err = "审计日志文件打不开：" + path_;
    return false;
  }
  return true;
}

bool AuditLog::reopen_for_today_locked() {
  std::string const day = today_string();
  if (file_ != nullptr && day == day_) return true;
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
  day_ = day;
  path_ = (std::filesystem::path(dir_) / file_name_of_day(day)).string();
  // "ab"：追加且二进制。文本模式下 Windows 会把 '\n' 写成 "\r\n"，而
  // 拆分那一侧只认 '\n' —— 多出来的 '\r' 会跑进 detail 的尾巴里。
  file_ = std::fopen(path_.c_str(), "ab");
  return file_ != nullptr;
}

// ---------------------------------------------------------------------------
// 为什么 `close()` / `prune()` 是「拿锁 + 转发」而不是「就是那几行」
//
// 这两个动作**既要从外面调**（恢复出厂前 `close()`、启动时 `prune()`），
// **又要从 `open()` 里面调** —— 而 `open()` 已经持着 `mutex_` 了。
// 直接互调会在同一个非递归 `std::mutex` 上重复加锁，当场死锁。
//
// 这不是假想的：`open()` 原本写的就是「拿锁 → `close()` → `prune()`」，
// 于是**整个程序在第一次进 `cmd_run` 时一声不响地停住** —— 0.05 秒 CPU、
// 没有输出、没有退出码、连要建的目录都没建、`--seconds` 永远不到期。
// 它编得过、`--selfcheck` 也过（自检不走 `cmd_run`），只有真的跑起来才发作。
//
// 所以纪律是：**需要「持锁地做某件事」时，把实现写成 `xxx_locked()`
// （前提写进注释），公开的 `xxx()` 只做「拿锁 + 转发」。**
// 与既有的 `reopen_for_today_locked()` 是同一个形状。
// ---------------------------------------------------------------------------
void AuditLog::close_locked() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
  // path_ / day_ 故意留着：`close()` 之后调用方多半还要删掉 path_ 再开回来，
  // 而「原来写在哪」只有这里知道（与 `Log::close_file()` 同一个理由）。
}

void AuditLog::close() {
  std::lock_guard<std::mutex> lk(mutex_);
  close_locked();
}

bool AuditLog::enabled() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return enabled_;
}

std::string AuditLog::dir() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return dir_;
}

int AuditLog::keep_days() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return keep_days_;
}

void AuditLog::note(AuditWho const& who, std::string const& action, std::string const& detail) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!enabled_) return;
  if (!reopen_for_today_locked()) return;   // 换日那一刻刚好写不出来：丢掉这一条，不抛

  std::tm tmv{};
  local_now(tmv);
  char head[32];
  std::snprintf(head, sizeof head, "%04d-%02d-%02d %02d:%02d:%02d",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

  std::string line;
  line.reserve(action.size() + detail.size() + who.source.size() + who.identity.size() + 48);
  line += head;
  line += " | ";
  // 空来源在审计里等于没有来源，所以给一个看得见的占位符而不是留空 ——
  // 留空的话，读日志的人分不出「这一条没有来源」和「解析器少拆了一段」。
  line += sanitize(who.source.empty() ? std::string("?") : who.source);
  line += " | ";
  line += sanitize(who.identity.empty() ? std::string("-") : who.identity);
  line += " | ";
  line += sanitize(action.empty() ? std::string("?") : action);
  line += " | ";
  line += sanitize(detail);
  line += '\n';

  std::fwrite(line.data(), 1, line.size(), file_);
  // 每行 fflush：审计最怕的就是「最后那几条在崩溃时没了」，而那正是要查的东西。
  std::fflush(file_);
}

int AuditLog::prune() {
  std::lock_guard<std::mutex> lk(mutex_);
  return prune_locked();
}

int AuditLog::prune_locked() {
  if (!enabled_ || keep_days_ <= 0) return 0;   // 0 = 永不剪枝

  std::tm tmv{};
  local_now(tmv);
  long long const today = days_from_civil(tmv.tm_year + 1900,
                                          static_cast<unsigned>(tmv.tm_mon + 1),
                                          static_cast<unsigned>(tmv.tm_mday));
  long long const cutoff = today - keep_days_;

  std::error_code ec;
  std::filesystem::directory_iterator it(dir_, ec);
  if (ec) return 0;

  int removed = 0;
  for (auto const& ent : it) {
    std::error_code ec2;
    if (!ent.is_regular_file(ec2) || ec2) continue;
    std::string const name = ent.path().filename().string();
    // **同一个判据**（`is_our_log_file`）在剪枝和清空两处都过一遍；
    // 不认路径，所以「删错目录」这件事在这个函数里不可能发生。
    std::string const day = audit_day_of(name);
    if (day.empty()) continue;
    long long const ord = days_from_civil(std::stoi(day.substr(0, 4)),
                                          static_cast<unsigned>(std::stoi(day.substr(5, 2))),
                                          static_cast<unsigned>(std::stoi(day.substr(8, 2))));
    if (ord >= cutoff) continue;   // 今天 - keep_days 这一天本身还留着
    std::error_code ec3;
    if (std::filesystem::remove(ent.path(), ec3) && !ec3) ++removed;
  }
  return removed;
}

int AuditLog::clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  // **先关**：Windows 的 fopen 不带 FILE_SHARE_DELETE，句柄开着就删不掉
  // （Permission denied），而症状恰好是那件事最不该出错的地方。
  //
  // 走 `close_locked()` 而不是在这里把 `fclose` 再抄一遍 —— 抄一遍本身无害，
  // 但它正是上面那个死锁没被发现的**原因**：写 `clear()` 的人（正确地）
  // 没有去调公开的 `close()`，于是这段代码看起来「知道要绕开锁」，
  // 而 `open()` 那边照旧互调也没人起疑。
  close_locked();

  int removed = 0;
  std::error_code ec;
  std::filesystem::directory_iterator it(dir_, ec);
  if (!ec) {
    for (auto const& ent : it) {
      std::error_code ec2;
      if (!ent.is_regular_file(ec2) || ec2) continue;
      std::string const name = ent.path().filename().string();
      if (audit_day_of(name).empty()) continue;
      std::error_code ec3;
      if (std::filesystem::remove(ent.path(), ec3) && !ec3) ++removed;
    }
  }

  // 开回来。**目录本身不删** —— 「只删我们自己的文件，不递归删目录」。
  if (enabled_) reopen_for_today_locked();
  return removed;
}

AuditStats AuditLog::stats() const {
  std::string dir;
  int keep = 0;
  bool on = false;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    dir = dir_;
    keep = keep_days_;
    on = enabled_;
  }

  AuditStats st;
  st.enabled = on;
  st.keep_days = keep;
  st.dir = dir;
  if (dir.empty()) return st;

  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) return st;
  for (auto const& ent : it) {
    std::error_code ec2;
    if (!ent.is_regular_file(ec2) || ec2) continue;
    if (audit_day_of(ent.path().filename().string()).empty()) continue;
    ++st.files;
    std::error_code ec3;
    auto const sz = std::filesystem::file_size(ent.path(), ec3);
    if (!ec3) st.bytes += static_cast<long long>(sz);
  }
  return st;
}

std::vector<AuditEntry> AuditLog::read(int days, int limit, std::string const& keyword,
                                       std::string& err) const {
  std::string dir;
  bool enabled = false;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    dir = dir_;
    enabled = enabled_;
  }
  err.clear();
  std::vector<AuditEntry> out;
  if (dir.empty()) return out;
  if (days < 1) days = 1;
  if (days > 400) days = 400;      // 上限只是防呆：文件名是算出来的，不扫目录
  if (limit < 1) limit = 1;
  if (limit > 5000) limit = 5000;

  // **先看一眼目录在不在，再逐天去算文件名。**
  //
  // 下面那个循环**不扫目录**（理由见循环上方那段），代价是它对「看不了」这件事
  // 完全无感：`is_regular_file(p, ec)` 在**每一级父目录都不可达**时也是 `ec != 0`，
  // 而那一支是 `continue` —— 于是整趟读回来一个**空列表、且 err 是空的**，界面上
  // 就报成「这段时间没有任何操作记录」。**这正是审计最不能出错的那个方向**：
  // 「没记」和「记了但我读不到」对着一件取证的事是两个完全相反的答案，而用户会据此
  // 下结论（"那几天没人动过配置"）。它是陷阱 91 的同一族 —— 一个被吞掉的失败和
  // "确实什么都没有"长得一模一样。
  //
  // 挂着 `enabled_ == true` 而这里判不过的两种**真的会发生**的情形：
  //   1. `open()` 自己失败过（最常见的是 `create_directories` 那一步 —— 那个位置
  //      被一个**同名文件**占了，或者配置目录不可写）。这时 `enabled_` / `dir_` 已经
  //      设好、目录却没建起来，`note()` 从此一条都写不出去，而 `read()` 以前会说
  //      "一切正常，只是没有记录"。
  //   2. 启动时建出来了，之后目录的**权限被改掉**、或者它所在的盘 / 网络共享掉了。
  //
  // ⚠️ **不是为"有人把目录删掉"写的** —— 那个场景在本进程里到不了：只要审计开着，
  // `note()` 就会 `reopen_for_today_locked()` 把当天的文件一直开在那里，而 `fopen(...,"ab")`
  // 的共享模式没有 `FILE_SHARE_DELETE`，Windows 因此**拒绝删除或改名这个目录**
  // （实测 `Directory.Delete(dir, true)` 报 "being used by another process"、
  // `Directory.Move` 报 "Access is denied"）。写在这里免得下一个人按一个到不了的
  // 场景去"修"它，也免得以为这条判据只在那种场景下有用。
  //
  // 只在 `enabled` 时判：关掉审计时 `dir_` 也是设着的（见 `open()`），
  // 而那种情况下「目录不存在」是**正常的**，报错反而把「没在记」说成「读不出来」。
  if (enabled) {
    std::error_code dec;
    bool const is_dir = std::filesystem::is_directory(dir, dec);
    if (!is_dir || dec) {
      err = "审计日志目录打不开或不存在：" + dir;
      if (dec) err += "（" + dec.message() + "）";
      return out;
    }
  }

  std::tm tmv{};
  local_now(tmv);
  long long const today = days_from_civil(tmv.tm_year + 1900,
                                          static_cast<unsigned>(tmv.tm_mon + 1),
                                          static_cast<unsigned>(tmv.tm_mday));

  // 从今天往前一天一天地算文件名。**不扫目录**：这样「读哪些文件」和
  // 「剪枝删哪些文件」用的是同一个日期算法，而扫目录会顺带把不该看的
  // （别的程序放进来、名字恰好像的那种）也读进来。
  for (int back = 0; back < days && static_cast<int>(out.size()) < limit; ++back) {
    std::string const day = day_string(today - back);
    std::filesystem::path const p =
        std::filesystem::path(dir) / file_name_of_day(day);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec) || ec) continue;

    std::ifstream in(p, std::ios::binary);
    if (!in) {
      err = "审计日志读不出来：" + p.string();
      continue;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      if (line.size() > kMaxLineBytes) line.resize(kMaxLineBytes);
      lines.push_back(std::move(line));
    }
    // 文件里是追加顺序（旧的在前），而界面上要**新的在前**。
    for (auto rit = lines.rbegin(); rit != lines.rend(); ++rit) {
      if (static_cast<int>(out.size()) >= limit) break;
      if (!keyword.empty() && rit->find(keyword) == std::string::npos) continue;
      AuditEntry e;
      e.day = day;   // 拆不出来时日期也还有：它来自文件名，不是来自那一行
      e.raw = *rit;
      std::string d, t, src, id, act, det;
      if (split_line(*rit, d, t, src, id, act, det)) {
        e.day = d;
        e.time = t;
        e.source = src;
        e.identity = id;
        e.action = act;
        e.detail = det;
      } else {
        // 手改过的、或者将来格式变过的那一份：**照样给出去**，把整行放进
        // detail，而不是静默丢掉 —— 审计里「少了一条」比「有一条读不懂」坏。
        e.detail = *rit;
      }
      out.push_back(std::move(e));
    }
  }
  return out;
}

}  // namespace d2o
