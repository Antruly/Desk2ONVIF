<#
.SYNOPSIS
  配置并构建 Desk2ONVIF（自动处理 MSYS2 的 PATH 问题）。

.DESCRIPTION
  这个脚本存在的唯一理由是 PATH。

  MSYS2 的 gcc.exe 自己能启动（它在 mingw64\bin 下，DLL 走"同目录优先"），
  但它调起来的 cc1.exe 在 lib\gcc\...\15.1.0\ 下 —— 那里没有任何运行库 DLL，
  cc1 只能靠 PATH 去找 zlib1.dll / libwinpthread-1.dll / libgmp-10.dll 这些。
  PATH 里没有 mingw64\bin 时，cc1 会从别处（例如某个自带 zlib1.dll 的程序目录）
  加载到版本对不上的同名 DLL，然后以 STATUS_ENTRYPOINT_NOT_FOUND 退出。

  这个失败是完全静默的：cc1 一个字都不打印，Ninja 那边只有一行
  "FAILED: [code=1]"，CMake 那边是"The C compiler is not able to compile a
  simple test program"。报错里没有任何字样指向 PATH。

  所以：本脚本把 mingw64\bin 前置进 PATH 再调 cmake。
  （scripts\build_live555.sh 里也是同样的处理，理由相同。）

.EXAMPLE
  .\scripts\build.ps1
  .\scripts\build.ps1 -Clean
  .\scripts\build.ps1 -BuildDir build-debug -BuildType Debug
  .\scripts\build.ps1 -Reconfigure
  .\scripts\build.ps1 -StopRunning    # 有 desk2onvif 在跑，占着 exe，先结束它
