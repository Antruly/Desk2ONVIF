<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import Icon from '../components/Icon.vue'
import { useAuth } from '../composables/useAuth'
import { useStats } from '../composables/useStats'
import { formatBytes, formatInt } from '../format'

/**
 * 浏览器预览：fMP4 over WebSocket → MediaSource。
 *
 * ## 这个页面的核心难题：分片的时间轴不从 0 开始
 *
 * 后端的 fMP4 封装器把**进程第一个样本**的时刻当作时间轴原点
 * （`Fmp4Muxer::write_nal` 里的 `base_dts_us_`）。所以：
 *
 *   - 程序刚起来就打开这一页 → 分片的 `tfdt` 从 0 开始 → 直接就能播
 *   - 程序已经跑了 75 秒才打开 → 分片的 `tfdt` 从 74 秒开始
 *
 * 第二种情况下 `MediaSource` 里 `buffered` 是 `[74, 76]`，而
 * `video.currentTime` 是 **0** —— 播放头不在任何已缓冲区间里。浏览器的表现
 * 是**什么都不做**：`readyState` 停在 1（HAVE_METADATA）、`currentTime` 永远
 * 是 0、不抛异常、控制台一个字都没有。看起来就是"快照能出、预览黑屏"。
 *
 * 修法就是每收到一块数据就把播放头对齐到"最新数据的末尾往前 `TARGET_LATENCY`
 * 秒"。这一步对上面两种情况都成立，也顺带覆盖了断线重连（重连后新分片的
 * `tfdt` 只会更靠后）。
 *
 * ## 断线重连为什么是"整个重建"而不是"接着喂"
 *
 * 因为后端在每次 WS 连接建立时会**重发初始化段**（中途连上来的客户端要靠它
 * 才能解码）。而 MSE 规定初始化段必须是 append 进 SourceBuffer 的第一块 ——
 * 往一个已经在跑的 SourceBuffer 里再塞一块 ftyp+moov，各家浏览器行为不一致
 * （Chrome 会报 `MediaSource is in a bad state`，把整个 buffer 废掉）。
 * 整个重建是唯一确定正确的那条路，付出的代价只是重连时黑一下。
 */

/* ------------------------------------------------------------------ 常量 */

/**
 * 目标延迟：播放头停在"最新数据末尾"往前这么多秒。
 *
 * 太小（<0.3）会频繁 stall —— 网络稍微抖一下缓冲就见底了；太大就白送延迟。
 * 0.5 秒配合本地回环（后端就在本机）是稳的。
 */
const TARGET_LATENCY = 0.5
/** 落后超过这么多秒才重新对齐。否则每来一块都会 seek 一下，画面会不停跳。 */
const MAX_DRIFT = 2.5
/** 缓冲最多往前留这么多秒，超了就裁掉。不裁的话长开的页面会一直吃内存。 */
const MAX_BUFFER_S = 30
/** 队列里最多压这么多块。超过说明解码端卡死了，重建整条链路。 */
const MAX_QUEUE = 120
/** 连上之后多久没收到**任何**数据就报"推流没起来"。 */
const FIRST_DATA_TIMEOUT_MS = 8000
/** 连上、收到初始化段之后，多久没收到**分片**就提示在等关键帧。 */
const FIRST_FRAGMENT_TIMEOUT_MS = 20000

/** 优先主档位，不支持就退到 baseline。两个都不支持说明浏览器压根给不了 MSE。 */
const MIME_CANDIDATES = [
  'video/mp4; codecs="avc1.42C033"',
  'video/mp4; codecs="avc1.4D401F"',
  'video/mp4; codecs="avc1.42E01E"',
]

type PlayerState =
  | 'idle'
  | 'connecting'
  | 'waiting-init'
  | 'waiting-fragment'
  | 'playing'
  | 'stalled'
  | 'ended'
  | 'unsupported'
  | 'error'

const route = useRoute()
const router = useRouter()
const { noteUnauthorized } = useAuth()
const { stats, state: statsConn } = useStats()

const videoRef = ref<HTMLVideoElement | null>(null)
const stageRef = ref<HTMLDivElement | null>(null)

