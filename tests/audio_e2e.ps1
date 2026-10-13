<#
音频全链路：**从真实采集**录一段带声音的 MP4 与 RTSP，验证"采集→混音→编码"的接线。

## 为什么这条不能由 tests\rtsp_audio_check.ps1 代替

那个测试是**协议层**的：它自己造 x264 + 自己造 AAC + 真的 `RtspServer`，证明的是
"推流与封装正确"。它**没有**经过 DXGI 采集、没有经过 WASAPI 环回、没有经过
`AudioMixer`、也没有经过 `AudioTrack` 的按拍驱动 —— 也就是说，把
`capture_session.cpp` 里接音频的那几行全删掉，它**照样 31 项全绿**。
本脚本要证的恰好是那几行：真实声音 → 真的进了 MP4 和 RTSP。

**这句话量过了，不是推的**（陷阱 85：每写一条断言先把它改坏看会不会红）。
把 `capture_session.cpp` 里 `aopt.mixer.system.gain` 改成 **`0.0f`**（音轨照样在、
时长照样对、采样率照样对，只是**永远是静音**）重新编译，然后：

  * `tests\rtsp_audio_check.ps1` → **全部通过，退出码 0**（一条都不红）
  * `tests\audio_e2e.ps1`      → **失败 4 项，退出码 1**，红的正好是
    `[1c] mean_volume`、`[1d] 放音与静音差 0.0 dB`、`[2] RTSP mean_volume`、
    `[3] 电平 L/R` —— 四条**全是"有没有声音"**，没有一条是结构性的。
  * 同一份文件里那些结构性判据（两条流、aac、48000、2 声道、**时长差 0.045s**）
    **一条都没红** —— 这正是"协议层看不见这一段接线"的具体形状。

另一次变体是把 `aopt.mixer.system.enabled` 改成 `false`（源一个都不勾）：那次
`AudioTrack::start()` 直接拒绝（陷阱 78），音频整个不建，`audio_e2e.ps1` 红 9 项。

判据里**唯一**能说明 A/V 对齐的是"音轨时长与视频时长差 < 0.5s"。另外三条
（两条流、48000、2 声道）只说明封装对了。

## 会出声

它用 `tests\play_tone.cpp`（`build\play_tone.exe`）循环播一段 1 kHz 正弦——
**这个测试真的会让你的扬声器响起来**。跑之前把系统音量调下来。

为什么要放声音而不是"录 10 秒就算"：安静时的音轨**时长照样正确、帧数照样正确、
`ffprobe` 照样两条流、采样率照样 48000** —— 也就是说结构性的判据一条都分辨不出
"有声音"和"没声音"。**只有电平（和 `volumedetect`）能分辨。** 所以"非静音"这一条
必须有一个**确定在响**的源，否则它判的是房间里的噪音。

（顺带更正计划里的一条：原文说环回安静时"一个包都不来"，**在这台机器上不成立**。
实测冷引擎、全程不放声音的 8 秒录制是 `376 包，补零 0 帧` —— 引擎不停，只是每包
全零。所以 `[1d]` 不判"补零 > 0"，见那一节的注释与陷阱 87。）

## 段落

    [0]  前置：可执行文件在不在、play_tone 比源码新、有没有播放设备
    [1]  --audio system 从真实采集录 10 秒（同时放着声音）：
         ffprobe 两条流、aac 48000/2、**音轨与视频时长差 < 0.5s**、
         两者各自都落在 10 秒附近（只判"两者相等"的话，两路一起错也会过）
    [1c] 同一份 MP4 的音频轨量一次 `volumedetect`：**非静音**
         （这条比 [2] 的 RTSP 那条更靠近"接线"，因为它完全不走网络）
    [1d] 同一把尺子量一段**真静音**（环回开着、不放声音）：音轨照样在、时长照样
         对、电平 ≈ 0、`mean_volume < -60 dB`，且比 [1c] **低 20 dB 以上**。
         最后那条差值才是"这把尺子在区分"，光判"静音 < -60"是证明不了什么的
         （陷阱 85：尺子要是一条恒定的读数，它也照样绿）
    [1b] --no-audio：**恰好一条流**（陷阱 78 的另一半 —— 只验开着的那一半，
         会漏掉"其实一直挂着一条空音轨"）
    [2]  --rtsp 起一条真流，ffprobe 断言同一个会话里两条流，
         `ffmpeg -map 0:a -af volumedetect` 量出 `mean_volume > -60 dB`
    [3]  日志：`音频已就绪`（stderr）与收尾那行 `音频 ok 音轨 X.XXXs`（stdout）
         都在，音轨秒数对得上视频，"补零"没有占到大头

## 为什么非写成脚本

两条：`ffprobe` 拉一个没人应答的 RTSP 地址**会永远等下去**（陷阱 44），必须
有硬超时；以及 `volumedetect` 的输出是 INFO 级、`-v error` 会让它一字不打
（陷阱 63）—— 手工敲的时候这两件事都会以"检查通过了"的样子收场。

## 用法

    powershell -ExecutionPolicy Bypass -File tests\audio_e2e.ps1

