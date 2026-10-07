# Desk2ONVIF

把 Windows 桌面伪装成一台 **ONVIF Profile S** 网络摄像机：海康、大华等 NVR
能像发现普通 IP 摄像机一样自动发现它（WS-Discovery），把它作为 IP 通道加入，
然后拉取 H.264 视频流。

```
DXGI 桌面复制 ──► 指针合成 ──► FFmpeg / libx264 ──┬──► Live555 RTSP ──► NVR / VLC / ffplay
   (采集线程)                                  │    (调度线程)   ├─ /live      主码流
                                               │                 └─ /live_sub  子码流
                                               ├──► fMP4 over WebSocket ──► 浏览器预览
                                               └──► MJPEG ──► GET /snapshot.jpg

libuvcpp（HTTP / WebSocket / ONVIF SOAP / WS-Discovery UDP 3702）
```

主、子两路是**两个独立的 x264 实例**，同一张 BGRA 进去、各自缩放到自己的目标尺寸
（不是"编一次再抽帧"，也不是"编一次再解码"）。子码流见下面「两路码流」。

## 状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 0 | 骨架与依赖供给 | ✅ |
| Phase 1 | DXGI 采集 + x264 编码 → MP4 | ✅ |
| Phase 2 | RTSP 推流（Live555） | ✅ |
| Phase 3 | Web 服务层（REST / WebSocket / 静态资源） | ✅ |
| Phase 4 | ONVIF（SOAP / Digest / WS-Discovery） | ✅ 协议层全通，**真实 NVR 待你实测** |
| Phase 5 | Vue 3 前端 | ✅ 后端契约逐条验证过（77 项；有人拉流时是 97 项），**浏览器实机也过了**（`tests/browser/`） |
| Phase 5b | 登录页 + 路由守卫 + 控制台改版（预览页作首页，科技风格） | ✅ 契约 `web_auth.py` 51 项、浏览器 `login_probe.mjs` 28 项 |
| Phase 6 | 子码流、快照、持久化、指针合成、防火墙辅助 | ✅ |
| Phase 6 | 配置热生效（ONVIF 身份当场生效，其余逐字段报"要重启"） | ✅ |
| Phase 6 | DXGI 自恢复的实机验证 | ⬜ |

一条命令起一整台"摄像机"：

```powershell
# 桌面 → RTSP + ONVIF + Web UI。**ONVIF 默认就是开的**，不用加开关
.\build\bin\desk2onvif.exe --rtsp --http

# 看看这台机器有哪些输出，哪些真的能录
.\build\bin\desk2onvif.exe --list-displays

# 录 10 秒
.\build\bin\desk2onvif.exe --record out.mp4 --seconds 10

# 推 RTSP（默认 rtsp://<本机IP>:554/live），拉流看
.\build\bin\desk2onvif.exe --rtsp --seconds 60
ffplay -rtsp_transport tcp rtsp://127.0.0.1:554/live

# 依赖自检
.\build\bin\desk2onvif.exe --selfcheck
```

Web / ONVIF 起来之后：

| 入口 | 说明 |
|---|---|
| `http://<本机IP>:8080/` | Vue 控制台，**没登录会先跳到登录页**。路由：`/` 预览（首页）、`/status` 状态、`/config` 配置、`/login` 登录 |
| `POST /api/login` | `{"user":…,"password":…}` 换一个会话 cookie。同一个 IP 在 5 分钟内错满 5 次会锁 60 秒（429 + `Retry-After`，冷却期内**正确口令也拒**） |
| `GET /api/session` | "我登录了吗"。**匿名可达** —— 前端进任何页面前都先问这一句 |
| `POST /api/logout` | 让浏览器丢掉 cookie（服务端无状态，见下） |
| `GET /api/status` | fps、码率、队列、观众数、ONVIF 与预览的计数；**逐路一份**（`rtsp_streams` 数组） |
| `GET|POST /api/config` | 读写配置（返回的是**实际生效值**，含命令行覆盖） |
| `POST /api/onvif/restart` | 重新组播 Hello（NVR 那边"搜不到"时用） |
| `GET /api/snapshot.jpg` | 当前画面（MJPEG，`Cache-Control: no-store`）。**要会话 cookie**，且**永远不发** Digest 挑战 |
| `GET /snapshot.jpg` | 同一个画面，**给 NVR 用的**（`onvif.snapshot_path`）。认两种凭据：会话 cookie 或 ONVIF Digest；ONVIF 开着时**必定**先发挑战 |
| `WS /ws/stats` | 每秒推一次状态 JSON。未登录会被后端用 **1008** 关掉（不是 401 —— WS 升级不经过 HTTP 的鉴权中间件） |
| `WS /ws/preview` | fMP4 分片，浏览器 MSE 直接播（延迟约 1 个 GOP）。同样受 1008 守卫 |

