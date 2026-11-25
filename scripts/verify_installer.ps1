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
      - **默认装的是 Windows 服务**（issue #8）。服务是不是真的注册成
        AUTO_START、是不是从 ProgramData 那份绝对路径配置读、卸载时是不是
        先停服务再删文件 —— 这些只有真装一遍才知道。控制台形态也至少要验
        一次安装产物（start-installed.bat）。

    这个脚本会占用**机器级**的 %ProgramData%\Desk2ONVIF（配置/日志）和公共
    桌面/开始菜单。所以：
      · 机器上已经有一个**不属于本验证目录**的 Desk2ONVIF 服务时直接中止 ——
        否则会把用户真正的安装卸掉；
      · 上一次验证留下的服务会先清掉（按服务命令行里的安装目录判定）。

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
$logConsole = Join-Path $env:TEMP 'd2o-verify-install-console.log'

# 安装版（0.2.0 起）把配置/日志放在机器级目录。这个脚本会覆盖并最终删除它，
# 所以开头会明确提示，且拒绝动一个不属于本次验证目录的服务。
$progDataDir = Join-Path $env:ProgramData 'Desk2ONVIF'
$progDataCfg = Join-Path $progDataDir 'config.json'
$commonDesktop = [Environment]::GetFolderPath('CommonDesktopDirectory')
$commonPrograms = [Environment]::GetFolderPath('CommonPrograms')
$groupDir = Join-Path $commonPrograms 'Desk2ONVIF'

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

# --- [0] 前置：服务 / 进程 / 端口 / 上一次的残留 ------------------------
Section "[0] 前置检查"

# **先看服务，再看端口。** 上一次验证失败时可能留下一个正在跑的服务，它正占
# 着 554/8080；先按端口判"不干净"会把清理对象误报成环境问题。
$svcQuery = Invoke-Capture 'sc.exe' @('query', 'Desk2ONVIF') 20000 $root
if ($svcQuery.ExitCode -eq 0) {
    $svcCfg = Invoke-Capture 'sc.exe' @('qc', 'Desk2ONVIF') 20000 $root
    $ours = $svcCfg.Stdout -and
            ($svcCfg.Stdout.IndexOf($InstallDir, [StringComparison]::OrdinalIgnoreCase) -ge 0)
    if (-not $ours) {
        Write-Host ''
        Write-Host '  机器上已经有一个 Desk2ONVIF 服务，而它的安装目录不是本次验证目录：' -ForegroundColor Red
        Show $svcCfg.Stdout 8
        Write-Host '  这个脚本要靠卸载/重装服务来验收，不能动一个真实的安装。' -ForegroundColor Red
        Write-Host '  先在“服务”里停掉/删掉它，或换一台机器再验。' -ForegroundColor Red
        exit 1
    }
    Write-Host '  （清理上一次验证留下的服务…）'
    $oldExe = Join-Path $InstallDir 'desk2onvif.exe'
    if (Test-Path -LiteralPath $oldExe) {
        $null = Invoke-Capture $oldExe @('--service', 'uninstall') 90000 $InstallDir
    } else {
        # 安装目录被删了一半：退回到 sc。先停再删，和 exe 里的顺序一致。
        $null = Invoke-Capture 'sc.exe' @('stop', 'Desk2ONVIF') 60000 $root
        $null = Invoke-Capture 'sc.exe' @('delete', 'Desk2ONVIF') 20000 $root
    }
    for ($i = 0; $i -lt 60; $i++) {
        if ((Invoke-Capture 'sc.exe' @('query', 'Desk2ONVIF') 20000 $root).ExitCode -ne 0) { break }
        Start-Sleep -Milliseconds 500
    }
    # --service uninstall 只等服务进程停；worker 晚一点退出。等它退完再动目录。
    for ($i = 0; $i -lt 60 -and @(Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue).Count -gt 0; $i++) {
        Start-Sleep -Milliseconds 500
    }
}
Check ((Invoke-Capture 'sc.exe' @('query', 'Desk2ONVIF') 20000 $root).ExitCode -ne 0) `
      '没有已存在的 Desk2ONVIF 服务' `
      '这个脚本会先 --service uninstall 再装，不能拿真实安装来验。'

