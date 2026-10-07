<#
.SYNOPSIS
    绿色版 zip 验收：解到一个空目录里、用真正的入口脚本跑起来、验完删掉。

.DESCRIPTION
    为什么绿色版要单独验，而不是"反正和安装包是同一批文件"：
    它有一整套安装包**没有**的处理，而且全在"不报错就会出事"的那一类里：
      · start.bat / start.ps1 是从 packaging\ 直接拷进去的 —— .ps1 缺 BOM
        会被 PS 5.1 按 936 读，报一个指不到编码的语法错；.bat **多**了 BOM
        会被 cmd 把开头三个字节当命令执行。
      · 它靠 `start.ps1 -Portable` 把配置放在 exe 旁边，而不是 %APPDATA%。
        这条要是没生效，用户以为整个目录是自包含的、拷走就行，其实配置
        留在原来那台机器上。
      · 它没有 uninstaller，"删目录"是唯一的卸载方式 —— 所以**不能**有文件
        落在目录外面，否则用户删了目录还以为干净了。

    所以这里跑的是 **start.ps1**，不是直接用命令行起 exe：路径怎么拼、
    `-Portable` 映射成什么，都只有走真实入口才验得到。而且启动时的工作
    目录**故意**不是安装目录 —— start.ps1 用 $PSScriptRoot 拼绝对路径就是
    为了"从任何地方启动都能跑"，而这条一旦坏了，症状是"服务起来了、推流
    也正常，只有网页打不开"（exe 的 --web-root 默认是相对路径）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\verify_portable.ps1
#>
[CmdletBinding()]
param(
    # 要验的 zip；默认取 release\ 下最新的那个
    [string] $Zip,
    # 解到这里（一个**新建的空目录**，验完整个删掉）
    [string] $ExtractDir,
    [int]    $RtspPort = 8558,
    [int]    $HttpPort = 8201,
    # 验完不删，留着肉眼看
    [switch] $Keep
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot '_verify_common.ps1')

if (-not $Zip) {
    $cand = @(Get-ChildItem -LiteralPath (Join-Path $root 'release') -Filter 'Desk2ONVIF-*-win64.zip' -ErrorAction SilentlyContinue |
              Sort-Object LastWriteTime -Descending)
    if ($cand.Count -eq 0) { throw 'release\ 下没有绿色版 zip。先跑 scripts\package.ps1 -Zip。' }
    $Zip = $cand[0].FullName
}
if (-not $ExtractDir) {
    $ExtractDir = Join-Path $env:TEMP ('d2o-portable-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
}
$apDir = Join-Path $env:APPDATA 'Desk2ONVIF'

# 运行前后各拍一张 %APPDATA%\Desk2ONVIF 的快照，比对即"有没有往那里写东西"。
#
# 为什么不是直接断言"这个目录不存在"：那种判据在**已经用过安装版**的机器上
# 恒真 —— 目录本来就在，里面写没写新东西它都"存在"，于是这一条永远通过，
# 等于没验。而那恰恰是最需要它生效的机器。快照比法对两种初始状态都成立：
# 目录原先没有，跑完还得没有；原先有，跑完得**一个字节都没变**。
function Get-TreeSnapshot([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '<不存在>' }
    $items = @(Get-ChildItem -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue |
               Sort-Object FullName | ForEach-Object {
                   $rel = $_.FullName.Substring($Path.Length + 1)
                   if ($_.PSIsContainer) { "D $rel" }
                   else { "F $rel $($_.Length) $($_.LastWriteTimeUtc.Ticks)" }
               })
    return ($items -join "`n")
}

Write-Host ''
Write-Host '=== 绿色版 zip 验收 ===' -ForegroundColor Cyan
Write-Host ("  zip       " + $Zip)
Write-Host ("  解到      " + $ExtractDir)
Write-Host ("  端口      RTSP " + $RtspPort + " / Web " + $HttpPort)

# --- [0] 前置 ---------------------------------------------------------
Section "[0] 前置检查"
Check (@(Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue).Count -eq 0) '没有正在运行的 desk2onvif'
foreach ($pt in @($RtspPort, $HttpPort)) { Check (-not (Test-PortBusy $pt)) "端口 $pt 空闲" }
Check (-not (Test-Path -LiteralPath $ExtractDir)) "解压目录不存在（$ExtractDir）" `
      '必须解进一个**全新的空目录**：解到别处会把无关文件混进来，验出来的是那一堆东西的状态。'
$apBefore = Get-TreeSnapshot $apDir
if ($apBefore -eq '<不存在>') {
    Write-Host "  （%APPDATA%\Desk2ONVIF 事先不存在 —— 跑完必须仍然不存在）" -ForegroundColor DarkGray
} else {
    Write-Host "  （%APPDATA%\Desk2ONVIF 事先存在，$(($apBefore -split "`n").Count) 项 —— 跑完必须一字未变）" -ForegroundColor DarkGray
}

