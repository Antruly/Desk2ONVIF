<#
.SYNOPSIS
    安装包验收：真的装一遍、真的跑起来、真的卸干净。

.DESCRIPTION
    为什么不能靠"双击一下看着能装"：
      - **装不上**和"装上了但少东西"是两回事，后者根本不报错。少了
        web\dist 的包装完照跑、推流照推、ONVIF 照应答，只有网页界面打不开。
      - 卸载不干净也是静默的。[Code] 写出来的 config.json 不在 Inno 的
        安装清单里，卸载会把它剩在目录里 —— 目录于是不空、也不会被移除。
      - "静默安装能不能用"必须单独验：/VERYSILENT 下任何 [Code] 里的
        MsgBox 都会把安装**挂住**，没有报错、没有退出码、日志停在问句上。

    所以这里逐条断言，最后给退出码。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\verify_installer.ps1
    powershell -ExecutionPolicy Bypass -File scripts\verify_installer.ps1 -Keep
#>
[CmdletBinding()]
param(
    # 要验的安装包；默认取 release\ 下最新的那个
    [string] $Setup,
    # 装到这里（验证用的临时位置，不是正式安装目录）
    [string] $InstallDir,
    [int]    $RtspPort = 8556,
    [int]    $HttpPort = 8199,
    # 验完不卸载，留着肉眼看
    [switch] $Keep
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot '_verify_common.ps1')

if (-not $Setup) {
    $cand = @(Get-ChildItem -LiteralPath (Join-Path $root 'release') -Filter 'Desk2ONVIF_Setup_*.exe' -ErrorAction SilentlyContinue |
              Sort-Object LastWriteTime -Descending)
    if ($cand.Count -eq 0) { throw 'release\ 下没有安装包。先跑 scripts\package.ps1。' }
    $Setup = $cand[0].FullName
}
if (-not $InstallDir) {
    $InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\Desk2ONVIF-verify'
}
$log = Join-Path $env:TEMP 'd2o-verify-install.log'

# 口令里同时放引号和反斜杠：JsonEscape 存在的**全部理由**就是这两个字符。
# 少了它，一份语法非法的 config.json 会让程序直接拒绝启动，而用户看到的
# 是"闪一下就没了"，很难想到是自己口令里的一个引号。
#
# 这两个值走**环境变量**而不是 /D2OUSER= /D2OPASS=：Inno 的开关解析不按
# MSVC 那套反斜杠转义规则来，值里一个双引号会被当引号分隔符吃掉，传进去的
# 就不是原样了。环境变量不过这一层。
$TestUser = 'verifyuser'
$TestPass = 'A"b\c'

Write-Host ''
Write-Host '=== 安装包验收 ===' -ForegroundColor Cyan
Write-Host ("  安装包    " + $Setup)
Write-Host ("  测试装到  " + $InstallDir)
Write-Host ("  端口      RTSP " + $RtspPort + " / Web " + $HttpPort)

# --- 前置：确认没有别的 Desk2ONVIF 在跑，否则端口和 3702 都不是干净状态 ----
$running = @(Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue)
Section "[0] 前置检查"
Check ($running.Count -eq 0) "没有正在运行的 desk2onvif（找到 $($running.Count) 个）" `
      '先关掉它再验：多个实例抢同一个 RTSP 端口，验出来的是噪音。'
foreach ($pt in @($RtspPort, $HttpPort)) { Check (-not (Test-PortBusy $pt)) "端口 $pt 空闲" }

# --- 清掉上一次的残留 ------------------------------------------------
if (Test-Path -LiteralPath $InstallDir) {
    $un = Join-Path $InstallDir 'unins000.exe'
    if (Test-Path -LiteralPath $un) {
        Write-Host '  （卸载上一次的验证安装…）'
        $null = Invoke-Capture $un @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') 180000 $InstallDir
    }
    for ($i = 0; $i -lt 40 -and (Test-Path -LiteralPath $InstallDir); $i++) { Start-Sleep -Milliseconds 500 }
    if (Test-Path -LiteralPath $InstallDir) { Remove-Item -LiteralPath $InstallDir -Recurse -Force -ErrorAction SilentlyContinue }
}

# --- [1] 静默安装 -----------------------------------------------------
Section "[1] 静默安装"
if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }

$env:DESK2ONVIF_USER = $TestUser
$env:DESK2ONVIF_PASSWORD = $TestPass

$sw = @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
    "/DIR=$InstallDir",
    "/LOG=$log",
    '/MERGETASKS=!desktopicon'          # 验证不往桌面上放东西
)
Write-Host ("  命令行  " + ($sw -join ' ') + "  (+ DESK2ONVIF_USER / DESK2ONVIF_PASSWORD 环境变量)")
$inst = Invoke-Capture $Setup $sw 600000 $root
if ($inst.TimedOut) {
    Check $false '安装进程在 600 秒内结束' `
          '挂住了 —— /VERYSILENT 下 [Code] 里的 MsgBox 没人能点，先看安装日志最后一行。'
} else {
    Check ($inst.ExitCode -eq 0) "退出码 = $($inst.ExitCode)"
}
Check (Test-Path -LiteralPath $log) '写了 /LOG 指定的日志'
Check (Test-Path -LiteralPath $InstallDir) '建了安装目录'

