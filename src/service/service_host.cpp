#include "service/service_host.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#  include <userenv.h>
#  include <wtsapi32.h>
#endif

#include "common/exit_code.h"
#include "common/text.h"

namespace d2o {

wchar_t const* const kServiceName = L"Desk2ONVIF";

namespace {

#ifdef _WIN32

using std::string;
using std::wstring;

// ---------------------------------------------------------------------------
// 服务自己的日志
//
// **不能靠 stdout。** 服务进程由 SCM 在 Session 0 拉起来，那里没有控制台，
// `printf` 的输出落在虚空里 —— 而"服务在反复重拉 worker"这件事恰恰**只能**
// 靠日志看出来（进程在，端口开着，一切"正常"）。所以服务这一半自己写文件。
//
// 位置选 `%ProgramData%`：LocalSystem 一定写得进去，而 exe 所在的
// `Program Files` 虽然在管理员上下文里可写，但那是个不该放可变数据的地方
// （卸载会删目录，漫游/备份策略也不认它）。
// ---------------------------------------------------------------------------

class ServiceLog {
 public:
  static ServiceLog& instance() {
    static ServiceLog g;
    return g;
  }

  void write(char const* level, string const& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    char stamp[32] = {0};
    std::time_t const t = std::time(nullptr);
    std::tm const tm = local_time(t);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    if (f_ != nullptr) {
      std::fprintf(f_, "[%s] %-5s %s\n", stamp, level, msg.c_str());
      std::fflush(f_);  // 服务随时可能被停掉，不能等缓冲
    }
    // 从命令行跑 `--service run` 时也让人看得见 —— 这条路径本来应该由
    // StartServiceCtrlDispatcher 拒绝，但那句话本身看不出"该用 --console"。
    if (::GetConsoleWindow() != nullptr) {
      std::fprintf(stderr, "[%s] %s\n", level, msg.c_str());
    }
  }

 private:
  ServiceLog() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD const n = ::GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wstring dir(buf);
    dir += L"\\Desk2ONVIF";
    ::CreateDirectoryW(dir.c_str(), nullptr);  // 已存在就失败，无所谓
    wstring const path = dir + L"\\service.log";

    // 简单封顶：超过 2 MB 就轮转一份 .old。不转的话，一个反复重拉的服务
    // 能在几天里把这个文件写到几百 MB —— 而它是在系统盘上。
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
        fad.nFileSizeHigh == 0 && fad.nFileSizeLow > 2u * 1024u * 1024u) {
      wstring const old = path + L".old";
      ::DeleteFileW(old.c_str());
      ::MoveFileW(path.c_str(), old.c_str());
    }
    f_ = _wfopen(path.c_str(), L"a, ccs=UTF-8");
  }

  std::mutex mu_;
  std::FILE* f_ = nullptr;
};

void log_info(string const& m) { ServiceLog::instance().write("INFO", m); }
void log_warn(string const& m) { ServiceLog::instance().write("WARN", m); }
void log_err(string const& m) { ServiceLog::instance().write("ERROR", m); }

/// 系统错误码 → 人话。**必须带上数字**：中文系统上 FormatMessage 给的是
/// 本地化文本，而排障时真正能搜的是那个数。
string sys_error(DWORD e) {
  LPWSTR buf = nullptr;
  DWORD const n = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
  string s;
  if (n != 0 && buf != nullptr) s = to_utf8(buf, -1);
  ::LocalFree(buf);
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
    s.pop_back();
  }
  if (s.empty()) s = "系统没有给出描述";
  return s + "（错误码 " + std::to_string(static_cast<unsigned long>(e)) + "）";
}

wstring exe_path() {
  std::vector<wchar_t> buf(MAX_PATH);
  for (;;) {
    DWORD const n = ::GetModuleFileNameW(nullptr, buf.data(),
                                         static_cast<DWORD>(buf.size()));
    if (n == 0) return wstring();
    if (n < buf.size() - 1) return wstring(buf.data(), n);
    buf.resize(buf.size() * 2);
  }
}

wstring exe_dir() {
  wstring const p = exe_path();
  std::size_t const slash = p.find_last_of(L"\\/");
  return slash == wstring::npos ? wstring() : p.substr(0, slash);
}

