# 更新日志

本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。版本号只有一个来源：
`CMakeLists.txt` 里的 `project(... VERSION x.y.z)`。

> **关于 0.1.0 – 0.1.8**：这些是**公开发布之前**的内部迭代版本，没有对外发过。
> 它们之间没有留下可靠的逐版本记录（这个仓库的历史是从首次开源时开始的），
> 所以不在这里编造条目。需要考古的话，`docs/ENGINEERING.md` 里按「轮次」
> 记了那几轮各做了什么。

## [0.3.0] — 2026-11-26

**服务形态与通道**

- 安装包新增「安装为 Windows 服务（推荐）」并**默认勾选**：注册 `Desk2ONVIF`
  服务（LocalSystem、开机自启），桌面/开始菜单放 `http://127.0.0.1:8080/`
  URL 快捷方式；配置与日志移到 `%ProgramData%\Desk2ONVIF`，旧版每用户安装
  自动迁移并清理旧快捷方式。
- 服务仍采用 **Session 0 看门狗 + Session 1 worker**：DXGI 在 Session 0
  抓不到交互桌面。worker 起不来时 `service.log` 记录会话号、配置路径与错误；
  日志目录按 `%ProgramData%` → exe 目录 → `%TEMP%` 回退，回退路径写进日志。
- 通道 1 来源新增 `capture.source = display|webcam` 与 `capture.camera`
  （空 = 第一台）。服务/无显示输出的机器可以只开 USB 摄像头，不再因为
  “没有找到任何显示输出”而没有 worker；`--camera` 仍是命令行最高优先级。
- ONVIF 改为**单设备多通道**：`web.port` 是唯一主端点，`GetProfiles` 聚合
  全部通道；通道 1 token 保持 `main`/`sub`，通道 N 用 `ch<N>_main`/
  `ch<N>_sub`；WS-Discovery 只宣告一次。旧 `web.port + N` 端口保留为兼容
  入口，不再广播。聚合端点快照走 `/snapshot.jpg[?channel=N]`，NVR 可用
  Digest 直接取图。

**ONVIF**

- 补齐 Profile S 设备/网络/系统查询：`GetScopes`、`GetHostname`、
  `GetNetworkProtocols`、`GetNTP`、`GetSystemLog`、`GetEndpointReference`、
  `GetRelayOutputs`、`GetDigitalInputs`、`GetVideoSourceConfigurationOptions`。
- `SetNTP` 落盘并通知 NTP 客户端；`SetNetworkProtocols` 只接受当前端口/开关，
  改端口明确 `ActionNotSupported`；`SetHostname` / `SetScopes` / `AddScopes`
  / `RemoveScopes` 明确拒绝（没有持久化存储或需要重启）；`SystemReboot` /
  `SetSystemFactoryDefault` 默认拒绝，只有显式注入回调才执行。
- 新增 **WS-UsernameToken**（PasswordDigest、Created、Nonce、±5 分钟窗口、
  nonce 重放缓存），与 HTTP Digest 并存；任一通过即放行。
- 新增 Media2 端点 `/onvif/media2_service`：能力、Profile、StreamUri、
  SnapshotUri、`GetVideoEncoderConfigurations`、
  `GetVideoEncoderConfigurationOptions`。本轮**未实现** Media2 的
  `GetVideoSourceConfigurations` / `GetVideoEncoderConfiguration` /
  `SetVideoEncoderConfiguration`，请求会明确回 `ActionNotSupported`；
  Media1 侧对应的 Set 能力不受影响。
- 新增 Events PullPoint 端点 `/onvif/events_service`：
  `GetEventProperties`、`CreatePullPointSubscription`、`PullMessages`、
  `Renew`、`Unsubscribe`；`PullMessages` 按 Timeout 延迟响应（上限 10 秒，
  不阻塞循环线程），空 Messages 不伪造报警；订阅 TTL 1 秒..24 小时，
  Unsubscribe 幂等。
- 音频：`GetAudioSources` / `GetAudioSourceConfigurations` /
  `GetAudioEncoderConfigurations` / `GetAudioEncoderConfigurationOptions`
  只读；`SetAudioEncoderConfiguration` 只接受 AAC 码率，采样率固定 48 kHz；
  音轨关闭时 Get 回空列表、Set 明确拒绝。

**Web 控制台**

- 参考 uvscope 重做视觉：统一设计 token、品牌标、登录双栏、响应式布局；
  背景改为**轻量网格 + 不超过 50 个粒子**，去掉原来的五层重动画。

**RTSP 鉴权**

- RTSP 默认开启 **Digest 鉴权**，凭据与 ONVIF 共用 `onvif.user` /
  `onvif.password`；未带凭据或凭据错误一律 `401 Unauthorized` +
  `WWW-Authenticate: Digest`，不做匿名降级。
- 新增 `rtsp.auth_enabled`（默认 `true`；老配置缺这个键时也按 `true`）。
  命令行 `--no-rtsp-auth` 可临时关掉（调试/回归用，启动会 WARN）。
- `GetStreamUri` 默认返回**带凭据**的完整地址
  （新配置 `onvif.embed_stream_credentials`，默认 `true`）。userinfo 按
  RFC 3986 做百分号编码，`@ : / % # ? [ ]`、空格、中文都不会把 URL 切坏。
- 网页上改 ONVIF 口令后，RTSP 鉴权库**当场生效**，不需要重启或重开管线。
- 新增 `tests/rtsp_auth_probe.py`（纯标准库），并接进 `rtsp_sub_e2e.ps1`：
  匿名 401、错口令 401、对口令 200 + `m=video`，主/子两路都验。

**依赖**

