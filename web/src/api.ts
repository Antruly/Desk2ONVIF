import { FACTORY_RESET_CONFIRM } from './types'
import type {
  AppConfig,
  AudioDevices,
  LifecycleResult,
  LoginResult,
  PreviewChannelResult,
  ReloadResult,
  SaveConfigResult,
  SessionInfo,
  Sources,
  Stats,
  TimeActionResult,
  TimeInfo,
  AuditPage,
} from './types'

/**
 * HTTP 接口封装。
 *
 * 原则是**不要抛裸异常**：调用方需要的是"给用户看的一句话"，不是 stack。
 * 所以每个失败都返回 { ok:false, error }，把 HTTP 状态、后端正文、网络故障
 * 统一成一条中文消息。
 */

export type ApiResult<T> =
  | { ok: true; data: T }
  | {
      ok: false
      error: string
      status?: number
      /**
       * 失败响应体**如果**是 JSON，原样带出来。
       *
       * 存在的理由很具体：429（登录失败次数过多）的正文里有 `retry_after_s`，
       * 而登录页要靠它起倒计时。只保留一句 error 文本的话，用户看到的是
       * "请稍后再试"，却不知道要等多久 —— 于是他会立刻再试一次，
       * 而每试一次冷却都重新算，越试越进不去。
       */
      body?: unknown
    }

/**
 * 会话失效时的回调。由 `useAuth` 在模块初始化时挂上。
 *
 * 为什么用回调而不是让这里 `import { useAuth }`：`/api/session` 自己也要走
 * 这条路，直接互相 import 会成环（api → useAuth → api）。成环在 ESM 里不一定
 * 报错，但初始化顺序变得依赖 import 语句的书写顺序 —— 那种 bug 只在改无关
 * 代码时才发作。
 */
type UnauthHandler = () => void
let onUnauthorized: UnauthHandler | null = null

export function setUnauthorizedHandler(h: UnauthHandler | null): void {
  onUnauthorized = h
}

/**
 * 会话 cookie 由浏览器自己带着走（后端标了 HttpOnly，JS 读不到也不该读）。
 *
 * `credentials: 'same-origin'` 是必须显式写的**默认值**：将来哪天这个前端被挪到
 * 别的源上（比如 Vite dev server 走 proxy 改成直连），默认行为会从
 * same-origin 变成 omit，症状是"登录成功、下一个请求又是未登录"，而且
 * 网络面板里一切正常（请求发出去了、响应也回来了）。写出来，不改。
 */
const CRED: RequestInit = { credentials: 'same-origin' }

/** 失败响应解析出来的两半：给人看的一句话，和结构化信息（如果有）。 */
interface FailureDetail {
  error: string
  body?: unknown
}

/**
 * 从失败响应里挖出后端想说的话。
 *
 * 错误体的形状没有冻结，所以按 error/message/detail/msg 依次找；都不是就把
 * 正文原样截断显示 —— 只说"HTTP 400"会让人完全无从下手，宁可显示一段原始 JSON。
 */
async function describeFailure(resp: Response): Promise<FailureDetail> {
  let text = ''
  try {
    text = await resp.text()
  } catch {
    text = ''
  }
  if (text) {
    let parsed: unknown
    try {
      parsed = JSON.parse(text)
    } catch {
      // 不是 JSON —— 当纯文本用，没有结构化信息可带。
      return { error: text.length > 400 ? `${text.slice(0, 400)}…` : text }
    }
    const j = parsed as Record<string, unknown>
    const clipped = text.length > 400 ? `${text.slice(0, 400)}…` : text
    for (const key of ['error', 'message', 'detail', 'msg']) {
      const v = j[key]
      // 消息字段找到了也照样带上整个对象：`retry_after_s` 这类字段不在消息里，
      // 而它恰恰是调用方唯一需要的结构信息。
      if (typeof v === 'string' && v.trim()) return { error: v, body: j }
    }
    return { error: clipped, body: j }
  }
  return { error: `HTTP ${resp.status}${resp.statusText ? ' ' + resp.statusText : ''}` }
}

/**
 * 401 的统一处理。
 *
 * **`/api/login` 和 `/api/session` 排除在外**：前者的 401 就是"口令不对"，
 * 是最正常的一种失败；后者是匿名接口，根本不会 401。把它们的 401 也当成
 * "会话过期"，用户会在登录页上被自己的一次手误踢回登录页。
 */