if (Test-Path -LiteralPath $log) {
    Write-Host '  --- 日志末尾 ---' -ForegroundColor DarkGray
    Show ((Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue)) 6
}

if (-not (Test-Path -LiteralPath $InstallDir)) {
    Write-Host ''
    Write-Host '安装目录都没建起来，后面的检查没有意义。' -ForegroundColor Red
    exit 1
}

# --- [2] 装下来的东西对不对 -------------------------------------------
Section "[2] 安装内容"

# README.md 必须是**文件**。曾经写成 DestDir={app}\README.md，于是装出来
# 一个叫 README.md 的目录，[Icons] 里"打开使用说明"点开是个文件夹。
$readme = Join-Path $InstallDir 'README.md'
Check ((Test-Path -LiteralPath $readme) -and -not (Get-Item -LiteralPath $readme).PSIsContainer) `
      'README.md 是文件（不是同名目录）'

foreach ($f in @('desk2onvif.exe', 'config.json', 'start.bat', 'start.ps1',
                 'start-local.bat', 'list-displays.bat', 'selfcheck.bat',
                 'firewall-allow.bat', 'firewall-print.bat',
                 'LICENSE', 'THIRD-PARTY.md', 'desk2onvif.ico', 'unins000.exe')) {
    Check (Test-Path -LiteralPath (Join-Path $InstallDir $f)) "有 $f"
}

$dlls = @(Get-ChildItem -LiteralPath $InstallDir -Filter '*.dll' -File)
Check ($dlls.Count -ge 15) "随包分发的 DLL 有 $($dlls.Count) 个（期望 ≥15）"

# 随包的启动脚本必须**能被解析**。
#
# 为什么安装包这边要单独加这一条：这套验收**不执行** start.ps1（走真实入口
# `start.ps1 -Portable` 是绿色版那半边的活），所以"文件在不在"是这里原本唯一
# 能回答的问题 —— 而一个语法坏掉的 start.ps1 照样"在"。实际发生过：往横幅里
# 加一行提示时字符串里混进一对 ASCII 双引号，安装包 43 项全绿，用户双击
# start.bat 却只会看到一句指不到编码/引号的 "Missing closing '}'"。
# 详见 _verify_common.ps1 里 Test-ScriptSyntax 的注释。
$err = Test-ScriptSyntax (Join-Path $InstallDir 'start.ps1')
Check ($null -eq $err) "start.ps1 语法可解析$(if ($err) { " —— $err" })"

# exe 自己的图标和版本信息。三处图标互不相干：安装包用 SetupIconFile、
# 快捷方式用 IconFilename，而任务栏和资源管理器认的是**进程的 exe** ——
# 只做前两处的话，程序一跑起来任务栏上是个白底默认图标。
$iconExe = Join-Path $InstallDir 'desk2onvif.exe'
Check ([D2oPeRes]::Size($iconExe, 1, 14) -gt 0) 'exe 里有图标组（RT_GROUP_ICON）'
Check ([D2oPeRes]::Size($iconExe, 1, 3)  -gt 0) 'exe 里有图标位图（RT_ICON）'
Check ([D2oPeRes]::Size($iconExe, 1, 16) -gt 0) 'exe 里有版本资源（RT_VERSION）'
# 版本字符串里的中文是过了一遍 --codepage=65001 的，不读回来不知道对没对 ——
# 对不上不会报任何错，只会有人打开文件属性时看到乱码。
$vi = (Get-Item -LiteralPath $iconExe).VersionInfo
Check ($vi.ProductName -ceq 'Desk2ONVIF') "版本信息里的产品名 = $($vi.ProductName)"
Check ($vi.FileDescription -match 'ONVIF') "版本信息里的说明 = $($vi.FileDescription)"

# 前端产物：整套检查里最该在的一条 —— 少了它程序一个字都不报。
$webDist = Join-Path $InstallDir 'web\dist'
Check (Test-Path -LiteralPath (Join-Path $webDist 'index.html')) 'web\dist\index.html 存在'
$assets = @(Get-ChildItem -LiteralPath (Join-Path $webDist 'assets') -File -ErrorAction SilentlyContinue)
Check ($assets.Count -ge 6) "web\dist\assets 有 $($assets.Count) 个文件（期望 ≥6）"

# --- [3] 配置真的按参数写出来了 ---------------------------------------
Section "[3] 安装时的参数有没有落进 config.json"
$cfgPath = Join-Path $InstallDir 'config.json'
if (Test-Path -LiteralPath $cfgPath) {
    $cfg = $null
    try { $cfg = Get-Content -LiteralPath $cfgPath -Raw -Encoding UTF8 | ConvertFrom-Json }
    catch { Check $false 'config.json 是合法 JSON' $_.Exception.Message }

    if ($cfg) {
        Check ($cfg.rtsp.port -gt 0)  "rtsp.port = $($cfg.rtsp.port)"
        Check ($cfg.web.port  -gt 0)  "web.port = $($cfg.web.port)"
        Check ($cfg.web.host -in @('0.0.0.0', '127.0.0.1')) "web.host = $($cfg.web.host)"
        Check ($cfg.onvif.user -ceq $TestUser) "onvif.user = $($cfg.onvif.user)"
        # 这一条同时验两件事：JsonEscape 转义对了，而且转义出来的确实是
        # 能解析、且解析回来和输入一模一样的 JSON。
        Check ($cfg.onvif.password -ceq $TestPass) `
              'onvif.password 往返一致（引号 + 反斜杠）' `
              ("期望 " + $TestPass + "，实际 " + $cfg.onvif.password)
    }
} else {
    Check $false 'config.json 存在'
}

# --- [4] 装在包里的 exe 真的能起来（DLL 一个不缺）----------------------
Section "[4] 原地自检（--selfcheck）"
$exe = Join-Path $InstallDir 'desk2onvif.exe'
$sc = Invoke-Capture $exe @('--selfcheck') 60000 $InstallDir
Check (-not $sc.TimedOut) '--selfcheck 在 60 秒内结束'
Check ($sc.ExitCode -eq 0) "--selfcheck 退出码 = $($sc.ExitCode)"
# 退出码是硬断言，这一条是防"退出码拿到了但程序其实在报错"。
Check ($sc.Stdout -match '全部通过') '--selfcheck 报告"全部通过"'
Show $sc.Stdout 10

# --- [5] 起服务，拿真的契约检查打它 ------------------------------------
Section "[5] 起服务 + 前端契约（web_contract.py）"
$appOut = Join-Path $env:TEMP 'd2o-verify-app.out'
$appErr = Join-Path $env:TEMP 'd2o-verify-app.err'

$appArgs = '--rtsp --rtsp-port ' + $RtspPort +
           ' --http --http-port ' + $HttpPort +
           ' --web-root "' + $webDist + '"' +
           ' --config "' + $cfgPath + '"'
$app = Start-Detached $exe $appArgs $InstallDir $appOut $appErr

try {
    $up = Wait-Backend $HttpPort
    Check $up "后端在 http://127.0.0.1:$HttpPort 起来了"
    if (-not $up) { Show (Get-Content -LiteralPath $appErr -Raw -Encoding UTF8 -ErrorAction SilentlyContinue) 20 }

    if ($up) {
        $wc = Invoke-PythonProbe (Join-Path $root 'tests\web_contract.py') `
                                 @('--port', "$HttpPort", '--web-root', $webDist) 180000 $root
        if (-not $wc) {
            Check $false '找得到 python（web_contract.py 要用）'
        } else {
            Check (-not $wc.TimedOut) 'web_contract.py 在 180 秒内结束'
            Check ($wc.ExitCode -eq 0) "web_contract.py 退出码 = $($wc.ExitCode)"
            # 退出码万一又拿不到，"失败 0 项"这一行还在输出里，双保险。
            Check ($wc.Stdout -match '失败 0 项') 'web_contract.py 报告"失败 0 项"'
            Show $wc.Stdout 6
        }
    }
} finally {
    Stop-Desk2Onvif
    try { if (-not $app.Process.HasExited) { $app.Process.Kill() } } catch {}
    Remove-Item -LiteralPath $app.CmdFile -Force -ErrorAction SilentlyContinue
}
Wait-PortFree @($RtspPort, $HttpPort)