$running = @(Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue)
Check ($running.Count -eq 0) "没有正在运行的 desk2onvif（找到 $($running.Count) 个）" `
      '先关掉它再验：多个实例抢同一个 RTSP 端口，验出来的是噪音。'
foreach ($pt in @($RtspPort, $HttpPort)) { Check (-not (Test-PortBusy $pt)) "端口 $pt 空闲" }

if (Test-Path -LiteralPath $progDataCfg) {
    Write-Host "  （注意：$progDataCfg 事先存在；安装会覆盖它，静默卸载会删掉它）" -ForegroundColor Yellow
    Write-Host '  （如果这是你在用的安装，先备份；验证本身没有备份这一步。）' -ForegroundColor Yellow
}

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

# --- [1] 静默安装（默认任务：装成服务）---------------------------------
Section "[1] 静默安装（默认勾选“安装为 Windows 服务”）"
if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }

$env:DESK2ONVIF_USER = $TestUser
$env:DESK2ONVIF_PASSWORD = $TestPass

# **故意不传 /MERGETASKS**：这一轮要验的就是 [Tasks] 里 installservice 的
# 默认勾选（checkedonce）。传 /MERGETASKS=installservice 只能证明"参数管用"，
# 证明不了"用户一路点下一步也会装服务"。
# 端口显式传给安装器：服务会按 config.json 里的端口监听，后面的探针打的就是
# 这两个端口。/D2OSCOPE=local 让 worker 只绑 127.0.0.1（CI 上不暴露、也不
# 依赖防火墙）。
$sw = @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
    "/DIR=$InstallDir",
    "/LOG=$log",
    "/D2ORTSPORT=$RtspPort",
    "/D2OHTTPPORT=$HttpPort",
    '/D2OSCOPE=local'
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

# --- [1b] 默认任务真的装出了服务 ---------------------------------------
#
# 这一段是 issue #8 的核心：[Tasks] 里 installservice 默认 selected，[Run]
# 里 --service install --start。三条都要量：
#   · 服务对象存在且 RUNNING（不是只写好注册表就完事）
#   · 启动类型 AUTO_START（CreateService 里写死的）
#   · 服务命令行里的 --config 是 ProgramData 的**绝对路径**
Section "[1b] 服务已注册并启动（默认任务）"
$svcRunning = $false
for ($i = 0; $i -lt 120; $i++) {
    $q = Invoke-Capture 'sc.exe' @('query', 'Desk2ONVIF') 20000 $root
    if ($q.ExitCode -eq 0 -and $q.Stdout -match 'RUNNING') { $svcRunning = $true; break }
    Start-Sleep -Milliseconds 500
}
Check $svcRunning '服务 Desk2ONVIF 是 RUNNING（默认任务 installservice 生效）' `
      '--service install --start 失败，或者任务没选中。看安装日志末尾和 service.log。'
if (-not $svcRunning) {
    $q = Invoke-Capture 'sc.exe' @('query', 'Desk2ONVIF') 20000 $root
    Show $q.Stdout 12
    Show (Get-Content -LiteralPath (Join-Path $progDataDir 'service.log') -Raw -Encoding UTF8 -ErrorAction SilentlyContinue) 25
}

$svcCfg = Invoke-Capture 'sc.exe' @('qc', 'Desk2ONVIF') 20000 $root
Check ($svcCfg.Stdout -match 'AUTO_START') `
      '服务启动类型 = AUTO_START' `
      '--service install 里的 SERVICE_AUTO_START 没生效；重启后服务不会自己起来。'
Check ($svcCfg.Stdout -and
       ($svcCfg.Stdout.IndexOf($progDataCfg, [StringComparison]::OrdinalIgnoreCase) -ge 0)) `
      "服务命令行带 --config $progDataCfg" `
      '相对路径会让服务在 C:\Windows\System32 下读一份全新的默认配置 —— 端口/口令全不对，日志里却没有任何错。'

# --- [2] 装下来的东西对不对 -------------------------------------------
Section "[2] 安装内容"

