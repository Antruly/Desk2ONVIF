<#
.SYNOPSIS
  把 gyan.dev 的 FFmpeg shared 包铺到 third_party\ffmpeg。

.DESCRIPTION
  幂等：third_party\ffmpeg\include\libavcodec\avcodec.h 存在时直接返回。
  取包顺序：-Archive 指定 > _cache 里已有的 ffmpeg-*-full_build-shared.7z > 联网下载。
  下回来的压缩包会留在 _cache，下次换机器可以直接拷过去离线用。

  选 gyan.dev 的 full_build **shared** 包的理由：它是 MinGW 构建，带 .dll.a
  导入库，MinGW 可以直接链接（MSVC 的 .lib 不行）；而且 full_build 才带 libx264。

.EXAMPLE
  .\scripts\fetch_ffmpeg.ps1
  .\scripts\fetch_ffmpeg.ps1 -Archive D:\downloads\ffmpeg-9.0.2-full_build-shared.7z
#>
[CmdletBinding()]
param(
  [string]$Dest     = '',
  [string]$CacheDir = '',
  [string]$Archive  = '',
  [string]$Url      = 'https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-9.0.2-full_build-shared.7z'
)
$ErrorActionPreference = 'Stop'

$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Dest)     { $Dest     = Join-Path $Root 'third_party\ffmpeg' }
if (-not $CacheDir) { $CacheDir = Join-Path $Root 'third_party\_cache' }

if (Test-Path (Join-Path $Dest 'include\libavcodec\avcodec.h')) {
  Write-Host "[ffmpeg] 已就绪，跳过：$Dest"
  return
}

$sevenZip = @(
  (Join-Path ${env:ProgramFiles} '7-Zip\7z.exe'),
  (Join-Path ${env:ProgramFiles(x86)} '7-Zip\7z.exe')
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $sevenZip) { throw '[ffmpeg] 找不到 7z.exe，请先装 7-Zip。' }

New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null

if (-not $Archive) {
  $cached = Get-ChildItem $CacheDir -Filter 'ffmpeg-*-full_build-shared.7z' -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if ($cached) { $Archive = $cached.FullName }
}
if (-not $Archive) {
  $Archive = Join-Path $CacheDir ([IO.Path]::GetFileName($Url))
  Write-Host "[ffmpeg] 下载 $Url"
  Invoke-WebRequest -Uri $Url -OutFile $Archive -UseBasicParsing
}
if (-not (Test-Path $Archive)) { throw "[ffmpeg] 压缩包不存在：$Archive" }
Write-Host "[ffmpeg] 压缩包：$Archive"

# 解到临时目录，再取内层的 ffmpeg-*-full_build-shared
$tmp = Join-Path $CacheDir ('_x_' + [IO.Path]::GetFileNameWithoutExtension($Archive))
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
# 7z 的进度和告警可能走 stderr；PS 5.1 会把它包成 ErrorRecord，
# 叠上 $ErrorActionPreference='Stop' 就成了误报。这里临时放开，只看退出码。
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $sevenZip x $Archive "-o$tmp" -y | Out-Null
$zipExit = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($zipExit -ne 0) { throw "[ffmpeg] 7z 解包失败，退出码 $zipExit" }

$inner = Get-ChildItem $tmp -Directory | Where-Object { $_.Name -like 'ffmpeg-*' } | Select-Object -First 1
if (-not $inner) { throw '[ffmpeg] 解包后没找到 ffmpeg-* 目录' }

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
foreach ($sub in 'bin', 'include', 'lib') {
  $s = Join-Path $inner.FullName $sub
  if (Test-Path $s) { Copy-Item $s -Destination $Dest -Recurse -Force }
}
Copy-Item (Join-Path $inner.FullName 'LICENSE') -Destination $Dest -Force -ErrorAction SilentlyContinue
Remove-Item $tmp -Recurse -Force

Write-Host "[ffmpeg] 完成：$Dest"
Get-ChildItem (Join-Path $Dest 'lib') -Filter '*.dll.a' | Select-Object -ExpandProperty Name