前置：`build\play_tone.exe` 已按 `tests\play_tone.cpp` 头部的 g++ 命令行编译过
（本脚本会检查它比 .cpp 新）；`build\bin\desk2onvif.exe` 已构建。

端口：RTSP **8561**（`rtsp_audio_check.ps1` 用 8559/8560，本脚本刻意错开 ——
同一个端口连着起停两次会撞上上一轮 Live555 还没释放的竞态，报出来是"服务器
起不来"）。配置**显式**指向 `out\audio-e2e\cfg.json`：不这么写的话，`%APPDATA%`
里那份用户自己的配置会参与解析，而它里面 `audio` 段是什么只有那台机器知道。
#>
param(
    # [1] 录多久。10 秒是计划里写的数：够长到让采集/混音/编码都进入稳态，
    # 又短到整个脚本跑在一分钟上下。
    [int]$Seconds = 10,
    # [1b] 纯视频那一段。它只验"流数对不对"，5 秒足够。
    [int]$ShortSeconds = 5,
    [int]$RtspPort = 8561,
    # [2] 服务活多久。要给"起服务 + 等端口 + 拉 $PullSeconds 秒 + 收尾"全留出余量；
    # 给短了的表现是被测程序跑到一半自己到点退出，报出来是"连不上"。
    [int]$RtspSeconds = 30,
    [int]$PullSeconds = 6
)
$ErrorActionPreference = 'Stop'
# 子进程输出一律按 UTF-8 解。**必须显式写死，不能靠控制台的代码页** ——
# 不设的话 ProcessStartInfo 用的是 Console.OutputEncoding（默认 936），
# 程序的 UTF-8 字节被按 GBK 解成乱码，再被 WriteAllText 按 UTF-8 写进日志，
# 变成"双重编码"（陷阱 41）。
$utf8 = New-Object Text.UTF8Encoding $false
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root 'out\audio-e2e'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$exe     = Join-Path $root 'build\bin\desk2onvif.exe'
$toneExe = Join-Path $root 'build\play_tone.exe'
$toneSrc = Join-Path $root 'tests\play_tone.cpp'
$ffprobe = Join-Path $root 'third_party\ffmpeg\bin\ffprobe.exe'
$ffmpeg  = Join-Path $root 'third_party\ffmpeg\bin\ffmpeg.exe'
$cfg     = Join-Path $out 'cfg.json'
$ready   = Join-Path $out 'tone.ready'

# **DLL 的 PATH 必须在这里给。** desk2onvif.exe 是 MinGW 编的，运行时要
# build\bin 旁边的 av*.dll 以及 mingw64\bin 下的 libstdc++/libgcc/libwinpthread。
# 缺了的话进程以 0xC0000135 起来就死，而这里看到的只是"端口 20 秒内没起来"——
# 离原因很远（陷阱 18 那个静默失败家族）。
$env:PATH = (Join-Path $root 'build\bin') + ';' +
            (Join-Path $root 'third_party\ffmpeg\bin') + ';' +
            'C:\msys64\mingw64\bin;' + $env:PATH

# 每次从干净配置开始：上一次留下的 audio.enabled / 设备 id 会让这一次的结果
# 取决于上一跑，而症状看起来像"这次配置没生效"。
Remove-Item $cfg -ErrorAction SilentlyContinue
foreach ($f in 'rec.mp4', 'rec-noaudio.mp4', 'rec-silent.mp4',
               'a.log', 'a.err', 'b.log', 'b.err', 'c.log', 'c.err', 'd.log', 'd.err') {
    Remove-Item (Join-Path $out $f) -ErrorAction SilentlyContinue
}
Remove-Item $ready -ErrorAction SilentlyContinue

$failed = 0
$skipped = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { "  [OK]   $what" } else { "  [FAIL] $what"; $script:failed++ }
}
function Skip([string]$what) {
    "  [SKIP] $what"; $script:skipped++
}

# 硬超时的外部命令。没有这一层，一次 ffprobe 挂死就会吃掉整个测试（陷阱 44）。
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

$script:tone = $null
# **名字不能叫 `$script:app`。** 脚本顶层那个 `$app` 本身就是脚本作用域的变量，
# 于是 `$script:app = $null`（在 Wait-App 里清场）会把**调用方手里的 `$app`
# 一起清掉** —— 紧接着的 `$app.P.ExitCode` 读到空，断言报出来是"退出码 （要 0）"，
# 看起来像被测程序没退，实际是自己把自己的句柄抹了。这正是陷阱 57 那个形状：
# **读不到退出码**和**退出码不对**在断言里长得一模一样。
$script:liveApp = $null
# [1c] 量出来的"放音那一跑是多少 dB"，[1d] 要拿它跟"静音那一跑"比。
# 提到脚本作用域是因为两段之间隔着别的段，局部变量传不过去；而在 [1d] 里
# 重新量一次等于让两段"各自量各自的"，那个比较就失去意义了。
$script:dbTone = $null