# README.md 必须是**文件**。曾经写成 DestDir={app}\README.md，于是装出来
# 一个叫 README.md 的目录，[Icons] 里"打开使用说明"点开是个文件夹。
$readme = Join-Path $InstallDir 'README.md'
Check ((Test-Path -LiteralPath $readme) -and -not (Get-Item -LiteralPath $readme).PSIsContainer) `
      'README.md 是文件（不是同名目录）'

# config.json 不在安装目录里了 —— 它现在在 %ProgramData%\Desk2ONVIF。[3] 段验。
foreach ($f in @('desk2onvif.exe', 'start.bat', 'start.ps1',
                 'start-local.bat', 'start-installed.bat',
                 'service-status.bat', 'wait-worker-exit.bat',
                 'list-displays.bat', 'selfcheck.bat',
                 'firewall-allow.bat', 'firewall-print.bat',
                 'LICENSE', 'LICENSE-GPL-3.0.txt', 'THIRD-PARTY.md',
                 'desk2onvif.ico', 'unins000.exe')) {
    Check (Test-Path -LiteralPath (Join-Path $InstallDir $f)) "有 $f"
}

# 安装版新增的 .bat 必须和 start.bat 一样是纯 ASCII：它们由 cmd.exe 在 936
# 代码页下解析，中文注释里的一对引号会被拼错，报的是指不到原因（也指不到
# 编码）的语法错。这条只有安装目录里的东西能验。
foreach ($bat in @('start-installed.bat', 'service-status.bat', 'wait-worker-exit.bat')) {
    $bp = Join-Path $InstallDir $bat
    if (Test-Path -LiteralPath $bp) {
        $bytes = [IO.File]::ReadAllBytes($bp)
        $nonAscii = @($bytes | Where-Object { $_ -gt 127 })
        Check ($nonAscii.Count -eq 0) "$bat 是纯 ASCII（非 ASCII 字节 $($nonAscii.Count) 个）" `
              '936 代码页下非 ASCII 字节会把引号配错；这些 .bat 必须只含 ASCII。'
    }
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

# --- 版本号：三方一致（第 1、2 处）-------------------------------------
#
# 一个包里的版本号有四处会说：包名、exe 的文件属性、程序在网络上报的
# `/api/status`（也就是 `--version` 那个数）、以及 config.json 里的
# `onvif.firmware_version`（NVR 通过 GetDeviceInformation 看得到）。四处全来自
# `CMakeLists.txt` 的 `project(... VERSION)` 一行。
#
# 但"全都来自一处"本身是一句**承诺，不是检查** —— 本轮动手之前，
# `package.ps1` 的注释就白纸黑字写着"必须是同一个"，而实操是：exe 的
# VersionInfo 硬编码在 resources\desk2onvif.rc 里，压根没接上，升到 0.2.0 之后
# 资源管理器里那个文件属性仍然写着 0.1.0。所以这里把它**量三遍再比**。
#
# 包名当锚：它是唯一一个不依赖任何构建产物、用户下载到手里的那个名字。
$nameVer = [regex]::Match((Split-Path $Setup -Leaf),
                          'Desk2ONVIF_Setup_(\d+\.\d+\.\d+)\.exe').Groups[1].Value
Check (-not [string]::IsNullOrEmpty($nameVer)) "包名里有版本号（$nameVer）"
Check ($vi.FileVersion -ceq $nameVer) `
      "exe 的文件属性版本 = 包名里的版本（属性 $($vi.FileVersion)，包名 $nameVer）" `
      '对不上说明 resources\desk2onvif.rc.in 没接上 project(... VERSION)，或者包是用旧 exe 打的。'

# 前端产物：整套检查里最该在的一条 —— 少了它程序一个字都不报。
$webDist = Join-Path $InstallDir 'web\dist'
Check (Test-Path -LiteralPath (Join-Path $webDist 'index.html')) 'web\dist\index.html 存在'
$assets = @(Get-ChildItem -LiteralPath (Join-Path $webDist 'assets') -File -ErrorAction SilentlyContinue)
Check ($assets.Count -ge 6) "web\dist\assets 有 $($assets.Count) 个文件（期望 ≥6）"

