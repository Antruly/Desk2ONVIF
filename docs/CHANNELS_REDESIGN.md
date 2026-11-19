# 通道模型与前端改版（0.2 方向）

这份文档回答三个问题：**通道现在是什么**、**哪些改动立即生效**、
以及**界面上为什么这样分页**。它描述的是 2026-11 这一轮改动，后续改动
如果推翻了其中任何一条，请连同这里一起改。

---

## 1. 通道模型

### 1.1 只有一个默认通道

全新安装只有 **通道 1：本机桌面（主屏）**。

- 它由顶层的 `capture.*` / `rtsp.*` / `osd.*` 描述，**不在**
  `channels.items[]` 里（兼容边界：老配置文件和启动路径一个字节都不变）。
- 它绑定**本机的硬件序列号**（`HKLM\HARDWARE\DESCRIPTION\System\BIOS` 里的
  `SystemSerialNumber`，读不到时退回 `machine_seed()` 派生的稳定值）。
- 它不可删除；界面上的「通道 1」卡片没有删除按钮。

**不再自动发现显示器。** 旧版会在启动时把每一块可捕获的显示器各建一个
通道，于是"默认只有一个通道"在双屏机器上不成立，而用户从界面上看不出
第二个通道是自己冒出来的。现在，别的屏和摄像头都要在「通道管理」里显式添加。

### 1.2 通道号是稳定的

`ChannelConfig.number`（2 起）在**添加时分配**，此后不随数组顺序变：

- 删掉通道 3 不会让通道 4 变成通道 3；
- 空号会被下一路新通道复用（NVR 的行为）；
- 端口 = `web.port + number`，默认 RTSP 路径 = `/ch<number>`。

`validate_config` 负责把 `number = 0`、越界、重复的号重新分配成当前最小的
空号，并把结果写回配置。

### 1.3 每一路都有身份

| 字段 | 含义 |
|---|---|
| `id` | 稳定通道 ID，由来源硬件标识派生（`make_channel_id`）。只用于显示与对账 |
| `hardware_id` | 绑定的来源硬件：USB 摄像头是完整符号链接，显示器是 `display:<device>:<adapter>` |
| `hardware_serial` | 给人看的序列号。USB 摄像头取符号链接里的实例段（含 `&` 的端口路径不算序列号）；本机通道取 BIOS 序列号。取不到时**留空**，界面显示「未提供」 |

### 1.4 每一路的视频参数、水印、延时都是独立的

`ChannelConfig` 里有：

- `fps` / `bitrate_kbps` / `max_height` / `quality`（画质档位展开器）
- `latency` / `gop_seconds` / `threads`（延时档位展开器）
- `sub_enabled` / `sub_height` / `sub_bitrate_kbps`
- `osd`（**这一路自己的水印**；通道 1 用顶层 `osd`）

ONVIF 的 `GetProfiles` / `GetOSDs` 对通道 2 起报的是这一份，不再是通道 1 的。

### 1.5 掉线 = 无信号，不是失败退出

- **摄像头**：采集会话内部本来就会重试重建（`session_down` → 每秒
  `reopen()`），期间推最后一帧。现在 `SessionStats::source_offline` 把这个
  状态报给通道列表，界面显示「无信号」，并且**不画那张冻住的最后一帧**。
  - 如果摄像头在**会话启动时**就不在，`run()` 返回 Failed；通道 2 起会进入
    「等待设备接入」，每 3 秒重试一次；通道 1 用摄像头时同样重试（不退出进程）。
- **显示器**：换分辨率/断电由会话内部的 `Restart` 处理；启动时就不存在的
  屏会在规划阶段被挡住（`blocked_reason`），不会建出一路永远黑屏的通道。
- **同一台设备只能被一路占用**：一块屏只有一个 DXGI duplication，一个摄像头
  只有一个 reader。`plan_channels` 会把重复的显式项挡住并写明原因；添加对话框
  里也会把已被占用的来源标出来（`已被 CH N 使用`）。

---

## 2. 哪些改动立即生效

| 改动 | 生效方式 | 界面 |
|---|---|---|
| 视频参数（帧率、码率、分辨率、子码流、延时档） | 保存 + `POST /api/reload`（重开管线，所有通道断流约 1–2 秒） | 改完自动应用，无保存按钮 |
| 每一路的水印 | **热生效**（`LiveOsd`，下一帧就变） | 改完自动应用 |
| 添加 / 删除 / 停用通道 | 保存 + `POST /api/reload` | 点按钮即应用 |
| ONVIF 身份与凭据 | 热生效（`apply_onvif_identity`） | 保存按钮 |
| 时间设置（手动校准 / NTP） | 热生效 | 手动动作即时；NTP 配置保存 |
| Web 端口、监听地址 | **要重启进程** | 保存后逐项列出待重启字段 |
| 音频 | 保存 + `POST /api/reload` | 「保存并应用」 |

