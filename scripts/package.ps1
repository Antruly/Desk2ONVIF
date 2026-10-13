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
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -NoBump
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -NoBuild -NoBump
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Zip
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -AppUrl https://github.com/me/Desk2ONVIF
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Jobs 2   # 内存紧张时降并行度
#>
[CmdletBinding()]
param(
    # 跳过 cmake 和 npm 构建，只用现有产物打包。
    # **必须和 -NoBump 一起用**：跳过构建却仍然自增版本号的话，包名会是新版本、
    # 而里面的 exe 还是上一次编的那个旧版本 —— 两个数字都是"正常"的，对不上却
    # 没有任何东西会报错。所以那种组合当场拒绝（见下面那段检查）。
    [switch] $NoBuild,
    # 不自增版本号，按 CMakeLists.txt 里现在这个版本原样重打一份。
    # 给"上一次打包失败了想重来一次"和"想重复验证同一个版本"用。
    [switch] $NoBump,
    # 除了安装包，再产出一个绿色版 zip
    [switch] $Zip,
    # 项目主页，写进安装程序的"发布者"信息里。
    #
    # 默认**不传**：这一项以前在 .iss 里有个占位符 "https://github.com/"，装出来
    # 的程序点"发布者"会跳到 GitHub 首页 —— 一个和本项目毫无关系的页面，比没有
    # 链接更糟。没有真地址就不显示这条链接，有真地址了再从这里传进来。
    [string] $AppUrl = '',
    # 构建并行度，直接转给 build.ps1。默认 0 = 不传，由 build.ps1 自己定（逻辑核数）。
    #
    # 为什么要留这个开关：这台机器全核编那几个重模板 TU（onvif_service.cpp、
    # h264_live_source.cpp 等）时会撞上 Windows 的提交上限，cc1plus 报
    # `out of memory allocating 65536 bytes`。那是**内存不够**，不是代码错，
    # 而报错长得像编译错误 —— 2026-10-09 那次打包就是这么失败的，重跑一遍
    # 结果随机。所以下面 [1/4] 里失败会自动降到 2 路重试一次，想一次到位也可以
    # 直接从命令行传 -Jobs 2。
    [int] $Jobs = 0
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

# --- 版本号：一次自增 + 一处读取 ---------------------------------------------
#
# 唯一的来源是 CMakeLists.txt 的 `project(Desk2ONVIF VERSION x.y.z)` 那一行，
# 它同时喂给三处：exe 的 VersionInfo（resources\desk2onvif.rc.in）、程序自己
# 报的 `--version` 与 `/api/status`（common\version.h），以及这个脚本读出来
# 给 ISCC 的 /DMyAppVersion。
#
# **在本脚本里自增的规则是"每次打包 +1"，这是用户明确选的。** 含义就是：连打
# 两次（一个字都没改）也会 +1。理由是这样一来"这个包是哪一次打的"永远唯一，
# 而唯一性正是"我装的到底是哪一版"要回答的问题。想原样重打用 -NoBump。
#
# **顺序是关键：自增必须在 build.ps1 之前。** 反过来的话 rc 里编进去的还是旧
# 版本，于是包名是新的、exe 属性是旧的 —— 正是这一轮要堵的那个漂移。
if ($NoBuild -and -not $NoBump) {
    throw @"
-NoBuild 和版本自增不能一起用。

  -NoBuild 打的是**上一次构建出来的 exe**，而自增会把 CMakeLists.txt 里的版本
  号 +1 —— 于是包名是 0.1.1、里面 exe 的 VersionInfo 还是 0.1.0。两个数字单看
  都正常，对不上却不会报任何错，只会让"这个包到底是哪一版"永远说不清。

两条出路：
  1. 想原样重打刚才那一版：加 -NoBump   →  package.ps1 -NoBuild -NoBump
  2. 想要新版本号：去掉 -NoBuild（让它真的重新构建一次）
"@
}

$cmakePath = Join-Path $root 'CMakeLists.txt'
# 行锚定的正则，不用裸的 'VERSION\s+(\d+...)'：CMakeLists.txt 里还有
# `cmake_minimum_required(VERSION 3.21)` 这种东西，裸匹配是在赌谁先出现 ——
# 而它现在的写法只是**碰巧**匹配不上（3.21 只有两段）。行锚定之后，它匹配的
# 必然是 `  VERSION x.y.z` 那唯一一行。
$versionRe = [regex]'(?m)^[ \t]*VERSION[ \t]+(\d+)\.(\d+)\.(\d+)[ \t]*$'

if ($NoBump) {
    Write-Host '  版本自增          跳过（-NoBump）'
} else {
    $text = [IO.File]::ReadAllText($cmakePath)
    $vm = $versionRe.Match($text)
    if (-not $vm.Success) {
        throw 'CMakeLists.txt 里没找到 `VERSION x.y.z` 那一行 —— 版本号无处自增。'
    }
    $oldVer = '{0}.{1}.{2}' -f $vm.Groups[1].Value, $vm.Groups[2].Value, $vm.Groups[3].Value
    $newVer = '{0}.{1}.{2}' -f $vm.Groups[1].Value, $vm.Groups[2].Value, ([int]$vm.Groups[3].Value + 1)

    # 改之前先留一份。**这条不是形式主义**：CMakeLists.txt 是版本号的唯一来源，
    # 而自增是"就地改源代码"，没有它的话"我原来是多少"只能靠回忆。
    $outDir = Join-Path $root 'out'
    $null = New-Item -ItemType Directory -Path $outDir -Force
    Copy-Item -LiteralPath $cmakePath -Destination (Join-Path $outDir 'CMakeLists.txt.before-bump') -Force

    # **无 BOM 写回。** 这个文件是 UTF-8 无 BOM，而 PowerShell 5.1 的
    # `Set-Content -Encoding utf8` / `Out-File` 会**加上** BOM —— CMake 读到
    # 开头那三个字节会当场报 `CMake Error at CMakeLists.txt:1`，而报错位置指向
    # 文件第一行，看不出是编码问题。（这个坑是本轮实测踩到的。）
    # 顺带一提，WriteAllText 会更新 mtime —— 这是必需的：mtime 不变的话 CMake
    # 认为没有东西脏了，**不会重新 configure**，于是 rc 还是旧版本号。
    # （Copy-Item 保留 mtime 这件事本轮也实测咬了一口：改坏再改回来之后
    #   `build.ps1` 什么都没做，exe 属性里还是那个探针版本。）
    $bumped = $text.Remove($vm.Index, $vm.Length).Insert($vm.Index, "  VERSION $newVer")
    [IO.File]::WriteAllText($cmakePath, $bumped, (New-Object Text.UTF8Encoding $false))
    Write-Host ("  版本自增          {0} → {1}" -f $oldVer, $newVer) -ForegroundColor Yellow
}

# 读的是**自增之后**的值（-NoBump 时就是原地那个）。
$text = [IO.File]::ReadAllText($cmakePath)
$m = $versionRe.Match($text)
if (-not $m.Success) { throw 'CMakeLists.txt 里没找到 `VERSION x.y.z` 那一行' }
$version = '{0}.{1}.{2}' -f $m.Groups[1].Value, $m.Groups[2].Value, $m.Groups[3].Value
Write-Host "  版本              $version"

# --- [1/4] 后端 -------------------------------------------------------------
if ($NoBuild) {
    Write-Host '  [1/4] 后端            跳过（-NoBuild）'
} else {
    Write-Host '  [1/4] 构建后端…'
    $buildArgs = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'build.ps1'))
    if ($Jobs -gt 0) { $buildArgs += @('-Jobs', "$Jobs") }
    & powershell @buildArgs | Select-Object -Last 3 | ForEach-Object { Write-Host "        $_" }
    if ($LASTEXITCODE -ne 0 -and $Jobs -le 0) {
        # 见 param 里 -Jobs 那段：全核编译撞提交上限时 cc1plus 会 OOM。那种失败
        # 全核重跑一次大概率还是同样的结果，而错误文本长得像代码有问题 —— 直接
        # 降级到 2 路重建一次，比自己重跑一遍赌内存划算。真·编译错误会原样复现，
        # 所以这个重试不会把真失败盖掉。
        Write-Host '        ↑ 构建失败，按内存不足降级到 -Jobs 2 重试…' -ForegroundColor Yellow
        & powershell @($buildArgs + @('-Jobs', '2')) | Select-Object -Last 3 | ForEach-Object { Write-Host "        $_" }
    }
    if ($LASTEXITCODE -ne 0) { throw "后端构建失败（build.ps1 退出码 $LASTEXITCODE）" }
}

