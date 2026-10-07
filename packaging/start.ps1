#Requires -Version 5.1
<#
  启动 Desk2ONVIF：把本机桌面模拟成一台 ONVIF Profile S 摄像机。

  从 start.bat 双击进来，或者自己带参数跑：

      .\start.ps1                          # 默认：RTSP 554 + Web 8080 + ONVIF
      .\start.ps1 -LocalOnly               # 只监听 127.0.0.1，不对局域网开放
      .\start.ps1 -RtspPort 8554           # 554 被占时换个端口
      .\start.ps1 -NoOnvif                 # 只推流，不做 ONVIF 设备模拟
      .\start.ps1 -Extra '--fps','60'      # 任何原样透传给 exe 的参数

  这个脚本只做三件事：把路径拼对、把地址打出来、把进程跑起来。
  真正的参数解析在 exe 里（exe --help 是唯一的权威来源）。
#>
[CmdletBinding()]
param(
    [int]    $RtspPort = 554,
    [int]    $HttpPort = 8080,
    [switch] $LocalOnly,
    [switch] $NoSub,
    [switch] $NoOnvif,
    [string] $OnvifUser = 'admin',
    [string] $OnvifPassword = 'admin',
    # 原样透传给 exe 的额外参数，例如 -Extra '--fps','60'
    [string[]] $Extra = @(),
    # 绿色版模式：配置文件放在 exe 旁边，整个目录自包含、拷走就能用。
    # 不给这个开关时（安装版）用程序自己的默认位置
    # %APPDATA%\Desk2ONVIF\config.json —— 那是 Windows 上应用配置该在的地方，
    # 而且装在 Program Files 之类的只读目录时，往 exe 旁边写是要管理员权限的。
    [switch] $Portable,
    # 认不出来的参数也照收 —— 这样 `start.bat --fps 60` 能直接用，
    # 而不必逼人写成 -Extra '--fps','60'。这些同样原样透传给 exe。
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Rest
)

$ErrorActionPreference = 'Stop'

# --- 路径全部相对脚本自己，和当前工作目录无关 -----------------------------
#
# 这一点是必需的，不是风格问题：exe 的 `--web-root` 默认值是**相对路径**
# `web/dist`，而"目录不存在"被**刻意**当成"没挂静态资源"而不是错误
#（见 src/main.cpp 里那段注释：Phase 5 之前根本没有 web/dist，而那时
# API 和 WS 就该能用）。所以从别的目录启动 exe 的话，界面会安静地不出现，
# 命令照跑、推流照推 —— 只有"网页打不开"这一个症状。
$root = $PSScriptRoot
$exe  = Join-Path $root 'desk2onvif.exe'
$web  = Join-Path $root 'web\dist'

if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "[x] 找不到 $exe" -ForegroundColor Red
    Write-Host "    这个脚本必须和 desk2onvif.exe 放在同一个目录里。"
    exit 1
}
if (-not (Test-Path -LiteralPath $web)) {
    Write-Host "[!] 找不到 $web —— 会起服务，但**网页界面打不开**。" -ForegroundColor Yellow
    Write-Host "    推流和 ONVIF 不受影响，只是 /api 之外的页面没有静态资源。"
}

# --- 组参数 ---------------------------------------------------------------
$argv = @(
    '--rtsp', '--rtsp-port', "$RtspPort",
    '--http', '--http-port', "$HttpPort",
    '--web-root', $web
)

