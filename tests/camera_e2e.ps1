# 摄像头来源端到端（第五轮 Phase AE）：**一件事说明白 —— 这一路录下来的画面
# 到底是不是那台摄像头的**。
#
# ## 这个脚本存在的理由
#
# "换成摄像头"这个功能最容易被误判成"做好了"：`--camera` 收了参数、进程正常退出、
# `out\cam.mp4` 也能被 ffprobe 打开 —— 而它是**显示器**的画面。相机没被打开、
# 打开失败后悄悄退回桌面、或者那条 `source` 根本没接上，这三种缺陷在这三条事实
# 上表现完全一样。
#
# 所以判据只能是**尺寸**：这台摄像头的原生是 1920x1080，而这块屏是 2880x1920
# （`--list-displays` 现场读出来的，不写死）。两个数不可能撞上，于是"录出来的
# 是哪一路"有了一个不含糊的答案。
#
# ⚠️ 这一条判据**只在"摄像头和显示器尺寸不同"时成立**。一台 1920x1080 的屏配一台
# 1920x1080 的摄像头，它就会假绿 —— 到那时要换判据（比如拿 `--list-displays`
# 之外的第二个来源，或者比画面的实际内容）。这不是"以后再说"：脚本开头会把这个
# 前提**当场量一遍并说出来**，前提不成立时报的是"判据失效"，不是"摄像头坏了"。
#
# ## 变异检验（陷阱 85）
#
# `src/main.cpp` 里 `--camera` 那一小段把 `opt.source.kind` 设成 `webcam`。
# 把它强制回 `display` 重编再跑：这里[2]段的每一条都必须变红，而且报的是
# "录到的是显示器那一档尺寸"。实测过一次（见 out\cam-e2e\mut\ 的说明）。
#
# ## 为什么还要单跑一个脚本（Phase AD 的第一幕已经覆盖了大半）
#
# 第一幕验的是"多通道那条路"，它的通道 1 恰好是摄像头；而**单通道下摄像头能不能
# 用**是独立的一条（计划里第 3 条特意让摄像头不依赖多通道落地）。这个脚本不含
# 多通道假设：它就是"一台机器、一个摄像头、一份录像"。
#
# 用法：powershell -ExecutionPolicy Bypass -File tests\camera_e2e.ps1
param(
    [int]$RtspPort = 8553,
    [int]$HttpPort = 8098,
    [string]$Camera = '',
    [int]$Seconds = 12,
    [string]$HttpUser = 'admin',
    [string]$HttpPassword = 'admin'
)
$ErrorActionPreference = 'Stop'
# 子进程输出一律按 UTF-8 解，**不能靠控制台代码页**（默认 936 会把 UTF-8 字节
# 当 GBK 解成乱码，再按 UTF-8 写进日志，成为"双重编码"）。见 multichannel_e2e
# 文件头那条。
$utf8 = New-Object Text.UTF8Encoding $false
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root 'out\cam-e2e'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$script:apps = @()
$exe     = Join-Path $root 'build\bin\desk2onvif.exe'
$ffprobe = Join-Path $root 'third_party\ffmpeg\bin\ffprobe.exe'
$ffmpeg  = Join-Path $root 'third_party\ffmpeg\bin\ffmpeg.exe'

$failed = 0
function Check([bool]$ok, [string]$what, [string]$hint = '') {
    if ($ok) { "  [OK]   $what" }
    else { "  [FAIL] $what"; if ($hint) { "         $hint" }; $script:failed++ }
}

# --- 起一次进程（带硬超时）------------------------------------------------
function Invoke-WithTimeout {
    param([string]$File, [string[]]$Argv, [int]$TimeoutMs)
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = $utf8
    $psi.StandardErrorEncoding = $utf8
    $psi.Arguments = ($Argv | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $p = New-Object Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $o = $p.StandardOutput.ReadToEndAsync()
    $e = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutMs)) {
        try { $p.Kill() } catch {}
        try { $p.WaitForExit(10000) | Out-Null } catch {}
        return [pscustomobject]@{ Code = -1; Out = ''; Err = ''; Timeout = $true }
    }
    return [pscustomobject]@{ Code = $p.ExitCode; Out = $o.Result; Err = $e.Result;
                             Timeout = $false }
}

