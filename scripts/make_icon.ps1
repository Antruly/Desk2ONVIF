<#
  生成 resources\desk2onvif.ico（以及作为参考的 icon.png）。

  图形是**用 GDI+ 画出来的**，不是一张需要人来维护的二进制素材 —— 这样它能进
  版本库、能改、能一眼看出画的是什么。设计上有意只用大色块：
  16x16 的任务栏图标放不下任何细节，细线和小字在那个尺寸下会糊成一团。
  所以只有三样东西：深色圆角底、白色显示器、右上角一个强调色圆点。

  用法：powershell -ExecutionPolicy Bypass -File scripts\make_icon.ps1
#>
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$res  = Join-Path $root 'resources'
$null = New-Item -ItemType Directory -Path $res -Force
$png  = Join-Path $res 'icon.png'
$ico  = Join-Path $res 'desk2onvif.ico'

# --- 先画一张 1024 的大图，缩小的活交给 ICO 生成那一步 ----------------------
$S = 1024
$bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.Clear([System.Drawing.Color]::Transparent)

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# 底色：从深蓝到近黑的对角渐变，和 Web 控制台的深色主题同一族
$bgRect = New-Object System.Drawing.Rectangle(0, 0, $S, $S)
$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $bgRect,
        [System.Drawing.Color]::FromArgb(255, 46, 66, 104),
        [System.Drawing.Color]::FromArgb(255, 16, 22, 33),
        [System.Drawing.Drawing2D.LinearGradientMode]::ForwardDiagonal)
$path = New-RoundedPath 0 0 $S $S 190
$g.FillPath($bg, $path)

# 显示器：屏幕（白，圆角）+ 支架 + 底座
$white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
$screenW = 620.0; $screenH = 400.0
$screenX = (($S - $screenW) / 2); $screenY = 250.0
$sp = New-RoundedPath $screenX $screenY $screenW $screenH 34
$g.FillPath($white, $sp)

# 屏幕里挖掉一块深色，让它读起来是"屏"而不是一块白板
$inner = New-RoundedPath ($screenX + 46) ($screenY + 46) ($screenW - 92) ($screenH - 92) 18
$deep = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 22, 32, 48))
$g.FillPath($deep, $inner)

# 支架（梯形用多边形画）和底座
$cx = $S / 2.0
$stand = New-Object System.Drawing.Drawing2D.GraphicsPath
$pts = [System.Drawing.PointF[]]@(
    [System.Drawing.PointF]::new([single]($cx - 70),  [single]650),
    [System.Drawing.PointF]::new([single]($cx + 70),  [single]650),
    [System.Drawing.PointF]::new([single]($cx + 108), [single]748),
    [System.Drawing.PointF]::new([single]($cx - 108), [single]748))
$stand.AddPolygon($pts)
$g.FillPath($white, $stand)
$base = New-RoundedPath ($cx - 200) 748 400 58 28
$g.FillPath($white, $base)

# 右上角的"在推流"标记：和 Web 控制台的强调色一致，也呼应程序里那个状态灯。
#
# 它压在屏幕的右上角上，所以**必须有白圈把它和屏幕隔开**。第一版是蓝圆点
# 直接叠在白屏幕上，256 下看着还行，16x16 下蓝点和屏幕边缘糊成一坨，
# 整个图标读起来就是一个缺角的方块。白圈在小尺寸下提供的那一圈对比，
# 是"这是个独立的徽标"和"屏幕破了个洞"之间的区别。
$badgeCx = 806.0; $badgeCy = 226.0
$accent  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 57, 135, 229))
$g.FillEllipse($white,  [single]($badgeCx - 122), [single]($badgeCy - 122), 244, 244)  # 白圈
$g.FillEllipse($accent, [single]($badgeCx - 96),  [single]($badgeCy - 96),  192, 192)  # 蓝底
$g.FillEllipse($white,  [single]($badgeCx - 34),  [single]($badgeCy - 34),   68,  68)  # 中心白点

$g.Dispose()
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)

# --- PNG → 多尺寸 ICO -------------------------------------------------------
# 每个尺寸单独存一条 32bpp BMP-DIB（XOR 位图倒序 BGRA + 全零 AND 掩码）。
# 32bpp 下透明度由 alpha 通道带，AND 掩码必须留着且全零。
$sizes = @(256, 128, 64, 48, 32, 16)
$blobs = New-Object System.Collections.Generic.List[byte[]]
$img = [System.Drawing.Image]::FromFile($png)

foreach ($s in $sizes) {
    $sb = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $sg = [System.Drawing.Graphics]::FromImage($sb)
    $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $sg.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $sg.CompositingMode   = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $sg.DrawImage($img, 0, 0, $s, $s)
    $sg.Dispose()

    $rect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $data = $sb.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                         [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = [Math]::Abs($data.Stride)
    $raw = New-Object byte[] ($stride * $s)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $raw, 0, $raw.Length)
    $sb.UnlockBits($data)

    $xor = New-Object byte[] ($s * $s * 4)
    for ($y = 0; $y -lt $s; $y++) {
        [Array]::Copy($raw, $y * $stride, $xor, ($s - 1 - $y) * ($s * 4), $s * 4)
    }
    $andRowPadded = [Math]::Ceiling([Math]::Ceiling($s / 8.0) / 4.0) * 4
    $and = New-Object byte[] ($andRowPadded * $s)

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([int32]40); $bw.Write([int32]$s); $bw.Write([int32]($s * 2))
    $bw.Write([int16]1); $bw.Write([int16]32); $bw.Write([int32]0)
    $bw.Write([int32]($xor.Length + $and.Length))
    $bw.Write([int32]0); $bw.Write([int32]0); $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write($xor); $bw.Write($and); $bw.Flush()
    $blobs.Add($ms.ToArray())
    $bw.Dispose(); $ms.Dispose(); $sb.Dispose()
}
$img.Dispose()
$bmp.Dispose()

$offset = 6 + 16 * $sizes.Count
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($out)
$w.Write([int16]0); $w.Write([int16]1); $w.Write([int16]$sizes.Count)
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $dim = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([int16]1); $w.Write([int16]32)
    $w.Write([int32]$blobs[$i].Length); $w.Write([int32]$offset)
    $offset += $blobs[$i].Length
}
foreach ($b in $blobs) { $w.Write($b) }
$w.Flush()
[IO.File]::WriteAllBytes($ico, $out.ToArray())
$w.Dispose(); $out.Dispose()

Write-Host ("wrote {0}  ({1} bytes, {2} sizes: {3})" -f `
    $ico, (Get-Item $ico).Length, $sizes.Count, ($sizes -join '/'))
Write-Host ("      {0} (source art, for reference)" -f $png)
