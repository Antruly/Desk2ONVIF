/**
 * 后端 API 的数据契约，与 src/web/web_server.cpp 推出来的 JSON 一一对应。
 *
 * 这里把统计字段写成必填，但**不能**据此认为它们一定会到 —— 后端在连接建立
 * 的第一帧可能还没凑齐所有字段。取值一律走 normalizeStats() 兜默认值，
 * 直接把 JSON 断言成 Stats 用，会在界面上印出 undefined / NaN。
 */

/** 一帧统计快照：GET /api/status 的响应体，也是 /ws/stats 每秒一帧的内容。 */
export interface Stats {
  /** 会话已运行秒数 */
  elapsed_s: number
  /** 主循环轮次 */
  ticks: number
  /** 画面真正变化的帧数（DXGI 只在这个条件下才交出帧） */
  content_frames: number
  /** 编码出的包数 */
  packets: number
  keyframes: number
  /** 累计字节数。注意是累计量不是速率，码率得自己按差值算 */
  bytes: number
  /** 有效帧率 */
  effective_fps: number
  /** 队列丢弃（drop-oldest 溢出） */
  queue_dropped: number
  /** 缓冲池耗尽次数 */
  pool_exhausted: number
  /** DXGI 会话重建次数 */
  access_lost: number

  // ---- 自适应降载与性能提示 ------------------------------------------------
  //
  // `queue_dropped` 是**累计**的，它只回答"总共丢过多少"。判断"现在还好吗"
  // 只能靠 drops_recent 和 perf_state —— 一个四位数的累计丢弃多半只是启动
  // 那一下造成的。

  /** 自适应主动丢掉的拍数。和 queue_dropped 分开计：一个是"我们故意降了帧率"，另一个是"编码器出问题了" */
  adaptive_dropped: number
  /** 桌面没动、只有水印文字变了而额外产出的帧数。恒为 0 而水印开着 = 静止桌面上秒数冻住了 */
  osd_refresh_frames: number
  /** 最近 10 秒的丢弃数。**这个会回到 0**，累计量不会 */
  drops_recent: number
  /** `ok` / `throttled`（已自动丢帧） / `overloaded`（丢到下限还不够） */
  perf_state: string
  /** 每几拍出一帧，1 = 不丢 */
  perf_duty: number
  /** 当前实际目标帧率（含自适应的结果） */
  perf_target_fps: number
  /** 给人看的中文提示，**由后端拼好**，前端直接显示 */
  perf_hint: string
  /**
   * 建议降到哪一档（`high`/`balanced`/`smooth`/`verylow`），空串 = 没有建议。
   *
   * 只在 `perf_state === 'overloaded'` 时才有值，且已经在最低档时也会是空串
   * （那时没有更低的可降）。前端据此决定横幅上有没有那个按钮。
   */
  perf_suggested_quality: string

  /**
   * 下面四个是**所有路合起来**的数（与只有一路时完全等价），为的是不破坏
   * 老的界面字段。要分路看就用 rtsp_streams。
   */
  nalu_dropped: number
  /** 各路里最大的那个 NALU。**不是求和** —— 它回答的是"有没有帧大到有截断风险" */
  max_nalu_bytes: number
  /** 当前 RTSP 观众数（各路之和） */
  rtsp_clients: number
  /** 主码流的地址，形如 rtsp://192.168.2.90:554/live；未开推流时是空串 */
  rtsp_url: string
  /** 主码流编码器的**实际**尺寸。采集还没起来时是 0。
   *
   * 与配置里的 `capture.max_height` 不是一回事：那个是上限（0 = 桌面原生），
   * 而桌面比上限小时不放大。要回答"现在推的到底是多大的画面"，只有这个数
   * 答得上来 —— 设备信息页显示的也是它。
   */
  video_width: number
  video_height: number
  /** 子码流编码器的实际尺寸。0 = 没开子码流 */
  sub_width: number
  sub_height: number
  /** 逐路明细，下标和 RTSP 的路一一对应：0 主、1 子。未开推流时是空数组 */
  rtsp_streams: RtspStream[]
  // ---- 音频 ---------------------------------------------------------------

  /**
   * `off` / `ok` / `nosignal` / `error`。
   *
   * **四档不能合成一个布尔值**：`off` 是默认状态而不是故障；`nosignal` 在
   * 环回（系统声音）这条路上**本来就是正常的** —— 系统不出声时环回一个包都
   * 不来（陷阱 81），所以它不是错误、只是"这一刻没有声音"。
   */
  audio_state: string
  /** 峰值电平 0..1（已过释放），左右分开 —— 混成一个数就看不出单声道接线问题 */
  audio_level_l: number
  audio_level_r: number
  /** 音轨已有的样本帧数。除以 audio_sample_rate 就是音轨时长 */
  audio_frames: number
  /** 采样率（48000）。0 = 音频没开 —— 这时别拿 audio_frames 去除 */
  audio_sample_rate: number
  /** 编出来的 AAC 包数。和 audio_frames 对不上就是编码器那条路的问题 */
  audio_packets: number
  /**
   * 其中由我们补零的帧数。
   *
   * **它涨不等于故障** —— 系统安静时环回不供数据，全靠补零把时间轴撑住
   * （这正是音轨不会因为"没人出声"而变短的原因）。它**一直不涨**而电平也是
   * 0 才说明源那边真的没在给数据。
   */
  audio_zero_filled: number

