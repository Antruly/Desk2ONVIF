#include "common/ntp_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

#include "common/log.h"
#include "common/text.h"

namespace d2o {
namespace {

/// 自 Unix 纪元起的秒数，带小数。
///
/// NTP 的四个时间戳都要这个量纲，而 `system_clock` 在 Windows 上的分辨率是
/// 100 ns 级，足够。
double unix_now() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/// 把「自 Unix 纪元起的秒数」写成 NTP 的 64 位定标数（高 32 位秒、低 32 位小数）。
/// 大端。
void put_ntp_timestamp(std::uint8_t* p, double unix_s) {
  double const v = unix_s + static_cast<double>(kNtpUnixEpochDelta);
  auto const sec = static_cast<std::uint32_t>(v);
  // `v - sec` 落在 [0,1)；乘 2^32 取整就是小数部分。
  // 边界上（v 恰好是整数时 sec 可能进位）不影响正确性：差 1 个 LSB。
  auto const frac = static_cast<std::uint32_t>((v - static_cast<double>(sec)) *
                                               4294967296.0);
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<std::uint8_t>(sec >> (24 - 8 * i));
    p[4 + i] = static_cast<std::uint8_t>(frac >> (24 - 8 * i));
  }
}

/// 读回一个 NTP 时间戳，换算成「自 Unix 纪元起的秒数」。
/// `ok` 置 false 表示这个时间戳是**空的**（秒字段为 0）—— 服务器没填它。
double get_ntp_timestamp(std::uint8_t const* p, bool& ok) {
  std::uint32_t sec = 0;
  std::uint32_t frac = 0;
  for (int i = 0; i < 4; ++i) {
    sec = (sec << 8) | p[i];
    frac = (frac << 8) | p[4 + i];
  }
  ok = sec != 0;
  return static_cast<double>(sec) - static_cast<double>(kNtpUnixEpochDelta) +
         static_cast<double>(frac) / 4294967296.0;
}

/// RAII：WSAStartup / WSACleanup 成对。
///
/// 进程里别的地方（libuv、Live555）已经调过 WSAStartup，这里再调一次只是把
/// 引用计数 +1 —— 但**必须配一次 WSACleanup**，否则每对一次时计数就涨一次。
/// `WSAStartup` 是可重入的，不需要去查别人有没有调过。
struct WsaGuard {
  bool ok = false;
  WsaGuard() {
    WSADATA d;
    ok = ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }
  ~WsaGuard() {
    if (ok) ::WSACleanup();
  }
};

std::string sock_error(int code) {
  switch (code) {
    case WSAETIMEDOUT: return "超时";
    case WSAECONNRESET: return "连接被重置（对端回了 ICMP 端口不可达）";
    case WSAENETUNREACH: return "网络不可达";
    case WSAEHOSTUNREACH: return "主机不可达";
    default: return fmt("socket 错误 %d", code);
  }
}

}  // namespace

char const* ntp_state_name(NtpState s) {
  switch (s) {
    case NtpState::kOk: return "ok";
    case NtpState::kTimeout: return "timeout";
    case NtpState::kBadReply: return "bad_reply";
    case NtpState::kUnreachable: return "unreachable";
  }
  return "unreachable";
}

