# RTSP 音频端到端：一个会话里到底是不是真的有 h264 + aac 两条轨，
# 以及解出来的声音是不是那条推出去的 1 kHz。
#
#   [1]  开着音频：ffprobe 报**同一个会话里两个流**、aac 48000/2；
#        ffmpeg 拉成 WAV，rtsp_audio_probe.py 量波形
#   [1b] 关着音频：同一个 ffprobe 必须**只报一条流**（陷阱 78 的另一半）
#   [2]  收尾统计里的入队/丢弃数，以及"AU 一个都没入队"这类现场
#
# 为什么非要用脚本：ffprobe 拉一路没人应答的 RTSP 地址会**永远等下去**
# （DESCRIBE 之后它阻塞在 socket 读上，没有超时）—— 在交互式调用里那等于把
# 整个调用拖到超时，连日志都来不及落盘。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File tests\rtsp_audio_check.ps1
#
# 前置：`build\rtsp_audio_check.exe` 已经按 tests\rtsp_audio_check.cpp 头部的
# g++ 命令行编译过（本脚本会检查它比 .cpp 新，旧的当场报出来而不是拿旧二进制跑）。
param(
    # [1] 拉 6 秒音频 + [0] 起服务，[1b] 再起一次 12 秒的服务 —— 两段串起来
    # 大约 30 秒。给短了的表现是后半段跑到一半被测程序自己到点退出，
    # 报出来的是"连不上"，看起来像产品坏了。
    [int]$Seconds = 40,
    [int]$RtspPort = 8559,
    # 关音频那一路用**另一个端口**：同一个端口连着起停两次会撞上上一轮
    # Live555 还没释放的竞态，失败看起来像"服务器起不来"。
    [int]$RtspPortNoAudio = 8560
)
$ErrorActionPreference = 'Stop'
# 子进程输出一律按 UTF-8 解。**必须显式写死，不能靠控制台的代码页**（陷阱 41）。
$utf8 = New-Object Text.UTF8Encoding $false
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root 'out\rtsp-audio'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$exe     = Join-Path $root 'build\rtsp_audio_check.exe'
$src     = Join-Path $root 'tests\rtsp_audio_check.cpp'
$ffprobe = Join-Path $root 'third_party\ffmpeg\bin\ffprobe.exe'
$ffmpeg  = Join-Path $root 'third_party\ffmpeg\bin\ffmpeg.exe'

$failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { "  [OK]   $what" } else { "  [FAIL] $what"; $script:failed++ }
}

# --- 0. 前置 ---------------------------------------------------------------
if (-not (Test-Path $exe)) {
    throw "没有 $exe —— 先按 tests\rtsp_audio_check.cpp 头部那段 g++ 命令行编译它。"
}
# 比 .cpp 旧就当场停下。拿一个旧二进制跑出来的"通过"是最坏的一种结果：
# 它证明的是上一次的代码。
if ((Get-Item $src).LastWriteTime -gt (Get-Item $exe).LastWriteTime) {
    throw "$exe 比源码旧 —— 先重新编译（见 tests\rtsp_audio_check.cpp 头部）。"
}

# **DLL 的 PATH 必须在这里给。** 这个 exe 是 MinGW 编的，运行时要
# third_party\ffmpeg\bin 下的 av*.dll（它们不在 exe 旁边），以及 mingw64\bin
# 下的 libstdc++/libgcc/libwinpthread。缺了的话进程以 0xC0000135 起来就死，
# 而这里看到的只是"端口 20 秒内没起来" —— 离原因很远。
$env:PATH = (Join-Path $root 'third_party\ffmpeg\bin') + ';' +
            'C:\msys64\mingw64\bin;' + $env:PATH

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

$script:app = $null
function Start-Harness {
    param([int]$Port, [int]$Secs, [string]$Mode)
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $root
    $psi.Arguments = ('{0} {1} {2}' -f $Port, $Secs, $Mode)
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = $utf8
    $psi.StandardErrorEncoding = $utf8
    $p = New-Object Diagnostics.Process
    $p.StartInfo = $psi
    if (-not $p.Start()) { throw "起不来：$exe" }
    $script:app = [pscustomobject]@{
        P   = $p
        Out = $p.StandardOutput.ReadToEndAsync()
        Err = $p.StandardError.ReadToEndAsync()
    }
    # 等端口真的能连上，而不是死等一个拍脑袋的秒数。
    $deadline = (Get-Date).AddSeconds(20)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
        $c = New-Object Net.Sockets.TcpClient
        try { $c.Connect('127.0.0.1', $Port); $c.Close(); $ok = $true; break }
        catch { Start-Sleep -Milliseconds 300 }
    }
    if (-not $ok) {
        try { $p.Kill() } catch {}
        throw "端口 $Port 20 秒内没起来（DLL 没找到的话就是这个症状）"
    }
    return $script:app
}