  /** 鼠标合成过的帧数 */
  cursor_frames: number
  /** 其中"只因为鼠标动了才产出"的帧数。桌面静止时靠的就是它们 */
  pointer_only_frames: number
  web_port: number
  /**
   * 软件版本号，形如 `"0.1.0"`。
   *
   * 它是**后端编译进去的那个数**（`project(... VERSION)`），和 exe 的
   * 文件属性、安装包的文件名同一个来源。界面上显示它是因为除此之外唯一的
   * 做法是让人去右键 exe 看属性 —— 而"我装的到底是哪个版本"正是出问题时
   * 第一个要回答的问题。
   *
   * 放在 stats 里而不是单开一个接口：它和 web_port 一样是"这份状态属于谁"
   * 的一部分，`/api/status` 与 `/ws/stats` 两条路都过 `patch_live_counters`，
   * 于是两条路自动都有。
   */
  version: string
  /** 当前统计推送的订阅者数 */
  ws_clients: number

  // ---- ONVIF / WS-Discovery ------------------------------------------------
  //
  // 这几个数放在状态页是刻意的：NVR「添加失败」的现场，第一个要看的就是
  // "它到底有没有发过 Probe"和"SOAP 有没有被拒" —— 没有这几个数就只能抓包。

  /** WS-Discovery 是否在监听（3702 被别的软件占了、或者没网卡时为 false） */
  wsd_running: boolean
  /** 收到的 Probe 数。NVR 点「搜索设备」一次就会涨几个 */
  wsd_probes_seen: number
  /**
   * 回出去的 ProbeMatch 数。
   *
   * **它比 seen 少是正常的**，不是回包发不出去：后端只对"类型匹配 +
   * scope 有交集"的探针回包（`wsd.cpp` 里 type_ok / scope 两道过滤都在
   * `seen++` 之后），局域网里别的设备问别的类型时这个差就会涨。
   * 真正有诊断价值的是 `seen` 本身 —— 它是 0 说明根本没有谁来问过。
   */
  wsd_probes_answered: number
  onvif_requests: number
  /** Digest 鉴权失败数。NVR 里填错口令就是这个在涨 */
  onvif_auth_failures: number
  /** 返回 SOAP Fault 的次数 */
  onvif_faults: number
  /** 最近一个「没实现」的操作名。非空说明 NVR 调了我们没做的接口 */
  onvif_last_unsupported: string

  // ---- 预览通道 -----------------------------------------------------------

  /** 当前 /ws/preview 的观看者 */
  preview_clients: number
  /** 初始化段是否已经产生。false = 还没编码出第一帧，预览连上也看不到东西 */
  preview_ready: boolean
  /** 产出的 fMP4 分片总数（含没人看的） */
  preview_fragments: number
  /** 其中"当时没有观看者"的部分。**没人看是常态，不是故障** */
  preview_fragments_unwatched: number
  /** 因积压超限被断开的客户端数。**这个才是真的丢数据** */
  preview_clients_kicked: number

  // ---- 时间 ---------------------------------------------------------------
  //
  // 这六个字段描述的是**设备时间**（= 主机时间 + 偏移，也就是水印上和 ONVIF
  // 报出去的那个时间），不是这台机器的系统时钟。两者可以差出几小时，那是
  // 选定的语义而不是故障（见 README 里"设备时间可以与系统时间不一致"）。
  //
  // 放在 stats 里而不是只挂在 `/api/time` 上，理由和 `version` 一样：状态页
  // 顶部要常显"与主机相差多少"，而 `/api/status` 与 `/ws/stats` 两条路都过
  // `patch_live_counters`，于是两条路自动都有、也不会有谁漏掉一个接口。

  /** `"manual"` 或 `"ntp"`。**它决定 ONVIF 的 `GetSystemDateAndTime` 报什么** */
  time_mode: string
  /**
   * 设备时间 − 主机时间，**整秒**（可负）。
   *
   * 亚秒部分在 NTP 那条路上会被丢掉 —— 水印是秒级显示，没有影响，但别拿
   * 这个数当精密对时的读数。
   */
  device_offset_s: number
  /**
   * 这个钟**被改过没有**（手动校准 / NVR 对时 / NTP 对时都算）。
   *
   * 它答的不是"现在准不准"，而是"有没有人动过它"。想判断准不准要看
   * `device_offset_s` —— 把这两个混起来，界面上就会出现"已同步"配一个
   * 三小时的偏移。
   */
  time_synced: boolean
  /** 最近一次对时**尝试**的主机时刻（Unix 秒）。0 = 到现在还没试过 */
  last_sync_at: number
  /**
   * 最近一次对时的结果：`"ok"` / `"timeout"` / `"bad_reply"` / `"unreachable"`。
   *
   * **四档不能合成一个布尔值。** UDP 到 123 在没有网络的环境里会被防火墙
   * 静默丢弃（UDP 没有 RST），于是"发出去成功"和"服务器没理我"看起来一模
   * 一样 —— 那正是这个字段存在的理由。空串 = 还没试过，与"失败"是两回事。
   */
  last_sync_state: string
  /** 失败时的人话。成功时是空串（不是"无错误"之类的占位文字） */
  last_sync_error: string

  /**
   * 每一路通道一行：通道 1 在最前，其后按通道号。
   *
   * **只有一路时它也有一行**（不是空数组）—— 界面上那张表因此不必为"单通道"
   * 单开一种画法，而"多通道"和"单通道"的差别就只剩行数。老后端不发这个字段，
   * 所以是可选的：`normalizeStats` 会把它兜成空数组。
   */
  channels?: ChannelStat[]
}