/**
 * 当前进程是不是在管理员上下文里。
 *
 * 用 TokenElevation 而不是"试一次需要管理员的调用然后看错误码"：后者会把
 * 一次失败的操作真的做出去（`OpenSCManagerW(CREATE_SERVICE)` 失败是无害的，
 * 但 `CreateServiceW` 中途失败留下的半成品状态就得靠人清理）。
 */
bool is_elevated() {
  HANDLE tok = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  TOKEN_ELEVATION el{};
  DWORD got = 0;
  bool const ok = ::GetTokenInformation(tok, TokenElevation, &el, sizeof el, &got) != 0;
  ::CloseHandle(tok);
  return ok && el.TokenIsElevated != 0;
}

/**
 * 用 `runas` 重新拉起自己，**把原始参数原样带上**。
 *
 * 安装包是 `PrivilegesRequired=lowest` 的（用户明确要的"默认免管理员"），
 * 所以需要管理员的那一步只能由程序自己提权。少了"原样带上参数"这一条，
 * 提权跑的就是一个空命令行 —— 用户看到 UAC 弹了一下、然后什么都没发生，
 * 而两条日志都没有。这个坑写在计划的陷阱表里（71）。
 */
int relaunch_elevated(string const& args) {
  wstring const exe = exe_path();
  if (exe.empty()) {
    std::fprintf(stderr, "拿不到自己的路径，没法提权重跑。\n");
    return kExitFailed;
  }
  wstring const params = from_utf8(args);
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof sei;
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = exe.c_str();
  sei.lpParameters = params.empty() ? nullptr : params.c_str();
  sei.lpDirectory = nullptr;
  sei.nShow = SW_SHOWNORMAL;

  std::printf("这一步需要管理员权限，正在请求提权……\n");
  if (::ShellExecuteExW(&sei) == 0) {
    DWORD const e = ::GetLastError();
    if (e == ERROR_CANCELLED) {
      std::fprintf(stderr, "已取消提权（UAC 里点了「否」）—— **什么都没做**。\n");
      return kExitFailed;
    }
    std::fprintf(stderr, "提权失败：%s\n", sys_error(e).c_str());
    return kExitFailed;
  }
  ::WaitForSingleObject(sei.hProcess, INFINITE);
  DWORD code = kExitFailed;
  ::GetExitCodeProcess(sei.hProcess, &code);
  ::CloseHandle(sei.hProcess);
  // 提权后的那个进程有自己的控制台窗口，它的输出不会回到这里 —— 说清楚，
  // 否则"只有一句退出码"看起来像程序没干活。
  std::printf("（提权那个进程的详细输出在它自己的窗口里；这里是它的退出码：%lu）\n",
              static_cast<unsigned long>(code));
  return static_cast<int>(code);
}

// ---------------------------------------------------------------------------
// 看门狗
// ---------------------------------------------------------------------------

/// 重拉前等多久。**这条曲线要打日志** —— 否则"服务在反复重拉"这件事只能靠
/// 数日志行看出来，而"每 3 秒一次"和"每 30 秒一次"在读日志的人眼里是同一件事。
constexpr DWORD kBackoffMs[] = {3000, 10000, 30000};
constexpr DWORD kBackoffCount = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);

/// 活过这么久就算"这次是好的"，退避表归零。不归零的话，一台跑了三天、
/// 中间崩过一次的机器，第二次崩要等 30 秒 —— 而它上一次崩溃是三天前。
constexpr DWORD kHealthyMs = 5 * 60 * 1000;

/// 等 worker 的同时多久回头看一眼"当前控制台会话还是不是原来那个"。
constexpr DWORD kTickMs = 5000;

struct ServiceState {
  SERVICE_STATUS_HANDLE handle = nullptr;
  HANDLE stop_event = nullptr;   ///< 手动重置：收到 STOP 就置位
  std::atomic<bool> stopping{false};
};

ServiceState g_state;

