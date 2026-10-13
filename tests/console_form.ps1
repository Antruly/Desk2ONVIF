# Phase F 的验收：进程形态与生命周期。
#
# 验四件在别处验不到的事：
#   [1] `--service` 里**不改系统状态**的那几个子命令（status / print / 参数写错）
#   [2] 单实例：第二个实例不是错误，要说人话并退 0（用户双击两下的场景）
#   [3] 父进程看守：父进程一没，worker 自己收尾退出（退出码 0、MP4 有 trailer）
#   [4] 控制台中断：真的发一次 Ctrl+C，走的是 CtrlHook 那条优雅收尾
#   [5] `--handoff`：新实例等旧实例把端口让出来，而不是当场宣告"已经在运行"
#
# **故意不验** `--service install` / `uninstall`：要管理员、会真的建/删一个服务对象。
# 那是一次系统改动，这一段没有授权 —— 而 `--service print` 存在的全部意义就是让人
# 在**不改变系统状态**的前提下把那条路看清楚。
#
# 为什么非写成脚本：这四条里有三条是时序相关的（谁先死、多久之内退出），手工敲一遍
# 只能证明那一次。而"单实例"那条尤其不能手工判 —— 手工跑的时候人眼看到的是
# "它说已经在运行了"，看不到"它退的是 0 还是 1"，而后者才是服务看门狗会不会开始
# 退避重拉的分水岭。
#
# 端口固定用 8099 / 8557（不是默认的 8080 / 8554）：跑这个脚本的机器上可能正跑着
# 一个真的在用默认端口的实例，那会让"端口被占"伪装成产品缺陷。
#
# [2] 会**弹一次浏览器**：第二个实例的设计就是"把用户领到那个网页上"，而既然是
# 控制台形态请求的，它就会真的 ShellExecute 一次。这是被测行为本身，不是测试的
# 副作用 —— 想不弹就得改用 `--worker` 起第二个实例，那样 [2] 就不再覆盖双击的场景了。
#
# 用法：powershell -ExecutionPolicy Bypass -File tests\console_form.ps1
param(
    [int]$WebPort = 8099,
    [int]$RtspPort = 8557
)
$ErrorActionPreference = 'Stop'

# 子进程输出一律按 UTF-8 解。必须显式写死 —— ProcessStartInfo 默认取
# Console.OutputEncoding（本机是 936），程序的 UTF-8 字节会被按 GBK 解成乱码，
# 再被我们按 UTF-8 写进日志，于是日志里每个中文字都不对，而日志本身"看着完全正常"。
$utf8 = New-Object Text.UTF8Encoding $false
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root 'out\console-form'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$exe     = Join-Path $root 'build\bin\desk2onvif.exe'
$ffprobe = Join-Path $root 'third_party\ffmpeg\bin\ffprobe.exe'
$helper  = Join-Path $PSScriptRoot 'send_ctrlc.ps1'

$cfgPath = Join-Path $out 'cfg.json'
$logPath = Join-Path $out 'desk2onvif.log'
$mp4A    = Join-Path $out 'parentwatch.mp4'
$mp4B    = Join-Path $out 'ctrlc.mp4'
$mp4C    = Join-Path $out 'handoff.mp4'

$script:failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  [OK]   $what" -ForegroundColor Green }
    else { Write-Host "  [FAIL] $what" -ForegroundColor Red; $script:failed++ }
}
function Section([string]$t) { Write-Host "`n== $t" -ForegroundColor Cyan }
function Note([string]$t) { Write-Host "   · $t" -ForegroundColor DarkGray }