/** `Stats.channels` 里的一行 = 一路通道此刻的样子。 */
export interface ChannelStat {
  /** 从 1 起。**1 = 主屏那一路**，它由顶层 `capture.*` 描述 */
  index: number
  /** 显示名，如「通道 1」「显示器 #2」。后端已经拼好，前端直接显示 */
  name: string
  /** `display` / `webcam` */
  source: string
  /**
   * 一句人话的状态：`采集中` / `运行中` / `已停止` / `已关闭` / `未启用：…`。
   *
   * **不是枚举** —— 「未启用」后面跟着的是**为什么建不了**的整句（同一块屏被
   * 前面某一路占了），前端原样显示。想判"能不能用"就看它是不是以 `未启用`
   * 开头，别去匹配整句。
   */
  state: string
  fps: number
  bitrate_kbps: number
  /** 正在录的那个文件（基准名，分段是它加序号）。空串 = 这一路没在录 */
  record_file: string
  /**
   * 这个通道**所有分段**加起来的字节数。不是当前那一个文件的大小。
   *
   * 后端算的是"基准名 + `.N.mp4`"这一族的总和，因为它回答的是"这路占了我多少
   * 盘"，而换分辨率会让文件分段、总量并不因此变少。读不到的文件按 0 计。
   */
  record_bytes: number
  /**
   * 这一路 ONVIF 端点所在的端口。**通道 1 在 `web.port` 本身，通道 N 在
   * `web.port + N`** —— 这个数由后端给，前端不要自己加：本机 `web.port` 未必
   * 是 8080（命令行可以改），而"界面上写的端口连不上"是最难查的一类错。
   */
  web_port: number
  /** 主码流的 RTSP 路径，**带前导斜杠**（`/live`、`/ch2`）。空串 = 没开 RTSP */
  rtsp_path: string
}

/**
 * `GET /api/session` 的响应。**匿名可达** —— 前端进任何页面前先问这一句。
 */
export interface SessionInfo {
  /** 这台机器上是否启用了 Web 登录。false 时下面那个字段恒为 true 的语义 */
  auth_required: boolean
  authenticated: boolean
  /** 只在已登录时出现。未登录时后端**故意**不报用户名 */
  user?: string
}

/** `POST /api/login` 的响应。 */
export interface LoginResult {
  ok: boolean
  user?: string
  expires_in_s?: number
  /** 失败原因，直接是给人看的一句话 */
  error?: string
  /** 429 时才有：还要等多少秒 */
  retry_after_s?: number
}

/** 一路 RTSP 码流。 */
export interface RtspStream {
  /** 不含前导斜杠，如 live / live_sub */
  path: string
  url: string
  clients: number
  nalu_dropped: number
  max_nalu_bytes: number
  packets: number
  bytes: number
}

/** WS 上收到的东西在信任之前都是未知形状。 */
export type StatsMessage = Partial<Record<keyof Stats, unknown>>

export interface CaptureConfig {
  display_index: number
  fps: number
  bitrate_kbps: number
  gop_seconds: number
  /** <= 0 表示按"延迟优先"自动选 */
  threads: number
  preset: string
  tune: string
  profile: string
  /**
   * 画质档位的 key：`high` / `balanced` / `smooth` / `verylow` / `custom`。
   *
   * **它不是"另一份 fps/码率"，而是上面那几个值的来源标签。** 后端在
   * `validate_config` 里按档位表把 fps / bitrate_kbps / max_height / sub_enabled
   * **重写**一遍，所以 GET 回来的那四个数永远是具体值，这个字段只回答
   * "这套值是哪来的"。用户手改过其中任何一项，它就变成 `custom`。
   */
  quality: string
  /**
   * 延时档位的 key：`realtime` / `balanced` / `quality` / `custom`。
   *
   * 与 `quality`（画质档位）**正交**：两者展开的字段集合不相交 —— 画质档只写
   * fps / bitrate_kbps / max_height / sub_enabled，延时档只写 `threads` 与
   * `gop_seconds`。所以「流畅 + 极致低延迟」这种组合天然合法，不需要优先级规则。
   *
   * 与 `quality` 一样，它是**来源标签**而不是另一份值：GET 回来的
   * `threads` / `gop_seconds` 永远是具体数，这个字段只回答"这套值是哪来的"。
   * 用户手改过其中任何一项，它就变成 `custom`。
   *
   * ⚠️ **三档都不动 `preset` / `tune` / `profile`。** 那不是保守：`preset` 一放松，
   * x264 就会把 H.264 profile 从 Constrained Baseline 换成 Main，而 NVR 兼容性
   * 不在这个档位表上。判据是"这一档换来的变化，必须能只看 `threads` 和
   * `gop_seconds` 两个数就解释得了"。
   */
  latency: string
  /**
   * 主码流的编码高度上限，**0 = 桌面原生**。
   *
   * 只写上限：桌面比它小就不放大；宽度按桌面宽高比推，并保证是偶数
   * （x264 对奇数边长是直接拒绝的，不是降级）。
   */
  max_height: number
}

export interface RtspConfig {
  enabled: boolean
  port: number
  path: string
  /**
   * 子码流：第二路编码器，给 NVR 的多画面网格用。**只有 rtsp.enabled 为真
   * 时才有意义** —— 它的唯一出口就是 RTSP。
   */
  sub_enabled: boolean
  sub_path: string
  /** 按高度给，宽度按主码流的宽高比推 */
  sub_height: number
  sub_bitrate_kbps: number
}

