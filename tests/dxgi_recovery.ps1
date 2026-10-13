#Requires -Version 5.1
<#
.SYNOPSIS
    DXGI 采集会话自恢复：录制途中**真的**把显示模式改掉，看采集会话能不能自己
    重建、画面能不能接着录。

.DESCRIPTION
    为什么要真的改显示模式，而不是在代码里伪造一次 ACCESS_LOST：伪造出来测的是
    "我们的重试循环对不对"，而这条链路上真正不知道的是**DXGI 到底什么时候会
    失效、失效之后重建要多久、几何变了会怎么样**。这三个问题只有真事件能回答。

    三条触发路径各对应一个真实的用户场景：

      refresh   同分辨率换刷新率 —— 会话失效但几何不变。覆盖"锁屏、显示器休眠、
                驱动复位、UAC 安全桌面"这一整类：它们的共同点就是画面尺寸不变。
      resize    换分辨率 —— 几何也变了。这是**录制中途改分辨率**那条路，也是最
                常被用户碰到的一条（插拔外接显示器、改显示设置）。
      monitor   关显示器电源 —— 量它到底会不会让会话失效。**不预设结论**：桌面
                的存在和显示器的开关是两件事，猜错方向会白查半天。
                **这台机器上它是被动过手脚的一条**：实测关电源会让**整个系统**
                挂起（一次 586 秒，PowerShell 自己也停着），于是所有按相对秒排
                的时刻全部作废。它的价值因此变成"顺带验了系统从挂起里醒来之后
                采集能不能自己接上"，而不是"准时切换 X 秒后的行为"—— 而那一趟
                真的验到了：重建先拿到两次 `E_ACCESSDENIED`（桌面不可访问，
                也就是锁屏/安全桌面那一类）然后成功，落盘 200 秒、退出码 0。

    每一段触发都是"改成 X → 静默 SettleSec 秒 → 改回原样"，而**改回去本身也
    是一次 ACCESS_LOST**。所以每段实际验的是连续两次重建。

    **refresh 和 resize 的判据不一样**，因为产品的行为不一样：尺寸没变时程序
    原地重建捕获会话、录到同一个文件里；尺寸变了则整条链重开（编码器的
    SPS/PPS、MP4 轨道头、SDP 的 sprop-parameter-sets 都绑死在旧尺寸上，就地
    换是解不出来的），于是文件分成 main.mp4 + main.1.mp4 + …、RTSP 服务重开。
    拿同一组断言去卡两种行为，多出来的那几条就是假失败。

    触发时机按"RTSP 端口起来之后 $T0 的相对秒"算，不用绝对时刻 —— 起服务本身
    要几秒，用绝对时刻的话机器慢一点触发就落在采集开始之前，测出来的是"没有
    任何重建"，而被冤枉的是程序。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\dxgi_recovery.ps1
    powershell -ExecutionPolicy Bypass -File tests\dxgi_recovery.ps1 -Stages refresh
#>
[CmdletBinding()]
param(
    # 要触发哪几路。默认两路串在一次采集里，顺带验"连续多次重建"——第一次能
    # 恢复不代表第五次还能。monitor 默认不开：它和另外两路验的是同一件事，
    # 而关屏期间这台机器的桌面是对用户不可用的。
    [string[]] $Stages = @('refresh', 'resize'),
    # 采集总时长。必须覆盖"起服务 + 所有触发 + 最后那次拉流"，
    # 给短了的表现是采集自己到点退出，后面几段报的是"连不上"。
    #
    # **monitor 那一路要给得特别大（200 起步）。** 这台机器上关显示器电源会让
    # 整个系统挂起（实测 586 秒），而 `--seconds` 数的是采集的拍数不是墙钟 ——
    # 挂起期间一拍都没产出，于是程序的墙钟总时长 = 挂起时长 + Seconds。给 30
    # 的结果是它在你排到的下一步之前就跑完退出了。
    [int] $Seconds = 40,
    [int] $RtspPort = 8557,
    # 触发之后等多久再改回原样。切换分辨率之后 DXGI 重建多半会先失败一两次
    # （驱动还在重配），这个静默期就是留给它的。
    [int] $SettleSec = 6,
    # 全部触发做完之后，从 RTSP 拉一段，验"重建过 N 次之后，新客户端还能不能
    # 拿到一条干净的流"。这一条才是这台产品意义上的"恢复了没有"。
    [int] $PullSec = 5
)
$ErrorActionPreference = 'Stop'

# 子进程输出一律按 UTF-8 解，**不能靠控制台的代码页**（默认 936 会把程序的
# UTF-8 字节解成乱码再写进日志，变成双重编码，而日志本身看着完全正常）。
$utf8 = New-Object Text.UTF8Encoding $false
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root 'out\dxgi-recovery'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe     = Join-Path $root 'build\bin\desk2onvif.exe'
$ffprobe = Join-Path $root 'third_party\ffmpeg\bin\ffprobe.exe'
$ffmpeg  = Join-Path $root 'third_party\ffmpeg\bin\ffmpeg.exe'

$failed = 0
function Check([bool]$ok, [string]$what, [string]$detail = '') {
    if ($ok) { Write-Host "  [OK]   $what" -ForegroundColor Green }
    else {
        Write-Host "  [FAIL] $what" -ForegroundColor Red
        if ($detail) { Write-Host "         $detail" -ForegroundColor DarkGray }
        $script:failed++
    }
}
function Section([string]$t) { Write-Host ''; Write-Host $t -ForegroundColor Cyan }
function Note([string]$t) { Write-Host "  $t" -ForegroundColor DarkGray }