# --- [2b] 服务形态的入口：桌面 / 开始菜单是 URL 快捷方式 ----------------
#
# admin 安装的 {autodesktop} 是**公共桌面**（C:\Users\Public\Desktop），不是
# 当前用户的 Desktop。开始菜单同理在 %ProgramData%\Microsoft\Windows\...
# 这一条验的是"点快捷方式真的会打开控制台"，而不是"文件建出来了"：
# .url 里的 URL= 要和安装时选的 Web 端口一致。
Section "[2b] 桌面 / 开始菜单的 URL 快捷方式"
$wantUrl = "URL=http://127.0.0.1:$HttpPort/"
$deskUrl = Join-Path $commonDesktop 'Desk2ONVIF.url'
Check (Test-Path -LiteralPath $deskUrl) "公共桌面上有 Desk2ONVIF.url（$deskUrl）"
if (Test-Path -LiteralPath $deskUrl) {
    # [IO.File]::ReadAllText 会按 BOM 识别 UTF-8/UTF-16；-Encoding Default 在
    # Inno 若把 .url 写成 UTF-16 时会读到一串夹着 NUL 的乱码，断言就永远红。
    $urlText = [IO.File]::ReadAllText($deskUrl)
    Check ($urlText -match [regex]::Escape($wantUrl)) `
          "桌面 URL 指向 $wantUrl" `
          ("实际内容：" + ($urlText -replace "`r?`n", ' '))
}

$groupUrls = @(Get-ChildItem -LiteralPath $groupDir -Filter '*.url' -File -ErrorAction SilentlyContinue)
Check ($groupUrls.Count -ge 1) '开始菜单有 URL 快捷方式'
$groupHit = $false
foreach ($gu in $groupUrls) {
    $t = [IO.File]::ReadAllText($gu.FullName)
    if ($t -match [regex]::Escape($wantUrl)) { $groupHit = $true }
}
Check $groupHit "开始菜单 URL 快捷方式同样指向 $wantUrl"
Check (Test-Path -LiteralPath (Join-Path $groupDir '服务状态.lnk')) `
      '开始菜单有“服务状态”入口'
Check (-not (Test-Path -LiteralPath (Join-Path $groupDir '启动 Desk2ONVIF.lnk'))) `
      '服务形态没有控制台启动快捷方式（那是控制台形态的）'
Check (-not (Test-Path -LiteralPath (Join-Path $commonDesktop 'Desk2ONVIF.lnk'))) `
      '服务形态桌面上没有控制台 .lnk（升级时 [InstallDelete] 清掉了旧的）'

# --- [3] 配置真的按参数写出来了 ---------------------------------------
#
# 路径变了：0.2.0 及以前写在 {app}\config.json，现在是机器级的
# %ProgramData%\Desk2ONVIF\config.json（Program Files 对普通用户只读，
# 而 worker 以登录用户身份跑，网页保存配置要写它）。
Section "[3] 安装时的参数有没有落进 %ProgramData% 的 config.json"
$cfgPath = $progDataCfg
if (Test-Path -LiteralPath $cfgPath) {
    $cfg = $null
    try { $cfg = Get-Content -LiteralPath $cfgPath -Raw -Encoding UTF8 | ConvertFrom-Json }
    catch { Check $false 'config.json 是合法 JSON' $_.Exception.Message }

    if ($cfg) {
        Check ($cfg.rtsp.port -eq $RtspPort) "rtsp.port = $($cfg.rtsp.port)（安装时传的是 $RtspPort）"
        Check ($cfg.web.port  -eq $HttpPort) "web.port = $($cfg.web.port)（安装时传的是 $HttpPort）"
        Check ($cfg.web.host -ceq '127.0.0.1') "web.host = $($cfg.web.host)（/D2OSCOPE=local）"
        Check ($cfg.onvif.user -ceq $TestUser) "onvif.user = $($cfg.onvif.user)"
        # 这一条同时验两件事：JsonEscape 转义对了，而且转义出来的确实是
        # 能解析、且解析回来和输入一模一样的 JSON。
        Check ($cfg.onvif.password -ceq $TestPass) `
              'onvif.password 往返一致（引号 + 反斜杠）' `
              ("期望 " + $TestPass + "，实际 " + $cfg.onvif.password)

        # Web 那一对。**这一条是用户报过的那条 bug 的正脸**：`.iss` 的 WriteConfig
        # 原来只写了 onvif 那一组，于是 Web 控制台永远拿出厂 admin/admin ——
        # 用户"在安装时设了口令、网页却怎么都登不进去"，而日志、安装日志、程序
        # 输出里一个字都没提。它和上面那两条**不是同一件事**：ONVIF 那份一直是
        # 对的，错的是 web 那份。（详见 desk2onvif.iss 里 WriteConfig 上面那段。）
        Check ($cfg.web.user -ceq $TestUser) "web.user = $($cfg.web.user)"
        Check ($cfg.web.password -ceq $TestPass) `
              'web.password 往返一致（引号 + 反斜杠）' `
              ("期望 " + $TestPass + "，实际 " + $cfg.web.password)
    }
} else {
    Check $false "config.json 存在（$cfgPath）"
}

# 否定式判据：安装目录里**不能**再有 config.json。有的话说明 WriteConfig 还
# 在往 Program Files 写 —— 那条路在普通用户下保存配置会失败，而失败的表现
# 只是"改了配置存不下来"。
Check (-not (Test-Path -LiteralPath (Join-Path $InstallDir 'config.json'))) `
      'config.json 没有落在 Program Files 安装目录里'