export interface WebConfig {
  enabled: boolean
  host: string
  port: number
  static_root: string
  /**
   * Web 控制台的登录开关。**默认 true** —— 关掉的人得是明确关掉它的。
   *
   * 和 ONVIF 那一份凭据**故意分开**：NVR 里存着的那份是给机器用的（改一次要
   * 去每台 NVR 上改），网页这份是人手敲的。共用的后果是"为了改网页口令，得把
   * NVR 全部重配一遍"，于是没人会去改它。
   */
  auth_enabled: boolean
  /**
   * 登录用户名。**注意线上的键名是 `user` / `password`，不是 `auth_user`** ——
   * C++ 那边 `WebOptions::auth_user` 是给运行期用的另一份拷贝，写进 JSON 的
   * 是 `AppConfig::web.user`。类型名跟着线上格式走，否则改前端的人会照着自己的
   * 类型去猜后端字段，而猜错的表现是输入框永远空白、且一句报错都没有。
   */
  user: string
  /**
   * GET 回来时是哨兵 `__unchanged__`，POST 回去等于"不改"。真口令永远到不了
   * 浏览器 —— 见 ConfigView 里对哨兵的说明。
   */
  password: string
}

export interface OnvifConfig {
  enabled: boolean
  user: string
  /**
   * GET 回来时是哨兵 "__unchanged__"，POST 回去等于"不改"。
   * 界面上的输入框永远是空的 —— 见 ConfigView 里对哨兵的说明。
   */
  password: string
  manufacturer: string
  model: string
  firmware_version: string
  /**
   * 以下三个是**只读**的（`硬件 ID` / `序列号` / `设备 UUID`）。
   *
   * 它们在网页上渲染成禁用输入框，后端 `apply_config` 里还有一道无条件的
   * 回灌 —— 就算有人绕过前端手搓一个 POST 也改不动。逃生门是直接编辑
   * `config.json`。理由：NVR 拿 UUID 当设备主键，这三个值一变，NVR 那边
   * 就是"多出一台新摄像机"、原来那台永远离线。
   *
   * 字段仍然在这份类型里、仍然随表单往返：它们**要能被读出来抄进 NVR**，
   * 而且原样 POST 回去正好等于"不改"。
   */
  hardware_id: string
  serial_number: string
  device_uuid: string
  device_name: string
  location: string
}

export interface AudioConfig {
  /**
   * 音频总开关。**默认关**（后端 `AudioConfig::enabled = false`）。
   *
   * 打开它会**改变每一台设备的 SDP**，而真实 NVR 收不收只能实测。所以界面上
   * 打开这一项时要提示一句"若 NVR 取流异常，先关掉音频试试"。
   */
  enabled: boolean
  /** 系统声音（WASAPI 环回）。与 `microphone` 独立，可以同时开。 */
  system: boolean
  microphone: boolean
  /**
   * 麦克风设备 id（`GET /api/audio/devices` 给的 `capture[].id`）。
   * **空串 = 系统默认端点** —— 设备被拔掉之后"默认"会跟着系统走。
   */
  device: string
  /** AAC 码率。不占视频的码率预算，两路各自独立编码。 */
  bitrate_kbps: number
  mic_gain: number
  system_gain: number
}

/** `GET /api/audio/devices` 里的一台设备。 */
export interface AudioDevice {
  /** WASAPI 端点 id，原样填进 `audio.device`。 */
  id: string
  /** 系统里显示的名字。虚拟声卡常常只填了 DeviceDesc —— 后端已经替我们兜过底。 */
  name: string
  /**
   * 是不是系统当前的默认端点。
   *
   * **不靠列表顺序判断** —— `EnumAudioEndpoints` 的顺序没有任何承诺，而"默认"
   * 是用户在系统设置里亲手选的那一个。后端为此专门再问了一次
   * `GetDefaultAudioEndpoint`，这个字段就是那个答案。
   */
  default: boolean
}

/**
 * `GET /api/audio/devices` 的回应。
 *
 * 两个方向**分开列**，和 `--list-audio` 是同一张表、同一个理由：「系统声音」只能
 * 从渲染端点环回，麦克风是采集端点。混成一栏的话，用户会在麦克风那里选到一个
 * 扬声器，然后得到一条永远静音的音轨 —— 而 SDP 和 ffprobe 看上去都正常。
 */
export interface AudioDevices {
  /** 采集端点 = 麦克风。`audio.device` 的取值只能来自这一组。 */
  capture: AudioDevice[]
  /** 渲染端点。界面用不到（「系统声音」用的是系统默认渲染端点，选不了）。 */
  render: AudioDevice[]
  /**
   * 枚举采集端点时出的错。**只在真出错时才出现**（COM 起不来、没权限），
   * 恒存在的话前端就得靠"空串算不算有错"来判断 —— 那是两边各写一次的口头约定。
   *
   * 它必须显示出来：只回一个空数组的话，"这台机器上没插麦克风"和"枚举坏了"
   * 在下拉框里长得一模一样。
   */
  capture_error?: string
  render_error?: string
}