# 起被测程序。$Port > 0 时等端口能真的连上，而不是死等一个拍脑袋的秒数。
function Start-App {
    param([string[]]$Argv, [int]$Port = 0, [string]$LogBase = 'a')
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    # 工作目录给成项目根：`web\dist` 那个默认值是**相对路径**，按子进程的工作
    # 目录解析。这里虽然没开 --http，但同理的坑别处已经踩过（9 项假失败）。
    $psi.WorkingDirectory = $root
    $psi.Arguments = ($Argv | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = $utf8
    $psi.StandardErrorEncoding = $utf8
    $p = New-Object Diagnostics.Process
    $p.StartInfo = $psi
    if (-not $p.Start()) { throw "起不来：$exe" }
    $script:liveApp = [pscustomobject]@{
        P       = $p
        Out     = $p.StandardOutput.ReadToEndAsync()
        Err     = $p.StandardError.ReadToEndAsync()
        LogBase = $LogBase
    }
    if ($Port -gt 0) {
        $deadline = (Get-Date).AddSeconds(25)
        $ok = $false
        while ((Get-Date) -lt $deadline) {
            $c = New-Object Net.Sockets.TcpClient
            try { $c.Connect('127.0.0.1', $Port); $c.Close(); $ok = $true; break }
            catch { Start-Sleep -Milliseconds 300 }
        }
        if (-not $ok) {
            try { $p.Kill() } catch {}
            throw "RTSP 端口 $Port 25 秒内没起来（DLL 没找到的话就是这个症状）"
        }
    }
    return $script:liveApp
}

# 等它自己跑完（`--seconds` 到了就退），并把两份日志落盘。**两份都要落** ——
# 这个程序按级别分流：逐秒日志走 stderr，收尾汇总块走 stdout（陷阱 46）。
function Wait-App {
    param($App, [int]$TimeoutMs)
    $exited = $App.P.WaitForExit($TimeoutMs)
    if (-not $exited) { try { $App.P.Kill() } catch {} }
    [IO.File]::WriteAllText((Join-Path $out ($App.LogBase + '.log')), $App.Out.Result)
    [IO.File]::WriteAllText((Join-Path $out ($App.LogBase + '.err')), $App.Err.Result)
    $script:liveApp = $null
    return $exited
}

# 放声音，等它**真的响起来**。
#
# `--ready` 那个文件是 `play_tone.exe` 在 `PlaySound` 返回 TRUE 之后立刻写的。
# 有它就不用"起完睡 2 秒再看看"——猜短了录到的是一段安静的头，判"非静音"就
# 可能假失败；猜长了白等。同一条纪律见 rtsp_sub_e2e.ps1 的端口等待循环。
function Start-Tone {
    param([int]$Secs)
    Stop-Tone
    Remove-Item $ready -ErrorAction SilentlyContinue
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $toneExe
    $psi.Arguments = ('--seconds {0} --ready "{1}"') -f $Secs, $ready
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = $utf8
    $psi.StandardErrorEncoding = $utf8
    $p = New-Object Diagnostics.Process
    $p.StartInfo = $psi
    if (-not $p.Start()) { throw "起不来：$toneExe" }
    # 不读走管道的话，4 KB 缓冲写满之后子进程会卡在写 stdout 上 —— 它就不 sleep 了。
    $null = $p.StandardOutput.ReadToEndAsync()
    $null = $p.StandardError.ReadToEndAsync()
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $ready) { $script:tone = $p; return $p }
        if ($p.HasExited) {
            throw "play_tone 还没放出声就退了（退出码 $($p.ExitCode)）—— 这台机器上没有播放设备？"
        }
        Start-Sleep -Milliseconds 100
    }
    try { $p.Kill() } catch {}
    throw "play_tone 10 秒内没写出就绪文件 $ready"
}

function Stop-Tone {
    if ($null -eq $script:tone) { return }
    $p = $script:tone
    $script:tone = $null
    if (-not $p.HasExited) { try { $p.Kill() } catch {} }
    try { $p.Dispose() } catch {}
    Remove-Item $ready -ErrorAction SilentlyContinue
}

# 取最后一个匹配。**用 `Matches` 而不是 `-match`**：volumedetect 在有些情况下会
# 打两组（过滤器重新配置时先打一组 n_samples: 0），`-match` 只告诉你"有没有"，
# 拿不到是哪一组。取最后一个 = 收尾那组，也就是真正的那组统计。
function Last-Match([string]$text, [string]$pattern) {
    if ([string]::IsNullOrEmpty($text)) { return $null }
    $m = [regex]::Matches($text, $pattern)
    if ($m.Count -eq 0) { return $null }
    return $m[$m.Count - 1]
}

# 从收尾统计（stdout，`*.log`）里把那行音频汇总抓出来。
# 字段依次是：state, 音轨秒数, 样本帧数, 采样率, 包数, 补零帧数, 电平 L, 电平 R。
# 抓不到返回 $null —— **调用方必须把 $null 当成失败**：零命中时什么都不打印的话，
# 这一段就和"日志里本来就没有这些行"长得一模一样（陷阱 46）。
$kAudioSummaryPattern = '音频\s+(\w+)\s+音轨\s+([\d.]+)s（(\d+) 样本帧 @ (\d+) Hz），(\d+) 包，补零 (\d+) 帧，电平 L ([\d.]+) / R ([\d.]+)'
function Get-AudioSummary([string]$logPath) {
    if (-not (Test-Path $logPath)) { return $null }
    return Last-Match ([IO.File]::ReadAllText($logPath, [Text.Encoding]::UTF8)) $kAudioSummaryPattern
}