# 等一个端口空出来。上一趟的进程刚退出、端口还没被内核回收时起下一趟，会让新的
# 那个**先绑上网页端口、再在起 RTSP 时失败然后退出** —— 报出来像产品随机崩溃。
function Wait-PortFree {
    param([int[]]$Ports, [int]$Secs = 20)
    $deadline = (Get-Date).AddSeconds($Secs)
    while ((Get-Date) -lt $deadline) {
        $busy = $false
        foreach ($port in $Ports) {
            $c = New-Object Net.Sockets.TcpClient
            try { $c.Connect('127.0.0.1', $port); $c.Close(); $busy = $true } catch {}
        }
        if (-not $busy) { return $true }
        Start-Sleep -Milliseconds 300
    }
    return $false
}

# 把两路读干净的采集进程拉起来（`--seconds` 到点自己退，MP4 的 moov 才会写出来）。
function Start-App {
    param([string]$Tag, [string]$CamSelector, [int]$Secs, [string]$RecordPath)
    $null = Wait-PortFree @($HttpPort, ($HttpPort + 2), $RtspPort) 20
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    # `--web-root` 与 `WorkingDirectory` 都要显式给：静态目录的默认值是**相对
    # 路径**，按子进程的工作目录解析（见 multichannel_e2e 里同一段说明）。
    $psi.WorkingDirectory = $root
    $argv = @('--rtsp', '--rtsp-port', "$RtspPort", '--http', '--http-port', "$HttpPort",
              '--record', $RecordPath, '--seconds', "$Secs",
              '--camera', $CamSelector, '--http-user', $HttpUser,
              '--http-password', $HttpPassword,
              '--web-root', (Join-Path $root 'web\dist'))
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
    $app = [pscustomobject]@{ Tag = $Tag; P = $p
                              Out = $p.StandardOutput.ReadToEndAsync()
                              Err = $p.StandardError.ReadToEndAsync() }
    $script:apps += ,$app
    return $app
}

# 让它自己到点退出（`--seconds`），再落盘。**不能 Kill** —— 还在录的进程被砍掉
# 的 MP4 没有 moov 索引，ffprobe 报 "moov atom not found"，看起来像封装器坏了。
function Wait-App {
    param($App, [int]$ExtraMs = 60000)
    if (-not $App.P.WaitForExit($ExtraMs)) {
        try { $App.P.Kill() } catch {}
        try { $App.P.WaitForExit(10000) | Out-Null } catch {}
    }
    return (Dump-Logs $App)
}

function Stop-App {
    param($App, [int]$GraceWaitMs = 3000)
    $App.P.WaitForExit($GraceWaitMs) | Out-Null
    if (-not $App.P.HasExited) { try { $App.P.Kill() } catch {} }
    try { $App.P.WaitForExit(10000) | Out-Null } catch {}
    return (Dump-Logs $App)
}

# 两份日志分开落盘（查现场时要能分清"程序自己印的"和"错误流"），但**返回的是
# 拼在一起的那一份** —— 断言只关心"这行字出现过"，而 D2O 的日志走 stderr、
# 管道报告走 stdout，分开写的话每一条断言都要先想清楚该查哪一份。
function Dump-Logs {
    param($App)
    [IO.File]::WriteAllText((Join-Path $out "$($App.Tag).out.log"), $App.Out.Result, $utf8)
    [IO.File]::WriteAllText((Join-Path $out "$($App.Tag).err.log"), $App.Err.Result, $utf8)
    return ([IO.File]::ReadAllText((Join-Path $out "$($App.Tag).err.log"), $utf8) + "`n" +
            [IO.File]::ReadAllText((Join-Path $out "$($App.Tag).out.log"), $utf8))
}