/** 水印的一项（`osd.items[]`）。后端 `OsdItemConfig`。 */
export interface OsdItemConfig {
  /**
   * 这一项**画不画**。与"删掉这一项"是两件事：关掉的项留在列表里，
   * 以后还能再打开；删掉就没了。用户明确要的是两者都有。
   */
  enabled: boolean
  /** `time` | `name` | `text`。`name` 的内容跟随设备名称，不可手填。 */
  type: string
  /** 仅 `type == 'text'` 时是用户填的内容。 */
  text: string
  /**
   * `upper_left` | `upper_right` | `lower_left` | `lower_right` | `custom`。
   * `custom` 时用 `x_pct` / `y_pct`。
   */
  position: string
  /**
   * 自定义位置的百分比（0..100）。**指的是文字左上角**，且以**编码尺寸**
   * （`stats.video_width/height`）为基准、没有边距 —— 与
   * `osd_overlay.cpp` 的 `anchor()` 里 custom 那一支逐字对应。
   */
  x_pct: number
  y_pct: number
  /** 0 = 自动（帧高 / 32，下限 16 px）。 */
  font_size: number
  /** `strftime` 格式，仅 `time` 类型用。 */
  date_format: string
  time_format: string
}

export interface OsdConfig {
  /** 水印总开关。关掉之后一个像素都不画。 */
  enabled: boolean
  /** 按水印所在位置的平均亮度自动选前景色（暗底白字 / 亮底黑字）。 */
  adaptive_color: boolean
  /** 反色描边，保证任何背景上都读得出来。 */
  outline: boolean
  /** 底板不透明度 0..255，0 = 不画底板。 */
  plate_alpha: number
  /**
   * 最多 4 项（后端 `OsdConfig::kMaxItems`）。**整体替换**：POST 回来的是整份
   * 列表，不是逐项合并 —— 所以"删掉了一项"不会在下一份配置里又冒出来。
   */
  items: OsdItemConfig[]
}

/**
 * 一路附加通道（`channels.items[]`）。后端 `ChannelConfig`。
 *
 * **通道 1 不在这里** —— 它由顶层 `capture.*` / `rtsp.*` 描述。数组里第 i 项
 * 就是**通道 i+2**，这条恒等式在前后端都是硬约定（后端 `validate_config` 按它
 * 补名字和默认路径）。所以**删掉中间一项会让后面每一项都换号**：第 3 项删掉，
 * 原来的第 4 项（`/ch4`）就成了第 3 项（`/ch3`），而 NVR 上那个书签是按 `/ch4`
 * 存的 —— 界面上删条目之前要把这句话说给用户听。
 *
 * 后端那边**去重丢项时不重编号**（保住 `items[i] ↔ 通道 i+2`），但那是"配置里
 * 有一项是坏的"这条路上的事；界面上主动删一项，后面确实会整体前移。
 */
export interface ChannelConfig {
  /** 关掉的项留在列表里，但这一路不建。**与"删掉这一项"是两回事** */
  enabled: boolean
  /** 显示名。空 → 后端按序号补「通道 N」 */
  name: string
  /** `display` = 抓一块屏；`webcam` = 抓本机摄像头。决定另外两个字段哪个有用 */
  source: string
  /** `source === 'display'` 时抓哪一块（`/api/sources` 里那个 `index`） */
  display_index: number
  /**
   * `source === 'webcam'` 时的设备。**空 = 第一台**（也是推荐值）。
   *
   * 存的是符号链接或友好名，而摄像头拔了再插链接就会变 —— 写死之后的表现是
   * "设备不存在"，而"第一台"跟着系统走正是想要的。所以界面上默认留空。
   */
  camera: string
  /** 画质档位标签（`quality_tiers` 里的 key，或 `custom`） */
  quality: string
  fps: number
  bitrate_kbps: number
  /** 0 = 源的原生尺寸（不缩放） */
  max_height: number
  sub_enabled: boolean
  sub_height: number
  sub_bitrate_kbps: number
  /** 空 → 后端补 `/chN` */
  rtsp_path: string
  /** 空 → 后端补 `/chN_sub` */
  rtsp_sub_path: string
}

/**
 * 多通道配置。后端 `ChannelsConfig`。
 *
 * ⚠️ **`items` 为空时 `GET /api/config` 里连 `channels` 这个键都没有** ——
 * 后端是故意的（`config.cpp:678`：键的存在与否会被 `diff_config` 比较，加一个
 * 恒存在的空数组会让每一台已装的机器在下次启动时报"配置变了"）。所以读的时候
 * 一律 `config.channels?.items ?? []`，写的时候**空列表也要照发**
 * `{"channels":{"items":[]}}`（那是"把通道全删了"这个意图的唯一表达）。
 */
export interface ChannelsConfig {
  items: ChannelConfig[]
}

/** 性能不足时的自适应降载。后端 `AdaptiveConfig`。 */
export interface AdaptiveConfig {
  /** `off` | `throttle` | `auto`。 */
  mode: string
  /** 降载的帧率下限。 */
  min_fps: number
}

/**
 * 时间设置。后端 `TimeConfig`。
 *
 * **这一整组管的都是"设备时间"**（= 主机时间 + `offset_s`，也就是水印上和 ONVIF
 * 报出去的那个），不是这台机器的 Windows 系统时钟 —— 后者要管理员权限、会连累
 * 这台机器上别的程序，而且那是用户的钟不是我们的。
 */
export interface TimeConfig {
  /**
   * `manual` | `ntp`。
   *
   * 它不只是"怎么对时"，还**决定 `GetSystemDateAndTime` 报的是 Manual 还是 NTP**
   * —— NVR 会照这个决定要不要来替我们对时。所以它在界面上必须显眼。
   */
  mode: string
  /** NTP 服务器地址（域名或 IP）。可以填内网地址 */
  ntp_server: string
  /** 对时间隔（秒）。**0 = 只在启动时对一次**，不是"关掉 NTP" */
  ntp_interval_s: number
  /**
   * 持久化的偏移：设备时间 = 主机时间 + 这个（秒）。
   *
   * **它落进配置是刻意的**：手动校准出来的偏移重启后要还在。NTP 模式下这个数
   * 只是"启动时的初值"，运行中对出来的偏移**不进配置** —— 所以界面上要把
   * "存着的"和"在用的"分开显示（见 `TimeInfo.configured_offset_s` 的说明）。
   */
  offset_s: number
}

