import { ref } from 'vue'
import { noteUnauthorized } from './useAuth'
import type { ConnState, RtspStream, Stats } from '../types'

/**
 * /ws/stats 的订阅状态。
 *
 * 做成模块级单例（而不是每个组件各开一条）有两个原因：顶栏的连接指示灯和
 * 状态页要显示同一个状态；以及浏览器对同一主机的并发连接数有限制，多开一条
 * 就多一份重连风暴。
 */

const RECONNECT_MIN_MS = 1000
/** 退避上限。再长的话后端重启之后用户会以为界面死了。 */
const RECONNECT_MAX_MS = 30000
/** 静默这么久就认定连接已经半开。后端每秒必推一帧，5 秒是 5 个周期的余量。 */
const SILENCE_TIMEOUT_MS = 5000
const WATCHDOG_PERIOD_MS = 1000

function emptyStats(): Stats {
  return {
    elapsed_s: 0,
    ticks: 0,
    content_frames: 0,
    packets: 0,
    keyframes: 0,
    bytes: 0,
    effective_fps: 0,
    queue_dropped: 0,
    pool_exhausted: 0,
    access_lost: 0,
    nalu_dropped: 0,
    max_nalu_bytes: 0,
    rtsp_clients: 0,
    rtsp_url: '',
    sub_width: 0,
    sub_height: 0,
    rtsp_streams: [],
    cursor_frames: 0,
    pointer_only_frames: 0,
    web_port: 0,
    ws_clients: 0,
    wsd_running: false,
    wsd_probes_seen: 0,
    wsd_probes_answered: 0,
    onvif_requests: 0,
    onvif_auth_failures: 0,
    onvif_faults: 0,
    onvif_last_unsupported: '',
    preview_clients: 0,
    preview_ready: false,
    preview_fragments: 0,
    preview_fragments_unwatched: 0,
    preview_clients_kicked: 0,
  }
}

/** 只接受有限数值。JSON 里出现 null / 字符串 / NaN 都当缺省，免得把 NaN 印到界面上。 */
function num(v: unknown): number {
  return typeof v === 'number' && Number.isFinite(v) ? v : 0
}

function str(v: unknown): string {
  return typeof v === 'string' ? v : ''
}

/** 布尔字段。后端发 `true` / `false`，缺失或类型不对时按 `false` 算。 */
function flag(v: unknown): boolean {
  return v === true
}

/** 逐路明细。**每一项都要过一遍兜底**，不能直接断言成 RtspStream。 */
function streams(v: unknown): RtspStream[] {
  if (!Array.isArray(v)) return []
  return v.map((e) => {
    const o = (e ?? {}) as Record<string, unknown>
    return {
      path: str(o.path),
      url: str(o.url),
      clients: num(o.clients),
      nalu_dropped: num(o.nalu_dropped),
      max_nalu_bytes: num(o.max_nalu_bytes),
      packets: num(o.packets),
      bytes: num(o.bytes),
    }
  })
}

/**
 * 把任意外来对象收敛成完整的 Stats。
 * 后端第一帧可能缺字段，这里逐字段兜底是必须的 —— 少一个字段就会让整块面板
 * 显示 undefined。
 */
export function normalizeStats(raw: unknown): Stats {
  const o = (raw ?? {}) as Record<string, unknown>
  return {
    elapsed_s: num(o.elapsed_s),
    ticks: num(o.ticks),
    content_frames: num(o.content_frames),
    packets: num(o.packets),
    keyframes: num(o.keyframes),
    bytes: num(o.bytes),
    effective_fps: num(o.effective_fps),
    queue_dropped: num(o.queue_dropped),
    pool_exhausted: num(o.pool_exhausted),
    access_lost: num(o.access_lost),
    nalu_dropped: num(o.nalu_dropped),
    max_nalu_bytes: num(o.max_nalu_bytes),
    rtsp_clients: num(o.rtsp_clients),
    rtsp_url: str(o.rtsp_url),
    sub_width: num(o.sub_width),
    sub_height: num(o.sub_height),
    rtsp_streams: streams(o.rtsp_streams),
    cursor_frames: num(o.cursor_frames),
    pointer_only_frames: num(o.pointer_only_frames),
    web_port: num(o.web_port),
    ws_clients: num(o.ws_clients),
    wsd_running: flag(o.wsd_running),
    wsd_probes_seen: num(o.wsd_probes_seen),
    wsd_probes_answered: num(o.wsd_probes_answered),
    onvif_requests: num(o.onvif_requests),
    onvif_auth_failures: num(o.onvif_auth_failures),
    onvif_faults: num(o.onvif_faults),
    onvif_last_unsupported: str(o.onvif_last_unsupported),
    preview_clients: num(o.preview_clients),
    preview_ready: flag(o.preview_ready),
    preview_fragments: num(o.preview_fragments),
    preview_fragments_unwatched: num(o.preview_fragments_unwatched),
    preview_clients_kicked: num(o.preview_clients_kicked),
  }
}

const stats = ref<Stats>(emptyStats())
const state = ref<ConnState>('offline')
/** 码率是从累计字节数算出来的，不是后端给的字段。 */
const bitrateKbps = ref(0)
const lastMessageAt = ref(0)

let socket: WebSocket | null = null
let reconnectTimer: number | null = null
let watchdog: number | null = null
let reconnectDelay = RECONNECT_MIN_MS
let prevBytes = 0
let prevBytesAt = 0
let running = false

function clearReconnect(): void {
  if (reconnectTimer !== null) {
    window.clearTimeout(reconnectTimer)
    reconnectTimer = null
  }
}

