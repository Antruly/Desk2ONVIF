<#
  验收脚本共用的那一套东西：断言、外部命令、起服务、PE 资源探针。

  **这个文件必须用 dot-source 引入（`. .\_verify_common.ps1`），不能当脚本跑。**
  它的函数靠两个约定工作：
    · 计数器 $VerifyCounters 是 dot-source 那一刻落在**调用者作用域**里的一个
      哈希表。函数改的是它的**属性**而不是变量本身，所以不需要 $script: 那套
      作用域修饰符 —— 而 $script: 在"被 dot-source 进来的文件里定义的函数"中
      指向谁，得绕一圈才想得明白，不值得依赖。
    · 命令名以 _verify_ 开头，于是 packaging\*.ps1 那种通配拷贝不会把它带进
      发布包（package.ps1 只拷 packaging\ 下的东西，这里只是多一层保险）。
#>

$VerifyCounters = @{ Pass = 0; Fail = 0 }

# --- 被重定向时，自己的输出一律写成 UTF-8 -------------------------------------
#
# `Invoke-Capture` 用 `StandardOutputEncoding = UTF8` 去读子进程，而
# `Invoke-Capture` 创建的子进程带 `CreateNoWindow`（= CREATE_NO_WINDOW），
# **没有控制台可以继承** —— 于是 `powershell.exe` 的 `[Console]::OutputEncoding`
# 退回 ANSI 代码页（这里 936），两边对不上，中文整片变成 U+FFFD。
#
# 症状有辨识度：脚本自己在控制台上跑得好好的，一旦被 `Invoke-Capture` 收走，
# 落进日志的断言文字就只剩英文标点在，**证据等于没有**。第一次跑 Phase AB 的
# 变异检验就是这么翻的车 —— 两份日志各 900 多个替换字符。
#
# **只在重定向时改**：挂在真人控制台上时不动它，否则在一个 GBK 控制台里
# 中文会变成乱码 —— 那是把一个坑换成另一个。
#
# 放共用层而不是各脚本自己写：这是"脚本的输出被别的脚本收走"这件事的性质，
# 与具体哪一份验收无关。`-verify_common.ps1` 给 python 探针加 `-X utf8`
# 处理的是同一个问题的另一半。
if ([Console]::IsOutputRedirected) {
    try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch { }
}

function Check {
    param([bool]$ok, [string]$what, [string]$detail = '')
    if ($ok) {
        $VerifyCounters.Pass++
        Write-Host ("  [OK]   " + $what) -ForegroundColor Green
    } else {
        $VerifyCounters.Fail++
        Write-Host ("  [FAIL] " + $what) -ForegroundColor Red
        if ($detail) { Write-Host ("         " + $detail) -ForegroundColor DarkGray }
    }
    # 这里**不能**返回值：没人接的返回值会被 PowerShell 打到输出里，
    # 每检查一条就多一行 True/False，把报告淹掉。
}

function Section([string]$t) { Write-Host ''; Write-Host $t -ForegroundColor Cyan }

function Show {
    param([string]$text, [int]$last = 12)
    if (-not $text) { return }
    $text -split "`r?`n" | Where-Object { $_ -ne '' } |
        Select-Object -Last $last | ForEach-Object { Write-Host ("  | " + $_) -ForegroundColor DarkGray }
}