function Stop-Harness {
    param($Job, [int]$WaitMs = 20000)
    if (-not $Job) { return $true }
    $exited = $Job.P.WaitForExit($WaitMs)
    if (-not $exited) { try { $Job.P.Kill() } catch {}; $Job.P.WaitForExit(5000) | Out-Null }
    return $exited
}

function Save-Logs {
    param($Job, [string]$Tag)
    [IO.File]::WriteAllText((Join-Path $out "$Tag.out"), $Job.Out.Result, $utf8)
    [IO.File]::WriteAllText((Join-Path $out "$Tag.err"), $Job.Err.Result, $utf8)
}

# ffprobe 一个 RTSP 地址，把 streams 解析成对象数组。超时/解析失败返回 $null。
function Probe-Streams {
    param([string]$Url, [int]$TimeoutMs = 15000)
    $r = Invoke-WithTimeout $ffprobe @(
        '-v', 'error', '-rtsp_transport', 'tcp',
        '-show_entries', 'stream=codec_name,codec_type,sample_rate,channels,width,height',
        '-of', 'json', '-i', $Url) $TimeoutMs
    if ($r.TimedOut) { return $null }
    try { return ($r.Out | ConvertFrom-Json).streams } catch { return $null }
}

# --- 1. 开着音频 -----------------------------------------------------------
"`n[1] 一次会话里两条轨（音频开）"
$job = Start-Harness -Port $RtspPort -Secs $Seconds -Mode 'on'
Start-Sleep -Seconds 2
$url = "rtsp://127.0.0.1:$RtspPort/live"