NtpResult ntp_query(std::string const& server, int timeout_ms) {
  NtpResult r;
  if (server.empty()) {
    r.state = NtpState::kUnreachable;
    r.error = "NTP 服务器名是空的。";
    return r;
  }
  if (timeout_ms <= 0) timeout_ms = 2000;

  WsaGuard wsa;
  if (!wsa.ok) {
    r.state = NtpState::kUnreachable;
    r.error = "WSAStartup 失败，这台机器上没法开 UDP 套接字。";
    return r;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  addrinfo* res = nullptr;
  int const gai = ::getaddrinfo(server.c_str(), "123", &hints, &res);
  if (gai != 0 || res == nullptr) {
    r.state = NtpState::kUnreachable;
    r.error = fmt("解析不出「%s」这个地址（%s）。", server.c_str(),
                  gai != 0 ? ::gai_strerrorA(gai) : "没有可用地址");
    if (res) ::freeaddrinfo(res);
    return r;
  }

  // 48 字节请求：首字节 0x1B = LI 0 / VN 3 / Mode 3（client）。
  // 其余全零，只有「发送时间戳」那一格填上我们自己的钟 —— 服务器会把它原样
  // 抄进回包的 Originate 字段，是这条链上唯一的往返一致性凭据。
  std::uint8_t req[48] = {};
  req[0] = 0x1B;
  double const t1 = unix_now();
  put_ntp_timestamp(req + 40, t1);

  std::string last_err = "没有可用的地址。";
  NtpState last_state = NtpState::kUnreachable;
  int stratum = 0;
  int attempts = 0;
  for (addrinfo* ai = res; ai != nullptr && attempts < 3; ai = ai->ai_next) {
    ++attempts;
    SOCKET s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCKET) {
      last_err = fmt("建不出套接字（%s）。", sock_error(::WSAGetLastError()));
      last_state = NtpState::kUnreachable;
      continue;
    }

    DWORD const tv = static_cast<DWORD>(timeout_ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&tv),
                 sizeof(tv));

    // `connect` 一个 UDP 套接字不改任何报文，它只是**装上一个对端过滤器**：
    // 之后只有来自这个地址的包会进到 recv，其它的一律丢掉。这样就不必自己去
    // 校验"这个回包是不是我这次问的那台服务器发的"（一个更早的、迟到的回包
    // 会带一个陈旧的 T2/T3，算出来的偏移看着完全合理）。
    int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
    if (rc != 0) {
      last_err = fmt("连不上 %s（%s）。", server.c_str(),
                     sock_error(::WSAGetLastError()));
      last_state = NtpState::kUnreachable;
      ::closesocket(s);
      continue;
    }

    rc = ::send(s, reinterpret_cast<char const*>(req), sizeof(req), 0);
    if (rc != static_cast<int>(sizeof(req))) {
      last_err = fmt("请求没发出去（%s）。",
                     sock_error(::WSAGetLastError()));
      last_state = NtpState::kUnreachable;
      ::closesocket(s);
      continue;
    }

    std::uint8_t rep[48] = {};
    rc = ::recv(s, reinterpret_cast<char*>(rep), sizeof(rep), 0);
    int const err = ::WSAGetLastError();
    ::closesocket(s);

    if (rc <= 0) {
      last_state = (rc < 0 && err == WSAETIMEDOUT) ? NtpState::kTimeout
                                                   : NtpState::kUnreachable;
      // 超时是最常见的那一种，而且它的成因（UDP 被静默丢弃）最需要说清楚。
      last_err = last_state == NtpState::kTimeout
                     ? fmt("%d 毫秒内没收到 %s 的回包（UDP 123 被防火墙静默丢弃"
                           "时就是这个样子：包发出去了，没有任何回应）。",
                           timeout_ms, server.c_str())
                     : fmt("收包失败：%s。", sock_error(err));
      continue;
    }
    if (rc < 48) {
      last_state = NtpState::kBadReply;
      last_err = fmt("回包只有 %d 字节，SNTP 至少要有 48 字节。", rc);
      continue;
    }

    stratum = rep[1];
    // Stratum 0 是特殊值：它表示这是一条 **KoD（kiss-o'-death）**，服务器在
    // 明确地拒绝服务（"别再来问了"，常见于公共服务器对高频客户端的回应）。
    // 16 是"未同步"。两种都不能当时间用。
    if (stratum == 0 || stratum >= 16) {
      last_state = NtpState::kBadReply;
      last_err = stratum == 0
                     ? fmt("%s 回了一个 Stratum=0 的包（KoD，服务器拒绝服务）。",
                           server.c_str())
                     : fmt("%s 自报 Stratum=%d（未同步），它的时间不能用。",
                           server.c_str(), stratum);
      continue;
    }

    bool t2_ok = false;
    bool t3_ok = false;
    double const t2 = get_ntp_timestamp(rep + 32, t2_ok);  // 服务器收到的时刻
    double const t3 = get_ntp_timestamp(rep + 40, t3_ok);  // 服务器发出的时刻
    double const t4 = unix_now();                          // 我们收到的时刻
    if (!t2_ok || !t3_ok) {
      last_state = NtpState::kBadReply;
      last_err = fmt("%s 的回包里没有时间戳（全是 0）。", server.c_str());
      continue;
    }

    // 标准的一次往返：网络时延在 (T2-T1) 与 (T3-T4) 里符号相反，相加之后
    // 只剩下两端时钟之差的两倍。
    double const offset = ((t2 - t1) + (t3 - t4)) / 2.0;
    // **量化到最近的整秒**（不是截断）。`DeviceState` 就是整秒，而截断会让
    // "服务器快 0.999 秒"落成 0、"服务器慢 0.001 秒"落成 -1：误差恒为负、
    // 而且正好在秒边界上把一次成功的对时报成差一秒（回环测试里最容易撞上）。
    r.state = NtpState::kOk;
    r.offset_s = static_cast<int>(std::llround(offset));
    r.raw_offset_s = offset;
    r.stratum = stratum;
    r.error.clear();
    ::freeaddrinfo(res);
    return r;
  }

  ::freeaddrinfo(res);
  r.state = last_state;
  r.error = last_err;
  return r;
}

}  // namespace d2o