function scheduleReconnect(): void {
  if (!running || reconnectTimer !== null) return
  const delay = reconnectDelay
  // 指数退避 1s → 30s。后端在重启时可能连着拒几次，退避能避免把日志刷满。
  reconnectDelay = Math.min(reconnectDelay * 2, RECONNECT_MAX_MS)
  reconnectTimer = window.setTimeout(() => {
    reconnectTimer = null
    connect()
  }, delay)
}

/**
 * 用累计字节数的差值算瞬时码率。
 *
 * 后端给的是**累计计数器**，不是速率，所以必须自己存上一拍的 (bytes, 时间)。
 * 采样间隔用真实时间差而不是"假设 1 秒"，因为定时器不准、消息也可能丢一帧。
 */
function updateBitrate(bytes: number): void {
  const now = Date.now()
  if (prevBytesAt !== 0 && now > prevBytesAt) {
    const delta = bytes - prevBytes
    // delta < 0 说明后端重启过、计数器归零了。负速率没有意义，显示 0，
    // 下一拍自然恢复（prevBytes 已经更新成新的基准）。
    bitrateKbps.value = delta < 0 ? 0 : Math.round(((delta * 8) / (now - prevBytesAt)) * 10) / 10
  }
  prevBytes = bytes
  prevBytesAt = now
}

function connect(): void {
  if (!running) return
  if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) {
    return
  }
  clearReconnect()
  state.value = 'connecting'

  // 把"最后收报时间"提前到发起连接的这一刻。
  // 不这么做，看门狗会拿初始值 0 去比，于是每次刚建好的连接在第一个 tick
  // 就被判成静默并掐掉 —— 表现是连不上又说不清为什么。这样写还顺带给了
  // 连接本身一个 5 秒超时（握手一直不完成也算半开）。
  lastMessageAt.value = Date.now()

  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:'
  let ws: WebSocket
  try {
    ws = new WebSocket(`${scheme}//${location.host}/ws/stats`)
  } catch {
    scheduleReconnect()
    return
  }
  socket = ws

  ws.onopen = () => {
    state.value = 'online'
    reconnectDelay = RECONNECT_MIN_MS
    lastMessageAt.value = Date.now()
    // 重置码率基准。跨一次断线拿旧基准算差值，会得到一个巨大的假尖峰。
    prevBytesAt = 0
  }

  ws.onmessage = (ev: MessageEvent) => {
    lastMessageAt.value = Date.now()
    state.value = 'online'
    // 这个通道是文本帧。收到二进制说明不是统计通道，忽略而不是去解析它。
    if (typeof ev.data !== 'string') return
    let parsed: unknown
    try {
      parsed = JSON.parse(ev.data)
    } catch {
      // 坏帧丢掉就好，不能让它把整页搞崩
      return
    }
    const next = normalizeStats(parsed)
    updateBitrate(next.bytes)
    stats.value = next
  }

  ws.onclose = (ev: CloseEvent) => {
    // 只有还是"当前这条"才处理：看门狗主动关掉的那条会走到这里，但它已经把
    // socket 置空并安排过重连了，不拦一下就会排出两条并行的重连。
    if (socket !== ws) return
    socket = null
    state.value = 'offline'

    // 1008（POLICY_VIOLATION）= 后端说"你没登录"。
    //
    // **这条必须和"网络断了"分开处理。** 两者都是"连接关掉了"，但重连对前者
    // 毫无意义：cookie 已经过期，重连一百次还是 1008。而重连还更糟 ——
    // 用户会看到界面在"已断开/连接中"之间来回跳，同时被踢回登录页，却不知
    // 道为什么。停掉轮询、告诉登录状态那一层，让路由守卫把他送回登录页。
    //
    // 顺带说一句：WS 升级**不经过** HTTP 的鉴权中间件（libuvcpp 的升级路径
    // 直接调 handler），所以这个 1008 是后端在两个 WS 处理器里自己发的。
    if (ev.code === 1008) {
      noteUnauthorized()
      stopStats()
      return
    }

    scheduleReconnect()
  }

  // onerror 后面必定跟 onclose，重连逻辑只放在 onclose 里，避免两处触发。
  ws.onerror = () => {}
}

/**
 * 单开一个定时器盯着"最后一帧的时间"。
 *
 * 光靠 onclose 是不够的：拔网线、系统休眠、NAT 静默超时这几种情况下，TCP 连接
 * 在半开状态可以挂很久，浏览器不会触发 onclose。没有这个看门狗，界面会永远
 * 停在最后一次收到的数字上，而指示灯还是"已连接" —— 比断开更糟，因为它看起来是好的。
 */
function startWatchdog(): void {
  if (watchdog !== null) return
  watchdog = window.setInterval(() => {
    if (!socket || socket.readyState === WebSocket.CLOSED) return
    if (Date.now() - lastMessageAt.value <= SILENCE_TIMEOUT_MS) return
    const dead = socket
    socket = null
    dead.close()
    state.value = 'offline'
    // 半开不代表服务端挂了，退避重置回 1 秒，网络恢复时能立刻接上。
    reconnectDelay = RECONNECT_MIN_MS
    scheduleReconnect()
  }, WATCHDOG_PERIOD_MS)
}

/** 幂等：多个组件都调也只会连一条。 */
export function startStats(): void {
  if (running) return
  running = true
  startWatchdog()
  connect()
}

export function stopStats(): void {
  running = false
  clearReconnect()
  if (watchdog !== null) {
    window.clearInterval(watchdog)
    watchdog = null
  }
  if (socket) {
    const s = socket
    socket = null
    s.close()
  }
  state.value = 'offline'
}

export function useStats() {
  return { stats, state, bitrateKbps, lastMessageAt, startStats, stopStats }
}