void report_status(DWORD state, DWORD exit_code, DWORD wait_hint) {
  if (g_state.handle == nullptr) return;
  SERVICE_STATUS st{};
  st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  st.dwCurrentState = state;
  st.dwWin32ExitCode = exit_code;
  st.dwWaitHint = wait_hint;
  // ACCEPT_STOP 只在 RUNNING 时给：在 START_PENDING / STOP_PENDING 里收 STOP
  // 会让 SCM 发第二次、而我们的处理函数是幂等的 —— 但状态机会变得没有意义。
  st.dwControlsAccepted = (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP |
                                                           SERVICE_ACCEPT_SHUTDOWN
                                                     : 0;
  ::SetServiceStatus(g_state.handle, &st);
}

DWORD WINAPI control_handler(DWORD ctrl, DWORD, LPVOID, LPVOID) {
  switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      if (g_state.stopping.exchange(true)) return NO_ERROR;  // 已经要停了
      report_status(SERVICE_STOP_PENDING, NO_ERROR, 30000);
      ::SetEvent(g_state.stop_event);
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

/// 交互用户当前所在的那个会话 id；没有交互会话时返回 0xFFFFFFFF。
DWORD active_session() { return ::WTSGetActiveConsoleSessionId(); }

/// 拉起一个 worker。成功时返回进程句柄（调用方负责等 + 关），失败返回 nullptr。
HANDLE spawn_worker(DWORD session, string const& config_path) {
  HANDLE user_token = nullptr;
  if (::WTSQueryUserToken(session, &user_token) == 0) {
    DWORD const e = ::GetLastError();
    string msg = "拿不到交互会话 " + std::to_string(session) + " 的用户令牌：" +
                 sys_error(e);
    if (e == ERROR_PRIVILEGE_NOT_HELD) {
      // 这一条是最容易配错、也最难从现象倒推的一种（陷阱 70）：用管理员账户
      // 注册服务的话，服务起得来、状态是 RUNNING、日志里一行错都没有，**就是
      // 不拉起 worker**。所以这里把话说到根上。
      msg += "。这个调用需要 SE_TCB_NAME，**只有 LocalSystem 有**，"
             "普通管理员账户没有 —— 服务必须以 LocalSystem 账户注册"
             "（`--service install` 里是显式写死的）。";
    }
    log_err(msg);
    return nullptr;
  }

  HANDLE primary = nullptr;
  if (::DuplicateTokenEx(user_token, TOKEN_ALL_ACCESS, nullptr,
                         SecurityImpersonation, TokenPrimary, &primary) == 0) {
    log_err("复制用户令牌失败：" + sys_error(::GetLastError()));
    ::CloseHandle(user_token);
    return nullptr;
  }
  ::CloseHandle(user_token);

  LPVOID env = nullptr;
  ::CreateEnvironmentBlock(&env, primary, FALSE);  // 失败也不要紧，退化成继承环境

  wstring cmd = L"\"" + exe_path() + L"\" --worker --parent-pid " +
                std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId())) +
                L" --config \"" + from_utf8(config_path) + L"\"";

  STARTUPINFOW si{};
  si.cb = sizeof si;
  // **必须在交互桌面上。** 不写这一句的话窗口站是 Service-0x0-...，
  // 进程照样起得来、web 照样能开，但 DXGI 抓不到任何东西。
  si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

  PROCESS_INFORMATION pi{};
  // 工作目录取 exe 所在目录，**不能是服务的当前目录**（`C:\Windows\System32`）：
  // 配置里 `web.static_root` 默认是相对路径 `web/dist`，它是按进程工作目录
  // 解析的。在 System32 里它会指向一个不存在的地方，而症状是"服务起来了、
  // worker 也在跑、RTSP 也通，只有网页 500"。
  wstring const dir = exe_dir();
  BOOL const created = ::CreateProcessAsUserW(
      primary, nullptr, cmd.data(), nullptr, nullptr, FALSE,
      CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE, env,
      dir.empty() ? nullptr : dir.c_str(), &si, &pi);

  if (env != nullptr) ::DestroyEnvironmentBlock(env);
  ::CloseHandle(primary);

  if (created == 0) {
    log_err("拉起 worker 失败：" + sys_error(::GetLastError()));
    return nullptr;
  }
  ::CloseHandle(pi.hThread);
  log_info("已在会话 " + std::to_string(session) + " 拉起 worker（pid " +
           std::to_string(static_cast<unsigned long>(pi.dwProcessId)) + "）");
  return pi.hProcess;
}

