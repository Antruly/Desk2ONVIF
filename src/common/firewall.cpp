#include "common/firewall.h"

#include "common/log.h"
#include "common/text.h"

#include <windows.h>

#include <cstdio>
#include <vector>

namespace d2o {

namespace {

std::string const kPrefix = "Desk2ONVIF-";

/// UTF-8 → 宽字符。text.h 里只有反方向（DXGI 给的是宽字符，往里收），
/// 这里要往外传命令行，所以就地补一个。
std::wstring to_wide(std::string const& s) {
  if (s.empty()) return std::wstring();
  int const n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

/// netsh 的输出 → UTF-8。
///
/// **必须按 OEM 代码页读，不能按 UTF-8。** 控制台程序（netsh 就是）按
/// `GetOEMCP()` 写它的输出，本机是 936（GBK）。按 UTF-8 解出来的中文是乱码，
/// 而乱码里照样能 grep 到 ASCII 的部分（"Ok." 之类），于是这个错误不会自曝，
/// 只会在某天需要看清那句话的时候坑人一次。
std::string from_oem(std::string const& raw) {
  if (raw.empty()) return std::string();
  UINT const cp = ::GetOEMCP();
  int const n = ::MultiByteToWideChar(cp, 0, raw.c_str(), static_cast<int>(raw.size()),
                                      nullptr, 0);
  if (n <= 0) return raw;
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(cp, 0, raw.c_str(), static_cast<int>(raw.size()), &w[0], n);
  return to_utf8(w.c_str());
}

std::string trim(std::string s) {
  std::size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return std::string();
  std::size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

/// 跑一个程序并把 stdout+stderr 一起抓回来。
///
/// **不过 cmd.exe**：命令行直接交给 CreateProcessW，所以里面没有第二层
/// 解析（引号、`&`、重定向都不会被解释）。调 netsh 这种参数全是自己拼的
/// 场景，这样比 `_popen` 少一整类"引号该转义几层"的问题。
bool run_capture(std::string const& exe, std::string const& args, std::string& out,
                 DWORD& code, std::string& err) {
  out.clear();
  code = 0;

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE rd = nullptr, wr = nullptr;
  if (!::CreatePipe(&rd, &wr, &sa, 0)) {
    err = "CreatePipe 失败";
    return false;
  }
  // 读端自己不能被子进程继承 —— 继承了它就一直不关，我们读不到 EOF。
  ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = nullptr;

  PROCESS_INFORMATION pi{};
  std::wstring const cmd = to_wide(exe + " " + args);
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');

  BOOL const ok = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  // 写端在我们这边也要关掉，否则子进程结束后管道仍不 EOF。
  ::CloseHandle(wr);
  if (!ok) {
    ::CloseHandle(rd);
    err = "无法启动 " + exe + "（错误码 " + std::to_string(::GetLastError()) + "）";
    return false;
  }

  char chunk[4096];
  DWORD got = 0;
  while (::ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
    out.append(chunk, got);
    // 上限只是防止万一有人拿一个永不退出的 netsh 喂进来把内存吃光。
    if (out.size() > (1u << 20)) break;
  }

  ::WaitForSingleObject(pi.hProcess, 30000);
  ::GetExitCodeProcess(pi.hProcess, &code);
  ::CloseHandle(pi.hProcess);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(rd);

  out = from_oem(out);
  return true;
}

std::string quote(std::string const& s) { return "\"" + s + "\""; }

/// netsh 的退出码语义：0 = 成功/找到，1 = 没找到或没权限。
/// 其余的（比如 2）说明命令行本身有问题，要当错误报出来。
bool netsh(std::string const& args, DWORD& code, std::string& out, std::string& err) {
  if (!run_capture("netsh.exe", args, out, code, err)) return false;
  return true;
}

}  // namespace

std::string const& firewall_rule_prefix() { return kPrefix; }

bool is_elevated() {
  HANDLE tok = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  TOKEN_ELEVATION el{};
  DWORD n = 0;
  BOOL const ok = ::GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &n);
  ::CloseHandle(tok);
  return ok && el.TokenIsElevated != 0;
}

std::string elevation_hint() {
  if (is_elevated()) return std::string();
  return "改防火墙需要管理员权限。两种办法：\n"
         "  · 用管理员身份重开一个终端，再跑一遍同样的命令；\n"
         "  · 或者把下面的命令抄进管理员 PowerShell / CMD 里手工执行"
         "（`--firewall print` 会把命令打出来）。";
}

bool firewall_rule_query(std::string const& name, bool& exists, std::string& err) {
  exists = false;
  DWORD code = 0;
  std::string out;
  if (!netsh("advfirewall firewall show rule name=" + quote(name), code, out, err)) {
    return false;
  }
  if (code == 0) {
    exists = true;
    return true;
  }
  if (code == 1) {
    // netsh 用 1 表示"没有匹配的规则"。**不看它那句话**：不同语言的 Windows
    // 文案不一样（"没有与指定标准相匹配的规则。"），按文案判会在英文系统上失效。
    exists = false;
    return true;
  }
  err = "netsh 查询规则失败（退出码 " + std::to_string(code) + "）：" + trim(out);
  return false;
}

std::string firewall_rule_add_args(FirewallRule const& r) {
  // 三处刻意的收窄，都是为了"别把端口对全世界开放"：
  //   profile=private,domain  不给"公用网络"开 —— 在咖啡馆/机场连 Wi-Fi 时，
  //                           这台机器上正跑着一个监听服务，不该被同网段看见。
  //   remoteip=localsubnet    只放行同网段。NVR 在同一局域网就是它。
  //   不加 program=            那样能精确到 exe，但**exe 一旦挪了位置或改了名，
  //                           规则就静默失效**，症状又变成"NVR 连不上"。
  //                           端口是我们自己的，按端口放行的可预测性更高。
  return "advfirewall firewall add rule name=" + quote(r.name) +
         " dir=in action=allow protocol=" + r.protocol +
         " localport=" + std::to_string(r.port) +
         " profile=private,domain remoteip=localsubnet";
}

bool firewall_rule_add(FirewallRule const& r, std::string& err) {
  if (r.port <= 0 || r.port > 65535) {
    err = "端口 " + std::to_string(r.port) + " 不在 1~65535 里，防火墙规则不建了";
    return false;
  }
  // 先删同名再加，而不是"存在就跳过"：改了端口之后，旧规则还放行着旧端口，
  // 新端口反而不通 —— 而规则名一模一样，`--firewall status` 看上去完全正常。
  // 名字里**不带端口**正是为了这个：一个用途一条规则，改端口就是改它。
  bool exists = false;
  if (!firewall_rule_query(r.name, exists, err)) return false;
  if (exists) {
    D2O_INFO("防火墙规则 %s 已存在，按新端口重建（%s %d）", r.name.c_str(),
             r.protocol.c_str(), r.port);
    std::string const del = "advfirewall firewall delete rule name=" + quote(r.name);
    DWORD dcode = 0;
    std::string dout;
    if (!netsh(del, dcode, dout, err)) return false;
    if (dcode != 0 && dcode != 1) {
      err = "删除旧规则失败（退出码 " + std::to_string(dcode) + "）：" + trim(dout);
      return false;
    }
  }

  std::string args = firewall_rule_add_args(r);

  DWORD code = 0;
  std::string out;
  if (!netsh(args, code, out, err)) return false;
  if (code != 0) {
    err = "建立规则 " + r.name + " 失败（退出码 " + std::to_string(code) + "）：" + trim(out);
    if (!is_elevated()) err += "\n" + elevation_hint();
    return false;
  }
  D2O_INFO("防火墙已放行入站 %s %d（同网段，专用/域网络）—— %s", r.protocol.c_str(),
           r.port, r.why.c_str());
  return true;
}

bool firewall_rule_remove(std::string const& name, std::string& err) {
  DWORD code = 0;
  std::string out;
  if (!netsh("advfirewall firewall delete rule name=" + quote(name), code, out, err)) {
    return false;
  }
  // 1 = 本来就没有。删除是幂等的，这不算失败。
  if (code == 1) {
    D2O_INFO("防火墙规则 %s 本来就不存在，无需删除。", name.c_str());
    return true;
  }
  if (code != 0) {
    err = "删除规则 " + name + " 失败（退出码 " + std::to_string(code) + "）：" + trim(out);
    if (!is_elevated()) err += "\n" + elevation_hint();
    return false;
  }
  D2O_INFO("防火墙规则 %s 已删除。", name.c_str());
  return true;
}

}  // namespace d2o