# 探针失败时，把它**所有**标了失败的行打出来，并把完整输出转储到文件。
#
# 为什么不是"尾巴多打几行"：`web_contract.py` 有一百多条断言，红的那条可能在
# 第 30 行，而尾巴（`Show $wc.Stdout 6`）只带得回最后 6 行 —— 于是报告上只剩
# 一句"退出码 = 1"，**没有一条说要紧的话**。2026-10-08 就是这样：安装包验收报
# `通过 42 项，失败 2 项`，而失败的是哪一条断言、为什么，全被那个过滤器吃掉了，
# 只能把整轮重跑一遍来找。这正是陷阱 46 的形状 —— 一个把答案滤掉的过滤器，
# 看上去像"信息本来就这么少"。
#
# 转储而不是只打屏：断言失败信息里常常带着一长串上下文（比如 pending 的字段
# 列表），打屏会被折行糊掉；文件里那份是逐字原样的，可以直接对着看。
function Show-Failures {
    param([string]$text, [string]$dumpTo = '')
    if (-not $text) { return }
    if ($dumpTo) {
        try {
            $dir = Split-Path -Parent $dumpTo
            if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
            [IO.File]::WriteAllText($dumpTo, $text, (New-Object Text.UTF8Encoding $false))
        } catch { }
    }
    $lines = $text -split "`r?`n" | Where-Object { $_ -ne '' }
    $bad = @($lines | Where-Object { $_ -match '\[FAIL\]' })
    if ($bad.Count -gt 0) {
        foreach ($l in $bad) { Write-Host ("  ! " + $l.Trim()) -ForegroundColor Red }
    } else {
        # 一条 [FAIL] 都没有却退了非零 —— 大概率是探针自己中途抛异常了，
        # 那时真正的原因在最后几行，而不在任何一条断言上。
        Write-Host "  ! 输出里没有 [FAIL] 行（探针可能自己抛异常了），最后 15 行：" -ForegroundColor Red
        $lines | Select-Object -Last 15 | ForEach-Object { Write-Host ("  ! " + $_) -ForegroundColor Red }
    }
    if ($dumpTo) { Write-Host ("  ! 探针的完整输出已存到 " + $dumpTo) -ForegroundColor DarkGray }
}