function handleUnauthorized(status: number, url: string): void {
  if (status !== 401) return
  if (url.startsWith('/api/login') || url.startsWith('/api/session')) return
  onUnauthorized?.()
}

async function getJson<T>(url: string): Promise<ApiResult<T>> {
  try {
    const resp = await fetch(url, { ...CRED, headers: { Accept: 'application/json' } })
    if (!resp.ok) {
      handleUnauthorized(resp.status, url)
      const f = await describeFailure(resp)
      return { ok: false, error: f.error, status: resp.status, body: f.body }
    }
    return { ok: true, data: (await resp.json()) as T }
  } catch (err) {
    // fetch 只在网络层失败时 reject。"请求发不出去"最常见的原因就是后端没起来，
    // 这句话得说出来，否则用户会以为是界面坏了。
    return { ok: false, error: `请求失败：${(err as Error).message}（后端没启动或地址不对？）` }
  }
}

async function postJson<T>(url: string, body: unknown): Promise<ApiResult<T>> {
  try {
    const resp = await fetch(url, {
      ...CRED,
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
      body: JSON.stringify(body),
    })
    if (!resp.ok) {
      handleUnauthorized(resp.status, url)
      const f = await describeFailure(resp)
      return { ok: false, error: f.error, status: resp.status, body: f.body }
    }
    // 成功的 POST 不保证有 JSON 体（可能只回 200 空体），空体不该算失败。
    const text = await resp.text()
    if (!text.trim()) return { ok: true, data: undefined as T }
    try {
      return { ok: true, data: JSON.parse(text) as T }
    } catch {
      return { ok: true, data: undefined as T }
    }
  } catch (err) {
    return { ok: false, error: `请求失败：${(err as Error).message}（后端没启动或地址不对？）` }
  }
}

/** 一次性快照。WS 断了的时候也能看到数，不至于整页空白。 */
export function fetchStatus(): Promise<ApiResult<Stats>> {
  return getJson<Stats>('/api/status')
}

export function fetchConfig(): Promise<ApiResult<AppConfig>> {
  return getJson<AppConfig>('/api/config')
}

/**
 * 音频设备列表，给配置页那个麦克风下拉框用。
 *
 * **每次进页面现拉，不缓存。** 插拔一个耳机列表就变（`EnumAudioEndpoints` 每次
 * 都是现枚举的），缓存住的话下拉里会一直留着一台已经拔掉的设备 —— 而选中它得到
 * 的是一条永远静音的音轨，SDP 与 ffprobe 看上去却完全正常。
 *
 * 取 `capture` 那一组才是麦克风；`render` 是环回那一侧，界面上选不了（「系统声音」
 * 固定用系统默认渲染端点）。接口本身由后端按 `opt.audio_devices` 决定注不注册，
 * **老后端上没有这条路** —— 调用方要能接受一次失败而不把整页配置带崩。
 */
export function fetchAudioDevices(): Promise<ApiResult<AudioDevices>> {
  return getJson<AudioDevices>('/api/audio/devices')
}

export function saveConfig(cfg: AppConfig): Promise<ApiResult<SaveConfigResult>> {
  return postJson<SaveConfigResult>('/api/config', cfg)
}

/**
 * 本机有什么可以当通道（显示器 + 摄像头），给配置页的「通道」那一段用。
 *
 * **每次进页面现拉，但要知道它是一份启动时的快照**：探"一块屏能不能抓"会真的
 * 建一个 `IDXGIOutputDuplication`，而一块屏同时只允许一个 —— 现枚举会让正在
 * 采集的那一路失败。所以插了一块新屏之后这里不会自己变，重启进程才会。界面上
 * 那句"启动时检测到"说的就是这件事。
 */
export function fetchSources(): Promise<ApiResult<Sources>> {
  return getJson<Sources>('/api/sources')
}

/** 网页预览现在看的是第几路。**全机一份**（后端持有），不是每个标签页各一份。 */
export function fetchPreviewChannel(): Promise<ApiResult<PreviewChannelResult>> {
  return getJson<PreviewChannelResult>('/api/preview-channel')
}

/**
 * 切换预览到第几路。
 *
 * 生效是**立刻**的（后端翻一个位），但画面要等前端把 `/ws/preview` 重开一次 ——
 * 一次采集只在开头发一次 ftyp+moov，中途换路必须重连才能拿到新那一路的初始化段。
 * 所以调用方的顺序是 **先关旧 WS、再 POST、再开新 WS**（见 `PreviewView.vue`）。
 */