export interface AppConfig {
  capture: CaptureConfig
  rtsp: RtspConfig
  audio: AudioConfig
  web: WebConfig
  onvif: OnvifConfig
  osd: OsdConfig
  adaptive: AdaptiveConfig
  time: TimeConfig
  /**
   * 通道 2 起。**`items` 为空时这个键整个不存在**（见 `ChannelsConfig`）。
   *
   * 所以是可选的，而且**不能用 `?? { items: [] }` 就地补一个再写回去** ——
   * 写回去的那一份里 `channels` 就恒存在了，于是一台从来没配过多通道的机器
   * 在下次启动时会因为"配置变了"而重开管线。要判空就用局部变量。
   */
  channels?: ChannelsConfig
  /**
   * 已保存但**还没生效**的字段，点分路径如 `rtsp.port`。由后端在 GET 时挂上。
   *
   * 存在的理由：`POST` 那个回应是一次性的，存完刷新一下页面提示就没了，而表单
   * 里显示的又都是新值 —— 看起来一切已生效。挂在 GET 上，提示才跟配置一样活得久。
   *
   * 老后端不发这个字段，所以是可选的。
   */
  pending?: string[]
  /**
   * 画质档位表，由后端在 GET 时挂上（`config.cpp` 的 `quality_tiers()`）。
   *
   * **从后端来，不在前端写死**：表里的 fps / 码率 / 分辨率上限是要显示给用户
   * 看的（"降到这一档会牺牲什么"），前端抄一份的话改了表界面就开始说假话。
   * 老后端不发这个字段，所以是可选的 —— 前端必须能在没有它的时候工作。
   */
  quality_tiers?: QualityTier[]
  /**
   * 延时档位表，由后端在 GET 时挂上（`config.cpp` 的 `latency_tiers()`）。
   *
   * 与 `quality_tiers` 同一个理由：表里的线程数与关键帧秒数是要显示给用户看的
   * （"这一档到底换来了什么、代价是什么"），前端抄一份的话改了表界面就说假话。
   * 老后端不发这个字段，所以是可选的。
   *
   * ⚠️ **没有 `custom` 这一档** —— 后端 `find_latency_tier("custom")` 返回
   * `nullptr`，它是"不展开"的落点，不是一个可选项。界面自己补那一项。
   */
  latency_tiers?: LatencyTier[]
}

/**
 * 一个延时档位。`key` 是配置里写的字面量，`label` 是中文显示名。
 *
 * 表里**只有这两个旋钮**（后端 `LatencyTier` 就这么两个字段）。它是刻意的：
 * 编码器帧级并行下线程数≈流水线延迟的帧数，而关键帧间隔决定"新观众多久能
 * 看到第一帧" —— 这两个量是这条链路上唯二既不碰协商结果、又能用一个数字
 * 说清楚的旋钮。
 */
export interface LatencyTier {
  key: string
  label: string
  /** 编码线程数 = 编码流水线的帧数（帧级并行下） */
  threads: number
  /** 关键帧间隔（秒），实际值是 fps × 这个数 */
  gop_seconds: number
}

/**
 * 一个画质档位。`key` 是配置里写的字面量，`label` 是中文显示名。
 *
 * `max_height` 为 0 表示"桌面原生"（不缩放），显示时要说成人话而不是 `0`。
 */
export interface QualityTier {
  key: string
  label: string
  fps: number
  bitrate_kbps: number
  max_height: number
  sub_enabled: boolean
}

/**
 * `POST /api/config` 的回应。
 *
 * `applied` / `pending` 是**逐字段**的，不是二选一：一次保存里常常两种字段同时
 * 存在（改个设备名当场生效，顺手改了端口就要重启）。只给一个
 * `restart_required` 布尔值时，界面只能二选一，两种情况里必有一种是在骗人。
 */
export interface SaveConfigResult {
  ok: boolean
  restart_required?: boolean
  applied?: string[]
  pending?: string[]
  note?: string
  error?: string
  config?: AppConfig
}

/** 密码字段被抹掉时代替真值的哨兵。后端 config.h 里的 kSecretSentinel。 */
export const SECRET_SENTINEL = '__unchanged__'

/**
 * `POST /api/reload` 的回应。**HTTP 状态是 202 而不是 200** —— 重开这件事发生
 * 在采集线程上，响应发出的这一刻它还没发生，回 200 等于宣称"已经重开了"。
 *
 * 所以调用方看到 202 + `ok:true` 只代表"收到了、正在做"，不代表已经做完。
 */
export interface ReloadResult {
  ok: boolean
  /** 此刻有没有一条管线在跑。false = 没在录制，这次请求是个空操作 */
  live?: boolean
  /** 后端拼好的中文说明，直接给用户看 */
  note?: string
}

/**
 * `POST /api/restart` 与 `POST /api/factory-reset` 的回应。
 *
 * 和 `ReloadResult` 一样是 **202**：动作发生在主线程那条分段循环上，响应发出的
 * 这一刻它还没发生。而且这两个比 reload 更早地"失去连接"—— 进程本身要退出，
 * 所以**拿不到 202 之后的任何东西**，连"它做完了没有"都只能靠轮询新的进程。
 */