# 用 volumedetect 量一段音频的平均电平。**不能加 `-v error`** —— 它按 INFO 级别
# 写日志，压掉之后一个字都不打，而"量不出来"和"量出来是静音"会长得一模一样
# （陷阱 63：一把坏掉的尺子报的是别人的错）。
function Measure-MeanVolume {
    param([string[]]$Argv, [int]$TimeoutMs = 60000)
    $res = Invoke-WithTimeout -File $ffmpeg -Argv $Argv -TimeoutMs $TimeoutMs
    $m = Last-Match $res.Err 'mean_volume:\s*(-?[\d.]+|-inf)\s*dB'
    return [pscustomobject]@{ Match = $m; Err = $res.Err; TimedOut = $res.TimedOut }
}

# 把 ffprobe 的流列出来，按类型分类。返回 @{ Video = @(); Audio = @(); Raw = ... }
function Probe-Streams {
    param([string[]]$Argv, [int]$TimeoutMs = 30000)
    $res = Invoke-WithTimeout -File $ffprobe -Argv $Argv -TimeoutMs $TimeoutMs
    if ($res.TimedOut) { return [pscustomobject]@{ Video = @(); Audio = @(); Err = $res.Err } }
    $j = $null
    try { $j = $res.Out | ConvertFrom-Json } catch { return [pscustomobject]@{ Video = @(); Audio = @(); Err = "ffprobe 的输出解不动：$($res.Err)" } }
    return [pscustomobject]@{
        Video = @($j.streams | Where-Object { $_.codec_type -eq 'video' })
        Audio = @($j.streams | Where-Object { $_.codec_type -eq 'audio' })
        Err   = $res.Err
    }
}

"Desk2ONVIF 音频全链路 —— 会出声，跑之前把音量调下来。"
"(录 $Seconds 秒 / RTSP $RtspPort / 配置 $cfg)"

