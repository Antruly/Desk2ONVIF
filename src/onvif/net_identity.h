#pragma once

/**
 * @file src/onvif/net_identity.h
 * @brief 本机网络身份：对外该宣称哪个地址、以及设备 UUID。
 *
 * 为什么不能简单地"取第一个非环回地址"：本机有三个 IPv4
 * （`192.168.2.90`、`172.16.3.75`、`172.23.0.1`），后两个是 WSL/Hyper-V 的
 * 虚拟网卡。挑错了的后果不是报错，而是 **XAddrs 里写了一个 NVR 永远连不上的
 * 地址** —— 而 NVR 界面上的表现只是"添加失败"或一直转圈，没有任何线索指向
 * 这里。所以宣称地址要**按探测包的来源反推**：谁的网段里来了 Probe，
 * 就用同一个网段的本机地址回答。
 *
 * 头文件里刻意不出现 `sockaddr` / winsock：那套头对包含顺序极敏感
 * （`winsock2.h` 必须在 `windows.h` 之前），暴露出去迟早有人踩。
 */

#include <cstddef>
#include <string>
#include <vector>

namespace d2o {

struct LocalAddr {
  std::string ip;
  unsigned prefix_len = 24;   ///< 255.255.255.0 → 24
  std::string adapter;        ///< 适配器描述，日志里用
  std::string mac;            ///< 大写十六进制，无分隔
};

/// 所有已启用、非环回的 IPv4 地址。
std::vector<LocalAddr> local_ipv4_addresses();

/// 与 peer_ip 同网段的本机地址；没有同网段的就退到默认路由地址。
/// 传入的不是合法 IPv4 时也走退路（返回空串只在完全没网时发生）。
std::string pick_local_address_for(std::string const& peer_ip);

/// 默认路由用的那个本机地址（对外单向广播时用，比如 Hello）。
/// 取不到时返回第一个可用地址，再不行返回空串。
std::string default_local_address();

/// 一个**跨重启稳定**的机器标识：主机名 + 注册表里的 MachineGuid。
///
/// 稳定性是硬要求：ONVIF 的设备 UUID 一变，NVR 就会把同一台机器当成新设备
/// 重复添加（列表里出现两台一样的摄像机）。所以这个种子绝不能掺时间戳、
/// 进程号或者内存地址。
///
/// **网卡 MAC 从第五轮起不参与**（在那之前参与）。原因见 `net_identity.cpp`
/// 里 `machine_seed()` 的注释：MAC **集合**会随插拔网卡、扩展坞、禁用虚拟网卡
/// 而变，而"这台机器是谁"不该跟着变。留下的两样都满足"换硬件不变"。
///
/// 判据在 `tests\info_identity.ps1` 的 `[3]` 段（在 PowerShell 里复算一遍
/// host+guid 的等式，与程序固化进 `config.json` 的值逐字比），
/// 并且做过变异检验：把 MAC 加回去，那一条立刻变红
/// （见 `out\ab-mut\EVIDENCE.md`）。
std::string machine_seed();

/// 由种子派生的稳定 UUID，形如 `urn:uuid:xxxxxxxx-xxxx-5xxx-yxxx-xxxxxxxxxxxx`。
std::string make_uuid_urn(std::string const& seed);

/// 由种子派生的稳定序列号，纯大写十六进制。
std::string make_serial(std::string const& seed, std::size_t hex_chars = 16);

}  // namespace d2o
