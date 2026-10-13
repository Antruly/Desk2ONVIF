<#
  Phase AB 验收：设备身份「固定死」的两条 —— 种子稳定 + 落盘权威。

  用户原话：「设备硬件码这个电脑正常来说应该固定死，（不）能直接在 web 页面修改，
  避免用户乱改，还有版本号啊这些」。第五轮把 `onvif` 身份那一段分成两半：

    · **锁死**（网页改不了）：`hardware_id` / `serial_number` / `device_uuid`
    · **仍可改**：`manufacturer` / `model` / `firmware_version` / `device_name` / `location`

  「网页改不了」那一条在 `tests\web_contract.py` 的 `[2b]` 段里验（前端禁用 +
  `apply_config` 无条件回灌 + 反面对照），这里不重复。本脚本验另外几条：

    [2] 首次启动就把身份**推导一次、写进 config.json**（而不是每次启动重推）
    [3] 种子只吃 hostname + MachineGuid，**不吃 MAC**
    [4] 文件里那份是权威：手改文件，程序就得听文件的
    [5] 一次**真实的保存**（POST /api/config 改 capture.fps）不会把身份改写回去

  ## 为什么 [3] 是"复算"而不是"真的拔一块网卡"

  方案原本要求「禁用一个网卡（或改 MAC）→ 重启 → 两个值都不变」。那条路在
  本会话走不通，两个原因都是硬的：

    · 当前进程**不是管理员**（`Disable-NetAdapter` / `Set-NetAdapterAdvancedProperty`
      都要提权），而 UAC 弹框没人点 —— 用户睡着了；
    · 两台 `Status=Up` 的网卡是 WLAN 与 Hyper-V 默认交换机。禁掉前者会切断用户
      自己的网络（他可能正靠它连着这台机器），禁掉后者会拆掉 WSL/Hyper-V 的网络。
      拿用户的可用性去换一条断言，不划算。

  所以 `[6]` 段做**能力探测**：`-MacChange -Adapter <名字>` 且确实提了权，就真做；
  做不到就**明说"这一条没测到"**，并把复现命令原样打出来 —— 含糊地"跳过"等于
  假装验过了。

  `[3]` 段改用**等式**：在 PowerShell 里按 `host=<机器名>;guid=<MachineGuid>`
  复算一遍 `make_serial` / `make_uuid_urn`（`src/onvif/net_identity.cpp` 那三段
  只用到 md5 和字符串拼接，照抄得出来），再和程序**真的写进 config.json** 的
  那两个值逐字比。等式两边都不是常量，所以它有判别力：

    · 把 MAC 加回种子 → 复算值 ≠ 程序值 → 红
    · 换掉 hostname 或 guid → 红
    · 每次重推而不是读文件 → [4] / [5] 红

  这比「跑两次同一个环境比一比」强的地方就在这里：后者在**没修之前的代码**下
  也是绿的（陷阱 85 的教科书形状）。而这条等式做过一次真变异检验 —— 把 MAC 那
  几行加回 `machine_seed()` 重新编译，[3] 立刻变红，红的正是 `[3] 复算的序列号`，
  证据在 `out\ab-mut\EVIDENCE.md`。

  ## 这个脚本不碰什么

  只吃源码树里编出来的 `build\bin\desk2onvif.exe` 与 `web\dist`，配置写在
  `%TEMP%` 下一个一次性目录里。不装、不卸、不碰 `%APPDATA%\Desk2ONVIF` 里
  用户那份配置（与 `verify_portable.ps1` 同一条纪律）。