const playerState = ref<PlayerState>('idle')
const playerMessage = ref('')
/** 与"最新数据末尾"的距离，秒。播放中每秒更新一次。 */
const latency = ref(0)
const paused = ref(false)
const muted = ref(true)
const isFullscreen = ref(false)

/**
 * 用户是不是想看画面。
 *
 * **必须是响应式的**：它决定工具栏上显示"开始预览"还是"暂停/停止"，而
 * 后者由 `stop()` 和 1008 两条路改写 —— 用一个普通 `let` 的话按钮永远
 * 停在初始那一组。
 */
const wantStream = ref(false)
/** 画面的真实宽高比。抓到的桌面是 3:2 或 21:9 都可能，写死 16:9 会留黑边。 */
const videoAspect = ref(16 / 9)

const stateText = computed(() => {
  switch (playerState.value) {
    case 'connecting':
      return '正在连接…'
    case 'waiting-init':
      return '等待初始化段'
    case 'waiting-fragment':
      return '等待关键帧'
    case 'playing':
      return '播放中'
    case 'stalled':
      return '画面停滞'
    case 'ended':
      return '通道已断开'
    case 'unsupported':
      return '浏览器不支持'
    case 'error':
      return '预览出错'
    default:
      return '未开始'
  }
})

const stateTone = computed(() => {
  if (playerState.value === 'playing') return 'good'
  if (
    playerState.value === 'waiting-init' ||
    playerState.value === 'waiting-fragment' ||
    playerState.value === 'stalled' ||
    playerState.value === 'ended'
  ) {
    return 'warn'
  }
  if (playerState.value === 'error' || playerState.value === 'unsupported') return 'crit'
  return ''
})

/** 播放中且落后很多时，给一个"回到最新"的按钮。 */
const isBehind = computed(() => playerState.value === 'playing' && latency.value > 3)

/* ------------------------------------------------------------------ 状态 */

let mediaSource: MediaSource | null = null
let sourceBuffer: SourceBuffer | null = null
let socket: WebSocket | null = null
let objectUrl = ''
let firstDataTimer: number | null = null
let fragmentTimer: number | null = null
let latencyTimer: number | null = null
let reconnectTimer: number | null = null

/** 待喂给 SourceBuffer 的分片。必须是队列，见 pump() 的说明。 */
let pending: ArrayBuffer[] = []
/** 收到过初始化段没有。**每块分片都要求它为真**，否则 MSE 会静默丢数据。 */
let gotInit = false
/** 收到过分片没有。用来区分"连上了没数据"和"有数据但还没到关键帧"。 */
let gotFragment = false
/** 下一次 updateend 要不要无条件对齐播放头。 */
let needsAlign = false
/** 自动重连的退避。 */
let reconnectDelay = 800

const MAX_RECONNECT_MS = 15000

/* ------------------------------------------------------------------ 工具 */

function pickMime(): string {
  if (typeof MediaSource === 'undefined') return ''
  for (const m of MIME_CANDIDATES) {
    try {
      if (MediaSource.isTypeSupported(m)) return m
    } catch {
      // 某些实现会直接抛，当作不支持继续试下一个
    }
  }
  return ''
}

/**
 * 读 fMP4 第一个 box 的类型（`ftyp` / `moov` / `moof` / `styp`）。
 *
 * 用来分辨"这块是初始化段还是媒体分片"。**不是可选的锦上添花**：如果第一块
 * 就是 `moof`（初始化段丢了或顺序错了），继续喂下去的结果是 MSE 静默丢弃
 * 后续所有数据 —— 界面停在"等待关键帧"转圈，而真正的原因（缺初始化段）
 * 一个字都不会出现。
 */
function boxType(buf: ArrayBuffer): string {
  if (buf.byteLength < 8) return ''
  const v = new Uint8Array(buf, 4, 4)
  return String.fromCharCode(v[0], v[1], v[2], v[3])
}

function clearTimer(t: number | null): null {
  if (t !== null) window.clearTimeout(t)
  return null
}

function fail(message: string, s: PlayerState = 'error') {
  playerMessage.value = message
  playerState.value = s
  firstDataTimer = clearTimer(firstDataTimer)
  fragmentTimer = clearTimer(fragmentTimer)
  closeSocket()
}

