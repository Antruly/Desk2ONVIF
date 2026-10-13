# 多通道端到端（第五轮 Phase AD）：**三次**独立的采集运行，每次验一件只有
# "真的跑起来"才答得出来的事。
#
# ## 为什么造不出三路（以及这一点本身就是结论）
#
# 这台机器**只有一块显示输出**。我一开始的写法是"通道 2 也去抓同一块屏"，
# 想着这样能凑出两路独立的 DXGI 采集 —— **那一路必然失败**：DXGI 的桌面复制
# 在一块输出上同时只允许一个 `IDXGIOutputDuplication`（第二个 `DuplicateOutput`
# 直接返回 `E_INVALIDARG`）。所以这台机器上能同时跑的**独立**画面来源最多
# 两路：那块屏 + 那台摄像头。
#
# 这个失败同时也是一条**产品**结论，第五轮把它变成了**规划期就拦掉**：
# `plan_channels` 的规则 2 认得出"这块屏已经被占了"，那一路 `enabled = false`
# 并带一句给用户看的原因（`blocked_reason`）。第三幕验的就是这条。
#
# ⚠️ 拦掉的**理由**是实测撞出来的，不是想出来的：那个 duplication 是**谁先
# 谁后赛跑**的（通道 2 的采集线程先起、通道 1 的 `session.run()` 后跑），所以
# 不拦的话结果有两种 —— 要么这一路红，要么**通道 1** 的 `DuplicateOutput`
# 失败、`run()` 返回 Failed、整个进程退出码 1，**服务直接没了**。
# 实测 out\mc-e2e\run7.txt（活下来）与 run8.txt（进程退出）是同一份配置的两趟。
# 正因为是赛跑，第三幕在脚本里**没法**稳定重现那两种结局中的任意一种 ——
# 它能验的只有"服务在、规划期那句话在"；规则本身由
# `tests\channel_plan_check.cpp` 的 [7] 段逐条钉住（纯函数，不需要双屏机器）。
#
# ## 三幕各自回答什么
#
#   第一幕 `--camera`：通道 1 = 摄像头，**通道 2 由自动发现建出来**（显示器
#        #0）。这一幕是全套里**唯一**会真的走到自动发现那条路的场景 ——
#        在一台只有一块屏的机器上它本来一次都不发生。验：两路各有各的 ONVIF
#        端口/设备/序列号/RTSP 路径、一次 Probe 只回一份 ProbeMatch、能同时拉、
#        各录各的。
#   第二幕 坏摄像头：通道 2 指到一台不存在的摄像头。验"坏一路不拖垮别人"。
#   第三幕 撞屏：通道 2 显式写通道 1 正在采的那块屏 → 规划期就挡住它。
#        验：**服务照样起来**（这是修掉的那个致命后果）、通道 1 毫发无损、
#        被挡的那一路在状态里带着原因、那一句原因在启动头几秒就印出来。
#
# ## 桌面必须被"动一下"
#
# DXGI 的桌面复制**只在桌面变化时才给帧**。静止的桌面上，一路显示器通道会
# 一帧都拿不到（日志里那句"还没取到任何桌面画面，等待桌面发生变化…"），
# 而 fps=0 看起来像采集坏了。实测：指针动起来之后同一路立刻稳在 20fps 上下。
# 所以整个脚本期间有一个后台任务在画小圈，结束时把指针放回原处。
#
# 用法：powershell -ExecutionPolicy Bypass -File tests\multichannel_e2e.ps1
param(
    [int]$RtspPort = 8554,
    [int]$HttpPort = 8099,
    [string]$Camera = 'FHD Camera',
    [string]$HttpUser = 'admin',
    [string]$HttpPassword = 'admin'
)
$ErrorActionPreference = 'Stop'
# 子进程输出一律按 UTF-8 解。**必须显式写死，不能靠控制台的代码页** ——
# 不设的话 ProcessStartInfo 用的是 Console.OutputEncoding（默认 936），程序的
# UTF-8 字节被按 GBK 解成乱码，再被 WriteAllText 按 UTF-8 写进日志，成为"双重
# 编码"。症状是日志里每一个中文字都不对，而这跟程序本身毫无关系。
$utf8 = New-Object Text.UTF8Encoding $false
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root 'out\mc-e2e'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# 起过的采集进程名册，`Start-App` 往里加、`finally` 兜底收尸。见 `Start-App`。
$script:apps = @()

$exe     = Join-Path $root 'build\bin\desk2onvif.exe'
$ffprobe = Join-Path $root 'third_party\ffmpeg\bin\ffprobe.exe'
$ffmpeg  = Join-Path $root 'third_party\ffmpeg\bin\ffmpeg.exe'

# 通道 2 的 ONVIF 端口是 `web.port + **2**`，不是 `+1`。
#
# 这条约定是"**通道 N 在 `web.port + N`**"（用户选的那个形态），而通道 2 的
# 通道号就是 2。写成 `+1` 的话这个脚本会在 8100 上等一个永远不来的服务，
# 而真正的端点 8101 就在它隔壁 —— 报出来的是"通道 2 的端口没起来"，看起来
# 像多通道的端口分配坏了，其实只是我按"第几路"而不是"通道号"算了一次。
$Ch2Port = $HttpPort + 2

$failed = 0
function Check([bool]$ok, [string]$what, [string]$hint = '') {
    if ($ok) { "  [OK]   $what" }
    else { "  [FAIL] $what"; if ($hint) { "         $hint" }; $script:failed++ }
}

# 解码判据的两把尺子（全文在 `tests\rtsp_sub_e2e.ps1` 的 Check 下面）。
# 一、`-f null` 那条 `non monotonically increasing dts` 是 **muxer 自己的账**，
#     不是这一路流的问题，放它过去；其余任何一个字都算致命。
# 二、帧数只有下界有意义：控制器最慢也只降到 `adaptive.min_fps`。
$script:DtsNote = 'Application provided invalid, non monotonically increasing dts to muxer in stream 0: \d+ >= \d+'
function Split-PullErr([string]$Text) {
    $lines = @($Text -split "`r?`n" | Where-Object { $_.Trim() })
    return [pscustomobject]@{
        Notes = @($lines | Where-Object { $_ -match $script:DtsNote })
        Fatal = @($lines | Where-Object { $_ -notmatch $script:DtsNote })
    }
}
function Get-PullFrames([string]$ProgressText) {
    $n = 0
    foreach ($line in ($ProgressText -split "`r?`n")) {
        if ($line -match '^frame=(\d+)') { $n = [int]$Matches[1] }
    }
    return $n
}

