# 往**另一个进程**的控制台发一次 Ctrl+C。
#
# 为什么非要有这么一个helper：`GenerateConsoleCtrlEvent` 只对"调用者当前挂着的那个
# 控制台"发事件，所以必须先 `FreeConsole()` 再从我们的控制台脱开、`AttachConsole(pid)`
# 挂到目标进程的控制台上。而 FreeConsole 会让**调用者自己**失去控制台 —— 放在主测试
# 脚本里做，主脚本后面所有的输出就都没地方去了（而且它的输出正是测试报告本身）。
# 所以这一步必须在一个用完就死的独立进程里做。
#
# 返回码：0 = 事件已投递；1 = AttachConsole 失败；2 = GenerateConsoleCtrlEvent 失败。
# **不用 stdout 传结果**：FreeConsole 之后 stdout 是死的，而结果只有发完之后才知道。
param(
    [Parameter(Mandatory = $true)][int]$TargetPid
)
$ErrorActionPreference = 'Stop'

# 注意 `$Pid` 是 PowerShell 的自动变量（当前进程号）**且变量名不区分大小写**，
# 所以这里的参数只能叫 $TargetPid —— 叫 $Pid 会在绑定时报"只读变量不能覆盖"，
# 而那条消息完全指不到真正的原因。
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class D2OCtrl {
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint pid);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GenerateConsoleCtrlEvent(uint evt, uint group);
}
'@

# 自己先免疫：不然这次 Ctrl+C 会先打死本 helper（它就挂在同一个控制台上）。
# 必须在 GenerateConsoleCtrlEvent **之前**装好，而且要在 AttachConsole 之后装 ——
# 控制处理函数表是按控制台挂的。
[void][D2OCtrl]::FreeConsole()
if (-not [D2OCtrl]::AttachConsole([uint32]$TargetPid)) {
    exit 1
}
[void][D2OCtrl]::SetConsoleCtrlHandler([IntPtr]::Zero, $true)
# 0 = CTRL_C_EVENT，group 0 = 挂在这个控制台上的所有进程。
if (-not [D2OCtrl]::GenerateConsoleCtrlEvent([uint32]0, [uint32]0)) {
    exit 2
}
exit 0