if ($Portable) {
    # 绿色版：配置落在 exe 旁边，整个目录自包含，拷到别的机器也不打架。
    $cfg = Join-Path $root 'config.json'
    $argv += @('--config', $cfg)
    if (-not (Test-Path -LiteralPath $cfg)) {
        Write-Host '[i] 绿色版模式：还没有 config.json，这次用内置默认值，保存时创建。'
    }
} else {
    # 安装版：不传 --config，用它自己的默认值 %APPDATA%\Desk2ONVIF\config.json。
    # 这样升级、卸载、重装都不会把配置弄丢，也不要求安装目录可写。
    $cfg = Join-Path $env:APPDATA 'Desk2ONVIF\config.json'
    if (-not (Test-Path -LiteralPath $cfg)) {
        Write-Host "[i] $cfg 还不存在，这次用内置默认值，保存时创建。"
    }
}
if ($LocalOnly) { $argv += @('--http-host', '127.0.0.1') } else { $argv += @('--http-host', '0.0.0.0') }
if ($NoSub)     { $argv += '--no-sub' }
if ($NoOnvif)   { $argv += '--no-onvif' } else {
    $argv += @('--onvif-user', $OnvifUser, '--onvif-password', $OnvifPassword)
}
if ($Extra.Count -gt 0) { $argv += $Extra }
if ($Rest.Count  -gt 0) { $argv += $Rest }

# --- 本机在局域网上的地址 -------------------------------------------------
#
# 用 UDP "connect" 让路由表挑一张网卡 —— **一个包都不会发出去**，
# 只是问内核"去这个地址你会走哪张网卡"。比枚举网卡再猜哪个是主用可靠。
function Get-LanAddress {
    try {
        $sock = New-Object Net.Sockets.UdpClient
        $sock.Connect('223.5.5.5', 53)
        $ip = ([Net.IPEndPoint]$sock.Client.LocalEndPoint).Address.ToString()
        $sock.Dispose()
        return $ip
    } catch {
        return $null
    }
}

$lan = Get-LanAddress
$hostForUrl = if ($LocalOnly -or -not $lan) { '127.0.0.1' } else { $lan }

$subPath = if ($NoSub) { $null } else { 'live_sub' }

Write-Host ''
Write-Host '  Desk2ONVIF —— 桌面 → RTSP + ONVIF + Web 控制台' -ForegroundColor Cyan
Write-Host ('  ' + ('-' * 58))
Write-Host ("  主码流      rtsp://${hostForUrl}:${RtspPort}/live")
if ($subPath) {
    Write-Host ("  子码流      rtsp://${hostForUrl}:${RtspPort}/${subPath}   （给 NVR 多画面网格用）")
}
Write-Host ("  Web 控制台  http://${hostForUrl}:${HttpPort}/")
Write-Host  ("  Web 登录    admin / admin   （出厂值；改过就以控制台里配置页那份为准）")
if (-not $NoOnvif) {
    Write-Host ("  ONVIF       http://${hostForUrl}:${HttpPort}/onvif/device_service")
    Write-Host  ("  NVR 凭据    ${OnvifUser} / ${OnvifPassword}")
}
Write-Host ('  ' + ('-' * 58))

if (-not $LocalOnly) {
    Write-Host '  [!] Web 控制台要登录，但出厂口令 admin/admin 是公开的，' -ForegroundColor Yellow
    Write-Host '      而这里监听的是 0.0.0.0 —— 在改掉口令之前，局域网内任何机器' -ForegroundColor Yellow
    Write-Host '      都能进来看你的桌面画面、读配置、改配置。' -ForegroundColor Yellow
    Write-Host '      改口令：控制台 → 配置 → 登录用户名/登录口令（存完当场生效，不用重启）。' -ForegroundColor Yellow
    Write-Host '      只想本机用：改用 start-local.bat。' -ForegroundColor Yellow
    Write-Host ''
}

Write-Host '  Ctrl+C 停止。' -ForegroundColor DarkGray
Write-Host ''

# --- 跑 -------------------------------------------------------------------
# 前台运行，不用 Start-Process：Ctrl+C 要能直接递到 exe（它自己处理
# 优雅停机：发 Bye、关 RTSP、收尾写文件）。Start-Process 会把它挡在外面。
& $exe @argv
$code = $LASTEXITCODE
Write-Host ''
if ($code -eq 0) {
    Write-Host '  已正常退出。' -ForegroundColor Green
} else {
    Write-Host "  退出码 $code" -ForegroundColor Red
    Write-Host '  常见原因：554/8080/3702 被占用（换个端口试试），或没有可录的显示输出。'
}
exit $code
