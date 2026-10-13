#Requires -Version 5.1
<#
.SYNOPSIS
  在一整块上暗下亮的双色全屏画布上录一段，供 osd_probe.py 验水印。

.DESCRIPTION
  为什么要一块**双色**画布：水印的自适应配色要一次验两个方向 ——
  左上角落在暗底上必须是白字，右下角落在亮底上必须是黑字。一种底色只能验
  一半，而"配色方向写反了"恰好是只在一半上看得出来的错。

  为什么必须是**平坦**的画布：判据是"偏离底色的像素就是水印"。真实桌面上
  文字边缘在 ultrafast 档下的压缩振铃能把单个像素从黑甩到白（cursor_harness
  的头注释里有实测数字），于是"偏离多少"根本分不出水印和噪声。平坦画布把
  振铃的来源直接去掉。

  还有一层是这个脚本**故意不干的事**：不驱动鼠标、不制造任何画面变化。
  画布静止 ⇒ 画布本身不会再产出新画面。这条路径（capture_session 里
  `osd_changed && last` 那个分支）是整个水印功能里最容易坏、坏了最看不出来
  的一半 —— 桌面静止时它能保证水印上的钟还在走。真坏了的话录出来的段只有
  开头那一两帧，探针会报"视频里没有 N 秒之后的帧"。

  **但"画布静止"只是必要条件，不是充分条件**：指针自己会产出帧
  （capture_session 的 `pointer_only_frames`，实测 6 秒里 298 帧），所以
  "后面还有帧"并不能证明那些帧是水印逼出来的。直接证据是日志里那一行
  `水印刷新 N 帧`（`SessionStats::summary()` 打的，只在 N>0 时出现）。
  跑完看一眼那行，不要只看探针绿不绿。

  所以指针要**钉在一个已知位置**，而且要钉在**远离两个角**的地方。探针只在
  两个角上各开一个窗口来量水印，窗口之外的一律不算数 —— 但那不等于指针可以
  乱放：手碰一下鼠标它就走了（实测一次 6 秒的录制里挪了 146 像素），而它要是
  正好压在角上，量出来的"墨迹"里就混进了指针，报出来的错会指向配色或位置。
  钉住它、并把落点打出来，是为了让"指针在窗口外"这件事实可以被对账。

  脚本会把落点打出来，探针的 `--cursor` 传进去只**对账**（差值会打出来），
  不作为判据 —— 拿它当判据的话，这条测试就变成"跑的时候没人碰鼠标"了。

.PARAMETER Items
  覆盖默认那两项水印的 **JSON 数组**（写在一行里，传给配置的 `osd.items`）。
  不给就是默认的两项（时间@左上、名称@右下），既有用法一字未变。
  给它是为了验"水印搬走了"这件事：`tests\osd_move_check.ps1` 用它把时间项
  挪到画面中间的 `custom` 位置上，然后让探针去断言**新位置有字、老位置一个
  像素都没有**。只验"新位置有字"是不够的 —— 一个"两边都画"的实现照样绿。

  注意它只替换 `items` 那一项，`enabled`/`adaptive_color`/`outline`/
  `plate_alpha` 仍然是脚本里写死的那组值（双色画布的几个判据都依赖它们）。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tests\osd_harness.ps1
  python -I -X utf8 tests\osd_probe.py out\osd-harness.mp4 `
      --dark 20,20,24 --light 240,240,236 --split 0.5 `
      --cursor 2520,480 --at 1.0 --at 4.0