$exePath = Join-Path $bin 'desk2onvif.exe'
if (-not (Test-Path -LiteralPath $exePath)) { throw "找不到 $exePath" }

# --- 对账：exe 属性里的版本必须等于刚读到的那个 ------------------------------
#
# 这一条才是"漂移真的被堵住了"的证明，前面那些"唯一的来源"都只是它的前提。
# 它挡的是两种都很安静的失败：
#   (a) 自增之后**没有真的重新构建**（mtime 没变、CMake 没 reconfigure，
#       build.ps1 报成功但什么都没编）→ exe 属性停在上一版；
#   (b) `-NoBuild` 用了一个更早的 exe。
# 两种情况下包名都正常、ISCC 也正常、装出来的程序也能跑 —— 只有右键看属性
# 才发现不对，而那正是没人会做的事。
$exeVer = (Get-Item -LiteralPath $exePath).VersionInfo.FileVersion
if ([string]::IsNullOrWhiteSpace($exeVer)) {
    throw @"
$exePath 里读不到版本资源（FileVersion 是空的）。

  资源脚本由 CMake 从 resources\desk2onvif.rc.in 生成到构建目录，再编进 exe。
  读不到通常意味着它没被编进去 —— 检查 CMakeLists.txt 的 configure_file 与
  add_executable 里那一行，以及 CMakeLists.txt 是否被写成了带 BOM 的
  （带 BOM 会让 CMake 直接报错，但也可能有人手工绕过）。
"@
}
if ($exeVer -ne $version) {
    throw @"
版本号对不上：CMakeLists.txt 里是 $version，而 exe 的属性是 $exeVer。

  包名会按 $version 生成，exe 里却是 $exeVer —— 装出来的东西"看起来"完全正常，
  只有右键看文件属性才看得出来。所以这里直接停下。

  常见原因：
    1. 用了 -NoBuild，打的是上一次构建的 exe（那就加 -NoBump，或去掉 -NoBuild）
    2. 自增之后构建没有真的重新 configure（把 CMakeLists.txt 的 mtime 碰一下，
       或删掉 build\desk2onvif.rc 再打一次）
"@
}
Write-Host "  exe 版本属性      $exeVer ✓"

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