try {
    # --- 0. 前置 -----------------------------------------------------------
    "`n[0] 前置"
    foreach ($f in @($exe, $ffprobe, $ffmpeg)) {
        if (-not (Test-Path $f)) { throw "缺文件：$f" }
    }
    Check $true "desk2onvif.exe / ffmpeg / ffprobe 都在"
    if (-not (Test-Path $toneExe)) {
        throw "没有 $toneExe —— 先编译它：`n  g++ -std=c++17 -Wall -Wextra tests/play_tone.cpp -o build/play_tone.exe -lwinmm"
    }
    # 比 .cpp 旧就当场停下。拿一个旧二进制跑出来的"通过"是最坏的一种结果：
    # 它证明的是上一次的代码。
    $toneStale = (Get-Item $toneSrc).LastWriteTime -gt (Get-Item $toneExe).LastWriteTime
    Check (-not $toneStale) "play_tone.exe 不比源码旧$(if ($toneStale) { '（重新编译：见 tests\play_tone.cpp 头部）' })"

    # 有没有播放设备。**没有就跳过而不是判失败**：这台机器压根放不出声音，
    # 而本测试判"采到的是非静音"必须有一个确定在响的源。跳过要说得响亮 ——
    # 打印 [SKIP] 而不是静悄悄地少跑几项。
    $la = Invoke-WithTimeout -File $exe -Argv @('--list-audio') -TimeoutMs 20000
    $renderSec = ''
    if ($la.Out -match '(?s)播放设备(.*?)(录音设备|$)') { $renderSec = $Matches[1] }
    $hasRender = ($renderSec -match '(?m)^\s+\[默认\]') -or ($renderSec -match '(?m)^\s+\{')
    if (-not $hasRender) {
        Skip "这台机器一个播放设备都没有（--list-audio 的「播放设备」段是空的）"
        Skip "音频那几段全部没跑 —— **这不是通过**"
        "`n" + ('=' * 60)
        "跳过 $skipped 项（没有播放设备）。"
        exit 0
    }

    # --- 1. 真实采集录一段带声音的 MP4 -------------------------------------
    "`n[1] 从真实采集录 $Seconds 秒（同时放着 1 kHz）"
    $null = Start-Tone -Secs ($Seconds + 12)
    Check $true "测试音已经在响（play_tone 写了就绪文件）"
    $app = Start-App -Argv @(
        '--record', (Join-Path $out 'rec.mp4'),
        '--audio', 'system',
        '--config', $cfg,
        '--seconds', "$Seconds"
    ) -LogBase 'a'
    $exited = Wait-App $app -TimeoutMs (($Seconds + 60) * 1000)
    Stop-Tone
    Check $exited "采集在 $($Seconds + 60) 秒内自己退出了"
    $code = $app.P.ExitCode
    # 空值和"非 0"分开报。两者在断言里都读作失败，但**空值说明读不到了**，
    # 那是脚本自己的问题（陷阱 57 的形状），指向被测程序只会浪费时间。
    if ($null -eq $code) {
        Check $false "退出码读不到（空）—— 这是脚本自己的问题，别去查被测程序"
    } else {
        Check ($code -eq 0) "退出码 $code（要 0）"
    }

    $p1 = Probe-Streams -Argv @('-v', 'error', '-show_entries',
                               'stream=index,codec_type,codec_name,sample_rate,channels,duration',
                               '-show_entries', 'format=duration', '-of', 'json',
                               (Join-Path $out 'rec.mp4'))
    # 陷阱 77 的唯一判据：只看 `ffprobe -i` 的输出行数，看不出"两条流是不是
    # 在**同一个会话/文件**里"。
    Check ($p1.Video.Count -eq 1) "视频流恰好 1 条（实际 $($p1.Video.Count)）"
    Check ($p1.Audio.Count -eq 1) "音频流恰好 1 条（实际 $($p1.Audio.Count)）"
    if ($p1.Audio.Count -ge 1) {
        $a = $p1.Audio[0]
        Check ($a.codec_name -eq 'aac') "音频是 aac（实际 $($a.codec_name)）"
        Check ([int]$a.sample_rate -eq 48000) "采样率 48000（实际 $($a.sample_rate)）"
        Check ([int]$a.channels -eq 2) "2 声道（实际 $($a.channels)）"
    }
    if ($p1.Video.Count -ge 1 -and $p1.Audio.Count -ge 1) {
        $vdur = [double]$p1.Video[0].duration
        $adur = [double]$p1.Audio[0].duration
        # **这条是 A/V 同步的真正判据。**
        Check ([Math]::Abs($vdur - $adur) -lt 0.5) `
            ("音轨与视频时长差 {0:N3}s < 0.5s（视频 {1:N3} / 音频 {2:N3}）" -f ([Math]::Abs($vdur - $adur)), $vdur, $adur)
        # 两条各自都要在 $Seconds 附近。**只判"两者相等"是不够的**：两路同时
        # 错成一半长也会相等（mp4_dual_check 踩过的那个形状）。
        $lo = $Seconds - 0.5
        $hi = $Seconds + 0.5
        Check ($vdur -ge $lo -and $vdur -le $hi) ("视频时长 {0:N3}s 落在 [{1},{2}]" -f $vdur, $lo, $hi)
        Check ($adur -ge $lo -and $adur -le $hi) ("音轨时长 {0:N3}s 落在 [{1},{2}]" -f $adur, $lo, $hi)
    } else {
        Check $false "流数不对，时长那几条没法判"
    }

    # --- 1c. 录下来的那份音频，真的不是静音 --------------------------------
    # 这一条完全不走网络 —— 它离"采集→混音→编码"这条接线最近。上面那些
    # （两条流、48000、时长）在**音轨全是零**的情况下也全都成立。
    "`n[1c] 录下来的音轨非静音（不走网络的判据）"
    $mv = Measure-MeanVolume -Argv @('-hide_banner', '-i', (Join-Path $out 'rec.mp4'),
                                     '-map', '0:a', '-af', 'volumedetect', '-f', 'null', '-')
    if ($null -eq $mv.Match) {
        Check $false "volumedetect 一行都没量出来 —— 是尺子坏了，不是「静音」（陷阱 63）"
    } elseif ($mv.Match.Groups[1].Value -eq '-inf') {
        Check $false "音轨是**数字静音**（mean_volume = -inf）"
    } else {
        $script:dbTone = [double]$mv.Match.Groups[1].Value
        Check ($script:dbTone -gt -60) ("mean_volume {0:N1} dB > -60 dB" -f $script:dbTone)
    }

    # --- 1d. 环回开着、系统不出声 ------------------------------------------
    # 这一段是计划里「音频静音路径」那一行，同时**也是 [1c] 那把尺子的校验**：
    # 同一把尺子量一段真静音，必须量出"静音"来。少了这一步，`> -60 dB` 这条
    # 判据就没被证明过它在区分什么（陷阱 82：一个用来量东西的工具，本身要先
    # 被量一遍）。
    #
    # 实测（本机）：放音那一跑 -16.9 dB，静音这一跑 -91.0 dB —— 阈值 -60 落在
    # 中间，两边各有 30 dB 以上余量。
    #
    # **这一跑验的是"安静被如实录下来"，不是"补零那条路"。** 两者的区别是有
    # 实测依据的：环回的播放引擎只要还在转（别的声音，或者刚停下的那一段还没
    # 释放），它就会照常出包，只是每一包内容全零。在这台机器上它**从不**因为
    # 没人出声就停下来 —— 实测一次"冷引擎、全程不放声音"的 8 秒录制：`376 包，
    # 补零 0 帧`。所以这里**不判"补零 > 0"**：那是陷阱 81 成立的那个前提，
    # 而前提取决于环境，拿它当判据会在完全健康的机器上变红（陷阱 82 的形状）。
    # 「源端一个样本都不来」由 `tests\audio_mixer_check.cpp` 的 [1] 段确定性
    # 覆盖（一个源都不开、不碰设备），那才是钉"时间轴不建立在收了多少样本上"
    # 这一条的地方。
    #
    # 分两档判：
    #   硬判据（与这台机器安不安静无关）：音轨在、时长对、state 不是 error。
    #   软判据（要日志证明这一跑**真的**安静才判）：mean_volume < -60 dB，
    #     且比放音那一跑低至少 20 dB。机器不安静时打 [SKIP]，**不是**通过。
    "`n[1d] 环回开着、系统不出声：音轨照样在、时长照样对"
    $app = Start-App -Argv @(
        '--record', (Join-Path $out 'rec-silent.mp4'),
        '--audio', 'system',
        '--config', $cfg,
        '--seconds', "$ShortSeconds"
    ) -LogBase 'd'
    $exited = Wait-App $app -TimeoutMs (($ShortSeconds + 60) * 1000)
    Check $exited "静音那一跑自己退出了"

    $p4 = Probe-Streams -Argv @('-v', 'error', '-show_entries',
                                'stream=codec_type,codec_name,duration',
                                '-of', 'json', (Join-Path $out 'rec-silent.mp4'))
    Check ($p4.Audio.Count -eq 1) "音轨仍然存在（实际 $($p4.Audio.Count) 条）—— 环回安静不等于没有音轨"
    Check ($p4.Video.Count -eq 1) "视频流 1 条（实际 $($p4.Video.Count)）"

    $sumD = Get-AudioSummary (Join-Path $out 'd.log')
    if ($null -eq $sumD) {
        Check $false "静音那一跑的收尾统计里没有「音频 … 音轨 …」那一行（零命中）"
        $silent = $false
    } else {
        Check $true "静音那一跑的收尾统计有「音频 … 音轨 …」那一行"
        # **这条是整段的重点。** 时间轴要是建立在"从源端收到了多少样本"上，
        # 环回一安静这一段音轨就会是 0 秒 —— 而那在"两条流都在、采样率也对"
        # 那几条里完全看不出来。
        $trackD = [double]$sumD.Groups[2].Value
        $lvlD_L = [double]$sumD.Groups[7].Value
        $lvlD_R = [double]$sumD.Groups[8].Value
        Check ([Math]::Abs($trackD - $ShortSeconds) -lt 0.5) `
            ("静音这一跑音轨 {0:N3}s 仍然对得上 {1} 秒（时间轴由编码线程按拍驱动，不是按收了多少样本）" -f $trackD, $ShortSeconds)
        Check ($sumD.Groups[1].Value -ne 'error') ("audio_state 不是 error（实际 $($sumD.Groups[1].Value)）")
        Check (($lvlD_L -lt 0.01) -and ($lvlD_R -lt 0.01)) `
            ("静音这一跑电平 L {0:N3} / R {1:N3} 都 < 0.01（没在响）" -f $lvlD_L, $lvlD_R)
        # 报告这一跑落在哪种情形里。**只报告、不判** —— 见上面那段注释。
        $zeroD = [double]$sumD.Groups[6].Value
        if ($zeroD -gt 0) {
            "       这一跑源端确实没数据（补零 {0:N0} 帧）：补零路径**走到了**" -f $zeroD
        } else {
            "       这一跑的环回一直在出包（{0} 包、补零 0 帧），所以**补零路径没被走到**" -f $sumD.Groups[5].Value
            "       —— 补零那条由 tests\audio_mixer_check.cpp 的 [1] 段确定性覆盖，不在这里重复"
        }
        # 电平小 == 这一跑确实安静（不是"凑巧量到别的声音"）。用它来决定软判据
        # 要不要判 —— 而不是假定这台机器是安静的。
        $silent = ($lvlD_L -lt 0.01) -and ($lvlD_R -lt 0.01)
    }

    if (-not $silent) {
        Skip "这一跑机器并不安静（电平 ≥ 0.01），静音那两条判据没验到 —— **这不是通过**"
    } else {
        $mv3 = Measure-MeanVolume -Argv @('-hide_banner', '-i', (Join-Path $out 'rec-silent.mp4'),
                                          '-map', '0:a', '-af', 'volumedetect', '-f', 'null', '-')
        if ($null -eq $mv3.Match) {
            Check $false "静音那一跑 volumedetect 一行都没量出来（尺子坏了，陷阱 63）"
        } elseif ($mv3.Match.Groups[1].Value -eq '-inf') {
            Check $true "静音那一跑是数字静音（mean_volume = -inf）"
        } else {
            $dbSilent = [double]$mv3.Match.Groups[1].Value
            Check ($dbSilent -lt -60) ("静音那一跑 mean_volume {0:N1} dB < -60 dB" -f $dbSilent)
            # **这一条才是尺子的校验**：同一个判据在两个真的不同的对象上给出了
            # 真的不同的数。上一条（静音 < -60）单独看证明不了什么 —— 尺子要是
            # constant 地报 -91，它也照样绿（陷阱 85：怎么都绿的断言等于没验）。
            if ($null -eq $script:dbTone) {
                Skip "[1c] 没量出数来（那边已经报过失败了），差值的这一条没法判"
            } else {
                Check (($script:dbTone - $dbSilent) -gt 20) `
                    ("放音 {0:N1} dB 比静音 {1:N1} dB 高 {2:N1} dB（> 20）—— 这把尺子真的在区分" -f `
                     $script:dbTone, $dbSilent, ($script:dbTone - $dbSilent))
            }
        }
    }

    # --- 1b. 关掉音频：只有一条流 ------------------------------------------
    "`n[1b] --no-audio：只有一条流"
    $app = Start-App -Argv @(
        '--record', (Join-Path $out 'rec-noaudio.mp4'),
        '--no-audio',
        '--config', $cfg,
        '--seconds', "$ShortSeconds"
    ) -LogBase 'b'
    $exited = Wait-App $app -TimeoutMs (($ShortSeconds + 60) * 1000)
    Check $exited "纯视频那一段自己退出了"
    $p2 = Probe-Streams -Argv @('-v', 'error', '-show_entries', 'stream=codec_type',
                               '-of', 'json', (Join-Path $out 'rec-noaudio.mp4'))
    # 陷阱 78 的另一半：只验"开着的时候有音轨"会漏掉"关着的时候其实一直挂着
    # 一条空音轨"，而后者对 NVR 是**两回事**（它会画上音频图标并期待声音）。
    Check ($p2.Audio.Count -eq 0) "零条音频流（实际 $($p2.Audio.Count)）"
    Check ($p2.Video.Count -eq 1) "一条视频流（实际 $($p2.Video.Count)）"

    # --- 2. RTSP：拉下来解一遍，量非静音 -----------------------------------
    "`n[2] RTSP 拉 $PullSeconds 秒（端口 $RtspPort）"
    $null = Start-Tone -Secs ($RtspSeconds + 12)
    $app = Start-App -Argv @(
        '--rtsp', '--rtsp-port', "$RtspPort",
        '--audio', 'system',
        '--config', $cfg,
        '--seconds', "$RtspSeconds"
    ) -Port $RtspPort -LogBase 'c'
    Start-Sleep -Seconds 2   # 让编码器把首帧和音频头吐出来

    $url = "rtsp://127.0.0.1:$RtspPort/live"
    $p3 = Probe-Streams -Argv @('-v', 'error', '-rtsp_transport', 'tcp',
                               '-show_entries', 'stream=index,codec_type,codec_name',
                               '-of', 'json', '-i', $url)
    Check ($p3.Video.Count -eq 1) "会话里 1 条视频流（实际 $($p3.Video.Count)）"
    Check ($p3.Audio.Count -eq 1) "会话里 1 条音频流（实际 $($p3.Audio.Count)）—— 两条要在**同一个会话**里"
    if ($p3.Audio.Count -ge 1) {
        Check ($p3.Audio[0].codec_name -eq 'aac') "RTSP 上音频是 aac（实际 $($p3.Audio[0].codec_name)）"
    }

    $mv2 = Measure-MeanVolume -Argv @('-hide_banner', '-rtsp_transport', 'tcp',
                                      '-i', $url, '-t', "$PullSeconds",
                                      '-map', '0:a', '-af', 'volumedetect',
                                      '-f', 'null', '-')
    if ($null -eq $mv2.Match) {
        Check $false "RTSP 那一路 volumedetect 一行都没量出来（尺子坏了，或者流里没音频）"
    } elseif ($mv2.Match.Groups[1].Value -eq '-inf') {
        Check $false "RTSP 上拉下来的是**数字静音**"
    } else {
        $db2 = [double]$mv2.Match.Groups[1].Value
        Check ($db2 -gt -60) ("RTSP mean_volume {0:N1} dB > -60 dB" -f $db2)
    }

    Stop-Tone
    $exited = Wait-App $app -TimeoutMs (($RtspSeconds + 30) * 1000)
    Check $exited "RTSP 那一段自己退出了"

    # --- 3. 日志 ------------------------------------------------------------
    # **两个文件都要读。** 这个程序按级别分流：逐秒日志走 stderr，收尾的汇总块
    # 走 stdout。只读其中一个，这一段就会安静地什么都不打 —— 而"什么都不打"
    # 和"日志里本来就没有这些行"看起来一模一样（陷阱 46）。
    "`n[3] 日志（两路都读，且零命中就失败）"
    $errA = [IO.File]::ReadAllText((Join-Path $out 'a.err'), [Text.Encoding]::UTF8)
    $logA = [IO.File]::ReadAllText((Join-Path $out 'a.log'), [Text.Encoding]::UTF8)
    $errC = [IO.File]::ReadAllText((Join-Path $out 'c.err'), [Text.Encoding]::UTF8)

    # `音频已就绪` 是 INFO，走 stderr。它证明的是 AudioTrack 真的起来了
    # （而不是"配置里开了音频，但两个源都没勾"那条静默降级的路径）。
    Check ($null -ne (Last-Match $errC '音频已就绪：48000 Hz 2 声道')) `
        "服务端日志里有「音频已就绪：48000 Hz 2 声道」（stderr）"

    # 收尾那行在 stdout。**音轨秒数必须对得上视频的秒数** —— 这是"时间轴由编码
    # 线程按拍驱动"这条设计的判据：按源端收到多少样本推的话，系统一安静音轨
    # 就会短一截，而那正是环回最常见的状态。
    $sum = Last-Match $logA '音频\s+(\w+)\s+音轨\s+([\d.]+)s（(\d+) 样本帧 @ (\d+) Hz），(\d+) 包，补零 (\d+) 帧，电平 L ([\d.]+) / R ([\d.]+)'
    if ($null -eq $sum) {
        Check $false "收尾统计里没有「音频 … 音轨 …」那一行（零命中 —— 日志是不是分流到别处了？）"
    } else {
        Check $true "收尾统计有「音频 … 音轨 …」那一行"
        Check ($sum.Groups[1].Value -eq 'ok') "audio_state 是 ok（实际 $($sum.Groups[1].Value)）"
        $track = [double]$sum.Groups[2].Value
        $frames = [double]$sum.Groups[3].Value
        $rate = [double]$sum.Groups[4].Value
        $zero = [double]$sum.Groups[6].Value
        $lvlL = [double]$sum.Groups[7].Value
        $lvlR = [double]$sum.Groups[8].Value
        Check ([Math]::Abs($track - $frames / $rate) -lt 0.001) `
            ("音轨 {0:N3}s 与帧数/采样率 ({1}/{2}) 自洽" -f $track, $frames, $rate)
        Check ([Math]::Abs($track - $Seconds) -lt 0.5) `
            ("音轨 {0:N3}s 与录制的 $Seconds 秒对得上" -f $track)
        # **补零不能占大头。** 它涨本身不是故障（环回安静时全靠它撑时间轴，
        # 陷阱 81），但这一跑从头到尾都放着 1 kHz，源端要是没在给数据就说明
        # 环回这一路根本没接上 —— 而那种情况下时长依然全对。
        Check ($zero -lt $frames / 2) `
            ("补零 {0:N0} 帧 < 总数的一半（{1:N0}）—— 源端真的在给数据" -f $zero, $frames)
        # 电平是"混音器真的看到了声音"的唯一证据。它是**采集→混音**那一段的
        # 判据：上面每一条（时长、流数、采样率）在音轨全是零的时候也成立。
        Check (($lvlL -gt 0.01) -and ($lvlR -gt 0.01)) `
            ("电平 L {0:N3} / R {1:N3} 都 > 0.01（非静音）" -f $lvlL, $lvlR)
    }

    # 关掉音频那一跑，日志里一个字都不该提音频 —— 它是"真的没开"，而不是
    # "开了但没起作用"。
    $errB = [IO.File]::ReadAllText((Join-Path $out 'b.err'), [Text.Encoding]::UTF8)
    $logB = [IO.File]::ReadAllText((Join-Path $out 'b.log'), [Text.Encoding]::UTF8)
    $mentions = @(($errB + $logB) -split "`r?`n" | Where-Object { $_ -match '音频' })
    Check ($mentions.Count -eq 0) `
        "--no-audio 那一跑日志里一次都没提音频（实际 $($mentions.Count) 行）"
} finally {
    # 中途炸了也要把声音关掉 —— 否则它会一直响到自己的 --seconds 用完
    # （最多 $RtspSeconds + 12 秒），而人只看到脚本退出了。
    Stop-Tone
    if ($null -ne $script:liveApp -and -not $script:liveApp.P.HasExited) {
        try { $script:liveApp.P.Kill() } catch {}
    }
}