# 解码判据的两把尺子（全文在 `tests\rtsp_sub_e2e.ps1` 的 Check 下面）。
# 一、`-f null` 那条 `non monotonically increasing dts` 是 **muxer 自己的账**，
#     不是这一路流的问题，放它过去；其余任何一个字都算致命。
# 二、帧数是个正数信号：解出多少帧，比"没报错"更难糊弄过去。
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

# ---------------------------------------------------------------------------
# 显示模式：读、改、还原；以及显示器电源
# ---------------------------------------------------------------------------
#
# 结构体布局必须**逐字节**对上 DEVMODEW，尤其是两个 ByValTStr：CCHDEVICENAME /
# CCHFORMNAME 都是 32 个 **WCHAR**（= 64 字节）。写成 32 字节会让结构短 64 字节，
# 而 Windows 是按 dmSize 决定往里填多少的 —— 填错的表现是所有字段读成 0，
# 看着像"这台机器没有这个模式"。所以下面有一条 sizeof 断言（应为 220）。
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
public struct DEVMODE {
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmDeviceName;
  public short dmSpecVersion; public short dmDriverVersion;
  public short dmSize; public short dmDriverExtra;
  public int dmFields;
  public int dmPositionX; public int dmPositionY;
  public int dmDisplayOrientation; public int dmDisplayFixedOutput;
  public short dmColor; public short dmDuplex; public short dmYResolution;
  public short dmTTOption; public short dmCollate;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmFormName;
  public short dmLogPixels;
  public int dmBitsPerPel; public int dmPelsWidth; public int dmPelsHeight;
  public int dmDisplayFlags; public int dmDisplayFrequency;
  public int dmICMMethod; public int dmICMIntent; public int dmMediaType;
  public int dmDitherType; public int dmReserved1; public int dmReserved2;
  public int dmPanningWidth; public int dmPanningHeight;
}
public static class D2oDisp {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern int ChangeDisplaySettingsEx(string dev, ref DEVMODE dm, IntPtr hwnd, int flags, IntPtr param);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);
  /// 关/开显示器电源。HWND_BROADCAST=0xFFFF, WM_SYSCOMMAND=0x0112,
  /// SC_MONITORPOWER=0xF170, 2=关 / -1=开。**一个像素都不改**，只是一条广播。
  public static void MonitorPower(bool on) {
    SendMessage((IntPtr)0xFFFF, 0x0112, (IntPtr)0xF170, (IntPtr)(on ? -1 : 2));
  }
}
'@
$DevModeSize = [Runtime.InteropServices.Marshal]::SizeOf([type][DEVMODE])

function New-DevMode {
    $dm = New-Object DEVMODE
    $dm.dmSize = $script:DevModeSize
    return $dm
}

# 当前模式。`-1` = ENUM_CURRENT_SETTINGS。
function Get-CurrentMode([string]$Device) {
    $dm = New-DevMode
    if (-not [D2oDisp]::EnumDisplaySettings($Device, -1, [ref]$dm)) { return $null }
    return $dm
}

# 按 (宽,高,刷新率) 找一个**枚举出来的** DEVMODE。
#
# 关键：改模式必须用 EnumDisplaySettings 拿到的那个 DEVMODE，不能自己拼一个只填
# 宽高刷新的 —— 别的字段（dmFields 的位、dmDisplayFlags 等）是有含义的，自己拼
# 出来的会被判 BADMODE，而报出来只是一个整数 -2。
function Find-Mode([string]$Device, [int]$W, [int]$H, [int]$Hz) {
    for ($i = 0; $i -lt 400; $i++) {
        $dm = New-DevMode
        if (-not [D2oDisp]::EnumDisplaySettings($Device, $i, [ref]$dm)) { break }
        if ($dm.dmPelsWidth -eq $W -and $dm.dmPelsHeight -eq $H -and
            ($Hz -le 0 -or $dm.dmDisplayFrequency -eq $Hz)) {
            return $dm
        }
    }
    return $null
}

# 应用一个模式。返回 DISP_CHANGE_* 的整数结果（0 = 成功）。**不抛异常** ——
# 调用方要把它当成一条断言，而不是让整个脚本断在半路上。
function Set-Mode([string]$Device, $Mode) {
    if (-not $Mode) { return -99 }
    return [D2oDisp]::ChangeDisplaySettingsEx($Device, [ref]$Mode, [IntPtr]::Zero, 0, [IntPtr]::Zero)
}

# 当前模式**真的**等于 $Mode 吗（按宽、高、刷新率比）。
#
# 判"改模式成功没有"要看它，不能只看 ChangeDisplaySettingsEx 的返回值：系统刚从
# 挂起里醒来时它会瞬时返回 DISP_CHANGE_FAILED（实测），而模式根本没变 —— 只看
# 返回值就是一条假失败。
function Test-Mode([string]$Device, $Mode) {
    if (-not $Mode) { return $true }
    $cur = Get-CurrentMode $Device
    if (-not $cur) { return $false }
    return ($cur.dmPelsWidth -eq $Mode.dmPelsWidth -and
            $cur.dmPelsHeight -eq $Mode.dmPelsHeight -and
            $cur.dmDisplayFrequency -eq $Mode.dmDisplayFrequency)
}

