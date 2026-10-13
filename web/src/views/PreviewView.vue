<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import Icon from '../components/Icon.vue'
import { fetchConfig, fetchPreviewChannel, setPreviewChannel } from '../api'
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
 * 播放这一侧的两个数，**跟着后端的延时档位走**。
 *
 * 它们与 `capture.latency`（后端那一半：编码线程数与关键帧间隔）是**同一次
 * 选择的两半**。后端那半决定"数据多久到浏览器手上"，这里决定"拿到之后播放头
 * 停在多后面"。两边都要跟着档位走，否则选了「极致低延迟」而播放器仍旧停在
 * 0.5 秒，用户盯着「真实延迟」量出来的数就只降了后端那一项，而他会以为整个
 * 档位没生效 —— 这正是陷阱 90 第一面那个形状：**每一处代码都做到了它承诺的
 * 事，只有用户关心的那个数字没动。**
 *
 * `balanced` 这一档**就是旧版本写死的两个值**（0.5 / 2.5），所以不选档位的
 * 默认行为一字未变。
 *
 * - `target`（目标延迟）：播放头停在"最新数据末尾"往前这么多秒。太小
 *   （<0.3）会频繁 stall —— 网络稍微抖一下缓冲就见底了；太大就白送延迟。
 * - `drift`（最大落伍）：落后超过这么多秒才重新对齐，否则每来一块都会 seek
 *   一下，画面会不停跳。**它必须跟着 `target` 一起变小** —— 它的含义是
 *   "卡顿之后最长容忍落伍这么多"，而落伍是相对**已经拿到的数据**算的。阈值
 *   比目标延迟大得多时，网络抖一次留下的那段"多出来的落后"就永远留在那儿了。
 */
const PREVIEW_LATENCY_BY_TIER: Record<string, { target: number; drift: number }> = {
  realtime: { target: 0.25, drift: 1.0 },
  balanced: { target: 0.5, drift: 2.5 },
  quality: { target: 1.0, drift: 4.0 },
}
/** 档位认不出来时的落点（`custom` 也走这里）。与旧版本写死的值相同。 */
const PREVIEW_LATENCY_DEFAULT = { target: 0.5, drift: 2.5 }

/**
 * 本机覆盖的存储键。
 *
 * **按浏览器站点存**，因为它取决于看着这块屏幕的这台机器和这条网络，而不是
 * 设备属性 —— 设备给默认值、决定权留给本机，是这两件事唯一正确的关系。
 */
const PREVIEW_LATENCY_KEY = 'd2o.preview.latency'

/**
 * 读本机覆盖。
 *
 * 存的是 `{key, target, drift}` 三项，而且**三项必须自洽**（`key` 认得出来，
 * 且那两个数正好等于该档的值）—— 不自洽就整份丢掉，回到档位默认。
 *
 * 为什么不只存两个数：那样下拉框就得靠"数值反查是哪一档"来显示当前选项，而
 * 一旦有人手改过 localStorage，反查会得到空 —— 界面上表现为"选了什么看不见"。
 * 存键名则让"选中的是哪一项"永远是确定的。
 *
 * 为什么不接受任意数值：那两个数的取值空间是"看着这块屏幕的这台机器和这条
 * 网络"，不是一个可以由用户随手填的东西；给三个成对的预设，比给两个自由输入框
 * 少一整类"目标是 0.25、阈值是 4.0"这种自相矛盾的组合（它的症状是网络抖一次
 * 就永久落后，很难查到是配置问题）。
 */
function readLocalLatencyOverride(): { key: string; target: number; drift: number } | null {
  try {
    const raw = window.localStorage.getItem(PREVIEW_LATENCY_KEY)
    if (!raw) return null
    const o = JSON.parse(raw) as { key?: unknown; target?: unknown; drift?: unknown }
    const key = typeof o?.key === 'string' ? o.key : ''
    const preset = PREVIEW_LATENCY_BY_TIER[key]
    if (!preset) return null
    if (o?.target !== preset.target || o?.drift !== preset.drift) return null
    return { key, target: preset.target, drift: preset.drift }
  } catch {
    // 隐私模式下 `localStorage` 的访问本身会抛。一份拿不到的覆盖等于没有覆盖，
    // 不该因为它让整页起不来。
    return null
  }
}