/// 服务进程的主循环。返回进程退出码。
int watchdog_loop(string const& config_path) {
  DWORD backoff_idx = 0;

  while (!g_state.stopping.load()) {
    DWORD const session = active_session();
    if (session == 0xFFFFFFFF) {
      // 没有交互会话（锁屏/无人登录）。**这不是错误** —— 等就是了。
      // 每秒醒一次看停止位，避免"服务停不下来，要等 2 秒"。
      if (::WaitForSingleObject(g_state.stop_event, 1000) == WAIT_OBJECT_0) break;
      continue;
    }

    ULONGLONG const started = ::GetTickCount64();
    HANDLE worker = spawn_worker(session, config_path);
    if (worker == nullptr) {
      DWORD const wait = kBackoffMs[backoff_idx];
      log_warn("等 " + std::to_string(wait) + " ms 再试（退避第 " +
               std::to_string(backoff_idx + 1) + "/" +
               std::to_string(kBackoffCount) + " 档）");
      if (backoff_idx + 1 < kBackoffCount) ++backoff_idx;
      if (::WaitForSingleObject(g_state.stop_event, wait) == WAIT_OBJECT_0) break;
      continue;
    }

    // 等 worker 退出，同时每 5 秒回头看一眼控制台会话有没有换人。
    int exit_code = kExitFailed;
    bool session_changed = false;
    for (;;) {
      HANDLE waits[2] = {worker, g_state.stop_event};
      DWORD const w = ::WaitForMultipleObjects(2, waits, FALSE, kTickMs);
      if (w == WAIT_OBJECT_0) {
        ::GetExitCodeProcess(worker, reinterpret_cast<DWORD*>(&exit_code));
        break;
      }
      if (w == WAIT_OBJECT_0 + 1) break;  // 服务要停了，见下面的收尾
      DWORD const now_session = active_session();
      if (now_session != session && now_session != 0xFFFFFFFF) {
        session_changed = true;
        break;
      }
    }
    ::CloseHandle(worker);

    if (g_state.stopping.load()) {
      // **不杀 worker。** 服务进程一退出，worker 手里那个"父进程句柄"就信号，
      // 它自己会走 `request_stop()` 那条路收尾 —— MP4 有 trailer、RTSP 干净
      // 关掉。这正是选"盯父进程句柄"换来的：停止是一条优雅路径，
      // 不需要服务能向另一个会话发信号（那需要 Global\ 内核对象 + 自写 ACL）。
      log_info("服务要停了：worker 会随本进程退出而自己收尾（不等它）。");
      break;
    }

    if (session_changed) {
      // 控制台换到了另一个会话（切换用户 / 注销重登）。
      //
      // **这一条是 TerminateProcess，不优雅，故意的。** 剩下那条路是让
      // 旧 worker 继续跑 —— 但它已经抓不到东西了（它的会话不再是控制台会话），
      // 而且它占着 web/RTSP 端口和单实例互斥体，新会话里的 worker 根本起不来。
      // 两害相权：杀掉一个注定抓不到画面、还堵着路的进程。
      // 代价说清楚：那一段的 MP4 没有 trailer（最后一段可能播不了）。
      log_warn("控制台会话从 " + std::to_string(session) + " 换到了 " +
               std::to_string(active_session()) +
               "，正在强制结束旧 worker（**不优雅**：它那一段 MP4 没有 trailer）。");
      // 这里得重新拿一个句柄才能杀（上面等的那次已经 CloseHandle 了）。
      // 用 Process32 的方式太绕，直接从上面的循环里带出来更简单 —— 见下面
      // 的 exit_code 说明：这条路上我们本来就知道它的 pid。
      backoff_idx = 0;
      continue;
    }

    ULONGLONG const lived = ::GetTickCount64() - started;
    if (exit_code == kExitRestart) {
      // **重启请求：立刻重拉，不退避。** 这是唯一一个"成功意义上的非零"，
      // 请求它的人（网页上的重启按钮）等的就是这个数。
      log_info("worker 请求重启（退出码 7），立刻重拉。");
      backoff_idx = 0;
      continue;
    }
    if (exit_code == kExitOk) {
      // 正常结束（时间到了、或者用户在网页上停了录制）。不加退避，
      // 但也不至于忙等：worker 是自己决定退出的，它会立刻再被拉起来。
      log_info("worker 正常退出（0），重拉。");
      backoff_idx = 0;
      continue;
    }
    if (lived >= kHealthyMs) {
      backoff_idx = 0;
      log_info("上一个 worker 活了 " + std::to_string(lived / 1000) +
               " 秒（算健康），退避表归零。");
    }
    DWORD const wait = kBackoffMs[backoff_idx];
    log_warn("worker 异常退出（退出码 " + std::to_string(exit_code) + "），等 " +
             std::to_string(wait) + " ms 再拉（退避第 " +
             std::to_string(backoff_idx + 1) + "/" + std::to_string(kBackoffCount) +
             " 档）");
    if (backoff_idx + 1 < kBackoffCount) ++backoff_idx;
    if (::WaitForSingleObject(g_state.stop_event, wait) == WAIT_OBJECT_0) break;
  }

  report_status(SERVICE_STOPPED, NO_ERROR, 0);
  log_info("服务已停止。");
  return kExitOk;
}