#>
[CmdletBinding()]
param(
  [string]$BuildDir  = 'build',
  [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
  [string]$BuildType = 'Release',
  [string]$MsysRoot  = 'C:\msys64',
  [string]$Target    = '',
  [int]$Jobs         = 0,
  [switch]$Clean,
  [switch]$Reconfigure,
  [switch]$NoSelfCheck,
  # 默认**不**动任何正在运行的进程。链接失败时把原因和命令行说清楚就够了，
  # 一个构建脚本顺手杀掉别人正在录屏的进程，是使用者没有要求过的副作用。
  [switch]$StopRunning
)
$ErrorActionPreference = 'Stop'

$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

$mingwBin = Join-Path $MsysRoot 'mingw64\bin'
$usrBin   = Join-Path $MsysRoot 'usr\bin'
foreach ($p in @($mingwBin, $usrBin)) {
  if (-not (Test-Path $p)) { throw "找不到 $p —— 先装 MSYS2 并安装 mingw-w64-x86_64-gcc。" }
}

$cmake = Join-Path $mingwBin 'cmake.exe'
$ninja = Join-Path $mingwBin 'ninja.exe'
$gcc   = Join-Path $mingwBin 'gcc.exe'
$gpp   = Join-Path $mingwBin 'g++.exe'
foreach ($t in @($cmake, $ninja, $gcc, $gpp)) {
  if (-not (Test-Path $t)) { throw "找不到 $t" }
}

# 关键的一步：前置，不是追加。PATH 里已经有的同名 DLL 目录必须排到后面去。
$env:PATH = "$mingwBin;$usrBin;$env:PATH"

# 依赖供给的兜底检查。不替使用者跑供给脚本 —— 那要下载几十 MB，
# 该不该跑应该由人决定，这里只把话说清楚。
$depChecks = @(
  @{ Path = 'third_party\ffmpeg\include\libavcodec\avcodec.h'; Script = 'fetch_ffmpeg.ps1' },
  @{ Path = 'third_party\live555\include\NetCommon.h';          Script = 'build_live555.ps1' },
  @{ Path = 'third_party\uvcpp\lib\libuvcpp.dll.a';             Script = 'fetch_uvcpp.ps1' }
)
foreach ($c in $depChecks) {
  if (-not (Test-Path (Join-Path $Root $c.Path))) {
    throw "依赖没供给：缺 $($c.Path)`n先跑 .\scripts\$($c.Script)（说明见 third_party\README.md）"
  }
}

$bd = Join-Path $Root $BuildDir
if ($Clean -and (Test-Path $bd)) {
  Write-Host "[build] 清理 $bd"
  Remove-Item $bd -Recurse -Force
}
if ($Reconfigure -and (Test-Path (Join-Path $bd 'CMakeCache.txt'))) {
  Write-Host "[build] 删除 CMakeCache 以强制重新配置"
  Remove-Item (Join-Path $bd 'CMakeCache.txt') -Force
}

# 正在跑的 desk2onvif 占着它自己的 exe，链接到最后一刻才会失败，而 ld 只说：
#   cannot open output file bin\desk2onvif.exe: Permission denied
#   collect2.exe: error: ld returned 1 exit status
# 唯一常见的原因 —— 有个 desk2onvif 还在跑 —— 这两句里一个字都没有。
# 所以链接之前先探一次这个文件能不能独占写，探到了就把话说全。
function Test-Locked([string]$Path) {
  if (-not (Test-Path $Path)) { return $false }
  try {
    # "独占写能不能打开"正是链接器要问的那个问题，所以直接问它。
    # 按进程名去猜会漏掉"换了名但占着同一个文件"的情况，也会误报别的副本。
    $fs = [IO.File]::Open($Path, [IO.FileMode]::Open,
                          [IO.FileAccess]::Write, [IO.FileShare]::None)
    $fs.Close()
    return $false
  } catch {
    return $true
  }
}

$exe = Join-Path $bd 'bin\desk2onvif.exe'
if (-not $Target -and (Test-Locked $exe)) {
  $procs = @(Get-Process -Name 'desk2onvif' -ErrorAction SilentlyContinue)
  $who = if ($procs.Count -gt 0) {
    ($procs | ForEach-Object { "PID $($_.Id)" }) -join '、'
  } else {
    '按名字没找到进程（可能被杀毒软件或搜索索引临时占着）'
  }
  if (-not $StopRunning) {
    throw @"
$exe 被占用：$who

这是链接失败最常见的原因，而且 ld 的报错里不会提它。
两种办法：
  · 在它自己的窗口里按 Ctrl+C 让它退出（这样 MP4 索引会写完，不是硬杀）；
  · 或者带上 -StopRunning 重跑这个脚本 —— 注意那是**直接终止**，
    它当时正在录的 MP4 索引不会写完。
"@
  }
  Write-Host "[build] $exe 被占用（$who），-StopRunning 已指定，正在结束它们"
  $procs | Stop-Process -Force -ErrorAction SilentlyContinue
  # 进程结束 ≠ 文件立刻能写。实测过：Stop-Process 之后单探一次仍然打不开
  # （镜像节和文件句柄随进程收尸关闭，中间还常夹一段杀毒软件的扫描。）
  # 所以这里轮询等一会儿，而不是"看一眼、还锁着就报错"。
  $freed = $false
  foreach ($i in 1..40) {                 # 40 × 250 ms = 最多 10 秒
    if (-not (Test-Locked $exe)) { $freed = $true; break }
    Start-Sleep -Milliseconds 250
  }
  if (-not $freed) {
    throw "$exe 在结束进程 10 秒后仍然打不开。除了 desk2onvif，也可能是杀毒软件在扫它 —— 稍等片刻重跑。"
  }
}

# CMake 在缓存里写的是正斜杠路径（我们就是那么传进去的），所以比对也用正斜杠。
# 拿反斜杠的原路径去 match 永远不会命中，表现是每次构建都白重配一遍。
$gppFwd   = $gpp   -replace '\\', '/'
$ninjaFwd = $ninja -replace '\\', '/'

$configure = $true
if (Test-Path (Join-Path $bd 'CMakeCache.txt')) {
  # 缓存里记的编译器/生成器还是不是我们这套？换了就得重配。
  $cache = Get-Content (Join-Path $bd 'CMakeCache.txt') -Raw
  if ($cache -match [Regex]::Escape($gppFwd) -and $cache -match [Regex]::Escape($ninjaFwd)) {
    $configure = $false
  } else {
    Write-Host "[build] 缓存里的工具链不是当前这套，重新配置"
    Remove-Item $bd -Recurse -Force
  }
}

if ($configure) {
  Write-Host "[build] 配置 $bd ($BuildType)"
  & $cmake -S $Root -B $bd -G Ninja `
    "-DCMAKE_BUILD_TYPE=$BuildType" `
    "-DCMAKE_C_COMPILER=$($gcc -replace '\\','/')" `
    "-DCMAKE_CXX_COMPILER=$gppFwd" `
    "-DCMAKE_MAKE_PROGRAM=$ninjaFwd"
  if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败，退出码 $LASTEXITCODE" }
}

Write-Host "[build] 编译（-$Jobs 并行）"
$buildArgs = @('--build', $bd, '--parallel', "$Jobs")
if ($Target) { $buildArgs += @('--target', $Target) }
& $cmake @buildArgs
if ($LASTEXITCODE -ne 0) {
  # 上面那次探测是在编译**之前**做的，而"编译期间有人把服务起起来了"照样会
  # 在链接时撞上同一个锁。这里再探一次：如果现在打不开了，原因就是它，
  # 不是源码 —— 报清楚，别让人去翻代码。
  #
  # 只在"这次构建真的会产出这个 exe"时才把锁当成原因：编 desk2onvif_core 之类
  # 别的目标失败时，exe 恰好被占用只是个巧合，说成原因就把人带偏了。
  $buildsExe = (-not $Target) -or ($Target -eq 'desk2onvif')
  if ($buildsExe -and (Test-Locked $exe)) {
    throw @"
编译失败，而且 $exe 现在是打不开的 —— 原因多半就是这个，不是源码问题。
上面 ld 那句 "Permission denied" 说的就是它。用 -StopRunning 重跑，或先关掉那个进程。
"@
  }
  throw "编译失败，退出码 $LASTEXITCODE"
}

if (-not $NoSelfCheck -and -not $Target -and (Test-Path $exe)) {
  Write-Host ""
  Write-Host "[build] 自检 $exe"
  & $exe --selfcheck
  if ($LASTEXITCODE -ne 0) { throw "自检失败，退出码 $LASTEXITCODE" }
}
