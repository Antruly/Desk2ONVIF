<#
.SYNOPSIS
    打 release：默认产出 Inno Setup 安装包（release\Desk2ONVIF_Setup_<版本>.exe），
    可选再产出一个绿色版 zip。

.DESCRIPTION
    为什么要有这个脚本，而不是每次手工拷一遍：手工拷贝有个**必然会犯**的错 ——
    只拷 build\bin 里的 exe 和 DLL。那样出来的包装得上、推得了流、应答得了
    ONVIF，**就是网页界面打不开**，而且一个字都不报：exe 的 `--web-root`
    默认值是相对路径 `web/dist`，而"目录不存在"被刻意当成"没挂静态资源"
    而不是错误（src/main.cpp 里那段注释解释了为什么）。于是"网页白屏"和
    "没打前端"在日志上完全一样。

    .iss 里的 [Files] 现在是逐条列的，web\dist 那一条**不能删**。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -NoBuild
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Zip
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -AppUrl https://github.com/me/Desk2ONVIF
#>
[CmdletBinding()]
param(
    # 跳过 cmake 和 npm 构建，只用现有产物打包
    [switch] $NoBuild,
    # 除了安装包，再产出一个绿色版 zip
    [switch] $Zip,
    # 项目主页，写进安装程序的"发布者"信息里。
    #
    # 默认**不传**：这一项以前在 .iss 里有个占位符 "https://github.com/"，装出来
    # 的程序点"发布者"会跳到 GitHub 首页 —— 一个和本项目毫无关系的页面，比没有
    # 链接更糟。没有真地址就不显示这条链接，有真地址了再从这里传进来。
    [string] $AppUrl = ''
)

$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot
$bin     = Join-Path $root 'build\bin'
$web     = Join-Path $root 'web\dist'
$pack    = Join-Path $root 'packaging'
$iss     = Join-Path $pack 'desk2onvif.iss'
$release = Join-Path $root 'release'
# fsrec 那台机器上 Inno Setup 装在用户目录下（非管理员安装的默认位置）。
# 也认一下 Program Files 的两个常规位置，免得换台机器就找不到。
$isccCandidates = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
)
$iscc = $isccCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

Write-Host ''
Write-Host '=== Desk2ONVIF release builder ===' -ForegroundColor Cyan
Write-Host ('-' * 60)

# --- 版本号：只从 CMakeLists.txt 读一处 -------------------------------------
# 安装程序显示的版本、程序 --version 输出的版本、文件名里的版本，必须是同一个。
# 各写一份的结果是"包里叫 0.1.0、--version 说 0.1.1"，而且几乎不可能被发现。
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
$m = [regex]::Match($cmake, 'VERSION\s+(\d+\.\d+\.\d+)')
if (-not $m.Success) { throw 'CMakeLists.txt 里没找到 VERSION x.y.z' }
$version = $m.Groups[1].Value
Write-Host "  版本              $version"

# --- [1/4] 后端 -------------------------------------------------------------
if ($NoBuild) {
    Write-Host '  [1/4] 后端            跳过（-NoBuild）'
} else {
    Write-Host '  [1/4] 构建后端…'
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1') |
        Select-Object -Last 3 | ForEach-Object { Write-Host "        $_" }
    if ($LASTEXITCODE -ne 0) { throw "后端构建失败（build.ps1 退出码 $LASTEXITCODE）" }
}

$exePath = Join-Path $bin 'desk2onvif.exe'
if (-not (Test-Path -LiteralPath $exePath)) { throw "找不到 $exePath" }

# --- [2/4] 前端 -------------------------------------------------------------
# **dist 是必需的，不是可选的。** 少了它安装包照样能装、程序照样能跑，
# 只有网页界面打不开 —— 这个失败模式太安静了，所以在这里直接拒绝。
$indexHtml = Join-Path $web 'index.html'
if ($NoBuild -and (Test-Path -LiteralPath $indexHtml)) {
    Write-Host '  [2/4] 前端            跳过（-NoBuild）'
} else {
    Write-Host '  [2/4] 构建前端…'
    Push-Location (Join-Path $root 'web')
    try {
        if (-not (Test-Path -LiteralPath (Join-Path $root 'web\node_modules'))) {
            Write-Host '        npm ci…'
            & npm ci
            if ($LASTEXITCODE -ne 0) { throw 'npm ci 失败' }
        }
        & npm run build
        if ($LASTEXITCODE -ne 0) { throw 'npm run build 失败' }
    } finally { Pop-Location }
}
if (-not (Test-Path -LiteralPath $indexHtml)) {
    throw "找不到 $indexHtml —— 前端没构建。装出来的包会没有网页界面，而程序不会报任何错。"
}
$webFiles = @(Get-ChildItem -LiteralPath $web -Recurse -File)
Write-Host "        前端产物        $($webFiles.Count) 个文件"

# --- [3/4] 图标 -------------------------------------------------------------
$ico = Join-Path $root 'resources\desk2onvif.ico'
if (-not (Test-Path -LiteralPath $ico)) {
    Write-Host '  [3/4] 生成图标…'
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'make_icon.ps1') |
        ForEach-Object { Write-Host "        $_" }
} else {
    Write-Host '  [3/4] 图标            已存在'
}