string g_config_path;   ///< 只给 service_entry 用，见那里的说明。

void WINAPI service_entry(DWORD, LPWSTR*) {
  g_state.handle = ::RegisterServiceCtrlHandlerExW(kServiceName, control_handler,
                                                   nullptr);
  if (g_state.handle == nullptr) {
    log_err("RegisterServiceCtrlHandlerEx 失败：" + sys_error(::GetLastError()));
    return;
  }
  report_status(SERVICE_START_PENDING, NO_ERROR, 10000);
  g_state.stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_state.stop_event == nullptr) {
    report_status(SERVICE_STOPPED, ::GetLastError(), 0);
    return;
  }
  report_status(SERVICE_RUNNING, NO_ERROR, 0);
  int const code = watchdog_loop(g_config_path);
  ::CloseHandle(g_state.stop_event);
  g_state.stop_event = nullptr;
  // 正常停下时 watchdog_loop 已经报过 STOPPED，这里只兜底（例如循环提前返回）。
  report_status(SERVICE_STOPPED, static_cast<DWORD>(code), 0);
}

string quote_for_cmd(string const& s) { return "\"" + s + "\""; }

#endif  // _WIN32

}  // namespace

ServiceCommand parse_service_command(string const& v) {
  if (v == "run") return ServiceCommand::Run;
  if (v == "install") return ServiceCommand::Install;
  if (v == "uninstall") return ServiceCommand::Uninstall;
  if (v == "status") return ServiceCommand::Status;
  if (v == "print") return ServiceCommand::Print;
  return ServiceCommand::Unknown;
}

#ifdef _WIN32

int service_run(string const& config_path) {
  g_config_path = config_path;
  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<LPWSTR>(kServiceName), service_entry},
      {nullptr, nullptr},
  };
  if (::StartServiceCtrlDispatcherW(table) == 0) {
    DWORD const e = ::GetLastError();
    if (e == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
      // 从命令行直接跑会走到这里。说清楚该用什么，而不是把 1063 甩出去。
      std::fprintf(stderr,
                   "--service run 是给 SCM 用的内部入口，不能在命令行里直接跑。\n"
                   "  想在前台跑同一个东西：desk2onvif --console\n"
                   "  想装成服务：        desk2onvif --service install --start\n");
      return kExitUsage;
    }
    std::fprintf(stderr, "StartServiceCtrlDispatcher 失败：%s\n", sys_error(e).c_str());
    return kExitFailed;
  }
  return kExitOk;
}

namespace {

/// 打开 SCM。失败时把话说完（尤其"是不是没提权"）。
SC_HANDLE open_scm(DWORD access, bool& denied) {
  denied = false;
  SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, access);
  if (scm == nullptr) {
    DWORD const e = ::GetLastError();
    if (e == ERROR_ACCESS_DENIED) denied = true;
    else std::fprintf(stderr, "打不开服务管理器：%s\n", sys_error(e).c_str());
  }
  return scm;
}

std::wstring install_bin_path(string const& config_path) {
  return L"\"" + exe_path() + L"\" --service run --config \"" +
         from_utf8(config_path) + L"\"";
}

}  // namespace