#>
[CmdletBinding()]
param(
    [int]    $HttpPort = 8213,
    [int]    $RtspPort = 8574,
    [string] $Adapter  = '',      # 配合 -MacChange：要切换的那块网卡
    [switch] $MacChange,          # 需要管理员；没有就只报告"未测到"
    [switch] $Keep                # 保留临时目录（排查用）
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# 共用那套在 scripts\ 下（它不在 tests\ 里 —— 点源指错地方时报的是
# "Check 不是内部或外部命令"，看着像脚本写坏了，其实只是找错了文件）。
. (Join-Path $root 'scripts\_verify_common.ps1')

$exe  = Join-Path $root 'build\bin\desk2onvif.exe'
$dist = Join-Path $root 'web\dist'
$work = Join-Path $env:TEMP ('d2o-ident-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$cfgPath = Join-Path $work 'config.json'
$outLog  = Join-Path $work 'app.out'
$errLog  = Join-Path $work 'app.err'
$app     = $null

# 哨兵值：形状和真值一样（16 位大写十六进制 / urn:uuid 的 v4 形状），这样
# [4]/[5] 段里那些"形状对不对"的断言不会因为哨兵是假的而误判。内容是故意
# 一眼认得出的 —— 报告里出现它就说明"程序听的是文件"。
$SENT_SERIAL = 'AAAA1111BBBB2222'
$SENT_UUID   = 'urn:uuid:0badc0de-0000-4000-8000-000000000001'

# ---------------------------------------------------------------------------
# 复算工具：net_identity.cpp 里 make_serial / make_uuid_urn 的 PowerShell 译本
# ---------------------------------------------------------------------------

function Get-Md5Hex([string]$s) {
    $md5 = [System.Security.Cryptography.MD5]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($s)
        return (($md5.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '')
    } finally { $md5.Dispose() }
}

function Get-SeededSerial([string]$seed, [int]$n = 16) {
    return (Get-Md5Hex ('desk2onvif-serial:' + $seed)).Substring(0, $n).ToUpperInvariant()
}

function Get-SeededUuid([string]$seed) {
    $h = (Get-Md5Hex ('desk2onvif-uuid:' + $seed)).Substring(0, 32).ToCharArray()
    # 版本位与变体位：和 C++ 那份逐位对齐。不设这两处，复算出来的 UUID 会和
    # 程序的差两个字符，而"差两个字符"看起来像种子不同，查起来要绕一大圈。
    $h[12] = '4'
    $h[16] = '89ab'[([Convert]::ToInt32([string]$h[16], 16)) -band 3]
    $hs = -join $h
    return 'urn:uuid:' + $hs.Substring(0, 8) + '-' + $hs.Substring(8, 4) + '-' +
           $hs.Substring(12, 4) + '-' + $hs.Substring(16, 4) + '-' + $hs.Substring(20, 12)
}

# `GetComputerNameA` 取的到底是哪一个：正常机器上这三个就是同一个值，但
# 改过名的机器在重启前后会短暂不一致，所以**三个都试**，命中任意一个即可。
# 只试 `$env:COMPUTERNAME` 的话，在一台刚改过名的机器上会红，而红的理由
# 指不到"程序用的是另一个名字"这件事上。
function Get-HostnameCandidates {
    $list = New-Object System.Collections.ArrayList
    $reg = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\ComputerName\ActiveComputerName' `
                             -Name ComputerName -ErrorAction SilentlyContinue).ComputerName
    foreach ($n in @($env:COMPUTERNAME, [System.Net.Dns]::GetHostName(), $reg)) {
        if ($n -and -not $list.Contains($n)) { $null = $list.Add($n) }
    }
    return $list.ToArray()
}

function Get-MachineGuid {
    return (Get-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Cryptography' `
                             -Name MachineGuid -ErrorAction SilentlyContinue).MachineGuid
}

function Get-LocalMacs {
    $macs = New-Object System.Collections.ArrayList
    try {
        foreach ($a in (Get-NetAdapter -ErrorAction Stop)) {
            if (-not $a.MacAddress) { continue }
            $m = ($a.MacAddress -replace '[-:]', '').ToUpperInvariant()
            if ($m -and -not $macs.Contains($m)) { $null = $macs.Add($m) }
        }
    } catch { }
    return $macs.ToArray()
}

# ---------------------------------------------------------------------------
# 配置读写与起停
# ---------------------------------------------------------------------------

function Get-CfgText { return [IO.File]::ReadAllText($cfgPath) }

# 直接在**原文**上做替换，不走 ConvertFrom-Json / ConvertTo-Json 一个来回：
# 那一趟会把文件重新排版（缩进、键序、非 ASCII 的转义方式都可能变），于是
# "程序发现文件被改过、于是回写"那道闸门会被我们自己触发 —— [4] 段要问的是
# "手改的值还在不在"，而不是"回写会不会把它弄丢"，两件事混在一起就分不清了。
function Get-CfgField([string]$name) {
    if (-not (Test-Path -LiteralPath $cfgPath)) { return $null }
    $m = [regex]::Match((Get-CfgText), '"' + $name + '"\s*:\s*"([^"]*)"')
    if (-not $m.Success) { return $null }
    return $m.Groups[1].Value
}

# 改一个字段。返回是否**真的改到了** —— [4] 段后面那句"程序报的是哨兵"必须
# 由这个返回值守着：正则没命中的话 [regex]::Replace 会安静地原样返回，
# 于是"文件里是哨兵"和"我以为文件里是哨兵"在代码里长得一模一样（陷阱 91）。
function Set-CfgField([string]$name, [string]$value) {
    $raw = Get-CfgText
    $re  = '"' + $name + '"\s*:\s*"([^"]*)"'
    if (-not [regex]::IsMatch($raw, $re)) { return $false }
    $rep = '"' + $name + '": "' + ($value -replace '\$', '$$$$') + '"'
    $new = [regex]::Replace($raw, $re, $rep)
    if ($new -eq $raw) { return $false }
    [IO.File]::WriteAllText($cfgPath, $new, (New-Object Text.UTF8Encoding $false))
    return $true
}

# 数字字段的读数。`Get-CfgField` 那个正则只认**带引号的**值，拿它去读
# `"fps": 30` 会恒返回空 —— 而"空"和"字段是空的"在读数上长得一模一样，
# 于是 [5] 段那句"保存真的落盘了吗"会一直红，看着像产品没保存。
# （这不是假想：第一次跑就是这么红的。陷阱 91 的形状 —— 量具坏了，
#   红的断言指错了地方。）
function Get-CfgNum([string]$name) {
    if (-not (Test-Path -LiteralPath $cfgPath)) { return $null }
    $m = [regex]::Match((Get-CfgText), '"' + $name + '"\s*:\s*(-?[0-9]+(\.[0-9]+)?)')
    if (-not $m.Success) { return $null }
    return [double]$m.Groups[1].Value
}

function Start-IdentityRun {
    $sw = @('--rtsp', '--rtsp-port', "$RtspPort", '--http', '--http-port', "$HttpPort",
            '--config', $cfgPath, '--web-root', $dist, '--console')
    $script:app = Start-Detached $exe ($sw -join ' ') $root $outLog $errLog
    $up = Wait-Backend $HttpPort
    if (-not $up) {
        Write-Host '  后端起不来，两份日志的尾巴：' -ForegroundColor Red
        Show (Get-Content -LiteralPath $errLog -Raw -Encoding UTF8 -ErrorAction SilentlyContinue) 25
        Show (Get-Content -LiteralPath $outLog -Raw -Encoding UTF8 -ErrorAction SilentlyContinue) 25
    }
    return $up
}

function Stop-IdentityRun {
    if ($script:app) {
        # 收的是**整棵**进程树：cmd.exe → desk2onvif.exe。只 Kill 那个 cmd，
        # exe 会留下来占着端口，下一段就起不来了。
        try { & taskkill.exe /F /T /PID $script:app.Process.Id 2>&1 | Out-Null } catch { }
        try { Remove-Item -LiteralPath $script:app.CmdFile -Force -ErrorAction SilentlyContinue } catch { }
        $script:app = $null
    }
    Stop-Desk2Onvif
    Wait-PortFree @($HttpPort, $RtspPort)
    Start-Sleep -Milliseconds 400
}

function Get-ApiConfig {
    $base = "http://127.0.0.1:$HttpPort"
    $body = [Text.Encoding]::UTF8.GetBytes('{"user":"admin","password":"admin"}')
    $null = Invoke-WebRequest -Uri "$base/api/login" -Method Post -Body $body `
                -ContentType 'application/json; charset=utf-8' -TimeoutSec 15 `
                -SessionVariable sess -UseBasicParsing
    $r = Invoke-WebRequest -Uri "$base/api/config" -WebSession $sess -TimeoutSec 20 -UseBasicParsing
    return ($r.Content | ConvertFrom-Json)
}

# 把一份改过的配置 POST 回去。**必须发 UTF-8 字节**：`Invoke-WebRequest`
# 拿到 String 时按 Latin-1 编码，而这份 JSON 里有中文的设备名/位置，
# 一进一出就变成乱码存进配置 —— 而"乱码"在下一次读回来时看着像用户自己填的。
function Send-ApiConfig([object]$cfg) {
    $base = "http://127.0.0.1:$HttpPort"
    $json = $cfg | ConvertTo-Json -Depth 25 -Compress
    $null = Invoke-WebRequest -Uri "$base/api/login" -Method Post `
                -Body ([Text.Encoding]::UTF8.GetBytes('{"user":"admin","password":"admin"}')) `
                -ContentType 'application/json; charset=utf-8' -TimeoutSec 15 `
                -SessionVariable sess -UseBasicParsing
    return Invoke-WebRequest -Uri "$base/api/config" -Method Post `
                -Body ([Text.Encoding]::UTF8.GetBytes($json)) `
                -ContentType 'application/json; charset=utf-8' -WebSession $sess `
                -TimeoutSec 60 -UseBasicParsing
}

# ---------------------------------------------------------------------------

$serial0 = ''      # 首次启动固化出来的那两个值，[3] 段要用
$uuid0   = ''
$hostHit = ''

try {

# ===========================================================================
Section '[0] 量具自检：先量一量"复算"这把尺子'
# ===========================================================================
#
# 陷阱 82：判据本身是坏的时侯，红和绿都不说明任何事。这一段的每一条都能
# 单独把 [3] 段变成空转 —— 而空转的 [3] 段是**绿**的。

$md5abc = Get-Md5Hex 'abc'
Check ($md5abc -ceq '900150983cd24fb0d6963f7d28e17f72') `
      "md5 复算对得上公开测试向量（md5('abc')）" `
      "得到 $md5abc —— 复算这把尺子坏了，[3] 段的结论一律不可信"

$probe = Get-SeededSerial 'host=probe;guid=probe'
Check ($probe -cmatch '^[0-9A-F]{16}$') `
      "复算的序列号形状对（16 位大写十六进制）：$probe"
Check ((Get-SeededSerial 'host=probe;guid=probe') -ceq $probe) '复算是确定性的（两次调用同值）'
Check ((Get-SeededSerial 'host=probe;guid=other') -cne $probe) `
      '种子变一位，复算值就跟着变（等式两边不是同一个常量）'

$guid = Get-MachineGuid
Check ([bool]$guid) "读得到本机 MachineGuid（$guid）" `
      '读不到的话 [3] 段复算出来的种子是 "host=…;guid="，两边必然不等 —— 红在错的地方'
$hosts = Get-HostnameCandidates
Check ($hosts.Count -gt 0) ("本机机器名候选 " + ($hosts -join ' / '))

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$macs = Get-LocalMacs
Write-Host ("  本机 MAC 集合（" + $macs.Count + " 个）：" + ($macs -join ', ')) -ForegroundColor DarkGray
Write-Host ("  当前会话是管理员吗：$isAdmin") -ForegroundColor DarkGray

Check (Test-Path -LiteralPath $exe) "被测程序在：$exe"
Check (Test-Path -LiteralPath $dist) "前端产物在：$dist"

# ===========================================================================
Section '[1] 前置：端口空闲、没有别人在跑'
# ===========================================================================
Stop-Desk2Onvif
Start-Sleep -Milliseconds 300
$busy = @()
foreach ($p in @($HttpPort, $RtspPort)) { if (Test-PortBusy $p) { $busy += $p } }
Check ($busy.Count -eq 0) "端口 $HttpPort / $RtspPort 都空着" `
      ("被占的是 " + ($busy -join ', ') + ' —— 先把它让出来再跑，否则下面红的是"后端起不来"')
if ($busy.Count -ne 0) { throw '端口被占' }

# ===========================================================================
Section '[2] 首次启动：身份推导一次，写进 config.json'
# ===========================================================================

Check (-not (Test-Path -LiteralPath $cfgPath)) '起跑时还没有配置文件（真的是"首次"）'
$up = Start-IdentityRun
Check $up '服务起来了'
if (-not $up) { throw '后端起不来' }

$serial0 = Get-CfgField 'serial_number'
$uuid0   = Get-CfgField 'device_uuid'

# 先断言**非空**。不先断这一步的话，下面"形状对不对""等于不等于复算值"全都
# 会把空串当成一个普通取值来判，而"字段根本不存在"和"字段是空的"在读数上
# 长得一模一样（陷阱 91）。
Check ([bool]$serial0) 'config.json 里出现了 serial_number（非空）' `
      '空或缺失 ⇒ 固化那一段没落地 —— 后面几条都会跟着红，但根因在这一条'
Check ([bool]$uuid0) 'config.json 里出现了 device_uuid（非空）'

Check ($serial0 -cmatch '^[0-9A-F]{16}$') "序列号形状对：$serial0" `
      '纯大写十六进制、16 位 —— 这是 NVR 那边也按它显示的东西'
Check ($uuid0 -cmatch '^urn:uuid:[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$') `
      "UUID 形状对（v4 的版本位/变体位都在）：$uuid0" `
      '版本位不是 4、变体位不是 8/9/a/b 的话，严格的解析器会判"格式非法的 UUID"'

$api = Get-ApiConfig
Check ($api.onvif.serial_number -ceq $serial0) `
      "运行中的程序报的序列号和文件里那份一致（$($api.onvif.serial_number)）"
Check ($api.onvif.device_uuid -ceq $uuid0) '运行中的程序报的 UUID 和文件里那份一致'

Stop-IdentityRun

# ===========================================================================
Section '[3] 复算：种子 = host + MachineGuid，不含 MAC'
# ===========================================================================

$guidOk = $false
foreach ($h in $hosts) {
    $seed = 'host=' + $h + ';guid=' + $guid
    if ((Get-SeededSerial $seed) -ceq $serial0) { $hostHit = $h; $guidOk = $true }
}
Check $guidOk "复算的序列号 == 程序固化那份（命中机器名：$hostHit）" `
      ("试过 " + ($hosts -join ' / ') + "；程序那份是 $serial0。" +
       '不等 ⇒ 种子里**还掺了别的东西**（MAC 是头号嫌疑，见 net_identity.cpp）')
if ($guidOk) {
    $seed = 'host=' + $hostHit + ';guid=' + $guid
    Check ((Get-SeededUuid $seed) -ceq $uuid0) '复算的 UUID == 程序固化那份（同一个种子）'
}

# 判别力自证：把 MAC 并进种子再算一遍，**必须**算出不同的值。
# 这一条要证的不是"程序没有 MAC"（那是上一条的事），而是"如果它有，上一条
# 会红" —— 没有它的话，上一条的绿可以被解读成"这条等式怎么算都相等"。
if (-not $guidOk) {
    # 上面那条已经红了，根因是"等式没命中"。这里**不再补一条红的** ——
    # 两条红的指向不同会让人以为是两个问题，而 [3] 段存在的理由只有一个。
    # （第一次跑变异检验时就是这么糊的：判别力那条报的是"本机读不到 MAC"，
    #   与真正的原因毫无关系。）
    Write-Host '  [跳过] 判别力自证：上面那条等式没命中，先修那一条。' -ForegroundColor DarkGray
} elseif ($macs.Count -eq 0) {
    Check $false '本机读不到任何网卡 MAC，判别力自证无法进行' `
          '不是产品的问题，是这台机器的读数为空 —— 但上一条的绿因此失去了旁证'
} else {
    $seedMac = 'host=' + $hostHit + ';mac=' + ($macs -join ',') + ';guid=' + $guid
    Check ((Get-SeededSerial $seedMac) -cne $serial0) `
          '把 MAC 并进种子，复算值就变了 —— 所以上一条的等式真的能抓到 MAC' `
          '两者相等 ⇒ 这条等式对"种子里有没有 MAC"不敏感，上一条等于没验'
}

# ===========================================================================
Section '[4] 文件是权威：手改文件，程序就得听文件的'
# ===========================================================================
#
# 这一条钉的是"**不是**每次启动重推"。如果身份只在内存里推导、磁盘上那份
# 永远缺这两个字段，那么每次启动都是"按当前机器重推一遍" —— 换过网卡的机器
# （第五轮之前种子里有 MAC）升级上来就会推出一串新值，NVR 那边多一台设备。
# 手改文件是最省的判别法：重推的实现会把手改的值**盖掉**。

Check (Set-CfgField 'serial_number' $SENT_SERIAL) '把手改的序列号写进了文件'
Check (Set-CfgField 'device_uuid'   $SENT_UUID)   '把手改的 UUID 写进了文件'
Check ((Get-CfgField 'serial_number') -ceq $SENT_SERIAL) '再读一次文件，确认改的真的落盘了'

$up = Start-IdentityRun
Check $up '服务又起来了'
if ($up) {
    $api = Get-ApiConfig
    Check ($api.onvif.serial_number -ceq $SENT_SERIAL) `
          "程序报的是文件里那份手改值（$($api.onvif.serial_number)）" `
          ("期望 $SENT_SERIAL。报的是别的 ⇒ 程序没读文件、而是又推导了一次" +
           "（那就是这一轮要修的那个行为）")
    Check ($api.onvif.device_uuid -ceq $SENT_UUID) `
          "UUID 同样听文件的（$($api.onvif.device_uuid)）"
}
Stop-IdentityRun

# ===========================================================================
Section '[5] 一次真实的保存之后，身份还是文件里那份'
# ===========================================================================
#
# 方案原文：「改一次 capture.fps 触发存盘 → 重开 → 序列号是文件里那份
# （而不是又推导了一次）」。走 **POST /api/config** 而不是手改文件 ——
# 那才是 `apply_config` → 只读锁 → `save_config` 这条真实路径，手改文件
# 根本碰不到它。
#
# **fps 不能单独改。** 第一次跑这一段时红的是"fps 没落盘"，而 POST 回的是
# 200、`note` 写着"配置已保存（这次没有改动任何字段）"。原因在
# `validate_config`：`capture.quality` 只要是**认得的档位**（出厂的 balanced
# 就是），`snap()` 就会把 `capture.fps` / `bitrate_kbps` / `max_height`
# 一律拉回该档的值 —— 填 30 进去，落盘的是 25。所以要把 quality 一起改成
# `custom`（那一档专门表示"下面这些值我自己定"）。
# 这个坑值钱的地方在于：**它一声不吭**（档位改回去的说明只进了日志，
# 没进响应里的 note）。所以这里两条一起改、并且分别断言。

$up = Start-IdentityRun
Check $up '服务起来了（第三趟）'
if ($up) {
    $api = Get-ApiConfig
    $fpsWas = $api.capture.fps
    $fpsNew = if ($fpsWas -eq 25) { 30 } else { 25 }
    Check ($api.capture.quality -eq 'balanced') `
          "出厂的画质档位是 balanced（这就是 fps 会被夹回去的原因）" `
          "实际是 $($api.capture.quality) —— 这一段的注释按 balanced 写，换档位了要跟着改"
    $api.capture.quality = 'custom'
    $api.capture.fps = $fpsNew
    $sent = $false
    try {
        $r = Send-ApiConfig $api
        $sent = ($r.StatusCode -eq 200)
    } catch {
        Write-Host ("  保存失败了：" + $_.Exception.Message) -ForegroundColor Red
    }
    Check $sent "POST /api/config 保存成功（quality → custom，fps：$fpsWas → $fpsNew）" `
          '没存成的话，这一段的绿说明不了"保存不动身份"'

    $fileSerial = Get-CfgField 'serial_number'
    $fileUuid   = Get-CfgField 'device_uuid'
    Check ($fileSerial -ceq $SENT_SERIAL) `
          "保存之后文件里的序列号没被改写（$fileSerial）" `
          ("期望还是手改的那个 $SENT_SERIAL。变成别的 ⇒ save_config 把身份重推了")
    Check ($fileUuid -ceq $SENT_UUID) "保存之后文件里的 UUID 没被改写（$fileUuid）"
    $fpsOnDisk = Get-CfgNum 'fps'
    Check ($fpsOnDisk -eq $fpsNew) `
          "确认这一次保存真的写盘了（文件里的 fps = $fpsOnDisk）" `
          'fps 没落盘 ⇒ 这次"保存"根本没发生，上面两条的绿是空的'
    Check ((Get-CfgField 'quality') -eq 'custom') `
          "画质档位也落盘了（$(Get-CfgField 'quality')）" `
          '档位没落盘而 fps 落了盘，说明夹取那一步和落盘那一步不是同一份值'
}
Stop-IdentityRun

$up = Start-IdentityRun
Check $up '服务又起来了（第四趟）'
if ($up) {
    $api = Get-ApiConfig
    Check ($api.onvif.serial_number -ceq $SENT_SERIAL) `
          "重启之后程序仍然报文件里那份（$($api.onvif.serial_number)）" `
          '第四趟启动是"文件里已经有值"的那条路 —— 它必须原样用，不能重推'
    Check ($api.onvif.device_uuid -ceq $SENT_UUID) 'UUID 同样原样用文件里那份'
}
Stop-IdentityRun

# ===========================================================================
Section '[6] 真的改一次 MAC 集合（需要管理员；本机多半做不到）'
# ===========================================================================

if (-not $MacChange) {
    Write-Host '  [未测到] 没有传 -MacChange —— 这一条要求"真的让网卡集合变一次"，' -ForegroundColor Yellow
    Write-Host '           默认不做（见文件头："为什么 [3] 是复算而不是真的拔一块网卡"）。' -ForegroundColor Yellow
    Write-Host '           [3] 段那条等式覆盖了同一个主张，且做过真变异检验。' -ForegroundColor Yellow
} elseif (-not $isAdmin) {
    Write-Host '  [未测到] 传了 -MacChange，但当前会话不是管理员。' -ForegroundColor Yellow
    Write-Host '           在**提权**的 PowerShell 里跑同一条命令即可真做：' -ForegroundColor Yellow
    Write-Host ("             Disable-NetAdapter -Name '<网卡>' -Confirm:`$false") -ForegroundColor Yellow
    Write-Host ("             powershell -File tests\info_identity.ps1 -MacChange -Adapter '<网卡>'") -ForegroundColor Yellow
    Write-Host '           能安全切换的只有 Status=Up 的那两块（WLAN / vEthernet 默认交换机），' -ForegroundColor Yellow
    Write-Host '           而它们各自承载着用户的网络与 WSL —— 所以这件事必须由人来点头。' -ForegroundColor Yellow
} elseif (-not $Adapter) {
    Check $false '传了 -MacChange 却没给 -Adapter' '不知道要切哪一块，不做比乱做好'
} else {
    $a0 = Get-NetAdapter -Name $Adapter -ErrorAction SilentlyContinue
    Check ([bool]$a0) "找得到网卡 $Adapter"
    if ($a0) {
        # 先量一次基线：把文件里那两个字段**清空**，逼程序重新推导一次。
        # 清空而不是删字段，是因为 config_from_json 对缺失和空串一视同仁，
        # 而少写一个字段会让"这一份配置是不是被程序改过"多出一层解释。
        Check (Set-CfgField 'serial_number' '') '先把文件里的序列号清空（逼它重新推导）'
        Check (Set-CfgField 'device_uuid'   '') 'UUID 同样清空'
        $up = Start-IdentityRun
        $serA = ''; $uuA = ''
        if ($up) {
            $api = Get-ApiConfig
            $serA = $api.onvif.serial_number; $uuA = $api.onvif.device_uuid
        }
        Stop-IdentityRun
        Check ([bool]$serA) "基线：清空后重新推导得到 $serA"

        $changed = $false
        try {
            Disable-NetAdapter -Name $Adapter -Confirm:$false -ErrorAction Stop
            Start-Sleep -Seconds 3
            $changed = -not ((Get-NetAdapter -Name $Adapter).Status -eq 'Up')
        } catch { Write-Host ("  禁用失败：" + $_.Exception.Message) -ForegroundColor Red }
        Check $changed "网卡 $Adapter 真的被禁用了（集合变了）" `
              '没变成 ⇒ 前后两次读的其实是同一个集合，后面的比较是空转'

        Check (Set-CfgField 'serial_number' '') '再清空一次'
        Check (Set-CfgField 'device_uuid'   '') 'UUID 再清空一次'
        $up = Start-IdentityRun
        $serB = ''; $uuB = ''
        if ($up) {
            $api = Get-ApiConfig
            $serB = $api.onvif.serial_number; $uuB = $api.onvif.device_uuid
        }
        Stop-IdentityRun

        try { Enable-NetAdapter -Name $Adapter -Confirm:$false -ErrorAction Stop } catch {
            Write-Host ("  **网卡没能恢复**，请手工启用：" + $Adapter) -ForegroundColor Red
        }

        Check ($serB -ceq $serA) "换了网卡之后序列号不变（$serA → $serB）" `
              '变了 ⇒ 种子里还有跟着网卡走的东西'
        Check ($uuB -ceq $uuA) "换了网卡之后 UUID 不变"
    }
}

} finally {
    Stop-IdentityRun
    if (-not $Keep) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    else { Write-Host "临时目录留着：$work" -ForegroundColor DarkGray }
}

Write-VerifySummary