function closeSocket() {
  if (socket) {
    const s = socket
    socket = null
    // 先摘掉回调再关：否则我们自己发起的 close 会走进 onclose，
    // 把刚设好的状态又改写成"已断开"，还会排一次重连。
    s.onopen = null
    s.onmessage = null
    s.onclose = null
    s.onerror = null
    try {
      s.close()
    } catch {
      // 已经关了就无所谓
    }
  }
}

/* ------------------------------------------------------------- 缓冲与播放 */

/**
 * 把队列头的分片喂进去。
 *
 * `SourceBuffer.updating` 为 true 时调 `appendBuffer` 会抛 `InvalidStateError`
 * —— 这是 MSE 最常见的崩法，因为网络分片到达的节奏和 SourceBuffer 消化的
 * 节奏完全无关。所以分片一律先进 pending，靠 updateend 驱动一块一块地喂。
 */
function pump() {
  const sb = sourceBuffer
  if (!sb || !mediaSource || mediaSource.readyState !== 'open') return
  if (sb.updating) return
  const chunk = pending.shift()
  if (chunk === undefined) return
  try {
    sb.appendBuffer(chunk)
  } catch (err) {
    pending.unshift(chunk)
    if (err instanceof DOMException && err.name === 'QuotaExceededError') {
      trimBuffer(true)
    } else {
      fail(`写入媒体缓冲失败：${(err as Error).message}`)
    }
  }
}

/**
 * 裁掉播过的部分。
 *
 * @param aggressive QuotaExceededError 之后的抢救：MSE 的配额是按 SourceBuffer
 *   总量算的，一次退 2 秒通常就够了；退得更多反而会把正在播的位置也切掉，
 *   触发一次可见的卡顿。
 */
function trimBuffer(aggressive = false) {
  const sb = sourceBuffer
  const v = videoRef.value
  if (!sb || !v || sb.updating || sb.buffered.length === 0) return
  try {
    const start = sb.buffered.start(0)
    const keep = aggressive ? 2 : MAX_BUFFER_S
    if (v.currentTime - start <= keep) return
    sb.remove(start, Math.max(start, v.currentTime - keep / 2))
  } catch {
    // 这次移除没成功，等下一个 updateend 再试
  }
}

/**
 * 把播放头对齐到直播边缘。
 *
 * 这是整个页面的关键一步 —— 见文件开头关于 `base_dts_us_` 的说明。没有它，
 * "程序跑了一会儿之后才打开预览页"就永远是黑屏。
 */
function alignToLiveEdge() {
  const sb = sourceBuffer
  const v = videoRef.value
  if (!sb || !v || sb.updating || sb.buffered.length === 0) return

  // 取**最后一段**缓冲。直播只有一段，但裁过之后偶尔会出现短暂的两段，
  // 而"最新的数据"永远在最后一段里。
  const last = sb.buffered.length - 1
  const start = sb.buffered.start(last)
  const end = sb.buffered.end(last)

  // 这一段还不够长：立刻跳过去会让播放头停在一个马上见底的位置，
  // 浏览器下一帧就会 stall 一次。等下一个分片。
  if (end - start < TARGET_LATENCY + 0.3) return

  const target = end - TARGET_LATENCY

  if (needsAlign) {
    needsAlign = false
    // 已经在附近就别动 —— 每次 updateend 都 seek 会让画面不停抖。
    if (Math.abs(v.currentTime - target) > 0.2) v.currentTime = target
    return
  }

  const cur = v.currentTime
  const inside = cur >= start && cur <= end
  // 不在区间里（刚连上、或缓冲被裁过头）→ 立刻对齐；
  // 在区间里但落后太多（用户按了暂停、或者解码卡过）→ 追上去。
  if (!inside || target - cur > MAX_DRIFT) v.currentTime = target
}

function onUpdateEnd() {
  // 先对齐再喂下一块：对齐会改 currentTime，而它必须在没有 append 在飞的时候做。
  alignToLiveEdge()
  trimBuffer()
  updateLatency()
  pump()
}