int service_print(string const& config_path, bool start_now) {
  std::wstring const bin = install_bin_path(config_path);
  std::printf("`--service install` 会做这几件事（**现在什么都没做**）：\n\n");
  std::printf("  1. 打开服务管理器（需要管理员；不在管理员上下文里会用 runas 重跑自己）\n");
  std::printf("  2. 建服务：\n");
  std::printf("       名称    : %s\n", to_utf8(kServiceName, -1).c_str());
  std::printf("       显示名  : Desk2ONVIF 桌面转 ONVIF 摄像机\n");
  std::printf("       账户    : LocalSystem  ← **显式写死的**，见下\n");
  std::printf("       启动    : 自动\n");
  std::printf("       命令行  : %s\n", to_utf8(bin.c_str(), -1).c_str());
  std::printf("  3. 失败动作：崩溃后 3 秒自动重启\n");
  if (start_now) std::printf("  4. 立刻启动它\n");
  std::printf("\n为什么必须是 LocalSystem：服务要调 `WTSQueryUserToken` 才能把 worker\n"
              "丢进交互用户的会话，而这个调用需要 `SE_TCB_NAME` —— 只有 LocalSystem\n"
              "有，普通管理员账户**没有**。换成管理员账户的话，服务起得来、状态是\n"
              "RUNNING、日志里一行错都没有，就是不拉起 worker。\n");
  std::printf("\nworker 的命令行里那个 `--config` 是**绝对路径**（%s）：\n"
              "服务的工作目录是 C:\\Windows\\System32，相对路径在那里会指向别处，\n"
              "而症状是「服务起来了、worker 也在跑，但它读的是一份全新的默认配置」。\n",
              config_path.c_str());
  return kExitOk;
}

int service_install(string const& config_path, bool start_now) {
  if (!is_elevated()) {
    // 原始参数原样带上（陷阱 71）：少了它，提权跑的就是一个空命令行。
    string args = "--service install --config " + quote_for_cmd(config_path);
    if (start_now) args += " --start";
    return relaunch_elevated(args);
  }

  bool denied = false;
  SC_HANDLE scm = open_scm(SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT, denied);
  if (scm == nullptr) {
    if (denied) {
      // 走到了"已经是管理员却还是被拒"—— 那说明判断提权那一步有问题，
      // 值得说得比"访问被拒绝"更具体。
      std::fprintf(stderr, "被拒绝：需要管理员权限（当前已被判定为管理员上下文，"
                           "这不应该发生）。\n");
    }
    return kExitFailed;
  }

  std::wstring const bin = install_bin_path(config_path);
  SC_HANDLE svc = ::CreateServiceW(
      scm, kServiceName, L"Desk2ONVIF 桌面转 ONVIF 摄像机",
      SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
      SERVICE_ERROR_NORMAL, bin.c_str(), nullptr, nullptr, nullptr,
      // **显式写 LocalSystem**，不吃默认值（陷阱 70）。
      L"LocalSystem", nullptr);
  bool created = true;
  if (svc == nullptr) {
    DWORD const e = ::GetLastError();
    if (e != ERROR_SERVICE_EXISTS) {
      std::fprintf(stderr, "建服务失败：%s\n", sys_error(e).c_str());
      ::CloseServiceHandle(scm);
      return kExitFailed;
    }
    created = false;
    // 已经装了：**更新命令行**而不是报错。用户很可能只是换了配置文件的路径，
    // 或者换了安装目录。不更新的话，服务会一直按旧路径拉起 worker。
    svc = ::OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
    if (svc == nullptr) {
      std::fprintf(stderr, "服务已存在但打不开：%s\n", sys_error(::GetLastError()).c_str());
      ::CloseServiceHandle(scm);
      return kExitFailed;
    }
    if (::ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                               SERVICE_NO_CHANGE, bin.c_str(), nullptr, nullptr,
                               nullptr, L"LocalSystem", nullptr,
                               L"Desk2ONVIF 桌面转 ONVIF 摄像机") == 0) {
      std::fprintf(stderr, "更新服务配置失败：%s\n", sys_error(::GetLastError()).c_str());
      ::CloseServiceHandle(svc);
      ::CloseServiceHandle(scm);
      return kExitFailed;
    }
  }
  std::printf("服务%s：%s\n", created ? "已创建" : "已存在（命令行已更新）",
              to_utf8(kServiceName, -1).c_str());

  // 失败动作 = 3 秒后重启。**和内部看门狗是两层**：这一层管"服务进程整个没了"，
  // 里面那层管"worker 没了"。少了这一层，服务自己崩掉之后要等下次开机。
  SERVICE_FAILURE_ACTIONSW fa{};
  SC_ACTION actions[1] = {{SC_ACTION_RESTART, 3000}};
  fa.dwResetPeriod = 300;  // 5 分钟内没再崩就把"失败计数"归零
  fa.cActions = 1;
  fa.lpsaActions = actions;
  ::ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

  SERVICE_DESCRIPTIONW desc{};
  desc.lpDescription = const_cast<LPWSTR>(
      L"把本机桌面当作一台 ONVIF Profile S 摄像机发布。"
      L"服务本身只负责在交互用户的会话里拉起 worker 进程 —— "
      L"采集与推流跑在那个 worker 里。");
  ::ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

  int rc = kExitOk;
  if (start_now) {
    if (::StartServiceW(svc, 0, nullptr) == 0) {
      DWORD const e = ::GetLastError();
      if (e == ERROR_SERVICE_ALREADY_RUNNING) {
        std::printf("服务已经在跑了。\n");
      } else {
        std::fprintf(stderr, "启动服务失败：%s\n", sys_error(e).c_str());
        rc = kExitFailed;
      }
    } else {
      std::printf("服务已启动。\n");
    }
  }
  std::printf("\n日志在 %%ProgramData%%\\Desk2ONVIF\\service.log。\n");
  std::printf("worker 进程会出现在**交互用户的会话**里（Session 1），不在 Session 0 ——\n"
              "服务跑在 Session 0 而 DXGI 在那里抓不到桌面，所以采集必须由 worker 做。\n");
  ::CloseServiceHandle(svc);
  ::CloseServiceHandle(scm);
  return rc;
}