# --- [6] 静默卸载，必须一点不剩 ----------------------------------------
Section "[6] 静默卸载"
if ($Keep) {
    Write-Host '  （-Keep：跳过卸载）' -ForegroundColor Yellow
} else {
    $un = Join-Path $InstallDir 'unins000.exe'
    if (Test-Path -LiteralPath $un) {
        $ui = Invoke-Capture $un @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') 180000 $InstallDir
        # 退出码在这里**不是**判据：Inno 的卸载器会把自己复制到 %TEMP% 再跑，
        # 父进程立刻返回 0。所以它恒为 0，挂住的话挂的是那个副本。
        # 真正的判据是下面那个"目录没了没有"，它同时对两种故障有效：
        # 卸载挂住（超时后目录还在）和卸载不干净（跑完了目录还在）。
        Check (-not $ui.TimedOut) '卸载进程在 180 秒内返回'
    } else {
        Check $false '有 unins000.exe'
    }

    # [UninstallDelete] 那两条就是为这一行存在的：config.json 不在 Inno 的
    # 安装清单里，不显式删就会剩下来，目录于是不空、也不会被移除。
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

Remove-Item Env:\DESK2ONVIF_USER, Env:\DESK2ONVIF_PASSWORD -ErrorAction SilentlyContinue
Write-VerifySummary