`POST /onvif/device_service` 是设备服务，`POST /onvif/media_service` 是媒体服务。
除了 `GetSystemDateAndTime`（规范要求匿名），其余操作都要 **HTTP Digest**；
用户名/口令默认都是 `admin`，配置项是 `onvif.user` / `onvif.password`。

> **Web 控制台默认要登录，而默认口令 `admin` 就印在 `--help` 里** —— 加上默认
> 监听 `0.0.0.0`，等于**出厂状态下同一局域网内任何机器都能用 `admin`/`admin`
> 进来**看你的桌面画面、读配置、改配置。所以：
>
> - **只给本机用**：加 `--http-host 127.0.0.1`。最省事，也最安全。
> - **要给 NVR 或别的机器用**：务必换掉口令 —— `--http-password <口令>`，
>   或配置文件的 `web.password`；用户名同理（`web.user`）。
>
> 四点容易踩的：
>
> - **Web 的凭据和 ONVIF 的是两套**（`web.user`/`web.password` 对
>   `onvif.user`/`onvif.password`）。改 ONVIF 那个口令**不会**保护这个控制台。
> - **关掉登录**（`--no-http-auth`，或 `web.auth_enabled=false`）等于把"读配置、
>   改配置、看桌面画面"三件事一起交出去，启动时会打一条 WARN。
> - 口令设成**空串**不是"关掉鉴权"，而是"口令就是空"：登录框照常出现、校验
>   照走，任何人按一下回车就进去了。留空时会打 WARN，而不是安静地开门。
> - 会话的签名密钥是**每进程一份的随机数**，不是口令本身。所以**重启程序会把
>   所有已登录的人踢下线**（通常正是想要的）；反过来，**改口令不会踢人** ——
>   已经发出去的 cookie 一直有效到 7 天后过期。要立刻清空所有人只能重启。

`--list-displays` 会对每个输出**真的**建一次桌面复制会话再关掉。这是判断
"能不能录"的唯一可靠办法 —— 按适配器名字去猜（"名字里有 idd/virtual 就当
虚拟屏"）只能覆盖已知的几个厂商，还会误伤真实硬件。

`--record` 的完整选项见 `--help`。不给 `--seconds` 就一直录，Ctrl+C 停止
（会正常收尾把 MP4 索引写完，不是硬杀）。

> **接 NVR 之前先看防火墙。** Windows 对入站连接默认全拦，而本机 `ffplay` 拉流
> 走回环、**不受防火墙影响** —— 所以"NVR 搜不到设备 / 添加时超时"的第一嫌疑人
> 就是它。先查：
>
> ```powershell
> .\build\bin\desk2onvif.exe --firewall status --rtsp --http     # 只读，不需要管理员
> .\build\bin\desk2onvif.exe --firewall print  --rtsp --http     # 只打命令，自己抄去执行
> .\build\bin\desk2onvif.exe --firewall add    --rtsp --http     # 直接加（要管理员）
> ```
>
> 加出来的规则只对**同网段**、**专用/域网络**放行（`profile=private,domain
> remoteip=localsubnet`）—— 不是"对全世界开放"。NVR 在别的子网/VLAN 里的话，
> 用 `print` 把命令打出来，把 `remoteip=localsubnet` 改掉。
>
> 它**只在你显式调用时才碰防火墙**：`--rtsp`/`--http` 起服务时一行规则都不会改。
> 一个录屏工具不声不响地把端口对局域网开放，是使用者从没同意过的事。