function updateLatency() {
  const sb = sourceBuffer
  const v = videoRef.value
  if (!sb || !v || sb.buffered.length === 0) {
    latency.value = 0
    return
  }
  const end = sb.buffered.end(sb.buffered.length - 1)
  const d = end - v.currentTime
  latency.value = d > 0 && Number.isFinite(d) ? d : 0
}

/* ---------------------------------------------------------------- 播放器 */

function start() {
  wantStream.value = true
  teardownPipeline()
  gotInit = false
  gotFragment = false
  needsAlign = true
  playerMessage.value = ''
  pending = []

  const v = videoRef.value
  if (!v) return

  const mime = pickMime()
  if (!mime) {
    playerState.value = 'unsupported'
    playerMessage.value =
      '当前浏览器不支持 MSE，或者没有 avc1（H.264）解码器。请改用 Chrome / Edge 打开本页。'
    return
  }

  playerState.value = 'connecting'

  const ms = new MediaSource()
  mediaSource = ms
  objectUrl = URL.createObjectURL(ms)
  v.src = objectUrl
  // 静音 + playsinline 是自动播放能通过策略的两个前提；这一页是监控画面，
  // 默认静音本来就是合理的行为（而且用户可能同时开着好几路）。
  v.muted = muted.value
  v.play().catch(() => {
    // 被策略拦下不致命：用户点一下画面上的播放键就行，所以只记一句提示。
    playerMessage.value = '浏览器拦下了自动播放，点一下画面即可开始。'
  })

  ms.addEventListener(
    'sourceopen',
    () => {
      if (mediaSource !== ms) return
      try {
        // SourceBuffer 只能在 sourceopen 之后建，更早建会抛 InvalidStateError。
        sourceBuffer = ms.addSourceBuffer(mime)
      } catch (err) {
        fail(`无法创建解码缓冲：${(err as Error).message}`)
        return
      }
      // segments 模式：时间戳完全按分片里的 tfdt 走，不由 MSE 自己推。
      // 这正是我们要的 —— 后端的分片带完整时间戳，而默认的 sequence 模式
      // 会把它们全部重排到从 0 开始，那样对齐逻辑就失去意义了。
      sourceBuffer.mode = 'segments'
      sourceBuffer.addEventListener('updateend', onUpdateEnd)
      // duration 设成 Infinity 才是直播：不设的话播放头走到已缓冲的末尾就会
      // 触发 ended，界面停在最后一帧，看起来就是"播了两秒就卡住"。
      try {
        ms.duration = Infinity
      } catch {
        // 个别实现不允许，不影响播放
      }
      openSocket()
    },
    { once: true },
  )

  startLatencyClock()
}