# --- 桌面刺激器 -----------------------------------------------------------
#
# 见文件头。指针动 = 桌面变化 = DXGI 给帧。
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Namespace D2O -Name Cursor -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out System.Drawing.Point p);
'@ -ReferencedAssemblies System.Drawing

$saved = New-Object Drawing.Point
[void][D2O.Cursor]::GetCursorPos([ref]$saved)
$jig = $null

# 硬超时的外部命令。没有这一层，一次 ffprobe 挂死就会吃掉整个测试 ——
# 而 ffprobe 拉一路不存在的直播流**会永远等下去**（RTSP 的 DESCRIBE 没人
# 应答时它不返回），那是 `tests\rtsp_sub_e2e.ps1` 文件头里那条教训。
function Invoke-WithTimeout {
    param([string]$File, [string[]]$Argv, [int]$TimeoutMs)
    $h = Start-Cmd $File $Argv
    return (Finish-Cmd $h $TimeoutMs)
}

# 分两步起的进程：`Start-Cmd` 只负责把两条管道立刻异步读走（子进程写满一个
# 没人读的管道就会阻塞，阻塞在哪一步决定了失败长什么样），`Finish-Cmd`
# 负责等 + 硬超时 + **真的杀掉进程**。
#
# ⚠️ 这两件事必须分开，"同时拉两路"才验得成：`Start-Job` 做不到 —— 它实测会
# 留下**活着的 ffmpeg 孤儿**（`Remove-Job -Force` 只杀 runspace，杀不到子进程），
# 而孤儿占着 RTSP 会话，后面每一次探测都跟着挂。
function Start-Cmd {
    param([string]$File, [string[]]$Argv)
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.Arguments = ($Argv | ForEach-Object { '"' + $_ + '"' }) -join ' '
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
        P = $p
        Out = $p.StandardOutput.ReadToEndAsync()
        Err = $p.StandardError.ReadToEndAsync()
    }
}

function Finish-Cmd {
    param($H, [int]$TimeoutMs)
    if (-not $H.P.WaitForExit($TimeoutMs)) {
        try { $H.P.Kill() } catch {}
        try { $H.P.WaitForExit(5000) | Out-Null } catch {}
        return [pscustomobject]@{ Out = ''; Err = "超时 ${TimeoutMs}ms"; TimedOut = $true
                                  ExitCode = -1 }
    }
    return [pscustomobject]@{ Out = $H.Out.Result; Err = $H.Err.Result
                              TimedOut = $false; ExitCode = $H.P.ExitCode }
}

# ffprobe 一个流/文件，返回字段表。
function Probe {
    param([string]$Url, [int]$TimeoutMs = 20000)
    # `-rtsp_transport tcp` **只在真的是 RTSP 时给**。它是 RTSP 解复用器的
    # 私有选项，喂给 mp4 解复用器会变成"Option rtsp_transport not found"
    # 直接失败 —— 同一段代码在文件和流两条路上一个能用、一个报错。
    $argv = @('-v', 'error')
    if ($Url -like 'rtsp://*') { $argv += @('-rtsp_transport', 'tcp') }
    $argv += @('-select_streams', 'v:0',
               '-show_entries', 'stream=codec_name,width,height',
               '-of', 'default=noprint_wrappers=1', '-i', $Url)
    $r = Invoke-WithTimeout $ffprobe $argv $TimeoutMs
    $kv = @{ _timedout = $r.TimedOut; _err = $r.Err }
    foreach ($line in ($r.Out -split "`r?`n")) {
        if ($line -match '^(\w+)=(.*)$') { $kv[$Matches[1]] = $Matches[2] }
    }
    return $kv
}

# 拉一路 RTSP 解 N 秒。
#
# `-fps_mode passthrough` 的理由写在 `tests\rtsp_sub_e2e.ps1` 的 Start-Pull
# 上面（2026-10-09 那次全量回归的现场实验，长话短说：默认 vsync 会把
# `adaptive.mode = "throttle"` 下**本来就该变动**的帧率折算成恒定帧率，
# 折算时偶发两帧落到同一个 1/25 刻度上，于是报
# `non monotonically increasing dts to muxer` —— 那是折算器自己的损失，
# 不是这一路流的问题）。这里和那里是同一类断言，所以用同一个开关。
function Start-Pull {
    param([string]$Url, [int]$Secs)
    return Start-Cmd $ffmpeg @('-v', 'error', '-rtsp_transport', 'tcp',
                              '-i', $Url, '-t', "$Secs",
                              '-fps_mode', 'passthrough',
                              '-progress', 'pipe:1', '-f', 'null', '-')
}

function Wait-Port {
    param([int]$Port, [int]$Secs = 25)
    $deadline = (Get-Date).AddSeconds($Secs)
    while ((Get-Date) -lt $deadline) {
        $c = New-Object Net.Sockets.TcpClient
        try { $c.Connect('127.0.0.1', $Port); $c.Close(); return $true }
        catch { Start-Sleep -Milliseconds 300 }
    }
    return $false
}

# 等一个端口**空出来**。每一幕开始之前都要等上一幕那三条端口真的放开 ——
# 上一幕的进程刚退出、端口还在 TIME_WAIT 或还没被内核回收时，这一幕的程序
# 会**先绑上 8099、再在起 RTSP 时失败然后退出**。于是 `Wait-Port 8099` 是
# 成功的，十几秒后动 `/api/*` 却是"连不上" —— 报出来像产品随机崩溃，
# 实际只是上一幕的端口还没放干净。
function Wait-PortFree {
    param([int[]]$Ports, [int]$Secs = 20)
    $deadline = (Get-Date).AddSeconds($Secs)
    while ((Get-Date) -lt $deadline) {
        $busy = @()
        foreach ($port in $Ports) {
            $c = New-Object Net.Sockets.TcpClient
            try { $c.Connect('127.0.0.1', $port); $c.Close(); $busy += $port }
            catch {}
        }
        if ($busy.Count -eq 0) { return $true }
        Start-Sleep -Milliseconds 300
    }
    return $false
}