# 把一个参数按 **msvcrt / CommandLineToArgvW 那套规则**拼进命令行。
#
# 这里原来写的是"含空白才加引号，加了引号再把引号换成 \"". 它有一个洞，是
# 第五轮 Phase AA 收尾时才踩到的：**参数里带一个不含空白的双引号时，它根本
# 不加引号**，于是 `A"b\c` 原样进了命令行 —— 而 argv 那一层的规则是"引号
# 开/关切换"，最终收到的是 `Ab\c`（引号被吃掉，没有任何东西报错）。
# 安装包验收里那个口令正好是 `A"b\c`（它存在的理由就是引号和反斜杠），
# 一旦把它当参数传给 python 探针，就会静默变成另一个口令，报出来的是
# "登录失败"，一个字的线索都不指向这里。这正是陷阱 91 那一族的形状：
# **参数被改坏之后的样子，和我根本没传对一个样。**
#
# 规则：
#   · 不含空白也不含引号、且非空 → 原样（绝大多数参数走这一支）
#   · 否则整段加引号；引号写成 \"，而**紧挨着引号的那串反斜杠**要加倍
#     （结尾那串反斜杠同理 —— 否则会把收尾的那个引号转义掉）
function Quote-Arg([string]$Arg) {
    if ($null -eq $Arg) { $Arg = '' }
    if ($Arg -ne '' -and $Arg -notmatch '[\s"]') { return $Arg }
    $sb = New-Object System.Text.StringBuilder
    $null = $sb.Append('"')
    $slashes = 0
    foreach ($ch in $Arg.ToCharArray()) {
        if ($ch -eq [char]0x5C) { $slashes++; continue }
        if ($ch -eq '"') {
            $null = $sb.Append('\' * ($slashes * 2 + 1))
            $null = $sb.Append('"')
            $slashes = 0
            continue
        }
        if ($slashes -gt 0) { $null = $sb.Append('\' * $slashes); $slashes = 0 }
        $null = $sb.Append($ch)
    }
    if ($slashes -gt 0) { $null = $sb.Append('\' * ($slashes * 2)) }
    $null = $sb.Append('"')
    return $sb.ToString()
}

# 跑一个外部命令，带硬超时，拿退出码和输出。
#
# 这两件事都别交给 Start-Process：
#   1. 它的 ExitCode 在 -NoNewWindow 或 -RedirectStandardOutput 下**恒为空**
#      （实测四种组合：朴素调用 7、-WindowStyle Hidden 7、-NoNewWindow 空、
#      -RedirectStandardOutput 空）。而"退出码为空"在断言里看着像失败，
#      实际是工具没拿到值 —— 假失败比不测更坏，它会教人忽略真失败。
#   2. 它的 -RedirectStandardOutput 解码子进程输出用的是控制台代码页：
#      UTF-8 的程序输出被按 936 解成乱码再写进文件，而文件本身"看着完全
#      正常"。要走 ProcessStartInfo 显式设 StandardOutputEncoding。
# 所以这里直接用 System.Diagnostics.Process。顺带好处是超时能真的把整棵
# 进程树收掉（陷阱 44：ffprobe 拉一个没人应答的 RTSP 地址会永远等下去）。
function Invoke-Capture {
    param([string]$File, [string[]]$Sw, [int]$TimeoutMs, [string]$WorkDir)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName  = $File
    $psi.Arguments = (($Sw | ForEach-Object { Quote-Arg $_ }) -join ' ')
    $psi.WorkingDirectory = $WorkDir
    $psi.UseShellExecute  = $false          # 设编码前必须为 false
    $psi.CreateNoWindow   = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $utf8 = New-Object System.Text.UTF8Encoding $false
    $psi.StandardOutputEncoding = $utf8
    $psi.StandardErrorEncoding  = $utf8

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    $null = $p.Start()
    # 先异步把两个流读空再等：管道缓冲区只有 4 KB，子进程写满了就阻塞在
    # write 上，我们阻塞在 wait 上 —— 谁也不动，表现和"命令挂了"一样。
    $so = $p.StandardOutput.ReadToEndAsync()
    $se = $p.StandardError.ReadToEndAsync()

    $done = $p.WaitForExit($TimeoutMs)
    if (-not $done) {
        # 连子进程一起收。Inno 的 SetupLdr 会把真正的安装体解到 %TEMP% 去跑，
        # 只杀父进程的话，挂住的那个还在。
        & taskkill.exe /F /T /PID $p.Id 2>&1 | Out-Null
        $null = $p.WaitForExit(5000)
    }
    return [pscustomobject]@{
        ExitCode = $(if ($done) { $p.ExitCode } else { $null })
        TimedOut = (-not $done)
        Stdout   = $(if ($so.IsCompleted) { $so.Result } else { '' })
        Stderr   = $(if ($se.IsCompleted) { $se.Result } else { '' })
    }
}

# 起一个**长时间运行**的服务：让 cmd 自己把输出重定向进文件，
# PowerShell 完全不碰它的管道。
#
# 为什么不用 Invoke-Capture：那条路要用 ReadToEnd 才能拿到输出，而服务正
# 跑着的时候 ReadToEnd 会一直阻塞；不读又会让 4 KB 的管道写满，服务阻塞在
# 日志上 —— 表现出来是"后端起不来"，而真正的原因是没人收它的日志。
# 交给 cmd 写文件，两边都不占。
#
# 命令写进一个临时 .cmd 再执行，是为了绕开 cmd.exe 那套引号规则：
# `/c "a.exe" x > "o" 2> "e"` 里有 4 个引号，会落进"去掉首尾引号"的老行为，
# 把结尾那个引号吃掉。写成 `/c "文件.cmd"` 只有两个引号、中间是空白、
# 且确实是个文件，三条都满足 → 引号原样保留，行为是确定的。
function Start-Detached {
    param([string]$Exe, [string]$AppArgs, [string]$WorkDir, [string]$OutFile, [string]$ErrFile)

    $cmdFile = Join-Path $env:TEMP ('d2o-run-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.cmd')
    $body = '@echo off' + "`r`n" + '"' + $Exe + '" ' + $AppArgs +
            ' > "' + $OutFile + '" 2> "' + $ErrFile + '"' + "`r`n"
    # .bat/.cmd 一律纯 ASCII，且不带 BOM（陷阱 45 的另一半）。
    [IO.File]::WriteAllText($cmdFile, $body, (New-Object System.Text.ASCIIEncoding))

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'cmd.exe'
    $psi.Arguments = '/c "' + $cmdFile + '"'
    $psi.WorkingDirectory = $WorkDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    return [pscustomobject]@{ Process = $p; CmdFile = $cmdFile }
}

function Test-PortBusy([int]$Port) {
    try {
        $c = New-Object System.Net.Sockets.TcpClient
        $c.Connect('127.0.0.1', $Port); $c.Close(); return $true
    } catch { return $false }
}

function Wait-PortFree {
    param([int[]]$Ports, [int]$Tries = 20)
    foreach ($i in 1..$Tries) {
        Start-Sleep -Milliseconds 500
        $busy = $false
        foreach ($pt in $Ports) { if (Test-PortBusy $pt) { $busy = $true } }
        if (-not $busy) { return }
    }
}

function Stop-Desk2Onvif {
    Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue | ForEach-Object {
        try { $_.Kill(); $null = $_.WaitForExit(15000) } catch {}
    }
}

# 一棵目录树的逐项快照（相对路径 + 大小 + mtime 的 tick）。用来判"有没有往
# 那里写东西"。
#
# 为什么不直接断言"这个目录不存在"：那种判据在**已经用过安装版**的机器上恒真
# —— 目录本来就在，里面写没写新东西它都"存在"，于是这一条永远通过，等于没验。
# 而那恰恰是最需要它生效的机器。快照比法对两种初始状态都成立：目录原先没有，
# 跑完还得没有；原先有，跑完得**一个字节都没变**。
#
# 两份脚本要用（绿色版那个判"没往 %APPDATA% 写"，安装凭据那个判"start-local.bat
# 没有回落到 %APPDATA% 那份配置"），所以放在共用层 —— 两份各写一遍的话，
# 迟早在"比不比 mtime""算不算目录"上不一致，而那种不一致没人会发现。
function Get-TreeSnapshot([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '<不存在>' }
    $items = @(Get-ChildItem -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue |
               Sort-Object FullName | ForEach-Object {
                   $rel = $_.FullName.Substring($Path.Length + 1)
                   if ($_.PSIsContainer) { "D $rel" }
                   else { "F $rel $($_.Length) $($_.LastWriteTimeUtc.Ticks)" }
               })
    return ($items -join "`n")
}

function Wait-Backend {
    param([int]$Port, [int]$Tries = 60)
    foreach ($i in 1..$Tries) {
        Start-Sleep -Milliseconds 500
        try {
            # **问 /api/session，不是 /api/status。** Web 控制台上了登录之后，
            # 不带 cookie 的 /api/status 恒为 401，而 Invoke-WebRequest 把非 2xx
            # 当异常抛 —— 于是这里会安静地重试满 30 秒再报"后端起不来"，而日志
            # 里"HTTP 服务已启动"早就打过了。这个函数问的是"端口能应答了吗"，
            # /api/session 是那条**匿名可达**的同义问法。
            $r = Invoke-WebRequest "http://127.0.0.1:$Port/api/session" -TimeoutSec 3 -UseBasicParsing
            if ($r.StatusCode -eq 200) { return $true }
        } catch {}
    }
    return $false
}

# 图标在 exe 里到底有没有 —— 这个**只能**问 PE 资源表，没有别的办法。
# 看文件大小、看 VersionInfo、`ExtractAssociatedIcon` 都不行：最后那个在
# 资源缺失时会**回落到 Windows 的通用图标**并正常返回一个 Icon 对象，
# 于是"有图标"和"没图标"在代码里长得一模一样。
#
# 用 LoadLibraryEx(LOAD_LIBRARY_AS_DATAFILE) + FindResource：前者只映射不执行
# （对一个来源不明的 exe 也安全），后者命中就是真的在。
# 资源类型号：3 = RT_ICON（单张位图），14 = RT_GROUP_ICON（资源管理器真正
# 读的那张表），16 = RT_VERSION。
if (-not ([System.Management.Automation.PSTypeName]'D2oPeRes').Type) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class D2oPeRes {
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern IntPtr LoadLibraryEx(string f, IntPtr h, uint flags);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern IntPtr FindResource(IntPtr h, IntPtr name, IntPtr type);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern uint SizeofResource(IntPtr h, IntPtr r);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool FreeLibrary(IntPtr h);
  /// 返回资源字节数；不存在返回 0。
  public static uint Size(string path, int id, int type) {
    IntPtr h = LoadLibraryEx(path, IntPtr.Zero, 0x2);
    if (h == IntPtr.Zero) return 0;
    try {
      IntPtr r = FindResource(h, (IntPtr)id, (IntPtr)type);
      if (r == IntPtr.Zero) return 0;
      return SizeofResource(h, r);
    } finally { FreeLibrary(h); }
  }
}
'@
}

# 跑 python 探针的统一入口。
# `-X utf8` 不能省，也不能换成 PYTHONIOENCODING：**`-I` 隐含 `-E`**，而 -E
# 就是"忽略一切 PYTHON* 环境变量" —— 那个环境变量在这里是死的（陷阱 40
# 开的那副药，在 -I 下不生效，而且一声不吭）。后果有辨识度：stdout 接到
# 管道/文件时 Python 改用**本地代码页**（这里 936）编码，探针退出码照样是 0、
# 断言也是"通过 N 项"，只有把输出按 UTF-8 读回来的人看到一堆乱码。
# 挂在控制台上时看不出来：那条路走 WriteConsoleW（UTF-16），代码页不参与 ——
# 所以同一份脚本，重定向到文件才会露馅。`-X` 是命令行开关，不受 -E 影响。
function Invoke-PythonProbe {
    param([string]$Script, [string[]]$Sw, [int]$TimeoutMs, [string]$WorkDir)
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (-not $py) { return $null }
    return Invoke-Capture $py (@('-I', '-X', 'utf8', $Script) + $Sw) $TimeoutMs $WorkDir
}

# 随包的 .ps1 能不能**解析**。返回 $null 表示没问题，否则是一句错误说明。
#
# 这一条补的是一个真实的漏：安装包验收原本完全不碰随包的 .ps1（只有绿色版
# 会执行 `start.ps1 -Portable`），于是一个**语法都过不去**的 start.ps1 能安安静静
# 装进安装包 —— 43 项全绿 —— 直到用户双击 start.bat 才发现，而那时报的是一句
# 指不到真正原因的 "Missing closing '}'"。
#
# 触发它的那次改动普通得不能再普通：往启动横幅里加一行提示，双引号字符串里
# 又写了一对 ASCII 双引号，字符串当场被截断。同一份坏文件，安装包验收全绿、
# 绿色版报的是"后端没起来"。
#
# 用 Parser 而不是跑一遍：解析只看语法，不执行、不起进程、不碰用户数据，
# 所以对任何随包脚本（包括别人的）都可以放心调。
function Test-ScriptSyntax([string]$Path) {
    $errs = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$null, [ref]$errs)
    if ($errs -and $errs.Count -gt 0) {
        return (($errs | ForEach-Object { "第 $($_.Extent.StartLineNumber) 行：$($_.Message)" }) -join '; ')
    }
    return $null
}

# 登录后读 `/api/status` 的 `version` 字段。读不到返回 $null。
#
# 存在的理由是一条**三方一致**断言：包名 / exe 的文件属性 / 程序在网络上报的
# 版本，必须是同一个数。前两处是文件属性，第三处是**运行期**的 —— 它顺带证明
# 那个字段真的接上了线（`patch_live_counters` 里那一行），而不只是"编译进去了
# 一个常量"。只对文件属性会漏掉"接口上根本没这个字段"，而那种情况界面上是
# 一片空白，谁都不会发现。
#
# `/api/*` 在 Web 登录之后（web_server.cpp 的 register_auth_routes），所以这里
# 得先 POST /api/login 拿会话 cookie —— 用 -SessionVariable 让 PS 自己管。
#
# 默认值取的是出厂那一对 admin/admin，但**用哪一对是调用方的输入，不是这个
# 函数能假定的事**：安装包从第五轮起把向导里填的用户名/口令同时写进
# web.user / web.password（在那之前 WriteConfig 只写了 ONVIF 那一份，这里
# 才碰巧能登进去 —— 那句"安装包那份走默认 admin/admin"已经过期了），
# 而绿色版那份配置由首次运行生成、仍然是内置默认值。所以谁调用谁给值。
function Get-StatusVersion {
    param(
        [int]    $Port,
        [string] $User = 'admin',
        [string] $Password = 'admin',
        [int]    $TimeoutMs = 15000
    )
    $sec = [Math]::Max(1, [int]($TimeoutMs / 1000))
    $base = "http://127.0.0.1:$Port"
    try {
        $body = @{ user = $User; password = $Password } | ConvertTo-Json -Compress
        $null = Invoke-WebRequest -Uri "$base/api/login" -Method Post -Body $body `
                    -ContentType 'application/json' -TimeoutSec $sec `
                    -SessionVariable sess -UseBasicParsing
        $resp = Invoke-WebRequest -Uri "$base/api/status" -WebSession $sess `
                    -TimeoutSec $sec -UseBasicParsing
        return [string](($resp.Content | ConvertFrom-Json).version)
    } catch {
        # 不在这里报错：调用方手里有"期望值"，由它给出一句能对账的失败信息。
        # 这里报的话，消息里只有"登录失败"这种没法定位的话。
        return $null
    }
}

function Write-VerifySummary {
    Write-Host ''
    if ($VerifyCounters.Fail -eq 0) {
        Write-Host ("全部通过：$($VerifyCounters.Pass) 项。") -ForegroundColor Green
        exit 0
    } else {
        Write-Host ("通过 $($VerifyCounters.Pass) 项，失败 $($VerifyCounters.Fail) 项。") -ForegroundColor Red
        exit 1
    }
}