function openSocket() {
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:'
  let ws: WebSocket
  try {
    ws = new WebSocket(`${scheme}//${location.host}/ws/preview`)
  } catch (err) {
    fail(`无法建立预览连接：${(err as Error).message}`)
    return
  }
  ws.binaryType = 'arraybuffer'
  socket = ws

  // 从"连上"开始计时，而不是从"发出连接"开始 —— 否则握手那几秒也被算进来，
  // 后端没起来时提示会来得莫名其妙。
  ws.onopen = () => {
    firstDataTimer = clearTimer(firstDataTimer)
    firstDataTimer = window.setTimeout(() => {
      if (!gotInit) {
        fail(
          `连接已建立，但 ${FIRST_DATA_TIMEOUT_MS / 1000} 秒内没有收到初始化段。` +
            '通常说明后端还没有开始采集编码（没有画面变化时 DXGI 不产出帧）。',
          'error',
        )
      }
    }, FIRST_DATA_TIMEOUT_MS)
  }

  ws.onmessage = (ev: MessageEvent) => {
    if (!(ev.data instanceof ArrayBuffer)) return
    firstDataTimer = clearTimer(firstDataTimer)

    const kind = boxType(ev.data)
    if (!gotInit) {
      // 第一块必须是初始化段。收到 moof 说明顺序错了 —— 而这件事必须明说，
      // 因为继续喂下去 MSE 会静默丢弃全部数据，界面只会一直转圈。
      if (kind === 'moof' || kind === 'styp') {
        fail('后端先发来了媒体分片而没有初始化段，无法解码。请刷新页面重试。')
        return
      }
      gotInit = true
      playerState.value = 'waiting-fragment'
      playerMessage.value = ''
      // 初始化段本身不含画面，但必须先于任何分片 append 进去。
      pending.push(ev.data)
      pump()
      startFragmentTimer()
      return
    }

    gotFragment = true
    fragmentTimer = clearTimer(fragmentTimer)

    if (playerState.value !== 'playing') {
      playerState.value = 'playing'
      playerMessage.value = ''
    }

    if (pending.length >= MAX_QUEUE) {
      // 消费端已经跟不上了（标签页被切走、或者解码器卡住）。继续堆只会把
      // 内存吃光，而丢中间几块等于给它一条有洞的时间轴 —— 那正是 MSE 最
      // 不擅长的东西。整条重建，代价只是黑一下。
      restart('预览解码跟不上，已重新连接。')
      return
    }
    pending.push(ev.data)
    pump()
  }

  ws.onclose = (ev: CloseEvent) => {
    socket = null
    firstDataTimer = clearTimer(firstDataTimer)
    fragmentTimer = clearTimer(fragmentTimer)

    if (!wantStream.value) return

    // 1008（POLICY_VIOLATION）= 后端说"你没登录"。
    // **必须和"网络断了"分开**：cookie 过期之后重连一百次还是 1008，而
    // 用户会看到界面在"已断开/连接中"之间来回跳却不知道原因。
    if (ev.code === 1008) {
      wantStream.value = false
      teardownPipeline()
      noteUnauthorized()
      playerState.value = 'ended'
      playerMessage.value = '登录已过期，正在跳转到登录页…'
      router.replace({ name: 'login', query: { next: route.fullPath } })
      return
    }

    playerState.value = gotFragment ? 'ended' : 'error'
    playerMessage.value = `预览通道已断开（${ev.code || '连接中断'}），正在自动重连…`
    scheduleReconnect()
  }

  // onerror 后面必定跟 onclose，状态收敛只放在 onclose 一处。
  ws.onerror = () => {}
}

function startFragmentTimer() {
  fragmentTimer = clearTimer(fragmentTimer)
  fragmentTimer = window.setTimeout(() => {
    if (!gotFragment) {
      playerState.value = 'waiting-fragment'
      playerMessage.value =
        `已经连上并拿到初始化段，${FIRST_FRAGMENT_TIMEOUT_MS / 1000} 秒内还没有等到下一个关键帧。` +
        '分片只在关键帧处产生，所以 GOP 间隔越大这里等得越久 —— ' +
        '去「配置」把关键帧间隔调到 2 秒以内即可。'
    }
  }, FIRST_FRAGMENT_TIMEOUT_MS)
}

function scheduleReconnect() {
  if (!wantStream.value || reconnectTimer !== null) return
  const delay = reconnectDelay
  reconnectDelay = Math.min(reconnectDelay * 2, MAX_RECONNECT_MS)
  reconnectTimer = window.setTimeout(() => {
    reconnectTimer = null
    if (wantStream.value) start()
  }, delay)
}

function restart(reason: string) {
  playerState.value = 'connecting'
  playerMessage.value = reason
  teardownPipeline()
  if (wantStream.value) scheduleReconnect()
}

/** 拆掉播放链路。**不动 `wantStream`** —— 那是"用户想不想看"，和这个是两件事。 */
function teardownPipeline() {
  closeSocket()
  pending = []
  needsAlign = true

  const sb = sourceBuffer
  sourceBuffer = null
  if (sb) {
    sb.removeEventListener('updateend', onUpdateEnd)
    try {
      // 结束 MediaSource 前必须确认它还是 open 且没有 append 在飞：否则抛异常。
      if (mediaSource && mediaSource.readyState === 'open' && !sb.updating) {
        mediaSource.endOfStream()
      }
    } catch {
      // 结束失败无所谓，下面直接把整个 MediaSource 丢掉
    }
  }
  mediaSource = null

  const v = videoRef.value
  if (v) {
    // 先摘 src 再撤销 URL：反过来的话，元素会短暂地指向一个已经失效的
    // blob URL，控制台里会多一条和真正原因无关的报错。
    v.removeAttribute('src')
    // load() 是必须的：只是移除 src 的话，浏览器会保持当前解码状态，
    // 下一次赋 src 时可能沿用旧的 pipeline。
    v.load()
  }
  if (objectUrl) {
    URL.revokeObjectURL(objectUrl)
    objectUrl = ''
  }
}