# ffprobe 一个 MP4 的第一路视频流。`-v error` 之外**不加**日志级别参数：不加是
# 为了让它一个字都不多打（有 tests\dxgi_recovery.ps1 里那条教训）。
function Probe-File {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        return [pscustomobject]@{ Ok = $false; Err = '文件不存在' }
    }
    $r = Invoke-WithTimeout $ffprobe @(
        '-v', 'error', '-select_streams', 'v:0',
        '-show_entries', 'stream=width,height,codec_name,nb_frames,avg_frame_rate',
        '-show_entries', 'format=duration', '-of', 'json', $Path) 30000
    if ($r.Timeout) { return [pscustomobject]@{ Ok = $false; Err = 'ffprobe 超时' } }
    try { $j = $r.Out | ConvertFrom-Json } catch {
        return [pscustomobject]@{ Ok = $false; Err = "ffprobe 输出解析不了：$($r.Out)" }
    }
    if (-not $j.streams -or $j.streams.Count -eq 0) {
        return [pscustomobject]@{ Ok = $false; Err = '没有视频流' }
    }
    $s = $j.streams[0]
    $dur = 0.0
    [void][double]::TryParse("$($j.format.duration)", [ref]$dur)
    return [pscustomobject]@{ Ok = $true; Width = [int]$s.width; Height = [int]$s.height
                              Codec = "$($s.codec_name)"; Frames = [int]$s.nb_frames
                              Fps = "$($s.avg_frame_rate)"; Dur = $dur; Err = '' }
}

# 某个时刻那一帧的平均亮度。**`:file=-` 是必须的**：`metadata=print` 默认按 INFO
# 级别写进日志，而这里 -v error，于是它一个字都不打 —— 读出来永远是"没有 YAVG"，
# 报出来却指向"画面是黑的"（量具坏了被报成被测对象的错，dxgi_recovery 里踩过）。
function Frame-Yavg {
    param([string]$Path, [double]$AtSec)
    $r = Invoke-WithTimeout $ffmpeg @(
        '-v', 'error', '-ss', "$AtSec", '-i', $Path,
        '-frames:v', '1', '-vf', 'signalstats,metadata=print:file=-',
        '-f', 'null', '-') 30000
    $m = [regex]::Match($r.Out + $r.Err, 'YAVG=([0-9.]+)')
    if (-not $m.Success) { return -1.0 }
    $v = 0.0
    [void][double]::TryParse($m.Groups[1].Value, [ref]$v)
    return $v
}

