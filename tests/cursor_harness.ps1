#Requires -Version 5.1
<#
.SYNOPSIS
  在一整块纯色全屏画布上驱动鼠标走一条已知路径，同时录制成视频。

.DESCRIPTION
  为什么非要在纯色画布上录：校验指针合成的办法是"两帧相减，差异应当只出现在指针
  所在的位置上"。在真实桌面上这条判据根本不成立 —— 文字边缘在 ultrafast 档下会产生
  幅度超过 200 的压缩振铃，散布在整屏。实测（2880x1920，两帧相隔 1.5 秒）：

      容差  8：288235 个像素不同，只有 1.3% 落在指针周围
      容差 48：  1146 个像素不同，只有 41.7% 落在指针周围
      容差 96：   812 个像素不同，只有 55.5% 落在指针周围，包围盒 x[47..2840]

  也就是说"差异的幅度"根本区分不开指针和压缩噪声 —— 两者都能从黑跳到白。
  纯色画布把这个问题从根上去掉：平坦区域没有可振铃的高频，差异只剩指针本身。

  代价是桌面静止时 DXGI 不产出帧，录到的几乎全是"只有鼠标动了才产出"的那些帧。
  这恰恰是合成路径里最该测的一半：它必须靠一份**没有指针的桌面图**重新合成，而不是
  重新去读纹理（那一刻纹理里是未定义内容）。

  指针是**分步**走的，不是一步跳过去的：一步跳过去只能证明"终点画对了"；分步才能
  证明"上一个位置被擦干净了" —— 擦除漏掉的话，每多走一步就会多出一团残留。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tests\cursor_harness.ps1
  python -I -X utf8 tests\cursor_probe.py out\cursor-flat.mp4 --bg 0,102,204 --from 300,300 --to 1200,900

  探针那条命令里的 `-X utf8` 不是装饰：`-I` 隐含 `-E`，会忽略
  PYTHONIOENCODING，于是输出一旦被重定向到文件/管道，Python 就用本地
  代码页（936）编码，中文全成乱码而退出码照样是 0。
  挂在控制台上时看不出来 —— 那条路走 WriteConsoleW，代码页不参与。
#>
param(
  [string]$Out   = 'out\cursor-flat.mp4',
  [int]   $Seconds = 5,
  [string]$From  = '300,300',
  [string]$To    = '1200,900',
  [int]   $Steps = 8,
  [string]$Color = '0,102,204',
  [string]$Exe   = 'build\bin\desk2onvif.exe'
)

$ErrorActionPreference = 'Stop'

function Parse-Point([string]$s) {
  $p = $s.Split(',')
  if ($p.Count -ne 2) { throw "坐标要写成 x,y，收到的是 '$s'" }
  return [Drawing.Point]::new([int]$p[0], [int]$p[1])
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# 必须先声明 DPI 感知，再建任何窗口。否则本进程是"DPI 不感知"的，Windows 会给它
# 一套虚拟化坐标：Screen.Bounds 报 1440x960（逻辑），而实际帧缓冲是 2880x1920；
# 实测把指针设到 (1200,900)，画面上落在 (2400,1800) —— **正好是两倍**。
# 那样的话脚本里写的坐标和画面上验证的坐标对不上，而且这种错在小数缩放
# （125%、150%）下还会变成非整数倍，更难解释。
Add-Type -Namespace D2O -Name Dpi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
'@
[D2O.Dpi]::SetProcessDPIAware() | Out-Null

$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$exePath = Join-Path $root $Exe
$outPath = Join-Path $root $Out
if (-not (Test-Path $exePath)) { throw "找不到 $exePath，先跑 scripts\build.ps1" }

# 输出目录可能还不存在（全新 clone）。--record 自己不会建目录。
$outDir = Split-Path -Parent $outPath
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }

$rgb = $Color.Split(',')
$bg  = [Drawing.Color]::FromArgb([int]$rgb[0], [int]$rgb[1], [int]$rgb[2])
$a   = Parse-Point $From
$b   = Parse-Point $To
$log = [IO.Path]::ChangeExtension($outPath, '.log')
$errPath = [IO.Path]::ChangeExtension($outPath, '.err')