# 起一个子进程并**立刻**开始读它的输出。返回的句柄里带着两个读任务。
#
# 为什么读要立刻开始、而不是等到 WaitForExit：管道只有 4 KB。这个程序每秒往
# stderr 写好几行日志，几秒钟就能把管道填满，然后它就**卡在写 stderr 上** ——
# 不录了、网也不应答了。症状看起来是"产品挂了"，而实际上是我们没读它的输出。
# 这里 [3] 那段要等 8 秒以上，正好够撞上。
function Start-Captured([string]$ArgLine, [string]$WorkDir = $root) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $ArgLine
    $psi.WorkingDirectory = $WorkDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = $script:utf8
    $psi.StandardErrorEncoding = $script:utf8
    $p = New-Object Diagnostics.Process
    $p.StartInfo = $psi
    $p.Start() | Out-Null
    return [pscustomobject]@{
        Proc = $p
        Out  = $p.StandardOutput.ReadToEndAsync()
        Err  = $p.StandardError.ReadToEndAsync()
    }
}

# 带硬超时的等待。超时返回 Code=$null 而不是编一个退出码 —— "没在规定时间内退出"
# 和"退了个非零码"是两件不同的事，混成一个数会让人去查错方向。
function Wait-Captured($h, [int]$TimeoutMs) {
    $p = $h.Proc
    $ok = $p.WaitForExit($TimeoutMs)
    if (-not $ok) {
        try { $p.Kill() } catch {}
        [void]$p.WaitForExit(5000)
    }
    $code = $null
    if ($ok) { try { $code = $p.ExitCode } catch { $code = $null } }
    $outText = ''
    $errText = ''
    try { $outText = $h.Out.Result } catch { }
    try { $errText = $h.Err.Result } catch { }
    return [pscustomobject]@{
        Code     = $code
        Out      = $outText
        Err      = $errText
        TimedOut = (-not $ok)
    }
}

function Run-Captured([string]$ArgLine, [int]$TimeoutMs = 20000) {
    return Wait-Captured (Start-Captured $ArgLine) $TimeoutMs
}

# 一个跑完就退的命令，输出一起打出来（失败时人要能看见程序说了什么）。
function Run-Short([string]$ArgLine, [int]$TimeoutMs = 20000) {
    $r = Run-Captured $ArgLine $TimeoutMs
    if ($r.Code -ne 0) {
        Note "退出码 $($r.Code)；stdout/stderr 见下"
        if ($r.Out) { Write-Host ($r.Out.TrimEnd()) -ForegroundColor DarkGray }
        if ($r.Err) { Write-Host ($r.Err.TrimEnd()) -ForegroundColor DarkGray }
    }
    return $r
}

function Wait-Http([string]$Url, [int]$TimeoutMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        try {
            $r = Invoke-WebRequest -Uri $Url -UseBasicParsing -TimeoutSec 2
            if ($r.StatusCode -eq 200) { return $true }
        } catch { }
        Start-Sleep -Milliseconds 300
    }
    return $false
}

# 停掉**我们自己的**构建产物。按路径比对，不按进程名 —— 这台机器上可能真的装了
# 一份服务形态在跑，那不该被这个测试顺手杀掉。
function Stop-OurExe {
    $killed = @()
    foreach ($p in @(Get-Process -Name desk2onvif -ErrorAction SilentlyContinue)) {
        $path = $null
        try { $path = $p.Path } catch { }
        if ($path -eq $exe) {
            $killed += $p.Id
            try { $p.Kill() } catch {}
        }
    }
    if ($killed.Count -gt 0) { Note "清掉了上次残留的进程：$($killed -join ', ')" }
}

# ---------------------------------------------------------------- 准备
# 上一次跑留下的日志会让 [4] 的断言读到上一次的内容（那条路是追加写）。
foreach ($f in $logPath, $mp4A, $mp4B, $mp4C, (Join-Path $out 'run.log'), (Join-Path $out 'run.err')) {
    Remove-Item $f -ErrorAction SilentlyContinue
}
Stop-OurExe