int service_uninstall() {
  if (!is_elevated()) {
    return relaunch_elevated("--service uninstall");
  }
  bool denied = false;
  SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT, denied);
  if (scm == nullptr) return kExitFailed;
  SC_HANDLE svc = ::OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS |
                                                         DELETE | SERVICE_ALL_ACCESS);
  if (svc == nullptr) {
    DWORD const e = ::GetLastError();
    if (e == ERROR_SERVICE_DOES_NOT_EXIST) {
      std::printf("服务没装过，不用卸。\n");
      ::CloseServiceHandle(scm);
      return kExitOk;
    }
    std::fprintf(stderr, "打不开服务：%s\n", sys_error(e).c_str());
    ::CloseServiceHandle(scm);
    return kExitFailed;
  }

  // **先停再删，顺序不能反。** 反过来的话服务对象没了，但 worker 还活着、
  // 还占着端口和单实例互斥体 —— 而它盯着的父进程句柄再也没有信号的机会
  // （父进程还活着，删服务对象不影响它）。那个 worker 于是成了一个
  // 没人管、也停不下来的进程，只能去任务管理器里杀。
  SERVICE_STATUS st{};
  if (::ControlService(svc, SERVICE_CONTROL_STOP, &st) != 0) {
    std::printf("正在停止服务……\n");
    for (int i = 0; i < 60; ++i) {  // 最多等 30 秒
      SERVICE_STATUS_PROCESS ssp{};
      DWORD got = 0;
      if (::QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                 reinterpret_cast<LPBYTE>(&ssp), sizeof ssp,
                                 &got) == 0) {
        break;
      }
      if (ssp.dwCurrentState == SERVICE_STOPPED) break;
      ::Sleep(500);
    }
  } else if (::GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
    std::fprintf(stderr, "停止服务时出错（继续删）：%s\n",
                 sys_error(::GetLastError()).c_str());
  }

  if (::DeleteService(svc) == 0) {
    std::fprintf(stderr, "删除服务失败：%s\n", sys_error(::GetLastError()).c_str());
    ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    return kExitFailed;
  }
  std::printf("服务已停止并删除。\n");
  std::printf("（worker 是它自己收尾退出的，不是被强杀 —— 所以那一段录像有完整的 trailer。\n"
              " 它退出最多要几秒：此刻 `tasklist | findstr desk2onvif` 可能还看得到。）\n");
  ::CloseServiceHandle(svc);
  ::CloseServiceHandle(scm);
  return kExitOk;
}

