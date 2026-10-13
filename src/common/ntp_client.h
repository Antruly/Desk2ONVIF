// SNTP 客户端 —— 自写，不引库。
//
// 它只做一件事：问一次时间服务器"现在几点"，算出一个**偏移**。
// 它**绝不改 Windows 系统时钟**：偏移交给 `DeviceState`，设备时间 = 主机时间 +
// 偏移，和 NVR 下发的 `SetSystemDateAndTime` 走的是同一个落点。这样既不需要
// 管理员权限，也不会连累这台机器上别的程序（见 config.h 的 TimeConfig 注释）。
//
// 为什么自己写而不是用现成的：整棵树里 NTP 只用到这一个 UDP 往返（48 字节请求、
// 48 字节回包、一次减法），引一个库换来的是一份要跟着走的依赖和一套我们不会用
// 到的接口。这一段是协议里最稳定的部分（RFC 5905 的报文格式几十年没变过）。
#pragma once

#include <cstdint>
#include <string>

namespace d2o {

/// NTP 纪元（1900-01-01）到 Unix 纪元（1970-01-01）之间的秒数。
///
/// **整整 70 年，其中 17 个闰年** —— 写成 2208988800。这个常数写错的话偏移会
/// 差 2208988800 秒（≈70 年），所以 `tests/ntp_check.cpp` 把它单独量了一次，
/// 而不是只验"偏移是个正数"（那种断言两个方向都放过）。
constexpr std::uint32_t kNtpUnixEpochDelta = 2208988800u;

/// 一次查询的结局。**必须分得清**这四种 —— UDP 到公网 123 在没有网络的环境里
/// 会被防火墙**静默丢弃**（UDP 没有 RST），于是"发出去成功"和"服务器没理我"
/// 长得一模一样。合成一个"失败"的话，`timeout` 这条分支永远没人验过。
enum class NtpState {
  kOk,           ///< 拿到了，`offset_s` 有效
  kTimeout,      ///< 包发出去了，超时内没收到回包
  kBadReply,     ///< 收到了，但太短 / 时间戳是空的 / `Stratum == 0`（KoD，服务器拒绝服务）
  kUnreachable,  ///< 连地址都拿不到（DNS 失败、没有可用地址）
};

/// `ok` / `timeout` / `bad_reply` / `unreachable` —— 直接进 `last_sync_state`。
char const* ntp_state_name(NtpState s);

struct NtpResult {
  NtpState state = NtpState::kUnreachable;
  /// 设备时间应当相对主机时间的偏移（**整秒**）。仅 `state == kOk` 时有效。
  ///
  /// 正数 = 设备时间比主机快。`DeviceState::offset_s_` 就是 int64 秒，所以这里
  /// 也回 int —— 两边的量纲一致，不需要在中途做一次会丢精度的转换。
  int offset_s = 0;
  /// 服务器自报的层数（诊断用：1 = 一级源，16 = 未同步）。
  int stratum = 0;
  /// 服务器时间与主机时间之差的**小数详情**，只用来诊断"到底差多少"。
  /// 不参与 `offset_s` 的取值（那个是量化过的）。
  double raw_offset_s = 0.0;
  /// 失败时的人话（中文）；成功时为空。
  std::string error;
};

/// 向 `server`（域名或 IP 字面量）的 UDP 123 端口发一次 SNTP 查询。
///
/// **阻塞**，最长约 `timeout_ms` ×（地址个数，最多 3）。调用方不要在热路径上
/// 直接调它 —— 产品里它只跑在那个后台对时线程上（`NtpSyncer`）。
///
/// 解析用完整的一次 SNTP 往返（T1 发、T2 服务器收到、T3 服务器发出、T4 收）：
/// `offset = ((T2 - T1) + (T3 - T4)) / 2`，网络时延被这一半抵消掉。只用 T3 也
/// "差不多"，但差的是单程时延那一项，而在回环上量的正是那几微秒 —— 它决定了
/// 量化到整秒时会不会在秒边界上抖成 N-1。
NtpResult ntp_query(std::string const& server, int timeout_ms = 2000);

}  // namespace d2o