/** 用户按了「停止」。和"断线"不同 —— 这一份意愿会让自动重连停手。 */
function stop() {
  wantStream.value = false
  reconnectTimer = clearTimer(reconnectTimer)
  firstDataTimer = clearTimer(firstDataTimer)
  fragmentTimer = clearTimer(fragmentTimer)
  stopLatencyClock()
  teardownPipeline()
  pending = []
  gotInit = false
  gotFragment = false
  latency.value = 0
  if (playerState.value !== 'unsupported') {
    playerState.value = 'idle'
    playerMessage.value = ''
  }
}

/* -------------------------------------------------------------- 计时与交互 */

function startLatencyClock() {
  if (latencyTimer !== null) return
  // 1 秒一次就够：这个数只是给人一个"跟得上还是落后了"的量级。
  latencyTimer = window.setInterval(() => {
    updateLatency()
    const v = videoRef.value
    if (v) paused.value = v.paused
  }, 1000)
}

function stopLatencyClock() {
  if (latencyTimer !== null) {
    window.clearInterval(latencyTimer)
    latencyTimer = null
  }
}

function togglePlay() {
  const v = videoRef.value
  if (!v) return
  if (v.paused) {
    // 从暂停恢复时必须重新对齐：暂停期间缓冲还在涨，直接播会从很旧的位置开始。
    needsAlign = true
    v.play().catch(() => {})
  } else {
    v.pause()
  }
  paused.value = v.paused
}

/** 回到直播边缘。暂停久了、或者解码落后太多时按这个。 */
function jumpToLive() {
  needsAlign = true
  alignToLiveEdge()
  const v = videoRef.value
  if (v && v.paused) v.play().catch(() => {})
}

function toggleMute() {
  const v = videoRef.value
  if (!v) return
  v.muted = !v.muted
  muted.value = v.muted
}

async function toggleFullscreen() {
  const el = stageRef.value
  if (!el) return
  try {
    if (document.fullscreenElement) await document.exitFullscreen()
    else await el.requestFullscreen()
  } catch {
    // 非安全上下文（http://192.168.x.x）下 requestFullscreen 可能不存在或被拒。
    playerMessage.value = '当前环境不允许全屏，请用浏览器自己的全屏（F11）。'
  }
}

function onFullscreenChange() {
  isFullscreen.value = document.fullscreenElement === stageRef.value
}

/**
 * 用画面的真实宽高比撑住舞台。
 *
 * 写死 16:9 的问题是：抓到的是一整块桌面，常见的是 3:2（2880x1920）或
 * 21:9。比例对不上时 `object-fit: contain` 会补出黑边，图像实际能用的像素
 * 少一大截 —— 而这是监控画面，少的那一截正是要看的地方。
 */
function syncVideoAspect() {
  const v = videoRef.value
  if (v && v.videoWidth > 0 && v.videoHeight > 0) {
    videoAspect.value = v.videoWidth / v.videoHeight
  }
}

/* ------------------------------------------------------------------ 快照 */

const snapshotNonce = ref(0)
const snapshotState = ref<'loading' | 'ok' | 'fail'>('loading')
const autoSnapshot = ref(false)
let snapshotTimer: number | null = null

/**
 * **必须用 `/api/snapshot.jpg`，不是 `/snapshot.jpg`。**
 *
 * 后者是 ONVIF 的 `GetSnapshotUri` 报给 NVR 的那条路：未登录时它回 401 +
 * `WWW-Authenticate: Digest`，而浏览器看见这个头会弹一个**系统级**的用户名/
 * 口令框 —— 和我们的登录页是两套东西，用户在里面输什么都不对。
 * `/api/` 那条只认会话 cookie，永远不发挑战。
 */
const snapshotUrl = computed(() => `/api/snapshot.jpg?t=${snapshotNonce.value}`)