export function setPreviewChannel(channel: number): Promise<ApiResult<PreviewChannelResult>> {
  return postJson<PreviewChannelResult>('/api/preview-channel', { channel })
}

/**
 * 只改画质档位。**发的是局部补丁，不是整份配置** —— `config_from_json` 是以
 * "当前生效值"为底合并的，所以 `{capture:{quality}}` 只动这一个字段，不会顺手
 * 把别处一起写一遍（整份提交要先把配置读回来，多一次可能失败的往返）。
 *
 * 后端在 `validate_config` 里按档位表展开它：fps / bitrate_kbps / max_height /
 * sub_enabled 四项会被**重写**成该档位的值，而它们都要重开管线才生效。
 */
export function saveQualityTier(tier: string): Promise<ApiResult<SaveConfigResult>> {
  return postJson<SaveConfigResult>('/api/config', { capture: { quality: tier } })
}

/**
 * 用**当前保存的配置**重开采集/推流管线。
 *
 * **代价是实打实的**：录制会分段（新文件从 `main.1.mp4` 续）、已连上的 RTSP
 * 客户端会断开、数秒后重连。所以调用方必须把这句话说给用户听，而不是把它
 * 包成一个看起来无害的"应用"按钮。
 *
 * 与 `/api/onvif/restart` **不是一回事**：那条只重播一次 WS-Discovery 组播，
 * 画面一秒都不中断。
 */
export function reloadPipeline(): Promise<ApiResult<ReloadResult>> {
  return postJson<ReloadResult>('/api/reload', {})
}

/** 重启 ONVIF / WS-Discovery 响应器。NVR 搜不到设备时先按这个。 */
export function restartOnvif(): Promise<ApiResult<unknown>> {
  return postJson<unknown>('/api/onvif/restart', {})
}

/**
 * **真的把这个进程退出再起来。**
 *
 * 三条代价，一条都不能省着不说（回应的 `note` 里后端也写了一遍）：
 *   1. 当前录制会中断并分段；
 *   2. 已连上的 RTSP 客户端会断开重连；
 *   3. **所有网页会话失效，必须重新登录** —— 会话令牌签的是每进程一份的随机
 *      密钥，重启必然换掉它。所以前端拿到 401 **不是**重启失败，而是"它回来了"。
 *
 * 返回 `ok:true` 只代表**请求被接受了**（202）。进程正在退出，接下来这个 HTTP
 * 连接也会断 —— 调用方要做的是轮询，而不是等这个 promise 给出"重启完了"。
 */
export function restartService(): Promise<ApiResult<LifecycleResult>> {
  return postJson<LifecycleResult>('/api/restart', {})
}

/**
 * 恢复出厂：删掉 config.json（先备份成 `.bak`）、清空日志、时间偏移归零，
 * 然后**重启**。
 *
 * `confirm` 必须是字面量 `RESET`，服务端**只认它** —— 界面上那个二次确认框
 * 是给人看的，服务端不信"我点了确认"。
 *
 * 一条必须写进确认框的诚实边界：**恢复的是配置文件**。命令行上带的参数
 * （端口、口令等）重启后照样生效，因为重启用的是同一条命令行。安装包里
 * 恰好回到 8080 / admin，那是"命令行与默认值一致"，不是"恢复出厂改了端口"。
 */
export function factoryReset(): Promise<ApiResult<LifecycleResult>> {
  return postJson<LifecycleResult>('/api/factory-reset', { confirm: FACTORY_RESET_CONFIRM })
}

/* ---------- 时间 ---------- */

/**
 * 读设备时间的完整状态。
 *
 * **这一组管的都是"设备时间"**（= 主机时间 + 偏移，水印上和 ONVIF 报出去的那个），
 * 不是这台机器的 Windows 系统时钟。两者可以差出几小时，那是选定的语义，不是故障。
 *
 * 与 `Stats` 上那六个字段的关系：状态页顶部那六个走 WebSocket 每秒刷；这一份是
 * 时间设置页要的"完整真相"（含 `syncing`、成功/失败计数、对时线程的读数），
 * 打开时拉一次、做完动作再拉一次。两处并存是刻意的，理由见 `TimeInfo` 的说明。
 */
export function fetchTime(): Promise<ApiResult<TimeInfo>> {
  return getJson<TimeInfo>('/api/time')
}