$DispResultName = @{
    0 = 'DISP_CHANGE_SUCCESSFUL'; 1 = 'DISP_CHANGE_RESTART'
    -1 = 'DISP_CHANGE_FAILED'; -2 = 'DISP_CHANGE_BADMODE'
    -3 = 'DISP_CHANGE_NOTUPDATED'; -4 = 'DISP_CHANGE_BADFLAGS'
    -5 = 'DISP_CHANGE_BADPARAM'; -6 = 'DISP_CHANGE_BADDUALVIEW'
    -99 = '没有这个模式'
}
function DispResultText([int]$r) {
    if ($DispResultName.ContainsKey($r)) { return "$($DispResultName[$r])(=$r)" }
    return "未知($r)"
}

# 硬超时的外部命令。没有这一层，一次 ffprobe 挂死就会吃掉整个测试。
function Invoke-WithTimeout {
    param([string]$File, [string[]]$Argv, [int]$TimeoutMs)
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
    $so = $p.StandardOutput.ReadToEndAsync()
    $se = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutMs)) {
        try { $p.Kill() } catch {}
        return [pscustomobject]@{ Out = ''; Err = "超时 ${TimeoutMs}ms"; TimedOut = $true }
    }
    return [pscustomobject]@{ Out = $so.Result; Err = $se.Result; TimedOut = $false }
}

# ---------------------------------------------------------------------------
# [0] 前置
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== DXGI 采集会话自恢复 ===' -ForegroundColor Cyan
Write-Host ("  触发      " + ($Stages -join ', '))
Write-Host ("  采集      $Seconds 秒，RTSP $RtspPort")

Section '[0] 前置'
Check ($DevModeSize -eq 220) "DEVMODEW 布局对得上（$DevModeSize 字节，应为 220）" `
      '结构体错了的话宽高全读成 0，看着像这台机器没有这些模式。'

$display = $null
$orig = $null
foreach ($i in 1..8) {
    $dev = "\\.\DISPLAY$i"
    $m = Get-CurrentMode $dev
    if ($m -and $m.dmPelsWidth -gt 0) { $display = $dev; $orig = $m; break }
}
if (-not $display) { throw '找不到任何显示输出' }
$origSize = "$($orig.dmPelsWidth)x$($orig.dmPelsHeight)"
$origHz   = $orig.dmDisplayFrequency
Note "输出 $display  $origSize @ ${origHz}Hz"

# 触发要用的模式**必须先在枚举里确认存在**。少了这一条，一台碰巧没有 60Hz 的
# 机器会让 DISP_CHANGE_BADMODE 静默失败 —— 而"模式没换成"和"程序没能自恢复"
# 在断言里长得一模一样，被冤枉的会是程序。
$otherHz = if ($origHz -ne 60) { 60 } else { 50 }
$sameSizeMode = Find-Mode $display $orig.dmPelsWidth $orig.dmPelsHeight $otherHz
# 换分辨率挑一个**比当前小**的：切换更快，对桌面的扰动也小。
$resizeW = 2560; $resizeH = 1600
$resizeMode = Find-Mode $display $resizeW $resizeH 60
foreach ($s in $Stages) {
    if ($s -eq 'refresh') {
        Check ($null -ne $sameSizeMode) "这台机器有 $origSize @ ${otherHz}Hz 可用来触发"
    } elseif ($s -eq 'resize') {
        Check ($null -ne $resizeMode) "这台机器有 ${resizeW}x${resizeH} 可用来触发"
    } elseif ($s -ne 'monitor') {
        throw "不认识的触发：$s（只有 refresh / resize / monitor）"
    }
}

Check (@(Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue).Count -eq 0) `
      '没有正在运行的 desk2onvif'

# 这几个工具在**改任何东西之前**就要确认在。放到用的时候再发现"文件不存在"，
# 异常会跳过所有还原逻辑，把用户的桌面留在别的分辨率上。
foreach ($tool in @($exe, $ffprobe, $ffmpeg)) {
    Check (Test-Path -LiteralPath $tool) "有 $(Split-Path -Leaf $tool)"
}