# --- [4] 装在包里的 exe 真的能起来（DLL 一个不缺）----------------------
Section "[4] 原地自检（--selfcheck）"
$exe = Join-Path $InstallDir 'desk2onvif.exe'
$sc = Invoke-Capture $exe @('--selfcheck') 60000 $InstallDir
Check (-not $sc.TimedOut) '--selfcheck 在 60 秒内结束'
Check ($sc.ExitCode -eq 0) "--selfcheck 退出码 = $($sc.ExitCode)"
# 退出码是硬断言，这一条是防"退出码拿到了但程序其实在报错"。
Check ($sc.Stdout -match '全部通过') '--selfcheck 报告"全部通过"'
Show $sc.Stdout 10

# --- [5] 服务真的把 Web 起来了：前端契约 + 版本 ------------------------
#
# **不再用 start.ps1 -Portable 自己起一个进程。** 默认安装已经由服务在登录
# 用户的会话里拉起了 worker（`--worker --parent-pid …`），HTTP 就是它在监听。
# 这里打的就是那个进程 —— 顺带验了"服务命令行里的 --config 真的被 worker
# 用上了"：如果 worker 读的是另一份配置，端口和口令都对不上。
Section "[5] 服务起来的 Web：前端契约 + 运行期版本"
$serviceLog = Join-Path $progDataDir 'service.log'
$workerLog = Join-Path $progDataDir 'desk2onvif.log'

# 服务启动是异步的，worker 又要等 SCM 拉起服务后再 CreateProcessAsUser。
# 60 秒（120 × 500ms）是给冷启动留的余量。
$up = Wait-Backend -Port $HttpPort -Tries 120
Check $up "后端在 http://127.0.0.1:$HttpPort 起来了（由服务拉起）"
if (-not $up) {
    Write-Host '  --- service.log（最后 30 行）---' -ForegroundColor DarkGray
    Show (Get-Content -LiteralPath $serviceLog -Raw -Encoding UTF8 -ErrorAction SilentlyContinue) 30
    Write-Host '  --- desk2onvif.log（最后 20 行）---' -ForegroundColor DarkGray
    Show (Get-Content -LiteralPath $workerLog -Raw -Encoding UTF8 -ErrorAction SilentlyContinue) 20
    Write-Host '  （服务 RUNNING 但没有 worker：确认 runner 有已登录的交互会话 ——' -ForegroundColor Yellow
    Write-Host '    worker 必须被拉进 Session 1，DXGI 才抓得到桌面。）' -ForegroundColor Yellow
}