# 固定配置：Web 与 RTSP 都开、ONVIF 关（它要 3702 组播，和同一台机器上别的实例
# 会互相干扰）、认证关掉（这是 127.0.0.1 上的测试夹具）。
# fps 给 5 是为了让两段录制加起来也不占满一个核。
$json = @"
{
  "web": { "enabled": true, "host": "127.0.0.1", "port": $WebPort, "auth_enabled": false },
  "rtsp": { "enabled": true, "port": $RtspPort },
  "onvif": { "enabled": false },
  "capture": { "quality": "custom", "fps": 5, "bitrate_kbps": 512 }
}
"@
[IO.File]::WriteAllText($cfgPath, $json, $utf8)

Write-Host "Desk2ONVIF 形态与生命周期验收" -ForegroundColor White
Note "exe  $exe"
Note "配置 $cfgPath"
Note "端口 web=$WebPort rtsp=$RtspPort"

# ---------------------------------------------------------------- [0] --help
Section '[0] --help 里要能看出有哪几种形态'
$h = Run-Short '--help'
Check ($h.Code -eq 0) "--help 退出码 0（实际 $($h.Code)）"
foreach ($needle in '--console', '--worker', '--service install', '--service print') {
    Check ($h.Out -match [regex]::Escape($needle)) "--help 提到了 $needle"
}
# 不带参数时是控制台形态 —— 安装包里双击 exe 就该能用。这条只能从文案上验，
# 真跑一次无参启动会用 %APPDATA% 里用户自己那份配置，那是别人的数据。
Check ($h.Out -match '不带参数') "--help 说明了不带参数时的行为"

# ---------------------------------------------------------------- [1] --service
Section '[1] --service 的只读子命令'
$s = Run-Short '--service status'
Check ($s.Code -eq 0) "--service status 退出码 0（只读查询，查询成功就是 0；实际 $($s.Code)）"
$statusText = $s.Out + $s.Err
Check ($statusText -match '没装成服务|正在运行') "--service status 给出明确结论（不是空白）"

$p = Run-Short "--service print --config `"$cfgPath`""
Check ($p.Code -eq 0) "--service print 退出码 0（实际 $($p.Code)）"
Check ($p.Out -match '--service run --config') "--service print 打出了将要写进 binPath 的命令行"
Check ($p.Out -match 'LocalSystem') "--service print 说明了账户是 LocalSystem（并解释了为什么）"
Check ($p.Out -match 'SE_TCB_NAME') "--service print 讲清了 LocalSystem 的理由（WTSQueryUserToken 要 SE_TCB_NAME）"
# 相对路径是这条子命令唯一静默失效的地方：服务的工作目录是 C:\Windows\System32。
Check ([IO.Path]::IsPathRooted($cfgPath)) "传给 print 的配置路径是绝对路径（服务形态的前提）"

$b = Run-Short '--service bogus'
Check ($b.Code -eq 2) "--service 参数写错时退出码是 2（用法错误；实际 $($b.Code)）"
Check (($b.Out + $b.Err) -match 'install') "参数写错时打出了用法"

# ---------------------------------------------------------------- [2][3] 夹具
# [2] 和 [3] 共用同一个实例，因为它们本来就是同一件事的两面：**那个持有单实例锁的
# 进程**同时也要能被父进程句柄叫停。分开起两次只会让 [3] 多花 8 秒，并且盖不住
# "持有锁的正是 worker"这一点。
Section '[2] 单实例：第二个实例要说人话并退 0'
$parent = Start-Process -FilePath 'powershell.exe' `
    -ArgumentList '-NoProfile', '-Command', 'Start-Sleep -Seconds 300' `
    -PassThru -WindowStyle Hidden
Note "假父进程 pid=$($parent.Id)"

$a = Start-Captured "--worker --parent-pid $($parent.Id) --record `"$mp4A`" --config `"$cfgPath`""
Note "worker pid=$($a.Proc.Id)"

$up = Wait-Http "http://127.0.0.1:$WebPort/api/status" 25000
Check $up "长驻形态的网页起来了（/api/status 200）"