> **已知未做**：DXGI 的自恢复（锁屏 / 切分辨率 / 显示器休眠）代码路径是通的、
> 统计里也一直有「会话重建 N 次」，但**这三种情况我没在这台机器上逐个实测过**。

## 配置热生效：能改的当场生效，改不了的逐字段告诉你

`POST /api/config` 的回应里**没有**一个笼统的 `restart_required` 布尔值，而是
两个数组 `applied` 和 `pending`：

```json
{ "applied": ["onvif.model"], "pending": ["rtsp.sub_height"], "restart_required": true }
```

一次保存里"这项当场生效、那项要重启"是常态，只给一个布尔值时前端只能二选一，
而**两种选法各有一半的时候在骗人**。逐字段报是把这件事说清楚的最小粒度。

当场生效的有两组：

- **ONVIF 的身份与凭据**（`manufacturer` / `model` / `firmware_version` /
  `hardware_id` / `serial_number` / `device_name` / `location` / `user` /
  `password`）—— 它们每次响应现读，改完下一次 `GetDeviceInformation` /
  Digest 挑战就是新值，**不用重启、不用重连**。
- **Web 控制台的登录开关与凭据**（`web.auth_enabled` / `web.user` /
  `web.password`）—— 改完下一次登录就用新的。有一点要知道：**它不会把已经
  登录的人踢下线**（令牌签的是每进程一份的随机密钥，不是口令），所以"改了口令
  怎么还进得去"不是 bug —— 要立刻清空所有登录，只能重启程序。

其余全部要重启：采集参数要重建编码器，端口要重新 bind，都做不到热改。

**热/冷的边界不是一张手写的清单**，而是"这次保存实际同步进了哪些字段"倒推
出来的：`diff_config(cfg, next)` 得到"变了什么"，`diff_config(cfg, running_cfg)`
得到"还没生效的"，两者相减就是 `applied`。手写清单迟早会和真正同步的那几行
代码不一致，而症状恰好是它要防的那一种 —— 某个字段改了却没进 `pending`，
界面上显示"已生效"。

`GET /api/config` 上也带 `pending`。只放在 POST 回应里的话，用户存完刷新一下
提示就没了，而表单里显示的又都是新值，看起来一切已生效。

> **码率是改不了的，这一点是证明过的，不是没做。** 实测（`tests/x264_reconfig_check.cpp`）
> 运行中改 `avctx->bit_rate` 后 libx264 封装层不认；`x264_encoder_reconfig` 在
> `avcodec-63.dll` 的**导出表里出现 0 次**，所以 FFmpeg 也没有别的路可以把它
> 递下去。顺带一提，这个问题不是"懒得做"，是**在这条链路上无解**。

## 两路码流（主码流 + 子码流）

主码流是桌面原分辨率，子码流是同一画面缩到 `--sub-height`（默认 360），
两条各自独立的 x264 实例、各自独立的 RTSP 路径（`/live` 与 `/live_sub`）。

**为什么默认就开**：NVR 的多画面网格默认拉的是**主码流**。16 路网格就是同时
拉 16 条全分辨率的流 —— 编码端 CPU 是 16 倍，网络也是 16 倍。开着子码流，
网格走子码流、点开单画面时 NVR 才切回主码流，这是它唯一的作用，也是所有
真实摄像机都这么干的原因。

```powershell
.\build\bin\desk2onvif.exe --rtsp --sub-height 360 --sub-bitrate 512   # 默认就是这样
.\build\bin\desk2onvif.exe --rtsp --no-sub                             # 只推主码流
```

ONVIF 侧它是**第二个 profile**：`GetProfiles` 返回 `main` 和 `sub` 两个
（token 就是这两个名字），各自带自己的分辨率；`GetStreamUri` 按 token 给
各自的地址。NVR 添加通道时选哪个 profile，就拉哪一路。