$form = New-Object Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.ShowInTaskbar   = $false
$form.TopMost         = $true
$form.BackColor       = $bg
$form.Bounds          = [Windows.Forms.Screen]::PrimaryScreen.Bounds

$proc = $null
try {
  # 先亮画布再摆指针：这样**第一帧**（DuplicateOutput 之后那一次 bootstrap）里
  # 指针就已经在起点上了，不必事后靠时间对齐去猜。
  $form.Show()
  [Windows.Forms.Application]::DoEvents()
  [Windows.Forms.Cursor]::Position = $a
  Start-Sleep -Milliseconds 600
  [Windows.Forms.Application]::DoEvents()

  Write-Host "[harness] 画布 $($form.Bounds.Width)x$($form.Bounds.Height) RGB($Color)，指针在 $From"
  Write-Host "[harness] 开始录制 $Seconds 秒 -> $Out"

  # 自己用 ProcessStartInfo 起进程，不走 Start-Process。**PS 5.1 下
  # `Start-Process -PassThru` 只要带了 -RedirectStandardOutput，返回对象上的
  # `ExitCode` 就永远是空的**（`HasExited` 却是 True，实测如此）。一个永远报不出
  # 失败的成功报告比没有报告更糟：录制崩了、MP4 没写出来，harness 照样安静退出。
  # 自己起的 Process 读到的退出码是真的。
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName               = $exePath
  $psi.Arguments              = '--record "{0}" --seconds {1} --quiet' -f $outPath, $Seconds
  $psi.UseShellExecute        = $false
  $psi.CreateNoWindow         = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError  = $true

  $proc = New-Object Diagnostics.Process
  $proc.StartInfo = $psi
  if (-not $proc.Start()) { throw "起不来：$exePath" }

  # 两条管道都必须立刻异步读走。子进程往一个没人读的管道里写满约 4 KB 就会阻塞 ——
  # 它阻塞在哪一步（刚开始录 / 收尾写 MP4 索引）决定了这个测试是"失败"还是
  # "莫名其妙没有文件"。
  $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
  $stderrTask = $proc.StandardError.ReadToEndAsync()

  # 等捕获会话建立、编码器开始出帧。开机到稳定大约 0.2 秒，这里给足余量 ——
  # 指针要是比录制开始得早太多，第一段就白录了；太晚则路径被截断。
  Start-Sleep -Milliseconds 2500

  for ($i = 1; $i -le $Steps; $i++) {
    $t = $i / [double]$Steps
    $x = [int][Math]::Round($a.X + ($b.X - $a.X) * $t)
    $y = [int][Math]::Round($a.Y + ($b.Y - $a.Y) * $t)
    [Windows.Forms.Cursor]::Position = [Drawing.Point]::new($x, $y)
    # DoEvents 只是让画布保持绘制；指针位置是 SetCursorPos 直接生效的，
    # 不依赖消息循环。
    [Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 100
    Write-Host "[harness]   第 $i/$Steps 步 -> ($x,$y)"
  }

  # 停在终点，等录制自己按 --seconds 收尾（MP4 的索引要它正常退出才会写）。
  if (-not $proc.WaitForExit(($Seconds + 15) * 1000)) {
    Write-Warning "[harness] 录制进程 $($Seconds + 15) 秒还没退出，强制结束 —— MP4 索引可能不完整"
    $proc.Kill()
    $proc.WaitForExit(5000) | Out-Null
  }
  $proc.WaitForExit()
  # 输出在这时才拿到（ReadToEndAsync 要等管道关闭）。
  [IO.File]::WriteAllText($log, $stdoutTask.Result)
  [IO.File]::WriteAllText($errPath, $stderrTask.Result)

  $code = $proc.ExitCode
  if ($code -ne 0) {
    throw "录制进程退出码 $code —— 视频大概是不完整的，看 $errPath"
  }
  Write-Host "[harness] 退出码 $code，视频写到 $Out"
}
finally {
  # 画布必须无条件收掉：中途抛异常时把人家的桌面留在一整块纯色上，
  # 是比测试失败严重得多的后果。
  # 没起来的 Process 上访问 HasExited 会抛 InvalidOperationException —— 而那会
  # 盖掉真正的原因，所以这里再兜一层。
  if ($proc) {
    try {
      if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null }
    } catch { }
  }
  $form.Close()
  $form.Dispose()
}
