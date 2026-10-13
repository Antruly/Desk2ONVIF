# 子码流端到端：起一次采集（两路 RTSP + HTTP/ONVIF），
#   1) ffprobe/ffmpeg 分别拉主、子两路，确认能解出画面且零报错
#   2) 跑 onvif_soap.py，确认 GetProfiles 报出两个各自独立的 profile
#   3) 打印收尾统计里的逐路数字
#
# 单独写成脚本而不是敲命令，是因为 ffprobe 拉一路不存在的直播流会**永远等下去**
# （RTSP 的 DESCRIBE 没人应答时它不返回），在交互式调用里那等于把整个调用拖到
# 超时，连日志都来不及落盘。
#
# 用法：powershell -ExecutionPolicy Bypass -File tests\rtsp_sub_e2e.ps1
param(
    # **要够长。** 各段串起来大约是：起服务 3s + [1] 13s + [1b] 16s + [2] 2s +
    # [3] 6s + [3.2] 1s + [3b] 6s ≈ 47s。给短了的表现是后半段跑到一半采集自己
    # 到点退出，于是那几段报的是"连不上" —— 看起来像服务坏了，其实只是测试比
    # 程序跑得久。
    #
    # [3]（web_contract.py）从 2s 变成 6s，是因为它最后那一段会**真的调一次
    # `POST /api/reload`**（把采集链拆了重建）再等它重开完 —— 那几秒是必须的，
    # 而且余量要给足：预算在等待期间用尽的话，[3.2] 之后的段会看到"连不上"，
    # 报出来的又是一种看起来像产品坏了的假失败。
    [int]$Seconds = 70,
    [int]$RtspPort = 8554,
    [int]$HttpPort = 8099,
    # Web 控制台的登录凭据。**每次跑之前 cfg.json 都被删掉**，所以服务端
    # 一定是配置文件里的默认值 admin/admin —— 这两个参数就是把同一对凭据
    # 透传给下面两个 Python 探针（它们要先登录才能打 /api/*）。
    [string]$HttpUser = 'admin',
    [string]$HttpPassword = 'admin'
)
$ErrorActionPreference = 'Stop'
# 子进程输出一律按 UTF-8 解。**必须显式写死，不能靠控制台的代码页** ——
# 不设的话 ProcessStartInfo 用的是 Console.OutputEncoding（默认 936），
# 于是程序的 UTF-8 字节被按 GBK 解成乱码，再被 WriteAllText 按 UTF-8 写进日志，
# 变成"双重编码"。症状是日志文件里每一个中文字都不对，而这跟程序本身毫无关系。
$utf8 = New-Object Text.UTF8Encoding $false
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root 'out\rtsp-sub-e2e'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$exe     = Join-Path $root 'build\bin\desk2onvif.exe'
$ffprobe = Join-Path $root 'third_party\ffmpeg\bin\ffprobe.exe'
$ffmpeg  = Join-Path $root 'third_party\ffmpeg\bin\ffmpeg.exe'

# 每次从干净配置开始：上一次测试留下的 gop_seconds 会让这次"一个分片都没有"，
# 而症状看起来像封装器坏了。
Remove-Item (Join-Path $out 'cfg.json') -ErrorAction SilentlyContinue
foreach ($f in 'run.log', 'run.err') {
    Remove-Item (Join-Path $out $f) -ErrorAction SilentlyContinue
}

$failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { "  [OK]   $what" } else { "  [FAIL] $what"; $script:failed++ }
}

# --- 解码判据的两把尺子 ----------------------------------------------------
#
# **一、`-f null` 自己那条时间戳账不算"这一路流有问题"。**
# 这是那道窄门：只有下面这一条正则命中的行被放过，其余**任何一个字**都算致命
# 报错。它量的不是画面，是 muxer 的账本 ——
# `Application provided invalid, non monotonically increasing dts to muxer`。
# 实测（`out\dup-ts-result.txt`、`out\dup-ts3-result.txt`，同一条流同一个时刻）：
#
#   · `-fps_mode passthrough`：解出 126 帧，`dup_frames=0 drop_frames=0`，stderr 空
#   · 默认 vsync（cfr 折算）  ：**只解出 66 帧**，且报 2 条 dts 账
#
# 同一段 5 秒的流，126 与 66 差的是**画面**：默认 vsync 把这一路变动的帧率
# （`adaptive.mode = "throttle"` 在 25 ↔ 12.5 之间跳）折算成恒定帧率时，
# 一半的帧被它自己丢掉了。也就是说：那条 dts 账是**折算器的损失**，
# 不是流的问题 —— 而帧数才是流的问题。所以这里既关掉折算，又把帧数记下来。
#
# **二、帧数只判一个有据可依的下界。** 控制器最快降到 `adaptive.min_fps`，
# 所以任何一条路在 N 秒里至少该出 `min_fps * N / 2` 帧（再留一半余量给
# 起流那几秒）。它抓的是"一个都没解出来"和"半路哑了"这一类，不是帧率精度。
$script:DtsNote = 'Application provided invalid, non monotonically increasing dts to muxer in stream 0: \d+ >= \d+'