if ($up) {
    # 凭据要**显式递给探针**：这份配置里的 web.user/web.password 是安装时
    # 填的那一对（上面 [3] 段刚验过），探针的默认值是出厂 admin/admin，
    # 不给就登不进去。顺带这一条也把"口令里的引号能原样穿过 Python 的命令行"
    # 量掉了（`_verify_common.ps1` 的 Quote-Arg 就是为它修的）。
    $wc = Invoke-PythonProbe (Join-Path $root 'tests\web_contract.py') `
                             @('--port', "$HttpPort", '--web-root', $webDist,
                               '--user', $TestUser, '--password', $TestPass) 180000 $root
    if (-not $wc) {
        Check $false '找得到 python（web_contract.py 要用）'
    } else {
        Check (-not $wc.TimedOut) 'web_contract.py 在 180 秒内结束'
        Check ($wc.ExitCode -eq 0) "web_contract.py 退出码 = $($wc.ExitCode)"
        # 退出码万一又拿不到，"失败 0 项"这一行还在输出里，双保险。
        Check ($wc.Stdout -match '失败 0 项') 'web_contract.py 报告"失败 0 项"'
        # 红了就把**红的那些行**逐条打出来（只打尾巴的话，第 30 行的失败
        # 会被最后 6 行挤掉，报告上只剩一句"退出码 = 1"）。
        if (-not $wc.TimedOut -and $wc.ExitCode -ne 0) {
            Show-Failures $wc.Stdout (Join-Path $root 'out\web_contract.txt')
            Show $wc.Stdout 6
        }
    }

    # 三方一致的第 3 处：**运行期**报出来的版本。
    #
    # 这一条和 [2] 段那两条问的不是同一件事。那两条证明"资源编对了"，
    # 而它证明"那个数真的接上了网络这条路"—— `patch_live_counters` 里那一行
    # 是 `/api/status` 与 `/ws/stats` 两条路共用的补丁点，加错了地方
    # （比如只加在 handler 里）会让两个接口的字段集合不一致，而单看 HTTP
    # 这条路的 version 却完全正常。
    $apiVer = Get-StatusVersion -Port $HttpPort -User $TestUser -Password $TestPass
    if ($null -eq $apiVer) {
        Check $false '能从 /api/status 读到 version' `
              ("登录或请求失败了。这份配置里应当是 web.user = $TestUser（装的时候填的那一对），" +
               '它在 [3] 段里刚被验过 —— 所以这里红了要往"这一对没被真的用上"那条路查。')
    } else {
        Check ($apiVer -ceq $nameVer) `
              "运行期 /api/status 的 version = 包名里的版本（接口 $apiVer，包名 $nameVer）" `
              '对不上说明 common\version.h 那条 D2O_VERSION 没接上，或者包是拿旧 exe 打的。'
    }

    # 登录契约（web_auth.py）。**放在最后**：它最后几步会故意连续失败登录，
    # 把来源 IP 打进登录冷却，之后所有登录都会 429 —— 上面 Get-StatusVersion
    # 的那次登录必须已经跑完，否则会误报"口令没生效"。
    $wa = Invoke-PythonProbe (Join-Path $root 'tests\web_auth.py') `
                             @('--port', "$HttpPort", '--user', $TestUser, '--password', $TestPass) 180000 $root
    if (-not $wa) {
        Check $false '找得到 python（web_auth.py 要用）'
    } else {
        Check (-not $wa.TimedOut) 'web_auth.py 在 180 秒内结束'
        Check ($wa.ExitCode -eq 0) "web_auth.py 退出码 = $($wa.ExitCode)"
        Check ($wa.Stdout -match '失败 0 项') 'web_auth.py 报告"失败 0 项"'
        if (-not $wa.TimedOut -and $wa.ExitCode -ne 0) {
            Show-Failures $wa.Stdout (Join-Path $root 'out\web_auth.txt')
            Show $wa.Stdout 6
        }
    }
}

# 这里**不 Stop-Desk2Onvif / Wait-PortFree**：worker 是服务的一部分，要留到
# 下面卸载时才停。提前杀掉它，服务会再拉一个，反而让卸载时序更难判。

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

    # [UninstallDelete] 里那两条（ProgramData 目录 + 旧的 {app} 残留）就是为
    # 这一步存在的：config.json / service.log / desk2onvif.log 都不在 Inno 的
    # 安装清单里，不显式删就会剩下来，目录于是不空、也不会被移除。
    $gone = $false
    for ($i = 0; $i -lt 120; $i++) {
        if (-not (Test-Path -LiteralPath $InstallDir)) { $gone = $true; break }
        Start-Sleep -Milliseconds 500
    }
    Check $gone "安装目录整个没了（$InstallDir）"
    if (-not $gone) {
        $left = @(Get-ChildItem -LiteralPath $InstallDir -Recurse -Force -ErrorAction SilentlyContinue |
                  ForEach-Object { $_.FullName.Substring($InstallDir.Length + 1) })
        Check $false "卸载没有留下东西（还剩 $($left.Count) 项）" ($left -join ', ')
    }

    # 服务对象必须没了。sc query 在服务不存在时退 1060；不用看文本。
    $svcGone = $false
    for ($i = 0; $i -lt 120; $i++) {
        if ((Invoke-Capture 'sc.exe' @('query', 'Desk2ONVIF') 20000 $root).ExitCode -ne 0) {
            $svcGone = $true; break
        }
        Start-Sleep -Milliseconds 500
    }
    Check $svcGone '卸载后服务 Desk2ONVIF 已删除'

    # 静默卸载不问"保留配置"，默认行为是连 ProgramData 一起删。交互式那条
    # "保留"的路会先复制到 %APPDATA%，这里走不到。
    $cfgGone = $false
    for ($i = 0; $i -lt 120; $i++) {
        if (-not (Test-Path -LiteralPath $progDataCfg)) { $cfgGone = $true; break }
        Start-Sleep -Milliseconds 500
    }
    Check $cfgGone "静默卸载删掉了 $progDataCfg"
    Check (-not (Test-Path -LiteralPath $progDataDir)) "卸载后 $progDataDir 整个没了" `
          ('还剩：' + (@(Get-ChildItem -LiteralPath $progDataDir -Recurse -Force -ErrorAction SilentlyContinue |
                          ForEach-Object { $_.FullName.Substring($progDataDir.Length + 1) }) -join ', '))

    # 图标是 Inno 记录的，卸载应一并删掉。admin 安装的 {autodesktop} 是**公共**
    # 桌面，不是 %USERPROFILE%\Desktop。
    Check (-not (Test-Path -LiteralPath (Join-Path $commonDesktop 'Desk2ONVIF.url'))) `
          '卸载删掉了公共桌面的 URL 快捷方式'
    Check (-not (Test-Path -LiteralPath $groupDir)) `
          "卸载删掉了开始菜单文件夹（$groupDir）"

    Check (@(Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue).Count -eq 0) '卸载后没有残留进程'
}

