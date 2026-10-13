<#
.SYNOPSIS
    安装凭据验收：安装时填的用户名 / 口令，真的落进配置、真的能用来登录。

.DESCRIPTION
    用户报的那条是「我用安装包安装的时候设置的 admin 账号密码，**没有写入到配置
    里面**，导致初次进入还是 admin 的密码」。

    它值得一个独立脚本，而不是往 verify_installer.ps1 里再加两条断言 ——
    因为**同一个症状有三条互相独立的断链**，任何一条单独坏掉都报一样的结果，
    而修好两条、第三条没修，用户照样登不进去：

      断链 1  desk2onvif.iss 的 WriteConfig 只写了 onvif 那一组 —— Web 那份
              落回出厂 admin/admin（`{app}\config.json` 里根本没有 web.user）
      断链 2  start.ps1 无条件追加 `--onvif-user admin --onvif-password admin`
              —— 安装时填的那一对**每次启动都被踩掉**（NVR 那一侧）
      断链 3  start-local.bat 漏了 `-Portable` —— 它去读
              %APPDATA%\Desk2ONVIF\config.json，而安装包写的是 {app}\config.json

    所以判据分三层，每层**各自**盯住一条（每一层都配一个变异检验，见
    out\creds-mut\ 里那一轮的记录）：

      [2] 读安装写出来的 {app}\config.json                  → 断链 1
      [3] 用真的 start-local.bat 起服务，判「配置落在 exe 旁边，
          而且被这次的命令行刷新过 + %APPDATA% 一字未动」   → 断链 3
      [4] /api/login：填的那对能进、admin/admin 进不去       → 断链 1 / 3 的行为面
      [5] /api/config 的 web.user 与 **onvif.user**          → 断链 2 的正脸

    ⚠️ [5] 里真正盯住断链 2 的是 `onvif.user`，**不是** `web.user`。
    `--onvif-user` / `--onvif-password` 在 main.cpp 里只喂 `wo.onvif.user`
    （`args.value("--onvif-user", cfg.onvif.user)`；web 那一对走的是
    `--http-user`），**碰不到 web.user**。也就是说只判 web.user 的话，把断链 2
    改回去这个脚本照样全绿 —— 陷阱 85 那个形状：一条断言在两种实现下都为真，
    留着等于没验。断链 2 的用户可见面在 NVR 那一侧，而在本机读得到它的正是
    `/api/config` 的 `onvif.user`（那是启动回写进 `cfg` 之后的**生效值**）。

    三条断链为什么非要分开判，而不是"反正登进去了就行"：它们是三个各自独立的
    写入点（陷阱 97 的同族）。症状消失只说明**至少有一条**修好了，而下一次
    "改了没生效"会从另外两条里长出来。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\installer_creds.ps1
    powershell -ExecutionPolicy Bypass -File tests\installer_creds.ps1 -Setup release\Desk2ONVIF_Setup_0.1.7.exe
    powershell -ExecutionPolicy Bypass -File tests\installer_creds.ps1 -Keep
