# 更新日志

本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。版本号只有一个来源：
`CMakeLists.txt` 里的 `project(... VERSION x.y.z)`。

> **关于 0.1.0 – 0.1.8**：这些是**公开发布之前**的内部迭代版本，没有对外发过。
> 它们之间没有留下可靠的逐版本记录（这个仓库的历史是从首次开源时开始的），
> 所以不在这里编造条目。需要考古的话，`docs/ENGINEERING.md` 里按「轮次」
> 记了那几轮各做了什么。

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

[0.1.9]: https://github.com/Antruly/Desk2ONVIF/releases/tag/v0.1.9
