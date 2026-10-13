# Phase R 的验收：生命周期操作 —— 重启服务 / 恢复出厂。
#
# 这一段验四件在别处验不到的事，每一件都是"进程真的换了一个"级别的：
#
#   [1] 裸形态（`--http` 不带 `--console`）**录制中**重启
#       —— 202 真的发得出去、旧进程退 0、新进程 PID 不同且把 8102 重新绑上、
#          **旧 cookie 打 /api/status 立刻 401**（这是"会话令牌签的是每进程一份的
#          随机密钥"那条设计的实测判据，不是从注释里推的）、重新登录成功、
#          配置文件一字未变
#   [2] 同一个裸形态**没在录制时**重启 —— 这才是"重启服务"的典型场景，
#       而它走的是分段循环里另一条退出条件（`!session_running`）
#   [3] worker 形态：重启换成**退出码 7**，让服务看门狗原地重拉；同时录制被中断
#       但 trailer 是完整的（ffprobe 读得回时长）
#   [3c] 操作日志（审计）：见那一段开头的注释。它盖三件**只有跨进程/跨文件才验得到**
#       的事 —— 剪枝只删我们自己的文件、整份语料里零口令、关掉之后什么都不做。
#   [4] 恢复出厂：形状不对回 400 且什么都不做、对了回 202，然后 `.bak` 是旧值、
#       `config.json` 没了、日志被清空、设备时间偏移归零、进程重启完成；
#       并且 console 形态下重启后绑的是**默认的 8080**（配置文件没了 ⇒ 回到出厂
#       默认），而命令行上的 `--rtsp-port 8564` **照样生效** —— 这一对正是
#       "恢复出厂恢复的是配置文件"那句话的正面证据。
#       **审计那一半也在这一段**：清空前 `logs\audit-*.log` 必须数得出文件来
#       （数不出的话"清空后一个都没有"是句空话，陷阱 60），清空后它们全没了、
#       目录还在、`notes.txt` 还在、而且新进程从那一秒开始写新的审计文件。
#
# **故意不验**：真实的 Windows 服务看门狗（要 `--service install`，一次系统改动，
# 这一段没有授权）、真的海康/大华 NVR（本机没有那台设备）。
#
# 端口：Web 8102 / RTSP 8563（夹具）+ 8564（恢复出厂那一跑的 `--rtsp-port`）
#       + 8108 / 8571（[3c] 用它自己那份审计夹具，理由见那一段）。
# 三个都不是默认端口 —— 这台机器上很可能正跑着一个真的在用 8080/554 的实例，
# 那会让"端口被占"伪装成产品缺陷。**而 [4] 里 8080 恰恰是要紧的那个数**，
# 所以那一段前面有一条"8080 现在空着吗"的前提检查：不空就只报告不判
# （陷阱 87 —— 判据依赖了没写进前提的环境条件时，它只能报告、不能判）。
#
# 日志那一条**不用文件大小判**（陷阱 85 的形状：`after < before` 在"日志被清了"
# 和"这次日志本来就少"两种情况下都为真）。用的是两条各自独立的判据：
#   (a) 恢复出厂前那一行 `设备时间被 NVR 改了`（本脚本自己先造出来的）**在清空后
#       必须找不到** —— 而它清空前必须找得到，否则这条断言是空的；
#   (b) 日志文件的 **CreationTimeUtc 必须晚于**按下恢复出厂的那一刻 ——
#       `Log` 是删除后重新打开的，所以创建时间会跳；没删掉的话它一直是最早那个。
#
# 用法：powershell -ExecutionPolicy Bypass -File tests\lifecycle_check.ps1
param(
    [int]$WebPort = 8102,
    [int]$RtspPort = 8563,
    [int]$ResetRtspPort = 8564,
    # [3c]（操作日志）不与上面几个共用端口：那一段要**自己起两次进程**（--no-audit
    # 一次、正常一次），而 8102 上刚跑过 [1]~[3b] 的三个形态。换一对端口让"端口
    # 还没让出来"这件事从一开始就不参与，而不是每次都得先把 Wait-NoListener 讲清楚。
    [int]$AuditWebPort = 8108,
    [int]$AuditRtspPort = 8571
)
$ErrorActionPreference = 'Stop'

# 子进程输出一律按 UTF-8 解。必须显式写死 —— ProcessStartInfo 默认取
# Console.OutputEncoding（本机是 936），程序的 UTF-8 字节会被按 GBK 解成乱码，
# 再被我们按 UTF-8 写进日志，于是日志里每个中文字都不对，而日志本身"看着完全正常"。
$utf8 = New-Object Text.UTF8Encoding $false
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root 'out\lifecycle'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$exe     = Join-Path $root 'build\bin\desk2onvif.exe'
$ffprobe = Join-Path $root 'third_party\ffmpeg\bin\ffprobe.exe'

$cfgPath  = Join-Path $out 'cfg.json'
$cfgBak   = Join-Path $out 'cfg.json.bak'
$logPath  = Join-Path $out 'desk2onvif.log'
$mp4One   = Join-Path $out 'main.mp4'
$mp4Two   = Join-Path $out 'idle.mp4'
$mp4Work  = Join-Path $out 'worker.mp4'
$mp4Reset = Join-Path $out 'reset.mp4'

# [3c] 那一份。审计目录**故意就是 `$out\logs`** —— 它与 [1]~[3b] 用的是同一个
# `logs` 目录（审计目录 = 配置文件所在目录 + `\logs`，见 `cmd_run` 里那个
# `audit_dir`），所以 [4] 的恢复出厂清空的是**同一个目录**，不需要第二套路径。
$auditDir  = Join-Path $out 'logs'
$auditCfg  = Join-Path $out 'audit-cfg.json'
$mp4Audit  = Join-Path $out 'audit-1.mp4'
$mp4Audit2 = Join-Path $out 'audit-2.mp4'

$apdata = Join-Path $env:APPDATA 'Desk2ONVIF'

# ONVIF 那一份凭据**故意用默认值**：恢复出厂之后夹具被删了，重启起来的进程
# 读的是出厂默认（admin/admin），而 [4] 里要拿它去问设备时间。用一套自定义口令
# 的话，那一步就会变成"在验命令行参数"而不是"在验恢复出厂"。
$script:OnvifUser = 'admin'
$script:OnvifPass = 'admin'

$script:failed = 0
$script:total  = 0
function Check([bool]$ok, [string]$what) {
    $script:total++
    if ($ok) { Write-Host "  [OK]   $what" -ForegroundColor Green }
    else { Write-Host "  [FAIL] $what" -ForegroundColor Red; $script:failed++ }
}
function Section([string]$t) { Write-Host "`n== $t" -ForegroundColor Cyan }
function Note([string]$t) { Write-Host "   . $t" -ForegroundColor DarkGray }
# 只报告、不判：宾语是"这台机器当时是什么状态"，不是产品（陷阱 87）。
function Report([string]$t) { Write-Host "  [--]   $t" -ForegroundColor DarkYellow }

# ---------------------------------------------------------------- 进程
# 起一个子进程并**立刻**开始读它的输出。返回的句柄里带着两个读任务。
#
# 为什么读要立刻开始、而不是等到 WaitForExit：管道只有 4 KB。这个程序每秒往
# stderr 写好几行日志，几秒钟就能把管道填满，然后它就**卡在写 stderr 上** ——
# 不录了、网也不应答了。症状看起来是"产品挂了"，而实际上是我们没读它的输出。
function Start-CapturedExe([string]$File, [string]$ArgLine, [string]$WorkDir = $root) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $File
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
function Start-Captured([string]$ArgLine) { return Start-CapturedExe $exe $ArgLine $root }

# 带硬超时的等待。超时返回 Code=$null 而不是编一个退出码 —— "没在规定时间内退出"
# 和"退了个非零码"是两件不同的事，混成一个数会让人去查错方向。
#
# **`.Result` 必须要一个 `Wait` 兜底。** 这一条是量出来的，不是推的：
# `out\pipe-probe.ps1`（web 8103）测到 **`bInheritHandles=FALSE` 挡不住孙进程
# 拿到我们的管道写端** —— 老进程退出之后管道不关，捕获到的 stderr 尾部就是新进程
# 自己的 `已编码 N 帧` 那几行，要等把新进程杀掉写端才关（`Out.Wait(4000)` 在杀之前
# 是 `False`、杀之后是 `True`）。所以 `.NET` 的 `Task.Result` 在这里**真的会永远阻塞**。
# 产品侧已改成"自己的 std 句柄里有管道时，`STARTF_USESTDHANDLES` + 三个 NULL"
# （见 `src\main.cpp` 的 `restart_process`），那样管道在老进程一退就关；但**兜底
# 不能撤**：它是"卡住的是这个脚本而不是被测程序"的唯一保证，而且产品那句修复
# 一旦被谁改回去，这里必须报出来而不是挂死。
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
    if ($h.Out.Wait(5000)) { try { $outText = $h.Out.Result } catch { } } else { Note '读 stdout 超时（管道的写端没关？）' }
    if ($h.Err.Wait(5000)) { try { $errText = $h.Err.Result } catch { } } else { Note '读 stderr 超时（管道的写端没关？）' }
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
function Run-Exe([string]$File, [string]$ArgLine, [int]$TimeoutMs = 20000) {
    return Wait-Captured (Start-CapturedExe $File $ArgLine $root) $TimeoutMs
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

function Get-ListenerPid([int]$Port) {
    try {
        $c = @(Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction Stop)
        if ($c.Count -gt 0) { return [int]$c[0].OwningProcess }
    } catch { }
    return $null
}
# `Get-NetTCPConnection` 拿不到时返回 $null，而 $null 和不匹配要分开说 —— 前者是
# "这条判据在此刻量不出来"，后者才是"新进程没把端口绑上"。
function Wait-ListenerPid([int]$Port, [int]$WantPid, [int]$TimeoutMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        if ((Get-ListenerPid $Port) -eq $WantPid) { return $true }
        Start-Sleep -Milliseconds 200
    }
    return $false
}
function Wait-NoListener([int]$Port, [int]$TimeoutMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        if ($null -eq (Get-ListenerPid $Port)) { return $true }
        Start-Sleep -Milliseconds 200
    }
    return $false
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
function Wait-FileSize([string]$Path, [int]$MinBytes, [int]$TimeoutMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        if (Test-Path -LiteralPath $Path) {
            $len = (Get-Item -LiteralPath $Path).Length
            if ($len -gt $MinBytes) { return [int64]$len }
        }
        Start-Sleep -Milliseconds 300
    }
    if (Test-Path -LiteralPath $Path) { return [int64](Get-Item -LiteralPath $Path).Length }
    return [int64](-1)
}

# ---------------------------------------------------------------- HTTP
# **不要用 `Invoke-WebRequest` 裸调。** 它在 4xx 上抛异常，而这里有一半的断言
# 要的正是"它回了个 400" —— 靠 catch 里翻 `$_.Exception.Response` 拼出来的状态码
# 只在"真抛了"的时候有，两种路径混在一起写会让 `Status` 的含义随调用点变。
function Invoke-Api {
    # **走裸 `HttpWebRequest`，不走 `Invoke-WebRequest`。** 这一条是量出来的：
    # `out\digest-probe.ps1` 用 `Invoke-WebRequest` 打 `/api/factory-reset` 的 400，
    # `catch [Net.WebException]` 拿到的 `StatusCode` 是**对的（400）**，但
    # `$_.Exception.Response.GetResponseStream()` **读出来是 0 字节** ——
    # 于是"400 的正文是一个 ok:false 的 JSON 对象"这条断言红得像是服务端没给正文。
    # 换裸 `HttpWebRequest`（`try GetResponse() catch WebException { Response }`）
    # 同一个响应读得 **83 字符 / 155 字节**，正文一个字节不少。
    # （`-SkipHttpErrorCheck` 是 PowerShell 7 才有的，这台机器上是 5.1。）
    #
    # 参数表与返回形状 `@{Status; Text}` **一个字都没改**，所以所有调用点原样能用。
    #
    # `$Body` 仍然**故意不声明成 `[string]`**：PowerShell 会把绑到 `[string]` 参数上的
    # `$null` 强制转成**空串**，那样"这一趟没打算给正文"和"给了一段空正文"就分不开。
    # 裸请求下后者已经不会炸（不再有 `ProtocolViolationException`），但这个区分本身
    # 还是要有意义的 —— 见下面那句守卫。
    param([string]$Method, [string]$Url, $Body = $null, $WebSession = $null)
    $req = [Net.HttpWebRequest]::Create($Url)
    $req.Method = $Method
    $req.Proxy = $null
    $req.KeepAlive = $false
    $req.Timeout = 8000
    $req.ReadWriteTimeout = 8000
    # 会话 cookie 从调用方给的那个 `WebRequestSession` 里取。用 `CookieContainer`
    # 而不是自己拼 `Cookie:` 头：那是 `HttpWebRequest` 的正路（拼头在别的
    # .NET 版本上是受限头，会抛 `ArgumentException`）。
    if ($null -ne $WebSession) { $req.CookieContainer = $WebSession.Cookies }
    if ($null -ne $Body -and '' -ne "$Body") {
        $req.ContentType = 'application/json'
        $bytes = [Text.Encoding]::UTF8.GetBytes([string]$Body)
        $req.ContentLength = $bytes.Length
        $st = $req.GetRequestStream()
        $st.Write($bytes, 0, $bytes.Length)
        $st.Close()
    } else {
        # POST 没有正文时显式写 0：请求里就有确定的 `Content-Length: 0` 而不是
        # 什么都没有。GET 上**不写** —— 一个 GET 带着正文长度只会让人怀疑
        # 我们发错了东西。
        if ($Method -ne 'GET' -and $Method -ne 'HEAD') { $req.ContentLength = 0 }
    }
    $resp = $null
    try {
        $resp = $req.GetResponse()
    } catch [Net.WebException] {
        $resp = $_.Exception.Response
    } catch {
        return [pscustomobject]@{ Status = -1; Text = $_.Exception.Message }
    }
    if ($null -eq $resp) { return [pscustomobject]@{ Status = -1; Text = '<连响应对象都没有>' } }
    $sr = New-Object IO.StreamReader($resp.GetResponseStream(), $script:utf8)
    $text = $sr.ReadToEnd()
    $sr.Dispose()
    $status = [int]$resp.StatusCode
    $resp.Close()
    return [pscustomobject]@{ Status = $status; Text = $text }
}
function Get-Json($Text) {
    if (-not $Text) { return $null }
    try { return $Text | ConvertFrom-Json } catch { return $null }
}
# 404/500 之外还有一种失败形态：SPA 兜底回了一份 index.html（200 + HTML）。
# 所以断言的宾语一律是**解析出来的 JSON 字段**，不是 HTTP 200。
function Assert-JsonField($r, [string]$Field, $Value, [string]$What) {
    $j = Get-Json $r.Text
    $ok = ($null -ne $j) -and ($null -ne $j.$Field) -and ([string]$j.$Field -eq [string]$Value)
    Check $ok "$What（HTTP $($r.Status)，$Field=$(if ($null -ne $j) { $j.$Field } else { '<没有 JSON>' })）"
    return $j
}