> 子码流的默认值是**开**，这一点值得留意：它意味着默认情况下 CPU 占用比
> 「只编一路」高。不给 `--rtsp` 时它不产生任何开销（没有消费者就不编）。

## 画面里的鼠标指针

**桌面复制给出的画面里没有鼠标指针。** DXGI 把指针单独交出来：位置和可见性在
帧信息里，形状要另外用 `GetFramePointerShape` 取。所以「录出来的视频里没有
鼠标」不是 bug，是 DXGI 的默认行为 —— 而对一个要冒充摄像机的桌面流来说，
看不见鼠标就等于看不见人在做什么。这个项目把它合成了回去（`src/capture/
cursor_overlay.h`），三种形状各自的绘制规则是**从 Windows 的 DDB 语义推出来的**：

| 形状 | 规则 |
|---|---|
| 彩色 | 逐像素按自己的 alpha 混合 |
| 掩码色 | `alpha == 0` 表示**反相**（`dst.rgb ^= src.rgb`），Windows 10 的反色指针走这支 |
| 单色 | 1bpp，高度是实际的两倍（上半 AND、下半 XOR）；先 SRCAND 再 SRCINVERT，**顺序不能反** |

还有一条容易忽略的：桌面静止时 DXGI 不产出帧，而"鼠标动了"是**独立于画面更新**
的另一种帧来源。那一类帧里的桌面纹理是**未定义内容**（DXGI 只在画面变了才更新
它），所以指针必须画在那份"上一次的完整桌面图"上，而不是重新去读纹理。

> `--list-displays` 与 `--selfcheck` 都不动鼠标。真要端到端验指针，用
> `tests\cursor_harness.ps1`（它会开一块纯色全屏画布并**驱动鼠标**走一条固定
> 路径）—— 那一条会真的动你的鼠标，别在远程操作时跑。

### 已验证到哪一步

每个阶段都用**可观察的证据**收尾，不靠"应该能跑"。脚本在 `tests/`，都是
标准库写的独立探针（`python -I tests/xxx.py`），不依赖第三方 ONVIF 库 ——
理由在脚本头注释里：现成的库会把我们写错的响应"修好"，而这里要验的恰恰
是那些东西。

| 验证 | 脚本 | 结果 |
|---|---|---|
| 子码流 + ONVIF 双 profile + Web 契约 + WS-Discovery（**一次跑完**） | `rtsp_sub_e2e.ps1` | 全过，见下 |
| SOAP 命名空间 / Digest 挑战应答 / Fault 形状 / **身份热生效** | `onvif_soap.py` | 逐条严格解析，68 项全过 |
| 改身份后**换一个新挑战**再 `GetDeviceInformation`，确认新值已生效 | 同上 `[9]` | Model / SerialNumber 都是新值，没重启 |
| Probe → ProbeMatch 完整往返（含 XAddrs 可达） | `wsd_probe.py --mode client` | 单播应答、`RelatesTo` 对得上 |
| 状态 WebSocket 每秒推送 | `ws_probe.py` | 1.00 s 间隔，漂移 0.94~1.00 |
| fMP4 初始化段 + 分片，拼起来能解码 | `preview_probe.py` | 300~420 帧，解码零错误输出 |
| 静态产物 / `/api/config` 往返（含 `applied`/`pending` 语义）/ `/api/status` 字段 / **HTTP 与 WS 两条路字段集一致** | `web_contract.py` | 全过（**项数取决于跑的时候有没有人在拉流**：没人拉 77 项，挂着两路拉流客户端 97 项 —— 有几项只在那时才验得到） |
| 登录契约：哪条路该被拦、哪条路不该被拦 | `web_auth.py` | 51 项全过。**必须开着 ONVIF 跑** —— 第 [9] 段要的是 `/snapshot.jpg` 上的 Digest 挑战，`--no-onvif` 下会有 3 项必挂（那是跑法不对，不是功能坏了） |
| **真实浏览器**里的登录流程与页面渲染 | `browser/login_probe.mjs` | 28 项全过：路由守卫带 `next`、错口令给的是人话而不是 `HTTP 401`、登录后回到原目标、刷新仍在、退出登录 |
| **真实浏览器**里预览页到底播不播得出来 | `browser/probe.mjs` | 全过：`readyState 4`、2880x1920、`currentTime` 持续推进 |
| 指针合成的三条像素规则（含四种单色组合与越界裁剪） | `cursor_check.cpp` | 33 项全过 |
| MJPEG 快照（含 stride、qscale 单调性、奇数分辨率） | `mjpeg_check.cpp` | 27 项全过 |
| fMP4 封装（init 段 + 5 次 write 产出的分片，整段解码） | `fmp4_check.cpp` | 全过 |
| **码率能不能热改**（三段同样内容的码流，中间只改 `bit_rate`） | `x264_reconfig_check.cpp` | 实测字节数不变 ⇒ **不能**，见上。**它退出码是 1，而那是预期结果** —— 它断言的是"该翻上去"，断言失败恰好证明了改不动。别把它接进 CI 当成挂了的用例 |

