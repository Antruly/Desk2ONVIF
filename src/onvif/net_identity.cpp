#include "onvif/net_identity.h"

#include "common/log.h"
#include "onvif/digest.h"  // 复用 md5_hex

// 顺序是硬性的：winsock2.h 必须在 windows.h 之前，否则 windows.h 会拉进
// 1.1 版的 winsock.h，然后是一堆重定义错误。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <sstream>

namespace d2o {
namespace {

/// 点分十进制 → 主机序 32 位。失败返回 false。
bool parse_ipv4(std::string const& s, std::uint32_t& out) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = 0;
  if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  out = (a << 24) | (b << 16) | (c << 8) | d;
  return true;
}

std::uint32_t mask_of(unsigned prefix_len) {
  if (prefix_len == 0) return 0;
  if (prefix_len >= 32) return 0xFFFFFFFFu;
  return ~((1u << (32 - prefix_len)) - 1u);
}

std::string mac_to_string(std::uint8_t const* p, unsigned len) {
  static char const kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 2);
  for (unsigned i = 0; i < len; ++i) {
    out.push_back(kHex[p[i] >> 4]);
    out.push_back(kHex[p[i] & 0x0F]);
  }
  return out;
}

std::string read_machine_guid() {
  HKEY key = nullptr;
  if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return {};
  }
  char buf[128] = {};
  DWORD size = sizeof(buf) - 1;
  DWORD type = 0;
  LONG const rc = ::RegQueryValueExA(key, "MachineGuid", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &size);
  ::RegCloseKey(key);
  if (rc != ERROR_SUCCESS || type != REG_SZ) return {};
  return std::string(buf, std::strlen(buf));
}

/// `IP_ADAPTER_ADDRESSES::Description` 是**宽字符**，且与 UNICODE 宏无关
/// （结构体里写死了 PWCHAR）。转 UTF-8 时长度必须自己算：传 -1 给
/// WideCharToMultiByte 会把结尾的 '\0' 也转进去并计入返回值，于是
/// std::string 里多一个内嵌的 '\0' —— 单独打印看不出来，一旦被 += 拼接
/// 就在那里断掉。
std::string narrow(wchar_t const* w) {
  if (w == nullptr || *w == L'\0') return {};
  int const n = static_cast<int>(std::wcslen(w));
  if (n <= 0) return {};
  int const need = ::WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
  if (need <= 0) return {};
  std::string out(static_cast<std::size_t>(need), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, n, out.data(), need, nullptr, nullptr);
  return out;
}

std::string hostname() {
  char buf[256] = {};
  DWORD n = sizeof(buf) - 1;
  if (::GetComputerNameA(buf, &n) == 0) return {};
  return std::string(buf, n);
}

}  // namespace

std::vector<LocalAddr> local_ipv4_addresses() {
  std::vector<LocalAddr> out;

  // 第一次调用只为拿长度：地址数随网卡变化，写死一个缓冲区是常见 bug。
  ULONG size = 0;
  ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                         nullptr, nullptr, &size);
  if (size == 0) return out;

  std::vector<std::uint8_t> buf(size);
  auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  if (::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, addrs, &size) != NO_ERROR) {
    return out;
  }

  for (auto* a = addrs; a != nullptr; a = a->Next) {
    // OperStatus != Up 的（拔了网线的、禁用的）不能宣称 —— 那些地址
    // 存在但不可达，写进 XAddrs 就是给 NVR 一个连不上的地址。
    if (a->OperStatus != IfOperStatusUp) continue;
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

    for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
      if (u->Address.lpSockaddr == nullptr) continue;
      if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
      auto const* sin = reinterpret_cast<sockaddr_in const*>(u->Address.lpSockaddr);

      char ip[INET_ADDRSTRLEN] = {};
      if (::inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) == nullptr) continue;

      LocalAddr la;
      la.ip = ip;
      la.prefix_len = u->OnLinkPrefixLength;
      if (la.prefix_len == 0 || la.prefix_len > 32) la.prefix_len = 24;
      la.adapter = narrow(a->Description);
      if (a->PhysicalAddressLength > 0) {
        la.mac = mac_to_string(a->PhysicalAddress, a->PhysicalAddressLength);
      }
      out.push_back(std::move(la));
    }
  }
  return out;
}

std::string pick_local_address_for(std::string const& peer_ip) {
  std::uint32_t peer = 0;
  if (parse_ipv4(peer_ip, peer)) {
    auto const all = local_ipv4_addresses();
    for (LocalAddr const& la : all) {
      std::uint32_t mine = 0;
      if (!parse_ipv4(la.ip, mine)) continue;
      if ((mine & mask_of(la.prefix_len)) == (peer & mask_of(la.prefix_len))) {
        return la.ip;
      }
    }
    D2O_DEBUG("没有与本机同网段的地址能回答 %s，退到默认路由地址。",
              peer_ip.c_str());
  }
  return default_local_address();
}

std::string default_local_address() {
  // 连一个外部地址让内核挑出口，但**不发任何包** —— UDP connect 只是把
  // 目的地址记在 socket 上，选路是本地行为。比翻路由表简单得多，且结果
  // 就是"这台机器访问外网时用的那个地址"。
  SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s != INVALID_SOCKET) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = ::htons(53);
    ::inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    std::string result;
    if (::connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
      sockaddr_in me{};
      int len = sizeof(me);
      if (::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
        char ip[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &me.sin_addr, ip, sizeof(ip)) != nullptr) result = ip;
      }
    }
    ::closesocket(s);
    if (!result.empty()) return result;
  }

  // 完全没路由（离线机器）时的兜底：第一个可用地址。
  auto const all = local_ipv4_addresses();
  return all.empty() ? std::string() : all.front().ip;
}