function Split-PullErr([string]$Text) {
    $lines = @($Text -split "`r?`n" | Where-Object { $_.Trim() })
    return [pscustomobject]@{
        Notes = @($lines | Where-Object { $_ -match $script:DtsNote })
        Fatal = @($lines | Where-Object { $_ -notmatch $script:DtsNote })
    }
}

# `-progress pipe:1` 每一段都重印一遍全部键，所以取最后一个 `frame=`。
function Get-PullFrames([string]$ProgressText) {
    $n = 0
    foreach ($line in ($ProgressText -split "`r?`n")) {
        if ($line -match '^frame=(\d+)') { $n = [int]$Matches[1] }
    }
    return $n
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

# --- 起采集 ---------------------------------------------------------------
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $exe
# `--web-root` 和 `WorkingDirectory` 都必须显式给，两处都防的是同一件事：
# 被测程序的静态目录默认值是**相对路径** `web/dist`，按**子进程的工作目录**
# 解析，而 ProcessStartInfo 不设 WorkingDirectory 时继承的是**调用方的** cwd。
# 于是从别的目录跑这个脚本（比如 `cd out` 之后再跑），服务照起、推流照推，
# 只有静态产物变成 404 —— 报出来的是 "GET / → 404"、"SPA 回落 → 404" 这种
# 看着像产品坏了的失败，实际是这个脚本自己没站对地方。9 项假失败，一处成因。
$psi.WorkingDirectory = $root
$psi.Arguments = ('--rtsp --rtsp-port {0} --http --http-port {1} --record "{2}" ' +
                  '--seconds {3} --config "{4}" --http-user "{5}" --http-password "{6}" ' +
                  '--web-root "{7}"') -f `
                 $RtspPort, $HttpPort, (Join-Path $out 'main.mp4'), $Seconds,
                 (Join-Path $out 'cfg.json'), $HttpUser, $HttpPassword,
                 (Join-Path $root 'web\dist')
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

# 等端口真的能连上，而不是死等一个拍脑袋的秒数。
foreach ($port in @($RtspPort, $HttpPort)) {
    $deadline = (Get-Date).AddSeconds(20)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
        $c = New-Object Net.Sockets.TcpClient
        try { $c.Connect('127.0.0.1', $port); $c.Close(); $ok = $true; break }
        catch { Start-Sleep -Milliseconds 300 }
    }
    if (-not $ok) { throw "端口 $port 20 秒内没起来" }
}
Start-Sleep -Seconds 3   # 让编码器把首帧和参数集吐出来

# --- 1. 两路都能拉能解 ----------------------------------------------------
"`n[1] 两路 RTSP"

# 期望帧率**从运行中的服务读**，不写死。
#
# 写死 '30/1' 的代价在默认帧率从 30 改成 25 的那一刻兑现了：这一行变成 2 项
# 失败，而"帧率不对"和"默认值改了"在输出里长得**一模一样** —— 假失败比不测
# 更坏，它会教人忽略 [1] 整段。这里断言的是"推出来的帧率和配置一致"，
# 而它与默认值取多少无关。
#
# **第二轮修正（2026-10-09 全量回归踩到）**：把 30 改成 25 还不够。
# ffprobe 的 `r_frame_rate` 是**从包的 pts 间隔反推**出来的（不是 VUI 里那个），
# 所以它量的是"这一路实际每隔多久出一帧"。而 `adaptive.mode = "throttle"` 时
# （`out\rtsp-sub-e2e\cfg.json` 正是），采集循环 `tick % duty == 0` 才出帧 ——
# 实际帧率 = `capture.fps / duty`，配置里那个数是**节拍**而不是**速率**。
# 机器一忙 duty 就 > 1，25/1 变成 25/2，这一行便报红，报的话还是"帧率不对"。
# 证据在 `out\rtsp-sub-e2e\run.err`：「性能不足，自适应降帧：目标 12 fps
# （原 25 fps，每 2 拍出 1 帧）」—— 一切正常，红的是这条断言的前提。
#
# 现在的判据是两个数**之间的关系**：推出来的帧率必须是配置帧率的整数分之一
# （1/1、1/2、…、1/maxDuty），即"跑的还是配置那个节拍，只是被控制器按整数
# 分之一降了速"。算不出整数 => 与配置没有倍数关系（编码器实际跑在 30 而配置
# 写 25 就是这一种）=> 红。关掉自适应时 maxDuty = 1，判据退化成逐字相等。
#
# 这里**没有**再拿 `/api/status` 的 `perf_duty` 去对账，虽然看着更严。原因是
# 两把尺子量的不是一个东西：`r_frame_rate` 按 ffmpeg 的定义是"能表示窗口里
# **所有**时间戳的最低帧率"，也就是窗口内最小间隔的倒数；`perf_duty` 是**当下**
# 那个值。窗口只要跨越一次降帧/升帧，两者必然对不上 —— 实测 sub 那一趟就是
# 窗口里 duty 从 1 荡到 2，r_frame_rate 报 25/1（最小间隔是 1 拍），而窗口两头
# 的 duty 读数都是 2。写对账等于给这条断言装了一个必然误报的引信。
# 所以 duty 只念不判（下面那行），并且把两头读数**打在输出里**（陷阱 91）。
$wantFps = 0
$minFps = 0
$adaptiveMode = ''
try {
    $sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
    $loginBody = @{ user = $HttpUser; password = $HttpPassword } |
                 ConvertTo-Json -Compress
    Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/api/login" -Method Post `
        -Body $loginBody -ContentType 'application/json' -WebSession $sess `
        -UseBasicParsing | Out-Null
    $cfgObj = Invoke-RestMethod -Uri "http://127.0.0.1:$HttpPort/api/config" `
                                -WebSession $sess
    $wantFps = [int]$cfgObj.capture.fps
    $minFps = [int]$cfgObj.adaptive.min_fps
    $adaptiveMode = [string]$cfgObj.adaptive.mode
} catch {
    $wantFps = 0
}
Check ($wantFps -gt 0) "从 /api/config 读到 capture.fps = $wantFps"

# 自适应能降到的下限决定"最多几拍才出一帧"。模式不是 throttle/auto 时控制器
# 把 duty 钉死在 1（见 `AdaptiveController::configure`），此时下面这个上限就是 1，
# 判据退化成"帧率必须与配置逐字相等" —— 那正是"关掉自适应"该有的严格程度。
$maxDuty = 1
if ($adaptiveMode -eq 'throttle' -or $adaptiveMode -eq 'auto') {
    if ($minFps -gt 0 -and $minFps -lt $wantFps) { $maxDuty = [int]($wantFps / $minFps) }
}
Check ($maxDuty -ge 1) `
      "自适应 mode=$adaptiveMode min_fps=$minFps → 最快 $wantFps fps，最慢每 $maxDuty 拍出 1 帧"

# `/api/status` 里的 `perf_duty`。读不到就返回 0（下面的分支会说出来）。
function Get-StatusDuty {
    try {
        $st = Invoke-RestMethod -Uri "http://127.0.0.1:$HttpPort/api/status" `
                                -WebSession $sess -TimeoutSec 5
        return [int]$st.perf_duty
    } catch { return 0 }
}