try {
  # =========================================================================
  # [0] 前提：这台机器的屏和摄像头**尺寸不一样**（否则尺寸判据失效）
  # =========================================================================
  ""
  "[*] 摄像头来源端到端（Phase AE）"
  ""
  "[0] 先把尺子量准"
  $displays = Invoke-WithTimeout $exe @('--list-displays') 30000
  Check ($displays.Code -eq 0) "--list-displays 退出码 0（实际 $($displays.Code)）"
  # `#0  Intel(R) Arc(TM) Graphics — \\.\DISPLAY1（2880x1920 @ 0,0）`
  $dm = [regex]::Match($displays.Out, '（(\d+)x(\d+) @')
  Check $dm.Success "从 --list-displays 里读到了这块屏的尺寸" `
        "读不出来的话，下面'录到的不是显示器'那一条无从谈起（输出：$($displays.Out -replace "`n", ' / ')）"
  $dispW = 0; $dispH = 0
  if ($dm.Success) {
      $dispW = [int]$dm.Groups[1].Value; $dispH = [int]$dm.Groups[2].Value
      "         显示器 #0：${dispW}x${dispH}"
  }

  $cams = Invoke-WithTimeout $exe @('--list-cameras') 60000
  Check ($cams.Code -eq 0) "--list-cameras 退出码 0（实际 $($cams.Code)）"
  # `  #0  FHD Camera（默认）` 后面一行是符号链接。
  $cmm = [regex]::Match($cams.Out, '(?m)^\s+#0\s+(.+?)(?:（默认）)?\s*$')
  Check $cmm.Success "列出了至少一台摄像头（--list-cameras）" `
        "一台都列不出来的话，这一整条路无从验起（输出：$($cams.Out -replace "`n", ' / ')）"
  $camName = ''
  if ($cmm.Success) { $camName = $cmm.Groups[1].Value.Trim() }
  if (-not $Camera) { $Camera = $camName }
  "         用这一台：$Camera"

  Check ($dispW -gt 0 -and $dispH -gt 0 -and $dispW -ne 1920) `
        "前提成立：这块屏的宽度不是 1920，所以'录出的宽度是 1920'只可能来自摄像头" `
        "前提不成立时下面那条尺寸断言会假绿 —— 那时要换判据，而不是当成摄像头坏了"

  # =========================================================================
  # [1] 真的采一趟：--camera + --record
  # =========================================================================
  ""
  "[1] 跑一趟：--camera <它> --record out\cam-e2e\cam.mp4 --seconds $Seconds"
  $rec = Join-Path $out 'cam.mp4'
  Remove-Item $rec -ErrorAction SilentlyContinue
  $app = Start-App 'cam' $Camera $Seconds $rec
  $txt = Wait-App $app
  Check ($app.P.ExitCode -eq 0) "进程干净退出（实际 $($app.P.ExitCode)）" `
        "非 0 时下面每一条都可能只是这一条的后果；日志在 out\cam-e2e\cam.err.log"
  # **这一行是"源真的换了"最直接的证据**（它是摄像头那一侧自己报的）。
  # 没有它的话，后面那条尺寸断言即便成立也可能只是"显示器恰好被缩放成了那个数"。
  Check ($txt -like '*摄像头已打开*') "日志里有「摄像头已打开：…」" `
        "没有它 = 摄像头压根没被打开，那录到的只能是桌面"
  # 协商出来的尺寸（`FHD Camera（1920x1080 @ 30fps，Media Foundation）`）。用它
  # 当**另一个独立来源**去对 ffprobe 报的尺寸 —— 两个数不一致 = 报的和编的不是一回事。
  $cm = [regex]::Match($txt, '摄像头已打开：(.+?)（(\d+)x(\d+) @')
  Check $cm.Success "从日志里读到了摄像头协商出来的尺寸" `
        "读不出来的话下面那条'报的和编的一致'无从谈起"
  $camW = 0; $camH = 0
  if ($cm.Success) {
      $camW = [int]$cm.Groups[2].Value; $camH = [int]$cm.Groups[3].Value
      "         摄像头那一侧报的是：${camW}x${camH}（$($cm.Groups[1].Value)）"
  }

  # =========================================================================
  # [2] 录出来的那一路是不是摄像头的画面
  # =========================================================================
  ""
  "[2] 录出来的这一路是**摄像头**的画面的三条判据"
  $p = Probe-File $rec
  Check $p.Ok "ffprobe 解出了 $($rec | Split-Path -Leaf)（$(if ($p.Ok) { "h264 $($p.Width)x$($p.Height)" } else { $p.Err })）" `
        "解不出来时先看它是不是**还在写着**被砍掉的 —— 但这一趟是等它自己退的，所以不是那个原因"
  if ($p.Ok) {
      # 判据一：**不是显示器那一档**。这是整个脚本的中心断言，也是变异检验要打红的
      # 那一条（把 source 强制回 display，它就报 2880x1920 或它的缩放档）。
      Check (-not ($p.Width -eq $dispW -and $p.Height -eq $dispH)) `
            "尺寸**不是**显示器那一档（录到 $($p.Width)x$($p.Height)，屏是 ${dispW}x${dispH}）" `
            "相等 = 这一路录的是桌面，--camera 没接上（或者接了又被悄悄退回了）"
      # 判据二：和摄像头自己报的尺寸一致。这条钉的是"报的和编的是同一路"。
      if ($cm.Success) {
          # 主码流可能被画质档位**往下**缩（`主码流按画质档位缩放到 …`），所以判据是
          # "不超过摄像头原生 + 宽高比一致"，不是"逐字节相等"。这台摄像头的原生
          # 1920x1080 正好等于档位上限，于是实际就是相等的 —— 但断言不该依赖这一点。
          $sameRatio = [math]::Abs(($p.Width * $camH) - ($p.Height * $camW)) -le ($camW * 2)
          Check ($p.Width -le $camW -and $p.Height -le $camH -and $sameRatio) `
                "尺寸与摄像头自报的 ${camW}x${camH} 同一路（录到 $($p.Width)x$($p.Height)，宽高比一致）" `
                "比它大 = 录的不是这台摄像头；宽高比不一致 = 两路被混到了一起"
      }
      # 判据三：**竖直方向的尺寸必须跟着摄像头走**。这一条单独列出来是因为"只错在
      # 高度上"正是"源换了、但缩放那条老路还在按显示器算"的症状。
      if ($dispH -gt 0) {
          Check ($p.Height -ne $dispH) `
                "高度也不是显示器那一档（录到 $($p.Height)，屏是 $dispH）"
      }
  }

  # =========================================================================
  # [3] 画面非黑 + 帧数在预算内
  # =========================================================================
  #
  # 尺寸对得上也可能是"一直在编同一帧黑图"。这两条是"它真的在出画"。
  ""
  "[3] 画面有内容、帧数在预算内"
  if ($p.Ok -and $p.Dur -gt 2) {
      $y = Frame-Yavg $rec ([math]::Round($p.Dur - 1.5, 2))
      Check ($y -ge 0) "取到了结尾那一帧的亮度" `
            "取不到 = 这把尺子坏了（见 Frame-Yavg 里那条 `:file=-` 的说明），不是画面黑"
      if ($y -ge 0) {
          Check ($y -gt 5 -and $y -lt 250) `
                "画面有内容（平均亮度 $([math]::Round($y,1))，黑屏约 0、纯白约 255）" `
                "亮度贴边 = 这一路没真的接上（对着镜头盖、或者一直在编空帧）"
      }
  } else {
      Check $false "文件时长够得着做亮度检查（实际 $(if ($p.Ok) { "$([math]::Round($p.Dur,1))s" } else { '文件解不开' })）"
  }
  if ($p.Ok -and $p.Dur -gt 0) {
      # 总时长而不是帧数：`--seconds $Seconds` 到点就停，容差 4 秒（首帧要等设备和
      # 编码器就绪）。短一截 = 中途断了 —— 每一帧单拎出来都正常，只有时长看得出来。
      Check ($p.Dur -ge ($Seconds - 4)) `
            "总时长 $([math]::Round($p.Dur,1))s（要求 $Seconds s，容差 4s）" `
            "短一截 = 中途断了（采集会话没撑住）"
  }

  # =========================================================================
  # [4] 计划报告里说清楚了通道 1 用的是摄像头
  # =========================================================================
  #
  # 用户加通道靠的是**这句话**（`--list-cameras` 报的是设备，不是"这次跑的是谁"）。
  ""
  "[4] 启动报告里说清了这一路是谁"
  Check ($txt -like '*画面来源：*FHD*' -or $txt -like '*画面来源：*Camera*') `
        "「画面来源：」那一行印的是摄像头，不是显卡/显示器" `
        "印成 Intel/Arc/DISPLAY 的话，用户第一眼看到的就是'我没换成功'"
} finally {
  # 收尸：还活着的采集进程一个都不留。它们占着 8098/8100/8553，而"下一次跑的时候
  # 端口被占"报出来的是**下一次**那个脚本起不来 —— 离真正的原因隔着一整个进程的
  # 生命周期。
  foreach ($a in @($script:apps)) {
      if ($a -and $a.P -and -not $a.P.HasExited) {
          try { $a.P.Kill() } catch {}
          try { $a.P.WaitForExit(10000) | Out-Null } catch {}
      }
  }
}

""
"" + ('=' * 60)
if ($failed -eq 0) { "全部通过。日志与录像在 out\cam-e2e\" } else { "失败 $failed 项。" }
exit $failed