/** 本机覆盖。`null` = 跟随档位。**必须是 ref**：界面上那个下拉框要跟着它变。 */
const localLatency = ref(readLocalLatencyOverride())

/** 播放头要多贴着直播边缘。**响应式的**：`isBehind` 那个 computed 要跟着它重算。 */
const targetLatency = ref(localLatency.value?.target ?? PREVIEW_LATENCY_DEFAULT.target)
/** 落后超过这么多秒才重新对齐。 */
const maxDrift = ref(localLatency.value?.drift ?? PREVIEW_LATENCY_DEFAULT.drift)
/** 这两个数现在是从哪来的（本机覆盖 / 档位 / 默认），界面上要说得出来。 */
const latencySource = ref<'local' | 'tier' | 'default'>(localLatency.value ? 'local' : 'default')
/** 后端配置里的档位字面量（`realtime` / `balanced` / `quality` / `custom`）。 */
const configTier = ref('')

/**
 * 后端的关键帧间隔（秒）。0 = 还没读到配置。
 *
 * **它决定的是"中途连上预览之后多久能看到第一幅画"，不是"看到之后有多滞后"。**
 * 这两件事必须分开（计划里 (d) 那一条）：分片现在每 0.25 秒来一个，所以画面
 * **数据**到得很快；但一副能解码的画面要等下一个 IDR，最坏一整个关键帧间隔。
 * 界面上这两句分开写，否则用户会把"等了 2 秒才出画"归到分片节奏上，而去调
 * 一个不相干的旋钮。
 */
const configGopSeconds = ref(0)

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

/**
 * 播放头离"我们已经拿到的数据的末尾"多远，秒。
 *
 * ⚠️ **这不是画面延迟。** 它的减数是我们手上数据的时刻，不是**现在的时刻** ——
 * 所以它稳定在 `TARGET_LATENCY` 上（0.5 秒），**和链路好不好没关系**。它真正
 * 回答的是"缓冲里还压着多少"，也就是下面的「缓冲余量」。
 */
const latency = ref(0)

/**
 * 画面**真正**落后现实多少秒：`服务端的现在 − 这一帧被拍下来的时刻`。
 *
 * 它需要后端告诉我们"媒体时间 0 点对应的墙钟"（WS 上那帧 `{"type":"t0",…}`
 * 文本消息），才能把 `<video>.currentTime` 换算成墙钟。没收到之前是 `null`
 * —— 界面上显示「—」，而不是拿 `latency` 顶上：那是一个**看起来合理的假数**，
 * 而假数比"未知"坏得多（用户会拿它去对账，然后发现怎么调都不动）。
 */
const trueLatency = ref<number | null>(null)

/**
 * 其中"还没送到我们手上"的那一段：`服务端的现在 − 我们手上数据的末尾`。
 *
 * **它才是后端那个旋钮（分片节奏）在界面上的读数。** 三个数的关系是
 * `真实延迟 = 分片滞后 + 缓冲余量`，所以改了分片节奏时只有这一个会动 ——
 * 只显示一个数的话，你会看到它纹丝不动，然后以为改动没生效。
 */
const transportLag = ref<number | null>(null)

/** 媒体时间 0 点对应的墙钟（Unix 毫秒）。0 = 后端还没报，一切都算不出来。 */
const t0WallMs = ref(0)

/**
 * 本机时钟与服务端时钟之差（毫秒）：`Date.now() − server_now_ms`。
 *
 * 每条 t0 消息都带着**它发出去那一刻**的服务端时刻，所以每次收到都重量一次
 * 是对的、也是必须的 —— 但**绝不能在读数的时候现算**：那会把这一来一回的网络
 * 时间混进差值里，而延迟是个差值，混进去的部分正好等于网络抖动。
 */
let clockSkewMs = 0

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

/** 后端报过 0 点没有。没报过就只能显示「—」—— 见 `trueLatency` 那段。 */
const hasT0 = computed(() => t0WallMs.value > 0)

/** 真实延迟的显示文本，未知时是「—」。 */
const trueLatencyText = computed(() =>
  trueLatency.value === null ? '—' : `${trueLatency.value.toFixed(1)}s`,
)
const transportText = computed(() =>
  transportLag.value === null ? '—' : `${transportLag.value.toFixed(1)}s`,
)

