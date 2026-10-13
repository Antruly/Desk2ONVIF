#pragma once

/**
 * @file src/service/service_host.h
 * @brief Windows 服务形态：Session 0 那一半。
 *
 * 这个文件只干**两件事**，因为服务形态能不能成立完全取决于认清一件事：
 * **服务跑在 Session 0，而 DXGI 桌面复制在 Session 0 里抓不到交互用户的桌面。**
 * 所以服务不采集、不解码、不开端口。它的全部职责是：
 *
 *   1. 等交互会话出现，拿那个会话的用户令牌，把**同一个 exe** 以 `--worker`
 *      丢进 `winsta0\default`；
 *   2. worker 退出就按退避重新拉起来（退出码 `kExitRestart` 表示"我要重启"，
 *      那就是立刻拉，不退避）。
 *
 * HTTP / ONVIF / RTSP / 采集 / 编码**全部在 worker 里** —— 也就是说装成服务
 * 之后**仍然只有一个 HTTP 服务**，配置的权威副本仍然只有一份。这是选这个架构
 * 换来的最大好处，不是顺带的结果。
 *
 * 为什么 worker 盯**父进程句柄**而不是命名事件：`Local\` 命名空间是**按会话
 * 隔离**的，服务在 Session 0、worker 在 Session 1，`Local\Desk2ONVIF.stop`
 * 在两边根本不是一个对象（服务一置信号，worker 那边永远等不到）；而 `Global\`
 * 要自己写安全描述符才让非管理员用户打得开。`OpenProcess(SYNCHRONIZE, ppid)`
 * 加 `WaitForSingleObject` 一行都不用碰 ACL，而且顺带覆盖了"服务自己崩了"
 * 这条路 —— 服务一死句柄就信号，worker 自己收尾走人，不会留下一个没人管的
 * 孤儿进程占着端口。
 */

#include <string>

namespace d2o {

/// `--service` 后面能跟的子命令。
enum class ServiceCommand {
  Run,        ///< SCM 拉起服务进程时用的（内部用，安装时装进 binPath）
  Install,
  Uninstall,
  Status,     ///< 只读，非管理员可跑
  Print,      ///< 只把"要做什么"打出来，什么都不改（照 `--firewall print` 的样子）
  Unknown,
};

ServiceCommand parse_service_command(std::string const& v);

/// 服务名。装/卸/查三处必须一致，所以只留一个定义。
extern wchar_t const* const kServiceName;

/// 服务进程入口：注册控制处理函数，然后跑看门狗循环。**阻塞**到服务被停止。
///
/// `config_path` 会原样传给 worker 的 `--config`。**必须是绝对路径** ——
/// 服务的工作目录是 `C:\Windows\System32`，相对路径在那里会指向别处，
/// 而症状是"服务起来了、worker 也起来了，但它读的是一份全新的默认配置"。
int service_run(std::string const& config_path);

/// 安装并（可选）立刻启动。**需要管理员**；不在管理员上下文里时会用
/// `ShellExecuteEx(runas)` 重新拉起自己，并把原始参数原样带上。
int service_install(std::string const& config_path, bool start_now);

/// 先 stop 再 delete。**顺序反了会留下一个停不掉的进程**（服务对象没了，
/// 但 worker 还在跑，且它盯着的父进程句柄一直没有信号）。
int service_uninstall();

/// 查询状态。非管理员可跑。
///
/// **退出码口径和 `--firewall status` 一致**：这是一个查询，"没装"和"没在跑"
/// 都是正常答案，查询本身成功就退 0；只有查询失败（打不开 SCM、查不了配置）
/// 才退非零。要按退出码判断在不在跑，那是 `sc query` 的事。
int service_status();

/// 把将要执行的每一步打出来，不做任何修改。和 `--firewall print` 同一个用途：
/// 让人**在不提权、不改变系统状态**的前提下看清楚这条命令到底会干什么。
int service_print(std::string const& config_path, bool start_now);

}  // namespace d2o
