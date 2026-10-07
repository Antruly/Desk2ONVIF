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
  /** 子码流编码器的实际尺寸。0 = 没开子码流 */
  sub_width: number
  sub_height: number
  /** 逐路明细，下标和 RTSP 的路一一对应：0 主、1 子。未开推流时是空数组 */
  rtsp_streams: RtspStream[]
  /** 鼠标合成过的帧数 */
  cursor_frames: number
  /** 其中"只因为鼠标动了才产出"的帧数。桌面静止时靠的就是它们 */
  pointer_only_frames: number
  web_port: number
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
  hardware_id: string
  serial_number: string
  device_name: string
  location: string
}

export interface AppConfig {
  capture: CaptureConfig
  rtsp: RtspConfig
  web: WebConfig
  onvif: OnvifConfig
  /**
   * 已保存但**还没生效**的字段，点分路径如 `rtsp.port`。由后端在 GET 时挂上。
   *
   * 存在的理由：`POST` 那个回应是一次性的，存完刷新一下页面提示就没了，而表单
   * 里显示的又都是新值 —— 看起来一切已生效。挂在 GET 上，提示才跟配置一样活得久。
   *
   * 老后端不发这个字段，所以是可选的。
   */
  pending?: string[]
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

/** 统计推送的连接状态。 */
export type ConnState = 'connecting' | 'online' | 'offline'