# 读一个**正被 desk2onvif 打开着**的文件（日志就是这种）。必须用共享读。
#
# 为什么不能用 [IO.File]::ReadAllText：它走的是 FileShare.Read，而我们要读的那个文件
# 是 desk2onvif 用 `fopen(path,"ab")` 一直开着写的 —— 两种共享模式冲突，实测抛
#     The process cannot access the file '...' because it is being used by another process.
# 六种读法在 out\logread.txt 里逐个量过：ReadAllText / ReadAllBytes / [IO.File]::OpenRead
# 三种都抛，而 FileStream(..., FileShare.ReadWrite) 与 Get-Content 能读到内容。
#
# 这里**不吞异常**：调用方要能分清"读失败"和"文件里没有那一行"——它们长得一模一样
# 是这个项目里已经栽过的坑（陷阱 46），而吞掉异常正好把两者合并成同一个空串。
function Read-SharedText([string]$Path) {
    $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $sr = New-Object IO.StreamReader($fs, (New-Object Text.UTF8Encoding $false))
        try { return $sr.ReadToEnd() } finally { $sr.Dispose() }
    } finally { $fs.Dispose() }
}

# ---------------------------------------------------------------- ONVIF Digest
# 这一段是为了 [4] 里那两个 ONVIF 操作。tools 目录里的 `onvif_soap.py` 已经有一份
# 完整的客户端，但它是 python；而这一段只用到两个操作，为此再拉一个解释器进来
# 会让"这一段验的是进程生命周期"这件事被淹没。
function Get-Md5Hex([string]$s) {
    $md5 = [Security.Cryptography.MD5]::Create()
    try {
        $b = [Text.Encoding]::UTF8.GetBytes($s)
        return ([BitConverter]::ToString($md5.ComputeHash($b)) -replace '-', '').ToLowerInvariant()
    } finally { $md5.Dispose() }
}
# 挑战头里 `realm="Desk2ONVIF"` 带引号、`qop=auth` 不带 —— 两种都要认。
function Get-DigestField([string]$header, [string]$name) {
    $m = [regex]::Match($header, $name + '=(?:"([^"]*)"|([^,\s]+))')
    if (-not $m.Success) { return $null }
    if ($m.Groups[1].Success) { return $m.Groups[1].Value }
    return $m.Groups[2].Value
}
function New-SoapEnvelope([string]$Inner) {
    return @"
<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope" xmlns:tds="http://www.onvif.org/ver10/device/wsdl" xmlns:trt="http://www.onvif.org/ver10/media/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <soap:Body>
$Inner
  </soap:Body>
</soap:Envelope>
"@
}
function Send-Soap([string]$Uri, [string]$Body, [string]$AuthHeader = '') {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Body)
    $req = [Net.HttpWebRequest]::Create($Uri)
    $req.Method = 'POST'
    $req.ContentType = 'application/soap+xml; charset=utf-8'
    $req.Timeout = 8000
    $req.ReadWriteTimeout = 8000
    $req.Proxy = $null
    $req.KeepAlive = $false
    if ($AuthHeader) {
        try { $req.Headers.Add('Authorization', $AuthHeader) }
        catch { $req.Headers['Authorization'] = $AuthHeader }
    }
    $req.ContentLength = $bytes.Length
    $st = $req.GetRequestStream()
    $st.Write($bytes, 0, $bytes.Length)
    $st.Close()
    $resp = $null
    try {
        $resp = $req.GetResponse()
    } catch [Net.WebException] {
        $resp = $_.Exception.Response
        if ($null -eq $resp) {
            return [pscustomobject]@{ Status = -1; Text = $_.Exception.Message; Challenge = '' }
        }
    }
    $sr = New-Object IO.StreamReader($resp.GetResponseStream(), $script:utf8)
    $text = $sr.ReadToEnd()
    $sr.Dispose()
    $challenge = ''
    try { $challenge = [string]$resp.Headers['WWW-Authenticate'] } catch { }
    $status = [int]$resp.StatusCode
    $resp.Close()
    return [pscustomobject]@{ Status = $status; Text = $text; Challenge = $challenge }
}
# 匿名先试一次，401 再按挑战算一个摘要重发。
#
# **摘要串一律用 `-f` 拼，不做字符串插值** —— PowerShell 会把 `"$User:$realm"` 里的
# `$User:` 当成"盘符限定的变量名"去解析，报出来的错和真正的原因（少了一段摘要）
# 看起来毫无关系。
function Invoke-Onvif([string]$Uri, [string]$BodyXml) {
    $first = Send-Soap $Uri $BodyXml
    if ($first.Status -eq 200) { return $first }
    if ($first.Status -ne 401) { return $first }
    $ch = $first.Challenge
    $realm = Get-DigestField $ch 'realm'
    $nonce = Get-DigestField $ch 'nonce'
    $qop   = Get-DigestField $ch 'qop'
    if (-not $realm -or -not $nonce) { return $first }
    # ⚠️ **`System.Uri` 没有 `Path` 这个属性。** 真的只有 `AbsolutePath` /
    # `PathAndQuery` / `LocalPath` / `Segments`；而在 StrictMode 关闭时 PowerShell
    # 对**不存在的属性**返回 `$null` 且**一声不吭** —— 于是这一行曾经写成
    # `([Uri]$Uri).Path`，摘要头里就是 `uri=""`，服务端 `parse_authorization` 的
    # `present` 判据（`!username.empty() && !nonce.empty() && !uri.empty() && …`）
    # 直接为假 ⇒ **每一个**摘要都被拒 ⇒ 401。症状是 [4] 段里那四条断言一起红，
    # 而它们红得完全像是"服务端不认摘要"。
    # 实测对照（脚本作用域和函数里都量过）：`cast(.Path) = ''`，
    # `AbsolutePath = '/onvif/device_service'`。
    $uriPath = ([Uri]$Uri).AbsolutePath
    if ([string]::IsNullOrEmpty($uriPath)) {
        # 宁可当场炸掉这一个脚本，也不要再让一个空的 uri 伪装成"服务端拒绝了"。
        throw "取不到 URI 的路径部分（\$Uri = $Uri）：这是**验收脚本自己**的 bug，不是被测程序的问题。"
    }
    $nc = '00000001'
    $cnonce = '0a4f113b'
    $ha1 = Get-Md5Hex ('{0}:{1}:{2}' -f $script:OnvifUser, $realm, $script:OnvifPass)
    $ha2 = Get-Md5Hex ('POST:{0}' -f $uriPath)
    if ($qop) {
        $digest = Get-Md5Hex ('{0}:{1}:{2}:{3}:{4}:{5}' -f $ha1, $nonce, $nc, $cnonce, 'auth', $ha2)
    } else {
        $digest = Get-Md5Hex ('{0}:{1}:{2}' -f $ha1, $nonce, $ha2)
    }
    $parts = @(
        ('username="{0}"' -f $script:OnvifUser)
        ('realm="{0}"' -f $realm)
        ('nonce="{0}"' -f $nonce)
        ('uri="{0}"' -f $uriPath)
        ('response="{0}"' -f $digest)
        'algorithm=MD5'
    )
    if ($qop) {
        $parts += 'qop=auth'
        $parts += ('nc={0}' -f $nc)
        $parts += ('cnonce="{0}"' -f $cnonce)
    }
    return Send-Soap $Uri $BodyXml ('Digest ' + ($parts -join ', '))
}
function Get-SoapInt([string]$Xml, [string]$Tag) {
    $m = [regex]::Match($Xml, "<(?:[A-Za-z0-9]+:)?$Tag>(-?\d+)</(?:[A-Za-z0-9]+:)?$Tag>")
    if (-not $m.Success) { return $null }
    return [int]$m.Groups[1].Value
}
function Get-SoapText([string]$Xml, [string]$Tag) {
    $m = [regex]::Match($Xml, "<(?:[A-Za-z0-9]+:)?$Tag>([^<]*)</(?:[A-Za-z0-9]+:)?$Tag>")
    if (-not $m.Success) { return $null }
    return $m.Groups[1].Value.Trim()
}
# `UTCDateTime` 那一块的六个数字要**从块里取**：同一个报文里 `LocalDateTime` 也
# 带着一个 `Hour`/`Minute`，扁平地找第一个数字会读到一个说不清是哪一块的值。
function Get-DeviceUtc([string]$Xml) {
    $m = [regex]::Match($Xml,
        '<(?:[A-Za-z0-9]+:)?UTCDateTime>(.*?)</(?:[A-Za-z0-9]+:)?UTCDateTime>', 'Singleline')
    if (-not $m.Success) { return $null }
    $blk = $m.Groups[1].Value
    $y  = Get-SoapInt $blk 'Year'
    $mo = Get-SoapInt $blk 'Month'
    $d  = Get-SoapInt $blk 'Day'
    $h  = Get-SoapInt $blk 'Hour'
    $mi = Get-SoapInt $blk 'Minute'
    $s  = Get-SoapInt $blk 'Second'
    if ($null -eq $y -or $null -eq $mo -or $null -eq $d -or
        $null -eq $h -or $null -eq $mi -or $null -eq $s) { return $null }
    return New-Object DateTime($y, $mo, $d, $h, $mi, $s, ([DateTimeKind]::Utc))
}
function New-SetTimeBody([datetime]$Utc) {
    $inner = @"
    <tds:SetSystemDateAndTime>
      <tds:DateTimeType>Manual</tds:DateTimeType>
      <tds:DaylightSavings>false</tds:DaylightSavings>
      <tds:UTCDateTime>
        <tt:Time>
          <tt:Hour>$($Utc.Hour)</tt:Hour>
          <tt:Minute>$($Utc.Minute)</tt:Minute>
          <tt:Second>$($Utc.Second)</tt:Second>
        </tt:Time>
        <tt:Date>
          <tt:Year>$($Utc.Year)</tt:Year>
          <tt:Month>$($Utc.Month)</tt:Month>
          <tt:Day>$($Utc.Day)</tt:Day>
        </tt:Date>
      </tds:UTCDateTime>
    </tds:SetSystemDateAndTime>
"@
    return New-SoapEnvelope $inner
}

function Get-Snapshot([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir)) { return '<目录不存在>' }
    $rows = @()
    foreach ($f in @(Get-ChildItem -LiteralPath $Dir -Recurse -File -Force -ErrorAction SilentlyContinue)) {
        $rel = $f.FullName.Substring($Dir.Length).TrimStart('\')
        $rows += ('{0}|{1}|{2}' -f $rel, $f.Length, $f.LastWriteTimeUtc.ToString('o'))
    }
    return (($rows | Sort-Object) -join "`n")
}