自己会起进程、一条命令就能跑的有三个：

```powershell
powershell -ExecutionPolicy Bypass -File tests\rtsp_sub_e2e.ps1   # 采集 + 两路 RTSP + ONVIF + Web 契约 + WSD

# 下面几个是浏览器探针，需要先装一次依赖（playwright-core + 一个本机 Chrome）
cd tests\browser ; npm install
node smoke.mjs                   # 先跑这个：Chrome 起不起得来、MSE 认不认 avc1
node login_probe.mjs --headed    # 登录流程；会自己起一个临时后端（8125/8126）
node probe.mjs                   # 预览能不能播；自己起后端（8123/8124）
```

两个浏览器探针都**自己起后端、自己关**，用的是 `out/browser/` 下的临时配置，
**不碰你 `%APPDATA%` 里的那份**。加 `--keep` 可以留着现场自己翻。

三个 `.cpp` 是**单元测试**：不碰窗口、不碰 DXGI、不碰网络，直接把数据喂进去再
把结果逐像素/逐字节读回来，所以不需要采集会话也不需要管理员。用 `g++` 手编
（不用 cmake —— 它们各自只依赖一两个 .cpp，链接行比写 CMake 目标还短），
编译命令在 `cursor_check.cpp` 的文件头里，`mjpeg_check` / `fmp4_check` 同理，
只是多链几个 FFmpeg 库。

```powershell
# 例：指针合成（不需要任何第三方库）
g++ -std=c++17 -Isrc tests/cursor_check.cpp src/capture/cursor_overlay.cpp -o build/cursor_check.exe
```

`rtsp_sub_e2e.ps1` 一次起采集，把四件事一起验了（分开跑的话每件都要重新起一次
进程，反而更容易漏）：

```
[1] 两路 RTSP   main 2880x1920 / sub 540x360，都是 30/1，各解 5 秒零报错
[2] ONVIF       GetProfiles 恰好 {main, sub}，GetStreamUri 两个不同的地址；
                改身份 + 换口令后**不重启**直接重新鉴权、重新取设备信息
[3] Web 契约    静态产物逐个全 200 且 MIME 正确；/api/config 往返；
                applied/pending 逐字段语义；逐路字段对账；
                HTTP 与 WS 两条路的字段集必须**一致**（差一个键就挂）
[3.2] 登录契约  该拦的拦住、不该拦的不拦（web_auth.py，51 项）
[3b] WS-Discovery  从临时端口发 Probe，收到 ProbeMatch 且 XAddrs 可达
[4] 收尾统计    26.0s / 780 包 / 13 关键帧 / 30.0 fps，主 2999 kbps、子 409 kbps，
                队列丢弃 0、缓冲池耗尽 0、会话重建 0
```

那个 `[1]` 里"两路必须是两个不同的尺寸"不是凑数的断言：两路尺寸相同就说明
子码流其实没编，只是把主码流又推了一遍 —— **而那样表面上一切正常**。

`preview_probe.py` 里那三条断言不是形式主义：MSE 对"初始化段不是第一块"
和"分片不是从 `moof` 开始"的反应都是**拒收后面所有分片**，而且报的错指
不到真正的原因；中途连上来的客户端也要拿到初始化段（它整个会话只产一次），
否则页面一直黑着而控制台什么都不说。