/**
 * 播放中且追不上直播边缘时，给一个"回到最新"的按钮。
 *
 * 判据用**缓冲余量**而不是真实延迟：`alignToLiveEdge()` 那个按钮要修的就是
 * "播放头离缓冲末尾太远"，而它用的阈值正是 `MAX_DRIFT`。拿真实延迟来判会把
 * 两件事混起来 —— 后端分片慢了的时候真实延迟也大，那时按「回到最新」没用
 * （播放头已经在缓冲末尾了），点了只会白跳一下。
 */
const isBehind = computed(() => playerState.value === 'playing' && latency.value > maxDrift.value)

/* -------------------------------------------------------- 档位与本机覆盖 */

/**
 * 把后端的延时档位映到播放这一侧的两个数。
 *
 * **本机覆盖优先**：它在构造时就读过了，档位再也不会覆盖它。挡在这里而不是挡在
 * `loadLatencyTier()` 里，是因为将来还会有别的调用者（改档位后重新拉一次配置），
 * 而"覆盖永远赢"这条规则只该写一遍。
 *
 * 档位不认识（`custom`、或者后端比前端旧、根本没有这个字段）时落在默认档上并把
 * `latencySource` 标成 `default` —— 界面上要能看出"这是默认值，不是你要的那一档"，
 * 不然 `custom` 档下那两个数是从哪来的就没人说得清了。
 */
function applyTierLatency(tier: string) {
  configTier.value = tier
  if (localLatency.value) {
    latencySource.value = 'local'
    return
  }
  const p = PREVIEW_LATENCY_BY_TIER[tier]
  if (!p) {
    targetLatency.value = PREVIEW_LATENCY_DEFAULT.target
    maxDrift.value = PREVIEW_LATENCY_DEFAULT.drift
    latencySource.value = 'default'
    return
  }
  targetLatency.value = p.target
  maxDrift.value = p.drift
  latencySource.value = 'tier'
}

/**
 * 改本机覆盖。`key` 传空串 = 跟随档位（把覆盖删掉）。
 *
 * **写入失败不报错**：`localStorage` 可能是关着的（隐私模式、站点数据被拦），
 * 那时这次选择只对当前这一次访问有效 —— 而它确实生效了（内存里的值改了），
 * 只是刷新之后会回去。为这件事弹一条错误会让人以为选择没生效。
 */
function setLocalLatency(key: string) {
  if (!key) {
    localLatency.value = null
  } else {
    const preset = PREVIEW_LATENCY_BY_TIER[key]
    if (!preset) return
    localLatency.value = { key, target: preset.target, drift: preset.drift }
  }
  try {
    if (localLatency.value) {
      window.localStorage.setItem(PREVIEW_LATENCY_KEY, JSON.stringify(localLatency.value))
    } else {
      window.localStorage.removeItem(PREVIEW_LATENCY_KEY)
    }
  } catch {
    // 见上。
  }
  // 重新落一次：覆盖删掉之后要回到档位给的那两个数。
  applyTierLatency(configTier.value)
}

/** 下拉框选中的那一项。`''` = 跟随档位。 */
const localPick = computed({
  get: () => localLatency.value?.key ?? '',
  set: (v: string) => setLocalLatency(v),
})

/**
 * 进页面时问一次后端是哪个档位。
 *
 * **失败不报错、不挡播放**：这一页的主业是出画面，读不到档位最多是那两个数落在
 * 默认值上（旧版本的行为），而弹一条错误会让人以为预览坏了。401 由 `api.ts` 的
 * 统一处理接管，这里只管"不知道就算了"。
 */
async function loadLatencyTier() {
  try {
    const res = await fetchConfig()
    if (res.ok && res.data?.capture) {
      applyTierLatency(String(res.data.capture.latency ?? ''))
      const gop = Number(res.data.capture.gop_seconds)
      configGopSeconds.value = Number.isFinite(gop) && gop > 0 ? gop : 0
    }
  } catch {
    // 见上。
  }
}

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
  if (end - start < targetLatency.value + 0.3) return

  const target = end - targetLatency.value

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
  if (!inside || target - cur > maxDrift.value) v.currentTime = target
}

function onUpdateEnd() {
  // 先对齐再喂下一块：对齐会改 currentTime，而它必须在没有 append 在飞的时候做。
  alignToLiveEdge()
  trimBuffer()
  updateLatency()
  pump()
}

