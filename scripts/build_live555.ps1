<#
.SYNOPSIS
  把 Live555 构建到 third_party\live555（MinGW64）。

.DESCRIPTION
  这是 scripts\build_live555.sh 的包装：真正的构建在 MSYS2 的 bash 里跑，
  因为 Live555 的 genMakefiles 和各 Makefile 都是 POSIX shell 脚本。
  幂等，重复执行无副作用。

.EXAMPLE
  .\scripts\build_live555.ps1
  .\scripts\build_live555.ps1 -Jobs 8
  .\scripts\build_live555.ps1 -Archive D:\downloads\live555-latest.tar.gz
#>
[CmdletBinding()]
param(
  [string]$CacheDir = '',
  [string]$Dest     = '',
  [string]$Work     = '',
  [int]$Jobs = 0,
  [string]$Archive = ''
)
$ErrorActionPreference = 'Stop'

$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $CacheDir) { $CacheDir = Join-Path $Root 'third_party\_cache' }
if (-not $Dest)     { $Dest     = Join-Path $Root 'third_party\live555' }
if (-not $Work)     { $Work     = Join-Path $Root 'third_party\_build\live555' }
if ($Jobs -le 0)    { $Jobs     = [Environment]::ProcessorCount }

# 自己转 /c/... /d/... —— git-bash 的转换在这里不可靠
function ConvertTo-PosixPath([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path).Replace('\', '/')
  if ($full -match '^([A-Za-z]):/(.*)$') {
    return '/' + $matches[1].ToLower() + '/' + $matches[2]
  }
  return $full
}

$bash = @('C:\msys64\usr\bin\bash.exe', "$env:SystemDrive\msys64\usr\bin\bash.exe") |
  Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $bash) { throw '找不到 MSYS2 的 bash.exe（期望 C:\msys64\usr\bin\bash.exe）。' }

$parts = @(
  (ConvertTo-PosixPath (Join-Path $PSScriptRoot 'build_live555.sh')),
  (ConvertTo-PosixPath $CacheDir),
  (ConvertTo-PosixPath $Dest),
  (ConvertTo-PosixPath $Work),
  "$Jobs"
)
if ($Archive) { $parts += (ConvertTo-PosixPath $Archive) }

# 单引号包住每个参数，路径含空格也不会被拆开
$cmd = ($parts | ForEach-Object { "'" + $_ + "'" }) -join ' '

Write-Host "[live555] bash : $bash"
Write-Host "[live555] jobs : $Jobs"
Write-Host "[live555] dest : $Dest"
# stderr 在 bash 内部就并进 stdout：PowerShell 5.1 会把原生程序的 stderr
# 包装成 ErrorRecord，叠上 $ErrorActionPreference='Stop' 会**误报失败** ——
# make 的 warning 全走 stderr，会看起来像构建挂了。成败只看退出码。
& $bash -lc ($cmd + ' 2>&1')
if ($LASTEXITCODE -ne 0) { throw "Live555 构建失败，退出码 $LASTEXITCODE" }