if ($Keep) {
    Write-Host '  （-Keep：保留服务安装，跳过控制台形态这一段）' -ForegroundColor Yellow
    Remove-Item Env:\DESK2ONVIF_USER, Env:\DESK2ONVIF_PASSWORD -ErrorAction SilentlyContinue
    Write-VerifySummary
}

# --- [7] 控制台形态（不勾服务）------------------------------------------
#
# 服务的默认路径已经验完。控制台形态至少要证明三件事：
#   · 安装仍然成功，start-installed.bat 在（任务要求的最低限度）；
#   · 没有注册服务（[Run] 的 --service install 被 Check: IsConsoleMode 跳过）；
#   · 桌面/开始菜单指向的是 .lnk（目标是 .bat），不是 URL。
Section "[7] 控制台形态安装（/MERGETASKS=!installservice）"
if (Test-Path -LiteralPath $logConsole) { Remove-Item -LiteralPath $logConsole -Force }
$sw2 = @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
    "/DIR=$InstallDir",
    "/LOG=$logConsole",
    "/D2ORTSPORT=$RtspPort",
    "/D2OHTTPPORT=$HttpPort",
    '/D2OSCOPE=local',
    '/MERGETASKS=!installservice,desktopicon'
)
Write-Host ("  命令行  " + ($sw2 -join ' ') + "  (+ DESK2ONVIF_USER / DESK2ONVIF_PASSWORD 环境变量)")
$inst2 = Invoke-Capture $Setup $sw2 600000 $root
if ($inst2.TimedOut) {
    Check $false '控制台模式安装进程在 600 秒内结束' '挂住了 —— 看 /LOG 最后一行。'
} else {
    Check ($inst2.ExitCode -eq 0) "控制台模式安装退出码 = $($inst2.ExitCode)"
}
Check (Test-Path -LiteralPath (Join-Path $InstallDir 'start-installed.bat')) '有 start-installed.bat'
Check ((Invoke-Capture 'sc.exe' @('query', 'Desk2ONVIF') 20000 $root).ExitCode -ne 0) `
      '控制台模式没有注册 Desk2ONVIF 服务' `
      '不勾服务时 [Run] 的 --service install 必须被 Check: IsConsoleMode 跳过。'