#>
[CmdletBinding()]
param(
    # 要验的安装包。默认取 release\ 下最新的那个（与两个 verify_*.ps1 同一口径）。
    # ⚠️ 跑变异检验时**必须显式给**：默认这条路是"目录里最新的那个包"，
    # 而它正是陷阱 102 记的那件事（打包失败时它会拿上一个版本的包当真交付物）。
    # 报告里会把包的 LastWriteTime 一起打出来，方便对账。
    [string] $Setup,
    # 装到这里（验证用的临时位置，不是正式安装目录 —— 与 verify_installer.ps1
    # 分开，两个脚本可以互不干扰地各跑一遍）
    [string] $InstallDir,
    # 装的时候写的端口（落进 config.json 的那一对）
    [int]    $InstallRtspPort = 8562,
    [int]    $InstallHttpPort = 8205,
    # 起服务时命令行传的端口。**故意与上面那一对不同**：它是"start.ps1 读的是
    # 哪一份配置"唯一的正面判据 —— 只有它读了 {app}\config.json 并走了启动回写，
    # 盘上那一份才会被刷成这两个数（见 [3] 段）。
    [int]    $RunRtspPort = 8563,
    [int]    $RunHttpPort = 8206,
    # 验完不卸载，留着肉眼看
    [switch] $Keep
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# 共用那套在 scripts\ 下（它不在 tests\ 里 —— 点源要指对地方，指错了
# 报的是"Check 不是内部或外部命令"，看着像脚本写坏了）。
. (Join-Path $root 'scripts\_verify_common.ps1')

# 拿一对用户名/口令去登 `/api/login`，返回 HTTP 状态码和（成功时的）会话对象。
#
# 为什么不直接 Invoke-WebRequest + try/catch：失败那条路要**分得清"被拒（401）"
# 和"根本没连上（拿不到状态码）"**。两者都落进 catch，而 `$_.Exception.Response`
# 只在真的收到应答时才有 —— 没有它就把状态码记 0，于是断言里写的 `-eq 401`
# 不会让"服务压根没起来"伪装成"登录被拒"（陷阱 91 那一族：失败与"没量到"
# 长得一样）。
#
# 请求体走 **UTF-8 字节**而不是字符串：PS 5.1 的 Invoke-WebRequest 拿字符串当
# body 时按本地代码页（936）编码，非 ASCII 口令在**客户端**就被改坏了 ——
# 而服务端回的那句话是"用户名或口令不对"，一个字的线索都不指向编码。
function Connect-Console {
    param([int]$Port, [string]$User, [string]$Password, [int]$TimeoutSec = 10)
    $json = @{ user = $User; password = $Password } | ConvertTo-Json -Compress
    try {
        $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/api/login" -Method Post `
                 -Body ([Text.Encoding]::UTF8.GetBytes($json)) `
                 -ContentType 'application/json; charset=utf-8' `
                 -TimeoutSec $TimeoutSec -UseBasicParsing `
                 -SessionVariable sess -ErrorAction Stop
        return [pscustomobject]@{ Status = [int]$r.StatusCode; Session = $sess; Error = '' }
    } catch {
        $resp = $_.Exception.Response
        return [pscustomobject]@{
            Status  = $(if ($resp) { [int]$resp.StatusCode } else { 0 })
            Session = $null
            Error   = $_.Exception.Message
        }
    }
}

if (-not $Setup) {
    $cand = @(Get-ChildItem -LiteralPath (Join-Path $root 'release') -Filter 'Desk2ONVIF_Setup_*.exe' -ErrorAction SilentlyContinue |
              Sort-Object LastWriteTime -Descending)
    if ($cand.Count -eq 0) { throw 'release\ 下没有安装包。先跑 scripts\package.ps1。' }
    $Setup = $cand[0].FullName
}
if (-not $InstallDir) {
    $InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\Desk2ONVIF-verify-creds'
}
$log    = Join-Path $env:TEMP 'd2o-verify-creds-install.log'
$apDir  = Join-Path $env:APPDATA 'Desk2ONVIF'
$cfgPath = Join-Path $InstallDir 'config.json'

# 口令同时含 **引号 / 反斜杠 / 非 ASCII** 三样东西，一样都不是装饰：
#   · 引号和反斜杠 —— `JsonEscape` 存在的全部理由。少了它，一份语法非法的
#     config.json 会让程序直接拒绝启动，而用户看到的是"闪一下就没了"。
#   · 非 ASCII   —— `.iss` 用 SaveStringsToUTF8FileWithoutBOM 落盘（按 ANSI/936
#     写会写出 GBK 字节，而程序按 UTF-8 解 ⇒ **用户按自己填的口令登不进去**，
#     症状与"安装包没写 web.user"一模一样）。纯 ASCII 的口令照不出这一条。
#
# 用户名走 /D2OUSER= 命令行参数、口令走环境变量：这两条路在 .iss 里是两个
# 不同的取值口（`GetParam` / `EnvValue`），**都得有人走一遍**，否则其中一个
# 坏了不会有任何东西变红。verify_installer.ps1 那边两个都走环境变量，
# 这里正好互补。
$TestUser = 'nbuser'
$TestPass = '验证Pw"b\c'

Write-Host ''
Write-Host '=== 安装凭据验收 ===' -ForegroundColor Cyan
Write-Host ("  安装包    " + $Setup)
Write-Host ("  包的日期  " + (Get-Item -LiteralPath $Setup).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
Write-Host ("  测试装到  " + $InstallDir)
Write-Host ("  端口      装(" + $InstallRtspPort + "/" + $InstallHttpPort + ") → 跑(" + $RunRtspPort + "/" + $RunHttpPort + ")")
Write-Host ("  用户名    " + $TestUser + "（走 /D2OUSER= 命令行参数）")
Write-Host  "  口令      （走 DESK2ONVIF_PASSWORD 环境变量；含引号 / 反斜杠 / 非 ASCII，不打印）"

# --- 量具先量一遍（陷阱 82）------------------------------------------------
#
# 这两条问的不是产品，是**这个脚本自己**：口令里那三样必须真的在。有人哪天
# 嫌麻烦把 $TestPass 改成 'secret'，上面 [2]/[4] 段会照样全绿 —— 而它们
# 覆盖的正是"引号会不会把 JSON 写坏""非 ASCII 会不会写成 GBK"这两件事，
# 于是整个脚本会在**看起来更干净**的外表下退化成"只验了一对普通口令"。
Section "[0] 量具：测试口令本身"
Check ($TestPass.Contains('"'))   '口令里有双引号（JsonEscape 那一格的判据）'
Check ($TestPass.Contains('\'))   '口令里有反斜杠（同上，且它是 \u 转义那一支）'
Check ($TestPass -match '[^\x00-\x7F]') '口令里有非 ASCII 字符（UTF-8 落盘那一格的判据）'
Check ($TestUser -cmatch '^[A-Za-z]') "用户名是纯 ASCII（$TestUser）" `
      '用户名要是也带非 ASCII，上面 [2] 段的口令断言就分不出"是口令写坏了"还是"用户名写坏了"。'

# --- 前置：确认没有别的 Desk2ONVIF 在跑 --------------------------------
Section "[1] 前置检查"
$running = @(Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue)
Check ($running.Count -eq 0) "没有正在运行的 desk2onvif（找到 $($running.Count) 个）" `
      '先关掉它再验：多个实例抢同一个 RTSP 端口，验出来的是噪音。'
foreach ($pt in @($InstallRtspPort, $InstallHttpPort, $RunRtspPort, $RunHttpPort)) {
    Check (-not (Test-PortBusy $pt)) "端口 $pt 空闲"
}

# 上一次的残留先清掉
if (Test-Path -LiteralPath $InstallDir) {
    $un = Join-Path $InstallDir 'unins000.exe'
    if (Test-Path -LiteralPath $un) {
        Write-Host '  （卸载上一次的验证安装…）'
        $null = Invoke-Capture $un @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') 180000 $InstallDir
    }
    for ($i = 0; $i -lt 40 -and (Test-Path -LiteralPath $InstallDir); $i++) { Start-Sleep -Milliseconds 500 }
    if (Test-Path -LiteralPath $InstallDir) { Remove-Item -LiteralPath $InstallDir -Recurse -Force -ErrorAction SilentlyContinue }
}

# 运行前后各拍一张 %APPDATA%\Desk2ONVIF 的快照。断链 3 的**否定式**判据。
$apBefore = Get-TreeSnapshot $apDir
if ($apBefore -eq '<不存在>') {
    Write-Host "  （%APPDATA%\Desk2ONVIF 事先不存在 —— 跑完必须仍然不存在）" -ForegroundColor DarkGray
} else {
    Write-Host "  （%APPDATA%\Desk2ONVIF 事先存在，$(($apBefore -split "`n").Count) 项 —— 跑完必须一字未变）" -ForegroundColor DarkGray
}

# --- [2] 静默安装，参数真的落进 config.json（断链 1）-------------------
Section "[2] 静默安装 → 读 {app}\config.json（断链 1：安装包到底写没写 web 那份）"
if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }

$env:DESK2ONVIF_PASSWORD = $TestPass
# 用户名**同时**清掉环境变量那份，免得它把命令行那份遮住 —— 这一轮要量的
# 就是 "/D2OUSER= 这条路通不通"，环境变量还留着的话量到的是另一条路。
Remove-Item Env:\DESK2ONVIF_USER -ErrorAction SilentlyContinue

$sw = @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
    "/DIR=$InstallDir",
    "/LOG=$log",
    "/D2OUSER=$TestUser",
    "/D2ORTSPORT=$InstallRtspPort",
    "/D2OHTTPPORT=$InstallHttpPort",
    '/MERGETASKS=!desktopicon'
)
Write-Host ("  命令行  " + ($sw -join ' ') + "  (+ DESK2ONVIF_PASSWORD 环境变量)")
$inst = Invoke-Capture $Setup $sw 600000 $root
if ($inst.TimedOut) {
    Check $false '安装进程在 600 秒内结束' `
          '挂住了 —— /VERYSILENT 下 [Code] 里的 MsgBox 没人能点，先看安装日志最后一行。'
} else {
    Check ($inst.ExitCode -eq 0) "退出码 = $($inst.ExitCode)"
}
Check (Test-Path -LiteralPath $InstallDir) '建了安装目录'
if (-not (Test-Path -LiteralPath $InstallDir)) {
    Write-Host ''
    Write-Host '安装目录都没建起来，后面的检查没有意义。' -ForegroundColor Red
    if (Test-Path -LiteralPath $log) { Show ((Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue)) 20 }
    exit 1
}

if (Test-Path -LiteralPath $cfgPath) {
    $cfg0 = $null
    try { $cfg0 = Get-Content -LiteralPath $cfgPath -Raw -Encoding UTF8 | ConvertFrom-Json }
    catch { Check $false '安装写出来的 config.json 是合法 JSON' $_.Exception.Message }

    if ($cfg0) {
        # 断链 1 的正脸：Web 那一对是**装的时候填的那一份**。
        Check ($cfg0.web.user -ceq $TestUser) "web.user = $($cfg0.web.user)（期望 $TestUser）" `
              ('对不上说明 .iss 的 WriteConfig 没把向导里那一对写进 web 段 —— ' +
               '那正是用户报的"初次进入还是 admin 的密码"。')
        Check ($cfg0.web.password -ceq $TestPass) `
              'web.password 往返一致（引号 + 反斜杠 + 非 ASCII）' `
              ("期望（$($TestPass.Length) 个字符，含中文/引号/反斜杠），实际 " +
               "$($cfg0.web.password.Length) 个字符：$($cfg0.web.password)")
        # ONVIF 那一份是同一对（.iss 里同一个 SelectedUser/SelectedPass）。
        # 它以前就有，所以这两条是**回归护栏**：将来有人把 web 那份补上、
        # 顺手把 onvif 那份改坏了，得有人喊。
        Check ($cfg0.onvif.user -ceq $TestUser) "onvif.user = $($cfg0.onvif.user)（期望 $TestUser）"
        Check ($cfg0.onvif.password -ceq $TestPass) 'onvif.password 与 web 那份是同一对'
        Check ($cfg0.web.port -eq $InstallHttpPort) "config.json 里的 web.port = $($cfg0.web.port)（装的时候传的是 $InstallHttpPort）"
        Check ($cfg0.rtsp.port -eq $InstallRtspPort) "config.json 里的 rtsp.port = $($cfg0.rtsp.port)"
    }
} else {
    Check $false "安装写了 $cfgPath" '没有它，后面的登录判据全部无意义。'
}