# --- [4/4] 编译安装包 -------------------------------------------------------
if (-not $iscc) {
    throw ("没找到 Inno Setup 6 的 ISCC.exe。找过这些位置：`n  " +
           ($isccCandidates -join "`n  ") +
           "`n装一个：https://jrsoftware.org/isdl.php （非管理员装在用户目录也可以）")
}
Write-Host '  [4/4] 编译安装包…'
$null = New-Item -ItemType Directory -Path $release -Force
# /D 把版本号传进 .iss，让版本号只有 CMakeLists.txt 一个来源。
# AppUrl 只在给了的时候才传 —— 传一个空的进去，.iss 那边 `#if` 判成假、
# 不写 AppPublisherURL，效果和完全不传一样，但少传一个参数更不容易看岔。
$isccArgs = @("/DMyAppVersion=$version")
if ($AppUrl) { $isccArgs += "/DMyAppUrl=$AppUrl" }
& $iscc @isccArgs $iss | ForEach-Object { Write-Host "        $_" }
if ($LASTEXITCODE -ne 0) { throw "ISCC 退出码 $LASTEXITCODE" }

$setup = Join-Path $release "Desk2ONVIF_Setup_$version.exe"
if (-not (Test-Path -LiteralPath $setup)) { throw "没生成 $setup" }
Write-Host ''
Write-Host ("  安装包            {0}  ({1:N1} MB)" -f $setup, ((Get-Item $setup).Length / 1MB)) `
    -ForegroundColor Green

# --- 可选：绿色版 zip -------------------------------------------------------
# 安装包是主要交付物；这个给"不想装、就想解压跑一下"的场合。
if ($Zip) {
    Write-Host ''
    Write-Host '  另出绿色版 zip…'
    $stage = Join-Path $release "portable\Desk2ONVIF-$version-win64"
    if (Test-Path -LiteralPath (Split-Path $stage -Parent)) {
        Remove-Item -LiteralPath (Split-Path $stage -Parent) -Recurse -Force
    }
    $null = New-Item -ItemType Directory -Path $stage -Force

    Copy-Item -LiteralPath $exePath -Destination $stage
    Copy-Item -Path (Join-Path $bin '*.dll') -Destination $stage
    $webOut = Join-Path $stage 'web\dist'
    $null = New-Item -ItemType Directory -Path $webOut -Force
    Copy-Item -Path (Join-Path $web '*') -Destination $webOut -Recurse
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $pack 'THIRD-PARTY.md') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $pack 'README-release.md') -Destination (Join-Path $stage 'README.md')
    Copy-Item -Path (Join-Path $pack '*.bat') -Destination $stage
    Copy-Item -Path (Join-Path $pack '*.ps1') -Destination $stage
    Copy-Item -LiteralPath $ico -Destination $stage

    # 绿色版的 start.bat 必须显式带 -Portable（配置放 exe 旁边，整包自包含）
    # —— 它已经带了。但 ps1 的 BOM 要保证：PS 5.1 没 BOM 时按 936 读中文注释，
    # 报的是 "Missing closing '}'" 之类指不到编码的语法错。
    foreach ($f in Get-ChildItem -LiteralPath $stage -Filter *.ps1) {
        $b = [IO.File]::ReadAllBytes($f.FullName)
        if (-not ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)) {
            [IO.File]::WriteAllBytes($f.FullName,
                ([byte[]](0xEF, 0xBB, 0xBF) + $b))
        }
    }
    foreach ($f in Get-ChildItem -LiteralPath $stage -Filter *.bat) {
        $b = [IO.File]::ReadAllBytes($f.FullName)
        # .bat 反过来**绝不能有** BOM：开头那三个字节会被 cmd.exe 当成命令，
        # 第一行直接报 "'..@echo' 不是内部或外部命令"。
        if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) {
            [IO.File]::WriteAllBytes($f.FullName, $b[3..($b.Length - 1)])
        }
        if ([IO.File]::ReadAllBytes($f.FullName) | Where-Object { $_ -gt 127 }) {
            throw "$($f.Name) 含非 ASCII 字节 —— .bat 在 936 代码页下会被拼错引号"
        }
    }

    # 这里**不能**叫 $zip：PowerShell 的变量名大小写不敏感，而上面那个
    # [switch] $Zip 参数会把 $Zip 变成一个**带类型约束**的变量。于是
    # `$zip = "…zip"` 实际是在往一个 SwitchParameter 里塞字符串，当场抛
    # 类型转换错 —— 而报错信息里出现的是那个**路径字符串**（"Cannot convert
    # the D:\…\Desk2ONVIF-0.1.0-win64.zip value…"）和调用方的行号，
    # 一个字的线索都没有指向"你撞了参数名"。既然叫 $zipPath 就没事。
    $zipPath = Join-Path $release "Desk2ONVIF-$version-win64.zip"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path $stage -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host ("  绿色版            {0}  ({1:N1} MB)" -f $zipPath, ((Get-Item $zipPath).Length / 1MB)) `
        -ForegroundColor Green
}

Write-Host ''
Write-Host '  完成。' -ForegroundColor Green
Write-Host "  安装包：$setup"
Write-Host '  装完双击桌面图标或开始菜单里的「启动 Desk2ONVIF」。'
Write-Host ''
