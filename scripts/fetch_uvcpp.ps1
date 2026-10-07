<#
.SYNOPSIS
  把本机已构建好的 libuvcpp 铺到 third_party\uvcpp。

.DESCRIPTION
  libuvcpp 与前两个依赖不同，**不走下载**：带 SOAP/WSDL 模块的 MinGW64 版本
  必须从源码编，所有预编译包的 UVCPP_WSDL_ENABLE 都是 0（头文件在，符号不在）。
  所以本脚本只做「校验 + 搬运」，不重新编库。构建步骤见 third_party\README.md。

  搬四样东西：
    include\        源码树的公开头（<Source>\src 下的 .h/.hpp 镜像过来）
    include-gen\    configure 期生成的 uvcpp_config.h（在构建目录里）
    include-ext\    头文件依赖 nlohmann/json（webapp 的 JSON 层要用）
    lib\            libuvcpp.dll.a **和 libuv.dll.a**
    runtime\        exe 旁边必须摆的那几个 DLL

  第二份导入库（libuv.dll.a）不是可选的：libuvcpp 的公开头里有内联函数直接调
  libuv（例如 `uvcpp_udp::recv_stop()` 的方法体就是 `uv_udp_recv_stop(...)`），
  所以**用到那些内联函数的调用方必须自己链得上 libuv**。少了它的症状是一串
  `undefined reference to 'uv_udp_recv_stop'` / `'uv_ip4_addr'`，而报错指向的是
  我们自己的 .cpp —— 看起来像是我们漏了什么头，实际是缺一个导入库。

  幂等：目标已完整时直接返回。

.EXAMPLE
  .\scripts\fetch_uvcpp.ps1
  .\scripts\fetch_uvcpp.ps1 -Source D:\src\libuvcpp -BuildDir build-mingw64-full
#>
[CmdletBinding()]
param(
  [string]$Source   = '',
  [string]$BuildDir = '',
  [string]$Dest     = '',
  [string]$GccBin   = '',
  [switch]$Force
)
$ErrorActionPreference = 'Stop'

$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Dest)     { $Dest     = Join-Path $Root 'third_party\uvcpp' }
if (-not $Source) {
  if ($env:UVCPP_SOURCE) { $Source = $env:UVCPP_SOURCE } else { $Source = 'D:\public\libuvcpp\libuvcpp' }
}
if (-not $BuildDir) { $BuildDir = 'build-mingw64-full' }
if (-not $GccBin)   { $GccBin   = 'C:\msys64\mingw64\bin' }

$Build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Source $BuildDir }

# --- 幂等检查 --------------------------------------------------------------
$stampFiles = @(
  (Join-Path $Dest 'lib\libuvcpp.dll.a'),
  (Join-Path $Dest 'lib\libuv.dll.a'),
  (Join-Path $Dest 'include-gen\uvcpp\uvcpp_config.h'),
  (Join-Path $Dest 'include\uvcpp\uvcpp_version.h'),
  (Join-Path $Dest 'include-ext\nlohmann\json.hpp'),
  (Join-Path $Dest 'runtime\libuvcpp.dll')
)
if (-not $Force) {
  $missing = $stampFiles | Where-Object { -not (Test-Path $_) }
  if (-not $missing) {
    Write-Host "[uvcpp] 已就绪，跳过：$Dest"
    return
  }
}

# --- 校验源与构建目录 ------------------------------------------------------
if (-not (Test-Path (Join-Path $Source 'src\uvcpp\uvcpp_version.h'))) {
  throw "[uvcpp] 找不到源码树：$Source`n（期望 <Source>\src\uvcpp\uvcpp_version.h）"
}
$dllA = Join-Path $Build 'libuvcpp.dll.a'
$dll  = Join-Path $Build 'libuvcpp.dll'
if (-not (Test-Path $dllA) -or -not (Test-Path $dll)) {
  throw @"
[uvcpp] 构建目录里没有 libuvcpp.dll / libuvcpp.dll.a：$Build
全量（含 SOAP/WSDL）的 MinGW64 版本必须自己编，预编译包一律是 UVCPP_WSDL_ENABLE 0。
构建命令见 third_party\README.md，或在源码树里跑：
  cmake -S . -B $BuildDir -G Ninja ``
    -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe ``
    -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe ``
    -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/ninja.exe ``
    -DUVCPP_BUILD_SHARED=ON -DUVCPP_BUILD_STATIC=OFF ``
    -DUVCPP_BUILD_NET=ON -DUVCPP_BUILD_WEB=ON -DUVCPP_BUILD_WEBAPP=ON ``
    -DUVCPP_ENABLE_WSDL=ON -DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_ENABLE_ZLIB=ON ``
    -DUVCPP_ENABLE_CAPI=ON -DUVCPP_BUILD_TESTS=OFF -DUVCPP_BUILD_EXAMPLES=OFF
"@
}

$genCfg = Join-Path $Build 'include\uvcpp\uvcpp_config.h'
if (-not (Test-Path $genCfg)) { throw "[uvcpp] 找不到生成的头：$genCfg" }

$jsonInc = Join-Path $Source '_local_deps\json-3113\include\nlohmann\json.hpp'
if (-not (Test-Path $jsonInc)) { throw "[uvcpp] 找不到 nlohmann/json：$jsonInc" }

# --- 1. 头文件（只搬头，不搬 .cpp） ----------------------------------------
$incDst = Join-Path $Dest 'include'
if (Test-Path $incDst) { Remove-Item $incDst -Recurse -Force }
$exts = @('.h', '.hpp', '.inl', '.ipp')
$headers = Get-ChildItem (Join-Path $Source 'src') -Recurse -File |
  Where-Object { $exts -contains $_.Extension.ToLower() }