function refreshSnapshot() {
  // 加时间戳是为了绕开浏览器缓存：同一个 URL 被缓存过之后，刷新按钮会毫无反应。
  // （后端也发了 no-store，但 NVR 和浏览器对它的尊重程度不一致，双重保险。）
  snapshotState.value = 'loading'
  snapshotNonce.value = Date.now()
}

function applyAutoSnapshot() {
  if (snapshotTimer !== null) {
    window.clearInterval(snapshotTimer)
    snapshotTimer = null
  }
  if (autoSnapshot.value) {
    snapshotTimer = window.setInterval(refreshSnapshot, 1000)
  }
}

/* --------------------------------------------------------------- 生命周期 */

onMounted(() => {
  document.addEventListener('fullscreenchange', onFullscreenChange)
  // 进页面就开始播：这一页存在的唯一理由就是看画面，让用户先点一下
  // 「开始预览」是纯多余的一步。
  start()
})

onBeforeUnmount(() => {
  stop()
  document.removeEventListener('fullscreenchange', onFullscreenChange)
  if (snapshotTimer !== null) window.clearInterval(snapshotTimer)
})

/* ------------------------------------------------------------------ 诊断 */

/**
 * 预览链路的自检信息。
 *
 * 这一块是给"预览是黑的"这个具体问题准备的：`preview_fragments` 不涨就说明
 * 编码那边根本没产分片（GOP 太长、或者桌面静止没有帧），而 `preview_ready`
 * 为假说明连初始化段都还没出来。没有这几个数就只能猜。
 */
const previewDiag = computed(() => [
  { label: '分片产出', value: formatInt(stats.value.preview_fragments), ok: stats.value.preview_fragments > 0 },
  { label: '初始化段', value: stats.value.preview_ready ? '已就绪' : '未产生', ok: stats.value.preview_ready },
  { label: '预览观看者', value: formatInt(stats.value.preview_clients), ok: stats.value.preview_clients > 0 },
  { label: '关键帧', value: formatInt(stats.value.keyframes), ok: stats.value.keyframes > 0 },
])
</script>