export interface LifecycleResult {
  ok: boolean
  /** `POST /api/restart` 才有。 */
  restarting?: boolean
  /** `POST /api/factory-reset` 才有。 */
  resetting?: boolean
  /** 后端拼好的中文说明（**里面写清了代价**），直接给用户看。 */
  note?: string
  error?: string
}

/**
 * `GET /api/time` 的回应。**它比 `Stats` 里那六个字段多得多**，而且多出来的那一半
 * 只有在时间设置页上才有意义。
 *
 * 为什么两处并存：状态页顶部那一行要跟着 WebSocket 每秒刷，所以它那六个字段走了
 * `patch_live_counters`（`Stats`）；而这一页是**打开时拉一次、动作之后再拉一次**，
 * 它要的是"这一瞬间的完整真相"，包括只在页面上才有用的那几个（`syncing`、
 * 成功/失败计数、对时线程自己的读数）。把这一整份塞进每秒的 WS 帧里，
 * 等于让每一帧都背着一半没人看的字段。
 */
export interface TimeInfo {
  /** 主机时间（Unix 秒）。就是这台机器的 Windows 系统时钟 */
  host_now: number
  /**
   * 主机时间（Unix **毫秒**），与 `host_now` **同一时刻**、只是单位更细。
   *
   * 它存在的唯一理由是**秒的那一位不够用**：`host_now` 是截断到整秒的，页面上
   * 那个"每秒自己走"的设备时间拿它当基准的话，会带上一个恒定不到 1 秒的相位
   * 误差 —— 显示的秒有大约一半的时间停在前一秒，看着就像设备时间慢了 1 秒。
   * 而"设备时间跟别的钟对准了没有"正是那一段要回答的问题，±1 秒的显示会把
   * 答案变成猜。有了它，剩下的误差只有网络单程延迟。
   *
   * **`floor(host_now_ms / 1000) === host_now` 恒成立**（后端取一次
   * `system_clock::now()` 分别按两种单位换算），所以它不是一个"第二个真相"。
   *
   * **可选**：老后端不发这个字段（`main.cpp` 里它是后加的）。缺了就得退回
   * `host_now * 1000` —— 会走，只是带上上面说的那个相位误差。这条不变量
   * 由 `tests/browser/layout_probe.mjs` 的桩钉着（它的 `/api/time` 就没有这一项）。
   */
  host_now_ms?: number
  /**
   * 设备时间（Unix 秒）= `host_now + offset_s`。
   *
   * **水印上写的、ONVIF 报出去的就是它**，不是 `host_now`。
   */
  device_now: number
  /**
   * 设备时间 − 主机时间，**整秒**（可负）。这是"在用的那个"偏移。
   *
   * NTP 模式下它与 `configured_offset_s` **本来就该不一样** —— 对时得来的偏移
   * 不进配置（配置里那个数重启后才是初值）。界面上这两个数必须分得开，
   * 否则用户会以为"配置里写着 0，为什么设备差三小时"是故障。
   */
  offset_s: number
  /** 这个钟**被改过没有**（手动校准 / NVR 对时 / NTP 对时都算），不是"准不准"。 */
  synced: boolean
  /** 最近一次"钟被改"发生的时刻（Unix 秒）。0 = 从没被改过。 */
  synced_at: number
  /** `manual` | `ntp`。与 `TimeConfig.mode` 同源，这里再报一次是为了让这一份自洽。 */
  mode: string
  ntp_server: string
  ntp_interval_s: number
  /** 配置文件里**存着的**偏移。见 `offset_s` 的说明。 */
  configured_offset_s: number
  /** 对时线程的读数。这几个与 `Stats` 上那四个同源（`NtpSyncer::Status`）。 */
  last_sync_at: number
  /** `''` / `ok` / `timeout` / `bad_reply` / `unreachable`。空串 = 还没试过。 */
  last_sync_state: string
  last_sync_error: string
  /** 最近一次**成功**对时解出的偏移（秒）。失败时保留上一次成功值，别拿它判成败。 */
  last_sync_offset_s: number
  /** SNTP 回包里的 stratum。0 = 没成功过或服务器拒绝了（KoD）。 */
  last_sync_stratum: number
  /** 此刻有没有一次查询在飞。`POST /api/time/sync` 回 202 之后靠它判断"还在做"。 */
  syncing: boolean
  sync_ok_count: number
  sync_fail_count: number
}

/**
 * `POST /api/time`、`/api/time/reset`、`/api/time/sync` 三个动作的回应。
 *
 * **三条路的形状不同，别照着同一份写**：前两条是 200 且 `time` 一定是**做完之后**
 * 重新读的那一份（所以调用方不必再发一次 GET）；`sync` 是 **202**、**没有 `time`**
 * —— 那一刻 UDP 查询还没发出去（见 `ReloadResult` 上写的同一个理由）。
 */
export interface TimeActionResult {
  ok: boolean
  /** `sync` 才有。true = 查询已经排上队了，结果去看 `last_sync_state`。 */
  syncing?: boolean
  /** 后端拼好的中文说明，直接给用户看。 */
  note?: string
  error?: string
  /** `POST /api/time` 与 `/api/time/reset` 才带。**`sync` 没有**。 */
  time?: TimeInfo
}