# --- [3] 用包里那份 start-local.bat 起服务（断链 3）--------------------
#
# 起的**就是用户双击的那个批处理**，不是把 start.ps1 的参数在这里再拼一遍：
# 断链 3 恰恰在批处理那一行里（少一个 `-Portable`），绕开它就永远验不到。
# 调用形态是 `cmd.exe /c call "<bat>" …` —— 已实测（out\creds-mut\launch-probe\）：
# `/c` 后面第一个字符不是引号，于是 cmd 那条"剥掉命令行首尾引号"的老规则不
# 参与，`> "out" 2> "err"` 照常工作，参数也逐字送到（含带空格的引号路径）。
Section "[3] 用 start-local.bat 起服务（断链 3：它读的是哪一份配置）"
$appOut = Join-Path $env:TEMP 'd2o-creds-app.out'
$appErr = Join-Path $env:TEMP 'd2o-creds-app.err'
$startBat = Join-Path $InstallDir 'start-local.bat'
Check (Test-Path -LiteralPath $startBat) 'start-local.bat 在安装目录里'

$runArgs = '/c call "' + $startBat + '" -RtspPort ' + $RunRtspPort + ' -HttpPort ' + $RunHttpPort
$app = Start-Detached 'cmd.exe' $runArgs $InstallDir $appOut $appErr