Check (Test-Path -LiteralPath $progDataCfg) '控制台模式也把配置写到 ProgramData'

# 桌面快捷方式：admin 安装 -> 公共桌面。控制台形态是 .lnk（目标是 .bat），
# 服务形态才是 .url。用 WScript.Shell 读目标，比只看文件名多一条判据。
$deskLnk = Join-Path $commonDesktop 'Desk2ONVIF.lnk'
$deskUrl = Join-Path $commonDesktop 'Desk2ONVIF.url'
Check (Test-Path -LiteralPath $deskLnk) '公共桌面上是 .lnk（控制台形态）'
Check (-not (Test-Path -LiteralPath $deskUrl)) '控制台形态没有把 URL 快捷方式放到桌面'
if (Test-Path -LiteralPath $deskLnk) {
    try {
        $wsh = New-Object -ComObject WScript.Shell
        $target = $wsh.CreateShortcut($deskLnk).TargetPath
        Check ($target -ieq (Join-Path $InstallDir 'start-installed.bat')) `
              "桌面快捷方式指向 start-installed.bat（$target）"
    } catch {
        Check $false '能读出桌面 .lnk 的目标' $_.Exception.Message
    }
}

# 开始菜单：控制台形态只有“启动”，没有“打开 Web 控制台 / 服务状态”。
Check (Test-Path -LiteralPath (Join-Path $groupDir '启动 Desk2ONVIF.lnk')) `
      '开始菜单有“启动 Desk2ONVIF”（控制台形态）'
Check (-not (Test-Path -LiteralPath (Join-Path $groupDir '打开 Web 控制台.url'))) `
      '控制台形态没有开始菜单 URL 快捷方式'
Check (-not (Test-Path -LiteralPath (Join-Path $groupDir '服务状态.lnk'))) `
      '控制台形态没有服务状态入口（那是服务形态的）'

# 不启动它：这一段只验安装产物（任务要求的最低限度）。启动路径在服务模式
# 已经由真实的 worker + web_contract 验过了。

# --- [8] 控制台形态卸载 -------------------------------------------------
Section "[8] 控制台形态卸载"
$un2 = Join-Path $InstallDir 'unins000.exe'
if (Test-Path -LiteralPath $un2) {
    $ui2 = Invoke-Capture $un2 @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') 180000 $InstallDir
    Check (-not $ui2.TimedOut) '控制台模式卸载进程在 180 秒内返回'
} else {
    Check $false '控制台模式有 unins000.exe'
}
$gone2 = $false
for ($i = 0; $i -lt 120; $i++) {
    if (-not (Test-Path -LiteralPath $InstallDir)) { $gone2 = $true; break }
    Start-Sleep -Milliseconds 500
}
Check $gone2 "控制台模式安装目录整个没了（$InstallDir）"
Check ((Invoke-Capture 'sc.exe' @('query', 'Desk2ONVIF') 20000 $root).ExitCode -ne 0) `
      '控制台模式卸载后仍然没有服务'
$cfgGone2 = $false
for ($i = 0; $i -lt 120; $i++) {
    if (-not (Test-Path -LiteralPath $progDataCfg)) { $cfgGone2 = $true; break }
    Start-Sleep -Milliseconds 500
}
Check $cfgGone2 "控制台模式静默卸载删掉了 $progDataCfg"
Check (-not (Test-Path -LiteralPath $progDataDir)) "控制台模式卸载后 $progDataDir 整个没了"
Check (-not (Test-Path -LiteralPath $deskLnk)) '控制台模式卸载删掉了桌面 .lnk'
Check (-not (Test-Path -LiteralPath $groupDir)) '控制台模式卸载删掉了开始菜单文件夹'
Check (@(Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue).Count -eq 0) '控制台模式卸载后没有残留进程'

Remove-Item Env:\DESK2ONVIF_USER, Env:\DESK2ONVIF_PASSWORD -ErrorAction SilentlyContinue
Write-VerifySummary