# ---------------------------------------------------------------- 异常兜底
# **这个脚本会拉起真的录制进程，而"脚本自己抛异常"和"测试失败"是两回事。**
# 前者如果就这么走掉，那个 desk2onvif.exe 会一直录下去、一直占着端口；更麻烦的是
# 它**还攥着调用方的 stdout 管道不放**（`Process.Start` 是带句柄继承起的），于是连
# "脚本已经结束"这件事都传不出去，调用方会一直等一个永远不会到的 EOF。
# 实测过：`Invoke-Api` 那个 bug 让脚本在 [1] 段中途抛异常，工具侧整整等满 600 秒
# 才被外力打断，而这期间残留的录制进程一直在跑。
# 所以清理不能只挂在正常结尾上，得挂在异常路径上。
trap {
    Write-Host "`n脚本异常中断：$($_.Exception.Message)" -ForegroundColor Red
    Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray
    Stop-OurExe
    exit 9
}

# ---------------------------------------------------------------- 准备
foreach ($f in @($cfgPath, $cfgBak, $logPath, $mp4One, $mp4Two, $mp4Work, $mp4Reset)) {
    Remove-Item -LiteralPath $f -ErrorAction SilentlyContinue
}
Stop-OurExe
[void](Wait-NoListener $WebPort 5000)

# 夹具：Web 与 RTSP 都开、ONVIF 开、登录开。
#
# **`web.port` 写在文件里，不写在命令行上** —— 这是 [4] 的前提：恢复出厂删掉的
# 正是这份文件，重启起来的进程只能从**默认值**里拿到端口。命令行上带 `--http-port`
# 的话，那个 8080 就会变成"命令行说了算"，而 [4] 里"绑回了 8080"这条断言也就
# 什么都没证明。
#
# `onvif` 的凭据故意用默认值（不写），理由见文件头。
# fps 给 5、码率 512 是为了让这几段录制加起来也不占满一个核。
$json = @"
{
  "web": { "enabled": true, "host": "127.0.0.1", "port": $WebPort, "auth_enabled": true,
           "user": "lifecycle", "password": "d2o-lifecycle-pw" },
  "rtsp": { "enabled": true, "port": $RtspPort },
  "onvif": { "enabled": true },
  "capture": { "quality": "custom", "fps": 5, "bitrate_kbps": 512 }
}
"@
[IO.File]::WriteAllText($cfgPath, $json, $utf8)
$cfgHash = (Get-FileHash -LiteralPath $cfgPath -Algorithm SHA256).Hash

Write-Host 'Desk2ONVIF 生命周期验收（重启服务 / 恢复出厂）' -ForegroundColor White
Note "exe    $exe"
Note "夹具   $cfgPath"
Note "端口   web=$WebPort rtsp=$RtspPort reset-rtsp=$ResetRtspPort"

# ---------------------------------------------------------------- [0] 前提
Section '[0] 前提'
$h = Run-Captured '--help'
Check ($h.Code -eq 0) "--help 退出码 0（实际 $($h.Code)）"
Check ($h.Out -match '--handoff') '--help 提到了 --handoff（自重启要用的那个开关）'
Check ($h.Out -match '--console' -and $h.Out -match '--worker') '--help 里有 --console 与 --worker'

$snapBefore = Get-Snapshot $apdata
Note "%APPDATA%\Desk2ONVIF 快照：$(if ($snapBefore -eq '<目录不存在>') { '不存在' } else { "$(@($snapBefore -split "`n").Count) 个文件" })"

# 8080 是 [4] 唯一一个"要靠默认值"的断言。这台机器上它可能正被别的程序占着 ——
# 那不是产品的错，所以只报告、不判（陷阱 87）。
$pid8080 = Get-ListenerPid 8080
$free8080 = ($null -eq $pid8080)
if ($free8080) { Report 'TCP 8080 现在空着（[4] 里"绑回默认端口"那条断言会真的判）' }
else { Report "TCP 8080 已被 PID $pid8080 占着 —— [4] 里那两条 8080 断言改成只报告" }
$pid8564 = Get-ListenerPid $ResetRtspPort
if ($null -eq $pid8564) { Report "TCP $ResetRtspPort 空着" }
else { Report "TCP $ResetRtspPort 已被 PID $pid8564 占着（[4] 会起不来）" }

# ---------------------------------------------------------------- [1] 裸形态 · 录制中重启
Section '[1] 裸形态：录制中 POST /api/restart'
$o1 = Start-Captured "--http --record `"$mp4One`" --config `"$cfgPath`" --no-wsd"
$oldPid1 = $o1.Proc.Id
Note "旧进程 pid=$oldPid1"

Check (Wait-Http "http://127.0.0.1:$WebPort/api/session" 25000) '网页起来了（/api/session 200）'

# 会话：`-SessionVariable` 拿到的 cookie 就是下面要用来证明"重启换了密钥"的那一份。
$sessOld = $null
try {
    $null = Invoke-WebRequest -Uri "http://127.0.0.1:$WebPort/api/login" -Method POST `
        -ContentType 'application/json' -Body '{"user":"lifecycle","password":"d2o-lifecycle-pw"}' `
        -UseBasicParsing -SessionVariable sessOld -TimeoutSec 8
    Check $true '用夹具口令登录成功'
} catch {
    Check $false "用夹具口令登录成功（$($_.Exception.Message)）"
}
$st = Invoke-Api 'GET' "http://127.0.0.1:$WebPort/api/status" $null $sessOld
Check ($st.Status -eq 200) "带 cookie 的 /api/status 是 200（实际 $($st.Status)）"
$js = Get-Json $st.Text
Check ($null -ne $js -and $js.ticks -ge 0) '/api/status 的正文是 JSON（不是 SPA 兜底的 HTML）'

$len1 = Wait-FileSize $mp4One 20480 15000
Check ($len1 -gt 20480) "录制真的在写盘（main.mp4 = $len1 字节 > 20 KB）"

# --- 重启不能碰配置文件：就近重抓一次哈希 --------------------------------------
#
# 准备阶段（第 539 行）那个 `$cfgHash` 到这里**已经过期了**，而它过期是产品
# **正当**做出来的：Phase AA 第 (h) 项之后，**第一次启动就会把生效值固化进文件**
# （推导出来的序列号、Web 凭据，以及夹具那份手写 JSON 里压根没写的默认段），
# 写它的是 `src/main.cpp:2501-2514` 那个**受守卫**的 `save_config`。而 `$cfgHash`
# 是**任何进程跑之前**量的 —— [1] 这一跑是第一个读这份夹具的，所以它必然已经变过。
#
# 拿准备阶段的哈希去比重启前后的文件，红的会是「尺子的对象过期了」，而报告上它和
# 「重启把配置改了」长得一模一样。所以在**这里**量一次、把原因写成它自己的断言，
# 比让下一个人去猜为什么差。
#
# 判据本身只问一件事：**重启那一下**有没有碰配置文件 —— 重启前一刻量一次、重启
# 之后再量一次，两次相同即通过。
$cfgHashBeforeRestart1 = ''
$cfgHashErr1 = ''
try {
    $cfgHashBeforeRestart1 = (Get-FileHash -LiteralPath $cfgPath -Algorithm SHA256).Hash
} catch {
    # 不吞异常（陷阱 91）：读不到时下面那两条都比不出来，而它们的红会指向
    # "重启改了配置"；真正的原因在这儿，得让它自己报出来。
    $cfgHashErr1 = $_.Exception.Message
}
Check ($cfgHashErr1 -eq '') "重启之前读得到 config.json（读的时候抛了：$cfgHashErr1）"
Check ($cfgHashBeforeRestart1 -ne $cfgHash) `
    '重启之前那份 config.json 已经不是准备阶段那一份了 —— 第一次启动就把生效值固化进去了（Phase AA 第 h 项）'
# **上一条只说明"文件变过"，这一条说明"变出来的是 (h) 承诺的那样东西"**：固化写进去
# 的那几样里，序列号在夹具里**根本没写**（`onvif` 段只有 `enabled`），它是在文件里
# 现推出来的。把启动那一次写盘拿掉，这一条当场变红 —— 而"哈希变了"那一族断言在
# 同一次变异下也会红，但它红得和"文件被别的什么东西改过"分不开。
$serErr1 = ''
$ser1 = ''
try {
    $j1 = Read-SharedText $cfgPath | ConvertFrom-Json
    $ser1 = [string]$j1.onvif.serial_number
} catch {
    $serErr1 = $_.Exception.Message
}
Check ($serErr1 -eq '' -and $ser1 -ne '') `
    "启动之后 config.json 里有了推导出来的序列号（读的时候抛了：$serErr1；读到的是「$ser1」）"

$r = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/restart" '{}' $sessOld
Assert-JsonField $r 'restarting' 'True' 'POST /api/restart 回了 202 + restarting:true' | Out-Null
Check ($r.Status -eq 202) "重启请求的状态码是 202（实际 $($r.Status)）"

$e1 = Wait-Captured $o1 25000
Check ($e1.Code -eq 0) "旧进程退出码 0（不是 1，也不是超时；实际 $($e1.Code)）"
$mPid = [regex]::Match($e1.Out, '已拉起新进程（PID (\d+)）')
Check $mPid.Success '旧进程打出了"已拉起新进程（PID …）"'
$newPid1 = $null
if ($mPid.Success) { $newPid1 = [int]$mPid.Groups[1].Value }
Check ($null -ne $newPid1 -and $newPid1 -ne $oldPid1) "新进程 PID 与旧的**不同**（旧 $oldPid1 / 新 $newPid1）"
$alive = $false
if ($null -ne $newPid1) {
    try { $null = Get-Process -Id $newPid1 -ErrorAction Stop; $alive = $true } catch { }
}
Check $alive '新进程真的在跑（Get-Process 拿得到）'
$bound = $false
if ($null -ne $newPid1) { $bound = Wait-ListenerPid $WebPort $newPid1 15000 }
Check $bound "Web 端口 $WebPort 由**新** PID 绑着（重试那次真的接上了）"

# **这一条是"重启踢会话"的实测判据。** 令牌签的是每进程一份的随机密钥，所以
# 新进程一定认不出旧 cookie —— 而它同时证明了"回话的确实是新进程"。
$stale = Invoke-Api 'GET' "http://127.0.0.1:$WebPort/api/status" $null $sessOld
Check ($stale.Status -eq 401) "拿**重启前**的 cookie 打 /api/status 是 401（实际 $($stale.Status)）"

$relogin = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/login" `
    '{"user":"lifecycle","password":"d2o-lifecycle-pw"}'
Check ($relogin.Status -eq 200) "重启之后用同一份口令重新登录成功（实际 $($relogin.Status)）"

$hash1 = (Get-FileHash -LiteralPath $cfgPath -Algorithm SHA256).Hash
Check ($hash1 -eq $cfgHashBeforeRestart1) `
    '重启那一下没有碰配置文件（重启前后两次哈希相同）'

Stop-OurExe
[void](Wait-NoListener $WebPort 8000)

# ---------------------------------------------------------------- [2] 裸形态 · 没在录制时重启
Section '[2] 裸形态：没在录制时 POST /api/restart'
$o2 = Start-Captured "--http --config `"$cfgPath`" --no-wsd"
$oldPid2 = $o2.Proc.Id
Note "旧进程 pid=$oldPid2"
Check (Wait-Http "http://127.0.0.1:$WebPort/api/session" 25000) '网页起来了（没有 --record 也长驻）'

$sess2 = $null
try {
    $null = Invoke-WebRequest -Uri "http://127.0.0.1:$WebPort/api/login" -Method POST `
        -ContentType 'application/json' -Body '{"user":"lifecycle","password":"d2o-lifecycle-pw"}' `
        -UseBasicParsing -SessionVariable sess2 -TimeoutSec 8
    Check $true '登录成功'
} catch {
    Check $false "登录成功（$($_.Exception.Message)）"
}
$st2 = Invoke-Api 'GET' "http://127.0.0.1:$WebPort/api/status" $null $sess2
Check ($st2.Status -eq 200) "带 cookie 的 /api/status 是 200（实际 $($st2.Status)）"

# --- 重启不能碰配置文件：同样就近重抓一次哈希 ----------------------------------
#
# 与第 [1] 段同源，只是过期发生得更早：`$cfgHash`（第 539 行，任何进程跑之前量的）
# 在 [1] 那一次启动里就作废了，[2] 这里读到的文件是**已经固化过**的那一份。
#
# 判据因此比 [1] 多一条、而且多出来的那条更要紧：**[1] 说"第一次启动要写"，
# 这里说"第二次启动不许写"。** 守卫坏掉时（每次启动都换一个新 inode）只有这条会红
# —— 所以参照片不是准备阶段那份，而是 **[1] 结束那一刻**那份。
$cfgHashBeforeRestart2 = ''
$cfgHashErr2 = ''
try {
    $cfgHashBeforeRestart2 = (Get-FileHash -LiteralPath $cfgPath -Algorithm SHA256).Hash
} catch {
    $cfgHashErr2 = $_.Exception.Message      # 不吞异常，同 [1]（陷阱 91）
}
Check ($cfgHashErr2 -eq '') "重启之前读得到 config.json（读的时候抛了：$cfgHashErr2）"
Check ($cfgHashBeforeRestart2 -eq $hash1) `
    '[2] 这一跑的启动**没有**再写一次 config.json（文件与 [1] 结束那一刻逐字节相同）'

$r2 = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/restart" '{}' $sess2
Assert-JsonField $r2 'restarting' 'True' 'POST /api/restart 回了 202 + restarting:true' | Out-Null

$e2 = Wait-Captured $o2 25000
Check ($e2.Code -eq 0) "旧进程退出码 0（实际 $($e2.Code)）"
$m2 = [regex]::Match($e2.Out, '已拉起新进程（PID (\d+)）')
$newPid2 = $null
if ($m2.Success) { $newPid2 = [int]$m2.Groups[1].Value }
Check ($null -ne $newPid2 -and $newPid2 -ne $oldPid2) "新进程 PID 与旧的**不同**（旧 $oldPid2 / 新 $newPid2）"
$bound2 = $false
if ($null -ne $newPid2) { $bound2 = Wait-ListenerPid $WebPort $newPid2 15000 }
Check $bound2 "Web 端口 $WebPort 由新 PID 绑着"
$stale2 = Invoke-Api 'GET' "http://127.0.0.1:$WebPort/api/status" $null $sess2
Check ($stale2.Status -eq 401) "旧 cookie 是 401（实际 $($stale2.Status)）"

$hash2 = (Get-FileHash -LiteralPath $cfgPath -Algorithm SHA256).Hash
Check ($hash2 -eq $cfgHashBeforeRestart2) `
    '重启那一下没有碰配置文件（重启前后两次哈希相同）'