$b = Run-Captured "--console --config `"$cfgPath`"" 20000
Check (-not $b.TimedOut) "第二个实例没有挂在那里（它该立刻退）"
Check ($b.Code -eq 0) "第二个实例退出码是 0（**不是错误** —— 服务看到非零会开始退避重拉；实际 $($b.Code)）"
Check ($b.Out -match '已经在运行') "第二个实例说了人话（不是把错误码甩给用户）"
Check ($b.Out -match ":$WebPort") "第二个实例把网页地址告诉了用户"

Section '[3] 父进程看守：父进程一没就自己收尾退出'
Start-Sleep -Seconds 8
$mp4ABytes = 0
if (Test-Path $mp4A) { $mp4ABytes = (Get-Item $mp4A).Length }
Check ($mp4ABytes -gt 20000) "杀掉父进程之前真的在录（$mp4ABytes 字节）"

$sw = [Diagnostics.Stopwatch]::StartNew()
Stop-Process -Id $parent.Id -Force
$r = Wait-Captured $a 30000
$sw.Stop()
Note "父进程死后 $($sw.ElapsedMilliseconds) ms 内退出"
Check (-not $r.TimedOut) "父进程一没，worker 在 30 秒内自己退了（不是变成孤儿进程占着端口）"
Check ($r.Code -eq 0) "退出码 0（优雅收尾；实际 $($r.Code)）"
Check ($r.Err -match '父进程没了') "stderr 说了原因（否则用户只知道它自己没了）"

$durA = & $ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 $mp4A 2>&1
Check ($LASTEXITCODE -eq 0) "ffprobe 读得出 $([IO.Path]::GetFileName($mp4A))（读不出 = moov 没写成 = 被截断）"
$durAnum = 0.0
if ([double]::TryParse($durA, [ref]$durAnum)) {
    Check ($durAnum -ge 4) "时长 $durAnum 秒（录了 8 秒，说明收尾时把缓冲冲干净了）"
} else {
    Check $false "时长读不出来（ffprobe 说：$durA）"
}
Check (Test-Path $logPath) "长驻形态把日志落到了配置旁边（worker 没有控制台，没有这一份就没法查）"

# ---------------------------------------------------------------- [4] Ctrl+C
Section '[4] 控制台中断：Ctrl+C 走优雅收尾'
# 这一路必须给子进程**自己的控制台**：`GenerateConsoleCtrlEvent` 只对调用者挂着的
# 那个控制台发事件，而我们的控制台是这个测试脚本自己的。UseShellExecute=$true 才会
# 让 ShellExecuteEx 给控制台程序开一个新控制台。
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = "--console --record `"$mp4B`" --seconds 120 --config `"$cfgPath`""
$psi.WorkingDirectory = $root
$psi.UseShellExecute = $true
$psi.WindowStyle = 'Minimized'
$c = [Diagnostics.Process]::Start($psi)
Note "控制台实例 pid=$($c.Id)"

Start-Sleep -Seconds 8
$mp4BBytes = 0
if (Test-Path $mp4B) { $mp4BBytes = (Get-Item $mp4B).Length }
Check ($mp4BBytes -gt 20000) "发 Ctrl+C 之前真的在录（$mp4BBytes 字节）"

# 发事件这一步必须在**独立进程**里做：它自己要 FreeConsole + AttachConsole，
# 而 FreeConsole 会把调用者自己的控制台也扔了 —— 放在这个脚本里做，后面所有的
# 测试输出就都没地方去了。
$send = Start-Process -FilePath 'powershell.exe' `
    -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $helper, '-TargetPid', $c.Id `
    -PassThru -WindowStyle Hidden