/**
 * 一条操作日志。对应磁盘上一行 `2026-10-08 15:30:12 | 192.168.1.23 | web:admin | config.set | …`。
 *
 * `raw` 是**整行原文**，不是把上面那几个字段拼回去的那份。两者不是同一串字节
 * （原文里可能有 `|` 之外的空白差异），而审计的全部价值就在于"贴出去的这一行
 * 确实是日志里那一行"——所以界面上那个「复制」钮必须用 `raw`。
 *
 * ⚠️ **这里永远不会有口令**。detail 里也**不写配置值、只写字段名**
 * （见 `audit_log.h` 的口令纪律）—— 所以看到 `capture.bitrate_kbps=…` 而看不到
 * 具体数字是设计，不是丢字段。
 */
export interface AuditEntry {
  /** `YYYY-MM-DD` */
  day: string
  /** `HH:MM:SS` */
  time: string
  /** 来源 IP；系统自己写的是 `local` / `system`。**不为空**（空会写成 `?`）。 */
  source: string
  /** 惯例是 `<通道>:<用户名>`（`web:admin` / `onvif:admin`），系统写 `system`。 */
  identity: string
  /** `config.set` / `login.fail` / `onvif.SetOSD` / `ntp.sync` / `startup` … */
  action: string
  detail: string
  /** 整行原文 —— 「复制一行」用的就是它。 */
  raw: string
}

/**
 * `GET /api/audit` 的一份回应。
 *
 * `days` / `limit` 是**服务端夹过之后**的值（可能和请求的不一样），界面按这两个
 * 数说话。`count` 是这一页的条数，不是总数 —— 后端按 `limit` 截断，没有总数可报
 * （要总数就得把全部日志读一遍，而那是"看"类操作，不值得为它扫盘）。
 */
export interface AuditPage {
  /** 读到了没有。**不是"有没有记录"** —— 空日志也是 `ok: true` + 空数组。 */
  ok: boolean
  error?: string
  /** 审计开着没有（`--no-audit` / `config.log.audit`）。关着时下面全是空/0。 */
  enabled: boolean
  /** 留存天数（0 = 永不剪枝）。 */
  keep_days: number
  /** 日志落在哪个目录 —— 「保存在本地」这句话要落到一个用户打得开的路径上。 */
  dir: string
  files: number
  bytes: number
  /** 服务端实际采用的那两个数。 */
  days: number
  limit: number
  count: number
  entries: AuditEntry[]
}

/**
 * 恢复出厂要求的确认字面量。
 *
 * 服务端**只认这一个字符串**，前端那个二次确认框是给人看的、服务端不信它。
 * 写成一个常量而不是在两处各写一个字面量：改了后端不改前端的话，症状是
 * "点了确认、按钮转圈、什么都没发生"，而两处代码看上去都对。
 */
export const FACTORY_RESET_CONFIRM = 'RESET'

/** 统计推送的连接状态。 */
export type ConnState = 'connecting' | 'online' | 'offline'

// ---- 通道来源（GET /api/sources）------------------------------------------
//
// 配置页的「通道」那一段靠它把"这台机器上还有什么可以加"摆出来。用户看不到
// `--list-displays` / `--list-cameras` 的输出 —— 那两个是命令行上的东西。
//
// ⚠️ **这份清单是后端启动时算好的一份快照，不是每次请求现枚举。** 探"一块屏
// 能不能抓"要真的建一个 `IDXGIOutputDuplication`，而一块屏同时只允许一个，
// 现枚举会让**正在采集的那一路**失败。所以清单不会随插拔显示器而变，重启进程
// 才会更新 —— 界面上那句"启动时检测到"就是在说这件事。

/** 一个显示输出。后端 `ChannelDisplayInfo` + DXGI 的原生尺寸。 */
export interface SourceDisplay {
  /** `--list-displays` 的 `#N`，也是 `ChannelConfig.display_index` 的取值 */
  index: number
  /** 后端 `describe()` 拼好的整句（含型号、`\\.\DISPLAY1`、分辨率和坐标） */
  name: string
  width: number
  height: number
  /** 在虚拟桌面里的位置（可负，多屏时左屏是负数） */
  x: number
  y: number
  /**
   * 这块屏现在**抓不抓得到**。
   *
   * `false` 的典型是向日葵那类间接显示驱动：DXGI 的桌面复制对它建不出来，
   * 建出来也是一路永远黑屏的通道。后端自动发现**一条都不建**它，界面上也要
   * 把这一项标出来并挡住 —— 否则用户加了一路，得到的是黑屏。
   */
  capturable: boolean
}

/** 一台摄像头。后端 `CameraInfo`。 */
export interface SourceCamera {
  /** `--list-cameras` 的 `#N` */
  index: number
  /** 友好名（设备管理器里那个） */
  name: string
  /** 第一台。`--camera` 不带值时开的就是它，`ChannelConfig.camera` 留空也选它 */
  is_default: boolean
}

/** `GET /api/sources` 的响应。 */
export interface Sources {
  displays: SourceDisplay[]
  cameras: SourceCamera[]
  /**
   * 枚举摄像头时出的错。**只在真出错时才出现**。
   *
   * 必须显示出来：只回一个空数组的话，"这台机器上没插摄像头"和"枚举坏了"
   * 在界面上长得一模一样 —— 和 `AudioDevices.capture_error` 同一条纪律。
   */
  camera_error?: string
}

/** `GET`/`POST /api/preview-channel` 的响应。 */
export interface PreviewChannelResult {
  ok?: boolean
  /** 当前（或请求生效后的）预览通道号 */
  channel: number
  error?: string
}
