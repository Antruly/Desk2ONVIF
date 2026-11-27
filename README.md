# Desk2ONVIF

> 把 Windows 桌面变成一台标准的 **ONVIF Profile S** 网络摄像机。

[![Release](https://img.shields.io/github/v/release/Antruly/Desk2ONVIF?label=release&color=blue)](https://github.com/Antruly/Desk2ONVIF/releases/latest)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-lightgrey)](#运行环境)
[![ONVIF](https://img.shields.io/badge/ONVIF-Profile%20S-green)](#onvif-协议支持)
[![License](https://img.shields.io/badge/license-MIT%20(source)%20%2F%20GPL--3.0%20(binary)-blue)](#许可证)
[![Built on libuvcpp](https://img.shields.io/badge/built%20on-libuvcpp-orange)](https://github.com/Antruly/libuvcpp)

[**⬇ 下载最新安装包**](https://github.com/Antruly/Desk2ONVIF/releases/latest/download/Desk2ONVIF_Setup_0.3.3.exe)
· [绿色版](https://github.com/Antruly/Desk2ONVIF/releases/latest/download/Desk2ONVIF-0.3.3-win64.zip)
· [全部版本](https://github.com/Antruly/Desk2ONVIF/releases)

Desk2ONVIF 把本机桌面模拟成一台标准 ONVIF 摄像机：海康、大华、Milestone
XProtect、Pelco 等 NVR 能像发现普通 IP 摄像机一样**自动发现它**
（WS-Discovery），把它作为 IP 通道加进来，然后拉取 H.264 码流 ——
**不需要任何硬件编码器**。

典型用场：收银机 / 柜员终端 / SCADA 画面 / 考试监考 / 工控屏的集中录像，
这些场景下 NVR 需要的是「把这块屏幕录下来」，而不是再装一台摄像机对着它拍。

> **对标 [DeskCamera](https://www.deskcamera.com/)。** DeskCamera 是 Baltic
> 公司的**闭源收费**软件（PC-locked 永久授权或订阅制，试用版输出带水印），
> 解决的是同一个问题。Desk2ONVIF 是这一能力的**开源实现**：源码可读可改、
> 自行编译即可用、无授权绑定与硬件锁、无输出水印。
>
> 本项目是独立实现，与 Baltic / DeskCamera 无任何关联，也未使用其任何代码
> 或文档。两者的功能对照仅代表本项目作者的理解，**不构成对 DeskCamera
> 现状的描述** —— 以它官网为准。

<sub>[English](#english) · [工程实录](docs/ENGINEERING.md)</sub>

---

## 目录

- [它能做什么](#它能做什么)
- [快速开始](#快速开始)
- [ONVIF 协议支持](#onvif-协议支持)
- [Web 控制台](#web-控制台)
- [命令行](#命令行)
- [架构](#架构)
- [从源码构建](#从源码构建)
- [打包](#打包)
- [运行环境](#运行环境)
- [许可证](#许可证)
- [致谢](#致谢)

---

## 它能做什么

**采集**

- DXGI 桌面复制。**默认只有通道 1（本机主屏）**；别的显示器与 USB / 内置
  摄像头在网页「通道管理」里按 **NVR 方式显式添加**，每一路分配一个稳定通道号
  （删掉别的通道不会让它换号）
- **鼠标指针合成**（不依赖任何第三方库，见[工程实录](docs/ENGINEERING.md)）
- 指定采集哪块屏、哪个摄像头、目标分辨率与帧率

**编码与推流**

- H.264（libx264），**主码流 + 子码流两路独立编码器** —— 同一张 BGRA 进去、
  各自缩放到自己的目标尺寸，不是「编一次再抽帧」也不是「编一次再解码」
- RTSP 推流（Live555），默认 `rtsp://<本机IP>:554/live`（**Digest 鉴权默认开**，
  凭据 = ONVIF 那一对），子码流 `/live_sub`
- **音频**：WASAPI 采集本机声音，AAC 编码，与视频同一路 RTSP 输出
- 浏览器预览：**fMP4 over WebSocket**（MSE 直接播）
- 快照：MJPEG，`GET /snapshot.jpg`
- 录制 MP4（`--record out.mp4 --seconds 10`）

**这是台「真摄像机」**

- **ONVIF Profile S**：SOAP + WS-Discovery（UDP 3702）+ Digest 鉴权。
  通道 1 的端口是**唯一的主设备端点**：`GetProfiles` / `GetStreamUri` /
  `GetSnapshotUri` 聚合全部已启用通道（`main`/`sub`、`ch2_main`/`ch2_sub`…），
  NVR 看到的是一台多通道摄像机。通道 N 另保留 `web.port + N` 作为直连调试
  和旧配置的兼容端口，但不再广播
- **配置热生效**：通道参数在网页上**改完即生效**（重开管线）；水印下一帧就变；
  只有端口/口令这一类系统设置需要点保存，需要重启的**逐字段告诉你**
- **设备身份**（厂商 / 型号 / 硬件码 / 序列号 / UUID）在界面上锁死，
  逃生门是手改 `config.json`
- **设备时间**：支持 NTP 对时，也支持 NVR 反过来给它对时（SetSystemDateAndTime）
- **OSD 水印**：时间戳与名称，**每一路通道各自独立**，改完下一帧就变
- **延时档位**：极致低延迟 / 均衡 / 优先画质，网页预览与 NVR 两侧分别可调
- **DXGI 自恢复**：换分辨率、换刷新率、关显示器电源之后自动重建管线
  （三条触发路径都实机验证过）
- **审计日志**：谁在什么时候登录、改了什么、重启过几次

## 快速开始

### 方式一：安装包（推荐）

当前版本 **v0.3.3**，直接下
[`Desk2ONVIF_Setup_0.3.3.exe`](https://github.com/Antruly/Desk2ONVIF/releases/latest/download/Desk2ONVIF_Setup_0.3.3.exe)
（全部版本见 [Releases](https://github.com/Antruly/Desk2ONVIF/releases)）。
Inno Setup 简体中文向导，**会请求管理员权限（UAC）**，默认装到
`C:\Program Files\Desk2ONVIF`。

向导里的附加任务 **“安装为 Windows 服务（推荐）”默认勾选**：

- **勾选（默认）**：注册成 Windows 服务 `Desk2ONVIF`，`LocalSystem` 账户、
  开机自动启动，崩溃后由 SCM 自动重启。桌面和开始菜单里放的是
  `http://127.0.0.1:8080/` 的 **URL 快捷方式**，双击直接进 Web 控制台；
  开始菜单里另有“服务状态”（等价于 `sc query Desk2ONVIF` /
  `desk2onvif.exe --service status`）、“启动服务”和“停止服务”。
  **要停就点“停止服务”**（或 `desk2onvif.exe --service stop`），它停的是
  Session 0 看门狗；在任务管理器里杀 worker 会被看门狗重新拉起来。
  “停止服务”只停不删，下次开机仍会自启；彻底移除用卸载程序。
- **不勾选**：控制台形态。桌面/开始菜单指向 `start-installed.bat`，
  双击后窗口里能看到日志，**关掉窗口或 Ctrl+C 就停止**。这种形态没有
  守护进程，也不会创建“启动服务/停止服务”两个入口。

两种形态的配置都在
`C:\ProgramData\Desk2ONVIF\config.json`（安装器创建时给 Users 组修改权限，
这样以登录用户身份运行的采集进程才能把网页上保存的配置写回去）。安装目录
（Program Files）本身仍是只读的。

> **从 0.2.0 早期安装包升级**：旧版是**每用户安装**
> （`%LOCALAPPDATA%\Programs\Desk2ONVIF`，不弹 UAC），新版是机器级安装。
> Inno 的 AppId 虽然相同，但安装模式变了，两者**不会被当成同一次安装** ——
> 请先在“应用和功能”里卸载旧版，再运行新版安装包。旧版配置在旧安装目录的
> `config.json` 里，需要的话先抄走。

> **服务采集的是“登录用户”的桌面。** 服务进程跑在 Session 0，但 DXGI 桌面
> 复制必须在交互会话（Session 1）里才能拿到画面，所以服务会把一个 worker
> 进程拉进当前登录用户的会话，采集/推流/Web/ONVIF 都在 worker 里。
> **未登录或锁屏时服务状态仍是 RUNNING，但不会采集**；等用户登录后自动拉起
> worker。想不用服务、在桌面上手动跑，就在安装时取消勾选。

向导里会让你填一对账号口令 —— 留意它其实写了**两个地方**（网页控制台一份、
NVR 一份，互相独立），细节见[工程实录](docs/ENGINEERING.md#安装时的口令到底写进了什么)。

装完之后（默认服务形态）：

1. 双击桌面/开始菜单里的 `Desk2ONVIF`（URL 快捷方式），或直接浏览器打开
   `http://127.0.0.1:8080`
2. 在 NVR 里「添加 IP 通道」，应该能**自动发现**到这台设备；没发现就手动填
   本机 IP + ONVIF 端口

> 首次运行 Windows 防火墙会拦。仓库里带了辅助脚本
> （`packaging\firewall-allow.bat`），或者手动放行。

### 方式二：绿色版

下
[`Desk2ONVIF-0.3.3-win64.zip`](https://github.com/Antruly/Desk2ONVIF/releases/latest/download/Desk2ONVIF-0.3.3-win64.zip)，
解压即用，整个目录拷走就能换机器。跑 `start.ps1`（或 `start-local.bat`）启动。
**绿色版不需要管理员权限**，配置在 exe 旁边的 `config.json`，和服务模式无关。

### 方式三：从源码

见下面的[从源码构建](#从源码构建)。

### 直接拉流看

```powershell
# RTSP **默认要求 Digest 鉴权**，凭据就是 ONVIF 那一对（默认 admin/admin，
# 在网页「系统设置 → ONVIF」里改）。用户名口令都要写进 URL 的 userinfo：
ffplay -rtsp_transport tcp rtsp://admin:admin@127.0.0.1:554/live
ffplay -rtsp_transport tcp rtsp://admin:admin@127.0.0.1:554/live_sub   # 子码流
```

> **NVR 那边填的「摄像机用户名 / 密码」也是这一对** —— 没有第二套 RTSP 账号。
> 口令里如果有 `@ : / % # ?`、空格或中文，要按 URL 百分号编码；`GetStreamUri`
> 报给 NVR 的地址已经替它编好了（`onvif.embed_stream_credentials`，默认开）。
> 未带凭据或凭据错误一律 401；只有在调试时才用 `--no-rtsp-auth` 关掉鉴权。

## ONVIF 协议支持

| 能力 | 实现的操作 |
|---|---|
| 设备管理 | `GetDeviceInformation` `GetCapabilities` `GetServices` `GetServiceCapabilities` `GetEndpointReference` |
| 设备名 / Scope | `GetHostname` `GetScopes` |
| 网络 | `GetNetworkInterfaces` `GetDNS` `GetNetworkProtocols` `GetHostname` `GetNTP` `SetNTP` |
| 系统 | `GetSystemLog`（调试日志尾部）；`GetRelayOutputs` `GetDigitalInputs` 回空列表 |
| 媒体 | `GetProfiles` `GetProfile` `GetStreamUri` `GetSnapshotUri` `GetVideoSources` |
| 视频源 | `GetVideoSourceConfigurations` `SetVideoSourceConfiguration` `GetVideoSourceConfigurationOptions` |
| 编码器 | `GetVideoEncoderConfigurations` `SetVideoEncoderConfiguration` `GetVideoEncoderConfigurationOptions` |
| 音频 | `GetAudioSources` `GetAudioSourceConfigurations` `GetAudioEncoderConfigurations` `GetAudioEncoderConfigurationOptions` `SetAudioEncoderConfiguration`（仅 AAC 码率；音频未启用时 Get 回空列表） |
| OSD | `GetOSDs` `GetOSD` `SetOSD` `CreateOSD` `DeleteOSD` `GetOSDOptions` |
| 时间 | `GetSystemDateAndTime` `SetSystemDateAndTime` |

**明确返回 `ter:ActionNotSupported` 的操作**（不假装成功）：

| 操作 | 原因 |
|---|---|
| `SetHostname` | Windows 主机名不能在运行中改，要重启/系统设置 |
| `SetScopes` `AddScopes` `RemoveScopes` | 没有可持久化的 scope 存储；`GetScopes` 报的是固定列表 |
| `SetNetworkProtocols` | 只接受与当前 HTTP/RTSP 端口/开关完全一致的请求；改端口要重启 |
| `SystemReboot` `SetSystemFactoryDefault` | 默认关闭；只有调用方显式注入回调才执行（测试机不会真的重启/清配置） |

`SetNTP` 写回 `time.ntp_server`；如果报文里附带厂商扩展元素 `<tt:Interval>`
（秒，0 = 只在启动时对一次），也一并写回 `time.ntp_interval_s`。`GetNTP`
只报服务器 —— ONVIF 的 `tt:NTPInformation` 里没有间隔这一项。

发现走 **WS-Discovery**（UDP 3702 组播），鉴权走 **HTTP Digest**
（ONVIF 要求的 WS-UsernameToken 也在）。NVR 那边看到的就是一台普通的
Profile S 摄像机。

多通道下媒体入口聚合在**通道 1 的端口**：一台设备、一个 UUID、一个 XAddrs，
`GetProfiles` 报全部已启用通道。**每通道的视频源 / 编码器 / OSD 配置仍走
各自的兼容端口**（`web.port + N`），聚合它们是下一步（issue #10）。

## Web 控制台

Vue 3 + Vite，按"读者要做的决定"分成独立页面（不再是一条长页面滚动）：

| 页面 | 干什么 |
|---|---|
| 实时预览 | 多画面缩略图墙 + 单路 MSE 实时画面，可切延时档位 |
| 通道管理 | NVR 式：有哪几路、加一路（显示器 / USB 摄像头）、删停启 |
| 通道详情 | 这一路的身份、图像、延时、子码流与 **独立的水印**；改完自动生效 |
| 运行状态 | 码率、帧率、队列、丢帧、连接数、ONVIF 计数 |
| 系统设置 | 端口、登录、自适应、审计留存、重启 / 恢复出厂（需要保存） |
| ONVIF 设置 | 设备名、NVR 凭据、重新广播 |
| 音频设置 | 系统声音 / 麦克风、增益 |
| 时间设置 | 设备时间、NTP、手动校准 |
| 日志与告警 | 操作审计 + 调试日志尾部 |
| 设备信息 | 设备身份、版本、能力 |
| 登录 | 会话鉴权 + 路由守卫 |

前端构建产物**全部内联**，运行时不含任何第三方 JS 依赖 —— 所以
`web/dist` 拷走就能离线跑，不需要 `node_modules`。

## 命令行

`desk2onvif.exe` 是薄薄一层命令行，底层是静态库 `desk2onvif_core`。

```powershell
# 桌面 → RTSP + ONVIF + Web UI。ONVIF 默认就是开的，不用加开关
.\desk2onvif.exe --rtsp --http

# 看看这台机器有哪些输出，哪些真的能录
.\desk2onvif.exe --list-displays

# 看看这台机器有哪些摄像头
.\desk2onvif.exe --list-cameras

# 有哪些音频设备
.\desk2onvif.exe --list-audio

# 摄像头当通道 1（桌面那块屏不再自动变成通道 2；要加就在网页「通道管理」里加）
.\desk2onvif.exe --camera "FHD Camera" --rtsp --http

# 录 10 秒
.\desk2onvif.exe --record out.mp4 --seconds 10

# 极致低延迟档 + 音频
.\desk2onvif.exe --latency realtime --audio --rtsp --http

# 依赖自检
.\desk2onvif.exe --selfcheck
```

常用开关：`--rtsp` `--http` `--onvif` `--display` `--camera` `--audio`
`--latency` `--bitrate` `--fps` `--max-height` `--osd` `--osd-name`
`--record` `--config` `--firewall` `--audit`；每个都有对应的 `--no-*`
可以关掉。完整列表跑 `--help`。

## 架构

```
DXGI 桌面复制 ──► 指针合成 ──► FFmpeg / libx264 ──┬──► Live555 RTSP ──► NVR / VLC / ffplay
   (采集线程)                                  │    (调度线程)   ├─ /live      主码流
                                               │                 └─ /live_sub  子码流
                                               ├──► fMP4 over WebSocket ──► 浏览器预览
                                               └──► MJPEG ──► GET /snapshot.jpg

libuvcpp（HTTP / WebSocket / ONVIF SOAP / WS-Discovery UDP 3702）
```

整条管线**按通道各一份**：一块屏一路，摄像头也能加进来当一路。

源码编成静态库 `desk2onvif_core`，`desk2onvif.exe` 只是薄薄一层命令行 ——
`tests/` 下的冒烟测试直接链库，不用把命令行解析也链进去。

## 从源码构建

需要 **MinGW64 / MSYS2**（GCC 15+）、CMake 3.21+、Ninja，以及 7-Zip
（解 FFmpeg 包用）。**不支持 MSVC** —— `third_party/` 下的依赖全是 MinGW
格式的（`.dll.a`、`.a`）。

```powershell
# 1. 供给依赖（三个都幂等，重复跑无副作用）
.\scripts\fetch_ffmpeg.ps1
.\scripts\build_live555.ps1
.\scripts\fetch_uvcpp.ps1      # 只搬运；库本身要先按 third_party\README.md 编好

# 2. 构建 + 自检
.\scripts\build.ps1
```

> ⚠️ **`build.ps1` 不构建网页。** 它只管 C++；`web\dist` 是 `web\` 目录下
> 另跑一次 `npm run build` 出来的。改完 `.vue` / `.ts` 不重编，程序照跑、
> 页面照开，只是发给你的是**上一次**的界面，而且没有任何地方会说这件事。
>
> ```powershell
> cd web; npm ci; npm run build; cd ..
> ```

`third_party/` 由脚本生成，不进版本库；依赖的各项说明见
[`third_party/README.md`](third_party/README.md)。

**为什么是 MinGW64 而不是 MSVC**：libuvcpp 的 SOAP 模块在所有预编译包里都是
关着的（`UVCPP_WSDL_ENABLE 0`：头文件在，`uvcpp_soap_*` 的符号不在），而
ONVIF 的 SOAP 信封层要靠它，只能从源码编；从源码编就需要一套和 FFmpeg 的
导入库格式对得上的工具链。

## 打包

```powershell
.\scripts\package.ps1 -Zip                  # 安装包 + 绿色版 zip，都进 release\
.\scripts\package.ps1 -NoBuild              # 只用现有产物重新打包
.\scripts\package.ps1 -AppUrl https://...   # 把项目主页写进安装程序信息里
```

产出两个交付物，**装出来的文件完全一样**，只是分发方式不同：

| | 产物 | 说明 |
|---|---|---|
| 安装包 | `release\Desk2ONVIF_Setup_<版本>.exe` | Inno Setup 简体中文向导，**需要管理员权限（UAC）**，默认装成 Windows 服务 |
| 绿色版 | `release\Desk2ONVIF-<版本>-win64.zip` | 解压即用 |

版本号只有 `CMakeLists.txt` 一个来源，由脚本传进 `.iss`。

两个交付物各配一份**脚本化的验收**：

```powershell
.\scripts\verify_installer.ps1     # 真装一遍、真跑起来、真卸干净
.\scripts\verify_portable.ps1      # 解到空目录、走 start.ps1 跑、验完删掉
```

`tests/` 下还有一批协议与端到端探针（自写，不用第三方 ONVIF 库）：
RTSP 拉流、ONVIF SOAP、Web 契约、前端浏览器探针、音频链路、多通道、
延时档位、OSD、NTP、DXGI 自恢复、安装包口令链路等。

## 运行环境

- **Windows 10 / 11 x64**
- 需要能跑 DXGI 桌面复制（Win8+ 的桌面复制 API）
- 分发时整个 `bin\` 目录一起拷走 —— 除 `desk2onvif.exe` 外还有 16 个 DLL
  （FFmpeg、OpenSSL、libuvcpp 及其依赖），构建时自动部署

**Linux / macOS 目前不支持**，但架构上是分层的（`capture/` 是唯一强绑定
Windows 的一层），移植是后续计划，不是"没想过"。

## 许可证

**分两句话，请都读完：**

| | 许可 |
|---|---|
| 本仓库的**源码**（我们自己写的那部分） | **MIT** —— [`LICENSE`](LICENSE) |
| 随 Releases 分发的**可执行文件** | **GPL-3.0-or-later** —— [`LICENSE-GPL-3.0.txt`](LICENSE-GPL-3.0.txt) |

**为什么二进制不是 MIT。** 程序链接 FFmpeg，而随包分发的这个 FFmpeg 构建带
`--enable-gpl --enable-libx264`，其中 **x264 是 GPL-2.0-or-later** —— 一旦
链接进二进制，整个可执行文件就成为 GPL 下的组合作品，这不是我们选得了的。
（Live555 是 LGPL-3.0-or-later 且被静态链接，与 GPLv3 兼容，义务通过
**完整发布源码 + 构建脚本**满足。）

所以：

- 你**读、改、复用**这份源码 → MIT，随便用，署名即可。
- 你**分发**由它构建出的可执行文件 → 受 GPL-3.0 约束，要提供对应源码。
- 想要**纯 MIT 的二进制** → 需要换掉 GPL 版 FFmpeg（改 LGPL 构建）并去掉
  x264（换 openh264 或硬编码器）。这是可做的改造，欢迎 PR。

各依赖的许可证归属与源码地址见 [`packaging/THIRD-PARTY.md`](packaging/THIRD-PARTY.md)。

> ⚠️ 一个诚实的提醒：**MIT 覆盖的是本仓库的源码，不是二进制。** 如果你的
> 用途要求「拿到的东西全是宽松许可」，请自行用 LGPL 版 FFmpeg 重新构建，
> 不要直接分发本项目的 Release 产物。

## 致谢

**主要依赖**

- [**libuvcpp**](https://github.com/Antruly/libuvcpp)（MIT）—— 本项目的
  web 服务层整个跑在它上面：HTTP、WebSocket、ONVIF SOAP 信封、WS-Discovery
  的 UDP 收发都是它的模块。ONVIF 那部分尤其关键 —— 预编译包里 SOAP 模块是
  关着的，只能从源码编，这也是本项目选 MinGW64 的原因。

  > **顺带说一句：有 libuvcpp 打底，写 web 项目这件事会轻松很多。**
  > HTTP 路由、WebSocket 升级、SOAP 信封、UDP 收发这些全在库里现成，于是
  > 「做一个 web 服务」基本退化成**描述清楚需求、然后用它当框架落地**——
  > 本项目那个能跑的 web 服务原型是一天之内出来的，主要功劳在这里，不在
  > 别的什么地方。如果你也在写 C++ 的 web 服务，值得先看它一眼。
- [FFmpeg](https://ffmpeg.org/)（本构建为 GPL-3.0）· [x264](https://code.videolan.org/videolan/x264)（GPL-2.0+）
- [Live555](https://download.live555.com/)（LGPL-3.0+，静态链接）
- [libuv](https://github.com/libuv/libuv) · [llhttp](https://github.com/nodejs/llhttp)（MIT）
- [OpenSSL](https://www.openssl.org/)（Apache-2.0）· [zlib](https://zlib.net/)（Zlib）
- [nlohmann/json](https://github.com/nlohmann/json)（MIT）
- [Vue 3](https://vuejs.org/) + [Vite](https://vitejs.dev/)（MIT）

完整清单、版本与源码获取方式见 [`packaging/THIRD-PARTY.md`](packaging/THIRD-PARTY.md)。

**关于 AI 辅助开发**

这个项目的开发过程里大量使用了 **AI 编程工具**（Claude Code）。第一个能跑的
web 服务原型是**一天之内**做出来的，之后在此基础上迭代了多个版本 ——
采集、RTSP、ONVIF、前端、音频、多通道、OSD、自恢复这些能力是分轮次加上去的，
每一轮都配了脚本化的验收。

把这件事写在这里，是因为它确实影响了这个仓库的样子：**验证脚本的密度远高于
同类项目**（八套验收、变异检验、端到端探针），因为 AI 写的代码更需要被
「和别人对账」，而不是靠读一遍觉得对。`docs/ENGINEERING.md` 里那些踩坑记录
很大程度上就是这么攒出来的。

## 文档

- [**`docs/ENGINEERING.md`**](docs/ENGINEERING.md) —— 工程实录。开发过程中的
  设计取舍、实测读数、以及大量「这里为什么坏过」的记录。想看细节去那里。
- [`packaging/THIRD-PARTY.md`](packaging/THIRD-PARTY.md) —— 第三方组件与许可证
- [`packaging/README-release.md`](packaging/README-release.md) —— 随安装包一起
  发出去的那份 README（含静默安装参数）
- [`third_party/README.md`](third_party/README.md) —— 依赖是怎么供给的

## English

**Desk2ONVIF** turns a Windows desktop into a standard **ONVIF Profile S**
network camera. Hikvision, Dahua, Milestone XProtect, Pelco and other NVRs
discover it over WS-Discovery, add it as an IP channel, and pull H.264 over
RTSP — no hardware encoder needed.

**Download:** [latest release](https://github.com/Antruly/Desk2ONVIF/releases/latest)
— `Desk2ONVIF_Setup_<version>.exe` (installer, no UAC prompt) or
`Desk2ONVIF-<version>-win64.zip` (portable, unzip and run). Both are attached
to the [latest release](https://github.com/Antruly/Desk2ONVIF/releases/latest);
older versions are on the [Releases page](https://github.com/Antruly/Desk2ONVIF/releases).

It is an **open-source take on what [DeskCamera](https://www.deskcamera.com/)
(closed-source, commercial) does**: capture the screen, a webcam, or both,
encode H.264, and present the result as a real ONVIF camera. Not affiliated
with Baltic / DeskCamera in any way.

Highlights: DXGI desktop capture (multi-monitor, one channel per display),
Media Foundation webcam channels, dual independent x264 encoders (main + sub
stream), audio via WASAPI + AAC, browser preview over fMP4/WebSocket, OSD
overlay, NTP time sync, latency presets, hot-reloadable config, DXGI
auto-recovery, audit log, and a Vue 3 web console. The whole web/ONVIF layer
runs on [libuvcpp](https://github.com/Antruly/libuvcpp).

Built with MinGW64 / MSYS2 (MSVC is not supported). Windows 10/11 x64 only
for now; the capture layer is the only Windows-bound part.

**Licensing:** the source in this repository is **MIT**; the binaries on the
Releases page are **GPL-3.0-or-later**, because they link an FFmpeg build
carrying GPL x264. See [许可证](#许可证) for the full explanation.

---

**相关链接**：[Releases](https://github.com/Antruly/Desk2ONVIF/releases) ·
[Issues](https://github.com/Antruly/Desk2ONVIF/issues) ·
[libuvcpp](https://github.com/Antruly/libuvcpp)