Stop-OurExe
[void](Wait-NoListener $WebPort 8000)

# ---------------------------------------------------------------- [3] worker 形态
Section '[3] worker 形态：重启 = 退出码 7（由服务看门狗原地重拉）'
$parent = Start-Process -FilePath 'powershell.exe' `
    -ArgumentList '-NoProfile', '-Command', 'Start-Sleep -Seconds 300' `
    -PassThru -WindowStyle Hidden
Note "假父进程 pid=$($parent.Id)"

$o3 = Start-Captured "--worker --parent-pid $($parent.Id) --record `"$mp4Work`" --config `"$cfgPath`" --no-wsd"
Note "worker pid=$($o3.Proc.Id)"
Check (Wait-Http "http://127.0.0.1:$WebPort/api/session" 25000) 'worker 形态的网页起来了'

$sess3 = $null
try {
    $null = Invoke-WebRequest -Uri "http://127.0.0.1:$WebPort/api/login" -Method POST `
        -ContentType 'application/json' -Body '{"user":"lifecycle","password":"d2o-lifecycle-pw"}' `
        -UseBasicParsing -SessionVariable sess3 -TimeoutSec 8
    Check $true 'worker 形态也能登录'
} catch {
    Check $false "worker 形态也能登录（$($_.Exception.Message)）"
}
# 录够 5 秒再点重启：ffprobe 那条断言要的是"录了一段又被打断"，而太短的话
# 时长断言本身没有分辨力（0.2 秒也是"读得回"）。
$lenW = Wait-FileSize $mp4Work 20480 15000
Check ($lenW -gt 20480) "worker 在录制（worker.mp4 = $lenW 字节）"

$r3 = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/restart" '{}' $sess3
Assert-JsonField $r3 'restarting' 'True' 'POST /api/restart 回了 202 + restarting:true' | Out-Null

$e3 = Wait-Captured $o3 25000
Check ($e3.Code -eq 7) "worker 退出码是 7（kExitRestart；实际 $($e3.Code)）"
Check ($e3.Out -match '重启：退出码 7') 'worker 说清了"退出码 7，由服务拉起"'
Check ($e3.Out -notmatch '已拉起新进程') 'worker **没有**自己拉进程（那是 console/裸形态的路）'
Check (Wait-NoListener $WebPort 8000) 'worker 退出后网页端口被放开了（服务看门狗才拉得起来）'

# "录制被中断但 trailer 完整"这条只有 ffprobe 说得清：文件是分段循环收尾写的，
# 而中断路径（`request_stop()`）和正常结束走的是同一段代码。
if (Test-Path -LiteralPath $mp4Work) {
    $fp = Run-Exe $ffprobe "-v error -show_entries format=duration -of default=nw=1:nk=1 `"$mp4Work`""
    $durText = ($fp.Out + $fp.Err).Trim()
    $dur = 0.0
    $parsed = [double]::TryParse($durText, [ref]$dur)
    Check ($fp.Code -eq 0 -and $parsed -and $dur -ge 3.0) `
        "被中断的录制仍是一个完整可读的 MP4（时长 $durText 秒，要 >= 3）"
} else {
    Check $false 'worker.mp4 存在'
}

try { $parent.Kill() } catch { }
Stop-OurExe
[void](Wait-NoListener $WebPort 8000)

# ---------------------------------------------------------------- [3b] 时间设置
# Phase U 的验收行：手动校准 → **重启进程之后偏移还在**（持久化）→ 归零 →
# NTP 对时（正路 `ok`，错路 `bad_reply` 与 `timeout`）。
#
# **这一段用单独一份配置文件**（`$timeCfg`），绝不碰 `$cfgPath` —— [4] 有一段
# 拿 `$cfgHash` 比对"恢复出厂之后 config.json 还是原来那一份"，在这里动它会红在
# 一个和本段毫无关系的地方（`$cfgHash` 是在准备阶段就抓下来的）。
#
# 形态用裸形态（`--http`，不带 `--record`）：这一段和画面无关，而"重启进程"
# 那条路 [1] 已经完整验过一遍；这里要问的是"配置里存的那个偏移，重启之后
# 回不回得来"。
Section '[3b] 时间设置：手动校准 → 重启后还在 → 归零 → NTP'
$timeCfg   = Join-Path $out 'time-cfg.json'
$ntpReady  = Join-Path $out 'ntp-ready.txt'
$ntpScript = Join-Path $root 'tests\ntp_probe.py'
Remove-Item -LiteralPath $timeCfg -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $ntpReady -ErrorAction SilentlyContinue

# `time` 整段写出来，**不依赖默认值** —— 判据里那几个数（`mode` / `offset_s`）
# 一旦哪个默认值改了，靠默认值的夹具会**安静地**换掉被测对象（陷阱 28 的形状）。
# `rtsp.enabled` 关掉：这一段一行 RTSP 都不验，而 [3] 刚刚才放开 Web 端口、
# 没有放开 RTSP 端口 —— 留着它只会多一个"上一段的监听还没走"的失败面。
$timeJson = @"
{
  "web": { "enabled": true, "host": "127.0.0.1", "port": $WebPort, "auth_enabled": true,
           "user": "lifecycle", "password": "d2o-lifecycle-pw" },
  "rtsp": { "enabled": false, "port": $RtspPort },
  "onvif": { "enabled": true },
  "capture": { "quality": "custom", "fps": 5, "bitrate_kbps": 512 },
  "time": { "mode": "manual", "ntp_server": "127.0.0.1", "ntp_interval_s": 3600, "offset_s": 0 }
}
"@
[IO.File]::WriteAllText($timeCfg, $timeJson, $utf8)

function Get-TimeJson($Sess) {
    $r = Invoke-Api 'GET' "http://127.0.0.1:$WebPort/api/time" $null $Sess
    if ($r.Status -ne 200) { return $null }
    return Get-Json $r.Text
}
# 轮询到某个 `last_sync_state`。**判据是那个字段本身**，不是"等了 N 秒"：
# 对时是后台线程做的（`/api/time/sync` 只回 202），要等的是一个状态跃迁，
# 而"多久"只该是超时上限 —— 写成 sleep 固定秒数的话，慢机器上红、快机器上
# 白等，两种都不是产品的问题。
function Wait-TimeState([string]$Want, $Sess, [int]$TimeoutMs = 25000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $last = '<一次都没问到>'
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $j = Get-TimeJson $Sess
        if ($null -ne $j) {
            $last = [string]$j.last_sync_state
            # **成功时也把 `Last` 带上。** 只填在失败分支上的话，`Check` 那句
            # "实际 '$($w1.Last)'" 在绿的时候打出的是 `实际 ''` —— 读日志的人会
            # 以为"这个状态是空的"，而它明明是我们想要的那个值（陷阱 46 的同一族：
            # 一句说不清自己读了什么的诊断，比没有诊断更费时间）。
            if ($last -eq $Want) { return [pscustomobject]@{ Ok = $true; Json = $j; Last = $last } }
        }
        Start-Sleep -Milliseconds 250
    }
    return [pscustomobject]@{ Ok = $false; Json = (Get-TimeJson $Sess); Last = $last }
}
# 假 SNTP 服务器。**绑不上就返回 `$null`**，让调用方明说"这一段没跑" ——
# 127.0.0.1:123 被别的 NTP 服务占着是这台机器的状态，不是产品的缺陷（陷阱 87）。
function Start-NtpProbe([string]$Extra) {
    if (Test-Path -LiteralPath $ntpReady) { Remove-Item -LiteralPath $ntpReady -Force }
    $argLine = "-I -X utf8 `"$ntpScript`" --serve --host 127.0.0.1 --port 123 " +
               "--ready `"$ntpReady`" $Extra"
    $h = Start-CapturedExe 'python' $argLine $root
    # **等 `--ready` 文件，不靠 sleep。** 探针在 bind 成功之后才写它，所以这个
    # 文件出现 = "可以开始打了"。少了这个握手的话，"服务器还没绑上、客户端先
    # 问了一次"会表现成一条 `timeout` —— 那是夹具的竞态，却长得像产品缺陷。
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 8000) {
        if (Test-Path -LiteralPath $ntpReady) { break }
        if ($h.Proc.HasExited) { break }
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $ntpReady)) {
        $r = Wait-Captured $h 4000
        Report "假 SNTP 服务器没能绑上 127.0.0.1:123（$Extra）—— 探针说：$((($r.Out + $r.Err).Trim() -split "`n")[-1])"
        return $null
    }
    Note "假 SNTP 服务器就位（$Extra）"
    return $h
}
# 杀掉探针并把它的日志摊出来。**探针收到几个包是这里唯一的证据来源** ——
# `Start-CapturedExe` 的两个读任务是 `ReadToEndAsync`，进程活着的时候读不到
# 已经写出来的行，所以这个数只能在收尾时拿（判据不靠它，见下面那三条）。
function Stop-NtpProbe($h) {
    if ($null -eq $h) { return }
    # 先杀再收。写成"直接 Wait-Captured"的话探针会正常地一直等下一个包，
    # 于是每一次收尾都要白等满一个超时才轮到自己被杀 —— 三条路就是 15 秒。
    try { $h.Proc.Kill() } catch { }
    $r = Wait-Captured $h 5000
    $lines = @((($r.Out + $r.Err) -split "`n") | Where-Object { $_.Trim() -ne '' })
    $pkts = @($lines | Where-Object { $_ -match '\[ntp-probe\] #' })
    foreach ($l in @($lines | Where-Object { $_ -notmatch '\[ntp-probe\] #' })) { Note $l.Trim() }
    Note "探针一共收到 $($pkts.Count) 个请求"
}

$o5 = Start-Captured "--http --config `"$timeCfg`" --no-wsd"
$oldPid5 = $o5.Proc.Id
Note "时间段进程 pid=$oldPid5"
Check (Wait-Http "http://127.0.0.1:$WebPort/api/session" 25000) '时间段的网页起来了'

$sessT = $null
try {
    $null = Invoke-WebRequest -Uri "http://127.0.0.1:$WebPort/api/login" -Method POST `
        -ContentType 'application/json' -Body '{"user":"lifecycle","password":"d2o-lifecycle-pw"}' `
        -UseBasicParsing -SessionVariable sessT -TimeoutSec 8
    Check $true '时间段登录成功'
} catch {
    Check $false "时间段登录成功（$($_.Exception.Message)）"
}

# --- (a) 手动校准 ---------------------------------------------------------
$t0 = Get-TimeJson $sessT
Check ($null -ne $t0 -and [int]$t0.offset_s -eq 0) `
    "初始偏移是 0（实际 $(if ($null -ne $t0) { $t0.offset_s } else { '<没有 JSON>' })）"
Check ($null -ne $t0 -and [string]$t0.mode -eq 'manual') `
    "初始模式是 manual（实际 $(if ($null -ne $t0) { $t0.mode } else { '<没有 JSON>' })）"

$rCal = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/time" '{"offset_s":-3600}' $sessT
Check ($rCal.Status -eq 200) "POST /api/time 手动校准回 200（实际 $($rCal.Status)）"
Assert-JsonField $rCal 'ok' 'True' '校准回应里 ok:true' | Out-Null

$t1 = Get-TimeJson $sessT
# **要精确相等的那一条在这里**：`/api/time` 的 `offset_s` 是 `DeviceState` 里
# 那个 `int64` 秒数本身，没有任何换算 —— 而下面那条 `device_now - host_now`
# 是在**同一次请求里**读的两次钟，中间隔着几微秒，所以它只能给 ±1 的容忍。
# 把精确的判据放在精确的量上，两者各判各的。
Check ($null -ne $t1 -and [int]$t1.offset_s -eq -3600) `
    "校准后 offset_s **恰好**是 -3600（实际 $(if ($null -ne $t1) { $t1.offset_s } else { '<没有 JSON>' })）"