# 起一次采集。
function Start-App {
    param([string]$Tag, [string]$CfgPath, [string]$RecordPath, [int]$Secs,
          [string[]]$Extra)
    # 先等上一幕那三条端口全放开再起。见 `Wait-PortFree` 上面的注释：
    # 不等的话这一幕会绑上网页端口、然后死在起 RTSP 那一步，十几秒后
    # `/api/*` 报"连不上"，看起来像产品随机崩溃。
    $null = Wait-PortFree @($HttpPort, $Ch2Port, $RtspPort) 20
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    # `--web-root` 和 `WorkingDirectory` 都必须显式给：静态目录的默认值是
    # **相对路径** `web/dist`，按**子进程的工作目录**解析，而 ProcessStartInfo
    # 不设 WorkingDirectory 时继承的是调用方的 cwd。从别的目录跑这个脚本时，
    # 服务照起、推流照推，只有静态产物变成 404。
    $psi.WorkingDirectory = $root
    $argv = @('--rtsp', '--rtsp-port', "$RtspPort", '--http', '--http-port', "$HttpPort",
              '--record', $RecordPath, '--seconds', "$Secs",
              '--config', $CfgPath, '--http-user', $HttpUser,
              '--http-password', $HttpPassword,
              '--web-root', (Join-Path $root 'web\dist'))
    if ($Extra) { $argv += $Extra }
    $psi.Arguments = ($argv | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = $utf8
    $psi.StandardErrorEncoding = $utf8
    $p = New-Object Diagnostics.Process
    $p.StartInfo = $psi
    if (-not $p.Start()) { throw "起不来：$exe" }
    $app = [pscustomobject]@{
        Tag = $Tag
        P = $p
        Out = $p.StandardOutput.ReadToEndAsync()
        Err = $p.StandardError.ReadToEndAsync()
    }
    # **登记进花名册**，由 `finally` 兜底收尸。
    #
    # 每一幕正常走完都会自己 `Wait-App` / `Stop-App`，花名册是空的。但"端口没
    # 起来"那条路会提前分叉 —— 少了这个兜底，那一幕的进程就在后台活到自己
    # `--seconds` 到点，而这几十秒里它**占着 8099 / 8101 / 8554**。下一次跑
    # 这个脚本（或者紧接着的下一幕）会在"端口被占"上失败，报出来像产品起不来。
    $script:apps += ,$app
    return $app
}

# 把两条管道落盘。落盘不能等进程对象被回收 —— 采集线程最后那几行
# （"通道 N 停了：…"）是第二、三幕唯一的正面证据。
function Dump-Logs {
    param($App)
    [IO.File]::WriteAllText((Join-Path $out "$($App.Tag).out.log"), $App.Out.Result, $utf8)
    [IO.File]::WriteAllText((Join-Path $out "$($App.Tag).err.log"), $App.Err.Result, $utf8)
    return ([IO.File]::ReadAllText((Join-Path $out "$($App.Tag).err.log"), $utf8) + "`n" +
            [IO.File]::ReadAllText((Join-Path $out "$($App.Tag).out.log"), $utf8))
}

# **让它自己到点退出**（`--seconds`），然后落盘。
#
# 第一幕非这样不可：MP4 的索引（moov）是**正常收尾时才写**的，进程被 Kill
# 掉的录像文件是"moov atom not found"—— ffprobe 一句"Invalid data"，看起来
# 像封装器坏了，其实只是我把一个还在录的进程砍了。
function Wait-App {
    param($App, [int]$ExtraMs = 45000)
    if (-not $App.P.WaitForExit($ExtraMs)) {
        try { $App.P.Kill() } catch {}
        try { $App.P.WaitForExit(10000) | Out-Null } catch {}
    }
    return (Dump-Logs $App)
}

# 提前收尾：杀掉 + 落盘。用在"不该再等下去"的路径上（端口没起来、断言已经
# 跑完但不想再等二十秒）。
function Stop-App {
    param($App, [int]$GraceWaitMs = 3000)
    $App.P.WaitForExit($GraceWaitMs) | Out-Null
    if (-not $App.P.HasExited) { try { $App.P.Kill() } catch {} }
    try { $App.P.WaitForExit(10000) | Out-Null } catch {}
    return (Dump-Logs $App)
}

# 两个 HTTP 助手都**不抛异常**。`$ErrorActionPreference = 'Stop'` 之下，
# "连不上"会变成整个脚本的中断 —— 后面的幕一次都不跑，报告里也看不出发生过
# 什么。这里让它返回 $null，由调用方那几条断言去红，而 `Assert-Alive` 负责
# 说出真正的原因（进程死了 + 它最后几行日志）。
function Login {
    $sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
    $body = @{ user = $HttpUser; password = $HttpPassword } | ConvertTo-Json -Compress
    try {
        Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/api/login" -Method Post `
            -Body $body -ContentType 'application/json' -WebSession $sess `
            -UseBasicParsing -ErrorAction Stop | Out-Null
    } catch {
        return $null
    }
    return $sess
}

function Get-Channels {
    param($Sess)
    try {
        $st = Invoke-RestMethod -Uri "http://127.0.0.1:$HttpPort/api/status" `
            -WebSession $Sess -ErrorAction Stop
    } catch {
        return $null
    }
    if ($null -eq $st.channels) { return $null }
    # `@(...)` 是必须的：5.1 的 `Invoke-RestMethod` 不做数组展开，单元素数组会
    # 变成裸对象，`.Count` 在那上面是 `$null`，而"两行"那条断言会以一个看着像
    # 产品 bug 的形状失败。
    return @($st.channels)
}

function By-Index {
    param($Rows)
    $m = @{}
    foreach ($c in $Rows) { $m[[int]$c.index] = $c }
    return $m
}

# 每一幕动 `/api/*` **之前**先确认进程还活着。
#
# 少了这一步，进程提前退出会让 `Invoke-WebRequest` 抛"无法连接到远程服务器"，
# 而 `$ErrorActionPreference = 'Stop'` 把它变成**整个脚本的中断** —— 后面的
# 幕一次都不跑，报告里也看不出发生了什么。这和陷阱 91 是同一个形状：一个
# 被吞掉的异常看起来和"预期中的否定结果"一模一样。这里要么说出"进程死了 +
# 它最后几行日志"，要么什么也别说。
function Assert-Alive {
    param($App, [string]$Stage)
    if (-not $App.P.HasExited) { return $true }
    Check $false "$Stage：进程已经退出了（ExitCode=$($App.P.ExitCode)）"
    $txt = Dump-Logs $App
    "         日志最后 15 行："
    ($txt -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 15) |
        ForEach-Object { "           $_" }
    return $false
}

# 打一个端口上的 ONVIF 设备。返回那个 JSON 对象（或 $null）。
function Probe-Device {
    param([int]$Port)
    $r = Invoke-WithTimeout 'python' @('-I','-X','utf8',
            (Join-Path $root 'tests\onvif_dev_probe.py'),
            '--port', "$Port", '--user', $HttpUser, '--password', $HttpPassword,
            '--json') 25000
    foreach ($line in ($r.Out -split "`r?`n")) {
        if ($line.StartsWith('PROBE ')) { return ($line.Substring(6) | ConvertFrom-Json) }
    }
    return $null
}

function Write-Cfg {
    param([string]$Path, [string]$ItemsJson)
    Remove-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    $json = @"
{
  "web": { "port": $HttpPort, "host": "127.0.0.1" },
  "rtsp": { "port": $RtspPort },
  "channels": { "items": $ItemsJson }
}
"@
    [IO.File]::WriteAllText($Path, $json, $utf8)
}

try {
  if (-not (Test-Path $exe)) { throw "先构建：scripts\build.ps1（找不到 $exe）" }

  $jig = Start-Job -ScriptBlock {
      Add-Type -Namespace D2O -Name C -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
'@
      # 在屏幕中偏左上画一个半径 60 的圈。600 步 × 150ms = 90 秒，比三幕加起来
      # 还长；跑完还没被停就自己停。
      for ($i = 0; $i -lt 600; $i++) {
          $a = $i * 0.25
          [void][D2O.C]::SetCursorPos([int](600 + 60 * [Math]::Cos($a)),
                                      [int](400 + 60 * [Math]::Sin($a)))
          Start-Sleep -Milliseconds 150
      }
  }

  # =========================================================================
  # 第一幕：两路都好（通道 1 = 摄像头，通道 2 = 自动发现的显示器 #0）
  # =========================================================================
  "`n[*] 第一幕：--camera 把通道 1 给摄像头，通道 2 由自动发现建出来"
  $cfgA = Join-Path $out 'cfg-a.json'
  Write-Cfg $cfgA '[]'
  foreach ($f in 'a1.mp4', 'a1-ch2.mp4') {
      Remove-Item (Join-Path $out $f) -ErrorAction SilentlyContinue
  }
  # 预算给足：起服务 + 两路首帧约 6s，[1] 1s，[2] 两次探针约 6s，[3] WS-Discovery
  # 往返 8s，[4] 两路并发拉 8s，[4] 末尾两次流探测约 4s ≈ 33s。给短了的表现是
  # 后半段跑到一半采集自己到点退出，那几段报的是"连不上" —— 看起来像服务坏了，
  # 其实只是测试比程序跑得久。
  #
  # **[5] 故意排在"进程自己退出之后"**：MP4 的索引是正常收尾时才写的，见
  # `Wait-App` 上的说明。
  $appA = Start-App 'a' $cfgA (Join-Path $out 'a1.mp4') 50 @('--camera', $Camera)
  $portOk = Wait-Port $HttpPort 30
  Check $portOk "Web 端口 $HttpPort 起来了"
  if (-not $portOk) {
      $txt = Stop-App $appA
      "  （端口没起来，后面全部无从谈起；看 out\mc-e2e\a.err.log）"
      ($txt -split "`r?`n" | Select-Object -Last 20) -join "`n"
      exit 1
  }
  Check (Wait-Port ($Ch2Port) 20) "通道 2 的 ONVIF 端口 $($Ch2Port) 起来了"
  Check (Wait-Port $RtspPort 20) "RTSP 端口 $RtspPort 起来了"
  Start-Sleep -Seconds 6

  # --- [1] channels 数组 ----------------------------------------------------
  "`n[1] 状态里的 channels 数组"
  $null = Assert-Alive $appA '第一幕'
  $sess = Login
  $chs = Get-Channels $sess
  Check ($null -ne $chs -and $chs.Count -eq 2) `
        "两行（实际 $(if ($chs) { $chs.Count } else { 'null' }) 行）" `
        ("收到：" + (($chs | ForEach-Object { "$($_.index):$($_.name)/$($_.source)/$($_.state)" }) -join ' | '))
  # 字段集合**相等**，不是"包含"。少一个键前端表格就多一个 undefined，
  # 多一个键说明有人往这里塞了没人画的东西 —— 两种都得当场知道。
  $wantKeys = @('index','name','source','state','fps','bitrate_kbps','record_file','record_bytes','web_port','rtsp_path') | Sort-Object
  $k0 = @($chs[0].PSObject.Properties.Name) | Sort-Object
  Check (($k0 -join ',') -eq ($wantKeys -join ',')) "字段集合与约定一致" `
        ("实际 " + ($k0 -join ',') + "，想要 " + ($wantKeys -join ','))
  $b = By-Index $chs
  Check ($b.ContainsKey(1) -and $b.ContainsKey(2)) `
        "通道号 1 / 2（实际 " + (@($b.Keys | Sort-Object) -join '/') + "）"
  Check ($b[1].source -eq 'webcam') "通道 1 的来源是 webcam（实际 $($b[1].source)）"
  Check ($b[2].source -eq 'display') "通道 2 的来源是 display（实际 $($b[2].source)）"
  Check ($b[2].name -eq '显示器 #0') `
        "通道 2 的名字带着**显示器**序号（实际「$($b[2].name)」）" `
        "写成通道号的话，用户按界面上那句去 --display N 会指到一块别的屏上"
  # 每个通道的 ONVIF 端口与 RTSP 路径。**由后端给，前端不许自己加** ——
  # `web.port` 未必是 8080（命令行能改），而"界面上写的端口连不上"是最难查的一类。
  Check ([int]$b[1].web_port -eq $HttpPort) `
        "通道 1 的 web_port = $HttpPort 本身，不是 +1（实际 $($b[1].web_port)）"
  Check ([int]$b[2].web_port -eq $Ch2Port) `
        "通道 2 的 web_port = $Ch2Port（实际 $($b[2].web_port)）"
  Check ("$($b[2].rtsp_path)" -eq '/ch2') `
        "通道 2 的 rtsp_path = /ch2（实际 $($b[2].rtsp_path)）"
  Check ("$($b[1].rtsp_path)".StartsWith('/')) `
        "通道 1 的 rtsp_path 带前导斜杠（实际 $($b[1].rtsp_path)）" `
        "两边写法不一致的话，界面上同一张表里会出现 /live 和 ch2 两种样子"
  # "两行都在"和"两路都在动"是两件事：停在"未启动"的一行照样会出现在数组里
  # （那是故意的），所以光数行数证明不了任何东西在跑。
  Check ([double]$b[1].fps -gt 0) "通道 1 的 fps > 0（实际 $($b[1].fps)）"
  Check ([double]$b[2].fps -gt 0) `
        "通道 2 的 fps > 0（实际 $($b[2].fps)）" `
        "桌面静止时 DXGI 不给帧 —— 刺激器没跑起来就会是这个症状"
  Check ([int64]$b[1].record_bytes -gt 0) "通道 1 的 record_bytes > 0（$($b[1].record_bytes)）"
  Check ([int64]$b[2].record_bytes -gt 0) "通道 2 的 record_bytes > 0（$($b[2].record_bytes)）"

  # --- [2] 两个端口上是两台不同的 ONVIF 设备 --------------------------------
  "`n[2] 端口 1 与端口 2 是两台不同的 ONVIF 设备"
  $j1 = Probe-Device $HttpPort
  $j2 = Probe-Device ($Ch2Port)
  Check ($null -ne $j1 -and -not $j1.err) "端口 $HttpPort 答出了设备信息" `
        "探针返回：" + $(if ($j1) { $j1.err } else { '<没有 PROBE 行>' })
  Check ($null -ne $j2 -and -not $j2.err) "端口 $($Ch2Port) 答出了设备信息" `
        "探针返回：" + $(if ($j2) { $j2.err } else { '<没有 PROBE 行>' })

  if ($j1 -and $j2 -and -not $j1.err -and -not $j2.err) {
      # **这一条是多通道的地基。** 序列号相同的话 NVR 会把两个端口当成同一台
      # 设备，后加的那一路直接覆盖先加的那一路 —— 症状是"只能加进去一个通道"，
      # 而两个端口各自看起来都完全正常。
      Check ($j1.serial -ne $j2.serial) `
            "两台的 SerialNumber 不同（$($j1.serial) vs $($j2.serial)）" `
            "相同的话 NVR 会把它们当成同一台，只能加进去一个"
      Check ($j2.model -like '*(通道 2)*') `
            "通道 2 的型号里带着「(通道 2)」（实际「$($j2.model)」）" `
            "NVR 的设备列表里两台长得一模一样时，用户就是靠这一行字对上号的"
      Check ($j1.model -notlike '*(通道 *)') `
            "通道 1 的型号没有被加上通道后缀（实际「$($j1.model)」）"
      # GetStreamUri 报出来的**路径**必须和这一路自己的 RTSP 路径一致。这才是
      # "通道 2 真的推在 /ch2 上"的正面证据 —— `/api/status` 里写着 `/ch2` 是
      # **我们自己的话**，不是流真的在那儿。
      $u1 = ($j1.profiles | Where-Object { $_.token -eq 'main' } | Select-Object -First 1).uri
      $u2 = ($j2.profiles | Where-Object { $_.token -eq 'main' } | Select-Object -First 1).uri
      Check ($u1 -like '*/live') "通道 1 的 GetStreamUri 指向 /live（实际 $u1）"
      Check ($u2 -like '*/ch2') "通道 2 的 GetStreamUri 指向 /ch2（实际 $u2）"
      # 两路共用一台 RTSP 服务器（按路径区分），所以 URL 里的端口**应当相同**。
      # 这一条防的是"某一路自己算了一个别人不听的端口"。
      #
      # ⚠️ `GetStreamUri` 报的是 **RTSP** 地址，里面**没有** HTTP 端口 —— 那个
      # 只在 WS-Discovery 的 XAddrs 里（见 [3]）。在这里断言 HTTP 端口，红的是
      # 写测试的人，不是产品。
      Check ($u2 -like "*:$RtspPort/*") `
            "通道 2 的 GetStreamUri 走的是共用的 RTSP 端口 $RtspPort（实际 $u2）"
      # 子码流的尺寸必须是**那一路自己的**。自动建出来的通道如果漏了子码流
      # 的尺寸推导，两个通道的子码流会一模一样。
      #
      # ⚠️ 比的是**尺寸对**，不是高度：两路主码流的比例不同（1920x1080 是 16:9，
      # 1620x1080 是 3:2），子码流高度都取到 360 是完全正常的，拿高度当判据
      # 会凭空红一条。
      $s1 = ($j1.profiles | Where-Object { $_.token -eq 'sub' } | Select-Object -First 1)
      $s2 = ($j2.profiles | Where-Object { $_.token -eq 'sub' } | Select-Object -First 1)
      Check ($s1 -and $s2 -and "$($s1.width)x$($s1.height)" -ne "$($s2.width)x$($s2.height)") `
            "两路的子码流尺寸不同（$($s1.width)x$($s1.height) vs $($s2.width)x$($s2.height)）"
  }

  # --- [3] WS-Discovery：一次 Probe → **一份** ProbeMatch，里面两台设备 -------
  #
  # 形状是刻意的（见 wsd.h）：只有一个 responder 挂在通道 1 上，一次回答里
  # 打包全部通道。**每个通道各起一个 responder 是错的** —— 那样 N 个通道回
  # N 份 ProbeMatch，每份里还都带全部通道，NVR 那边看到的是同一台设备被重复
  # 收录。
  "`n[3] WS-Discovery（从临时端口发 Probe，真实客户端的形状）"
  # **`--dump` 不能省。** 不带它的时候探针只把**它认出的第一个** XAddrs 打进
  # 人读的那几行，而"通道 2 在不在里面"这个问题只有看整段原始报文才答得出来
  # —— 第一次跑就是这么假红了一条。
  $w = Invoke-WithTimeout 'python' @('-I','-X','utf8',
          (Join-Path $root 'tests\wsd_probe.py'), '--mode', 'client',
          '--seconds', '8', '--dump') 30000
  $wAll = $w.Out + $w.Err
  Check ($w.ExitCode -eq 0) "wsd_probe.py --mode client 退出码 $($w.ExitCode)" `
        (($wAll -split "`r?`n" | Select-Object -Last 6) -join ' / ')
  # 一次 Probe 只回**一个 UDP 报文** —— 这是"只挂一个 responder"的直接后果。
  # N 个 responder 的话这里会是 N。
  Check ($wAll -match '共收到 1 个应答') `
        "一次 Probe 只回**一个报文**" `
        (($wAll -split "`r?`n" | Where-Object { $_ -match '共收到|来自' }) -join ' / ')
  # 而那一个报文里要有**两条** ProbeMatch（通道 1 一条、通道 2 一条）。这才是
  # 多通道在组播里的形状：一个报文、N 条记录，一次单播里齐活，丢包不会只丢
  # 其中一路。
  $pmCount = ([regex]::Matches($wAll, [regex]::Escape('<d:ProbeMatch>'))).Count
  Check ($pmCount -eq 2) `
        "那一个报文里有两条 ProbeMatch（实际 $pmCount 条）" `
        "只有一条 = 通道 2 没进宣告表，NVR 自动扫描只能扫到通道 1"
  Check ($wAll -like "*:$HttpPort/onvif/device_service*") `
        "宣告表里有通道 1 的地址（端口 $HttpPort）"
  Check ($wAll -like "*:$($Ch2Port)/onvif/device_service*") `
        "宣告表里也有通道 2 的地址（端口 $($Ch2Port)）" `
        "自动建出来的通道如果没进 XAddrs，NVR 就只能手工填 URL 添加它"

  # --- [4] 两路 RTSP 同时拉 -------------------------------------------------
  "`n[4] 两路 RTSP 同时拉"
  # **同时**拉才验得了"一台共享的 RtspServer、全部通道挂在上面靠路径区分"
  # 这件事 —— 串行拉只证明两条路径各自是对的。
  $pull1 = Start-Pull "rtsp://127.0.0.1:$RtspPort/live" 8
  $pull2 = Start-Pull "rtsp://127.0.0.1:$RtspPort/ch2" 8
  $r1 = Finish-Cmd $pull1 40000
  $r2 = Finish-Cmd $pull2 40000
  $v1 = Split-PullErr $r1.Err
  $v2 = Split-PullErr $r2.Err
  $f1 = Get-PullFrames $r1.Out
  $f2 = Get-PullFrames $r2.Out
  Check (-not $r1.TimedOut -and $r1.ExitCode -eq 0 -and $v1.Fatal.Count -eq 0) `
        "通道 1 /live 并发拉 8 秒零报错（exit=$($r1.ExitCode)，致命行 $($v1.Fatal.Count) 条，解出 $f1 帧）" `
        (($v1.Fatal | Select-Object -First 3) -join ' / ')
  Check (-not $r2.TimedOut -and $r2.ExitCode -eq 0 -and $v2.Fatal.Count -eq 0) `
        "通道 2 /ch2 并发拉 8 秒零报错（exit=$($r2.ExitCode)，致命行 $($v2.Fatal.Count) 条，解出 $f2 帧）" `
        (($v2.Fatal | Select-Object -First 3) -join ' / ')
  foreach ($n in (@($v1.Notes) + @($v2.Notes) | Select-Object -First 2)) { "         [记账] $n" }

  $p1 = Probe "rtsp://127.0.0.1:$RtspPort/live"
  $p2 = Probe "rtsp://127.0.0.1:$RtspPort/ch2"
  Check (-not $p1._timedout -and $p1.codec_name -eq 'h264') `
        "通道 1 /live 是 h264（实际 $($p1.codec_name)）" $p1._err
  Check (-not $p2._timedout -and $p2.codec_name -eq 'h264') `
        "通道 2 /ch2 是 h264（实际 $($p2.codec_name)）" $p2._err
  # 两路**必须不同尺寸**。相同就说明通道 2 其实没编，只是把通道 1 那条流又挂
  # 了一遍 —— 那样"多通道"这句话是假的，而表面上一切正常（两边都解得开）。
  Check ([int]$p1.width -ne [int]$p2.width) `
        "两路尺寸不同：$($p1.width)x$($p1.height) vs $($p2.width)x$($p2.height)" `
        "相同 = 通道 2 没有自己编，只是把通道 1 的流又挂了一遍"

  # `GetProfiles` 报的分辨率必须**等于**这条流真实的分辨率。
  #
  # 这两件事各有各的信息来源：profile 里那个数来自建 profile 时的**猜**
  # （那一刻采集还没开，连桌面/摄像头的原生尺寸都不知道），而这里是 ffprobe
  # 从码流里解出来的。NVR 拿 profile 里的数决定画面怎么摆、怎么缩，两个数
  # 对不上就是"通道 2 的画面被拉扁"，而两边谁都不报错。
  #
  # 通道 2 起那几路曾经**永远不会自我纠正**：真值由 `publish()` 每秒报一次，
  # 而那个 WebServer 只服务 SOAP、没有人订阅 `/ws/stats`。所以这一条是
  # `publish_video_size()` 存在的理由，也是它唯一的判据。
  foreach ($pair in @(@($j1, $p1, '通道 1'), @($j2, $p2, '通道 2'))) {
      $j = $pair[0]; $p = $pair[1]; $who = $pair[2]
      $m = $j.profiles | Where-Object { $_.token -eq 'main' } | Select-Object -First 1
      Check ("$($m.width)x$($m.height)" -eq "$($p.width)x$($p.height)") `
            "$who 的 GetProfiles 主码流尺寸与流真实尺寸一致（报 $($m.width)x$($m.height)，实际 $($p.width)x$($p.height)）" `
            "profile 里那个数建的时候只能猜，靠每秒的真值上报纠正；对不上 NVR 会按错的尺寸摆画面"
  }

  # --- [5] 每通道各录一份 ----------------------------------------------------
  #
  # 排在进程**自己到点退出**之后：MP4 的索引（moov）是正常收尾时才写的，
  # 砍掉的进程留下的是"moov atom not found"，ffprobe 只回一句 Invalid data
  # —— 看起来像封装器坏了，其实只是我把一个还在录的进程砍了。
  "`n[5] 每通道各录一份 MP4（等它自己收尾）"
  $txtA = Wait-App $appA
  $recA = @(
      @{ f = 'a1.mp4';     who = '通道 1（摄像头）'; h = [int]$p1.height },
      @{ f = 'a1-ch2.mp4'; who = '通道 2（显示器）'; h = [int]$p2.height }
  )
  foreach ($r in $recA) {
      $path = Join-Path $out $r.f
      $exists = (Test-Path $path) -and ((Get-Item $path).Length -gt 1000)
      Check $exists "$($r.who) 录到了 $($r.f)" "路径 $path 不存在或太小"
      if ($exists) {
          $pi = Probe $path
          Check (-not $pi._timedout -and $pi.codec_name -eq 'h264') `
                "$($r.f) 能解出 h264（实际 $($pi.codec_name)）" $pi._err
          # **文件里的高度必须和这一路 RTSP 报的一致。** 这是"每一路各录各的"
          # 的判据：录像文件名对了、内容其实是另一路的画面，是这一整块最可能
          # 的写错方式，而它只看文件名永远发现不了。
          Check ([int]$pi.height -eq $r.h) `
                "$($r.f) 的高度是 $($r.h)（实际 $($pi.height)）" `
                "和这一路 RTSP 报的尺寸对不上 = 两条通道的画面串了"
      }
  }

  # =========================================================================
  # 第二幕：通道 2 指到一台不存在的摄像头
  # =========================================================================
  "`n[*] 第二幕：通道 2 指到一台**不存在**的摄像头"
  $cfgB = Join-Path $out 'cfg-b.json'
  Write-Cfg $cfgB @"
[
  { "source": "webcam", "camera": "NO-SUCH-CAMERA-PHASE-AD", "quality": "verylow",
    "rtsp_path": "/ch2", "rtsp_sub_path": "/ch2_sub" }
]
"@
  $appB = Start-App 'b' $cfgB (Join-Path $out 'b1.mp4') 30
  $okB = Wait-Port $HttpPort 30
  Check $okB "服务照样起来了（没有因为一路坏掉就退出）"
  if ($okB) {
      Check (Wait-Port $RtspPort 20) "RTSP 端口照样起来了"
      Start-Sleep -Seconds 10

      "`n[6] 坏掉的通道 2 不影响通道 1"
      $null = Assert-Alive $appB '第二幕'
      $sessB = Login
      $chsB = Get-Channels $sessB
      $bB = By-Index $chsB
      Check ($chsB.Count -eq 2) "channels 还是两行（坏掉的那一路不消失，实际 $($chsB.Count)）" `
            ("收到：" + (($chsB | ForEach-Object { "$($_.index):$($_.state)" }) -join ' | '))
      Check ($bB[1].state -ne '未启动' -and [double]$bB[1].fps -gt 0) `
            "通道 1 还在跑（状态「$($bB[1].state)」，fps $($bB[1].fps)）"
      # 状态要**说出原因**。只写"失败"的话，用户得翻日志才知道是摄像头名字
      # 写错了、还是被别的程序占着。
      Check ("$($bB[2].state)" -like '失败*') `
            "通道 2 的状态是「失败…」（实际「$($bB[2].state)」）"
      Check ("$($bB[2].state)".Length -gt 2) `
            "而且带着原因，不是光一个「失败」（实际「$($bB[2].state)」）"
      Check ([double]$bB[2].fps -eq 0) "通道 2 的 fps 是 0（没在出帧）"

      $pb1 = Probe "rtsp://127.0.0.1:$RtspPort/live"
      Check (-not $pb1._timedout -and $pb1.codec_name -eq 'h264') `
            "通道 1 的 RTSP 照常能拉（实际 $($pb1.codec_name)）" $pb1._err

      "`n[7] 进程还活着，两个 ONVIF 端点都还答"
      Check (-not $appB.P.HasExited) "进程没有退出"
      $jB1 = Probe-Device $HttpPort
      Check ($null -ne $jB1 -and -not $jB1.err) "通道 1 的 ONVIF 端点照常答"
      # 坏的只是**通道 2 的采集**。它的 ONVIF 端点照样该起来 —— 端口 8101 是
      # WebServer 的事，和摄像头能不能打开无关。这一条把"哪些东西是独立的"
      # 钉住：哪天有人把"起端点"挪进采集成功之后的分支里，这里会当场变红。
      $jB2 = Probe-Device ($Ch2Port)
      Check ($null -ne $jB2 -and -not $jB2.err) `
            "通道 2 的 ONVIF 端点也照样答（采集坏了，端点没坏）" `
            "端点和采集是两件事，端点起不来应该只跟端口被占有关"
  }
  $txtB = Wait-App $appB
  # 日志里那句"通道 2 停了：…"是这一路**真的开不起来**的正面证据。没有它的
  # 话，"状态显示失败"也可能只是状态机自己写错了，而采集其实好好的。
  Check ($txtB -like '*通道 2 停了*') "日志里有「通道 2 停了：…」这一行"

  # =========================================================================
  # 第三幕：通道 2 显式写上通道 1 正在采的那块屏
  # =========================================================================
  "`n[*] 第三幕：通道 2 显式写上通道 1 正在采的那块屏（显示器 #0）"
  $cfgC = Join-Path $out 'cfg-c.json'
  Write-Cfg $cfgC @"
[
  { "source": "display", "display_index": 0, "quality": "verylow",
    "rtsp_path": "/ch2", "rtsp_sub_path": "/ch2_sub" }
]
"@
  $appC = Start-App 'c' $cfgC (Join-Path $out 'c1.mp4') 35
  $okC = Wait-Port $HttpPort 30
  # **这一行是整个第三幕的中心断言。** 修之前它是红的（run8.txt：通道 2 赢了
  # 那场 duplication 赛跑 → 通道 1 的 `DuplicateOutput` 失败 → 进程退出码 1），
  # 而同一份配置在 run7.txt 里是绿的 —— 所以它红的时候看着像"随机崩溃"。
  Check $okC "服务照样起来了（这一条是修掉的那个致命后果）"
  if (-not $okC) {
      # **说清楚是"没起来"还是"起来又死了"**：这两种在这一行断言上长得一样，
      # 而它们的成因、要查的地方完全不同。`Stop-App` 顺手把这一幕收掉 ——
      # 不收的话它会活到 `--seconds` 到点，把三条端口占住，让**下一次**跑
      # 在完全无关的地方失败。
      $txt = Stop-App $appC
      "  （端口没起来：进程 $(if ($appC.P.HasExited) { "已退出，ExitCode=$($appC.P.ExitCode)" } else { '还活着' })；"
      "    看 out\mc-e2e\c.err.log）"
      ($txt -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 15) |
          ForEach-Object { "    $_" }
  }
  if ($okC) {
      Start-Sleep -Seconds 10
      "`n[8] 被挡掉的通道 2 不影响通道 1"
      $null = Assert-Alive $appC '第三幕'
      $sessC = Login
      $chsC = Get-Channels $sessC
      $bC = By-Index $chsC
      # 被挡的那一路**照样占一行**：它凭空消失的话，"我配的那一路去哪了"
      # 就没有答案（和"用户自己关掉的"是同一条理由）。
      Check ($chsC.Count -eq 2) "channels 还是两行（实际 $($chsC.Count)）"
      Check ([double]$bC[1].fps -gt 0) "通道 1 还在跑（fps $($bC[1].fps)）"
      Check ("$($bC[2].state)" -like '未启用*') `
            "通道 2 的状态是「未启用…」（实际「$($bC[2].state)」）"
      # **这一条钉的是"原因里点名是哪一块屏"。** 只写一句"未启用"的话，用户
      # 会去配置里找一个明明是开着的开关 —— 那正是 `blocked_reason` 和
      # "用户自己关的"要分开的理由。
      Check ("$($bC[2].state)" -like '*显示器 #0*') `
            "原因里点名是显示器 #0（实际「$($bC[2].state)」）" `
            "不点名的话，用户只知道建不了，不知道是跟哪一块屏撞了"
      # 状态里**不该**再出现 DuplicateOutput：这条规则就是在 DXGI 之前拦下来的，
      # 换句话说它压根不该有机会去试。真跑到了那一步，说明拦住的是别的东西 ——
      # 而那个东西有大约一半的概率会把通道 1 一起带走。
      Check ("$($bC[2].state)" -notlike '*DuplicateOutput*') `
            "而且根本没试到 DuplicateOutput 那一步（实际「$($bC[2].state)」）"
      # 被挡的那一路**没有 ONVIF 端点**：一个出不了画面的端口在 NVR 里就是一台
      # 连不上的设备（和"关掉的通道不宣告"是同一条理由，见 main.cpp 那句）。
      Check (Wait-PortFree @($Ch2Port) 8) `
            "被挡的那一路连端口都没占（$Ch2Port 是空的）"
      $pc1 = Probe "rtsp://127.0.0.1:$RtspPort/live"
      Check (-not $pc1._timedout -and $pc1.codec_name -eq 'h264') `
            "通道 1 的 RTSP 照常能拉（实际 $($pc1.codec_name)）" $pc1._err
  }
  $txtC = Wait-App $appC
  # 规划期那句话。**它必须在启动的头几秒就出现**（规划是在起服务之前做的、
  # 也是在起服务之前就 WARN 的），所以这里同时钉了"说了"和"别等采集中途才说"。
  Check ($txtC -like '*通道 2 建不了*') `
        "规划期就说了「通道 2 建不了」" `
        "没有这句的话，用户只能从通道状态那一栏去猜"
  Check ($txtC -like '*正被通道 1 使用*') `
        "而且说清了是因为通道 1 正在用那块屏" `
        "只说'建不了'而不说跟谁撞了，用户没法动手改"

  "`n" + ('=' * 60)
  if ($failed -eq 0) { "全部通过。日志在 out\mc-e2e\" } else { "失败 $failed 项。" }
  exit $failed
}
finally {
  # 收尸：还活着的采集进程一个都不留。它们占着 8099 / 8101 / 8554，而
  # "下一次跑的时候端口被占"报出来的是**下一次**那个脚本起不来 —— 离真正
  # 的原因隔着一整个进程的生命周期。
  foreach ($a in @($script:apps)) {
      if ($a -and $a.P -and -not $a.P.HasExited) {
          try { $a.P.Kill() } catch {}
          try { $a.P.WaitForExit(10000) | Out-Null } catch {}
      }
  }
  # 指针必须无条件放回去：这个脚本期间它在替我们"制造桌面变化"，中途抛异常
  # 时把人家的指针留在屏幕中间，是比测试失败严重得多的后果。
  if ($jig) {
      try { Stop-Job $jig -ErrorAction SilentlyContinue } catch {}
      try { Remove-Job $jig -Force -ErrorAction SilentlyContinue } catch {}
  }
  try { [void][D2O.Cursor]::SetCursorPos($saved.X, $saved.Y) } catch {}
}
