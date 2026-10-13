#Requires -Version 5.1
<#
.SYNOPSIS
  证明水印**真的动了**：把时间项从左上角搬到画面中间、再放大字号，断言
  新位置出现了文字、**老位置一个像素都没有**。

.DESCRIPTION
  这一条填的是 osd_check.cpp / osd_harness.ps1 那一对留下的空档。

  `tests\osd_check.cpp` 是进程内的：它验 compose() 往哪几个像素写了字，
  但它验的是"我以为传进去的那个配置"—— 配置从 config.json 读出来、经过
  `validate_config` 夹取、再走到采集线程的那一段，它一个字都没碰。
  `tests\osd_harness.ps1` + `osd_probe.py` 那一对是端到端的，但它只量
  **两个角**（默认那两项的落点）。也就是说"位置改了以后画面对不对"这件事，
  在两边都恰好落在缝里：进程内那条看不见配置层，端到端那条看不见非默认位置。

  所以这个脚本跑三趟，判据逐层加严：

  [0] **先量尺子**：`osd_probe.py --selftest`。它拿合成帧喂给**同一个**
      `analyse()`/`judge()`，断言九种情形该红的红了、该绿的绿了。
      没有这一步，下面三条断言在别人的改动里一起失效了也没人知道 ——
      这正是陷阱 82/85 的纪律（一个用来量东西的工具，本身要先被量一遍；
      怎么都绿的断言等于没验）。

  [1] **既有那一对仍然绿**：默认两项（时间@左上、名称@右下）照旧。参数化
      不许把原来的语义改坏 —— 这一条是 [2][3] 的前提。

  [2] **搬到中间**：时间项改成 `position=custom` + `x_pct/y_pct`，探针在
      `(w*x/100, h*y/100)` 处开窗口断言**那里有字**，同时在左上角开一个窗口
      断言**一个墨迹像素都没有**。

      后半句才是"搬走了"这件事的判据，而且是**必须**有的一句：只验"新位置
      有字"的话，一个"两边都画"的实现（比如旧位置那条路没接上 new config）
      照样全绿 —— 而那恰好是这次改动最可能出的错。

  [3] **同一个位置、字号放大**：除了断言位置没被字号带偏，还跨两次录制比
      墨迹高度（探针单跑一次看不见另一趟的数，所以让它 `--dump-json`，
      由这里对账）。**只验"字号改了配置里那个数变了"是不够的**：字号从
      `font_px()` 走到 GDI 的 `CreateFontW(-px, ...)`，中间任何一处漏传，
      配置里那个数照样是对的。

      "位置没被字号带偏"这一条**不能拿墨迹左上角直接比**：锚点是**掩码**的
      左上角，而墨迹从掩码内留边处才开始，那个内移量**与字号成正比**。判据
      是「内移 ÷ 墨迹高」两次录制保持在容差内 —— 推导与实际读数写在 [4] 那段。

  屏幕上别的窗口压在测量窗口上时（实测两次：开始菜单、Alt-Tab 切换器），
  探针会给**退出码 2**，也就是"这一跑判不了"。那**不是跳过**：这里会把整趟
  **重录**，试满三次仍然判不了才报 SKIP，并把那时候压在窗口上的那个东西的
  包围盒打出来。把它当通过会让这条测试永远不红，把它当失败又会把别人的窗口
  记成产品的错。

  全程**不驱动鼠标**：画布静止 ⇒ 画布自己不再产出新画面，于是 4 秒那一帧
  只可能是"水印文字变了"逼出来的。这不是充分条件（指针自己也会产出帧），
  所以 [5] 还要去读日志里那行 `水印刷新 N 帧` —— 它是这条路径的**直接证据**，
  而探针只能证明"帧确实在变"。

.PARAMETER X
.PARAMETER Y
  水印锚点在画面上的百分比（就是 `osd.items[n].x_pct/y_pct`）。默认 55,60 ——
  画面正中间偏右下。挑这两个数有三条约束，改之前先看一眼：

    * 整块窗口要落在**亮底**上（分割线在 50%）：锚点 60% 处窗口是
      y ∈ [1152-40, 1152+320]，全在分割线以下。**横跨两种底色的话，
      "这里该是白字还是黑字"就没有答案**，探针会明说它判不了。
    * 放大到 96 px 之后**文字不能顶出画面右边缘**：日期+时间在 96 px 下量到
      ~965 px 宽，55% 处是 1584，加起来 2549 < 2880，还够。
    * 锚点要**离左上角那个空窗口足够远**（老位置），否则两个字都落在同一块里，
      "老位置是空的"这条就恒假。