.EXAMPLE
  # 水印搬到 55%,60% 处，老位置（左上角）必须一个像素都没有
  $items = '[{"enabled":true,"type":"time","position":"custom",' +
           '"x_pct":55,"y_pct":60,"font_size":0}]'
  powershell -ExecutionPolicy Bypass -File tests\osd_harness.ps1 `
      -Out out\osd-moved.mp4 -Items $items
  python -I -X utf8 tests\osd_probe.py out\osd-moved.mp4 `
      --dark 20,20,24 --light 240,240,236 --split 0.5 `
      --ink-pct 55,60 --blank-corner tl --at 1.0 --at 4.0
#>
param(
  [string]$Out     = 'out\osd-harness.mp4',
  [int]   $Seconds = 6,
  [string]$Dark    = '20,20,24',
  [string]$Light   = '240,240,236',
  [string]$Exe     = 'build\bin\desk2onvif.exe',
  [string]$Items   = ''
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# 必须先声明 DPI 感知，再建任何窗口。否则本进程是"DPI 不感知"的，Windows 会给它
# 一套虚拟化坐标：Screen.Bounds 报 1440x960（逻辑），而帧缓冲实际是 2880x1920。
# 画布会只铺满左上四分之一，而水印的落点判断跟着一起错。
Add-Type -Namespace D2O -Name Dpi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
'@
[D2O.Dpi]::SetProcessDPIAware() | Out-Null

# 把画布顶到最上层。任务栏自己也是 topmost，两者谁在上面取决于谁最后被激活
# —— 而"任务栏盖在画布上"在探针那侧看起来是"水印位置错了"。
Add-Type -Namespace D2O -Name Win -MemberDefinition @'
[DllImport("user32.dll")]
public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int X, int Y,
                                       int cx, int cy, uint flags);
'@

# 录制期间**反复**把画布顶到最上层的一条后台线程。
#
# 为什么必须"反复"而不是"开始前顶一次"：画布的 TopMost 只在没人抢的时候有效。
# 实测两次录制各被别的东西压过一次 —— 一次是 Windows 的开始菜单，一次是 Alt-Tab
# 的切换器 —— 而它们在探针那侧看起来是"水印位置错了"或"水印糊成一团"。压在
# 右下角那种更坏：那正是名称水印待的地方。
#
# 标志是 `SWP_NOSIZE|SWP_NOMOVE|SWP_NOACTIVATE`（0x0013），**NOACTIVATE 是关键**：
# 少了它，每 500 ms 就会把画布激活一次，等于以 2 Hz 抢焦点 —— 用户手边正在动的
# 任何东西都会被反复打断，而那本身就会成为新的干扰源（它是这个测试要消除的东西，
# 不是要引入的东西）。SetWindowPos 只是把窗口挪到 topmost 那一组的顶上，
# 不需要激活就能压住同为 topmost 的任务栏与开始菜单。
Add-Type -Namespace D2O -Name KeepTop -MemberDefinition @'
[DllImport("user32.dll")]
public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int X, int Y,
                                       int cx, int cy, uint flags);

private static System.Threading.Thread _t;
private static volatile bool _stop;

public static void Start(IntPtr hWnd, int intervalMs) {
  Stop();
  _stop = false;
  _t = new System.Threading.Thread(delegate() {
    while (!_stop) {
      SetWindowPos(hWnd, (IntPtr)(-1), 0, 0, 0, 0, 0x0013);
      System.Threading.Thread.Sleep(intervalMs);
    }
  });
  _t.IsBackground = true;
  _t.Start();
}

public static void Stop() {
  _stop = true;
  if (_t != null) { _t.Join(1000); _t = null; }
}
'@

$root    = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$exePath = Join-Path $root $Exe
$outPath = Join-Path $root $Out
if (-not (Test-Path $exePath)) { throw "找不到 $exePath，先跑 scripts\build.ps1" }

$outDir = Split-Path -Parent $outPath
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }
$cfgPath = [IO.Path]::ChangeExtension($outPath, '.json')
$log     = [IO.Path]::ChangeExtension($outPath, '.log')
$errPath = [IO.Path]::ChangeExtension($outPath, '.err')

# 配置里把两项都写成**确定能画出来的**：时间在左上、名称在右下。
#
# 右下角那一项刻意用 `name`（而不是固定文字）：它逼着这条测试去验
# **`DeviceState` 真的接到了 `SessionOptions`** 这条线。这条线断了的话
# `item_text()` 返回空串、`key()` 里那一项被跳过 —— 画面右下角干干净净，
# 而其余一切正常，看起来像"水印功能没做"。用固定文字就永远测不到它。
#
# `onvif.device_name` 写在配置里，是为了让名字有个**确定的**值可断言；
# 注意这条测试**不带 `--http`**，所以走的是"ONVIF 关着、水印仍要显示名称"
# 那条路 —— 名字的初值是 main.cpp 播的种子，不是 ONVIF 推的。
$defaultItems = @'
[
      { "enabled": true, "type": "time", "position": "upper_left",  "font_size": 0 },
      { "enabled": true, "type": "name", "position": "lower_right", "font_size": 0 }
    ]
'@

# 调用方给的 `-Items` 原样进配置。**不能在这里做 JSON 解析再重新序列化** ——
# 那一步会把"调用方到底写了什么"变成"我们以为他写了什么"，而这个参数的用途
# 恰恰是让驱动脚本能构造出配置层不接受、或解析器会改写的形状（比如某项少了
# 一个字段），好让上层那条路也被覆盖到。
$itemsJson = if ($Items) { $Items } else { $defaultItems.Trim() }
$cfgJson = @"
{
  "onvif": { "device_name": "DESK2ONVIF" },
  "osd": {
    "enabled": true,
    "adaptive_color": true,
    "outline": true,
    "plate_alpha": 0,
    "items": $itemsJson
  }
}
"@
[IO.File]::WriteAllText($cfgPath, $cfgJson, (New-Object Text.UTF8Encoding $false))

function Parse-Rgb([string]$s) {
  $p = $s.Split(',')
  if ($p.Count -ne 3) { throw "颜色要写成 R,G,B，收到的是 '$s'" }
  return [Drawing.Color]::FromArgb([int]$p[0], [int]$p[1], [int]$p[2])
}

# **局部变量不能叫 `$dark` / `$light`**：PowerShell 的变量名不区分大小写，
# 于是它和参数 `$Dark` / `$Light` 是同一个变量；而 `param([string]$Dark)`
# 把它变成了**类型受限**的。往里头赋一个 `[Drawing.Color]` 会被静默地
# `ToString()` 成 "Color [A=255, R=20, G=20, B=24]"，然后在 `$g.Clear()`
# 那里报一条指不到这里的转换错误（陷阱 54 的同一个形状）。
$darkColor  = Parse-Rgb $Dark
$lightColor = Parse-Rgb $Light

$form = New-Object Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.ShowInTaskbar   = $false
$form.TopMost         = $true
$bounds               = [Windows.Forms.Screen]::PrimaryScreen.Bounds
$form.Bounds          = $bounds

# 上半暗、下半亮。分界线取正中间：两个水印框各自离分界线至少 800 像素，
# 不会有哪个框横跨两种底色（横跨的话配色是按框内**平均**亮度选的，
# 判据就变得依赖框有多大了）。
$bmp = New-Object Drawing.Bitmap $bounds.Width, $bounds.Height
$g   = [Drawing.Graphics]::FromImage($bmp)
$g.Clear($darkColor)
$brush = New-Object Drawing.SolidBrush $lightColor
$cut   = [int]($bounds.Height / 2)
$g.FillRectangle($brush, 0, $cut, $bounds.Width, $bounds.Height - $cut)
$g.Dispose()
$brush.Dispose()
$form.BackgroundImage = $bmp
$form.BackgroundImageLayout = 'None'

$proc = $null
try {
  $form.Show()
  [Windows.Forms.Application]::DoEvents()
  Start-Sleep -Milliseconds 600
  [Windows.Forms.Application]::DoEvents()

  # 把指针钉在右上象限：那里既不是"左上"也不是"右下"，探针在两个角上开的
  # 测量窗口都够不着它。钉之前先挪 —— 挪动本身会让捕获层合成一帧指针，
  # 这没关系：我们从没打算让指针**不出现**，只是要它的位置**可预测**。
  $parkX = [int]($bounds.Width - $bounds.Width / 8)
  $parkY = [int]($bounds.Height / 4)
  [Windows.Forms.Cursor]::Position = New-Object Drawing.Point $parkX, $parkY
  [Windows.Forms.Application]::DoEvents()
  Start-Sleep -Milliseconds 300
  [Windows.Forms.Application]::DoEvents()

  # **然后把它藏起来。** 钉位置只解决"它待在哪"，解决不了"有没有人去碰它"
  # —— 实测两次录制各被碰走一次（一次挪了 146 像素，一次挪到屏幕左下角，
  # 正好落在右下角那个测量窗口里，探针报出来的却是"水印位置在飘"）。
  # 藏起来之后它压根不进画面（捕获层读 `PointerPosition.Visible`，
  # 见 desktop_capture.cpp），于是"指针在哪儿"这件事对这条测试不再有影响。
  #
  # 藏的是**整个桌面的指针**（ShowCursor 的显示计数），所以 finally 里一定要
  # 还回来 —— 忘了的话，用户下次看到的是一个没有光标的桌面，而这条测试
  # 自己却是绿的。
  [Windows.Forms.Cursor]::Hide()
  [Windows.Forms.Application]::DoEvents()

  # **再把 TopMost 顶一次。** 任务栏自己也是 topmost，谁后激活谁在上面；实测
  # 有一次它盖在了画布上方（左下角整整一条 1680x96 进了画面，而那条正好压在
  # 右下角的水印上）。`$form.TopMost = $true` 只在 Show 之前设过一次，中途任何
  # 一次抢焦点都会把它顶下去。SetWindowPos 是无副作用的，重复调没有代价 ——
  # 而"任务栏压在画布上"这件事光看探针输出会以为是水印画错了位置。
  [D2O.Win]::SetWindowPos($form.Handle, [IntPtr](-1), 0, 0, 0, 0, 0x0003) | Out-Null
  [Windows.Forms.Application]::DoEvents()
  Start-Sleep -Milliseconds 300
  [Windows.Forms.Application]::DoEvents()

  # 再从后台每 500 ms 顶一次（见上面 D2O.KeepTop 的注释）。**必须等窗口真的
  # 有句柄之后再起** —— `$form.Show()` 之后 Handle 才有效，在那之前拿到的是
  # 一个会被重建的临时句柄，线程顶的是一个不存在的窗口。
  [D2O.KeepTop]::Start($form.Handle, 500)

  Write-Host "[harness] 画布 $($bounds.Width)x$($bounds.Height)，上半 RGB($Dark)、下半 RGB($Light)"
  Write-Host "[harness] 指针钉在 $parkX,$parkY 并已隐藏（探针 --cursor 传 $parkX,$parkY 只作对账）"
  Write-Host "[harness] 开始录制 $Seconds 秒 -> $Out"

  # 自己用 ProcessStartInfo 起进程，不走 Start-Process：PS 5.1 下带
  # -RedirectStandardOutput 的 Start-Process，返回对象上的 ExitCode 永远是空的
  # （陷阱 57）。一个永远报不出失败的成功报告比没有报告更糟。
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName               = $exePath
  # `--quiet` 关掉逐秒日志；两条管道仍然要读（下面 ReadToEndAsync），
  # 否则子进程往一个没人读的管道里写满 4 KB 就会阻塞。
  $psi.Arguments              = '--record "{0}" --config "{1}" --seconds {2} --quiet' -f $outPath, $cfgPath, $Seconds
  $psi.UseShellExecute        = $false
  $psi.CreateNoWindow         = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError  = $true
  # **不要靠控制台代码页**（陷阱 41）：ProcessStartInfo 默认按 Console.OutputEncoding
  # 解码子进程输出，而那是 936，于是日志里的中文全成乱码而文件"看着正常"。
  $utf8 = New-Object Text.UTF8Encoding $false
  $psi.StandardOutputEncoding = $utf8
  $psi.StandardErrorEncoding  = $utf8

  $proc = New-Object Diagnostics.Process
  $proc.StartInfo = $psi
  if (-not $proc.Start()) { throw "起不来：$exePath" }

  $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
  $stderrTask = $proc.StandardError.ReadToEndAsync()

  if (-not $proc.WaitForExit(($Seconds + 20) * 1000)) {
    Write-Warning "[harness] 录制进程 $($Seconds + 20) 秒还没退出，强制结束 —— MP4 索引可能不完整"
    $proc.Kill()
    $proc.WaitForExit(5000) | Out-Null
  }
  $proc.WaitForExit()
  [IO.File]::WriteAllText($log, $stdoutTask.Result)
  [IO.File]::WriteAllText($errPath, $stderrTask.Result)

  $code = $proc.ExitCode
  if ($code -ne 0) { throw "录制进程退出码 $code —— 视频大概是不完整的，看 $errPath" }
  Write-Host "[harness] 退出码 $code，视频写到 $Out，配置 $cfgPath"
}
finally {
  # 顶窗线程要在关窗**之前**停掉：它拿着句柄，窗口一销毁那个句柄就可能被复用，
  # 那时它顶的是别人的窗口（而且每 500 ms 顶一次，症状是"桌面时不时闪一下"，
  # 完全指不到这里）。
  try { [D2O.KeepTop]::Stop() } catch { }
  # 指针和画布一样，必须无条件还回来 —— 中途抛异常时把用户留在一个没有光标的
  # 桌面上，比测试失败严重得多。
  try { [Windows.Forms.Cursor]::Show() } catch { }
  # 画布必须无条件收掉：中途抛异常时把桌面留在一整块双色上，比测试失败严重得多。
  if ($proc) {
    try {
      if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null }
    } catch { }
  }
  $form.Close()
  $form.Dispose()
  $bmp.Dispose()
}
