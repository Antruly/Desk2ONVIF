#include "web/web_session.h"

#include "common/log.h"
#include "onvif/digest.h"

#include <openssl/rand.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace d2o {
namespace {

/// `nbytes` 个随机字节，返回 2*nbytes 个小写十六进制字符。
///
/// 失败时**不静默退化**：RAND_bytes 拿不到熵意味着令牌的签名密钥可猜，
/// 那等于没有鉴权。这种情况几乎只会在系统严重异常时出现，所以这里既报错
/// 又继续跑（比起拒绝启动，"能连上但门锁是坏的"至少用户能进去改配置）。
std::string random_hex(std::size_t nbytes) {
  static char const* const kHex = "0123456789abcdef";
  std::string out;
  out.reserve(nbytes * 2);

  unsigned char buf[32];
  std::size_t left = nbytes;
  bool ok = true;
  while (left > 0) {
    std::size_t const chunk = std::min<std::size_t>(left, sizeof(buf));
    if (RAND_bytes(buf, static_cast<int>(chunk)) != 1) {
      ok = false;
      // 退化的来源：时间 + 地址，够随机才有鬼，但比一串常量强。
      std::uint64_t const t = static_cast<std::uint64_t>(::time(nullptr));
      for (std::size_t i = 0; i < chunk; ++i) {
        buf[i] = static_cast<unsigned char>((t >> ((i % 8) * 8)) ^ (i * 31 + nbytes));
      }
    }
    for (std::size_t i = 0; i < chunk; ++i) {
      out.push_back(kHex[buf[i] >> 4]);
      out.push_back(kHex[buf[i] & 0x0f]);
    }
    left -= chunk;
  }
  if (!ok) {
    D2O_ERROR("取不到密码学随机数，登录令牌的签名密钥强度已下降 —— 请检查系统状态。");
  }
  return out;
}

/// 十六进制 → uint64。非法输入返回 false（调用方一律当令牌无效）。
bool parse_hex_u64(std::string const& s, std::uint64_t& out) {
  if (s.empty() || s.size() > 16) return false;
  std::uint64_t v = 0;
  for (char const c : s) {
    unsigned d;
    if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
    else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
    else return false;
    v = (v << 4) | d;
  }
  out = v;
  return true;
}

std::string to_hex_u64(std::uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

std::uint64_t now_seconds() { return static_cast<std::uint64_t>(::time(nullptr)); }

}  // namespace

SessionStore::SessionStore() : secret_(random_hex(32)) {}
SessionStore::~SessionStore() = default;

void SessionStore::set_credentials(std::string user, std::string password) {
  // 空用户名会让"用户名对不对"这一半的校验失去意义（谁都填得中）。
  // 调用方（validate_config）已经保证非空，这里只兜一次底。
  user_ = user.empty() ? std::string("admin") : std::move(user);
  password_ = std::move(password);
}

bool SessionStore::check_password(std::string const& user,
                                  std::string const& pass) const {
  if (!effective()) return false;
  // **两个比较都要做，不要用 `&&` 短路。** 短路的话用户名一错就返回，
  // 于是"用户名对不对"会从耗时上泄漏出去 —— 虽然危害小得多，但既然
  // 常数时间比较已经写了，就没理由再留一个短路的口子。
  bool const u = secure_equals(user, user_);
  bool const p = secure_equals(pass, password_);
  return u && p;
}

std::string SessionStore::sign(std::uint64_t expiry) const {
  // 只签过期时间，不签用户名。理由：这个服务只有**一个**账户，"这份令牌是
  // 谁的"没有意义；不绑用户名换来的是"改用户名不会把已登录的人踢掉"，
  // 与改口令的行为一致。
  return md5_hex(to_hex_u64(expiry) + ":" + secret_);
}

std::string SessionStore::issue(std::uint64_t now_s) const {
  std::uint64_t const expiry = now_s + kTtlSeconds;
  return to_hex_u64(expiry) + "." + sign(expiry);
}

bool SessionStore::verify(std::string const& token, std::uint64_t now_s) const {
  // 令牌里有 '.' 是唯一的结构约定。**先按第一个 '.' 切**，签名部分本身
  // 是纯十六进制不含点，所以怎么切都一样；多出来的点会让签名长度不符，
  // 下面的比较自然失败。
  auto const dot = token.find('.');
  if (dot == std::string::npos) return false;

  std::uint64_t expiry = 0;
  if (!parse_hex_u64(token.substr(0, dot), expiry)) return false;
  if (expiry <= now_s) return false;  // 过期（含恰好到期）

  std::string const want = sign(expiry);
  std::string const got = token.substr(dot + 1);
  if (want.size() != got.size()) return false;
  return secure_equals(got, want);
}

bool SessionStore::throttled(std::string const& ip, std::uint64_t now_s) {
  auto const it = attempts_.find(ip);
  if (it == attempts_.end()) return false;
  return it->second.until_s > now_s;
}

std::uint64_t SessionStore::lock_remaining(std::string const& ip,
                                           std::uint64_t now_s) {
  auto const it = attempts_.find(ip);
  if (it == attempts_.end() || it->second.until_s <= now_s) return 0;
  return it->second.until_s - now_s;
}

void SessionStore::note_failure(std::string const& ip, std::uint64_t now_s) {
  Attempts& a = attempts_[ip];
  // 窗口外重新计数：否则一台机器几个月里零星手滑五次就会被锁，
  // 而那时它完全想不起来上一次是什么时候。
  if (a.first_s == 0 || now_s - a.first_s > kWindowSeconds) {
    a.count = 0;
    a.first_s = now_s;
  }
  ++a.count;
  if (a.count >= kMaxFailures) {
    a.until_s = now_s + kLockSeconds;
    D2O_WARN("Web 控制台登录：%s 在 %llu 秒内失败 %u 次，冷却 %llu 秒。", ip.c_str(),
             static_cast<unsigned long long>(now_s - a.first_s), a.count,
             static_cast<unsigned long long>(kLockSeconds));
  }
}

void SessionStore::note_success(std::string const& ip) { attempts_.erase(ip); }

}  // namespace d2o