# 还原显示模式。**注册成 finally，不是执行到最后一行** —— 中途任何一条断言
# 抛出来，用户的屏幕都必须回到原样。改回去这件事本身也要断言：
# 一个静默失败的还原会把用户的桌面留在别的分辨率上，而脚本已经报"全部通过"。
$script:restored = $false
function Restore-Display {
    if ($script:restored) { return }
    $script:restored = $true
    # 连显示输出都还没找到就中止的话（[0] 里那几条 throw），没有东西可还原。
    if (-not $script:display -or -not $script:orig) { return }
    # 判据是**读回来的实际模式**，不是这一次调用的返回值。系统刚从挂起里醒来时
    # ChangeDisplaySettingsEx 会瞬时返回 DISP_CHANGE_FAILED，而模式其实一点没变
    # （monitor 那一路压根不改模式）—— 只认返回值就是一条假失败，而这条假失败
    # 会让人去手动"修"一个本来就对的设置。返回非 0 就再试几次，最后按实际模式判。
    [void](Set-Mode $script:display $script:orig)
    for ($i = 0; $i -lt 5 -and -not (Test-Mode $script:display $script:orig); $i++) {
        Start-Sleep -Seconds 2
        [void](Set-Mode $script:display $script:orig)
    }
    if (Test-Mode $script:display $script:orig) {
        Note "显示模式已还原为 $script:origSize @ ${script:origHz}Hz"
    } else {
        $cur = Get-CurrentMode $script:display
        $now = if ($cur) { "$($cur.dmPelsWidth)x$($cur.dmPelsHeight)@$($cur.dmDisplayFrequency)Hz" } else { '读不出来' }
        Write-Host ("  [FAIL] 还原显示模式失败：现在是 $now" +
                    "，应为 $script:origSize @ ${script:origHz}Hz —— 请手动调回去") `
                   -ForegroundColor Red
        $script:failed++
    }
}

# 兜底：脚本以**任何**方式异常中止，屏幕都要回到原样。
#
# 用 trap 而不是把两百行正文包进 try/finally，是为了不多加一层缩进 ——
# 而那一层缩进恰好会让人以为"这些代码嵌套在某个事务里"。
trap {
    Write-Host ''
    Write-Host "未预料的错误：$_" -ForegroundColor Red
    Restore-Display
    Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue | ForEach-Object {
        try { $_.Kill(); $null = $_.WaitForExit(15000) } catch {}
    }
    exit 1
}

# ---------------------------------------------------------------------------
# [1] 起采集
# ---------------------------------------------------------------------------
Section '[1] 起采集（RTSP + 录制）'
foreach ($f in 'run.log', 'run.err', 'main.mp4', 'cfg.json') {
    Remove-Item (Join-Path $out $f) -ErrorAction SilentlyContinue
}
# 上一趟留下的分段文件必须一起清掉。它们不删的话，这一趟只录出 2 段而磁盘上
# 有 3 个 main*.mp4，段数断言读到的是上一次的残骸 —— 而且它**永远是偏多的**，
# 也就是"少录了一段"这种真缺陷会被残骸盖住。（按名字逐个删，不用通配符。）
Get-ChildItem -LiteralPath $out -Filter 'main*.mp4' -ErrorAction SilentlyContinue |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -ErrorAction SilentlyContinue }

$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $exe
# WorkingDirectory 显式给：程序的静态目录默认值是相对路径，不设的话从别的目录
# 跑就会解析到别处（详见 rtsp_sub_e2e.ps1 里那段说明）。这个脚本不开 HTTP，
# 但保持和其它 e2e 一样的工作目录约定。
$psi.WorkingDirectory = $root
$psi.Arguments = ('--rtsp --rtsp-port {0} --record "{1}" --seconds {2} --config "{3}"') -f `
                 $RtspPort, (Join-Path $out 'main.mp4'), $Seconds, (Join-Path $out 'cfg.json')
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = $utf8
$psi.StandardErrorEncoding = $utf8
$app = New-Object Diagnostics.Process
$app.StartInfo = $psi
if (-not $app.Start()) { throw "起不来：$exe" }
$appOut = $app.StandardOutput.ReadToEndAsync()
$appErr = $app.StandardError.ReadToEndAsync()

$t0 = Get-Date
$deadline = $t0.AddSeconds(20)
$up = $false
while ((Get-Date) -lt $deadline -and -not $app.HasExited) {
    $c = New-Object Net.Sockets.TcpClient
    try { $c.Connect('127.0.0.1', $RtspPort); $c.Close(); $up = $true; break }
    catch { Start-Sleep -Milliseconds 300 }
}
Check $up "RTSP 端口 $RtspPort 起来了"
if (-not $up) {
    try { $app.Kill() } catch {}
    Restore-Display
    Write-Host ''
    Write-Host '采集没起来，后面的检查没有意义。' -ForegroundColor Red
    exit 1
}

# 等到"相对 t0 的第 OffsetSec 秒"。
#
# **超时要报出来。** 所有触发时刻都是按 t0 的相对秒算的，而这台机器上"关显示器
# 电源"会把整个系统挂起 —— 实测两次：计划第 12 秒那一步分别等到了第 126 秒和
# 第 598 秒，中间那些时间里连这个 PowerShell 进程自己都停着。挂起之后每一步都
# 落在计划之外，而"晚到"和"准时"打出来的那一行长得一模一样
# （`[125.9s] monitor 开电源 生效`），不报的话读日志的人会把它当成一次准时切换。
# 晚到的那些时刻不能按计划读。
function Wait-Until([int]$OffsetSec) {
    $target = $script:t0.AddSeconds($OffsetSec)
    while ((Get-Date) -lt $target) { Start-Sleep -Milliseconds 200 }
    $late = ((Get-Date) - $target).TotalSeconds
    if ($late -gt 2) {
        $script:suspended = $true
        # 括号要显式包住 `+` 那一段：`-f` 比 `+` **结合得更紧**，写成
        # `'a' + 'b' -f $x` 会解析成 `'a' + ('b' -f $x)` —— 前半段的占位符
        # 原样打出来，看着像没格式化，其实是被 `+` 拼在了外面。
        Note ((('计划第 {0}s 那一步实际等到第 {1:n0}s（晚了 {2:n0}s）—— 这期间整个系统' +
                '被挂起了，从这一步起的时刻都不在计划上，别当准时的那次读。') -f `
                $OffsetSec, ((Get-Date) - $script:t0).TotalSeconds, $late))
    }
}

# ---------------------------------------------------------------------------
# [2] 触发
# ---------------------------------------------------------------------------
Section '[2] 触发'
$base = 6
$plan = @()
foreach ($s in $Stages) {
    switch ($s) {
        'refresh' {
            $plan += [pscustomobject]@{ Name = "refresh -> ${otherHz}Hz"; At = $base; Mode = $sameSizeMode }
            $plan += [pscustomobject]@{ Name = "refresh -> ${origHz}Hz"; At = $base + $SettleSec; Mode = $orig }
        }
        'resize' {
            $plan += [pscustomobject]@{ Name = "resize -> ${resizeW}x${resizeH}"; At = $base; Mode = $resizeMode }
            $plan += [pscustomobject]@{ Name = 'resize -> 原尺寸'; At = $base + $SettleSec; Mode = $orig }
        }
        'monitor' {
            # 两条都 Mode = $null：这一路**根本不动显示模式**，只广播电源。
            # 开电源那条原本写的是 $orig，于是它醒来之后会去 Set-Mode 一次 ——
            # 而系统刚挂起过、显示栈还没缓过来，这一次会返回 DISP_CHANGE_FAILED，
            # 报出来是一条"切换失败"的 FAIL。**假失败比不测更坏**：它教人忽略
            # 这一节，而这一节正是唯一在测"系统挂起之后还能不能接上"的地方。
            $plan += [pscustomobject]@{ Name = 'monitor 关电源'; At = $base; Mode = $null }
            $plan += [pscustomobject]@{ Name = 'monitor 开电源'; At = $base + $SettleSec; Mode = $null }
        }
    }
    $base += $SettleSec + 4
}
# 最后一次模式变更的时刻。后面那次拉流要等在这个之后 —— 拿循环结束时的 $base
# 去推会算错，因为它已经**越过了**最后一段。
$lastAt = ($plan | Measure-Object -Property At -Maximum).Maximum

# 这一路会不会让**整条链**重开（而不是原地重建捕获会话）。
#
# 这不是"跑完再看结果"能补上的区别：resize 之后编码器的 SPS/PPS、MP4 的轨道头、
# SDP 里的 sprop-parameter-sets 全都绑死在旧尺寸上，程序那边**必须**另起一段，
# 于是文件是 main.mp4 + main.1.mp4、RTSP 服务整台重开。判定方式不一样，
# 断言自然不能是同一组 —— 拿"只该有一段"去卡 resize 是假失败，反过来拿
# "该有两段"去卡 refresh 会把一个真缺陷（尺寸没变却误判成分段）判成通过。
$restartsSession = $Stages -contains 'resize'

# 触发完之后等多久再去拉流。resize 那一路的最后一次模式变更会让整条链重开
# （新的 MP4 文件、新的 RTSP 服务），端口从关到重新 bind 需要一点时间 ——
# 等得短了会拉到一台正在换手的服务上，报出来像是"重建之后服务没了"。
$afterTriggerWait = if ($restartsSession) { 8 } else { 2 }

# 触发过程中**整个系统**被挂起过（见 Wait-Until）。挂起会让所有按相对秒排的
# 时刻失效，而且会把 `--seconds` 的预算一起吃进去 —— 于是"程序已经跑完退出"
# 和"程序恢复失败退出"在时间线上长得一样。这两种必须分开，否则这一路要么报
# 一堆假失败，要么把真失败混进去。
$script:suspended = $false

foreach ($step in $plan) {
    Wait-Until $step.At
    if ($null -eq $step.Mode) {
        [D2oDisp]::MonitorPower($step.Name -match '开电源')
        Note "[$((Get-Date) - $t0 | ForEach-Object { '{0,5:n1}s' -f $_.TotalSeconds })] $($step.Name)：已广播（没有返回值可断言）"
        continue
    }
    $r = Set-Mode $display $step.Mode
    $cur = Get-CurrentMode $display
    $now = "$($cur.dmPelsWidth)x$($cur.dmPelsHeight)@$($cur.dmDisplayFrequency)"
    # 判"切换生效没有"看的是**读回来的模式**，不是返回值。切换本身成功而没落到
    # 实际模式上（比如另一台显示器把坐标抢走了）不算生效；反过来系统刚醒来时
    # 返回值是 FAILED 而模式其实已经对了，也不算失败。返回码留在 detail 里当线索。
    Check (Test-Mode $display $step.Mode) `
          "[$('{0,5:n1}' -f ((Get-Date) - $t0).TotalSeconds)s] $($step.Name) 生效（现在 $now）" `
          "调用返回 $(DispResultText $r)"
}

# 触发做完之后程序**必须还活着**。这一条是整段的中心：采集会话重建失败时，
# 程序可能已经退出了 —— 而"退出"和"还活着但录不到东西"在后面的文件检查里
# 长得几乎一样，只有这里能分开。
$aliveAfter = -not $app.HasExited
if ($script:suspended -and -not $aliveAfter) {
    # 挂起把 --seconds 的时间预算一起吃掉了：程序是**正常跑完**退出的（[4] 的
    # 退出码、[5] 的时长都还在判），不是"恢复失败退出了"。前提不成立的时候
    # 硬判就是假失败 —— 但也不能当成通过，所以这里明说它被跳过了，改由
    # [4][5] 去判。
    Note '（触发期间系统被挂起，程序已经跑完自己的时长 —— 这一条不适用，改由 [4][5] 判）'
} else {
    Check $aliveAfter '全部触发之后进程还在跑' '采集会话没能恢复，程序已经退出了。'
}

# ---------------------------------------------------------------------------
# [3] 重建过 N 次之后，RTSP 还能不能伺候新客户端
# ---------------------------------------------------------------------------
#
# 这一条才是产品意义上的"恢复了没有"。上面的日志只能证明"重建函数被调过"，
# 证明不了"服务还在、还能出画"。
Section '[3] 重建之后新客户端拉流'
if (-not $aliveAfter) {
    # 进程已经不在了，没有服务可拉。**不静默跳过**：上面那条已经判过"该活着
    # 却退出了"（挂起那一趟除外），这里只说清楚这一段为什么没跑，免得读日志
    # 的人以为这一段验过了。
    Note '（程序已经退出，没有服务可拉 —— 这一段跳过）'
} else {
    Wait-Until ($lastAt + $afterTriggerWait)
    $url = "rtsp://127.0.0.1:$RtspPort/live"
    $probe = Invoke-WithTimeout $ffprobe @(
        '-v', 'error', '-rtsp_transport', 'tcp', '-select_streams', 'v:0',
        '-show_entries', 'stream=codec_name,width,height',
        '-of', 'default=noprint_wrappers=1', '-i', $url) 15000
    $kv = @{}
    foreach ($line in ($probe.Out -split "`r?`n")) {
        if ($line -match '^(\w+)=(.*)$') { $kv[$Matches[1]] = $Matches[2] }
    }
    Check (-not $probe.TimedOut) '重建之后能拿到 SDP'
    Check ($kv['codec_name'] -eq 'h264') "重建之后 codec = $($kv['codec_name'])"
    Check ("$($kv['width'])x$($kv['height'])" -eq $origSize) `
          "重建之后报的还是原尺寸 $($kv['width'])x$($kv['height'])（应为 $origSize）" `
          $(if ($restartsSession) {
              "最后一次模式变更已经切回 $origSize 了，新起的这一段就该报这个尺寸。" +
              "报成 ${resizeW}x${resizeH} 说明整条链还没重开完（等的时间不够，或者重开卡住了）。"
            } else {
              '尺寸没变，重建之后当然还是原尺寸 —— 变了说明中转纹理那条路算错了。'
            })
    # `-fps_mode passthrough`：理由见 `tests\rtsp_sub_e2e.ps1` 的 Start-Pull
    # 上面那段（默认 vsync 会把 throttle 模式下属正常变动的帧率折算成恒定
    # 帧率，折算时偶发重复时间戳，报的是折算器自己的损失）。这一条同样是
    # "解 N 秒零报错"，所以同样要关掉折算。
    $play = Invoke-WithTimeout $ffmpeg @(
        '-v', 'error', '-rtsp_transport', 'tcp', '-i', $url, '-t', "$PullSec",
        '-fps_mode', 'passthrough', '-progress', 'pipe:1', '-f', 'null', '-') `
        (($PullSec + 20) * 1000)
    $v = Split-PullErr $play.Err
    $fr = Get-PullFrames $play.Out
    Check (-not $play.TimedOut -and $v.Fatal.Count -eq 0) `
          "重建之后解 $PullSec 秒零报错（致命行 $($v.Fatal.Count) 条）" `
          (($v.Fatal | Select-Object -First 3) -join ' / ')
    # 帧数这条是 2026-10-09 加的：实测同一条 5 秒的流，`-fps_mode passthrough`
    # 解出 126 帧而默认 vsync 只解出 66 帧 —— "没报错"这句话本身量不出这个。
    # 下界取"平均至少 1 fps"：控制器最慢也只降到 `adaptive.min_fps`（默认 5），
    # 就算重开之后头两秒还在等关键帧，5 秒的窗口也远不止 5 帧。
    Check ($fr -ge $PullSec) "重建之后 $PullSec 秒解出 $fr 帧（下界平均 1 fps）"
    foreach ($n in ($v.Notes | Select-Object -First 2)) { Note "[记账] $n" }
}

# ---------------------------------------------------------------------------
# [4] 收尾与断言
# ---------------------------------------------------------------------------
Restore-Display

Section '[4] 采集统计'
# 等待窗口额外给 monitor 一路留出挂起的余量：程序的总墙钟 = 挂起时长 + Seconds，
# 而挂起时长本身不可控（实测 74s 和 118s 两次）。窗口给紧了会把"还在正常跑"
# 判成"到点没退"，而这一类假失败最难查 —— 它指向的是程序的退出逻辑。
$exitWaitSec = $Seconds + 40
if ($Stages -contains 'monitor') { $exitWaitSec += 300 }
if (-not $app.WaitForExit($exitWaitSec * 1000)) {
    try { $app.Kill() } catch {}
    Check $false '采集在自己的时长内结束'
} else {
    Check ($app.ExitCode -eq 0) "退出码 = $($app.ExitCode)" `
          '非零说明采集是**异常结束**的（重建失败会走这条路），而不是跑完到点。'
}
[IO.File]::WriteAllText((Join-Path $out 'run.log'), $appOut.Result)
[IO.File]::WriteAllText((Join-Path $out 'run.err'), $appErr.Result)

# **两个文件都要读。** 程序按级别分流：逐秒日志走 stderr，收尾汇总块走 stdout。
# 只读一个的话这一段会安静地什么都不匹配 —— 而"零条命中"和"日志里本来就没有
# 这些行"长得一模一样，等于这段检查自己废掉了却没人知道。
$txt = @(
    [IO.File]::ReadAllText((Join-Path $out 'run.err'), [Text.Encoding]::UTF8)
    [IO.File]::ReadAllText((Join-Path $out 'run.log'), [Text.Encoding]::UTF8)
) -join "`n"

$rebuilds = @([regex]::Matches($txt, '捕获会话已重建'))
# 收尾统计块**每段一份**（resize 那一路有三段就有三份），所以这里取所有匹配的
# 最大值而不是第一份 —— 只取第一份的话，resize 那一路永远看到的是第 1 段的
# 那个数，后面几段"一次都没重建"就永远验不到。
$summaries = @([regex]::Matches($txt, '会话重建\s*(\d+)\s*次'))
$summaryMax = 0
foreach ($mm in $summaries) { $summaryMax = [math]::Max($summaryMax, [int]$mm.Groups[1].Value) }
$expectMin = $Stages.Count      # 每路至少一次（改成 X 那一次）
Check ($rebuilds.Count -ge $expectMin) `
      "日志里有 $($rebuilds.Count) 次「捕获会话已重建」（期望 >= $expectMin）" `
      '一次都没有的话，说明改模式根本没有让 DXGI 会话失效 —— 那这个触发器就没有在测东西。'
Check ($summaries.Count -ge 1 -and $summaryMax -ge 1) `
      "收尾统计里报了重建次数 = $summaryMax（共 $($summaries.Count) 份统计块）" `
      '统计块一份都没有说明程序没有正常收尾（或者 [4] 上面那段"两个文件都要读"又漏了一个）。'

# 日志里出现过的错误行，原样打出来 —— 自恢复"成功"和"每次都在报错但最后还是
# 接上了"是两件事，后者也值得看一眼。
# 判据要**区分大小写**（-cmatch）：-match 默认不区分，于是程序自己收尾时打印的
# "ffprobe -v error …" 那几行建议命令会被当成 ERROR 命中 —— 一个纯噪音的假
# 命中，而它教人忽略这一节。日志的级别标记就是 `[WARN ]` / `[ERROR]` 这个形状。
$warns = @($txt -split "`r?`n" | Where-Object { $_ -cmatch '\[WARN |\[ERROR|失败' } |
           Select-Object -First 8)
if ($warns.Count -gt 0) {
    Note '日志里的告警/错误行（前 8 条）：'
    $warns | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
}

Section '[5] 录下来的文件'

# --- 分段 ----------------------------------------------------------------
#
# 换分辨率之后编码器的 SPS/PPS、MP4 的轨道头、SDP 里的 sprop-parameter-sets
# 全都绑死在旧尺寸上，就地换只会得到一个解不出来的文件 —— 所以程序按段另起
# 文件：第一段是用户给的名字，之后是 main.1.mp4、main.2.mp4。
#
# 于是 resize 那一路的判据和 refresh 那一路**正好相反**，不能用同一组：
# 拿"只该有一段"去卡 resize 是假失败，拿"该有两段"去卡 refresh 又会把一个真
# 缺陷（尺寸没变却误判成分段）判成通过。
$segs = @()
for ($i = 0; ; $i++) {
    $p = if ($i -eq 0) { Join-Path $out 'main.mp4' } else { Join-Path $out "main.$i.mp4" }
    if (-not (Test-Path -LiteralPath $p)) { break }
    $segs += $p
}
$segNames = if ($segs.Count -gt 0) { ($segs | ForEach-Object { Split-Path -Leaf $_ }) -join ', ' } else { '一个都没有' }
Check ($segs.Count -ge 1) "有 main.mp4（共 $($segs.Count) 段：$segNames）"

if ($restartsSession) {
    Check ($segs.Count -ge 2) "换分辨率另起了新的一段（共 $($segs.Count) 段：$segNames）" `
          '换了分辨率却没有新段，说明程序就地改了尺寸 —— 那个文件解不出来。'
    Check ($txt -match '采集尺寸从') '日志里报了「采集尺寸从…变成…」' `
          '没报的话，分段是无声发生的：用户只看到凭空多出一个文件，不知道为什么。'
    $segLine = [regex]::Match($txt, '分段\s*:\s*共\s*(\d+)\s*段')
    Check ($segLine.Success -and [int]$segLine.Groups[1].Value -eq $segs.Count) `
          "收尾报的分段数 = $(if ($segLine.Success) { $segLine.Groups[1].Value } else { '没找到这一行' })（磁盘上 $($segs.Count) 段）" `
          '两处对不上说明程序自己数的和真落盘的不是一回事（漏写、或者名字拼错了）。'
} else {
    Check ($segs.Count -eq 1) "尺寸没变，只落了一段（共 $($segs.Count) 段：$segNames）" `
          'refresh 那一路尺寸没变，不该分段；分了段说明"尺寸变了"的判断误报了。'
}

# --- 每段各自的尺寸和时长 -------------------------------------------------
#
# 尺寸**逐段**核：这是"换分辨率真的换到了"唯一能直接看出来的地方。光看段数
# 不够 —— 一个把新尺寸算错（比如忘了更新中转纹理）的实现照样会分段。
$segInfo = @()
$totalDur = 0.0
foreach ($p in $segs) {
    $info = Invoke-WithTimeout $ffprobe @(
        '-v', 'error', '-show_entries', 'stream=width,height',
        '-show_entries', 'format=duration',
        '-of', 'default=noprint_wrappers=1', '-i', $p) 20000
    $kv = @{}
    foreach ($line in ($info.Out -split "`r?`n")) {
        if ($line -match '^(\w+)=(.*)$') { $kv[$Matches[1]] = $Matches[2] }
    }
    $d = 0.0
    [void][double]::TryParse($kv['duration'], [ref]$d)
    $totalDur += $d
    $segInfo += [pscustomobject]@{
        Name = Split-Path -Leaf $p
        Size = "$($kv['width'])x$($kv['height'])"
        Dur  = $d
    }
}
foreach ($s in $segInfo) {
    Note ('{0,-14} {1,-11} {2,5:n1}s' -f $s.Name, $s.Size, $s.Dur)
}

if ($restartsSession -and $segInfo.Count -ge 2) {
    Check ($segInfo[0].Size -eq $origSize) `
          "第 1 段是 $($segInfo[0].Size)（应为 $origSize —— 开录时的显示模式）"
    # 换分辨率那一段的尺寸**必须**在整张表里出现过。按"第 N 段"去卡是错的：
    # 两路串在一起跑时（默认就是 refresh, resize），resize 触发时前面已经因为
    # 换刷新率分过段了，它落在哪一段取决于排在前面的有几路。
    $hit = @($segInfo | Where-Object { $_.Size -eq "${resizeW}x${resizeH}" })
    Check ($hit.Count -ge 1) `
          "有按新尺寸 ${resizeW}x${resizeH} 重开的那一段（$(($hit | ForEach-Object { $_.Name }) -join ', ')）" `
          '没有的话说明重开时还在用旧的输出信息 —— 尺寸变了，新段却没跟着变。'
    if ($segInfo.Count -ge 3) {
        Check ($segInfo[-1].Size -eq $origSize) `
              "最后一段是 $($segInfo[-1].Size)（切回原尺寸之后应为 $origSize）" `
              '切回原尺寸时也得重开一段，而且尺寸要跟着回来。'
    }
}

# 总时长而不是单段时长：采集会话死掉的话总时长会短一截，而"短一截"恰好是
# 最容易被忽略的失败 —— 每一段单拎出来都能打开、能播、有画面，只是提前结束了。
if ($segInfo.Count -ge 1) {
    Check ($totalDur -ge ($Seconds - 4)) `
          "总时长 $([math]::Round($totalDur,1))s（要求 $Seconds s，容差 4s）" `
          '某一段提前结束了。单看每一段能播是看不出来的。'
}

# 最后一秒的画面必须**是有内容的**，不是黑屏。这一条才真正回答"重建之后
# 还在出画" —— 时长对得上也可能是"会话没了、一直在编上一帧"。
#
# `metadata=print` 默认把结果按 **INFO 级别写进日志**，而这里是 -v error，
# 于是它一个字都不打 —— 读出来永远是"没有 YAVG"，而报出来的失败信息指向
# "结尾是黑屏"。`:file=-` 让它直接写 stdout，绕开日志级别这一层。
# （当时就是这么踩的：一个量不出东西的检查，报的是被测对象的错。）
if ($segInfo.Count -ge 1) {
    $last = $segInfo[-1]
    $lastPath = Join-Path $out $last.Name
    $stat = Invoke-WithTimeout $ffmpeg @(
        '-v', 'error', '-ss', "$([math]::Max(0, $last.Dur - 1.5))", '-i', $lastPath,
        '-frames:v', '1', '-vf', 'signalstats,metadata=print:file=-',
        '-f', 'null', '-') 25000
    $m = [regex]::Match($stat.Out + $stat.Err, 'YAVG=([0-9.]+)')
    # 分成两条：读不出来和读出来是黑的，是两件不同的事（前者是这把尺子坏了），
    # 合成一条的话尺子坏了会被报成"结尾黑屏"。
    Check $m.Success "取到了 $($last.Name) 结尾那一帧的亮度"
    if ($m.Success) {
        $yavg = 0.0
        [void][double]::TryParse($m.Groups[1].Value, [ref]$yavg)
        Check ($yavg -gt 5 -and $yavg -lt 250) `
              "最后一秒的画面有内容（平均亮度 $([math]::Round($yavg,1))，黑屏约 0、纯白约 255）" `
              '亮度贴边说明结尾这段是黑的/白的 —— 采集没真的接上。'
    }
}

Write-Host ''
Write-Host ('=' * 60)
if ($failed -gt 0) {
    Write-Host "失败 $failed 项。日志：$out\run.err" -ForegroundColor Red
    exit 1
}
Write-Host '全部通过。' -ForegroundColor Green

# ⚠️ `exit 0` 不能省：不加 `exit` 结束的 .ps1 不会把 `$LASTEXITCODE` 归零，
# 它留着脚本里最后一个**原生命令**的值 —— 而这个脚本一路在跑 ffmpeg/ffprobe/
# taskkill，其中好几个是故意非零的。见陷阱 89。
exit 0