try {
    $up = Wait-Backend $RunHttpPort
    Check $up "后端在 http://127.0.0.1:$RunHttpPort 起来了（走 start-local.bat）"
    if (-not $up) {
        Show (Get-Content -LiteralPath $appErr -Raw -Encoding UTF8 -ErrorAction SilentlyContinue) 20
        Show (Get-Content -LiteralPath $appOut -Raw -Encoding UTF8 -ErrorAction SilentlyContinue) 20
    }

    if ($up) {
        # (a) 正面判据：盘上那一份被**这一次的命令行端口**刷新过。
        #     只有 start.ps1 真的把 {app}\config.json 交给了 exe（-Portable），
        #     而且启动回写真的落了盘，才会有这个数。
        $cfg1 = $null
        try { $cfg1 = Get-Content -LiteralPath $cfgPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch {}
        if ($cfg1) {
            Check ($cfg1.web.port -eq $RunHttpPort) `
                  "-Portable：配置落在 exe 旁边，且被这次的命令行端口刷新（web.port = $($cfg1.web.port)，期望 $RunHttpPort）" `
                  ('对不上有两个可能，都要查：start-local.bat 没把 -Portable 传给 start.ps1' +
                   '（于是它去读了 %APPDATA% 那份），或者启动回写没有落盘。')
            # 顺带：这一份里那一对还是装的时候填的，命令行没有把它踩掉。
            Check ($cfg1.web.user -ceq $TestUser) "跑完之后 config.json 里的 web.user 仍是 $($cfg1.web.user)"
        } else {
            Check $false "跑完之后 $cfgPath 还能读回来"
        }

        # (b) 否定式判据：没有往 %APPDATA% 写任何东西。
        #     只看 (a) 是不够的 —— 两边都写的话 (a) 照样绿，而用户看到的是
        #     "改了配置有时生效有时不生效"。
        $apNow = Get-TreeSnapshot $apDir
        Check ($apNow -ceq $apBefore) `
              '运行期间没有往 %APPDATA%\Desk2ONVIF 写任何东西' `
              "运行前：`n$apBefore`n运行后：`n$apNow"

        # (c) 同一个判据的**文字**形态：start.ps1 在回落的时候会把那份路径打出来。
        #     先量一下告示牌真的有输出 —— 输出为空的话下面那条否定式断言是
        #     白过的（陷阱 91：被吞掉的异常和"确实没有那句话"长得一模一样）。
        $outTxt = Get-Content -LiteralPath $appOut -Raw -Encoding UTF8 -ErrorAction SilentlyContinue
        if (-not $outTxt) { $outTxt = '' }
        Check ($outTxt.Length -gt 50) "start.ps1 的告示牌有输出（$($outTxt.Length) 字符）" `
              '空输出说明这条路根本没跑到打印那一步 —— 那样下面"没回落到 %APPDATA%"就是白过的。'
        Check (-not $outTxt.Contains($apDir)) 'start.ps1 没有回落到 %APPDATA% 那份配置' `
              ("输出里出现了 $apDir —— 那就是断链 3（start-local.bat 少了 -Portable）。")
        # 正面形态：命令行参数确实送到了 exe（告示牌上念的是这个端口）。
        Check ($outTxt -match [regex]::Escape(":$RunRtspPort/live")) `
              "告示牌上的 RTSP 地址是这次传的端口（$RunRtspPort）" `
              '对不上说明参数没送到 start.ps1 的 param 块里 —— 那样 [3] 段判的就不是这一对端口。'

        # --- [4] 登录：填的那对能进、admin/admin 进不去（断链 1 / 3 的行为面）---
        #
        # **两条一起判**。只判"填的那对能进"的话，一个"两个账号都接受"的
        # 实现（比如口令比较被写成了恒真）会照样过。
        Section "[4] /api/login（断链 1 / 3 的行为面）"
        $good = Connect-Console $RunHttpPort $TestUser $TestPass
        Check ($good.Status -eq 200) `
              "拿安装时填的用户名 + 口令登录成功（HTTP $($good.Status)，期望 200）" `
              (("用户名 $TestUser、口令是装的时候填的那一份。" ) + $good.Error)
        $bad = Connect-Console $RunHttpPort 'admin' 'admin'
        Check ($bad.Status -eq 401) `
              "拿 admin/admin 登录被拒（HTTP $($bad.Status)，期望 401）" `
              ('返回 200 说明**出厂口令仍然有效** —— 安装时填的那一份没被真的用上。' + $bad.Error)

        # --- [5] 生效凭据：命令行没有把它踩回 admin（断链 2）-------------------
        Section "[5] GET /api/config（断链 2：启动器有没有把凭据踩掉）"
        $live = $null
        if ($good.Session) {
            try {
                $resp = Invoke-WebRequest -Uri "http://127.0.0.1:$RunHttpPort/api/config" `
                            -WebSession $good.Session -TimeoutSec 10 -UseBasicParsing
                $live = $resp.Content | ConvertFrom-Json
            } catch {
                Check $false 'GET /api/config 能读到' $_.Exception.Message
            }
        } else {
            Check $false 'GET /api/config 能读到' '上一步没登录成功，拿不到会话。'
        }
        if ($live) {
            Check ($live.web.user -ceq $TestUser) `
                  "生效配置里的 web.user = $($live.web.user)（期望 $TestUser）" `
                  '对不上说明起服务的命令行把 Web 凭据覆盖掉了。'
            # **这一条才是断链 2 的正脸。** `--onvif-user admin` 只喂 onvif 那一份，
            # 碰不到 web.user —— 只判上面那条的话，把断链 2 改回去这里照样全绿。
            Check ($live.onvif.user -ceq $TestUser) `
                  "生效配置里的 onvif.user = $($live.onvif.user)（期望 $TestUser）" `
                  ('对不上说明 start.ps1 把 --onvif-user 无条件追加上了 —— 断链 2。' +
                   '用户在这一侧的可见面是"NVR 上怎么填都连不上"。')
        }
    }
} finally {
    Stop-Desk2Onvif
    # 收整棵树：cmd.exe → powershell.exe(start.ps1) → desk2onvif.exe。
    # 只 Kill 那个 cmd 的话，下面两个会变成孤儿继续占着端口。
    try { & taskkill.exe /F /T /PID $app.Process.Id 2>&1 | Out-Null } catch {}
    Remove-Item -LiteralPath $app.CmdFile -Force -ErrorAction SilentlyContinue
}
Stop-Desk2Onvif
Wait-PortFree @($RunRtspPort, $RunHttpPort)

# 停机那一刻再拍一张：退出时写配置（收尾保存是好几种程序的习惯做法）正好
# 落在"运行期间"那一张之后。
$apEnd = Get-TreeSnapshot $apDir
Check ($apEnd -ceq $apBefore) '直到停机为止 %APPDATA%\Desk2ONVIF 都和运行前一模一样' `
      "运行前：`n$apBefore`n停机后：`n$apEnd"

# --- [6] 静默卸载，必须一点不剩 ----------------------------------------
#
# 这一段不只是"收尾干净"：这次起服务走的是 `--console` 形态（start.ps1 就是
# 这么起的），它会在配置目录里建 `desk2onvif.log`，操作日志还会建 `logs\`
# —— 两个都是**运行期**才出现的文件，正是陷阱 101 那一类。
Section "[6] 静默卸载"
if ($Keep) {
    Write-Host '  （-Keep：跳过卸载）' -ForegroundColor Yellow
} else {
    $un = Join-Path $InstallDir 'unins000.exe'
    if (Test-Path -LiteralPath $un) {
        $ui = Invoke-Capture $un @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') 180000 $InstallDir
        Check (-not $ui.TimedOut) '卸载进程在 180 秒内返回'
    } else {
        Check $false '有 unins000.exe'
    }
    $gone = $false
    for ($i = 0; $i -lt 60; $i++) {
        if (-not (Test-Path -LiteralPath $InstallDir)) { $gone = $true; break }
        Start-Sleep -Milliseconds 500
    }
    Check $gone "安装目录整个没了（$InstallDir）"
    if (-not $gone) {
        $left = @(Get-ChildItem -LiteralPath $InstallDir -Recurse -Force -ErrorAction SilentlyContinue |
                  ForEach-Object { $_.FullName.Substring($InstallDir.Length + 1) })
        Check $false "卸载没有留下东西（还剩 $($left.Count) 项）" ($left -join ', ')
    }
    Check (@(Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue).Count -eq 0) '卸载后没有残留进程'
}

Remove-Item Env:\DESK2ONVIF_PASSWORD -ErrorAction SilentlyContinue
Write-VerifySummary