"`n" + ('=' * 60)
if ($failed -gt 0) { "失败 $failed 项。日志：$out"; exit 1 }
if ($skipped -gt 0) { "全部通过（跳过 $skipped 项）。" } else { "全部通过。" }

# ⚠️ `exit 0` 不能省，它不是装饰。
#
# 不带 `exit` 结束的 .ps1 **不会**把 `$LASTEXITCODE` 归零 —— 那个变量保留的是
# 脚本里最后一个**原生命令**留下的值。这个脚本一路在跑 ffprobe/ffmpeg/taskkill，
# 其中好几个是**故意**非零的（探一个注定没有音频流的输入、`taskkill` 收一个
# 已经自己退了的进程返回 128），于是"全部通过"打完之后调用方读到的可能是 1。
#
# 2026-10-08 实测：`out\rerun-after-perf-fix.ps1` 把三个脚本串起来跑，日志里
# 这一段明明写着"全部通过。"，紧跟着一行 `EXIT[audio_e2e.ps1] = 1`。查下去
# 发现是**这里**少了一句 —— 也就是"脚本说它过了、退出码说它没过"。
#
# 这正是陷阱 57 那一族（假失败比不测更坏）：假红会教人忽略真红，而下一个把
# 这些脚本串进流水线的人会花一晚上去查一个不存在的回归。
exit 0