.PARAMETER BigFont
  [3] 那一趟用的字号。0 = 自动（帧高/32，本机 1920 高 ⇒ 60 px）。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tests\osd_move_check.ps1
#>
param(
  [string]$Exe     = 'build\bin\desk2onvif.exe',
  [int]   $Seconds = 6,
  [double]$X       = 55,
  [double]$Y       = 60,
  [int]   $BigFont = 96
)

$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$harness = Join-Path $root 'tests\osd_harness.ps1'
$probe   = Join-Path $root 'tests\osd_probe.py'
$workDir = Join-Path $root 'out\osd-move'
if (-not (Test-Path $workDir)) { New-Item -ItemType Directory -Force $workDir | Out-Null }

# 画布颜色必须和 osd_harness.ps1 的默认值一致：判据（"墨迹比底色亮/暗多少"）
# 就是拿它们当基准的，两边写不一样的话探针量的是另一块画布。
$Dark  = '20,20,24'
$Light = '240,240,236'
$Split = '0.5'

$script:passed = 0
$script:failed = 0
$script:skipped = 0

function Ok([string]$what, [string]$detail = '') {
  $script:passed++
  if ($detail) { Write-Host ("  [ OK ] {0} —— {1}" -f $what, $detail) }
  else         { Write-Host ("  [ OK ] {0}" -f $what) }
}
function Bad([string]$what, [string]$detail = '') {
  $script:failed++
  if ($detail) { Write-Host ("  [FAIL] {0} —— {1}" -f $what, $detail) -ForegroundColor Red }
  else         { Write-Host ("  [FAIL] {0}" -f $what) -ForegroundColor Red }
}
# 前提没成立就不判 —— 但要**说出来**。默默跳过和默默通过长得一模一样
# （陷阱 87：判据依赖了没写进前提的环境条件时，它只能报告、不能判）。
function Skip([string]$what, [string]$why) {
  $script:skipped++
  Write-Host ("  [SKIP] {0} —— {1}" -f $what, $why) -ForegroundColor Yellow
}