$srcLen = (Join-Path $Source 'src').Length + 1
foreach ($h in $headers) {
  $rel = $h.FullName.Substring($srcLen)
  $out = Join-Path $incDst $rel
  New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($out)) | Out-Null
  Copy-Item $h.FullName $out -Force
}
Write-Host "[uvcpp] 头文件 $($headers.Count) 个 → $incDst"

# --- 2. 生成的头 -----------------------------------------------------------
$genDst = Join-Path $Dest 'include-gen\uvcpp'
New-Item -ItemType Directory -Force -Path $genDst | Out-Null
Copy-Item $genCfg (Join-Path $genDst 'uvcpp_config.h') -Force
Write-Host "[uvcpp] 生成头 → $genDst"

# --- 3. 外部头依赖（nlohmann） ---------------------------------------------
# uvcpp_web_json.h 里 `#include <nlohmann/json.hpp>` 是**暴露面**：任何碰 webapp
# 的编译单元都得看得见它，所以这一份必须跟着铺过来，不能指望系统里刚好有。
$extDst = Join-Path $Dest 'include-ext'
if (Test-Path $extDst) { Remove-Item $extDst -Recurse -Force }
New-Item -ItemType Directory -Force -Path $extDst | Out-Null
Copy-Item (Join-Path $Source '_local_deps\json-3113\include\nlohmann') -Destination $extDst -Recurse -Force
Write-Host "[uvcpp] nlohmann → $extDst"

# 在构建树里按名字找文件。定义必须在**第一次调用之前** —— PowerShell 是
# 从上往下执行的，把函数放在调用点后面是个运行期才暴露的错误。
function Find-OneDll {
  param([string]$Name, [string[]]$SearchRoots, [string]$FixedDir)
  if ($FixedDir) {
    $p = Join-Path $FixedDir $Name
    if (Test-Path $p) { return $p }
  }
  foreach ($r in $SearchRoots) {
    if (-not (Test-Path $r)) { continue }
    $hit = Get-ChildItem $r -Recurse -Filter $Name -File -ErrorAction SilentlyContinue |
      Select-Object -First 1
    if ($hit) { return $hit.FullName }
  }
  return $null
}

# --- 4. 导入库 -------------------------------------------------------------
$libDst = Join-Path $Dest 'lib'
New-Item -ItemType Directory -Force -Path $libDst | Out-Null
Copy-Item $dllA (Join-Path $libDst 'libuvcpp.dll.a') -Force

# libuv 的导入库。见文件头说明：libuvcpp 的公开头里有直接调 uv_* 的内联函数。
# **必须取 libuvcpp 自己编出来的那一份**，不能用 C:\msys64\mingw64\lib\libuv.dll.a：
# 导入库是符号存根，版本对不上时会缺符号（或者更糟，链上一个 DLL 里根本没有的
# 符号，运行期才炸）。同一个构建目录出来的才和 runtime\libuv.dll 一一对应。
$uvA = Join-Path $Build '_deps\libuv-build\libuv.dll.a'
if (-not (Test-Path $uvA)) {
  $uvA = Find-OneDll -Name 'libuv.dll.a' -SearchRoots @($Build) -FixedDir ''
}
if (-not $uvA) {
  throw "[uvcpp] 找不到 libuv 的导入库（libuv.dll.a），在 $Build 下搜过了。`n它由 libuvcpp 的 FetchContent 构建产生，通常和 libuv.dll 在同一个目录。"
}
Copy-Item $uvA (Join-Path $libDst 'libuv.dll.a') -Force
Write-Host "[uvcpp] 导入库 → $libDst（libuvcpp + libuv）"

# --- 5. 运行期 DLL ---------------------------------------------------------
# 这份清单不是猜的，是把 objdump -p libuvcpp.dll 的 import 表读出来对过的。
# libstdc++ / libgcc / libwinpthread 三个来自 GCC 运行时目录；
# libuv / libllhttp / zlib1__ 是 libuvcpp 构建时从源码编出来的，只在构建树里。
$rtDst = Join-Path $Dest 'runtime'
New-Item -ItemType Directory -Force -Path $rtDst | Out-Null

$spec = @(
  @{ Name = 'libuvcpp.dll';         Fixed = $Build;                                Roots = @() },
  @{ Name = 'libuv.dll';            Fixed = (Join-Path $Build '_deps\libuv-build'); Roots = @($Build) },
  @{ Name = 'libllhttp.dll';        Fixed = (Join-Path $Build '_deps\llhttp-build'); Roots = @($Build) },
  @{ Name = 'zlib1__.dll';          Fixed = '';                                     Roots = @($Build) },
  @{ Name = 'libstdc++-6.dll';      Fixed = $GccBin;                                Roots = @() },
  @{ Name = 'libgcc_s_seh-1.dll';   Fixed = $GccBin;                                Roots = @() },
  @{ Name = 'libwinpthread-1.dll';  Fixed = $GccBin;                                Roots = @() }
)

$notFound = @()
foreach ($s in $spec) {
  $p = Find-OneDll -Name $s.Name -SearchRoots $s.Roots -FixedDir $s.Fixed
  if ($p) {
    Copy-Item $p (Join-Path $rtDst $s.Name) -Force
    Write-Host ("[uvcpp] runtime {0,-22} ← {1}" -f $s.Name, $p)
  } else {
    $notFound += $s.Name
  }
}
if ($notFound) {
  throw "[uvcpp] 运行期 DLL 没找到：$($notFound -join ', ')`n先确认 $Build 是完整的构建产物，且 $GccBin 下有 GCC 运行时。"
}

Write-Host "[uvcpp] 完成：$Dest"