# --- [1] 解压 ---------------------------------------------------------
Section "[1] 解压"
Expand-Archive -LiteralPath $Zip -DestinationPath $ExtractDir -Force
# Compress-Archive 会把顶层目录一起装进去，所以真正的根在下一层。
$dirs = @(Get-ChildItem -LiteralPath $ExtractDir -Directory)
Check ($dirs.Count -eq 1) "zip 里只有一个顶层目录（实际 $($dirs.Count)）" `
      (($dirs | ForEach-Object { $_.Name }) -join ', ')
$stage = if ($dirs.Count -eq 1) { $dirs[0].FullName } else { $ExtractDir }
Write-Host ("  解出来的根 " + $stage)
Check ((Split-Path $stage -Leaf) -match 'win64') "顶层目录名带 win64（$(Split-Path $stage -Leaf)）"

$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
$mb = [math]::Round((($files | Measure-Object -Property Length -Sum).Sum) / 1MB, 1)
Write-Host ("  共 $($files.Count) 个文件，$mb MB")

# --- [2] 布局 ---------------------------------------------------------
Section "[2] 布局"
foreach ($f in @('desk2onvif.exe', 'start.bat', 'start.ps1', 'start-local.bat',
                 'list-displays.bat', 'selfcheck.bat', 'firewall-allow.bat',
                 'firewall-print.bat', 'LICENSE', 'THIRD-PARTY.md',
                 'README.md', 'desk2onvif.ico')) {
    Check (Test-Path -LiteralPath (Join-Path $stage $f)) "有 $f"
}
$dlls = @(Get-ChildItem -LiteralPath $stage -Filter '*.dll' -File)
Check ($dlls.Count -ge 15) "DLL 有 $($dlls.Count) 个（期望 ≥15）"
Check (Test-Path -LiteralPath (Join-Path $stage 'web\dist\index.html')) '有 web\dist\index.html'
# 绿色版**不应该**带安装器的产物 —— 带了说明是从装好的目录里拷的，那里面
# 的路数（写注册表、卸载项）和"解压即用"是冲突的。
Check (-not (Test-Path -LiteralPath (Join-Path $stage 'unins000.exe'))) `
      '没有 unins000.exe（那是安装版的产物，不该混进绿色版）'