$streams = Probe-Streams $url
Check ($null -ne $streams) "ffprobe 拿得到 SDP"
if ($null -ne $streams) {
    $vid = @($streams | Where-Object { $_.codec_type -eq 'video' })
    $aud = @($streams | Where-Object { $_.codec_type -eq 'audio' })
    # **`$streams.Count -eq 2` 这一条就是陷阱 77 的判据。** 音频要是被挂成了
    # 第二个 ServerMediaSession，这里只会看到一条流 —— 而 NVR 那边同样是
    # "只看到视频，且不报错"，靠日志和单元测试都照不出来。
    Check ($streams.Count -eq 2) "同一个会话里两个流（实际 $($streams.Count) 条）"
    Check ($vid.Count -eq 1) "其中视频 1 条"
    Check ($aud.Count -eq 1) "其中音频 1 条"
    if ($vid.Count -eq 1) {
        Check ($vid[0].codec_name -eq 'h264') "视频是 h264（$($vid[0].codec_name)）"
        Check ([int]$vid[0].width -gt 0 -and [int]$vid[0].height -gt 0) `
              "视频分辨率 $($vid[0].width)x$($vid[0].height)"
    }
    if ($aud.Count -eq 1) {
        Check ($aud[0].codec_name -eq 'aac') "音频是 aac（$($aud[0].codec_name)）"
        # 这两个数是从 SDP 的 `a=rtpmap:97 MPEG4-GENERIC/48000/2` 来的，
        # 而那个 payload 号是"会话里第几个 subsession"算出来的（96+n-1）。
        # 通道数对得上，说明 `config=1190` 那 2 个字节没写错。
        Check ([int]$aud[0].sample_rate -eq 48000) "音频采样率 $($aud[0].sample_rate)"
        Check ([int]$aud[0].channels -eq 2) "音频声道数 $($aud[0].channels)"
    }
}

# 拉一路音频存成 WAV，交给探针量波形。**这一段才是能分开"裸 AU 又套了一层
# ADTS 头"（解出来是噪音而 SDP 完全正常）的唯一手段。**
$wav = Join-Path $out 'pulled.wav'
Remove-Item $wav -ErrorAction SilentlyContinue
$pull = Invoke-WithTimeout $ffmpeg @(
    '-v', 'error', '-rtsp_transport', 'tcp', '-i', $url, '-map', '0:a',
    '-t', '6', '-acodec', 'pcm_s16le', '-y', $wav) 30000
if ($pull.TimedOut) { Check $false "ffmpeg 拉音频超时" }
else { Check (-not $pull.Err.Trim()) "ffmpeg 拉 6 秒音频零报错" }
if ($pull.Err.Trim()) { "         $($pull.Err.Trim() -split "`r?`n" | Select-Object -First 3)" }
Check (Test-Path $wav) "存下了 $wav"

if (Test-Path $wav) {
    [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
    & python -I -X utf8 (Join-Path $root 'tests\rtsp_audio_probe.py') $wav
    Check ($LASTEXITCODE -eq 0) "rtsp_audio_probe.py 退出码 $LASTEXITCODE"
}

$exited = Stop-Harness $job 30000
Save-Logs $job 'on'
Check $exited "被测程序自己跑完退出（不是被杀）"
if ($exited) { Check ($job.P.ExitCode -eq 0) "退出码 $($job.P.ExitCode)" }

# --- 1b. 关掉音频 ----------------------------------------------------------
# 陷阱 78：关掉音频时**不建 subsession**。"有一条一直是静音的音轨"在 NVR 上
# 是画着音频图标的，用户会去找一个不存在的问题 —— 所以这一半必须单独验。
"`n[1b] 关掉音频时只有一条流"
$job2 = Start-Harness -Port $RtspPortNoAudio -Secs 12 -Mode 'off'
Start-Sleep -Seconds 2
$streams2 = Probe-Streams "rtsp://127.0.0.1:$RtspPortNoAudio/live"
Check ($null -ne $streams2) "关着音频时 ffprobe 一样拿得到 SDP"
if ($null -ne $streams2) {
    Check (@($streams2).Count -eq 1) "会话里只有 1 条流（实际 $(@($streams2).Count) 条）"
    Check (@($streams2 | Where-Object { $_.codec_type -eq 'audio' }).Count -eq 0) `
          "没有任何音频流"
}
Stop-Harness $job2 | Out-Null
Save-Logs $job2 'off'

# --- 2. 收尾统计 -----------------------------------------------------------
"`n[2] 收尾统计"
function Get-LogText {
    param([string]$Tag)
    return @(
        [IO.File]::ReadAllText((Join-Path $out "$Tag.err"), [Text.Encoding]::UTF8)
        [IO.File]::ReadAllText((Join-Path $out "$Tag.out"), [Text.Encoding]::UTF8)
    ) -join "`n"
}

$txtOn = Get-LogText 'on'
# **一条都没匹配上就要失败。** 只打印命中的行的话，"过滤失效"和"日志里本来
# 就没有这些行"长得一模一样，这段检查会自己废掉而没人知道（陷阱 46）。
$hit = @($txtOn -split "`r?`n" | Where-Object { $_ -match '音频统计|视频统计|^audio: ' })
if ($hit.Count -eq 0) { Check $false "收尾统计里一条预期行都没有（日志是不是分流到别处了？）" }
else { $hit | ForEach-Object { "  $_" } }

$auLine = @($txtOn -split "`r?`n" | Where-Object { $_ -match '^音频统计:' })
Check ($auLine.Count -eq 1) "收尾统计里有且只有一行「音频统计」"
if ($auLine.Count -eq 1) {
    $null = $auLine[0] -match 'AU 入队=(\d+) AU 丢弃=(\d+) 最大 AU=(\d+).*payload=(\d+)'
    Check ([int]$Matches[1] -gt 100) "AU 入队 $($Matches[1]) 个（要 > 100）"
    Check ([int]$Matches[2] -eq 0) "AU 丢弃 $($Matches[2]) 个"
    Check ([int]$Matches[3] -gt 0 -and [int]$Matches[3] -lt 1000) `
          "最大 AU $($Matches[3]) 字节（几十到几百是正常的）"
    Check ([int]$Matches[4] -eq 97) "payload type $($Matches[4])（音频是会话里第二个 subsession）"
}

# 关着的那一次：payload 必须是 0、AU 一个都不该入队，而且日志要说清楚。
$txtOff = Get-LogText 'off'
Check ($txtOff -match 'audio: 关闭') "关音频那次明确说了「不建 subsession」"
$offLine = @($txtOff -split "`r?`n" | Where-Object { $_ -match '^音频统计:' })
if ($offLine.Count -eq 1) {
    $null = $offLine[0] -match 'AU 入队=(\d+).*payload=(\d+)'
    Check ([int]$Matches[1] -eq 0) "关着音频时 AU 入队 $($Matches[1]) 个"
    Check ([int]$Matches[2] -eq 0) "关着音频时 payload type $($Matches[2])"
} else { Check $false "关音频那次没有「音频统计」行" }

"`n" + ('=' * 60)
if ($failed -gt 0) { "失败 $failed 项。日志：$out\on.err / $out\on.out"; exit 1 }
"全部通过。"

# ⚠️ `exit 0` 不能省：不加 `exit` 结束的 .ps1 不会把 `$LASTEXITCODE` 归零，
# 它留着脚本里最后一个**原生命令**的值 —— 而这个脚本一路在跑 ffmpeg/ffprobe/
# taskkill，其中好几个是故意非零的。见陷阱 89。
exit 0
