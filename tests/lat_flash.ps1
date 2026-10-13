#Requires -Version 5.1
<#
  延时探针（tests\browser\latency_probe.mjs）的**受控刺激源**。

  它只做一件事：在**主屏正中**放一个方块，按一张固定的时刻表在黑白之间翻面，
  并把每一次翻面的**墙钟毫秒**记到日志里。探针拿它和 video 像素里读到的那次
  变化对账，得到的就是端到端真值 —— 不依赖界面上任何一个自报数。

  为什么必须是**纯色画布**（陷阱 82 的纪律）：真实桌面在 ultrafast 档下的压缩
  振铃幅度能盖过要测的差异，`cursor_harness.ps1` 当初就是被这件事逼成纯色画布的
  （实测容差 8 → 28.8 万像素不同，只有 1.3% 落在指针周围）。这里取的是同一个
  手法，只是把「鼠标走固定路径」换成「方块按时翻面」。

  为什么时刻表是**不等距而且周期很长**的：探针要靠它把视频里观察到的翻面序列和
  日志里的序列对齐。等距的话，差整整 k 个周期的对齐给出完全一样的残差，于是
  「量出来的延迟」会随对齐方式跳一个周期 —— 而它看起来只是个偏大或偏小的正常数。
  短周期的不等距只解决一半：**任何周期为 P 的时刻表，错 P 个翻面都给出完全相同的
  残差**（实测：周期 4 时 k0=0 与 k0=4 的残差 sd 都是 103ms，而真值 k0=6 那一组
  是 19ms —— 光看 sd 分不出来，因为 k0=0 那一组也是"完美拟合"，只是平移了 2 秒）。
  所以周期取 **11**，而一次测量里探针最多只看得到 20 来个翻面 ⇒ 那个"平移一整个
  周期"的孪生解**根本落不进搜索范围**，残差最小就唯一对应真值。

  方块取 min(宽,高)/4（2880x1920 上是 480x480）：够大到探针把它缩到 320x160
  之后仍有 50x40 像素可采，又小到不至于在每个翻面上撞爆编码器码率上限
  （整屏黑白闪会把「优先画质」那一档逼进自适应降载，那就不是在量档位了）。
#>
param(
  [int]    $Seconds = 30,
  [string] $Log = '',
  [string] $Ready = '',
  [double] $SquareFrac = 0.25
)

$ErrorActionPreference = 'Stop'

if (-not $Log) { throw '必须给 -Log（翻面时刻表的输出路径）' }

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# **必须在创建任何窗口之前**声明 DPI 感知。见 tests\cursor_harness.ps1 里那段：
# 不声明的话 Screen.Bounds 报的是逻辑尺寸（本机 1440x960），而 DXGI 抓到的是
# 物理帧缓冲（2880x1920）—— 方块会跑到左上四分之一去，而探针在正中采到的
# 是背景色，一个翻面都看不到。
Add-Type -Namespace D2O -Name Dpi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
'@
[D2O.Dpi]::SetProcessDPIAware() | Out-Null

# 1 ms 的定时器精度。默认 15.6 ms 的调度粒度会让翻面时刻抖 ±15 ms，而这里要
# 量的东西在百毫秒量级 —— 抖动量级和被测差异同阶就等于没量。用完在 finally 里还原。
Add-Type -Namespace D2O -Name Timer -MemberDefinition @'
[DllImport("winmm.dll")] public static extern uint timeBeginPeriod(uint p);
[DllImport("winmm.dll")] public static extern uint timeEndPeriod(uint p);
'@
[D2O.Timer]::timeBeginPeriod(1) | Out-Null

$bounds = [Windows.Forms.Screen]::PrimaryScreen.Bounds

$form = New-Object Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.ShowInTaskbar   = $false
$form.TopMost         = $true
$form.StartPosition   = 'Manual'
$form.BackColor       = [Drawing.Color]::FromArgb(96, 96, 96)
$form.Bounds          = $bounds

$side = [int]([Math]::Min($bounds.Width, $bounds.Height) * $SquareFrac)
$panel = New-Object Windows.Forms.Panel
$panel.BackColor = [Drawing.Color]::Black
$panel.Bounds = New-Object Drawing.Rectangle(
  [int](($bounds.Width - $side) / 2), [int](($bounds.Height - $side) / 2), $side, $side)
$form.Controls.Add($panel)

$form.Show()
[Windows.Forms.Application]::DoEvents()

# 让首次绘制（窗口出现、背景铺满、方块画上）落在时刻表之外。不做这一步的话，
# 第一次翻面会和"窗口刚出来"撞在一起，那一次的观测时刻没有意义。
Start-Sleep -Milliseconds 400
[Windows.Forms.Application]::DoEvents()

# 不等距、长周期时刻表（周期 11 个间隔，合计 5500 ms，均值 500 ms）。见文件头。
$intervals = @(330, 590, 280, 700, 450, 620, 310, 760, 400, 520, 540)

[IO.File]::WriteAllText($Log, '')
if ($Ready -and (Test-Path -LiteralPath $Ready)) { [IO.File]::Delete($Ready) }

$sw      = [Diagnostics.Stopwatch]::StartNew()
$flips   = 0
$state   = 'b'
$slot    = 0
$nextMs  = $intervals[0]
$endMs   = $Seconds * 1000

try {
  while ($sw.ElapsedMilliseconds -lt $endMs) {
    $now = $sw.ElapsedMilliseconds
    if ($now -ge $nextMs) {
      if ($state -eq 'b') { $state = 'w' } else { $state = 'b' }
      if ($state -eq 'b') { $panel.BackColor = [Drawing.Color]::Black }
      else                { $panel.BackColor = [Drawing.Color]::White }
      # Refresh() = Invalidate + Update：同步发 WM_PAINT，把这一面真的画到
      # 窗口上。只设 BackColor 不 Refresh 的话，重绘要等下一次消息泵空闲，
      # 而那把"什么时候变的"这一项从"我们定的"变成"系统定的"。
      $panel.Refresh()
      [Windows.Forms.Application]::DoEvents()

      $ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
      $flips++
      [IO.File]::AppendAllText($Log, "$flips $ms $state" + [char]13 + [char]10)
      if ($Ready -and $flips -eq 1) { [IO.File]::WriteAllText($Ready, "$ms") }

      $slot   = ($slot + 1) % $intervals.Count
      $nextMs = $nextMs + $intervals[$slot]
    }
    [Windows.Forms.Application]::DoEvents()
    [Threading.Thread]::Sleep(1)
  }
  Write-Host "翻面 $flips 次，时刻表在 $Log"
}
finally {
  $form.Close()
  $form.Dispose()
  [D2O.Timer]::timeEndPeriod(1) | Out-Null
}

exit 0
