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
    # [3] 2s + [3b] 6s。给短了的表现是后半段跑到一半采集自己到点退出，
    # 于是那几段报的是"连不上" —— 看起来像服务坏了，其实只是测试比程序跑得久。
    [int]$Seconds = 55,
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
$probed = @{}
foreach ($pair in @(@('main', 'live'), @('sub', 'live_sub'))) {
    $tok = $pair[0]; $path = $pair[1]
    $url = "rtsp://127.0.0.1:$RtspPort/$path"
    $r = Invoke-WithTimeout $ffprobe @(
        '-v', 'error', '-rtsp_transport', 'tcp', '-select_streams', 'v:0',
        '-show_entries', 'stream=codec_name,width,height,r_frame_rate',
        '-of', 'default=noprint_wrappers=1', '-i', $url) 15000
    $kv = @{}
    foreach ($line in ($r.Out -split "`r?`n")) {
        if ($line -match '^(\w+)=(.*)$') { $kv[$Matches[1]] = $Matches[2] }
    }
    $probed[$tok] = $kv
    Check (-not $r.TimedOut) "$tok 能拿到 SDP"
    Check ($kv['codec_name'] -eq 'h264') "$tok codec = $($kv['codec_name'])"
    $dims = "$($kv['width'])x$($kv['height'])"
    Check ([int]$kv['width'] -gt 0 -and [int]$kv['height'] -gt 0) "$tok 分辨率 $dims"
    Check ($kv['r_frame_rate'] -eq '30/1') "$tok 帧率 $($kv['r_frame_rate'])"

    $r2 = Invoke-WithTimeout $ffmpeg @(
        '-v', 'error', '-rtsp_transport', 'tcp', '-i', $url, '-t', '5',
        '-f', 'null', '-') 25000
    Check (-not $r2.TimedOut -and -not $r2.Err.Trim()) "$tok 解 5 秒零报错"
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
function Start-Pull {
    param([string]$Url, [int]$Secs)
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $ffmpeg
    $psi.Arguments = ('-v error -rtsp_transport tcp -i "{0}" -t {1} -f null -' -f $Url, $Secs)
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
    $err = $j.Err.Result.Trim()
    Check ($j.P.ExitCode -eq 0 -and -not $err) "$($it.Tag) 并发拉 8 秒零报错（exit=$($j.P.ExitCode)）"
    if ($err) { "         $($err -split "`r?`n" | Select-Object -First 3)" }
}

# 断线重连：拉 3 秒**把它杀掉**（模拟 NVR 切画面/网络抖动），再拉一次。
# 第二次必须和第一次一样干净 —— "服务器被一个没打招呼就走的客户端搞坏了"
# 这类问题只在**下一次**连接时才显形，只测一次永远看不到。
$first = Start-Pull $mainUrl 60          # 故意给一个长时长，下面直接杀
Start-Sleep -Seconds 3
try { $first.P.Kill() } catch {}
$first.P.WaitForExit(5000) | Out-Null
Check (-not $first.Err.Result.Trim()) "中途被杀掉的那一次没报错"

$second = Start-Pull $mainUrl 5
if (-not $second.P.WaitForExit(25000)) { try { $second.P.Kill() } catch {}; Check $false "重连后 25 秒内没结束" }
else {
    Check ($second.P.ExitCode -eq 0 -and -not $second.Err.Result.Trim()) `
          "客户端断开之后再连仍然零报错（exit=$($second.P.ExitCode)）"
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
& python -I -X utf8 (Join-Path $root 'tests\onvif_soap.py') --host 127.0.0.1 `
    --port $HttpPort --web-user $HttpUser --web-password $HttpPassword
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