# 命令行参数按 **MSVCRT 的规则**拼，不能简单地给每个参数套一对引号。
# 这里有一个真的会踩到的坑：`-Items` 传的是一段 JSON，**里面全是双引号**，
# 而它自己还要被包在外层引号里。外层引号里出现的 `"` 必须写成 `\"`，否则
# 子进程看到的是"引号提前闭合、后面那半截变成独立参数" —— 症状是
# osd_harness.ps1 报一个参数绑定错误，或者更坏：JSON 被截断成合法但错误
# 的片段，录出来的画面里水印出现在默认位置，而探针那边报的是"新位置没字"。
# 反斜杠只在**引号前面**才需要加倍，所以不能无脑 replace。
function Quote-Arg([string]$s) {
  $sb = New-Object Text.StringBuilder
  [void]$sb.Append('"')
  $bs = 0
  foreach ($ch in $s.ToCharArray()) {
    if ($ch -eq '\') { $bs++; continue }
    if ($ch -eq '"') {
      [void]$sb.Append('\' * ($bs * 2 + 1)); [void]$sb.Append('"'); $bs = 0; continue
    }
    if ($bs) { [void]$sb.Append('\' * $bs); $bs = 0 }
    [void]$sb.Append($ch)
  }
  if ($bs) { [void]$sb.Append('\' * ($bs * 2)) }
  [void]$sb.Append('"')
  return $sb.ToString()
}

# 自己起进程，不走 Start-Process（陷阱 57：带 -RedirectStandardOutput 时
# ExitCode 恒为空），也不走 `2>&1`（PS 5.1 会把原生命令的 stderr 包成
# ErrorRecord，在 $ErrorActionPreference='Stop' 下当场变成终止错误）。
# 两条管道的编码都显式设成 UTF-8（陷阱 41：默认按 936 解码，中文全成乱码）。
function Invoke-Py {
  param([string[]]$PyArgs, [int]$TimeoutMs = 600000)
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName        = $PyArgs[0]
  $psi.Arguments       = (($PyArgs | Select-Object -Skip 1) | ForEach-Object { Quote-Arg $_ }) -join ' '
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow  = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError  = $true
  $utf8 = New-Object Text.UTF8Encoding $false
  $psi.StandardOutputEncoding = $utf8
  $psi.StandardErrorEncoding  = $utf8

  $p = New-Object Diagnostics.Process
  $p.StartInfo = $psi
  if (-not $p.Start()) { throw "起不来：python $($psi.Arguments)" }
  $o = $p.StandardOutput.ReadToEndAsync()
  $e = $p.StandardError.ReadToEndAsync()
  if (-not $p.WaitForExit($TimeoutMs)) {
    $p.Kill(); $p.WaitForExit(5000) | Out-Null
    return @{ Code = -1; Out = ''; Err = "超时 $TimeoutMs ms：$($psi.Arguments)" }
  }
  $p.WaitForExit()
  return @{ Code = $p.ExitCode; Out = $o.Result; Err = $e.Result }
}

function Invoke-Probe {
  param([string]$Video, [string]$DumpJson, [string[]]$Extra)
  $a = @('python', '-I', '-X', 'utf8', $probe, $Video,
         '--dark', $Dark, '--light', $Light, '--split', $Split,
         '--at', '1.0', '--at', '4.0')
  if ($DumpJson) { $a += @('--dump-json', $DumpJson) }
  if ($Extra)    { $a += $Extra }
  return Invoke-Py -PyArgs $a
}

# 录制一趟。**`-Items` 走命令行参数**，不在这里拼配置文件 —— 拼配置文件的话
# 这段代码就成了第二个"配置是这么写的"的出处，而它和 osd_harness.ps1 里的那份
# 迟早不一致（陷阱 28 的同一族：结构里有值、没有代码读它）。
function Invoke-Take {
  param([string]$Name, [string]$Items)
  $video = Join-Path $workDir "$Name.mp4"
  $a = @('-ExecutionPolicy', 'Bypass', '-File', $harness,
         '-Out', "out\osd-move\$Name.mp4", '-Seconds', "$Seconds", '-Exe', $Exe)
  if ($Items) { $a += @('-Items', $Items) }
  $r = Invoke-Py -PyArgs (@('powershell') + $a) -TimeoutMs (($Seconds + 60) * 1000)
  if ($r.Code -ne 0) {
    Write-Host $r.Out
    Write-Host $r.Err
    throw "录制 $Name 失败（退出码 $($r.Code)）"
  }
  if (-not (Test-Path $video)) { throw "录制 $Name 说成功了，但 $video 不存在" }
  return $video
}

function Show-Probe([hashtable]$r, [int]$Tail = 40) {
  $lines = @($r.Out -split "`r?`n")
  foreach ($l in $lines) { Write-Host "    $l" }
  if ($r.Err) { foreach ($l in @($r.Err -split "`r?`n")) { Write-Host "    $l" -ForegroundColor DarkGray } }
}

# 录一趟 + 量一趟；探针退出码 **2 = "这一跑判不了"**（屏幕上别的窗口压住了
# 测量窗口，探针不肯把它算成通过，见 osd_probe.py 里那段）时就**整趟重录**。
#
# 为什么不直接把 2 当成跳过：压住这件事是**当时**发生的（实测两次分别是 Windows
# 的开始菜单和 Alt-Tab 的切换器，都是人手碰出来的），重录一次的代价只有几秒；
# 而「判不了」一旦被当成「过了」，这条测试就永远不会红 —— 那正是陷阱 85 说的
# 那种"怎么都绿的断言"。试满 3 次仍然判不了才报 SKIP，并把那时压在窗口上的
# 那个东西的包围盒打出来：到那一步它已经是一条能拿去查的线索，而不是噪声。
function Invoke-Round {
  param([string]$Name, [string]$Items, [string[]]$Extra)
  $json = Join-Path $workDir "$Name.json"
  $last = $null
  for ($attempt = 1; $attempt -le 3; $attempt++) {
    $v = Invoke-Take -Name $Name -Items $Items
    $r = Invoke-Probe -Video $v -DumpJson $json -Extra $Extra
    $last = @{ Code = $r.Code; Out = $r.Out; Err = $r.Err; Video = $v; Attempts = $attempt }
    if ($r.Code -ne 2) { return $last }
    if ($attempt -lt 3) {
      Write-Host ("    （第 {0} 次判不了：{1}，重录一次）" -f $attempt, (Covered-Desc $json))
    }
  }
  return $last
}

# 把 `--dump-json` 里记的"别的窗口压住了谁"翻译成一行人话。判不了的时候没有
# 它，报告上就只剩一句"无法判定"，而那句话查不动任何东西。
function Covered-Desc([string]$jsonPath) {
  if (-not (Test-Path $jsonPath)) { return '（没有 --dump-json，看不到是什么压住了）' }
  $j = [IO.File]::ReadAllText($jsonPath) | ConvertFrom-Json
  $parts = @()
  foreach ($p in $j.samples.PSObject.Properties) {
    $s = $p.Value
    if ($null -eq $s.foreign_bbox) { continue }
    $b = @($s.foreign_bbox)
    $parts += ("t={0}s 被 {1}x{2} @({3},{4}) 压住（{5}）" -f
               $p.Name,
               ([int]$b[2] - [int]$b[0] + 1), ([int]$b[3] - [int]$b[1] + 1),
               [int]$b[0], [int]$b[1], ($s.covered -join '、'))
  }
  if (-not $parts) { return '（JSON 里没有外来包围盒 —— 判不了的原因不在那儿）' }
  return ($parts -join '；')
}

# 从探针的输出里读回帧尺寸。要它是因为 [4] 那一条要**算出锚点原点的坐标**
# （配置里的 x_pct/y_pct 是乘 dst_w/dst_h 得来的），而桌面尺寸是随机器变的，
# 不能在这儿写死 2880x1920。窄正则、读不到就返回 $null —— 让调用方去分辨
# "没读到"和"读到了 0"（陷阱 63）。
function Read-FrameSize([string]$probeOut) {
  $m = [regex]::Match($probeOut, '视频\s+\S+\s+(\d+)x(\d+)')
  if (-not $m.Success) { return $null }
  return @{ w = [int]$m.Groups[1].Value; h = [int]$m.Groups[2].Value }
}

# 从 SessionStats::summary() 的输出里抠三个数。窄正则、只认这一种形状：
# 抠不出来就返回 $null，让调用方去判"没读到"和"读到 0"是两回事
# （陷阱 63：一把量不出东西的尺子，报的会是别人的错）。
function Read-Summary([string]$logPath) {
  if (-not (Test-Path $logPath)) { return $null }
  $t = [IO.File]::ReadAllText($logPath)
  $out = @{ content = $null; pointer_only = $null; osd_refresh = $null }
  $m = [regex]::Match($t, '画面变化\s+(\d+)\s+次\s*/\s*共\s+(\d+)\s+帧')
  if ($m.Success) { $out.content = [int]$m.Groups[1].Value }
  $m = [regex]::Match($t, '其中\s+(\d+)\s+帧只因为鼠标动了才产出')
  if ($m.Success) { $out.pointer_only = [int]$m.Groups[1].Value }
  $m = [regex]::Match($t, '水印刷新\s+(\d+)\s+帧')
  if ($m.Success) { $out.osd_refresh = [int]$m.Groups[1].Value }
  return $out
}

function Ink-Box([string]$jsonPath) {
  if (-not (Test-Path $jsonPath)) { return $null }
  $j = [IO.File]::ReadAllText($jsonPath) | ConvertFrom-Json
  # 采样点的键是 `str(t)`，也就是 "1.0" 这种**带点的属性名**。`$j.samples.'1.0'`
  # 在 PS 里能写，但 `.1.0` 这种形状一旦被谁改成 `$j.samples.1.0` 就变成
  # "取 samples 的第 1 个元素的 .0 属性"，报的错指不到这里。按名字查属性更稳，
  # 而且查不到时能老实返回 $null（调用方判"没量到"和"量到 0"是两回事）。
  $s = $j.samples.PSObject.Properties | Where-Object { $_.Name -eq '1.0' } |
       Select-Object -First 1 -ExpandProperty Value
  if (-not $s) { return $null }
  $r = $s.regions.PSObject.Properties | Where-Object { $_.Name -eq 'ink0' } |
       Select-Object -First 1 -ExpandProperty Value
  if (-not $r) { return $null }
  $b = @($r.bbox)
  return @{ w = [int]$b[2] - [int]$b[0] + 1; h = [int]$b[3] - [int]$b[1] + 1;
            x = [int]$b[0]; y = [int]$b[1] }
}

Write-Host "== OSD 搬位置 / 改字号：端到端 =="
Write-Host "水印锚点 $X%,$Y%，放大字号 $BigFont，画布 暗 RGB($Dark) / 亮 RGB($Light)"
Write-Host ""

# --- [0] 量具自校 ---------------------------------------------------------
Write-Host "[0] 量具自校（合成图，不碰桌面）"
$st = Invoke-Py -PyArgs @('python', '-I', '-X', 'utf8', $probe, '--selftest')
if ($st.Code -eq 0) {
  Ok "探针在合成图上的刻度对得上" ($st.Out -split "`r?`n" | Where-Object { $_ -match '该红的红了' })
} else {
  Bad "探针自己的刻度就不对" "退出码 $($st.Code)"
  Show-Probe $st
  Write-Host ""
  Write-Host "[FAIL] 量具没量准之前，下面几条绿了也不作数。"
  exit 1
}

# --- [1] 默认两项仍然绿（参数化没改坏原有语义）----------------------------
Write-Host ""
Write-Host "[1] 默认两项（时间@左上、名称@右下）—— 参数化不许改坏原来的用法"
$rA = Invoke-Round -Name 'A-default' -Items '' -Extra $null
$vA = $rA.Video
Show-Probe $rA
if ($rA.Code -eq 0) {
  Ok "默认那一对的判据照旧全过（新位置那一套没有污染默认路径）"
} elseif ($rA.Code -eq 2) {
  Skip "默认那一对的判据" ("重录了 {0} 次都判不了：{1}" -f $rA.Attempts, (Covered-Desc (Join-Path $workDir 'A.json')))
} else {
  Bad "默认那一对红了" "退出码 $($rA.Code) —— 参数化改坏了既有语义"
}

# --- [2] 搬到自定义位置 ---------------------------------------------------
Write-Host ""
Write-Host "[2] 时间项搬到 $X%,$Y%（自动字号）—— 新位置要有字，老位置要空"
$itemsB = '[{"enabled":true,"type":"time","position":"custom","x_pct":' + [int]$X +
          ',"y_pct":' + [int]$Y + ',"font_size":0}]'
$rB = Invoke-Round -Name 'B-moved' -Items $itemsB `
                   -Extra @('--ink-pct', "$([int]$X),$([int]$Y)", '--blank-corner', 'tl')
$vB = $rB.Video
Show-Probe $rB
if ($rB.Code -eq 0) {
  Ok "新位置有字、且老位置（左上角）一个墨迹像素都没有"
} elseif ($rB.Code -eq 2) {
  Skip "搬到 $X%,$Y% 之后的判据" ("重录了 {0} 次都判不了：{1}" -f $rB.Attempts, (Covered-Desc (Join-Path $workDir 'B.json')))
} else {
  Bad "搬到 $X%,$Y% 之后的判据红了" "退出码 $($rB.Code)"
}

# --- [3] 同一个位置、字号放大 --------------------------------------------
Write-Host ""
Write-Host "[3] 同一位置、字号 $BigFont —— 位置不许被字号带偏"
$itemsC = '[{"enabled":true,"type":"time","position":"custom","x_pct":' + [int]$X +
          ',"y_pct":' + [int]$Y + ',"font_size":' + $BigFont + '}]'
$rC = Invoke-Round -Name 'C-bigfont' -Items $itemsC `
                   -Extra @('--ink-pct', "$([int]$X),$([int]$Y)", '--blank-corner', 'tl')
$vC = $rC.Video
Show-Probe $rC
if ($rC.Code -eq 0) {
  Ok "字号放大之后锚点仍然对得上、老位置仍然空着"
} elseif ($rC.Code -eq 2) {
  Skip "字号 $BigFont 那一趟" ("重录了 {0} 次都判不了：{1}" -f $rC.Attempts, (Covered-Desc (Join-Path $workDir 'C.json')))
} else {
  Bad "字号 $BigFont 那一趟红了" "退出码 $($rC.Code)"
}

# --- [4] 字号真的变大了（跨两次录制，单跑一次的探针看不见）----------------
Write-Host ""
Write-Host "[4] 两次录制的墨迹尺寸对账（字号 0=自动 vs $BigFont）"
$boxB = Ink-Box (Join-Path $workDir 'B.json')
$boxC = Ink-Box (Join-Path $workDir 'C.json')
$size  = Read-FrameSize $rC.Out
if ($rB.Code -eq 2 -or $rC.Code -eq 2) {
  Skip "字号对账" "B 或 C 那一趟判不了（屏幕上别的东西压住了测量窗口），没有可对账的数"
} elseif (-not $boxB) {
  Skip "字号对账" "B 那一趟没有量到 ink0 —— 上一段已经报了，这里不重复计一次"
} elseif (-not $boxC) {
  Skip "字号对账" "C 那一趟没有量到 ink0 —— 上一段已经报了"
} elseif (-not $size) {
  Skip "字号对账" "探针输出里读不到帧尺寸 —— 算不出锚点原点的坐标，这条量不了"
} else {
  Write-Host ("    自动字号：墨迹 {0}x{1} @({2},{3})" -f $boxB.w, $boxB.h, $boxB.x, $boxB.y)
  Write-Host ("    字号 {0}：墨迹 {1}x{2} @({3},{4})" -f $BigFont, $boxC.w, $boxC.h, $boxC.x, $boxC.y)
  # 判**差值**而不是比例：比例会被抗锯齿和编码的模糊带偏（两边各差一两像素，
  # 小字号那一侧的比例误差就放大一倍）。要求高度至少高出 15 像素 —— 60→96
  # 的期望差是 20 多像素，15 这个门限仍能把"字号根本没生效"照出来。
  if ($boxC.h - $boxB.h -ge 15) { Ok "字号真的落到了画面像素上" "高 $($boxB.h) → $($boxC.h)" }
  else { Bad "字号没生效" "墨迹高 $($boxB.h) → $($boxC.h)，差不出一格字号" }
  if ($boxC.w - $boxB.w -ge 60) { Ok "宽度也跟着放大了" "宽 $($boxB.w) → $($boxC.w)" }
  else { Bad "宽度没跟着放大" "宽 $($boxB.w) → $($boxC.w)" }

  # 位置**不能**被字号带偏 —— 但**墨迹左上角本身会随字号走**，所以不能拿它
  # 直接比。锚点是**掩码**的左上角（`compose()` 把掩码尺寸交给 `anchor()`），
  # 而墨迹从掩码内 `kHaloPad = 2` 处 + 字形的侧边距才开始 —— 那个内移量**与
  # 字号成正比**。实测（2880x1920，锚点 55%,60% = 1584,1152）：
  #   自动字号（60 px）→ 墨迹 602x48 @(1589,1169)，内移 (5,17)
  #   字号 96          → 墨迹 948x75 @(1592,1179)，内移 (8,27)
  # 内移量差了一倍，而**内移 ÷ 墨迹高**两边几乎一样：(5/48, 17/48) = (0.104, 0.354)
  # 对 (8/75, 27/75) = (0.107, 0.360)。也就是说"锚点一动没动，只是字形内移跟着
  # 字号放大了"。**第一版这条断言是错的**：它写的是"墨迹左上角相差不超过 3 像素"，
  # 于是把一次完全正确的放大报成了"放大字号把锚点挪走了"。
  #
  # 容差 0.06 ≈ 在 75 像素高的墨迹上允许 4.5 像素 —— 上面那两个比值只差 0.006，
  # 而"锚点真被挪走了"（比如被字号带偏 50 像素）对应 0.67，两档之间空得很开。
  $ax = [Math]::Floor($size.w * $X / 100.0)
  $ay = [Math]::Floor($size.h * $Y / 100.0)
  $rxB = ($boxB.x - $ax) / [double]$boxB.h;  $ryB = ($boxB.y - $ay) / [double]$boxB.h
  $rxC = ($boxC.x - $ax) / [double]$boxC.h;  $ryC = ($boxC.y - $ay) / [double]$boxC.h
  Write-Host ("    锚点原点 ({0},{1})；内移 ÷ 墨迹高  自动字号 ({2:N3},{3:N3}) / 字号 {4} ({5:N3},{6:N3})" -f
              $ax, $ay, $rxB, $ryB, $BigFont, $rxC, $ryC)
  $tol = 0.06
  if ([Math]::Abs($rxC - $rxB) -le $tol -and [Math]::Abs($ryC - $ryB) -le $tol) {
    Ok "放大字号没有把锚点挪走" ("内移 ÷ 墨迹高 从 ({0:N3},{1:N3}) 到 ({2:N3},{3:N3})，差在 {4} 以内" -f $rxB, $ryB, $rxC, $ryC, $tol)
  } else {
    Bad "放大字号把锚点挪走了" ("内移 ÷ 墨迹高 ({0:N3},{1:N3}) → ({2:N3},{3:N3})，差超过 {4}" -f $rxB, $ryB, $rxC, $ryC, $tol)
  }
}

# --- [5] 静止桌面那条路的直接证据 ----------------------------------------
Write-Host ""
Write-Host "[5] 静止桌面上水印自己产出帧的直接证据（日志里的 `水印刷新` 计数）"
foreach ($pair in @(@('B-moved', $vB), @('C-bigfont', $vC), @('A-default', $vA))) {
  $name = $pair[0]
  $log  = [IO.Path]::ChangeExtension($pair[1], '.log')
  $s    = Read-Summary $log
  if ($null -eq $s -or $null -eq $s.content) {
    Bad "$name 的收尾统计读不出来" "日志里没有『画面变化 N 次 / 共 M 帧』那一行（$log）—— 尺子坏了，不是产品坏了"
    continue
  }
  $desc = "画面变化 $($s.content) 次，鼠标单独产出 $($s.pointer_only) 帧"
  if ($null -eq $s.osd_refresh) { $desc += "，水印刷新 0 帧" }
  else { $desc += "，水印刷新 $($s.osd_refresh) 帧" }
  Write-Host "    $name ：$desc"

  # **顺序是有讲究的：先看直接证据，再看前提。**
  #
  # `水印刷新 N 帧` 这个计数器只在 `capture_session.cpp` 的
  # `!got_content && osd_changed && last` 那个分支里累加，也就是"桌面这一拍没
  # 内容更新、只有水印的文字变了" —— 所以 **N > 0 本身就是完备的证据**，不需要
  # 任何关于桌面的前提：它已经包含了"桌面当时是静止的"这一条。
  #
  # 第一版把顺序写反了（先判 `画面变化 > 3` 就跳过），于是在一次**完全干净**的
  # 录制上跳过了 —— A 那一趟 `画面变化 4 次`（只有开头那一两下的瞬态）却
  # `水印刷新 5 帧`，判据却因为那个 4 而弃权。**前提只在计数器为 0 时才需要**：
  # 那时要分清的是"水印真的没产出帧"（失败）还是"桌面本来就在动，这个话题
  # 轮不到水印"（前提没成立，不判）。
  if ($null -ne $s.osd_refresh -and $s.osd_refresh -gt 0) {
    Ok "$name 的帧确实是水印逼出来的" ("水印刷新 $($s.osd_refresh) 帧（这条计数只在『桌面没动、只有水印变了』那个分支里累加，本身就是直接证据）")
  } elseif ($s.content -gt 3 -or $s.pointer_only -gt 0) {
    Skip "$name 的静止桌面证据" "水印刷新 0 帧，而前提没成立（画面变化 $($s.content) 次 / 鼠标单独产出 $($s.pointer_only) 帧）—— 桌面本来就在动，这一跑说明不了什么"
  } else {
    Bad "$name 在静止桌面上没有水印刷新" "画面没变（$($s.content) 次）、指针也没动，水印刷新却是 0 —— 那么 4 秒那一帧是从哪来的？"
  }
}

Write-Host ""
if ($script:skipped -gt 0) {
  Write-Host ("通过 {0} 项，失败 {1} 项，跳过 {2} 项" -f $script:passed, $script:failed, $script:skipped)
} else {
  Write-Host ("全部通过 {0} 项" -f $script:passed)
}
Write-Host "产物在 out\osd-move\（三份 mp4 + 两份 --dump-json + 日志）"

# 显式退出码（陷阱 89）：PS 只在**原生命令**退出时设 $LASTEXITCODE，脚本正常
# 跑完不会把它归零 —— 不写 exit 的话，成功那一次会带出最后一个 python 的码，
# 而假红会教人忽略真红。
if ($script:failed -gt 0) { exit 1 }
exit 0