# 也不该预置 config.json：绿色版的配置由首次运行生成，
# 预置一份会让"我改了配置怎么没反应"变成"改的是哪一份"。
Check (-not (Test-Path -LiteralPath (Join-Path $stage 'config.json'))) `
      '没有预置 config.json（应当由首次运行生成）'

# --- [3] 编码：.ps1 必须有 BOM，.bat 必须没有且纯 ASCII ------------------
Section "[3] 文本文件的编码（这一条只有绿色版要验）"
foreach ($f in @(Get-ChildItem -LiteralPath $stage -Filter '*.ps1')) {
    $b = [IO.File]::ReadAllBytes($f.FullName)
    Check ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) `
          "$($f.Name) 带 UTF-8 BOM" `
          'PS 5.1 没有 BOM 时按 936 读，中文注释会被拼成引号，报的是 "Missing closing }" 之类指不到编码的语法错。'
    # BOM 对了不等于能解析。这两个失败长得像、原因完全不同，所以分开报：
    # 绿色版这边最终也会因为"后端没起来"间接发现，但那条消息指的是症状。
    $err = Test-ScriptSyntax $f.FullName
    Check ($null -eq $err) "$($f.Name) 语法可解析$(if ($err) { " —— $err" })" `
          '字符串里混进一对 ASCII 双引号就够它报 Missing closing } —— 文件是 UTF-8 也照样。'
}
foreach ($f in @(Get-ChildItem -LiteralPath $stage -Filter '*.bat')) {
    $b = [IO.File]::ReadAllBytes($f.FullName)
    Check (-not ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)) `
          "$($f.Name) 没有 BOM" `
          '.bat 开头那三个字节会被 cmd.exe 当成命令，第一行直接报"不是内部或外部命令"。'
    $nonAscii = @($b | Where-Object { $_ -gt 127 })
    Check ($nonAscii.Count -eq 0) "$($f.Name) 是纯 ASCII（非 ASCII 字节 $($nonAscii.Count) 个）" `
          '936 代码页下非 ASCII 字节会被拼错引号。'
}

# --- [4] 原地自检 -----------------------------------------------------
Section "[4] 原地自检（--selfcheck）"
$exe = Join-Path $stage 'desk2onvif.exe'
$sc = Invoke-Capture $exe @('--selfcheck') 60000 $stage
Check (-not $sc.TimedOut) '--selfcheck 在 60 秒内结束'
Check ($sc.ExitCode -eq 0) "--selfcheck 退出码 = $($sc.ExitCode)"
Check ($sc.Stdout -match '全部通过') '--selfcheck 报告"全部通过"'
Check ([D2oPeRes]::Size($exe, 1, 14) -gt 0) 'exe 里有图标组（RT_GROUP_ICON）'

# --- [5] 用 start.ps1 跑起来 ------------------------------------------
Section "[5] start.ps1 -Portable：配置落在 exe 旁边"
$appOut = Join-Path $env:TEMP 'd2o-portable.out'
$appErr = Join-Path $env:TEMP 'd2o-portable.err'
$portableCfg = Join-Path $stage 'config.json'

# 走真实入口，工作目录**故意**选在别处（见文件头）。`--save-config` 让这次
# 的参数当场写盘，否则要等 /api/config 被 POST 才会有文件，那验的就是另一件事了。
$psArgs = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $stage 'start.ps1') + '"' +
          ' -Portable -RtspPort ' + $RtspPort + ' -HttpPort ' + $HttpPort +
          ' --save-config'
$app = Start-Detached 'powershell.exe' $psArgs $env:TEMP $appOut $appErr

try {
    $up = Wait-Backend $HttpPort
    Check $up "后端在 http://127.0.0.1:$HttpPort 起来了（用 start.ps1，工作目录在别处）"
    if (-not $up) { Show (Get-Content -LiteralPath $appErr -Raw -Encoding Default -ErrorAction SilentlyContinue) 20 }

    if ($up) {
        # -Portable 的**唯一**判据：配置出现在 exe 旁边，而不是 %APPDATA%。
        Check (Test-Path -LiteralPath $portableCfg) 'config.json 生成在 exe 旁边（-Portable 生效）'
        if (Test-Path -LiteralPath $portableCfg) {
            $cfg = Get-Content -LiteralPath $portableCfg -Raw -Encoding UTF8 | ConvertFrom-Json
            Check ($cfg.web.port -eq $HttpPort) "配置里的 web.port = $($cfg.web.port)（命令行传的是 $HttpPort）" `
                  '对不上的话说明命令行参数没有回写到 cfg，--save-config 存的是别的东西。'
        }
        # -Portable 的**否定**判据：运行期间 %APPDATA% 那份一个字都没动。
        # 光验"配置在 exe 旁边"是不够的 —— 两边都写的话前一条照样通过，
        # 而用户看到的是"改了配置有时生效有时不生效"。
        $apNow = Get-TreeSnapshot $apDir
        Check ($apNow -ceq $apBefore) `
              '运行期间没有往 %APPDATA%\Desk2ONVIF 写任何东西' `
              "运行前：`n$apBefore`n运行后：`n$apNow"

        # 静态资源必须是**绝对路径**挂上的：工作目录在别处，相对路径的
        # web/dist 解析不到，而那种失败是静默的（"目录不存在"被当成"没挂
        # 静态资源"）。所以这一条正是冲着 start.ps1 的 $PSScriptRoot 来的。
        $wc = Invoke-PythonProbe (Join-Path $root 'tests\web_contract.py') `
                                 @('--port', "$HttpPort", '--web-root', (Join-Path $stage 'web\dist')) 180000 $root
        if (-not $wc) {
            Check $false '找得到 python（web_contract.py 要用）'
        } else {
            Check (-not $wc.TimedOut) 'web_contract.py 在 180 秒内结束'
            Check ($wc.ExitCode -eq 0) "web_contract.py 退出码 = $($wc.ExitCode)"
            Check ($wc.Stdout -match '失败 0 项') 'web_contract.py 报告"失败 0 项"'
            Show $wc.Stdout 4
        }
    }
} finally {
    Stop-Desk2Onvif
    # 收的是**整棵**进程树。Start-Detached 起的是 cmd.exe，下面还挂着
    # powershell.exe 和 desk2onvif.exe；只 Kill 那个 cmd 的话，它下面两个
    # 会变成孤儿继续占着端口 —— 而症状是"下一次运行的端口检查莫名其妙失败"。
    try { & taskkill.exe /F /T /PID $app.Process.Id 2>&1 | Out-Null } catch {}
    Remove-Item -LiteralPath $app.CmdFile -Force -ErrorAction SilentlyContinue
}
Stop-Desk2Onvif
Wait-PortFree @($RtspPort, $HttpPort)

# --- [6] 清理：删目录就是唯一的"卸载" -----------------------------------
Section "[6] 清理"
if ($Keep) {
    Write-Host ("  （-Keep：留着 $ExtractDir）") -ForegroundColor Yellow
} else {
    Remove-Item -LiteralPath $ExtractDir -Recurse -Force -ErrorAction SilentlyContinue
    Check (-not (Test-Path -LiteralPath $ExtractDir)) '解压目录删干净了（绿色版就靠这个"卸载"）'
    # 绿色版没有 unins000.exe，"删目录"必须真的够用 —— 也就是**不能**有东西
    # 落在目录外面。上面那条管的是"运行期间"，这一条管**停机那一刻**：
    # 退出时写配置（收尾保存是好几种程序的习惯做法）正好落在两次快照之间，
    # 只拍运行中的那一张就会漏掉它。
    $apEnd = Get-TreeSnapshot $apDir
    Check ($apEnd -ceq $apBefore) `
          '直到停机为止 %APPDATA%\Desk2ONVIF 都和运行前一模一样（没有文件落在目录外）' `
          "运行前：`n$apBefore`n停机后：`n$apEnd"
}

Write-VerifySummary