**为什么通道参数要重开管线而不是就地改编码器。** 编码器的 SPS/PPS、MP4 的
轨道头、SDP 里的 `sprop-parameter-sets` 都绑死在创建时的尺寸上。就地换会得到
一条解不出来的流。重开管线是唯一确定正确的做法；代价是共享的 RTSP 服务器
也会重开，所以**当前是所有通道一起断 1–2 秒**，不是只断被改的那一路。
这一点在界面上如实写出，不包装成"只影响这一路"。

### 2.1 后端如何判断"只改了水印"

`apply_config` 用 `channels_media_equal()` 比较新旧 `channels.items` 的
**除 `osd` 之外**的字段：

- 只有 `osd` 变了 → 运行侧直接跟上（`running_cfg.channels = next.channels`），
  不进 `pending`，前端也不触发 reload；
- 其它任何字段变了 → `channels.items` 进 `pending`，前端随后 `POST /api/reload`；
  采集循环每一趟重算计划，并在开跑时把 `running_cfg.channels` 跟上，pending 清空。

> ⚠️ 加通道字段时要同时改三处：`channels_json_node`（落盘/比较）、
> `channel_media_equal`（是否热生效）、`types.ts` 的 `ChannelConfig`。

---

## 3. 后端接口

| 接口 | 用途 |
|---|---|
| `GET /api/channels` | 全部通道的统一视图（配置 + 运行状态），与 `/ws/stats` 的 `channels` 同源 |
| `GET /api/channels/<n>/snapshot.jpg` | 按通道取缩略图；没帧时 404（界面显示「无画面」） |
| `GET /api/sources` | 可添加的来源。显示器是启动快照；**摄像头每次请求现枚举** |
| `GET /api/logs?lines=N` | 调试日志尾部（「异常与告警」页） |
| `POST /api/config` | 局部补丁，合并到当前生效配置后落盘 |
| `POST /api/reload` | 重开采集/推流管线（202，异步） |

增删改通道**复用** `POST /api/config` + `POST /api/reload`：`config_from_json`
本身就是"以当前值为底合并"，前端发 `{"channels":{"items":[...]}}` 这种整数组
替换即可，不需要为增删改各开一条专用路由。

### 3.1 已知边界：新通道的 WS-Discovery

WS-Discovery 的宣告表（`WebOptions::wsd_extra_endpoints`）在 `web.start()` 时
固定。**通过网页新加的通道，ONVIF 端点会立刻起来，但不会自动出现在 NVR 的
"搜索设备"结果里**，直到进程重启或用户在 NVR 里手动填地址：

```
http://<本机IP>:<web.port + 通道号>/onvif/device_service
```

详情页把这一行直接显示出来了。要把新通道也纳入自动发现，需要给 `WsdResponder`
加一个"更新 extra 表"的入口（`WsdOptions::extra` 现在是只读副本）。

---

## 4. 前端页面

| 路由 | 页面 | 读者要做的决定 |
|---|---|---|
| `/` | 实时预览 | 看哪一路（多画面墙 + 单路 MSE） |
| `/channels` | 通道管理 | 有哪几路、加一路、删一路、停/启 |
| `/channels/:n` | 通道详情 | 这一路的图像、延时、子码流、水印 |
| `/settings/system` | 系统设置 | 端口、登录、自适应、审计、重启/恢复出厂 |
| `/settings/onvif` | ONVIF 设置 | NVR 看到的名字与凭据、重新广播 |
| `/settings/audio` | 音频设置 | 系统声音/麦克风、增益 |
| `/settings/time` | 时间设置 | 设备时间、NTP、手动校准 |
| `/logs` | 日志与告警 | 谁改了什么（审计）/ 机器出了什么事（调试日志） |
| `/info` | 设备信息 | 抄给 NVR 的身份与地址 |

旧版那个 4000 行的 `/config`（锚点滚动）已经删除。`/config` 会重定向到
`/channels`，旧书签不会落到空白页。

### 4.1 全局状态

- `composables/useConfig.ts`：全站共用一份 `AppConfig` 副本；
  `save()` 只落盘，`applyMedia()` 落盘 + reload。
- `composables/useStats.ts`：`/ws/stats` 的单例订阅，`channels` 数组现在
  带 `number` / `id` / `hardware_*` / 目标参数 / `state`（来源掉线时是
  `无信号…`）。