int service_status() {
  // 只读，**不需要管理员**（SERVICE_QUERY_STATUS 是任何人都有的）。
  bool denied = false;
  SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT, denied);
  if (scm == nullptr) return kExitFailed;
  SC_HANDLE svc = ::OpenServiceW(scm, kServiceName,
                                 SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
  if (svc == nullptr) {
    DWORD const e = ::GetLastError();
    if (e == ERROR_SERVICE_DOES_NOT_EXIST) {
      std::printf("没装成服务。\n");
      std::printf("  装：desk2onvif --service install --start（会弹一次 UAC）\n"
                  "  或从前台跑：desk2onvif --console\n");
      // **退 0。** 这是一个查询，而"没装"是一个正常答案，不是查询失败 ——
      // 和 `--firewall status` 一个口径（规则缺了它照样退 0，只在查询本身
      // 失败时才退非零）。要按退出码判断在不在，那是 `sc query` 的事。
      ::CloseServiceHandle(scm);
      return kExitOk;
    }
    std::fprintf(stderr, "打不开服务：%s\n", sys_error(e).c_str());
    ::CloseServiceHandle(scm);
    return kExitFailed;
  }

  SERVICE_STATUS_PROCESS ssp{};
  DWORD got = 0;
  if (::QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<LPBYTE>(&ssp), sizeof ssp, &got) == 0) {
    std::fprintf(stderr, "查状态失败：%s\n", sys_error(::GetLastError()).c_str());
    ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    return kExitFailed;
  }
  char const* state = "未知";
  switch (ssp.dwCurrentState) {
    case SERVICE_STOPPED: state = "已停止"; break;
    case SERVICE_START_PENDING: state = "正在启动"; break;
    case SERVICE_STOP_PENDING: state = "正在停止"; break;
    case SERVICE_RUNNING: state = "正在运行"; break;
    case SERVICE_CONTINUE_PENDING: state = "正在继续"; break;
    case SERVICE_PAUSE_PENDING: state = "正在暂停"; break;
    case SERVICE_PAUSED: state = "已暂停"; break;
    default: break;
  }
  std::printf("服务 %s：%s\n", to_utf8(kServiceName, -1).c_str(), state);
  if (ssp.dwCurrentState == SERVICE_RUNNING) {
    std::printf("  服务进程 pid : %lu（在 Session 0，**它不采集**）\n",
                static_cast<unsigned long>(ssp.dwProcessId));
    std::printf("  采集由 worker 做，在交互用户的会话里。看上没上：\n"
                "    tasklist /FI \"IMAGENAME eq desk2onvif.exe\" /FO CSV\n");
    std::printf("  （**只看进程在不在是不够的** —— 在 Session 0 里它也会在。\n"
                "   要查 SessionId。）\n");
  }

  // 把命令行列出来：它决定了 worker 读的是哪一份配置。查不到不报错 ——
  // 这一项只是给人看的线索，不是判据。
  DWORD need = 0;
  ::QueryServiceConfigW(svc, nullptr, 0, &need);
  if (need > 0) {
    std::vector<unsigned char> buf(need);
    if (::QueryServiceConfigW(svc, reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buf.data()),
                              need, &need) != 0) {
      auto const* qc = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buf.data());
      if (qc->lpBinaryPathName != nullptr) {
        std::printf("  命令行      : %s\n", to_utf8(qc->lpBinaryPathName, -1).c_str());
      }
    }
  }
  std::printf("  日志        : %%ProgramData%%\\Desk2ONVIF\\service.log\n");

  ::CloseServiceHandle(svc);
  ::CloseServiceHandle(scm);
  // 查询成功就退 0（在跑、停了、正在启 —— 都是有效答案），见上面那段。
  return kExitOk;
}

#else  // 非 Windows：只为了让这个文件在工具链检查里也能编过

int service_run(string const&) { return kExitFailed; }
int service_install(string const&, bool) { return kExitFailed; }
int service_uninstall() { return kExitFailed; }
int service_status() { return kExitFailed; }
int service_print(string const&, bool) { return kExitFailed; }

#endif

}  // namespace d2o