/**
 * 服务端的"现在"（Unix 毫秒）。
 *
 * 延迟是个**差值**，这条式子里的时钟偏差会被减掉 —— 前提是它是个常数，
 * 所以它来自收到 t0 那一刻量的一次，不是现算的（见 `clockSkewMs`）。
 */
function serverNowMs(): number {
  return Date.now() - clockSkewMs
}

/**
 * 三个读数一起更新。
 *
 * 关键区别（这是这个页面最容易搞错的一处）：`buffered.end` 是"我们**已经拿到**
 * 的数据的末尾"，不是"画面的现在"。而分片是后端按关键帧节奏切出来发的，所以
 * 一帧从被拍下到落到我们手上，中间还压着半到一个 GOP。旧版拿它减 `currentTime`
 * 报出来的那个数**稳定在 `TARGET_LATENCY` 上**，看着很合理，却和链路好不好
 * 毫无关系 —— 一把量不出真东西的尺子。
 *
 * 现在按墙钟算：`这一帧是什么时候拍的 = t0_wall_ms + 媒体时间 × 1000`。
 * 这条关系成立的前提是媒体时间与墙钟 1:1 推进 —— 本项目的采集侧正是如此
 * （PTS 就是拍号，自适应降载改的是"每 d 拍入队一次"，拍号的量纲没变）。
 */
