/**
 * @file tests/ntp_check.cpp
 * @brief `ntp_client` 的解码：纪元常数、字节序、四种结局的分类。
 *
 * ## 为什么这件事非要单元测
 *
 * 端到端那条路（`tests\ntp_probe.py` + `/api/time/sync`）量得到的是**结果**：
 * 偏移对不对、状态串是什么。它量不到的是**常数**——因为真机上的偏移是"差不多
 * 对"，判据只能写成"接近某个值"，而**纪元换算错一年，偏移就错 31536000 秒**，
 * 这种量级的错在"接近"这个词下会当场通过。
 *
 * 这里的判据是**精确相等**：假服务器的时间戳按 `本机时钟 + N` 编码，解码回来
 * 必须**恰好**是 N。N 取 0 / +300 / −3600 / +86400 四个值，覆盖正、负、零、
 * 以及**整整一天** —— 后面那个是故意挑的：纪元常数写错 86400 的整数倍差，
 * 或者偏移的**量纲**从秒错成天，都会在那一条上露馅。
 *
 * ## 为什么服务器写在进程内，而不是连 `tests\ntp_probe.py`
 *
 * 因为这个文件的价值恰恰在于**它可以自己跑**。一个必须由夹具起头才有意义的
 * 单元测试，在夹具被跳过的时候不会有任何人发现——本项目里"没有调用者的代码
 * 等于没验过的代码"这条纪律，在测试身上的写法就是"没有入口的断言等于没有
 * 断言"。所以 `build\ntp_check.exe` 不带参数就该能绿或该红。
 *
 * 顺带换来一件更值钱的事：**编码器有两份、且互不相干**。这一份在 C++ 里手写
 * （`put_stamp` 直接按 `2208988800.0` 加、按大端铺字节），`ntp_probe.py` 里那份
 * 是 Python 手写的。两份**独立实现**编码、同一个 `ntp_client` 解码 —— 如果
 * 产品把纪元常数写错了，两份都会报错；如果只有一份写错了，那也只是那条路红。
 * 反过来说，**共用一份实现的两个断言只是在验"同一个错被犯了两次"**，那才是
 * 真正没有覆盖的东西。
 *
 * ## 四种结局每一路都有一个"同伴断言"
 *
 * 光断言 `state == kTimeout` 是不够的：**"服务器收到了但没回"和"我们根本没发
 * 出去"报的是同一个 state**，而这两件事要修的地方完全不同。所以每一种失败都
 * 配一条同伴 —— `served() >= 1`（包真的送到过服务器手上）；而每一条失败还都要
 * 断言 `error` 非空（一个什么都不说的失败，和"这条分支没跑到"在报告上长得
 * 一模一样，见陷阱 46/91）。
 *
 * ⚠️ **`kTimeout` 必须靠"收到了、但不回"来造**（`kSilent`）：本机回环上没人
 * 监听时，Windows 会给一个 ICMP 端口不可达，而 `connect()` 过的 UDP 套接字把它
 * 报成 `WSAECONNRESET` ⇒ `ntp_client` 归到 `kUnreachable`。所以"不起服务器"
 * 造出来的是 `unreachable`，不是 `timeout` —— 这条是实测出来的，写在
 * `tests\ntp_probe.py` 的文件头里。
 *
 * ## 绑 123 端口失败只报告、不判（陷阱 87）
 *
 * 这个测试需要 127.0.0.1:123（`ntp_query` 把端口写死成 123）。Windows 上非管理员
 * 就能绑，实测过。但要是这台机器上真有别的东西（nssm、其它 NTP 服务、上一次
 * 没退干净的进程）占着它，**红的是环境不是产品** —— 所以那种情况打印 `[SKIP]`
 * 并说明原因，退出码仍然是 0。判据依赖了一个没写进前提的环境条件时，它只能
 * 报告，不能判。
 *
 * 编译（在项目根目录下跑；`ntp_client.cpp` 自己没有日志调用，不需要 log.cpp）：
 *   g++ -std=c++17 -Wall -Wextra -Isrc tests/ntp_check.cpp \
 *       src/common/ntp_client.cpp -o build/ntp_check.exe -lws2_32
 *
 * 运行（不需要参数，也不需要管理员）：
 *   ./build/ntp_check.exe
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/ntp_client.h"

namespace {

int g_failed = 0;
int g_passed = 0;
int g_skipped = 0;

void check(bool ok, std::string const& what, std::string const& detail = {}) {
  if (ok) {
    ++g_passed;
    std::printf("  [OK]   %s\n", what.c_str());
  } else {
    ++g_failed;
    std::printf("  [FAIL] %s\n", what.c_str());
    if (!detail.empty()) std::printf("         %s\n", detail.c_str());
  }
  std::fflush(stdout);
}

std::string human(int v) { return std::to_string(v); }

// ---------------------------------------------------------------------------
// 假 SNTP 服务器（进程内）
// ---------------------------------------------------------------------------

/// 把「自 Unix 纪元起的秒数」铺成 8 字节的 NTP 定标数。
///
/// **这是被测代码之外的第二份实现**，故意不复用 `ntp_client.cpp` 里那个
/// `put_ntp_timestamp`（它在匿名命名空间里，本来也调不到——这正是独立性的
/// 现成保障）。这里用 `2208988800.0` 这个**字面量**，而不是 `kNtpUnixEpochDelta`：
/// 引用产品的常数会让这条测试在常数被改坏时**跟着一起错**，那就什么都不验了。
void put_stamp(std::uint8_t* p, double unix_s, bool big_endian) {
  double const v = unix_s + 2208988800.0;
  std::uint32_t const sec = static_cast<std::uint32_t>(v);
  std::uint32_t const frac =
      static_cast<std::uint32_t>((v - static_cast<double>(sec)) * 4294967296.0);
  std::uint8_t const be[8] = {
      static_cast<std::uint8_t>(sec >> 24), static_cast<std::uint8_t>(sec >> 16),
      static_cast<std::uint8_t>(sec >> 8),  static_cast<std::uint8_t>(sec),
      static_cast<std::uint8_t>(frac >> 24), static_cast<std::uint8_t>(frac >> 16),
      static_cast<std::uint8_t>(frac >> 8),  static_cast<std::uint8_t>(frac)};
  if (big_endian) {
    std::memcpy(p, be, 8);
  } else {
    // 故意写反：用来证明解码端**真的在看字节序**，而不是恰好两种都一样。
    for (int i = 0; i < 4; ++i) {
      p[i] = be[3 - i];
      p[4 + i] = be[7 - i];
    }
  }
}

class FakeServer {
 public:
  enum Mode {
    kNormal,      ///< 正常应答 → 客户端应报 ok
    kSilent,      ///< 收下但不回 → 唯一能确定造出 timeout 的办法
    kShort,       ///< 只回 20 字节 → bad_reply（回包太短）
    kZeroStamps,  ///< T2/T3 全零 → bad_reply（没有时间戳）
  };

  struct Cfg {
    Mode mode = kNormal;
    int stratum = 2;
    int offset_s = 0;
    bool big_endian = true;
  };

  ~FakeServer() { stop(); }

  bool start(Cfg const& cfg, std::string& err) {
    cfg_ = cfg;
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
      err = "socket() 失败，错误码 " + human(::WSAGetLastError());
      return false;
    }
    BOOL const yes = TRUE;
    ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<char const*>(&yes), sizeof(yes));
    DWORD const rcv_to = 100;  // 让 stop() 能及时收手
    ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<char const*>(&rcv_to), sizeof(rcv_to));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = ::htons(123);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::bind(sock_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) ==
        SOCKET_ERROR) {
      int const e = ::WSAGetLastError();
      err = "bind(127.0.0.1:123) 失败，错误码 " + human(e);
      ::closesocket(sock_);
      sock_ = INVALID_SOCKET;
      return false;
    }
    th_ = std::thread([this] { loop(); });
    return true;
  }

  void stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    if (sock_ != INVALID_SOCKET) {
      ::closesocket(sock_);
      sock_ = INVALID_SOCKET;
    }
  }

  /// 服务器**收到过几个**请求。它是 timeout 那一路的同伴断言：见文件头。
  int served() const { return served_.load(); }
  /// 收到的第一个请求的首字节（LI/VN/Mode），没收到时是 -1。
  int first_byte() const { return first_byte_.load(); }
  int request_len() const { return request_len_.load(); }

 private:
  void loop() {
    while (!stop_.load()) {
      std::uint8_t req[512];
      sockaddr_in peer{};
      int plen = sizeof(peer);
      int const rc = ::recvfrom(sock_, reinterpret_cast<char*>(req), sizeof(req),
                                0, reinterpret_cast<sockaddr*>(&peer), &plen);
      if (rc == SOCKET_ERROR) continue;  // 超时，回去看 stop_
      if (served_.fetch_add(1) == 0) {
        first_byte_ = req[0];
        request_len_ = rc;
      }
      if (cfg_.mode == kSilent) continue;

      double const now = std::chrono::duration<double>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
      std::uint8_t rep[48] = {};
      rep[0] = 0x24;  // LI=0 / VN=4 / Mode=4（server）
      rep[1] = static_cast<std::uint8_t>(cfg_.stratum);
      // 24..31 Originate：把客户端那 8 字节原样抄回去（真服务器就是这么做的）。
      if (rc >= 48) std::memcpy(rep + 24, req + 40, 8);
      if (cfg_.mode != kZeroStamps) {
        put_stamp(rep + 32, now + cfg_.offset_s, cfg_.big_endian);
        put_stamp(rep + 40, now + cfg_.offset_s, cfg_.big_endian);
      }
      int const len = (cfg_.mode == kShort) ? 20 : 48;
      ::sendto(sock_, reinterpret_cast<char*>(rep), len, 0,
               reinterpret_cast<sockaddr*>(&peer), plen);
    }
  }

  Cfg cfg_{};
  SOCKET sock_ = INVALID_SOCKET;
  std::thread th_;
  std::atomic<bool> stop_{false};
  std::atomic<int> served_{0};
  std::atomic<int> first_byte_{-1};
  std::atomic<int> request_len_{0};
};

}  // namespace

int main() {
  WSADATA wsa{};
  if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::printf("WSAStartup 失败\n");
    return 1;
  }

  std::printf("=== [1] 常数与状态串的字面量 ===\n");
  // 这两个都是**把期望值重写一遍**的断言，看着笨，但它们的宾语是"产品的源码
  // 有没有被改过"：常数一改，偏移全线差 70 年；状态串一改，前端那份白名单
  // （`SYNC_FAIL_STATES = ['timeout','bad_reply','unreachable']`）就悄悄失效。
  check(d2o::kNtpUnixEpochDelta == 2208988800u,
        "kNtpUnixEpochDelta == 2208988800（1900→1970 整整 70 年）",
        "实际 " + human(static_cast<int>(d2o::kNtpUnixEpochDelta)));
  struct {
    d2o::NtpState s;
    char const* name;
  } const names[] = {
      {d2o::NtpState::kOk, "ok"},
      {d2o::NtpState::kTimeout, "timeout"},
      {d2o::NtpState::kBadReply, "bad_reply"},
      {d2o::NtpState::kUnreachable, "unreachable"},
  };
  for (auto const& n : names) {
    check(std::strcmp(d2o::ntp_state_name(n.s), n.name) == 0,
          std::string("状态串 ") + n.name + "（直接进 last_sync_state，前端按它判颜色）",
          std::string("实际 ") + d2o::ntp_state_name(n.s));
  }

  // 先探一次端口：绑不上就整段跳过（陷阱 87）。
  {
    FakeServer probe;
    std::string err;
    if (!probe.start(FakeServer::Cfg{}, err)) {
      std::printf("\n[SKIP] 绑不上 127.0.0.1:123：%s\n", err.c_str());
      std::printf("       这一条测的是环境不是产品，所以不判。\n");
      std::printf("       （Windows 上非管理员本来就能绑 —— 有空着它的别的程序吗？）\n");
      ++g_skipped;
      ::WSACleanup();
      std::printf("\n通过 %d 项，失败 %d 项，跳过 %d 项。\n", g_passed, g_failed,
                  g_skipped);
      return 0;
    }
  }

  std::printf("\n=== [2] 请求的形状 ===\n");
  {
    FakeServer::Cfg cfg;
    cfg.offset_s = 0;
    FakeServer srv;
    std::string err;
    if (srv.start(cfg, err)) {
      d2o::NtpResult const r = d2o::ntp_query("127.0.0.1", 1500);
      int const fb = srv.first_byte();
      int const rl = srv.request_len();
      srv.stop();
      check(r.state == d2o::NtpState::kOk, "正常应答判为 ok",
            "实际 " + std::string(d2o::ntp_state_name(r.state)) + "：" + r.error);
      check(rl == 48, "请求是 48 字节", "实际 " + human(rl) + " 字节");
      // LI/VN/Mode 的低三位是 Mode：3 = client。真 NTP 服务器看到 0（保留）或
      // 4（server）是会直接丢掉的，而"服务器不理我们"报出来就是 timeout ——
      // 归因会指向网络。
      check(fb >= 0 && (fb & 0x07) == 3,
            "请求 Mode 位是 3（client）", "首字节 0x" + human(fb < 0 ? 0 : fb));
      check(fb >= 0 && ((fb >> 3) & 0x07) >= 3,
            "请求版本号 >= 3", "首字节 0x" + human(fb < 0 ? 0 : fb));
    }
  }

  std::printf("\n=== [3] 偏移：精确相等（纪元常数写错会当场抓住）===\n");
  struct {
    int offset;
    char const* why;
  } const cases[] = {
      {0, "零偏移 —— 不能有系统性偏差"},
      {300, "正偏移"},
      {-3600, "负偏移（方向不能反）"},
      {86400, "整整一天 —— 量纲从秒错成天、或纪元差一天，都在这一条上露馅"},
  };
  for (auto const& c : cases) {
    FakeServer::Cfg cfg;
    cfg.offset_s = c.offset;
    FakeServer srv;
    std::string err;
    if (!srv.start(cfg, err)) {
      check(false, std::string("起服务器失败：") + err);
      continue;
    }
    d2o::NtpResult const r = d2o::ntp_query("127.0.0.1", 1500);
    int const served = srv.served();
    srv.stop();
    check(served >= 1, std::string("服务器收到了请求（") + c.why + "）",
          "served = " + human(served));
    check(r.state == d2o::NtpState::kOk,
          std::string("offset ") + human(c.offset) + " 判为 ok：" + c.why,
          std::string("实际 ") + d2o::ntp_state_name(r.state) + "：" + r.error);
    check(r.offset_s == c.offset,
          std::string("offset_s **恰好** == ") + human(c.offset) + "：" + c.why,
          "实际 " + human(r.offset_s));
    // 小数详情也要对得上。只看量化后的整数的话，"t2/t3 取错字节、恰好凑出
    // 一个相近的整数"这种错会被放过。
    double const d = r.raw_offset_s - static_cast<double>(c.offset);
    check(d > -0.5 && d < 0.5,
          std::string("raw_offset_s 也在 ") + human(c.offset) + " 附近（±0.5s）",
          "实际 " + std::to_string(r.raw_offset_s));
  }

  std::printf("\n=== [4] 字节序（诊断用，不单独成立）===\n");
  {
    FakeServer::Cfg cfg;
    cfg.offset_s = 300;
    cfg.big_endian = false;
    FakeServer srv;
    std::string err;
    if (srv.start(cfg, err)) {
      d2o::NtpResult const r = d2o::ntp_query("127.0.0.1", 1500);
      srv.stop();
      long long const d = static_cast<long long>(r.offset_s) - 300LL;
      // ⚠️ **这一条不会单独红，别把它当成第二道防线。** 实测过（把
      // `get_ntp_timestamp` 改成小端读取、重编）：[3] 八条和这一条**一起**红
      // —— 因为"大端服务器给出 N"这一步本身就已经把字节序钉死了，反过来
      // 也一样，不存在"两种字节序都糊得过去"的解码实现。
      //
      // 留着它的理由是**失败时的可读性**：解码端写反时，[3] 报出来的是
      // `offset_s = -1311768576，期望 300` 这种数，看的人要去猜为什么；
      // 这一条直接把原因打在脸上。陷阱 46 那一族 —— 失败信息指错方向
      // 比不报还费时间。
      check(d > 1000 || d < -1000,
            "时间戳字节序写反时，偏移**不是** 300（诊断：帮 [3] 说出原因）",
            "实际 " + human(r.offset_s) + "（差 " + human(static_cast<int>(d)) + "）");
    }
  }

  std::printf("\n=== [5] 四种结局分类 ===\n");
  {
    struct {
      char const* label;
      FakeServer::Cfg cfg;
      d2o::NtpState expect;
    } const cases[] = {
        {"stratum=0（KoD，服务器拒绝服务）",
         FakeServer::Cfg{FakeServer::kNormal, 0, 0, true},
         d2o::NtpState::kBadReply},
        {"stratum=16（自报未同步）",
         FakeServer::Cfg{FakeServer::kNormal, 16, 0, true},
         d2o::NtpState::kBadReply},
        {"回包只有 20 字节",
         FakeServer::Cfg{FakeServer::kShort, 2, 0, true},
         d2o::NtpState::kBadReply},
        {"T2/T3 全是零",
         FakeServer::Cfg{FakeServer::kZeroStamps, 2, 0, true},
         d2o::NtpState::kBadReply},
    };
    for (auto const& c : cases) {
      FakeServer srv;
      std::string err;
      if (!srv.start(c.cfg, err)) {
        check(false, std::string("起服务器失败：") + err);
        continue;
      }
      d2o::NtpResult const r = d2o::ntp_query("127.0.0.1", 1500);
      int const served = srv.served();
      srv.stop();
      check(r.state == c.expect,
            std::string(c.label) + " → " + d2o::ntp_state_name(c.expect),
            std::string("实际 ") + d2o::ntp_state_name(r.state));
      check(!r.error.empty(), std::string(c.label) + " 说了人话（error 非空）");
      check(served >= 1, std::string(c.label) + "：请求确实送到过服务器",
            "served = " + human(served));
    }
  }

  std::printf("\n=== [6] timeout 与 unreachable 是两件事 ===\n");
  {
    FakeServer::Cfg cfg;
    cfg.mode = FakeServer::kSilent;
    FakeServer srv;
    std::string err;
    if (srv.start(cfg, err)) {
      auto const t0 = std::chrono::steady_clock::now();
      d2o::NtpResult const r = d2o::ntp_query("127.0.0.1", 700);
      auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
      int const served = srv.served();
      srv.stop();
      check(r.state == d2o::NtpState::kTimeout,
            "收下但不回 → timeout", std::string("实际 ") +
                                        d2o::ntp_state_name(r.state) + "：" + r.error);
      check(served >= 1,
            "**同伴**：包真的送到了服务器手上（不是「我们没发出去」）",
            "served = " + human(served));
      check(!r.error.empty(), "timeout 说了人话（error 非空）");
      // 判据宽松：只挡"timeout_ms 被忽略、走了默认的 2000"。下界挡"根本没等
      // 就说超时"。两头都不需要精确——精确的墙钟断言在负载高的机器上会假红。
      check(ms >= 400 && ms < 3000,
            "超时时间听 timeout_ms（700ms），不是默认的 2000ms",
            "实测 " + human(static_cast<int>(ms)) + " ms");
    }
  }
  {
    // 名字必须是**解析不出来**的那一种（`.invalid` 是 RFC 2606 保留的，不会
    // 被任何正经 DNS 受理）。这不是"地址不可达"，是"连地址都拿不到"。
    d2o::NtpResult const r = d2o::ntp_query("no-such-host.invalid", 1500);
    check(r.state == d2o::NtpState::kUnreachable,
          "解析不出来的主机名 → unreachable",
          std::string("实际 ") + d2o::ntp_state_name(r.state) + "：" + r.error);
    check(!r.error.empty(), "unreachable 说了人话（error 非空）");
  }

  ::WSACleanup();
  std::printf("\n通过 %d 项，失败 %d 项。\n", g_passed, g_failed);
  return g_failed ? 1 : 0;
}