$delta = $null
if ($null -ne $t1) { $delta = [int64]$t1.device_now - [int64]$t1.host_now }
Check (($null -ne $delta) -and ([Math]::Abs($delta + 3600) -le 1)) `
    "device_now 比 host_now 早 3600 秒（实测差 $delta 秒；±1 是这次请求里两次读钟之间的一拍）"

# `/api/status` 上那个字段叫 **`device_offset_s`**，不叫 `offset_s` —— 它和
# `/ws/stats` 走的是同一个补丁点（`patch_live_counters`），所以这一条同时说明
# 两条路都带上了它。
$stT = Invoke-Api 'GET' "http://127.0.0.1:$WebPort/api/status" $null $sessT
$jsT = Get-Json $stT.Text
Check ($null -ne $jsT -and [int]$jsT.device_offset_s -eq -3600) `
    "GET /api/status 的 device_offset_s 也是 -3600（实际 $(if ($null -ne $jsT) { $jsT.device_offset_s } else { '<没有 JSON>' })）"

$cfgT = Get-Json (Read-SharedText $timeCfg)
Check ($null -ne $cfgT -and [int]$cfgT.time.offset_s -eq -3600) `
    '偏移也写进了配置文件（time.offset_s = -3600）—— 这是"重启之后还在"的根据，不是它的结论'

# --- (b) 重启之后偏移还在 -------------------------------------------------
$rRe = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/restart" '{}' $sessT
Check ($rRe.Status -eq 202) "重启回 202（实际 $($rRe.Status)）"
$e5 = Wait-Captured $o5 25000
Check ($e5.Code -eq 0) "旧进程退出码 0（实际 $($e5.Code)）"
$mPid5 = [regex]::Match($e5.Out, '已拉起新进程（PID (\d+)）')
$newPid5 = $null
if ($mPid5.Success) { $newPid5 = [int]$mPid5.Groups[1].Value }
Check ($null -ne $newPid5 -and $newPid5 -ne $oldPid5) `
    "新进程 PID 与旧的**不同**（旧 $oldPid5 / 新 $newPid5）"
$bound5 = $false
if ($null -ne $newPid5) { $bound5 = Wait-ListenerPid $WebPort $newPid5 15000 }
Check $bound5 "Web 端口 $WebPort 由**新** PID 绑着（不然下面问的是个已经没了的进程）"

# 令牌签的是每进程一份的随机密钥 ⇒ 重启必然踢掉所有会话，所以要重新登录。
# 这一步同时是"回话的确实是新进程"的第二个证据。
$sessT2 = $null
try {
    $null = Invoke-WebRequest -Uri "http://127.0.0.1:$WebPort/api/login" -Method POST `
        -ContentType 'application/json' -Body '{"user":"lifecycle","password":"d2o-lifecycle-pw"}' `
        -UseBasicParsing -SessionVariable sessT2 -TimeoutSec 8
    Check $true '重启之后重新登录成功'
} catch {
    Check $false "重启之后重新登录成功（$($_.Exception.Message)）"
}
$t2 = Get-TimeJson $sessT2
Check ($null -ne $t2 -and [int]$t2.offset_s -eq -3600) `
    "**重启之后偏移还在**（-3600；实际 $(if ($null -ne $t2) { $t2.offset_s } else { '<没有 JSON>' })）"
$jsT2 = Get-Json (Invoke-Api 'GET' "http://127.0.0.1:$WebPort/api/status" $null $sessT2).Text
Check ($null -ne $jsT2 -and [int]$jsT2.device_offset_s -eq -3600) `
    "重启之后 /api/status 的 device_offset_s 也是 -3600"

# --- (c) 归零 -------------------------------------------------------------
$rRes = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/time/reset" '{}' $sessT2
Check ($rRes.Status -eq 200) "POST /api/time/reset 回 200（实际 $($rRes.Status)）"
$t3 = Get-TimeJson $sessT2
Check ($null -ne $t3 -and [int]$t3.offset_s -eq 0) `
    "归零之后 offset_s = 0（实际 $(if ($null -ne $t3) { $t3.offset_s } else { '<没有 JSON>' })）"
# `reset_time()` 清的是三样（偏移 / 同步时刻 / synced 位），只归零偏移那一样
# 会让状态页显示"已同步，偏移 +0 秒"——一个看起来像"NVR 设了个和主机一样的
# 时间"的假象。
Check ($null -ne $t3 -and -not $t3.synced) '归零之后 synced = false（回到了「没被设过」）'
$cfgT3 = Get-Json (Read-SharedText $timeCfg)
Check ($null -ne $cfgT3 -and [int]$cfgT3.time.offset_s -eq 0) `
    '归零也落了盘（不然重启一次就又弹回 -3600）'