[void]$send.WaitForExit(30000)
switch ($send.ExitCode) {
    0 { Note "Ctrl+C 已投递" }
    1 { Note "AttachConsole 失败（退出码 1）" }
    2 { Note "GenerateConsoleCtrlEvent 失败（退出码 2）" }
    default { Note "helper 退出码 $($send.ExitCode)" }
}
Check ($send.ExitCode -eq 0) "Ctrl+C 投递成功（helper 退出码 0；实际 $($send.ExitCode)）"

$sw2 = [Diagnostics.Stopwatch]::StartNew()
$exited = $c.WaitForExit(30000)
$sw2.Stop()
if (-not $exited) { try { $c.Kill() } catch {}; [void]$c.WaitForExit(5000) }
Check $exited "收到 Ctrl+C 后 30 秒内退出了（不等的话用户只能去任务管理器）"
if ($exited) {
    Note "中断后 $($sw2.ElapsedMilliseconds) ms 退出，退出码 $($c.ExitCode)"
    Check ($c.ExitCode -eq 0) "退出码 0（不是 0xC000013A = 被默认 Ctrl+C 处理直接砍掉；实际 $($c.ExitCode)）"
}

$durB = & $ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 $mp4B 2>&1
Check ($LASTEXITCODE -eq 0) "ffprobe 读得出 $([IO.Path]::GetFileName($mp4B))"
$durBnum = 0.0
if ([double]::TryParse($durB, [ref]$durBnum)) {
    Check ($durBnum -ge 4) "时长 $durBnum 秒（关窗口/Ctrl+C 之前录的都在，且 trailer 完整）"
} else {
    Check $false "时长读不出来（ffprobe 说：$durB）"
}

# ---------------------------------------------------------------- [5] --handoff
Section '[5] --handoff：等前一个实例让出位置，而不是当场宣告"已经在运行"'
# 这条是控制台形态自重启（Phase H 的 `/api/restart`）的地基：新进程先起来，
# 而旧进程还要几百毫秒才把互斥体和两个端口放掉。没有 --handoff 的话，新进程
# 在这一瞬间看到的正是"已经在运行"，于是它退 0 走人 —— 重启变成"关掉了、没起来"，
# 而日志上只有一句"已经在运行"，看起来完全正常。
$parent2 = Start-Process -FilePath 'powershell.exe' `
    -ArgumentList '-NoProfile', '-Command', 'Start-Sleep -Seconds 300' `
    -PassThru -WindowStyle Hidden
$a5 = Start-Captured "--worker --parent-pid $($parent2.Id) --record `"$mp4C`" --config `"$cfgPath`""
Check (Wait-Http "http://127.0.0.1:$WebPort/api/status" 25000) "先起一个实例，等它把网页开起来"

$b5 = Start-Captured "--console --handoff 8000 --config `"$cfgPath`""
Start-Sleep -Seconds 2
Check (-not $b5.Proc.HasExited) "--handoff 期间新实例还活着（它没有当场放弃）"

# 让老的那个优雅退出：放开互斥体和两个端口。
Stop-Process -Id $parent2.Id -Force
$r5 = Wait-Captured $a5 30000
Check ($r5.Code -eq 0) "旧实例优雅退出（退出码 $($r5.Code)）"

Check (Wait-Http "http://127.0.0.1:$WebPort/api/status" 20000) "位置让出来之后新实例接上了（网页又回来了）"
Check (-not $b5.Proc.HasExited) "现在在上面的是**新**实例（它没被那个'已经在运行'判掉）"
if (-not $b5.Proc.HasExited) {
    try { $b5.Proc.Kill() } catch {}
    [void]$b5.Proc.WaitForExit(5000)
}

# ---------------------------------------------------------------- 收尾
Stop-OurExe

Write-Host ""
if ($script:failed -eq 0) {
    Write-Host "全部通过。" -ForegroundColor Green
} else {
    Write-Host "$($script:failed) 项失败。" -ForegroundColor Red
}
Write-Host "现场：$out" -ForegroundColor DarkGray
exit $script:failed
