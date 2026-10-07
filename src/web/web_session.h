#pragma once

/**
 * @file src/web/web_session.h
 * @brief Web 控制台的登录会话：无状态签名令牌 + 常数时间口令校验 + 失败限速。
 *
 * ## 为什么是"无状态令牌"而不是一张会话表
 *
 * 令牌形如 `<过期时间(16进制)>.<md5(过期时间:进程密钥)>`，校验时**只算一次
 * 摘要**，不查任何表。会话表要么得配清理线程（不清理就是慢性内存泄漏，而
 * 泄漏速度取决于有多少人登录过），要么得在每次请求上抢锁 —— 而 Web 循环是
 * 单线程的，这个锁纯粹是自找的。签名令牌把"服务端要记住的东西"降到零。
 *
 * 代价是**没法单独吊销某一个会话**（改口令也不会踢人，见 `set_credentials`）。
 * 对这个场景是划算的：真正的"全部踢掉"手段是重启进程 —— 密钥是每进程一份
 * 的随机数，重启即全失效。
 *
 * ## 为什么口令比较要常数时间
 *
 * `==` 在第一个不同的字节上就返回，于是"错 0 个字符"和"错 8 个字符"的耗时
 * 差得出一个可测的信号，逐字节就能把口令猜出来。局域网里延迟低而稳定，
 * 这类攻击在这里比在公网上更实用，不是学术问题。
 */

#include <cstdint>
#include <string>
#include <unordered_map>

namespace d2o {

/// 会话 cookie 的名字。**改名等于把所有人的登录状态清空一次**。
inline constexpr char const* kSessionCookie = "d2o_session";

class SessionStore {
 public:
  /// 令牌有效期：7 天。
  ///
  /// 这是个局域网设备，不是网银。有效期定得太短（几小时）的实际后果是用户
  /// 把口令交给浏览器密码管理器记住 —— 那比一个 7 天的 cookie 更该担心。
  static constexpr std::uint64_t kTtlSeconds = 7ull * 24 * 3600;

  /// 一个 IP 在 `kWindowSeconds` 内失败满 `kMaxFailures` 次之后，冷却
  /// `kLockSeconds` 秒。取值原则：正常人手滑几次不该被挡住（5 次够多），
  /// 但脚本爆破的速率被压到约每分钟一次。
  static constexpr unsigned kMaxFailures = 5;
  static constexpr std::uint64_t kWindowSeconds = 300;
  static constexpr std::uint64_t kLockSeconds = 60;

  SessionStore();
  ~SessionStore();
  SessionStore(SessionStore const&) = delete;
  SessionStore& operator=(SessionStore const&) = delete;

  /// 换用户名与口令。
  ///
  /// **已经发出去的令牌不受影响。** 令牌签的是 `secret_`（每进程一份的随机
  /// 密钥）而不是口令，所以改口令不会把任何人的会话作废 —— 包括改口令的这个
  /// 人自己。这是刻意的：把人当场踢下线，他再登录一次也就好了，但那一刻他
  /// 会以为"改口令把界面改坏了"，而这类误会会让人不敢再动口令。
  void set_credentials(std::string user, std::string password);

  void set_enabled(bool on) { enabled_ = on; }

  /// 配置里写的开关。
  bool enabled() const { return enabled_; }

  /// **这个服务现在是否真的在拦人。** 开关开着但口令是空的时候返回 false ——
  /// 空口令不是"不鉴权"而是"口令就是空"，任何知道用户名的人都能过。与其
  /// 摆一道假门，不如承认没门，由调用方去打那条 WARN。
  bool effective() const { return enabled_ && !password_.empty(); }

  std::string const& user() const { return user_; }

  /// 校验用户名与口令。启用状态下**必须走这条路**，不要自己写 `==`。
  bool check_password(std::string const& user, std::string const& pass) const;

  /// 签发一个新令牌。
  std::string issue(std::uint64_t now_s) const;

  /// 校验令牌：签名对得上，且没过期。格式不对一律当无效。
  bool verify(std::string const& token, std::uint64_t now_s) const;

  /// 该 IP 现在是不是正处在冷却里。
  bool throttled(std::string const& ip, std::uint64_t now_s);
  /// 记一次失败（并在越过阈值时进入冷却）。
  void note_failure(std::string const& ip, std::uint64_t now_s);
  /// 记一次成功：把该 IP 的失败计数清掉。
  void note_success(std::string const& ip);

  /// 冷却剩余秒数，只给日志用。
  std::uint64_t lock_remaining(std::string const& ip, std::uint64_t now_s);

 private:
  std::string sign(std::uint64_t expiry) const;

  struct Attempts {
    unsigned count = 0;
    std::uint64_t first_s = 0;
    std::uint64_t until_s = 0;
  };

  bool enabled_ = true;
  std::string user_ = "admin";
  std::string password_ = "admin";
  /// 每进程一份的随机签名密钥。**进程重启 = 所有会话立刻失效。**
  std::string secret_;
  /// key 是来源 IP。**按 IP 而不是全局**：全局计数意味着局域网里任何一台
  /// 机器都能靠连续失败把所有人锁在门外，那把限速本身变成了攻击手段。
  std::unordered_map<std::string, Attempts> attempts_;
};

}  // namespace d2o