<template>
  <div class="page-head">
    <h1>预览</h1>
    <span class="hint">
      画面走 WebSocket 上的 fMP4，延迟约 1 秒；和 NVR 拉的 RTSP 是两条独立的链路。
    </span>
  </div>

  <div class="card">
    <div class="row" style="justify-content: space-between; margin-bottom: 12px">
      <h2 style="margin: 0">实时画面</h2>
      <span
        class="pill"
        :class="{
          'is-online': stateTone === 'good',
          'is-connecting': stateTone === 'warn',
          'is-offline': stateTone === 'crit',
        }"
      >
        <i class="dot"></i>预览：{{ stateText }}
      </span>
    </div>

    <div ref="stageRef" class="stage">
      <video
        ref="videoRef"
        muted
        playsinline
        :style="{ aspectRatio: videoAspect }"
        :class="{ 'is-clickable': playerState === 'playing' }"
        @click="togglePlay"
        @loadedmetadata="syncVideoAspect"
      ></video>

      <!-- 画面上的实时读数。`pointer-events: none`，不挡点击。 -->
      <div v-if="playerState === 'playing'" class="stage-overlay">
        <span class="chip">
          <Icon name="activity" :size="12" />{{ stats.effective_fps.toFixed(0) }} fps
        </span>
        <span class="chip">
          <Icon name="zap" :size="12" />{{ formatBytes(stats.bytes) }}
        </span>
        <span class="chip">
          <Icon name="clock" :size="12" />延迟 {{ latency.toFixed(1) }}s
        </span>
      </div>

      <!-- 中央状态。只在"没有画面可看"的时候出现。 -->
      <div v-if="playerState !== 'playing'" class="stage-placeholder">
        <Icon
          :name="playerState === 'unsupported' || playerState === 'error' ? 'alert' : 'loader'"
          :size="30"
          :stroke="1.5"
        />
        <div class="ph-title">{{ stateText }}</div>
        <div v-if="playerMessage" class="ph-text">{{ playerMessage }}</div>
        <div v-else-if="playerState === 'connecting'" class="ph-text">
          正在建立预览通道…
        </div>
      </div>
    </div>

    <!-- 播放中的补充提示（自动播放被拦、全屏不可用一类）。 -->
    <p v-if="playerMessage && playerState === 'playing'" class="hint" style="margin: 10px 0 0">
      {{ playerMessage }}
    </p>

    <div class="actions" style="margin-top: 12px">
      <button v-if="!wantStream || playerState === 'idle'" class="primary" @click="start">
        <Icon name="play" :size="15" />开始预览
      </button>
      <template v-else>
        <button @click="togglePlay">
          <Icon :name="paused ? 'play' : 'pause'" :size="15" />{{ paused ? '继续' : '暂停' }}
        </button>
        <button :class="{ primary: isBehind }" @click="jumpToLive">
          <Icon name="refresh" :size="15" />回到最新
        </button>
        <button class="ghost" @click="stop"><Icon name="x" :size="15" />停止</button>
      </template>
      <button class="ghost" @click="toggleMute">
        <Icon name="signal" :size="15" />{{ muted ? '取消静音' : '静音' }}
      </button>
      <button class="ghost" @click="toggleFullscreen">
        <Icon name="expand" :size="15" />{{ isFullscreen ? '退出全屏' : '全屏' }}
      </button>
      <span v-if="isBehind" class="hint tone-warn">
        已落后 {{ latency.toFixed(1) }} 秒，点「回到最新」追上去。
      </span>
    </div>
  </div>

  <!-- 预览链路自检。「预览是黑的」时，这一块比日志有用得多。 -->
  <div class="card">
    <h2>预览链路自检</h2>
    <div class="tiles">
      <div v-for="d in previewDiag" :key="d.label" class="tile">
        <div class="tile-label">{{ d.label }}</div>
        <div class="tile-value small" :class="d.ok ? 'tone-good' : 'tone-warn'">{{ d.value }}</div>
      </div>
    </div>
    <p class="hint" style="margin-bottom: 0">
      分片只在<strong>关键帧</strong>处产生，所以「关键帧」不涨时预览必然是黑的 ——
      去「配置」把关键帧间隔（GOP）调到 2 秒以内。
      <template v-if="!stats.preview_ready">
        <br />初始化段还没产生，说明后端还没编码出第一帧：桌面内容完全静止时
        DXGI 不会交出帧，动一下鼠标即可。
      </template>
      <template v-if="stats.preview_clients_kicked > 0">
        <br />有 {{ formatInt(stats.preview_clients_kicked) }} 次因积压被服务端断开
        —— 通常是浏览器标签页被切走后还在收流，切回来会自动重连。
      </template>
      <template v-if="statsConn !== 'online'">
        <br />统计推送当前未连接，上面这些数字停在最后一次收到的值。
      </template>
    </p>
  </div>

  <div class="card">
    <div class="row" style="justify-content: space-between; margin-bottom: 12px">
      <h2 style="margin: 0">快照</h2>
      <label class="field check" style="padding-top: 0">
        <input v-model="autoSnapshot" type="checkbox" @change="applyAutoSnapshot" />
        <span class="track"></span>
        <span class="field-label">每秒自动刷新</span>
      </label>
    </div>

    <div class="stage scroll-x">
      <img
        v-show="snapshotState === 'ok'"
        :src="snapshotUrl"
        alt="桌面快照"
        class="snapshot"
        @load="snapshotState = 'ok'"
        @error="snapshotState = 'fail'"
      />
      <div v-if="snapshotState !== 'ok'" class="placeholder stage-placeholder">
        <Icon :name="snapshotState === 'loading' ? 'loader' : 'alert'" :size="26" :stroke="1.5" />
        <div class="ph-title">
          {{ snapshotState === 'loading' ? '正在加载快照…' : '快照不可用' }}
        </div>
        <div v-if="snapshotState === 'fail'" class="ph-text">
          后端还没有采到画面（还没开始编码时会返回 503），或者会话已过期。
        </div>
      </div>
    </div>

    <div class="actions" style="margin-top: 12px">
      <button @click="refreshSnapshot"><Icon name="refresh" :size="15" />刷新快照</button>
      <span class="hint">
        快照是后端单独抓一帧编成 JPEG，和预览通道互不影响 —— 预览出问题时它通常还是好的。
      </span>
    </div>
  </div>
</template>