/**
 * 手动校准。两种给法，**二选一**：
 *   - `{ offset_s: n }` —— 直接给偏移（秒，可负）；
 *   - `{ device_now: "2026-10-08T15:30:00" }` —— 给一个绝对时间，由**服务端**算偏移。
 *     用绝对时间是"与浏览器时间对齐"那条路：把用户手边这个钟填进去。
 *
 * **NTP 模式下后端会拒绝**（400，`error` 里写清了原因）。那不是保守：NTP 模式下
 * 的钟归对时线程管，下一个周期就把手动设的值覆盖回去，用户看到的是"设成功了、
 * 一小时后自己变回去了"。
 *
 * 回 200 且带 `time` = **做完之后重新读的那一份**，调用方不必再发一次 GET。
 */
export function setTime(body: { offset_s: number } | { device_now: string }): Promise<
  ApiResult<TimeActionResult>
> {
  return postJson<TimeActionResult>('/api/time', body)
}

/** 偏移归零 = 设备时间恢复成主机时间。同样回 200 + 做完之后的 `time`。 */
export function resetTime(): Promise<ApiResult<TimeActionResult>> {
  return postJson<TimeActionResult>('/api/time/reset', {})
}

/**
 * 立刻向 NTP 服务器发起一次查询。
 *
 * **回 202，而且没有 `time`** —— 真正的 UDP 查询在后台线程上（最长 2 秒 × 地址数），
 * 响应发出的这一刻它还没发出去。所以调用方看到 202 只代表"排上队了"，结果要去
 * `last_sync_state` / `last_sync_error` 上看（或再拉一次 `fetchTime()`）。
 *
 * 当前是**手动模式**时，这次只报结果、**不会改动设备时间**；NTP 模式下才会同时
 * 把设备时间对齐。回应的 `note` 里后端把这句话写了一遍。
 */
export function syncTimeNow(): Promise<ApiResult<TimeActionResult>> {
  return postJson<TimeActionResult>('/api/time/sync', {})
}

/* ---------- 操作日志 ---------- */

/**
 * 读回最近的审计记录（新的在前）。
 *
 * 三个参数都会被**服务端**夹到合法范围内，并**把夹过之后的值回报**在
 * `days` / `limit` 上 —— 界面要按回报的那两个数画分页，不能按自己请求的那两个：
 * 请求 5000 天而实际只读了 400 天时，少掉的那一截看起来像"那天没有记录"。
 *
 * `q` 是关键字（大小写敏感，与后端一致）。**只读我们自己的 `audit-*.log`** ——
 * 这个接口看不到 `desk2onvif.log`，那不是遗漏，是审计与调试日志本来就是两份
 * （留存期、内容纪律、恢复出厂要清的集合三条都不一样，见 `audit_log.h`）。
 *
 * `ok` 说的是**读到了没有**，不是"有没有记录"：一台刚恢复出厂、日志一个都不在
 * 的设备会回 `ok:true` + 空数组。所以界面上"没有记录"和"读不出来"必须分开说，
 * 不能都画成一张空表。
 */
export function fetchAudit(
  days = 7,
  limit = 500,
  keyword = '',
): Promise<ApiResult<AuditPage>> {
  const qs = new URLSearchParams({ days: String(days), limit: String(limit) })
  if (keyword !== '') qs.set('q', keyword)
  return getJson<AuditPage>(`/api/audit?${qs.toString()}`)
}

/* ---------- 会话 ---------- */

/** 匿名可达。返回 `authRequired` 为 false 时，这台机器上没开登录。 */
export function fetchSession(): Promise<ApiResult<SessionInfo>> {
  return getJson<SessionInfo>('/api/session')
}

/**
 * 登录。成功时 cookie 由 `Set-Cookie` 落到浏览器里，这里不需要也不该处理它
 * （后端标了 HttpOnly，JS 根本读不到）。
 */
export function postLogin(user: string, password: string): Promise<ApiResult<LoginResult>> {
  return postJson<LoginResult>('/api/login', { user, password })
}

/**
 * 登出。
 *
 * **服务端是无状态的，它什么都没做** —— 令牌签的是每进程一份的随机密钥，
 * 不是口令，所以"登出"只是让浏览器丢掉 cookie，拿着旧令牌的人照样进得来。
 * 真要立刻吊销所有会话只有一条路：重启后端进程。
 */
export function postLogout(): Promise<ApiResult<unknown>> {
  return postJson<unknown>('/api/logout', {})
}