$probed = @{}
foreach ($pair in @(@('main', 'live'), @('sub', 'live_sub'))) {
    $tok = $pair[0]; $path = $pair[1]
    $url = "rtsp://127.0.0.1:$RtspPort/$path"
    # duty 的两次读数把 ffprobe 那次测量夹在中间，**只为了让下面的读数有个上下文**
    # （为什么不当判据用，见下面那一段）。
    $dutyBefore = Get-StatusDuty
    $r = Invoke-WithTimeout $ffprobe @(
        '-v', 'error', '-rtsp_transport', 'tcp', '-select_streams', 'v:0',
        '-show_entries', 'stream=codec_name,width,height,r_frame_rate',
        '-of', 'default=noprint_wrappers=1', '-i', $url) 15000
    $dutyAfter = Get-StatusDuty
    $kv = @{}
    foreach ($line in ($r.Out -split "`r?`n")) {
        if ($line -match '^(\w+)=(.*)$') { $kv[$Matches[1]] = $Matches[2] }
    }
    $probed[$tok] = $kv
    Check (-not $r.TimedOut) "$tok 能拿到 SDP"
    Check ($kv['codec_name'] -eq 'h264') "$tok codec = $($kv['codec_name'])"
    $dims = "$($kv['width'])x$($kv['height'])"
    Check ([int]$kv['width'] -gt 0 -and [int]$kv['height'] -gt 0) "$tok 分辨率 $dims"
    # 读不到配置时**不冒充"帧率不对"**：量不出来的东西不能报成被测对象的错。
    #
    # `r_frame_rate` 是个约分过的分数（25/2 = 每 2 拍出 1 帧）。`$shift` 就是
    # 把它还原成"几拍出一帧"：与配置的比是整数才说明这一路跑的还是配置那个
    # 节拍，只是被控制器按整数分之一降了速。算不出整数 => `r_frame_rate` 与
    # 配置**没有倍数关系**（比如编码器实际跑在 30 而配置写 25）=> $shift = 0。
    $num = 0; $den = 0
    if ($kv['r_frame_rate'] -match '^(\d+)/(\d+)$') {
        $num = [int]$Matches[1]; $den = [int]$Matches[2]
    }
    $shift = 0
    if ($num -gt 0 -and $den -gt 0 -and $wantFps -gt 0 -and
        ($wantFps * $den) % $num -eq 0) {
        $shift = [int](($wantFps * $den) / $num)
    }
    Check ($shift -ge 1 -and $shift -le $maxDuty) `
          "$tok 帧率 $($kv['r_frame_rate'])，即配置 $wantFps fps 的 1/$shift（自适应上限 1/$maxDuty）"
    # `perf_duty` **只念不判**，这里曾经写成"和 duty 对账"，那是错的：
    # `r_frame_rate` 按 ffmpeg 的定义是"能表示窗口里**所有**时间戳的最低帧率"，
    # 也就是窗口内**最小**间隔的倒数（一堆 pts 间隔的最大公约数），而
    # `perf_duty` 是**当下**那个值。窗口里只要跨越一次降帧/升帧，
    # 两个数就必然对不上：实测 sub 那一趟就是这样 —— 窗口里 duty 从 1 荡到 2，
    # 于是 r_frame_rate = 25/1（最小间隔是 1 拍），而窗口两头的 duty 读数都是 2。
    # 那不是缺陷，是这两把尺子量的根本不是同一个东西。念出来，不判它。
    "         （$tok 测量窗口两头的 perf_duty：$dutyBefore → $dutyAfter；上面那个 1/$shift 是窗口里的**最小**间隔）"

    # `-fps_mode passthrough` 见 [1b] 里 Start-Pull 上面那段说明（同样是
    # 2026-10-09 那次全量回归定下来的）。这里和那里是同一类判据。
    $r2 = Invoke-WithTimeout $ffmpeg @(
        '-v', 'error', '-rtsp_transport', 'tcp', '-i', $url, '-t', '5',
        '-fps_mode', 'passthrough', '-progress', 'pipe:1', '-f', 'null', '-') 25000
    $verdict = Split-PullErr $r2.Err
    $frames = Get-PullFrames $r2.Out
    Check (-not $r2.TimedOut -and $verdict.Fatal.Count -eq 0) `
          "$tok 解 5 秒零报错（致命行 $($verdict.Fatal.Count) 条）"
    Check ($frames -ge ($minFps * 5 / 2)) `
          "$tok 5 秒解出 $frames 帧（下界 $([int]($minFps * 5 / 2))，按 min_fps=$minFps 的一半算）"
    foreach ($n in ($verdict.Fatal | Select-Object -First 3)) { "         [致命] $n" }
    foreach ($n in ($verdict.Notes | Select-Object -First 2)) { "         [记账] $n" }
}
# 两路必须是**两个不同的尺寸**。相同就说明子码流其实没编，只是把主码流
# 又推了一遍 —— 那样"子码流能用"这句话是假的，而表面上一切正常。
Check ($probed['main']['width'] -ne $probed['sub']['width']) `
      "两路尺寸不同：$($probed['main']['width'])x$($probed['main']['height']) vs $($probed['sub']['width'])x$($probed['sub']['height'])"
Check ([int]$probed['sub']['height'] -lt [int]$probed['main']['height']) `
      "子码流更矮（$($probed['sub']['height']) < $($probed['main']['height'])）"

# --- 1b. 并发与重连 -------------------------------------------------------
# **这一段是给 NVR 铺路的。** 单客户端拉通只证明"流是对的"，证明不了"这台
# 服务器能同时伺候几个客户端，也不会被一个中途断开的人搞死" —— 而 NVR 恰恰
# 就是那么用的：多画面网格上同时开好几路、反复开关、超时重连。
#
# 三条并发 + 一次断线重连。任何一条解出来有错，或者服务器在重连之后就哑了，
# 都说明 OnDemandServerMediaSubsession 那边的复用/回收没做对。
"`n[1b] 并发客户端与断线重连"
# `-fps_mode passthrough` **不是**用来压报错的，是用来让"零报错"这句话量的
# 是**这一路流**，而不是 ffmpeg 自己的帧率换算器。
#
# 2026-10-09 全量回归里 [1b] 三条并发全红，三条都报
# `Application provided invalid, non monotonically increasing dts to muxer
# in stream 0: 8 >= 8`（三条各自的数值不同）。现场实验（`out\rtsp-packets\`，
# 同一条流同一个时刻）：
#
#   · `-c copy`（包原样进 muxer，不解码也不折算帧率）  3/3 次 0 条
#   · `-fps_mode passthrough`（解码但不折算帧率）       4/4 次 0 条
#   · 默认 vsync（cfr 折算）                            27 / 3 / 0 / 0 条
#
# 三组拉的是同一条流。所以撞在一起的是 **ffmpeg 把变动的帧率折算成恒定帧率**
# 时落到同一个 1/25 刻度上的那两帧（`-f null` 的输出流时基就是 1/25，
# `-debug_ts` 里看得见），不是 RTP 时间戳 —— 后者在 `-c copy` 下严格递增。
# 而这一路的帧率**本来就该变**：`adaptive.mode = "throttle"` 时控制器会在
# 25 ↔ 12.5 fps 之间来回跳（`run.err` 里那两行「自适应降帧 / 升帧」），
# 默认 vsync 对这种流是有损折算，它报的是折算自己的损失。
#
# 关掉折算之后解出来的还是同一批帧（实测 `frame=` 计数照涨、退出码照常），
# 而多 slice / 花屏那一类**真**故障本来就是走 stderr 报出来的，一个字没被盖掉。
function Start-Pull {
    param([string]$Url, [int]$Secs)
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $ffmpeg
    # `-progress pipe:1`：把解出来的帧数记下来（理由见上面那两把尺子）。
    # `-f null -` 本身不往 stdout 写东西，两件事不打架。
    #
    # ⚠️ 整串**必须**先括号拼好再 `-f`：PowerShell 里 `-f` 比 `+` 结合得紧，
    # `'a' + 'b' -f $x` 是先拿 `'b'` 去 -f（没有占位符，原样返回）再拼 `'a'`，
    # 于是 `{0}` 永远不被替换，ffmpeg 收到的是字面量
    # `-i "{0}"` → `Error opening input file {0}.`。这个坑踩过一次。
    $psi.Arguments = (('-v error -rtsp_transport tcp -i "{0}" -t {1} ' +
                       '-fps_mode passthrough -progress pipe:1 -f null -') -f $Url, $Secs)
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

$mainUrl = "rtsp://127.0.0.1:$RtspPort/live"
$subUrl  = "rtsp://127.0.0.1:$RtspPort/live_sub"

# 三条同时开（两主一子）。同一条路径上的两个客户端走的是**同一个 source**
# （reuseFirstSource=True），所以这一条同时在验"多客户端共享一个编码器"
# 这件事本身没写错 —— 写错的话第二个客户端会拿不到 SDP 或者一直没数据。
$jobs = @(
    @{ Tag = 'A 主'; J = (Start-Pull $mainUrl 8) }
    @{ Tag = 'B 子'; J = (Start-Pull $subUrl  8) }
    @{ Tag = 'C 主'; J = (Start-Pull $mainUrl 8) }
)
foreach ($it in $jobs) {
    $j = $it.J
    if (-not $j.P.WaitForExit(30000)) { try { $j.P.Kill() } catch {}; Check $false "$($it.Tag) 30 秒内没结束" ; continue }
    $v = Split-PullErr $j.Err.Result
    $fr = Get-PullFrames $j.Out.Result
    Check ($j.P.ExitCode -eq 0 -and $v.Fatal.Count -eq 0) `
          "$($it.Tag) 并发拉 8 秒零报错（exit=$($j.P.ExitCode)，致命行 $($v.Fatal.Count) 条，解出 $fr 帧）"
    foreach ($n in ($v.Fatal | Select-Object -First 3)) { "         [致命] $n" }
    foreach ($n in ($v.Notes | Select-Object -First 2)) { "         [记账] $n" }
}

# 断线重连：拉 3 秒**把它杀掉**（模拟 NVR 切画面/网络抖动），再拉一次。
# 第二次必须和第一次一样干净 —— "服务器被一个没打招呼就走的客户端搞坏了"
# 这类问题只在**下一次**连接时才显形，只测一次永远看不到。
$first = Start-Pull $mainUrl 60          # 故意给一个长时长，下面直接杀
Start-Sleep -Seconds 3
try { $first.P.Kill() } catch {}
$first.P.WaitForExit(5000) | Out-Null
$v1 = Split-PullErr $first.Err.Result
Check ($v1.Fatal.Count -eq 0) "中途被杀掉的那一次没报错（致命行 $($v1.Fatal.Count) 条）"
foreach ($n in ($v1.Fatal | Select-Object -First 3)) { "         [致命] $n" }

$second = Start-Pull $mainUrl 5
if (-not $second.P.WaitForExit(25000)) { try { $second.P.Kill() } catch {}; Check $false "重连后 25 秒内没结束" }
else {
    # 判据是 exit=0、没有**致命**行、而且真的解出了画面。三件事都可能单独失败，
    # 而报错行里只印 exit 的话，`exit=0` 看着像通过、实际是 stderr 有字 ——
    # 2026-10-09 那次它就是这么红着过来的，所以这里把三样都摊开写。
    $v2 = Split-PullErr $second.Err.Result
    $fr2 = Get-PullFrames $second.Out.Result
    Check ($second.P.ExitCode -eq 0) "重连后 ffmpeg 正常退出（exit=$($second.P.ExitCode)）"
    Check ($v2.Fatal.Count -eq 0) "客户端断开之后再连仍然零报错（致命行 $($v2.Fatal.Count) 条）"
    Check ($fr2 -ge ($minFps * 5 / 2)) `
          "重连后 5 秒解出 $fr2 帧（下界 $([int]($minFps * 5 / 2))）"
    foreach ($n in ($v2.Fatal | Select-Object -First 3)) { "         [致命] $n" }
    foreach ($n in ($v2.Notes | Select-Object -First 2)) { "         [记账] $n" }
}

# --- 2. ONVIF 的两个 profile ---------------------------------------------
"`n[2] ONVIF GetProfiles"
# 探针输出 UTF-8，而默认控制台是 936 代码页 —— 不设 OutputEncoding 的话
# 全篇中文都是乱码，失败时最需要看的那几行恰好最难读。
#
# 下面三个探针都带 `-X utf8`，这个开关不是多余的：**`-I` 隐含 `-E`**，
# 而 -E 是"忽略一切 PYTHON* 环境变量"，所以那个 PYTHONIOENCODING 在
# -I 下是**死的**。挂在控制台上时看不出来（Python 走 WriteConsoleW，
# 编码是 UTF-16，代码页不参与），可一旦把本脚本的输出重定向到文件或管道，
# Python 就改用本地代码页编码，"通过 57 项"会以乱码落进日志 ——
# 而退出码仍然是 0。`-X` 是命令行开关，不受 -E 影响。
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
$env:PYTHONIOENCODING = 'utf-8'
# `--config` 是被测进程自己那份配置文件，只给 [11e2] 的防抖判据用：那一段
# 比"重复下发同一份 OSD 前后，配置文件动没动"。
# **必须显式传**：探针猜不出被测进程用的是哪一份配置，而猜错时它当场抛异常
# （不是返回一个像"没动过"的值）—— 那是故意的，见探针里 `cfg_stamp` 的注释。
# 留意这里**不能**改成传日志文件：本脚本起的是**裸形态**（`--rtsp --http`），
# 而那个形态**根本不建日志文件**（`Log::set_file` 只在 `cmd_console` 里调）。
& python -I -X utf8 (Join-Path $root 'tests\onvif_soap.py') --host 127.0.0.1 `
    --port $HttpPort --web-user $HttpUser --web-password $HttpPassword `
    --config (Join-Path $out 'cfg.json')
$soap = $LASTEXITCODE
Check ($soap -eq 0) "onvif_soap.py 退出码 $soap"

# --- 3. Web 层契约 --------------------------------------------------------
# 静态产物、/api/config 往返、/api/status 逐路字段。和上面两段共用这一次采集，
# 因为这几类错（chunk hash 变了、字段名写错）都必须对着**真的在跑的后端**才验得出来。
"`n[3] Web 层契约"
& python -I -X utf8 (Join-Path $root 'tests\web_contract.py') --port $HttpPort `
    --web-root (Join-Path $root 'web\dist') `
    --user $HttpUser --password $HttpPassword
$web = $LASTEXITCODE
Check ($web -eq 0) "web_contract.py 退出码 $web"

# --- 3.2 登录契约 ----------------------------------------------------------
# 为什么单独一段、而不是塞进上面那个：web_contract 验的是"静态产物能下来、
# 字段对得上"，它**整段是匿名跑不动的**；而这一段验的恰好相反 —— 哪条路该被
# 拦住、哪条路不该被拦住。
#
# 这个脚本以前没挂在任何地方跑，于是它自己烂掉了没人知道：后来才发现它需要
# **开着 ONVIF** 才能过（第 [9] 段的 /snapshot.jpg 要 Digest 挑战），而漏传
# 时它会以"3 项失败"的样子报出来 —— 看起来像功能坏了。挂在这里正好：本脚本
# 本来就是开着 ONVIF 跑的。
"`n[3.2] 登录契约"
& python -I -X utf8 (Join-Path $root 'tests\web_auth.py') --port $HttpPort `
    --user $HttpUser --password $HttpPassword
$auth = $LASTEXITCODE
Check ($auth -eq 0) "web_auth.py 退出码 $auth"

# --- 3.5 WS-Discovery 往返 -------------------------------------------------
# 从**临时端口**发 Probe（真实 ONVIF 客户端的形状），而不是只听组播 ——
# 只听的话会错过启动时的 Hello，"什么都收不到"和"共存失败"分不出来。
"`n[3b] WS-Discovery"
& python -I -X utf8 (Join-Path $root 'tests\wsd_probe.py') --mode client --seconds 6
$wsd = $LASTEXITCODE
Check ($wsd -eq 0) "wsd_probe.py --mode client 退出码 $wsd"

# --- 收尾 -----------------------------------------------------------------
if (-not $app.WaitForExit(($Seconds + 30) * 1000)) { try { $app.Kill() } catch {} }
[IO.File]::WriteAllText((Join-Path $out 'run.log'), $appOut.Result)
[IO.File]::WriteAllText((Join-Path $out 'run.err'), $appErr.Result)

"`n[4] 收尾统计"
# **两个文件都要读。** 这个程序按级别分流：逐秒日志走 stderr，收尾的汇总块走
# stdout。只读其中一个，这一段就会安静地什么都不打 —— 而"什么都不打"和
# "统计里没有这些行"看起来一模一样，等于这段检查自己废掉了却没人知道。
$txt = @(
    [IO.File]::ReadAllText((Join-Path $out 'run.err'), [Text.Encoding]::UTF8)
    [IO.File]::ReadAllText((Join-Path $out 'run.log'), [Text.Encoding]::UTF8)
) -join "`n"
$hit = @($txt -split "`r?`n" | Where-Object { $_ -match '推流\[|子码流尺寸|统计\s*:|^失败' })
if ($hit.Count -eq 0) { Check $false "收尾统计里一条预期行都没有（日志是不是分流到别处了？）" }
else { $hit | Select-Object -Last 6 | ForEach-Object { "  $_" } }

"`n" + ('=' * 60)
if ($failed -gt 0) { "失败 $failed 项。日志：$out\run.err"; exit 1 }
"全部通过。"

# ⚠️ `exit 0` 不能省：不加 `exit` 结束的 .ps1 不会把 `$LASTEXITCODE` 归零，
# 它留着脚本里最后一个**原生命令**的值 —— 而这个脚本一路在跑 ffmpeg/ffprobe/
# taskkill，其中好几个是故意非零的。见陷阱 89。
exit 0