# --- (d) NTP 三条路 -------------------------------------------------------
# 判据全部落在**精确相等**上：假服务器回的是 `host + 300`，纪元常数写错一天
# 就是 86400 的倍数差，这个断言会当场抓住；而"偏移是个正数"两个方向都放过。
# 错路那两条各自也是精确的：`--stratum 0`（KoD）→ `bad_reply`；`--silent`
# （收下但永不回包）→ `timeout`。
#
# ⚠️ **"根本不起探针"造不出 `timeout`，造的是 `unreachable`**（实测：没人绑
# 123 时 ICMP 端口不可达 → `WSAECONNRESET`，客户端立刻得到"连地址都拿不到"）。
# 所以 `timeout` 这条唯一确定的造法是 `--silent` 那个黑洞。
#
# 三条都用**同一个** `$sessT2`、都不重启进程，所以设备时间在整段里是连续的
# —— "失败的对时不许改动设备时间"那条断言才有宾语。
$pythonCmd = Get-Command python -ErrorAction SilentlyContinue
if ($null -eq $pythonCmd) {
    Report 'python 不在 PATH 上 —— NTP 那三条（ok / bad_reply / timeout）一条都没跑'
} else {
    $p1 = Start-NtpProbe '--offset 300'
    if ($null -eq $p1) {
        Report 'NTP 三条一条都没跑：假 SNTP 服务器绑不上 127.0.0.1:123'
    } else {
        # 先起探针、再切模式：`NtpSyncer::set_config` 在 manual→ntp 时会**立刻**
        # 排一次对时，顺序反过来的话那一次会打在一个还没绑上的端口上，
        # 得到一个和我们想验的东西无关的 `timeout`。
        $rMode = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/config" `
            '{"time":{"mode":"ntp","ntp_server":"127.0.0.1","ntp_interval_s":3600,"offset_s":0}}' $sessT2
        Check ($rMode.Status -eq 200) "切到 NTP 模式回 200（实际 $($rMode.Status)）"
        $jMode = Get-Json $rMode.Text
        $applied = @()
        if ($null -ne $jMode) { $applied = @($jMode.applied) }
        $pending = @()
        if ($null -ne $jMode) { $pending = @($jMode.pending) }
        # `applied` / `pending` 是**差分**，不是契约（`applied` 里出现什么取决于
        # 这一次请求到底改了什么）。所以正的那一半挑一个这次确实改了的字段，
        # 反的那一半才是真正的不变量：`time.*` 是热字段，**任何一个都不许进
        # `pending`** —— 进了就意味着"界面上说存好了，实际要重启才有"。
        Check ($applied -contains 'time.mode') "time.mode 进了 applied（这次确实改了它）"
        $timePend = @($pending | Where-Object { $_ -like 'time.*' })
        Check ($timePend.Count -eq 0) `
            "pending 里一个 time.* 都没有（实际 $($timePend -join ', ')）—— 时间是热字段"

        $rSync = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/time/sync" '{}' $sessT2
        Check ($rSync.Status -eq 202) "POST /api/time/sync 回 202（实际 $($rSync.Status)）"
        $w1 = Wait-TimeState 'ok' $sessT2 25000
        Check $w1.Ok "对时状态变成 ok（实际 '$($w1.Last)'）"
        if ($w1.Ok) {
            $j1 = $w1.Json
            Check ([int]$j1.last_sync_offset_s -eq 300) `
                "last_sync_offset_s **恰好**是 300（实际 $($j1.last_sync_offset_s)；差一天就是 86400 的倍数）"
            Check ([int]$j1.last_sync_stratum -eq 2) `
                "last_sync_stratum = 2（探针自报的层数，实际 $($j1.last_sync_stratum)）"
            Check ([int]$j1.offset_s -eq 300) `
                "设备时间真的被挪了 300 秒（offset_s = $($j1.offset_s)）"
            # NTP 得来的偏移**不进配置**（`ntp_syncer` 从不写 config）：界面上
            # "在用的那个"和"存的那个"必须是两个字段，这一条盯的就是这件事。
            Check ([int]$j1.configured_offset_s -eq 0) `
                "configured_offset_s 仍是 0（NTP 的偏移只进 DeviceState，不进配置）"
        } else {
            Check $false 'ok 那一路的四个数没读到（状态没变成 ok，上面那条已经报过了）'
        }

        # NTP 模式下手动校准必须被拒 —— 并且**要在回应里说清为什么**。
        # 静默接受的话，用户设完过一小时被下一次对时覆盖，那才是最难查的那种。
        $rRefuse = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/time" '{"offset_s":60}' $sessT2
        Check ($rRefuse.Status -eq 400) "NTP 模式下手动校准被拒（400；实际 $($rRefuse.Status)）"
        $jRefuse = Get-Json $rRefuse.Text
        Check ($null -ne $jRefuse -and -not $jRefuse.ok -and "$($jRefuse.error)" -match 'NTP') `
            "拒绝的理由里点明了 NTP 模式（实际 '$(if ($null -ne $jRefuse) { $jRefuse.error } else { '<没有 JSON>' })'）"
        Stop-NtpProbe $p1

        # 错路一：KoD（Stratum=0）。
        $p2 = Start-NtpProbe '--stratum 0'
        if ($null -eq $p2) {
            Report 'bad_reply 那一路没跑：假服务器第二次没绑上（UDP 端口没来得及释放？）'
        } else {
            $null = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/time/sync" '{}' $sessT2
            $w2 = Wait-TimeState 'bad_reply' $sessT2 25000
            Check $w2.Ok "KoD（Stratum=0）判成 bad_reply（实际 '$($w2.Last)'）"
            if ($w2.Ok) {
                Check ("$($w2.Json.last_sync_error)" -ne '') `
                    "bad_reply 带了一句人话（实际 '$($w2.Json.last_sync_error)'）"
                # **失败的对时不许改动设备时间** —— 这一条是上面那些成功判据的
                # 反面同伴：只有它绿，"offset_s == 300"才说明那是那一次**成功**
                # 的对时干的，而不是某个别的东西顺手写进去的。
                Check ([int]$w2.Json.offset_s -eq 300) `
                    "对时失败之后设备时间没被动过（offset_s 仍是 $($w2.Json.offset_s)）"
            }
            Stop-NtpProbe $p2
        }

        # 错路二：`--silent` 黑洞（收包、不回）。这是 `timeout` 唯一确定的造法。
        $p3 = Start-NtpProbe '--silent'
        if ($null -eq $p3) {
            Report 'timeout 那一路没跑：假服务器第三次没绑上'
        } else {
            $null = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/time/sync" '{}' $sessT2
            $w3 = Wait-TimeState 'timeout' $sessT2 30000
            Check $w3.Ok "黑洞服务器（收下不回）判成 timeout（实际 '$($w3.Last)'）"
            if ($w3.Ok) {
                Check ([int]$w3.Json.offset_s -eq 300) `
                    "超时之后设备时间同样没被动过（offset_s 仍是 $($w3.Json.offset_s)）"
            }
            Stop-NtpProbe $p3
        }
    }
}

Stop-OurExe
[void](Wait-NoListener $WebPort 8000)

# ---------------------------------------------------------------- [3c] 操作日志（审计）
#
# 这一段盖三件**只有跨进程、跨文件才验得到**的事：
#
#   (a) **剪枝只删我们自己的文件，而且 `--audit-keep-days` 真的在起作用。**
#       手工种四个审计文件 + 一个**绝不能被删**的 `notes.txt`：
#         audit-2025-01-01.log / audit-2025-01-02.log   —— 该被剪掉
#         audit-‹今天-30 天›.log                        —— 该被剪掉（理由见下）
#         audit-‹今天›.log（里面有一行标记）            —— 必须活着，且要被追加
#         notes.txt                                     —— 任何清理都不许碰
#       **「30 天前」那一份是刻意加的，不是凑数**：默认留存是 180 天，只种 2025 年那
#       两份的话，「它们没了」在 180 天和 1 天两种设置下**都成立** —— 那条断言于是
#       一次都没验到 `--audit-keep-days`（陷阱 85 的形状：怎么都绿的断言等于没验）。
#       30 天前那一份在 180 天下**必须活着**、在 1 天下**必须死掉**，它才是那个参数
#       真的生效了的证据。
#
#   (b) **整份审计语料里零口令。** 这是「口令永不进审计」那条例律唯一的执行者 ——
#       只记字段名不记值，写在代码里是一句注释，写在这里才是牙齿。为了让它**不是
#       空转**，正面控制串（`config.set` / `capture.bitrate_kbps` / `onvif.password` /
#       `login.fail`）必须**找得到**：否则「找不到口令」和「根本没读到文件」在报告
#       上一模一样（陷阱 46 / 91 那一族）。
#
#   (c) **是追加而不是覆盖。** 种下的那一行必须还在，且同一份文件里多了这一跑写的行。
#
# 这一跑不共用 8102 / 8563：它要自己起**两次**进程（`--no-audit` 一次、正常一次），
# 而那两个端口上刚跑完 [1]~[3b] 的三个形态。换一对端口，让「端口还没让出来」这件事
# 从一开始就不参与，而不是每次都得先把 Wait-NoListener 讲清楚。
Section '[3c] 操作日志（审计）'

# 口令值都带一段随机后缀：它们要**在整份语料里零命中**，而共用夹具那个 `admin`
# 会和合法的 `web:admin` 身份串撞上，撞上之后那条断言就分不出真假了。
$auditUser    = 'audituser'
$auditWebPw   = 'Audit-Web-Pw-7c31'
$auditOnvifPw = 'Audit-Onvif-Pw-9d42'
$auditNewPw   = 'Audit-Onvif-Pw-NEW-4f27'
$auditWrongPw = 'Audit-Wrong-Pw-5e13'

Note "审计夹具 $auditCfg（Web $AuditWebPort / RTSP $AuditRtspPort / 审计目录 $auditDir）"

$auditJson = @"
{
  "web": { "enabled": true, "host": "127.0.0.1", "port": $AuditWebPort, "auth_enabled": true,
           "user": "$auditUser", "password": "$auditWebPw" },
  "rtsp": { "enabled": true, "port": $AuditRtspPort },
  "onvif": { "enabled": true, "password": "$auditOnvifPw" },
  "capture": { "quality": "custom", "fps": 5, "bitrate_kbps": 512 }
}
"@
[IO.File]::WriteAllText($auditCfg, $auditJson, $utf8)

foreach ($f in @($mp4Audit, $mp4Audit2)) { Remove-Item -LiteralPath $f -ErrorAction SilentlyContinue }

# ---- 清场：上一轮（[1]~[3b]，审计默认开着）留下的文件会让下面的计数说不清 -----
Remove-Item -LiteralPath $auditDir -Recurse -Force -ErrorAction SilentlyContinue
Check (-not (Test-Path -LiteralPath $auditDir)) `
    '清场：上一轮留下的审计目录已删掉（不然下面数文件数就说不清）'

$today      = [DateTime]::Now.ToString('yyyy-MM-dd')
$midDay     = [DateTime]::Now.AddDays(-30).ToString('yyyy-MM-dd')
$auditToday = Join-Path $auditDir "audit-$today.log"
$notesPath  = Join-Path $auditDir 'notes.txt'
$markerLine = '【种下的标记行】启动前就已经在这里了'

[void][IO.Directory]::CreateDirectory($auditDir)
[IO.File]::WriteAllText($notesPath, "这不是审计日志。任何清理都不许删它。`n", $utf8)
foreach ($d in @('2025-01-01', '2025-01-02', $midDay)) {
    [IO.File]::WriteAllText((Join-Path $auditDir "audit-$d.log"),
        "$d 00:00:00 | 0.0.0.0 | planted | planted | 种下的老文件`n", $utf8)
}
# 今天这一份同时是「追加而不是覆盖」的证据：它在这一跑之前就存在，且里面有一行
# **只有它才有的**标记。
[IO.File]::WriteAllText($auditToday,
    "$today 00:00:01 | 0.0.0.0 | planted | planted | $markerLine`n", $utf8)

$planted = @(Get-ChildItem -LiteralPath $auditDir -File | Sort-Object Name |
             ForEach-Object { $_.Name })
Check ($planted.Count -eq 5) `
    "种下了 5 个文件（今天的 + 三个该被剪的 + notes.txt；实际 $($planted.Count) 个：$($planted -join '、')）"

# ---- A：`--no-audit` 时**什么都不做**（连目录都不建、更不剪枝）-----------------
# `AuditLog::open()` 在 enabled 为假时直接 return，剪枝压根不跑。所以这一跑之后
# 目录里必须**一字未动**。
$oA = Start-Captured "--console --record `"$mp4Audit`" --config `"$auditCfg`" --no-wsd --no-audit"
$sessA0 = $null
if (Wait-Http "http://127.0.0.1:$AuditWebPort/api/session" 25000) {
    Check $true "关掉审计的那一跑也起得来（网页 $AuditWebPort 有应答）"
    $loginBody = '{"user":"' + $auditUser + '","password":"' + $auditWebPw + '"}'
    try {
        $null = Invoke-WebRequest -Uri "http://127.0.0.1:$AuditWebPort/api/login" -Method POST `
            -ContentType 'application/json' -Body $loginBody `
            -UseBasicParsing -SessionVariable sessA0 -TimeoutSec 8
    } catch {
        Check $false "关掉审计那一跑登录成功（$($_.Exception.Message)）"
    }
    $rA = Invoke-Api 'GET' "http://127.0.0.1:$AuditWebPort/api/audit?days=7&limit=500" $null $sessA0
    Check ($rA.Status -eq 200) "`--no-audit` 时 /api/audit 照样应答 200（实际 $($rA.Status)）"
    $jA0 = Get-Json $rA.Text
    Check ($null -ne $jA0 -and $jA0.ok -eq $true) '正文是 JSON 且 ok:true（读得到，不是"读不出来"）'
    Check ($null -ne $jA0 -and $jA0.enabled -eq $false) `
        'enabled:false —— 界面据此画"没在记"那句话（而不是"没发生过事"）'
    # **这里故意不判 count == 0。** `read()` 里那道 `if (enabled)` 只包住了目录存在
    # 性检查，关掉审计之后**照样会去读已有的文件** —— 断言"关掉就没有记录"是把
    # "没在写"和"没有历史文件"混为一谈，而这一跑的手边恰好就有历史文件。
} else {
    Check $false "关掉审计的那一跑起得来（网页 $AuditWebPort 25 秒内没有应答）"
}
Stop-OurExe
Check (Wait-NoListener $AuditWebPort 8000) "关掉审计那一跑的网页端口让出来了（$AuditWebPort）"

$afterA = @(Get-ChildItem -LiteralPath $auditDir -File | Sort-Object Name |
            ForEach-Object { $_.Name })
Check (($afterA -join '|') -eq ($planted -join '|')) `
    "`--no-audit` 之后审计目录**一字未动**（没新建今天的文件、也没剪枝；现在 $($afterA.Count) 个）"
Check (Test-Path -LiteralPath (Join-Path $auditDir 'audit-2025-01-01.log')) `
    "`--no-audit` 时**没有剪枝**（2025 年那份还在 —— open() 在 enabled 为假时直接返回）"

# ---- B：正常跑一次，`--audit-keep-days 1` -------------------------------------
# **`--audit` 是显式写上的，不是冗余。** `--no-audit` 那一趟会把 `log.audit=false`
# 折进内存里的 `cfg`（`main.cpp:1911`，为的是 `GET /api/config` 报生效值），而那份
# `cfg` 只要被写过一次盘就会把 false 留在文件里 —— 于是 B 若只写 `--audit-keep-days 1`，
# 它到底记不记就取决于 A 有没有恰好存过盘。这一条断言（以及整段 B）不该有那种前提。
$oB = Start-Captured "--console --record `"$mp4Audit2`" --config `"$auditCfg`" --no-wsd --audit --audit-keep-days 1"
Check (Wait-Http "http://127.0.0.1:$AuditWebPort/api/session" 25000) `
    "审计那一跑起得来（网页 $AuditWebPort 有应答）"

# 剪枝发生在 `open()` 里、也就是进程刚起来的那一刻，所以这四条不用等任何东西。
Check (-not (Test-Path -LiteralPath (Join-Path $auditDir 'audit-2025-01-01.log'))) `
    '剪枝：audit-2025-01-01.log 没了'
Check (-not (Test-Path -LiteralPath (Join-Path $auditDir 'audit-2025-01-02.log'))) `
    '剪枝：audit-2025-01-02.log 没了'
Check (-not (Test-Path -LiteralPath (Join-Path $auditDir "audit-$midDay.log"))) `
    "剪枝：audit-$midDay.log（30 天前）也没了 —— **这一条才证明 `--audit-keep-days 1` 真的生效**（默认 180 天下它必须活着）"
Check (Test-Path -LiteralPath $notesPath) `
    '剪枝：**没删错** —— notes.txt 还在（只删名字匹配 audit-YYYY-MM-DD.log 的）'
Check (Test-Path -LiteralPath $auditToday) '剪枝：今天的文件本来就在，剪枝不该动它'

$sessB = $null
$loginBody = '{"user":"' + $auditUser + '","password":"' + $auditWebPw + '"}'
try {
    $null = Invoke-WebRequest -Uri "http://127.0.0.1:$AuditWebPort/api/login" -Method POST `
        -ContentType 'application/json' -Body $loginBody `
        -UseBasicParsing -SessionVariable sessB -TimeoutSec 8
    Check $true '审计那一跑：登录成功（要留下一条 login.ok）'
} catch {
    Check $false "审计那一跑：登录成功（$($_.Exception.Message)）"
}

# **只错这一次。** 登录失败按 IP 限流（5 次 / 300 秒），第 6 次在写审计**之前**
# 就回 429 了 —— 连发几次的话，多出来的那几次既不会多一条 `login.fail`，也不会
# 说明限流坏了，只会让这段读起来像在赌限流的窗口。
$badBody = '{"user":"' + $auditUser + '","password":"' + $auditWrongPw + '"}'
$rBad = Invoke-Api 'POST' "http://127.0.0.1:$AuditWebPort/api/login" $badBody
Check ($rBad.Status -eq 401) "错口令登录回 401（实际 $($rBad.Status)）"

# 一次**会改口令**的配置改动：用户点名的三件事（改名 / 改码率 / 对时）里，
# 这是唯一一条 detail 里**必须只出现字段名、不能出现值**的 —— 于是它同时是
# 下面那条"零口令"断言的**取材现场**。
$patchBody = '{"capture":{"quality":"custom","bitrate_kbps":3000},"onvif":{"password":"' +
             $auditNewPw + '"}}'
$rPatch = Invoke-Api 'POST' "http://127.0.0.1:$AuditWebPort/api/config" $patchBody $sessB
Check ($rPatch.Status -eq 200) "改配置回 200（实际 $($rPatch.Status)）"
$jPatch = Get-Json $rPatch.Text
Check ($null -ne $jPatch -and @($jPatch.applied) -contains 'onvif.password') `
    '改口令进了 applied（ONVIF 身份是热生效的）'
Check ($null -ne $jPatch -and @($jPatch.pending) -contains 'capture.bitrate_kbps') `
    '改码率进了 pending（码率要重启才生效 —— 陷阱 51）'

# ---- 读回来 ---------------------------------------------------------------
$rAudit = Invoke-Api 'GET' "http://127.0.0.1:$AuditWebPort/api/audit?days=7&limit=500" $null $sessB
Check ($rAudit.Status -eq 200) "GET /api/audit 是 200（实际 $($rAudit.Status)）"
$jA = Get-Json $rAudit.Text
Check ($null -ne $jA) '/api/audit 的正文是 JSON（不是 SPA 兜底的 HTML）'

if ($null -eq $jA) {
    Check $false '拿不到审计 JSON，下面那几条没法判'
} else {
    Check ($jA.ok -eq $true) 'ok:true（读到了）'
    Check ($jA.enabled -eq $true) 'enabled:true（这一跑在记）'
    Check ([int]$jA.keep_days -eq 1) `
        "keep_days 回报的是 1（实际 $($jA.keep_days)）—— 顺带证明 --audit-keep-days 是**带值的**选项（陷阱 27：漏进 kFlagNames 就会被吞掉）"
    Check (([string]$jA.dir).Replace('/', '\').TrimEnd('\') -ieq $auditDir.TrimEnd('\')) `
        "dir 指向那份夹具的 logs（实际「$($jA.dir)」）"
    Check ([int]$jA.count -gt 0) "读到 $($jA.count) 条"

    $E = @($jA.entries)

    # 每一条都把 `@()` 包上：**只有一条命中时 `Where-Object` 回来的不是数组**，
    # 那时 `.Count` 在 PS 5.1 上读的是……什么都没有（`$null.Count` 静默为 0）。
    $startup = @($E | Where-Object {
        $_.action -eq 'startup' -and $_.detail -match 'console' -and $_.detail -match "网页 $AuditWebPort" })
    Check ($startup.Count -ge 1) `
        "有这一跑的 startup 行（细节里写着 console 形态 + 网页 $AuditWebPort，于是它不可能是上一跑留下的）"

    $loginOk = @($E | Where-Object { $_.action -eq 'login.ok' -and $_.identity -eq "web:$auditUser" })
    Check ($loginOk.Count -ge 1) "有 login.ok，身份是 web:$auditUser"

    $loginFail = @($E | Where-Object { $_.action -eq 'login.fail' })
    Check ($loginFail.Count -ge 1) "有 login.fail（$($loginFail.Count) 条）"
    if ($loginFail.Count -ge 1) {
        Check ($loginFail[0].source -eq '127.0.0.1') "login.fail 的来源是 127.0.0.1（$($loginFail[0].source)）"
        Check ($loginFail[0].identity -eq "web:$auditUser") `
            "login.fail 记的是**提交上来的那个用户名**（$($loginFail[0].identity)）"
    }

    $cfgSet = @($E | Where-Object { $_.action -eq 'config.set' })
    Check ($cfgSet.Count -ge 1) "有 config.set（$($cfgSet.Count) 条）"

    # 按**内容**挑出这一趟那一条，而不是取 `[0]`："最新的一条就是我的"依赖
    # "这一跑里 `config.set` 只可能有一条"，而那句前提没有任何东西在守 ——
    # 多出一条（启动时那次落盘、或者别的什么）就会让下面这几条去判一条不相干的记录。
    $mine = @($cfgSet | Where-Object { $_.detail -match 'capture\.bitrate_kbps' })
    Check ($mine.Count -ge 1) "其中一条记的正是这次改的码率字段"
    if ($mine.Count -ge 1) {
        $c = $mine[0]
        Check ($c.source -eq '127.0.0.1') "config.set 的来源是 127.0.0.1（$($c.source)）"
        Check ($c.identity -eq "web:$auditUser") "config.set 的身份是 web:$auditUser（$($c.identity)）"
        Check ($c.detail -match 'onvif\.password') `
            "config.set 的细节里有 onvif.password **这个字段名**（$($c.detail)）"
    }

    # 口令纪律单判一条比"那一组里没泄漏"弱得多，所以对着**整组** `config.set` 判。
    $leak = @($cfgSet | Where-Object { $_.detail -match [regex]::Escape($auditNewPw) })
    Check ($leak.Count -eq 0) `
        "任何一条 config.set 的细节里都没有那个新口令的值（只记字段名，不记值；$($cfgSet.Count) 条全查过）"
}

# ---- 整份语料：正面控制串必须找得到，口令必须零命中 ---------------------------
# 读法用 `Read-SharedText`：今天的文件**正被进程用 `fopen(...,"ab")` 开着**，
# `[IO.File]::ReadAllText` 会在它上面抛共享冲突，而 `catch` 把它吞成空串之后，
# 「我读不到」和「里面没有」报的是同一句话（陷阱 91，`out\logread.txt` 有实测）。
$corpus = ''
$corpusErr = ''
try {
    foreach ($f in @(Get-ChildItem -LiteralPath $auditDir -File -Filter 'audit-*.log')) {
        $corpus += (Read-SharedText $f.FullName)
    }
} catch {
    $corpusErr = $_.Exception.Message
}

if ($corpusErr -ne '') {
    Check $false "整份审计语料读得出来（读的时候抛了：$corpusErr）"
} else {
    Note "审计语料 $($corpus.Length) 个字符"

    # 正面控制。**没有这几条，下面那四条"零命中"就是空转** —— 一个读失败（或者
    # 读错了目录）返回的空串，会让所有"找不到"的断言全部通过（陷阱 85 / 91）。
    foreach ($p in @('config\.set', 'capture\.bitrate_kbps', 'onvif\.password',
                     'login\.fail', 'login\.ok', 'startup',
                     [regex]::Escape($markerLine))) {
        Check ($corpus -match $p) "语料里找得到 $p（正面控制）"
    }

    # 负面：**四个真口令值，一个都不许出现**。值本身不打印（与产品那条纪律同源：
    # 口令不落进任何一份日志 —— 包括这一份测试报告）。
    $secretProbes = @(
        @{ Name = '网页口令';               Value = $auditWebPw },
        @{ Name = 'ONVIF 口令';             Value = $auditOnvifPw },
        @{ Name = '这次改过去的新口令';     Value = $auditNewPw },
        @{ Name = '登录失败那次填的错口令'; Value = $auditWrongPw }
    )
    foreach ($s in $secretProbes) {
        Check (-not ($corpus -match [regex]::Escape($s.Value))) `
            "语料里找不到$($s.Name)（口令永不进审计）"
    }
}

Stop-OurExe
Check (Wait-NoListener $AuditWebPort 8000) "审计那一跑的网页端口让出来了（$AuditWebPort）"
Check (Wait-NoListener $AuditRtspPort 8000) "审计那一跑的 RTSP 端口让出来了（$AuditRtspPort）"

# ---------------------------------------------------------------- [4] 恢复出厂
Section '[4] 恢复出厂（console 形态）'
$bound8564 = $false
foreach ($p in @(Get-NetTCPConnection -LocalPort $ResetRtspPort -State Listen -ErrorAction SilentlyContinue)) {
    Note "端口 $ResetRtspPort 被 PID $($p.OwningProcess) 占着"
    $bound8564 = $true
}
if ($bound8564) { Report "端口 $ResetRtspPort 被占 —— 这一段大概率会起来失败（那是环境，不是产品）" }

# 日志先删掉：[4] 要判"日志被清空"，而**清空前那份内容必须是这一跑自己写的** ——
# 上一跑留下的东西会让"WARN 找不到"这条断言变成一句空话（陷阱 60 的形状）。
Remove-Item -LiteralPath $logPath -ErrorAction SilentlyContinue

# `--console` 而不是裸形态：**只有 console 形态有日志文件**（`cmd_console` 里那句
# `Log::set_file`），而"恢复出厂清空日志"这条只能在没有日志文件的形态下验不了。
# `--rtsp-port 8564` 钉在命令行上：恢复出厂删的是**配置文件**，命令行参数活下来
# 正是要正面演示的那件事。
$o4 = Start-Captured "--console --config `"$cfgPath`" --no-wsd --rtsp-port $ResetRtspPort --record `"$mp4Reset`""
Note "console pid=$($o4.Proc.Id)"
Check (Wait-Http "http://127.0.0.1:$WebPort/api/session" 25000) 'console 形态的网页起来了（端口来自配置文件）'

$sess4 = $null
try {
    $null = Invoke-WebRequest -Uri "http://127.0.0.1:$WebPort/api/login" -Method POST `
        -ContentType 'application/json' -Body '{"user":"lifecycle","password":"d2o-lifecycle-pw"}' `
        -UseBasicParsing -SessionVariable sess4 -TimeoutSec 8
    Check $true '登录成功'
} catch {
    Check $false "登录成功（$($_.Exception.Message)）"
}

# --- (a) 形状不对 -> 400，而且什么都不做
$bad = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/factory-reset" '{}' $sess4
Check ($bad.Status -eq 400) "裸 {} 被拒：400（实际 $($bad.Status)）"
$badj = Get-Json $bad.Text
Check ($null -ne $badj -and -not $badj.ok) '400 的正文是一个 ok:false 的 JSON 对象'
Check (Test-Path -LiteralPath $cfgPath) '被拒之后 config.json 还在（400 不做事）'
$stillUp = $false
try { $null = Get-Process -Id $o4.Proc.Id -ErrorAction Stop; $stillUp = $true } catch { }
Check $stillUp '被拒之后进程还活着（没有把 400 当成重启）'

# --- (b) 先造一个非零的时间偏移，否则"偏移归零"那条断言是空的
$onvifUri = "http://127.0.0.1:$WebPort/onvif/device_service"
$hostBefore = [DateTime]::UtcNow
$target = $hostBefore.AddSeconds(3600)
$setBody = New-SetTimeBody $target
$setResp = Invoke-Onvif $onvifUri $setBody
Check ($setResp.Status -eq 200) "SetSystemDateAndTime 成功（HTTP $($setResp.Status)）"
$getResp = Invoke-Onvif $onvifUri (New-SoapEnvelope '    <tds:GetSystemDateAndTime/>')
Check ($getResp.Status -eq 200) '匿名 GetSystemDateAndTime 成功（它是唯一一个匿名操作）'
$dtt = Get-SoapText $getResp.Text 'DateTimeType'
Check ($dtt -eq 'Manual') "有人能设时间之后 DateTimeType 报 Manual（实际 '$dtt'）"
$devUtc = Get-DeviceUtc $getResp.Text
$offsetSeen = $null
if ($null -ne $devUtc) { $offsetSeen = [math]::Round(($devUtc - [DateTime]::UtcNow).TotalSeconds) }
Check ($null -ne $offsetSeen -and [math]::Abs($offsetSeen - 3600) -le 5) `
    "设备时间确实比主机快 3600 秒（实测 $offsetSeen）"

# 日志里必须已经出现那行 WARN —— 这是下面"清空之后找不到它"那条断言的**前一半**。
# 少了它，后一半在两种情况下都为真（陷阱 85）。
Start-Sleep -Seconds 2
# 日志正被**正在跑的那个进程**开着 —— 这里必须共享读，而且**不许吞异常**。
# 原来这里是 `try { ... ReadAllText ... } catch { }`：ReadAllText 因为共享模式冲突
# 每次都会抛（实测，见 Read-SharedText 的注释），异常被吞成空串之后 `-match` 报的是
# "文件里没有那行"，而事实是"我根本没读到文件"。两种情况的输出一模一样。
$logTextBefore = $null
$logReadError = ''
try { $logTextBefore = Read-SharedText $logPath } catch { $logReadError = $_.Exception.Message }
if ($logReadError -ne '') {
    Check $false "清空前：日志读得出来（读的时候抛了：$logReadError）"
} else {
    Check ($logTextBefore -match '设备时间被 NVR 改了') `
        '清空前：日志里找得到"设备时间被 NVR 改了"（那行 WARN 真的进来了）'
    if ($logTextBefore -notmatch '设备时间被 NVR 改了') {
        # 失败时自报家门：否则下一次还得再写一个单点探针来复现（那已经花过一整轮）
        Report "  ↑ 读到 $($logTextBefore.Length) 个字符，尾部 12 行如下："
        ($logTextBefore -split "`n") | Where-Object { $_ -ne '' } | Select-Object -Last 12 |
            ForEach-Object { Report "      $_" }
    }
}
$logItem = $null
try { $logItem = Get-Item -LiteralPath $logPath } catch { }
$logCreatedBefore = $null
if ($null -ne $logItem) { $logCreatedBefore = $logItem.CreationTimeUtc }
$logSizeBefore = 0
if ($null -ne $logItem) { $logSizeBefore = $logItem.Length }
Report "清空前日志 $logSizeBefore 字节，创建于 $($logCreatedBefore.ToString('HH:mm:ss'))"

# --- 审计日志的另一半：清空**前**的样子 --------------------------------------
#
# 与上面那条 WARN 断言同一条纪律（陷阱 85）：只验「清空之后没了」而不验
# 「清空之前确实有」，那一条在**两种**情况下都为真 —— 目录本来就是空的话，
# 「清空后是空的」照样绿。
#
# 这个数还有第二个用处：它是下面「旧进程报出来的 N」那条断言的对照物。
# 只看 `N >= 1` 挡不住「只删了一部分」，而 `clear()` **最可能的坏法**恰好
# 就是只删一部分 —— 今天那个文件正被它自己开着（`main.cpp:1433-1436`
# 那段注释专门讲的就是这件事：Windows 的 `fopen` 不带 `FILE_SHARE_DELETE`）。
$auditBefore = @(Get-ChildItem -LiteralPath $auditDir -Filter 'audit-*.log' -File -ErrorAction SilentlyContinue)
$auditBeforeCount = $auditBefore.Count
Check ($auditBeforeCount -ge 1) `
    "清空前：审计目录里有 $auditBeforeCount 个 audit-*.log（$auditDir）"
Check (Test-Path -LiteralPath $notesPath) `
    '清空前：种下的 notes.txt 还在 —— 下面「它还在」才是「没删错」的那一半'

# --- `.bak` 要比的是**哪一份**：就近重抓一次哈希 -------------------------------
#
# 准备阶段（第 539 行）那个 `$cfgHash` 到这里**已经过期了**，而它过期是产品
# **正当**做出来的：本段中间那条 `SetSystemDateAndTime` 会把 NVR 设的偏移落进
# `time.offset_s`（Phase U 的取舍 —— 手动校准出来的偏移重启后要还在，NVR 设的
# 是同一类事实），于是磁盘上那份 `config.json` 被重写过一遍：夹具那 250 字节的
# 手写 JSON，变成了产品序列化出来的完整那份（实测 2002 字节）。
#
# 拿准备阶段的哈希去比 `.bak`，红的会是「尺子的对象过期了」，而报告上它和
# 「备份错了」长得一模一样 —— 2026-10-08 首次执行这条就是这么红的。所以在
# **这里**量一次、把原因写成它自己的断言，比让下一个人去猜为什么差。
# 判据本身只问一件事：`.bak` 是不是 POST 之前那一刻的那一份。
$cfgHashBeforeReset = ''
$cfgHashErr = ''
try {
    $cfgHashBeforeReset = (Get-FileHash -LiteralPath $cfgPath -Algorithm SHA256).Hash
} catch {
    # 不吞异常（陷阱 91）：读不到时下面那两条都比不出来，而它们的红会指向
    # `.bak`；真正的原因在这儿，得让它自己报出来。
    $cfgHashErr = $_.Exception.Message
}
Check ($cfgHashErr -eq '') "POST 之前读得到 config.json（读的时候抛了：$cfgHashErr）"
Check ($cfgHashBeforeReset -ne $cfgHash) `
    'POST 之前那份 config.json 已经不是准备阶段那一份了 —— 本段的 SetSystemDateAndTime 把 time.offset_s 落盘了（Phase U 的行为；[3b] 与 onvif_soap 的 [1b] 另有正面覆盖）'

# --- (c) 形状对了 -> 202
$resetMoment = [DateTime]::UtcNow
$ok = Invoke-Api 'POST' "http://127.0.0.1:$WebPort/api/factory-reset" '{"confirm":"RESET"}' $sess4
Assert-JsonField $ok 'resetting' 'True' 'POST /api/factory-reset 回了 202 + resetting:true' | Out-Null
Check ($ok.Status -eq 202) "恢复出厂请求的状态码是 202（实际 $($ok.Status)）"

# --- (d) 重启完成 + 四个文件级后果
$e4 = Wait-Captured $o4 30000
Check ($e4.Code -eq 0) "旧进程退出码 0（实际 $($e4.Code)）"
Check ($e4.Out -match '恢复出厂：日志已清空（') '旧进程说清了"日志已清空"'
Check ($e4.Out -match '恢复出厂：设备时间偏移已归零') '旧进程说清了"设备时间偏移已归零"'
Check ($e4.Out -match '恢复出厂设置已完成，接着重启服务') '旧进程说清了"接着重启服务"'
$m4 = [regex]::Match($e4.Out, '已拉起新进程（PID (\d+)）')
$newPid4 = $null
if ($m4.Success) { $newPid4 = [int]$m4.Groups[1].Value }
Check ($null -ne $newPid4) "旧进程打出了新进程的 PID（$newPid4）"

Check (-not (Test-Path -LiteralPath $cfgPath)) 'config.json 不存在了'
Check (Test-Path -LiteralPath $cfgBak) 'config.json.bak 存在（后悔药）'
if (Test-Path -LiteralPath $cfgBak) {
    $bakHash = (Get-FileHash -LiteralPath $cfgBak -Algorithm SHA256).Hash
    # 与 **POST 之前那一刻**比，不与准备阶段比 —— 理由见上面那段。
    Check ($bakHash -eq $cfgHashBeforeReset) `
        '.bak 里是删除前那一刻的那一份（SHA256 与 POST 之前的 config.json 相同）'
    # 只比哈希还缺一半：它相等只说明「.bak 与**某一份**文件相同」，两边同为
    # 一份出厂默认时照样绿（陷阱 85 的形状）。所以再判一次它确实是我们的夹具。
    $bakJson = $null
    $bakParseErr = ''
    try {
        $bakJson = (Get-Content -LiteralPath $cfgBak -Raw -Encoding UTF8 | ConvertFrom-Json)
    } catch {
        $bakParseErr = $_.Exception.Message
    }
    Check ($bakParseErr -eq '') ".bak 是能解析的 JSON（解析时抛了：$bakParseErr）"
    if ($null -ne $bakJson) {
        Check ([int]$bakJson.web.port -eq $WebPort) `
            ".bak 里是我们夹具的网页端口（$($bakJson.web.port)）—— 它是一份真的旧配置，不是出厂默认"
        Check ([int]$bakJson.capture.bitrate_kbps -eq 512) `
            ".bak 里是我们夹具的码率（$($bakJson.capture.bitrate_kbps)）"
    }
} else {
    Check $false '.bak 里是旧那份内容'
}

$logExists = Test-Path -LiteralPath $logPath
$logTextAfter = $null
$logReadErrorAfter = ''
try { $logTextAfter = Read-SharedText $logPath } catch { $logReadErrorAfter = $_.Exception.Message }
Check $logExists '日志文件还在（被删掉之后重新打开了，否则后面到退出之间的日志只进终端）'
# 这一条同样不许吞异常：读不到文件时"找不到那行"是一句空话 —— 它会在
# "日志真的被清了"和"我根本没读到"两种情况下都为真（陷阱 85 的形状）。
# 原来那句 `try { ... } catch { }` 一直是**空转**的：恢复出厂之后文件由新进程开着，
# ReadAllText 必然抛，于是这一条从来没验过任何东西。
Check ($logReadErrorAfter -eq '') "清空后：日志读得出来（读的时候抛了：$logReadErrorAfter）"
Check ($null -ne $logTextAfter -and -not ($logTextAfter -match '设备时间被 NVR 改了')) `
    '清空后：日志里**找不到**那行 WARN（旧内容真的没了）'
$logCreatedAfter = $null
if ($logExists) { $logCreatedAfter = (Get-Item -LiteralPath $logPath).CreationTimeUtc }
Check ($null -ne $logCreatedAfter -and $null -ne $logCreatedBefore -and
       $logCreatedAfter -gt $logCreatedBefore -and $logCreatedAfter -ge $resetMoment.AddSeconds(-2)) `
    "日志文件的创建时间跳到了恢复出厂之后（旧 $($logCreatedBefore.ToString('HH:mm:ss')) -> 新 $($logCreatedAfter.ToString('HH:mm:ss'))）"
Report "清空后日志 $((Get-Item -LiteralPath $logPath).Length) 字节（**不判大小**：'更小'在'日志被清了'和'这次本来就少'两种情况下都成立）"

# --- (e) 重启后的新进程：默认端口 + 命令行参数活下来
if ($free8080) {
    $bound8080 = $false
    if ($null -ne $newPid4) { $bound8080 = Wait-ListenerPid 8080 $newPid4 20000 }
    Check $bound8080 "新进程绑在**默认的 8080** 上（配置文件没了 ⇒ 回到出厂默认）"
    $adminOk = $false
    $r8080 = Invoke-Api 'POST' 'http://127.0.0.1:8080/api/login' '{"user":"admin","password":"admin"}'
    $adminOk = ($r8080.Status -eq 200)
    Check $adminOk "登录用的是出厂默认的 admin/admin（实际 HTTP $($r8080.Status)）"
} else {
    Report '（8080 被占，跳过"绑回默认端口 / 默认口令"这两条）'
}
$boundResetRtsp = ($null -ne (Get-ListenerPid $ResetRtspPort))
Check $boundResetRtsp "RTSP $ResetRtspPort **照样在听** —— 命令行参数活过了恢复出厂（这就是那句诚实边界的正面证据）"

# --- (f) 审计日志：清空**后** -------------------------------------------------
#
# 与上面「调试日志被清空」那几条是**两份文件、两个集合**（Phase V 的取舍：留存期、
# 内容纪律、要清的集合三样都不一样），所以必须各留一组断言 —— 只判
# `desk2onvif.log` 的话，`AuditLog::clear()` 整个没接上也一条都不会红。
#
# 等一小会儿再读：`| startup |` 那一行是**新进程**写的，而它落在 `cmd_run` 里
# 相当靠后的位置（上面那几条只是等到端口重新在听而已）。
$auditAfter = @()
$auditText4 = ''
$auditReadErr4 = ''
$auditDeadline4 = [DateTime]::UtcNow.AddSeconds(10)
while ([DateTime]::UtcNow -lt $auditDeadline4) {
    $auditAfter = @(Get-ChildItem -LiteralPath $auditDir -Filter 'audit-*.log' -File -ErrorAction SilentlyContinue)
    if ($auditAfter.Count -ge 1) {
        # 正被新进程开着写 —— 必须共享读，而且**不许吞异常**（陷阱 91：吞掉的
        # 异常会变成一个看起来像「什么都没有」的空串，而那正好和「日志真被清了」
        # 报同一个结果）。
        try { $auditText4 = Read-SharedText $auditAfter[0].FullName } catch { $auditReadErr4 = $_.Exception.Message }
        if ($auditReadErr4 -eq '' -and $auditText4 -match '\| startup \|') { break }
    }
    Start-Sleep -Milliseconds 250
}

Check (Test-Path -LiteralPath $auditDir) '审计**目录**还在（`clear()` 只删文件、不递归删目录）'
Check (Test-Path -LiteralPath $notesPath) `
    '清空后 notes.txt 还在 —— 只删我们自己的那份文件，没删错（陷阱 60）'

Check ($auditAfter.Count -ge 1) "清空后审计目录里还有 $($auditAfter.Count) 个 audit-*.log（新进程写的）"
$strayAudit = @($auditAfter | Where-Object { $_.Name -notmatch '^audit-\d{4}-\d{2}-\d{2}\.log$' })
Check ($strayAudit.Count -eq 0) `
    "剩下的文件名字都合我们的格式（audit-YYYY-MM-DD.log；不合的：$($strayAudit.Name -join '、')）"
$oldAudit = @($auditAfter | Where-Object { $_.CreationTimeUtc -lt $resetMoment.AddSeconds(-2) })
Check ($oldAudit.Count -eq 0) `
    "没有一个是恢复出厂**之前**建的（旧的都被删了，这是重建出来的；$($oldAudit.Name -join '、')）"

Check ($auditReadErr4 -eq '') "清空后：审计文件读得出来（读的时候抛了：$auditReadErr4）"
Check ($auditText4 -match '\| startup \|') `
    '新审计文件里有新进程的 startup 一行 —— 不是「建了个空文件」'
Check ($auditText4 -match '\| factory-reset \|') `
    '同一份文件里留着「谁恢复的出厂」那一行（旧进程清完之后自己写下的第一行）'
Check (-not ($auditText4 -match [regex]::Escape($markerLine))) `
    '种在**今天**那份里的标记行没了 —— 连今天这一份也被删掉重建过，不是只删了旧日期那几份'

# `clear()` 的返回值一路打到 stdout 上，正好拿来和清空前数到的文件数对账。
$mAudit = [regex]::Match($e4.Out, '操作日志 (\d+) 个文件')
Check $mAudit.Success '旧进程打出了「操作日志 N 个文件」'
$auditCount4 = -1
if ($mAudit.Success) { $auditCount4 = [int]$mAudit.Groups[1].Value }
Check ($auditCount4 -eq $auditBeforeCount) `
    "报出来的 N 与清空前数到的文件数一致（报 $auditCount4，实数 $auditBeforeCount）—— 报 0 或只报一部分都说明只清掉了一部分"

# 恢复出厂之后设备时间报什么，**只报告不判**。
#
# 想要的那条断言（"reset_time() 真的跑了 ⇒ DateTimeType 回到 NTP"）是**空的**：
# reset_time() 是在**旧**进程里跑的，而那个进程紧接着就退出了；新进程构造的是一个
# **全新**的 DeviceState，time_synced() 本来就是 false —— 把 device_reset_time()
# 整个删掉，这里读到的仍然是 NTP。而且它也没法在旧进程里观测：`web.stop()` 在
# `factory_reset_files()` **之前**执行，那时已经没有 HTTP 能问到那个 DeviceState 了。
# 真正量得到 `reset_time()` 的地方是"时间偏移持久化 + 归零"那一条（Phase U 的
# `/api/time/reset`）—— 那里偏移活在进程内，看得见。
$finalPort = if ($free8080) { 8080 } else { $WebPort }
$post = Invoke-Onvif "http://127.0.0.1:$finalPort/onvif/device_service" (New-SoapEnvelope '    <tds:GetSystemDateAndTime/>')
$dttAfter = Get-SoapText $post.Text 'DateTimeType'
$devAfter = Get-DeviceUtc $post.Text
$offAfter = $null
if ($null -ne $devAfter) { $offAfter = [math]::Round(($devAfter - [DateTime]::UtcNow).TotalSeconds) }
Report "恢复出厂后：DateTimeType='$dttAfter'，设备时间与主机差 $offAfter 秒（**只看不判**，理由见脚本注释）"

Stop-OurExe
[void](Wait-NoListener $WebPort 8000)

# ---------------------------------------------------------------- [5] 用户自己的那份配置
Section '[5] %APPDATA% 里用户自己那份没被动过'
# 快照比法（陷阱 60）：`%APPDATA%\Desk2ONVIF` **事先不存在**这条断言在开发机上
# 恒真、在真装过的机器上恒假，两种都不验东西。比两次快照则两种初始状态都成立。
$snapAfter = Get-Snapshot $apdata
Check ($snapAfter -eq $snapBefore) '%APPDATA%\Desk2ONVIF 一字未变（相对路径 + 大小 + mtime 全等）'
if ($snapAfter -ne $snapBefore) {
    Note "before: $snapBefore"
    Note "after : $snapAfter"
}

# ---------------------------------------------------------------- 收尾
Write-Host ''
if ($script:failed -eq 0) {
    Write-Host "全部通过（$($script:total) 项）。" -ForegroundColor Green
} else {
    Write-Host "通过 $($script:total - $script:failed) 项，失败 $($script:failed) 项。" -ForegroundColor Red
}
# **必须显式 exit。** PowerShell 只在原生命令退出时设置 `$LASTEXITCODE`，脚本自己
# 正常跑完不会把它归零 —— 于是调用方读到的会是最后那个 ffprobe 留下的值，而"通过"
# 的那一次带出一个非零码正是最坏的方向（陷阱 89）。
exit $script:failed