**没有验证的两件事**，都不会由脚本替掉：

1. **真实海康/大华 NVR 的发现与拉流** —— 这需要你手上的设备。协议层能替的
   都替了（ONVIF Device Manager 也建议过一遍），剩下的就是插上 NVR 试。
2. **DXGI 的自恢复** —— 锁屏、切换分辨率、显示器休眠这三条路。

（浏览器里的那部分**以前**在这张单子上，现在不在 —— `tests/browser/` 下两个
探针起真实 Chrome 把登录流程、路由守卫、错口令提示、MSE 真的播起来全跑了一遍，
连截图都留了。剩下没有脚本能替的，只有上面那两件需要真硬件的事。）

## 构建

需要 **MinGW64 / MSYS2**（GCC 15+）、CMake 3.21+、Ninja，以及 7-Zip（解
FFmpeg 包用）。

```powershell
# 1. 供给依赖（三个都幂等，重复跑无副作用）
.\scripts\fetch_ffmpeg.ps1
.\scripts\build_live555.ps1
.\scripts\fetch_uvcpp.ps1      # 只搬运；库本身要先按 third_party\README.md 编好

# 2. 构建 + 自检
.\scripts\build.ps1
```

`build.ps1` 会配好 PATH、配置、编译、部署运行期 DLL，最后跑一遍自检。
**建议直接用它，不要手敲 cmake** —— 原因见下。

> 如果 `desk2onvif.exe` 正开着，链接会因为**文件被占用**而失败。脚本会告诉你
> 是谁占着，并给出 `.\scripts\build.ps1 -StopRunning`（它会**直接终止**那些进程，
> 不打招呼、也不等它写盘）。

<details>
<summary>手工调用 cmake 的等价命令</summary>

```powershell
# PATH 必须前置 msys64\mingw64\bin，否则编译器会以一种完全静默的方式失败
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH

cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/ninja.exe
cmake --build build -j
```

</details>