std::string machine_seed() {
  std::ostringstream os;
  os << "host=" << hostname();
  os << ";guid=" << read_machine_guid();

  // **MAC 不参与。** 第五轮之前这里是"把所有网卡 MAC 排序后拼进种子"，
  // 理由写的是"GetAdaptersAddresses 的顺序不稳定，排序之后才稳定"。
  // 那句话本身没错，但它**答的不是这里要问的问题**：排序解决的是同一组
  // 网卡的不同枚举顺序，而这里要的是"这台机器的身份"。多插一块 USB 网卡、
  // 插拔一次扩展坞、禁用一张虚拟网卡（Sunlogin/Oray 那种），**集合本身
  // 就变了** —— 排序再稳，种子也会变，于是序列号和 UUID 一起变，NVR 那边
  // 多出一台"新"摄像机。
  //
  // 留在种子里的两样都满足"换硬件不变"：hostname 是用户自己起的名字，
  // MachineGuid 是安装系统时生成、写在注册表里的，换网卡不动它。
  //
  // **诚实边界**：用同一个磁盘镜像克隆出来的两台机器 MachineGuid 相同 ⇒
  // 序列号相同。这是换来"换网卡不变"的代价。真要区分就去手改 config.json
  // 里的 `onvif.serial_number` / `onvif.device_uuid`（那是逃生门，见
  // `apply_config` 里那段只读锁的注释）。
  return os.str();
}

std::string make_uuid_urn(std::string const& seed) {
  std::string const hex = md5_hex("desk2onvif-uuid:" + seed);
  if (hex.size() < 32) return "urn:uuid:00000000-0000-4000-8000-000000000000";

  // 版本位和变体位：写成 UUID v4 的形状（虽然它是确定性的，不是随机的）。
  // 有些解析器会检查这两处，不设的话算"格式非法的 UUID"。
  std::string h = hex.substr(0, 32);
  h[12] = '4';
  {
    int const v = std::stoi(std::string(1, h[16]), nullptr, 16);
    h[16] = "89ab"[v & 3];
  }

  std::string u = "urn:uuid:";
  u += h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" +
       h.substr(16, 4) + "-" + h.substr(20, 12);
  return u;
}

std::string make_serial(std::string const& seed, std::size_t hex_chars) {
  std::string const hex = md5_hex("desk2onvif-serial:" + seed);
  if (hex.size() < hex_chars) return hex;
  std::string out = hex.substr(0, hex_chars);
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

namespace {

/// 读一个 REG_SZ。取不到（键不存在 / 值缺失 / 类型不对）返回空串。
///
/// 和前面 `read_machine_guid()` 是同一个写法，只是那一个写死了键名；这里要
/// 试好几个序列号位置，所以抽成一个按名字取的小函数。
std::string read_reg_sz(char const* subkey, char const* value) {
  HKEY key = nullptr;
  if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS) {
    return {};
  }
  char buf[256] = {};
  DWORD size = sizeof(buf) - 1;
  DWORD type = 0;
  LONG const rc = ::RegQueryValueExA(key, value, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &size);
  ::RegCloseKey(key);
  if (rc != ERROR_SUCCESS || type != REG_SZ) return {};
  // `buf` 是零初始化的，而 REG_SZ 的值本身带结尾的 NUL —— 用 `std::string(buf)`
  // 就够，不必依赖 `strnlen`（它不是标准 C++，MSVC 的 C++ 头里不保证有
  // `std::strnlen`）。
  std::string s(buf);
  // 去掉首尾空白。
  auto const not_space = [](unsigned char c) { return c != ' ' && c != '\t'; };
  while (!s.empty() && !not_space(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
  while (!s.empty() && !not_space(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

/// OEM 占位串。它们**不是**序列号，但比空串更危险：直接显示出来，用户会以为
/// 这台机器的序列号真的叫 "To be filled by O.E.M."。
bool is_oem_placeholder(std::string const& s) {
  static char const* const kBad[] = {
      "to be filled by o.e.m.", "default string", "n/a", "none", "0",
      "system serial number", "chassis serial number", "base board serial number",
  };
  std::string low;
  low.reserve(s.size());
  for (char c : s) {
    low.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  for (char const* b : kBad) {
    if (low == b) return true;
  }
  return false;
}

}  // namespace

std::string machine_hardware_serial() {
  char const* const kKey = "HARDWARE\\DESCRIPTION\\System\\BIOS";
  static char const* const kValues[] = {
      "SystemSerialNumber",
      "BaseBoardSerialNumber",
      "ChassisSerialNumber",
  };
  for (char const* v : kValues) {
    std::string s = read_reg_sz(kKey, v);
    if (s.empty() || is_oem_placeholder(s)) continue;
    return s;
  }
  // 读不到就退回到稳定派生值。**不返回空串** —— 通道 1 的 ID 和序列号都要有
  // 一个确定的值，而空串会让两路本机通道在界面上看起来像同一个"未绑定"。
  return make_serial(machine_seed());
}

}  // namespace d2o