function updateLatency() {
  const sb = sourceBuffer
  const v = videoRef.value
  if (!sb || !v || sb.buffered.length === 0) {
    latency.value = 0
    trueLatency.value = null
    transportLag.value = null
    return
  }

  // 缓冲余量：和旧版逐字相同的那个数。它**仍然要显示** —— 三个数的差才是
  // 信息（见 `transportLag` 的说明）。
  const end = sb.buffered.end(sb.buffered.length - 1)
  const d = end - v.currentTime
  latency.value = d > 0 && Number.isFinite(d) ? d : 0

  if (!hasT0.value) {
    trueLatency.value = null
    transportLag.value = null
    return
  }

  const now = serverNowMs()
  const t0 = t0WallMs.value
  const total = now - (t0 + v.currentTime * 1000)
  const notYet = now - (t0 + end * 1000)

  // 钳到 0 而不是显示负数：负值只可能来自时钟抖动，或者"这段缓冲是上一个
  // 会话留下的"（会话重开时媒体时间轴归零，而 t0 还没来得及换新）。显示成
  // "−0.3s" 会被读成"画面来自未来"，而它真正的意思是"这个数这一刻不可信"。
  trueLatency.value = total > 0 && Number.isFinite(total) ? total / 1000 : 0
  transportLag.value = notYet > 0 && Number.isFinite(notYet) ? notYet / 1000 : 0
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

  // 时间原点**每次连接都要先清掉**，等后端重新报。后端在连接建立时会立刻发
  // 一帧，所以这个"未知"窗口只有一次往返。留着上一份不行的原因很具体：会话
  // 重开（换分辨率、重启进程）之后媒体时间轴归零，而旧的 t0 还指着上一段 ——
  // 算出来的"真实延迟"会是一个荒唐的数，而它看起来只是个偏大或偏小的正常值。
  t0WallMs.value = 0
  trueLatency.value = null
  transportLag.value = null

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

/**
 * 处理一帧**文本**控制消息。目前只有一种：`{"type":"t0",…}`。
 *
 * 走文本帧而不是塞进二进制流：二进制那一路是 fMP4 字节，MSE 只认它，中间混进
 * 任何别的东西都会让 SourceBuffer 报错。WS 本来就分文本/二进制两种帧类型，
 * 用它区分是框架自带的能力。
 *
 * **读不懂的消息一律忽略，不报错。** 这条通道以后加新类型时，老页面不该因为
 * 一条自己不认识的消息就把整块预览判成"出错"。t0 只是精度问题，不是可用性问题
 * —— 收不到就一直显示「—」。
 */
function onControlMessage(raw: string) {
  let msg: unknown
  try {
    msg = JSON.parse(raw)
  } catch {
    return
  }
  if (typeof msg !== 'object' || msg === null) return

  const m = msg as { type?: unknown; t0_wall_ms?: unknown; server_now_ms?: unknown }
  if (m.type !== 't0') return
  if (typeof m.t0_wall_ms !== 'number' || typeof m.server_now_ms !== 'number') return
  if (!(m.t0_wall_ms > 0) || !(m.server_now_ms > 0)) return

  // 偏差**用这条消息自己的 server_now_ms 量**：每条 t0 都带着"我发出去那一刻
  // 的服务端时刻"，所以这次量出来的差值里只含单程的网络时间。绝不能把它存下来
  // 之后再拿去做别的换算 —— 存的是"那一刻"的偏差，不是"永远"的。
  clockSkewMs = Date.now() - m.server_now_ms
  t0WallMs.value = m.t0_wall_ms
  updateLatency()
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
    // **文本帧必须在下面那条二进制守卫之前分辨。** 后端除了 fMP4 字节，还会在
    // 连接建立时单独发一帧**文本**控制消息（媒体时间 0 点）。而那句
    // `instanceof ArrayBuffer` 会把文本帧**静默丢掉** —— 不报错、不进 pending、
    // 什么痕迹都没有，症状是"真实延迟一直显示 —"，看起来像后端没发。
    if (typeof ev.data === 'string') {
      onControlMessage(ev.data)
      return
    }
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

    // **自己关的那条不算断线。** 通道切换的第一步就是 `teardownPipeline()`，
    // 它会触发这里 —— 不挡的话会同时冒出"预览通道已断开，正在自动重连…"和
    // 一条按指数退避排上的重连，而重连到的还是**刚被切走的那一路**（后端那个
    // 位此刻可能还没翻过来），于是刚切过去的画面被一条迟到的旧连接顶掉。
    if (switching) return

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
      // ⚠️ 这句话在 Phase W (b)（后端改成按节奏主动切分片）之后被改写了一次。
      // 改写前它写的是"分片只在关键帧处产生，所以 GOP 间隔越大这里等得越久" ——
      // 那句在改动之前是量出来的、是对的，改动之后就成了错的（陷阱 86 的形状：
      // 一条当时量出来的结论，在前提变了之后变成误导）。现在两件事必须分开说：
      //   - **分片**按调用方的节奏切（约每 0.25 秒一个，见 `fmp4_muxer.h` 的
      //     `flush()` 那段），所以"20 秒一个分片都没到"不再能用关键帧间隔解释。
      //   - **第一幅能显示的画面**仍然要等下一个关键帧，那一项由 `gop_seconds`
      //     决定，与分片节奏无关 —— 但它不表现为"没有分片"（分片照来，只是
      //     解不出画面），所以不该在这里提它。
      playerMessage.value =
        `已经连上并拿到初始化段，${FIRST_FRAGMENT_TIMEOUT_MS / 1000} 秒内一个分片都没有到。` +
        '分片是后端按固定节奏切出来的（约每 0.25 秒一个），所以这么久没有分片不是关键帧间隔的问题 —— ' +
        '去「实时状态」页看「分片产出」那个计数，或者确认后端还在跑。'
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
  trueLatency.value = null
  transportLag.value = null
  t0WallMs.value = 0
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

/**
 * 快照是**按需**的：进页面不抓，点一次抓一张。
 *
 * 原来那张常驻的 `<img v-show>` 加上一个「每秒自动刷新」的勾选，实际效果就是
 * "一进预览页就每秒钟向服务端要一张 2880x1920 的 JPEG"—— 而用户明确说了快照
 * 「默认不抓拍、点了才显示」。这一页的正文本来就是上面的 MSE 实时画面，底下那
 * 张静帧除了占带宽没有别的用处。
 *
 * `snapshotSrc` 是空串就代表"还没抓过"，这**一整条**就够实现了：没有 URL 就
 * 没有 `<img>`，也就没有请求。
 */
const snapshotState = ref<'idle' | 'loading' | 'ok' | 'fail'>('idle')
const snapshotSrc = ref('')

/** 抓过、而且没坏 —— 这时才把 `<img>` 放进 DOM。 */
const showSnapshotImage = computed(
  () => snapshotSrc.value !== '' && snapshotState.value !== 'fail',
)

/**
 * **必须用 `/api/snapshot.jpg`，不是 `/snapshot.jpg`。**
 *
 * 后者是 ONVIF 的 `GetSnapshotUri` 报给 NVR 的那条路：未登录时它回 401 +
 * `WWW-Authenticate: Digest`，而浏览器看见这个头会弹一个**系统级**的用户名/
 * 口令框 —— 和我们的登录页是两套东西，用户在里面输什么都不对。
 * `/api/` 那条只认会话 cookie，永远不发挑战。
 *
 * 用真的 `<img>` 而不是先 `new Image()` 预载、载完再赋值：后端对快照发的是
 * `Cache-Control: no-store`，预载会让每一次抓拍发出**两个**请求。
 */
function captureSnapshot() {
  // 加时间戳是为了绕开浏览器缓存：同一个 URL 被缓存过之后，再点一次会毫无反应。
  // （后端也发了 no-store，但浏览器对它的尊重程度不一致，双重保险。）
  snapshotSrc.value = `/api/snapshot.jpg?t=${Date.now()}`
  snapshotState.value = 'loading'
}

/* ------------------------------------------------------------- 通道选择器 */

/**
 * 现在看的是第几路。**这个状态的权威在后端**（全机一份），这里只是一份镜像。
 *
 * 为什么不在前端放一个 `ref(1)` 就完事：换一个标签页、刷新一次页面之后前端会
 * 从 1 重新开始，而后端还停在用户上回切过去的那一路 —— 界面上高亮的是通道 1、
 * 画面却是通道 2 的，按快门存下来的也是通道 2 的。所以进页面时**读一次**后端
 * 的状态（见 `onMounted`）。
 */
const activeChannel = ref(1)
/** 切换过程中为真。用来让 `ws.onclose` 知道"这次断开是我自己关的"。 */
let switching = false

/**
 * 可选的那几路。**空数组就什么都不画** —— 后端第一帧统计还没到时是空的，而
 * 只有一路的机器上它永远只有一项：这两种情况都不该出现一排只有一个按钮的选择器。
 *
 * 数据源是 `/ws/stats` 的 `channels`（后端每秒重拼一次），不是新开一个接口：
 * 那份数组本来就带着 `index` 和 `name`，而它的刷新节奏正好是"通道增删之后一秒内"。
 */
const channelChoices = computed(() => stats.value.channels ?? [])

async function switchChannel(n: number) {
  if (n === activeChannel.value || switching) return
  const wasPlaying = wantStream.value

  // **顺序是刻意的：先关旧的 → 再 POST → 最后（下面）开新的。**
  //
  // 1) 先关：POST 那一下后端会把新那一路的初始化段**重放**一遍，而
  //    `publish_preview` 的语义是"存一份 + 广播给所有连接"。旧连接要是还在，
  //    就会在一条正在跑的 MSE 流中间收到一段别的 moov —— Chrome 会把整个
  //    SourceBuffer 废掉，症状是切过去之后画面永远黑着。
  //
  //    ⚠️ 下面那一行 `teardownPipeline()` **不是**今天兑现这个顺序的那一行。
  //    把它删掉、重编、重跑浏览器探针，结果**一模一样全绿**（2026-10-09 实测，
  //    陷阱 85 的变异检验）：`wasPlaying` 为真时收尾那个 `start()` 自己第一件
  //    事就是 `teardownPipeline()`，为假时 `stop()` 早把连接关了 —— 每条可达
  //    路径上旧连接都已经不在。留着它是因为**被依赖的那个不变量应当写在依赖
  //    它的地方**："POST 之前旧连接已经不在了"这句话说给 `switchChannel` 听，
  //    兑现它的却可以是文件另一头某个函数的内部步骤；那样下一次有人给 `start()`
  //    换实现时，代价是一次**没有任何报错**的黑屏，而探针也照样全绿。
  // 2) 再 POST：那之后后端"现在看哪一路"才翻过去，`/api/snapshot.jpg` 跟着换。
  // 3) 最后开：新连接建立时后端会把**当前这一路**的初始化段立刻发下来，所以
  //    画面从第一块就是新路的。
  switching = true
  teardownPipeline()
  const r = await setPreviewChannel(n)
  if (r.ok) {
    activeChannel.value = r.data?.channel ?? n
    playerMessage.value = ''
  } else {
    // 失败时**留在原来那一路**并把话说清楚。拆掉的那条流下面会重新接上，
    // 所以代价只是黑一下 —— 比"切到一个不存在的通道然后什么都不显示"好。
    playerMessage.value = r.error || `切不到通道 ${n}。`
  }
  switching = false
  if (wasPlaying) start()
}

/* --------------------------------------------------------------- 生命周期 */

onMounted(() => {
  document.addEventListener('fullscreenchange', onFullscreenChange)
  // 进页面就开始播：这一页存在的唯一理由就是看画面，让用户先点一下
  // 「开始预览」是纯多余的一步。
  start()
  // 档位那几个数是"这一页怎么播"的参数，不是画面本身，所以**不进 start() 的
  // 关键路径** —— 读不到就按默认档播（旧版本的行为），不能让一次配置请求的
  // 失败把画面挡在门外。
  void loadLatencyTier()
  // 后端可能停在上一个用户切过去的那一路（这个状态是全机一份、活得比这个
  // 标签页久）。读回来只是让**按钮上那个高亮**和画面一致；画面本身不用等它
  // —— 上面 `start()` 连上去拿到的本来就是当前那一路。
  void (async () => {
    const r = await fetchPreviewChannel()
    if (r.ok && r.data?.channel > 0) activeChannel.value = r.data.channel
  })()
})

onBeforeUnmount(() => {
  stop()
  document.removeEventListener('fullscreenchange', onFullscreenChange)
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

/* ------------------------------------------------- (c) / (d) 的两句说明 */

/** 延时档位的中文名。线值永远是英文，只有显示层翻译（陷阱 80）。 */
const TIER_LABELS: Record<string, string> = {
  realtime: '极致低延迟',
  balanced: '均衡',
  quality: '优先画质',
  custom: '自定义',
}

/** 下拉框的三个可选项（跟档位 + 三个本机覆盖）。 */
const latencyChoices = [
  { key: '', label: '跟随档位' },
  { key: 'realtime', label: '极致低延迟' },
  { key: 'balanced', label: '均衡' },
  { key: 'quality', label: '优先画质' },
]

/**
 * (c)：播放这一侧当前用的两个数，以及**它们是从哪来的**。
 *
 * 来源必须说出来：档位是"设备给的默认值"、本机覆盖是"这台机器的选择"，两者
 * 混在一个数里的话，改了后端的档位而这个页面纹丝不动（因为本机覆盖在赢），
 * 就没有任何东西能解释为什么。
 */
const latencyNote = computed(() => {
  let src: string
  if (latencySource.value === 'local') {
    src = '本机覆盖'
  } else if (latencySource.value === 'tier') {
    src = `延时档位「${TIER_LABELS[configTier.value] ?? configTier.value}」`
  } else if (configTier.value) {
    src = `默认值（档位「${TIER_LABELS[configTier.value] ?? configTier.value}」没有对应的播放参数）`
  } else {
    src = '默认值（还没读到后端配置）'
  }
  return `播放目标 ${targetLatency.value.toFixed(2)}s / 落伍阈值 ${maxDrift.value.toFixed(1)}s —— 来自${src}`
})

/**
 * (d)：中途连上预览时的**首帧等待**。
 *
 * **和「分片滞后」分开写**（计划里 (d) 那一条）：一个是"多久能看到第一幅画"
 * （由关键帧间隔决定），一个是"看到之后画面有多滞后"（由分片节奏决定）。
 * 合成一句的话，用户会把"等了 2 秒才出画"归到分片节奏上，去调一个不相干的旋钮。
 */
const firstFrameNote = computed(() => {
  if (!configGopSeconds.value) return ''
  return (
    `另外：中途连上预览时，分片约每 0.25 秒到一个，但第一幅能显示的画面要等下一个关键帧 —— ` +
    `当前关键帧间隔 ${configGopSeconds.value} 秒，最坏就等这么久。` +
    `这一项由关键帧间隔（延时档位的第二个旋钮）决定，和上面的「分片滞后」不是同一件事。`
  )
})
</script>

<template>
  <div class="page-head">
    <h1>预览</h1>
    <span class="hint">
      画面走 WebSocket 上的 fMP4，和 NVR 拉的 RTSP 是两条独立的链路。画面上的
      「真实延迟」是按墙钟量的端到端值，等于「分片滞后」+「缓冲」两项之和。
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

    <!--
      通道选择器。**只有一路时整排都不画** —— 一个只有一个按钮的按钮排是噪音，
      而"这台机器上有几路"正是用户要一眼看出来的东西。

      为什么是"一次只看一路"而不是并排多画面：CPU 和「真实延迟」这个读数都和
      今天**同口径**。并排会同时跑 N 路解码 + N 条 WS，而那个延迟数字就没法
      归因了 —— 它答的必须是"这一路怎么样"。
    -->
    <div v-if="channelChoices.length > 1" class="channel-pick">
      <span class="hint">通道</span>
      <button
        v-for="c in channelChoices"
        :key="c.index"
        :class="{ primary: c.index === activeChannel }"
        :title="`${c.name}（${c.source === 'webcam' ? '摄像头' : '显示器'}）`"
        @click="switchChannel(c.index)"
      >
        {{ c.name }}
      </button>
      <span class="hint">切换会重连一次预览流，黑一下是正常的。</span>
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
        <span class="chip" title="画面落后现实多少秒（按墙钟算，端到端）">
          <Icon name="clock" :size="12" />真实延迟 {{ trueLatencyText }}
        </span>
        <span
          class="chip"
          title="其中还没送到浏览器的那一段：服务端的分片节奏。改延时档位动的是它"
        >
          <Icon name="signal" :size="12" />分片滞后 {{ transportText }}
        </span>
        <span class="chip" title="播放器缓冲里压着多少（播放头离已收到的末尾多远）">
          <Icon name="pause" :size="12" />缓冲 {{ latency.toFixed(1) }}s
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

    <!--
      (c)：播放这一侧的两个数从哪来。**必须看得见** —— 它决定了「真实延迟」
      那个数的地板在哪里，而它跟着延时档位走。
    -->
    <div class="latency-note">
      <div class="row" style="gap: 8px; align-items: center; flex-wrap: wrap">
        <span class="hint">{{ latencyNote }}</span>
        <select v-model="localPick" style="width: auto; min-width: 130px">
          <option v-for="c in latencyChoices" :key="c.key" :value="c.key">{{ c.label }}</option>
        </select>
      </div>
      <p class="hint" style="margin: 6px 0 0">
        右边那个下拉框是「本机覆盖」：档位是设备给的默认值，而「播放头该贴多近」
        取决于看着这块屏幕的这台机器和这条网络。选择存在本机，换浏览器就回到跟随档位。
      </p>
      <p v-if="firstFrameNote" class="hint" style="margin: 6px 0 0">{{ firstFrameNote }}</p>
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
      <template v-if="!hasT0">
        <br />「真实延迟」显示「—」是因为还没收到后端报的<strong>时间原点</strong>
        （它就是媒体时间 0 点对应的墙钟）。没有它就没法把播放位置换算成墙钟，
        算不出端到端延迟 —— 这时宁可说「未知」，也不拿缓冲余量顶上冒充。
      </template>
    </p>
  </div>

  <div class="card">
    <div class="row" style="justify-content: space-between; margin-bottom: 12px">
      <h2 style="margin: 0">快照</h2>
      <span class="hint">默认不抓拍 —— 点一下才向服务端要一帧</span>
    </div>

    <div class="stage scroll-x">
      <!--
        抓过之后这个 `<img>` 一直待在 DOM 里：重新抓拍时浏览器会先留着旧画面、
        新图 decoded 之后才换，于是不会闪一下白的。真的坏了（加载失败）才收起来，
        让下面的占位块出来说话。
      -->
      <img
        v-if="showSnapshotImage"
        :src="snapshotSrc"
        alt="桌面抓拍"
        class="snapshot"
        @load="snapshotState = 'ok'"
        @error="snapshotState = 'fail'"
      />
      <div v-else class="placeholder stage-placeholder">
        <Icon :name="snapshotState === 'fail' ? 'alert' : 'camera'" :size="26" :stroke="1.5" />
        <div class="ph-title">
          {{ snapshotState === 'fail' ? '快照不可用' : '还没有抓拍' }}
        </div>
        <div class="ph-text">
          <template v-if="snapshotState === 'fail'">
            后端还没有采到画面（还没开始编码时会返回 503），或者会话已过期。
          </template>
          <template v-else>
            上面那块是实时预览。快照是后端另外抓一帧编成 JPEG，和预览通道互不影响
            —— 预览出问题时它通常还是好的。
          </template>
        </div>
      </div>
    </div>

    <div class="actions" style="margin-top: 12px">
      <button :disabled="snapshotState === 'loading'" @click="captureSnapshot">
        <Icon name="refresh" :size="15" />
        {{ snapshotState === 'ok' ? '重新抓拍' : '抓拍' }}
      </button>
    </div>
  </div>
</template>