> **为什么不能直接用普通的 PowerShell 敲 cmake。**
> MSYS2 的 `gcc.exe` 自己能启动（它在 `mingw64\bin` 里，DLL 走"同目录优先"），
> 但它调起来的 `cc1.exe` 在 `lib\gcc\...\15.1.0\` 下 —— 那里没有任何运行库 DLL，
> `cc1` 只能靠 PATH 找。PATH 里没有 `mingw64\bin` 时，`cc1` 会从别处（例如某个
> 自带 `zlib1.dll` 的程序目录）加载到版本对不上的同名 DLL，然后以
> `STATUS_ENTRYPOINT_NOT_FOUND` 退出。这个失败**完全静默**：`cc1` 一个字都不打印，
> CMake 报的是 "The C compiler is not able to compile a simple test program"，
> Ninja 那边只有一行 `FAILED: [code=1]`，没有任何字样指向 PATH。
>
> `CMakeLists.txt` 里对这个情况有显式检查，会直接说明原因；
> 但 `build.ps1` 更省事 —— 它一开始就把 PATH 配好。

## 打包

```powershell
.\scripts\package.ps1 -Zip                  # 安装包 + 绿色版 zip，都进 release\
.\scripts\package.ps1 -NoBuild              # 只用现有产物重新打包（改的只是打包脚本时用）
.\scripts\package.ps1 -AppUrl https://...   # 把项目主页写进安装程序的信息里
```

产出两个交付物，**装出来的文件完全一样**，只是分发方式不同：

| | 产物 | 说明 |
|---|---|---|
| 安装包 | `release\Desk2ONVIF_Setup_<版本>.exe` | Inno Setup 简体中文向导，装到 `%LOCALAPPDATA%\Programs\Desk2ONVIF`，**不弹 UAC** |
| 绿色版 | `release\Desk2ONVIF-<版本>-win64.zip` | 解压即用，整个目录拷走就能换机器 |

版本号只有 `CMakeLists.txt` 一个来源，由脚本传进 `.iss`；不传 `-AppUrl` 时安装程序里
**不显示**发布者链接（以前那里是个指向 GitHub 首页的占位符，比没有链接更糟）。

两个交付物各配一份**脚本化的验收**，动过打包相关的东西之后跑一下：

```powershell
.\scripts\verify_installer.ps1     # 43 项：真装一遍、真跑起来、真卸干净
.\scripts\verify_portable.ps1      # 48 项：解到空目录、走 start.ps1 跑、验完删掉
```

它们不是"双击一下看着能装"的替代品，而是逐条断言：装下来的文件清单、**exe 里到底
有没有图标和版本资源**（这个 `ExtractAssociatedIcon` 查不出来，见陷阱 56）、配置里的
口令带着引号和反斜杠能不能原样往返、`web\dist` 在不在（少了它程序照跑、只有一个字
都不报的白屏）、起服务后前端契约过不过、卸载是不是一点不剩。绿色版那份还额外验
`.ps1` 必须带 BOM 而 `.bat` 必须没有、`start.ps1 -Portable` 的配置落点、
以及**目录之外不该多出任何东西**。

> 静默安装（`/VERYSILENT`）的参数、口令为什么必须走环境变量、以及静默路径上和
> 交互式不一样的三处行为，写在 `packaging\README-release.md` 里 —— 那份文件就是
> 装出来的 `README.md`。

## 运行期依赖

`build\bin\` 里除了 `desk2onvif.exe` 还会有 16 个 DLL（FFmpeg 8 个、
OpenSSL 2 个、libuvcpp 及它的依赖 6 个），由构建时自动部署。分发时整个
`bin\` 目录一起拷走即可。

`third_party/` 由脚本生成，不进版本库；依赖的各项说明见
[`third_party/README.md`](third_party/README.md)。

MSVC 不支持 —— `third_party` 下的依赖全是 MinGW 格式的（`.dll.a`、`.a`），
CMake 配置阶段就会拦下来并说明原因。

## 为什么是 MinGW64 而不是 MSVC

原方案定的是 MSVC。改成 MinGW64 是因为 **libuvcpp 的 SOAP 模块在所有预编译
包里都是关着的**（`UVCPP_WSDL_ENABLE 0`：头文件在，`uvcpp_soap_*` 的符号不在），
而 ONVIF 的 SOAP 信封层要靠它，只能从源码编。从源码编就需要一套和 FFmpeg 的
导入库格式对得上的工具链，MinGW64 是顺理成章的选择。

## 目录

```
├── CMakeLists.txt
├── cmake/                  依赖查找模块
├── scripts/                幂等的依赖供给脚本
├── src/
│   ├── common/             日志、有界队列、配置、防火墙、字符串/HRESULT
│   ├── capture/            DXGI 桌面复制（枚举输出、捕获会话、帧缓冲池、指针合成）
│   ├── encoder/            x264 编码、MP4 封装、MJPEG 快照、fMP4 分片
│   ├── app/                把上面几条线接起来的采集会话（扇出到各下游）
│   ├── rtsp/               Live555 推流：NALU 切分、有界队列、跨线程唤醒
│   ├── onvif/              SOAP 分发、Digest、WS-Discovery、本机地址选择
│   └── web/                HTTP / WebSocket / REST / 静态资源
├── third_party/            依赖产物（生成，gitignore）
├── web/                    Vue 3 配置界面
└── tests/                  冒烟与协议测试（自写探针，不用第三方 ONVIF 库）
```

源码编成 `desk2onvif_core` 静态库，`desk2onvif.exe` 只是薄薄一层命令行 ——
后期的 `tests/` 直接链库做冒烟测试，不用把命令行解析也链进去。

## 许可证

**GPL-3.0**，见 [`LICENSE`](LICENSE)。

选 GPL-3.0 是被 libx264 决定的：它是 GPL-2.0-or-later，链接进二进制即受约束。
Live555 是 LGPL-3.0-or-later，与 GPLv3 兼容。各依赖的许可证归属见
[`third_party/README.md`](third_party/README.md)。