- libuvcpp 1.6.4 → **1.11.0**（服务化、跨进程 WebSocket 广播、进程监管重启策略、
  作业队列持久化与异步 handler、共享会话等）。服务形态仍由 `src/service/` 的
  **Session 0 看门狗 + Session 1 worker** 负责，没有改用 `uvcpp_service_run()`
  —— 原因见 [`docs/ENGINEERING.md`](docs/ENGINEERING.md) 的「服务形态」一节。

## [0.2.0] — 2026-11-19

**通道**

- 默认安装只有一个通道：**通道 1 = 本机桌面**，绑定本机 BIOS 序列号，不可删除。
- **取消显示器自动发现**：别的屏 / USB 摄像头都要在「通道管理」里显式添加。
- 通道号**稳定**：`number` 在添加时分配，删掉别的通道不会让后面的通道换号；
  空号会被新通道复用（NVR 行为）。
- 每一路新增 `id` / `hardware_id` / `hardware_serial`；USB 摄像头从符号链接里
  读序列号，读不到就显示「未提供」，不编。
- 每一路有**独立的**画质、帧率、码率、分辨率、延时档、子码流与 **OSD 水印**
  （通道 1 仍用顶层 `osd`）。
- 摄像头掉线：会话内部继续每秒重建，期间状态显示「无信号」；启动时就不在的
  摄像头通道每 3 秒重试，插回来自动接上。通道 1 用摄像头时也不再退出进程。
- 改通道参数**点击即生效**（保存 + 重开管线）；只改水印时热生效，不重开。

**Web 控制台**

- 拆掉 4000 行的单页 `/config`，改为独立页面：实时预览（多画面墙）、通道管理、
  通道详情、系统 / ONVIF / 音频 / 时间设置、日志与告警、设备信息。
- 通道详情页没有「保存」按钮：图像参数与水印改完自动应用；系统类设置才需要保存。
- 预览页加了缩略图墙，点任意一路切到实时流；叠加显示该路的实测 fps / 码率。
- 日志页新增「异常与告警」页签（调试日志尾部）。

**接口**

- 新增 `GET /api/channels`、`GET /api/channels/<n>/snapshot.jpg`、
  `GET /api/logs?lines=N`。
- `/api/sources` 的摄像头列表改为**每次请求现枚举**（插上就能加）；显示器仍是
  启动快照。返回项新增 `hardware_id` / `hardware_serial`。
- `/api/status` 与 `/ws/stats` 的 `channels[]` 新增稳定号、身份、目标参数。

**已知边界**

- 新加的通道不会自动出现在 NVR 的「搜索设备」里（WS-Discovery 宣告表在启动时
  固定），需要手动填 ONVIF 地址或重启服务；详情页直接显示了该地址。
- 改通道参数会重开整条管线（所有通道断流约 1–2 秒），不是只断被改的那一路。

详见 [`docs/CHANNELS_REDESIGN.md`](docs/CHANNELS_REDESIGN.md)。

## [0.1.9] — 2026-10-09

**首次公开发布。**

在这一版之前，项目已经过若干轮迭代，功能面覆盖：

**采集**

- DXGI 桌面复制（多显示器，一块屏一路通道，自动发现、默认开）
- Media Foundation 摄像头采集，可作为独立通道与屏幕通道混用
- 鼠标指针合成（无第三方依赖）
- 指定输出、指定分辨率与帧率

**编码与推流**

- libx264 编码，主码流 + 子码流**两路独立编码器**
- Live555 RTSP 推流（`/live`、`/live_sub`）
- WASAPI 音频采集 + AAC 编码，与视频同路输出
- 浏览器预览：fMP4 over WebSocket（MSE）
- MJPEG 快照（`GET /snapshot.jpg`）
- MP4 录制

**ONVIF**

- Profile S：SOAP + HTTP Digest + WS-Discovery（UDP 3702）
- 每个通道一个独立 ONVIF 端口
- 设备管理 / 媒体 / 视频源 / 编码器 / OSD / 时间 / 网络七组操作
- 设备身份（厂商、型号、硬件码、序列号、UUID）在界面上锁死

**Web 控制台（Vue 3）**

- 预览 / 状态 / 配置 / 信息 / 日志 五个页面 + 登录鉴权与路由守卫
- 配置**热生效**：能当场生效的立刻生效，需要重启的逐字段标注
- 前端产物全部内联，运行时无第三方 JS 依赖

**运行期行为**

- OSD 水印（时间戳 / 名称），网页与 NVR 两侧均可改
- NTP 对时，以及对 NVR 的 `SetSystemDateAndTime`
- 三档延时预设（极致低延迟 / 均衡 / 优先画质）
- DXGI 自恢复：换分辨率、换刷新率、显示器断电后重建管线（三条路径实机验证）
- 审计日志

**交付**

- Inno Setup 简体中文安装包（装到 `%LOCALAPPDATA%\Programs\Desk2ONVIF`，不弹 UAC）
- 绿色版 zip（解压即用）
- 两套脚本化验收（`verify_installer.ps1`、`verify_portable.ps1`）
- 修正：安装向导里填的账号口令此前**没有**写进配置（三条独立的断链），
  0.1.7 起修复 —— 0.1.6 及更早的安装包装出来仍是内置的 `admin`/`admin`

**许可证**

- 源码采用 **MIT**，随包分发的二进制因链接 GPL 版 x264 而按 **GPL-3.0-or-later**
  分发。详见 [README](README.md#许可证)

[0.3.0]: https://github.com/Antruly/Desk2ONVIF/releases/tag/v0.3.0
[0.2.0]: https://github.com/Antruly/Desk2ONVIF/releases/tag/v0.2.0
[0.1.9]: https://github.com/Antruly/Desk2ONVIF/releases/tag/v0.1.9
