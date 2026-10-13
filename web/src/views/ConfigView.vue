<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import {
  factoryReset,
  fetchAudioDevices,
  fetchConfig,
  fetchSources,
  fetchTime,
  reloadPipeline,
  resetTime,
  restartOnvif,
  restartService,
  saveConfig,
  setTime,
  syncTimeNow,
} from '../api'
import type { ApiResult } from '../api'
import { SECRET_SENTINEL } from '../types'
import type {
  AppConfig,
  AudioDevice,
  ChannelConfig,
  ChannelsConfig,
  LatencyTier,
  LifecycleResult,
  OsdItemConfig,
  QualityTier,
  Sources,
  TimeInfo,
} from '../types'
import { useAuth } from '../composables/useAuth'
import { useStats } from '../composables/useStats'
import Icon from '../components/Icon.vue'
import { scrollToHash } from '../scroll'
import { setLifecycleNote } from '../lifecycle'

const route = useRoute()
const router = useRouter()
const { stats } = useStats()
const { ensureSession } = useAuth()

/**
 * 表单自己那一份配置。
 *
 * 与后端给的那份（`AppConfig`）只差一处：这里 `channels` **是必有的**。
 * 后端 `config_to_json` 在通道一条都没有时**故意不发这个键**（那是个兼容
 * 边界，见 types.ts），于是"从来没配过多通道"和"后端版本旧、压根没这功能"
 * 在线上是同一个形状。表单里需要一个恒存在的容器才写得下去 ——
 * 而"发出去的 JSON 里要不要这个键"是另一件事，由 `buildPayload()` 管。
 */
type FormConfig = Omit<AppConfig, 'channels'> & { channels: ChannelsConfig }

/** 与后端 config.h 里的默认值保持一致，只为在读取失败时有个形状参考。 */
function defaults(): FormConfig {
  return {
    capture: {
      display_index: 0,
      // 与后端 config.h 一致：均衡档（25 fps / 2048 kbps / 桌面原生）。
      fps: 25,
      bitrate_kbps: 2048,
      gop_seconds: 2,
      threads: 0,
      preset: 'ultrafast',
      tune: 'zerolatency',
      profile: 'main',
      quality: 'balanced',
      latency: 'balanced',
      max_height: 0,
    },
    rtsp: {
      enabled: false,
      port: 554,
      path: '/live',
      sub_enabled: true,
      sub_path: '/live_sub',
      sub_height: 360,
      sub_bitrate_kbps: 512,
    },
    audio: {
      // 与后端 AudioConfig 一致：默认**关**。
      enabled: false,
      system: false,
      microphone: false,
      device: '',
      bitrate_kbps: 64,
      mic_gain: 1,
      system_gain: 1,
    },
    web: {
      enabled: false,
      host: '0.0.0.0',
      port: 8080,
      static_root: 'web/dist',
      auth_enabled: true,
      user: 'admin',
      // 表单里不回填口令，这里只是形状参考。
      password: '',
    },
    onvif: {
      enabled: false,
      user: 'admin',
      password: '',
      manufacturer: 'Desk2ONVIF',
      model: 'Desk2ONVIF Virtual Camera',
      firmware_version: '1.0.0',
      hardware_id: '1.0',
      serial_number: '',
      device_uuid: '',
      device_name: 'Desk2ONVIF',
      location: 'Desktop',
    },
    osd: {
      enabled: true,
      adaptive_color: true,
      outline: true,
      plate_alpha: 0,
      // 与后端 OsdConfig::items 的默认一致：时间在左上、名称在右下
      // （用户明确指定，与海康自己的默认正好相反）。
      items: [
        {
          enabled: true,
          type: 'time',
          text: '',
          position: 'upper_left',
          x_pct: 0,
          y_pct: 0,
          font_size: 0,
          date_format: '%Y-%m-%d',
          time_format: '%H:%M:%S',
        },
        {
          enabled: true,
          type: 'name',
          text: '',
          position: 'lower_right',
          x_pct: 0,
          y_pct: 0,
          font_size: 0,
          date_format: '%Y-%m-%d',
          time_format: '%H:%M:%S',
        },
      ],
    },
    adaptive: {
      mode: 'throttle',
      min_fps: 5,
    },
    time: {
      // 与后端 TimeConfig 一致：默认手动、间隔一小时、偏移 0。
      mode: 'manual',
      ntp_server: 'ntp.aliyun.com',
      ntp_interval_s: 3600,
      offset_s: 0,
    },
    // **恒存在的一份空列表**，哪怕后端的 `AppConfig.channels` 是可选的。
    // 表单需要一个可以 `.push()` 的数组，而"键在不在"这件事的语义只在
    // **发出去的那一份 JSON** 里才要紧 —— 那由 `buildPayload()` 单独处理。
    channels: { items: [] },
  }
}

const form = reactive<FormConfig>(defaults())
const loading = ref(true)
/** 配置没读到就不给编辑：拿默认值去覆盖一份真实配置，是最糟的失败方式。 */
const loaded = ref(false)
const loadError = ref('')
const saving = ref(false)
const saveError = ref('')
const saveOk = ref('')
/**
 * 已保存但**还没生效**的字段。
 *
 * 两个来源，缺一不可：`POST` 的回应给出"这次保存的结果"，`GET` 给出"累积到
 * 现在还欠着什么"。只有前者的话，用户存完刷新一下页面提示就没了，而表单里
 * 显示的又都是新值 —— 看起来一切已生效。
 */
const pendingFields = ref<string[]>([])
const restarting = ref(false)
const restartMsg = ref('')

/**
 * 画质档位表。**从后端来**（`GET /api/config` 的 `quality_tiers`），不在前端写死
 * —— 表里的帧率/码率/分辨率上限要显示给用户看（"降到这一档会牺牲什么"），
 * 抄一份的话改了表界面就开始说假话。
 *
 * 老后端不发这个字段，所以可能是空数组。空的时候下面的推导会**原样保留**配置里
 * 读到的档位，而不是一律当自定义：没有表就没有判据，那就不该动它。
 */
const tiers = ref<QualityTier[]>([])

/**
 * 当前**实际**处在哪一档 —— 由具体字段推出来，不信任 `form.capture.quality` 这个
 * 存下来的字面量。
 *
 * 后端的规矩是"非 custom 的档位会把它辖下的四项覆盖成该档的固定值"
 * （`validate_config` 里的展开），所以"选了流畅、界面上却写着帧率 30"是一个
 * 后端自己都要打 note 的矛盾。手改过任一项就该显示「自定义」——
 * **推导而不是在四个输入框上各挂一个 oninput 去翻标志**：那种写法漏掉一个入口
 * （比如从别处改、或者将来加了第五项）就静默失效，而失效的表现是"改了帧率、
 * 保存、帧率又变回去了"。
 */
function effectiveQuality(): string {
  const cur = form.capture.quality
  if (tiers.value.length === 0) return cur // 没有表 → 不做判断，原样
  const t = tiers.value.find((x) => x.key === cur)
  if (!t) return 'custom'
  const same =
    form.capture.fps === t.fps &&
    form.capture.bitrate_kbps === t.bitrate_kbps &&
    form.capture.max_height === t.max_height &&
    form.rtsp.sub_enabled === t.sub_enabled
  return same ? cur : 'custom'
}

/** 选中档位的那四项值，用来说明"这一档到底是什么"。 */
const currentTierDetail = computed(() => {
  const t = tiers.value.find((x) => x.key === effectiveQuality())
  if (!t) return ''
  const h = t.max_height > 0 ? `${t.max_height}p 以内` : '桌面原生'
  return `${t.fps} fps / ${t.bitrate_kbps} kbps / ${h} / 子码流${t.sub_enabled ? '开' : '关'}`
})

/**
 * 选档位。**必须当场把这四项也改掉**：只改标签的话，界面上显示"流畅"而帧率
 * 还是 30，而上面那个推导会立刻把它显示成「自定义」—— 用户会觉得按钮坏了。
 * 选「自定义」时则一个字段都不动（那才是自定义的语义）。
 */
function onTierChange(e: Event) {
  const key = (e.target as HTMLSelectElement).value
  form.capture.quality = key
  const t = tiers.value.find((x) => x.key === key)
  if (!t) return
  form.capture.fps = t.fps
  form.capture.bitrate_kbps = t.bitrate_kbps
  form.capture.max_height = t.max_height
  form.rtsp.sub_enabled = t.sub_enabled
}

/**
 * 延时档位表。与 `tiers` 一样**从后端来**（`GET /api/config` 的
 * `latency_tiers`），不在前端写死 —— 每档的线程数与关键帧秒数是要显示给用户
 * 看的（"这一档换来什么、代价是什么"），抄一份的话改了表界面就开始说假话。
 *
 * ⚠️ 表里**没有 `custom`**：后端 `find_latency_tier("custom")` 返回 nullptr，
 * 它是"一个字段都不展开"的落点，不是一个可选项。界面自己补那一项。
 */
const latencyTiers = ref<LatencyTier[]>([])

/**
 * 当前**实际**处在哪一档 —— 与 `effectiveQuality()` 同一条纪律：由具体字段
 * 推出来，不信任存下来的 `form.capture.latency` 字面量。
 *
 * 理由是同一个：后端对非 custom 的档位会把它辖下的两个字段覆盖成该档的固定
 * 值，于是"选了极致低延迟、界面上却写着线程 6"是一个后端自己都要打 note 的
 * 矛盾。手改过任一项就应当显示「自定义」。
 */
function effectiveLatency(): string {
  const cur = form.capture.latency
  if (latencyTiers.value.length === 0) return cur // 没有表 → 不做判断，原样
  const t = latencyTiers.value.find((x) => x.key === cur)
  if (!t) return 'custom'
  const same =
    form.capture.threads === t.threads && form.capture.gop_seconds === t.gop_seconds
  return same ? cur : 'custom'
}

/**
 * 选档位。**必须当场把这两个字段也改掉**，理由与 `onTierChange` 逐条相同：
 * 只改标签的话界面上显示「极致低延迟」而线程数还是 6，上面那个推导会立刻把它
 * 显示成「自定义」，用户会觉得按钮坏了。
 */
function onLatencyChange(key: string) {
  form.capture.latency = key
  const t = latencyTiers.value.find((x) => x.key === key)
  if (!t) return // 「自定义」：一个字段都不动，那才是自定义的语义
  form.capture.threads = t.threads
  form.capture.gop_seconds = t.gop_seconds
}

/**
 * 一档的「换来什么 / 代价是什么」。数字部分由**表里的值现算**（线程数 ÷ 帧率
 * 就是流水线延迟的秒数），只有那两句人话是按 key 写死的 —— 它们不是从数字里
 * 推导得出来的。
 *
 * 认不出的 key 就**只报数字、不说人话**，而不是硬套一个默认说法：后端加了新档
 * 而这里没跟上时，宁可少说一句，也不能说错一句。
 */
function latencyTierNote(t: LatencyTier) {
  const fps = form.capture.fps > 0 ? form.capture.fps : 25
  const pipeMs = Math.round((t.threads * 1000) / fps)
  const numbers = `编码流水线 ${t.threads} 帧（≈ ${pipeMs} ms @ ${fps} fps）· 关键帧每 ${t.gop_seconds} 秒一个`
  const prose: Record<string, { gain: string; cost: string }> = {
    realtime: {
      gain: '预览出画最快、分片最密 —— 中途连上预览几乎立刻有画面。',
      cost: '编码近乎单线程，高分辨率下容易跟不上（自适应会去降帧率，那是设计不是故障）；关键帧密，同码率下画质略降。',
    },
    balanced: {
      gain: '编码并行度与关键帧间隔都在中间，是默认档。',
      cost: '比「极致低延迟」多等一点，比「优先画质」画质略低。',
    },
    quality: {
      gain: '编码并行度最高、关键帧最稀，同码率下画质最好。',
      cost: `延迟最大：中途连上预览最坏要等 ${t.gop_seconds} 秒才有第一个分片。`,
    },
  }
  const p = prose[t.key]
  return { numbers, gain: p?.gain ?? '', cost: p?.cost ?? '' }
}

/**
 * 卡片列表 = 后端的三档 + 界面自己补的那一项「自定义」。
 *
 * 算成 computed 而不是在模板里逐张调 `latencyTierNote`：模板里一张卡要三行，
 * 三张就是九次调用，每次都现算一遍流水线毫秒数。
 */
const latencyCards = computed(() =>
  latencyTiers.value.map((t) => ({ key: t.key, label: t.label, note: latencyTierNote(t) })),
)

/**
 * 「自定义」那一张卡上写的字。它和后端那三档不一样：**它的值是手填的**，
 * 所以这里报的是**当前实际那两个数**，而不是表里的某个常数。
 *
 * 另外要回答"当前值恰好落在哪一档" —— 用户从旧版本升上来时
 * `capture.threads` 是 0（自动），落不进任何一档；而手把它改成 3 之后
 * 又恰好等于「均衡」。这两种情况在界面上必须分得开，否则用户会以为
 * 「自定义」是个坏掉了的档位。
 */
const customLatencyNote = computed(() => {
  const th = form.capture.threads
  const gop = form.capture.gop_seconds
  const fps = form.capture.fps > 0 ? form.capture.fps : 25
  const thText = th > 0 ? `${th} 帧` : '0（按「延迟优先」自动选，封顶 6）'
  const pipeMs = th > 0 ? `（≈ ${Math.round((th * 1000) / fps)} ms @ ${fps} fps）` : ''
  const hit = latencyTiers.value.find((t) => t.threads === th && t.gop_seconds === gop)
  return {
    numbers: `编码流水线 ${thText}${pipeMs} · 关键帧每 ${gop} 秒一个`,
    landed: hit
      ? `这两个值恰好等于「${hit.label}」那一档。`
      : '这两个值不等于上面任何一档 —— 这就是「自定义」的常态。',
  }
})

/**
 * 口令框永远是空的，而且独立于 form —— 后端 GET 回来的是哨兵 "__unchanged__"，
 * 真口令永远不会到达浏览器，所以这里没有任何东西可以回填。
 * 用户不动这个框，提交时就发哨兵回去（含义：保持原值）。
 *
 * **两套凭据各有一份**（Web 登录 / ONVIF），它们在后端也是完全独立的两个字段。
 */
const onvifPasswordInput = ref('')
const hasStoredOnvifPassword = ref(false)
const webPasswordInput = ref('')
/**
 * Web 口令"存过没有"。
 *
 * 这里能（也只有这里能）分辨两件事，因为 GET 对**空的**口令不做脱敏：
 * 哨兵 = 存过，空串 = 就是空的。而"空的"在上面的服务里不是"不鉴权"，
 * 是"口令就是空" —— 所以这个区别必须显示出来（见下面的安全提示）。
 */
const hasStoredWebPassword = ref(false)
const webPasswordEmpty = ref(false)

function num(v: unknown, fallback: number): number {
  return typeof v === 'number' && Number.isFinite(v) ? v : fallback
}

function bool(v: unknown, fallback: boolean): boolean {
  return typeof v === 'boolean' ? v : fallback
}

function str(v: unknown, fallback: string): string {
  return typeof v === 'string' ? v : fallback
}

/**
 * 路径归一化成后端 `lead()` 那样：空串保持空（= 让后端补 `/chN`），否则补前导斜杠。
 *
 * **必须在模块这一层**，不能只放在 `errors` 里面：去重判据和"加一路时挑一个
 * 空路径"用的是同一条规则，两处各写一遍迟早会分叉 —— 而分叉的症状是界面
 * 放行一份后端会丢项的配置，或者新加的路径正好撞上别人。
 */
function normalizePath(p: string): string {
  const v = p.trim()
  if (!v) return ''
  return v.startsWith('/') ? v : '/' + v
}

/**
 * 水印项照后端那份**整份重建**，逐字段兜默认值。
 *
 * 每一项都从一份"默认项"起手、只覆盖确实存在的键 —— 与后端
 * `config_from_json` 里 `OsdItemConfig one;` 那句是同一个写法。缺字段时**不能**
 * 直接留 `undefined`：`x_pct` 之类的会一路传回后端变成 JSON null，而
 * `config_from_json` 的 `take` 遇到 null 会保持默认值 —— 也就是"界面上看着有值、
 * 存进去却是另一个数"，两处都不报错。
 */
function mergeOsdItems(raw: unknown): OsdItemConfig[] {
  const d = defaults().osd.items
  const list = Array.isArray(raw) ? raw : d
  return list.map((it, i) => {
    const o = (it ?? {}) as Partial<OsdItemConfig>
    const fb = d[i] ?? d[d.length - 1]
    return {
      enabled: bool(o.enabled, fb.enabled),
      type: str(o.type, fb.type),
      text: str(o.text, fb.text),
      position: str(o.position, fb.position),
      x_pct: num(o.x_pct, fb.x_pct),
      y_pct: num(o.y_pct, fb.y_pct),
      font_size: num(o.font_size, fb.font_size),
      date_format: str(o.date_format, fb.date_format),
      time_format: str(o.time_format, fb.time_format),
    }
  })
}

/** 后端可能少发字段（版本不一致时），逐个兜默认值，别让表单里出现 undefined。 */
function mergeConfig(raw: Partial<AppConfig> | undefined): FormConfig {
  const d = defaults()
  const c = raw ?? {}
  const cap = c.capture ?? ({} as AppConfig['capture'])
  const rtsp = c.rtsp ?? ({} as AppConfig['rtsp'])
  const audio = c.audio ?? ({} as AppConfig['audio'])
  const web = c.web ?? ({} as AppConfig['web'])
  const onvif = c.onvif ?? ({} as AppConfig['onvif'])
  const osd = c.osd ?? ({} as AppConfig['osd'])
  const adaptive = c.adaptive ?? ({} as AppConfig['adaptive'])
  const time = c.time ?? ({} as AppConfig['time'])
  return {
    capture: {
      display_index: num(cap.display_index, d.capture.display_index),
      fps: num(cap.fps, d.capture.fps),
      bitrate_kbps: num(cap.bitrate_kbps, d.capture.bitrate_kbps),
      gop_seconds: num(cap.gop_seconds, d.capture.gop_seconds),
      threads: num(cap.threads, d.capture.threads),
      preset: str(cap.preset, d.capture.preset),
      tune: str(cap.tune, d.capture.tune),
      profile: str(cap.profile, d.capture.profile),
      // **这两项必须原样带过来。** 丢了的话 `Object.assign` 之后的表单里就只剩
      // `defaults()` 的 `balanced`，于是"载入一份 smooth 的配置、改个设备名、
      // 保存"会把档位悄悄写回均衡 —— 而后端会照着档位把帧率码率分辨率一起
      // 重写。界面上一路绿灯，配置却整段换了。
      quality: str(cap.quality, d.capture.quality),
      // 与 `quality` **同一条**理由，逐字相同：丢了的话载入一份 realtime 的
      // 配置、改个设备名、保存，会把档位悄悄写回均衡 —— 而后端会照着档位把
      // 线程数与关键帧间隔一起重写。界面上一路绿灯，编码参数却整段换了。
      latency: str(cap.latency, d.capture.latency),
      max_height: num(cap.max_height, d.capture.max_height),
    },
    rtsp: {
      enabled: bool(rtsp.enabled, d.rtsp.enabled),
      port: num(rtsp.port, d.rtsp.port),
      path: str(rtsp.path, d.rtsp.path),
      sub_enabled: bool(rtsp.sub_enabled, d.rtsp.sub_enabled),
      sub_path: str(rtsp.sub_path, d.rtsp.sub_path),
      sub_height: num(rtsp.sub_height, d.rtsp.sub_height),
      sub_bitrate_kbps: num(rtsp.sub_bitrate_kbps, d.rtsp.sub_bitrate_kbps),
    },
    audio: {
      enabled: bool(audio.enabled, d.audio.enabled),
      system: bool(audio.system, d.audio.system),
      microphone: bool(audio.microphone, d.audio.microphone),
      device: str(audio.device, d.audio.device),
      bitrate_kbps: num(audio.bitrate_kbps, d.audio.bitrate_kbps),
      mic_gain: num(audio.mic_gain, d.audio.mic_gain),
      system_gain: num(audio.system_gain, d.audio.system_gain),
    },
    web: {
      enabled: bool(web.enabled, d.web.enabled),
      host: str(web.host, d.web.host),
      port: num(web.port, d.web.port),
      static_root: str(web.static_root, d.web.static_root),
      auth_enabled: bool(web.auth_enabled, d.web.auth_enabled),
      user: str(web.user, d.web.user),
      // 哨兵本身不进表单，它只是一个信号：后端存过口令。
      password: '',
    },
    onvif: {
      enabled: bool(onvif.enabled, d.onvif.enabled),
      user: str(onvif.user, d.onvif.user),
      password: '',
      manufacturer: str(onvif.manufacturer, d.onvif.manufacturer),
      model: str(onvif.model, d.onvif.model),
      firmware_version: str(onvif.firmware_version, d.onvif.firmware_version),
      hardware_id: str(onvif.hardware_id, d.onvif.hardware_id),
      serial_number: str(onvif.serial_number, d.onvif.serial_number),
      device_uuid: str(onvif.device_uuid, d.onvif.device_uuid),
      device_name: str(onvif.device_name, d.onvif.device_name),
      location: str(onvif.location, d.onvif.location),
    },
    osd: {
      enabled: bool(osd.enabled, d.osd.enabled),
      adaptive_color: bool(osd.adaptive_color, d.osd.adaptive_color),
      outline: bool(osd.outline, d.osd.outline),
      plate_alpha: num(osd.plate_alpha, d.osd.plate_alpha),
      // **整份替换，不合并**：后端 `config_from_json` 对 items 就是这么做的
      // （数组就是列表本身）。合并的话"删掉一项"在下一份配置里又会冒出来。
      items: mergeOsdItems(osd.items),
    },
    adaptive: {
      mode: str(adaptive.mode, d.adaptive.mode),
      min_fps: num(adaptive.min_fps, d.adaptive.min_fps),
    },
    time: {
      // 空串会落到界面上那个二选一的分支外面，表现为"模式那两行整个不显示"，
      // 而模式是这一段其余内容的开关 —— 所以空串也兜回默认的 `manual`。
      mode: str(time.mode, d.time.mode) || d.time.mode,
      ntp_server: str(time.ntp_server, d.time.ntp_server),
      ntp_interval_s: num(time.ntp_interval_s, d.time.ntp_interval_s),
      offset_s: num(time.offset_s, d.time.offset_s),
    },
    // `c.channels` 缺省有两种来源：从来没用过多通道（后端**故意**不发这个键，
    // 见 types.ts 里 `ChannelsConfig` 的说明），或者是个老后端。两种都当空列表。
    channels: { items: mergeChannelItems(c.channels?.items) },
  }
}

/**
 * `channels.items[]` 逐项兜默认值。写法与 `mergeOsdItems` 同源，理由也同一句：
 * 缺字段时留 `undefined` 会一路变成 JSON null，而后端 `config_from_json` 对
 * null 是"保持默认值"—— 于是界面上看着有值、存进去是另一个数，两处都不报错。
 *
 * **默认值不能从 `defaults().channels.items[0]` 抄**（OSD 那边能那么写是因为它
 * 的默认项本身就有意义）：这里空的，所以每一项的兜底值都按**后端
 * `ChannelConfig` 的字段默认值**写一遍。两处对不上的症状是"界面上加了一路，
 * 存进去的帧率不是显示的那个"。
 */
function mergeChannelItems(raw: unknown): ChannelConfig[] {
  if (!Array.isArray(raw)) return []
  return raw.map((it) => {
    const o = (it ?? {}) as Partial<ChannelConfig>
    return {
      enabled: bool(o.enabled, true),
      name: str(o.name, ''),
      // 认不出来的来源按 display 算：后端 `validate_config` 也是这么改的
      // （它会把不认识的 source 改成 display 并写一条 note），这里跟着它，
      // 否则下拉框会落到一个没有对应 option 的值上，显示成空白。
      source: o.source === 'webcam' ? 'webcam' : 'display',
      display_index: num(o.display_index, 0),
      camera: str(o.camera, ''),
      // 与后端 ChannelConfig::quality 的默认一致：**流畅**，不是均衡。
      // 通道 2 起多半是给 NVR 的多画面网格用的，每路都是一个独立编码器。
      quality: str(o.quality, 'smooth'),
      fps: num(o.fps, 20),
      bitrate_kbps: num(o.bitrate_kbps, 1024),
      max_height: num(o.max_height, 1080),
      sub_enabled: bool(o.sub_enabled, true),
      sub_height: num(o.sub_height, 360),
      sub_bitrate_kbps: num(o.sub_bitrate_kbps, 512),
      rtsp_path: str(o.rtsp_path, ''),
      rtsp_sub_path: str(o.rtsp_sub_path, ''),
    }
  })
}

async function load() {
  loading.value = true
  loadError.value = ''
  const res = await fetchConfig()
  if (!res.ok) {
    loadError.value = res.error
    loaded.value = false
    loading.value = false
    return
  }
  const merged = mergeConfig(res.data)
  Object.assign(form.capture, merged.capture)
  Object.assign(form.rtsp, merged.rtsp)
  Object.assign(form.audio, merged.audio)
  Object.assign(form.web, merged.web)
  Object.assign(form.onvif, merged.onvif)
  Object.assign(form.osd, merged.osd)
  // `items` 是数组，而 `Object.assign` 是**覆盖**（不是追加），所以上面那一句
  // 直接换掉了整个数组引用 —— 这正是要的。列成一行是为了和上面几句排在一起，
  // 免得下一个人以为 osd 少了什么特殊处理。
  Object.assign(form.adaptive, merged.adaptive)
  Object.assign(form.time, merged.time)
  // 和 `osd.items` 一样是**整份换掉数组引用**。这一点在这里更要紧：后端
  // `config_to_json` 在 `items` 为空时**根本不发 `channels` 键**，所以"这份配置
  // 里几路都没有"和"后端没答这一项"看起来都是"没有键"—— 前者必须把表单里
  // 残留的旧行清干净，否则用户删掉的通道会随着下一次保存复活。
  Object.assign(form.channels, merged.channels)
  // GET 的 password 字段只有两种可能：哨兵（存过）或空串（没存过）。
  // 无论哪种都不显示真值，只记一个"已设置"用于提示文案。
  const rawWeb = res.data?.web?.password
  const rawOnvif = res.data?.onvif?.password
  hasStoredWebPassword.value = rawWeb === SECRET_SENTINEL
  webPasswordEmpty.value = typeof rawWeb === 'string' && rawWeb.length === 0
  hasStoredOnvifPassword.value = rawOnvif === SECRET_SENTINEL
  // 待重启的项由后端在 GET 里一并给出（见 types.ts 里 `pending` 的说明）。
  // 老后端不发这个字段，所以缺省是空数组而不是报错。
  pendingFields.value = Array.isArray(res.data?.pending) ? res.data.pending : []
  tiers.value = Array.isArray(res.data?.quality_tiers) ? res.data.quality_tiers : []
  latencyTiers.value = Array.isArray(res.data?.latency_tiers) ? res.data.latency_tiers : []
  webPasswordInput.value = ''
  onvifPasswordInput.value = ''
  // 重新载入之后"上次按保存被拦下"这件事不再有意义：表单已经不是那一份了。
  saveAttempted.value = false
  loaded.value = true
  loading.value = false
  // 见下面那段：从 `/config#video` 这类地址进来时，头一次滚动发生在配置还没
  // 回来、页面上只有一句"正在读取配置"的时候，那时目标段落根本不存在。
  // 排到下一帧 —— 段落要等 `v-else` 那一支渲染出来才存在。
  nextTick(() => scrollToHash(route.hash))
}

onMounted(() => {
  load()
  // 设备列表**另起一个请求**，不和配置合并：列表拉失败（老后端没有这条接口）
  // 不该把整页配置也说成"读取失败"，那会让人以为配置也读不到了。
  loadAudioDevices()
  // 时间信息同理：`/api/time` 是**读态**，不是配置（配置里那个 `time` 段只存
  // 模式与间隔）。它读失败不该把整页配置带崩。
  loadTime()
  // 「通道」那一段要列出**检测到的**显示器与摄像头，这也是另一条接口：
  // `/api/sources` 抓失败时列表空着，但已经配好的每一路照样能编辑（那些行
  // 是从配置里读出来的），所以它同样不该把整页配置说成读取失败。
  //
  // **只抓这一次**。后端算这份快照时真的要建一次 `IDXGIOutputDuplication`
  // 去试"这块屏能不能捕获"，而同一块屏同时只允许一个 —— 见上面 `sources`
  // 的说明，别改成"顺手刷新一下"。
  loadSources()
  // 「主机时间 / 设备时间」两格要自己走（理由见上面 `clockBase` 那段）。
  // 这里**只是把钟启动起来**，对时本身仍旧只发生在读数到手的时候 ——
  // 没有读数时 `scheduleClock()` 会立刻返回，等 `timeInfo` 一到，
  // 那个 watch 会再排一次，所以这里的先后顺序无所谓。
  scheduleClock()
})

/**
 * 段落锚点：`router.ts` 的 `scrollBehavior` 漏掉的那两半。
 *
 *  (a) **表单是异步拉回来的**（上面那个 `load()`）。它跑完之前页面上只有一句
 *      "正在读取配置"，`#video` 这些段落还不存在 —— 那一次滚动无声地什么都没做。
 *      这是 `load()` 结尾那次 `nextTick` 的存在理由。
 *  (b) **已经在 `/config` 上、再点另一个子项时，path 没变**：组件不会重挂，
 *      于是没有"路由变化"可供 `scrollBehavior` 挂钩。靠这里的 watch 补。
 *
 * 两处都调同一个幂等的 `scrollToHash()`；和 `scrollBehavior` 重叠也没关系，
 * 重复滚一次没有副作用。顶部那条状态栏的偏移由 CSS 的 `scroll-margin-top` 管，
 * **不要**在这里再减一次。
 */
watch(
  () => route.hash,
  (h) => {
    // hash 先变、DOM 后更新，所以要等这一帧渲染完才找得到那个元素。
    nextTick(() => scrollToHash(h))
    // 切到音视频那一段时把设备列表重读一次。组件在 `/config` 内部换锚点**不会
    // 重挂**，光靠 onMounted 的话，用户拔了耳机再点回这一段，看到的下拉还是
    // 进页面那一刻的那一份 —— 而"列表是新的"正是这一段唯一的正确性要求。
    if (h === '#audio') loadAudioDevices()
    // 时间这一段同理，理由略有不同：那几个数**会自己变**（NTP 定时对时每
    // `ntp_interval_s` 一次，NVR 也可能随时来调），进页面那一刻读到的不一定
    // 还是现在这个。而这一段上显示的正是"此刻差多少"，过期的数在这里就是错的
    // 数 —— 不是"略旧"。
    if (h === '#time') loadTime()
  },
)

type Errors = Record<string, string>

/**
 * 客户端校验。范围按约定写死，但这些只是"早一点告诉用户"，
 * 最终仍以后端 validate_config() 的夹取和它返回的错误为准。
 */
const errors = computed<Errors>(() => {
  const e: Errors = {}
  const int = (path: string, v: unknown, lo: number, hi: number, name: string) => {
    const n = typeof v === 'number' ? v : NaN
    if (!Number.isFinite(n) || !Number.isInteger(n)) {
      e[path] = `${name}必须是整数`
    } else if (n < lo || n > hi) {
      e[path] = `${name}应在 ${lo}–${hi} 之间`
    }
  }

  int('capture.display_index', form.capture.display_index, 0, 64, '显示器序号')
  int('capture.fps', form.capture.fps, 1, 120, '帧率')
  int('capture.bitrate_kbps', form.capture.bitrate_kbps, 100, 50000, '码率')
  int('capture.gop_seconds', form.capture.gop_seconds, 1, 10, 'GOP 间隔')
  int('capture.threads', form.capture.threads, 0, 128, '编码线程数')
  int('capture.max_height', form.capture.max_height, 0, 4320, '分辨率上限')
  int('rtsp.port', form.rtsp.port, 1, 65535, 'RTSP 端口')
  int('rtsp.sub_height', form.rtsp.sub_height, 120, 2160, '子码流高度')
  int('rtsp.sub_bitrate_kbps', form.rtsp.sub_bitrate_kbps, 32, 100000, '子码流码率')
  int('web.port', form.web.port, 1, 65535, 'Web 端口')

  if (!form.rtsp.path) e['rtsp.path'] = '路径不能为空'
  else if (!form.rtsp.path.startsWith('/')) e['rtsp.path'] = '路径必须以 / 开头'

  if (!form.rtsp.sub_path) e['rtsp.sub_path'] = '路径不能为空'
  else if (!form.rtsp.sub_path.startsWith('/')) e['rtsp.sub_path'] = '路径必须以 / 开头'
  // 两条 ServerMediaSession 挂同一个路径时只有一个能生效，而无论哪个赢，
  // 现象都是一条能正常播的流 —— 分不出它是主还是子。后端也会拦，这里先拦一道。
  else if (form.rtsp.sub_enabled && form.rtsp.sub_path === form.rtsp.path) {
    e['rtsp.sub_path'] = '不能与主码流路径相同'
  }

  if (!form.web.host.trim()) e['web.host'] = '监听地址不能为空'
  if (!form.web.static_root.trim()) e['web.static_root'] = '静态目录不能为空'
  // 开了登录却不给用户名，等于让人对着一个填什么都进不去的框。空口令后端会
  // 有一整套说法（见下面的安全提示），但用户名为空是真的没法登。
  if (form.web.auth_enabled && !form.web.user.trim()) {
    e['web.user'] = '开启登录时用户名不能为空'
  }

  if (form.onvif.enabled && !form.onvif.user.trim()) e['onvif.user'] = '开启 ONVIF 时用户名不能为空'

  // 音频。范围与后端 `validate_config` 的夹取值逐条对齐（8..512 / 0..8）——
  // 这里松一点或者紧一点都会变成"界面说不行、后端其实接受"或者反过来。
  int('audio.bitrate_kbps', form.audio.bitrate_kbps, 8, 512, '音频码率')
  const gain = (path: string, v: unknown, name: string) => {
    const n = typeof v === 'number' ? v : NaN
    if (!Number.isFinite(n)) e[path] = `${name}必须是数字`
    else if (n < 0 || n > 8) e[path] = `${name}应在 0–8 之间（0 = 静音这一路）`
  }
  gain('audio.mic_gain', form.audio.mic_gain, '麦克风增益')
  gain('audio.system_gain', form.audio.system_gain, '系统声音增益')
  // 勾了源却一个都没勾：后端不会报错（它只是开一条没有源的音轨），但那对
  // NVR 来说就是"有音轨、永远静音"，比不开音频更难查。所以在这里拦一次。
  if (form.audio.enabled && !form.audio.system && !form.audio.microphone) {
    e['audio.system'] = '开启了音频却一个来源都没勾'
  }

  // 水印各项。上限 4 是后端 `OsdConfig::kMaxItems`；这里的判据是"界面上加不出来"
  // （`addOsdItem` 里挡），所以这一条只在**别人手改过配置文件**时才可能命中。
  if (form.osd.items.length > OSD_MAX_ITEMS) {
    e['osd.items'] = `最多 ${OSD_MAX_ITEMS} 项水印`
  }
  form.osd.items.forEach((it, i) => {
    int(`osd.items.${i}.x_pct`, it.x_pct, 0, 100, '横向位置')
    int(`osd.items.${i}.y_pct`, it.y_pct, 0, 100, '纵向位置')
    int(`osd.items.${i}.font_size`, it.font_size, 0, 512, '字号')
    // 勾着、类型是固定文字、内容却是空的 —— 画出来是一片空白，而用户会觉得
    // "水印开了怎么什么都没看见"。名字与时间那两类的内容是推出来的，不判。
    if (it.enabled && it.type === 'text' && !it.text.trim()) {
      e[`osd.items.${i}.text`] = '固定文字不能为空'
    }
  })

  // 通道 2 起（通道 1 由上面 capture/rtsp 那两段管）。上限 8 是后端
  // `ChannelsConfig::kMaxChannels`；和 OSD 一样，界面上本来就加不出来，
  // 这一条只在**别人手改过配置文件**时才可能命中。
  //
  // 这里比后端 `validate_config` **严**一处：后端遇到重复的 `display_index`
  // 或重复的 `rtsp_path` 会**悄悄丢掉整项**（只留一条 note），而界面上一份
  // 看着四路齐全的配置真的按下去就少一路。所以在发出之前就拦下来，让用户
  // 自己决定改哪个。
  if (form.channels.items.length > CHANNEL_MAX_ITEMS) {
    e['channels.items'] = `最多 ${CHANNEL_MAX_ITEMS} 路通道（含通道 1 之外的这些）`
  }
  form.channels.items.forEach((it, i) => {
    int(`channels.items.${i}.fps`, it.fps, 1, 120, '帧率')
    int(`channels.items.${i}.bitrate_kbps`, it.bitrate_kbps, 100, 100000, '码率')
    // 0 = 不缩放，同 capture.max_height。
    int(`channels.items.${i}.max_height`, it.max_height, 0, 4320, '分辨率上限')
    int(`channels.items.${i}.sub_height`, it.sub_height, 120, 2160, '子码流高度')
    int(`channels.items.${i}.sub_bitrate_kbps`, it.sub_bitrate_kbps, 32, 100000, '子码流码率')
    // 勾了摄像头却没选设备：后端会拿 `camera` 去匹配友好名，匹配不上就
    // **建不出这一路**（`blocked_reason` 非空），而界面上那一行看着是开的。
    if (it.source === 'webcam' && !it.camera.trim()) {
      e[`channels.items.${i}.camera`] = '选了摄像头源，得指定用哪一个'
    }
    // 路径留空是合法的（后端会补 `/chN`），但写了就必须以 / 开头 —— 后端
    // 会替它补一条 note 并改写，也就是"存进去的和填的不是一个"。
    const lead = (field: 'rtsp_path' | 'rtsp_sub_path', name: string) => {
      const v = it[field].trim()
      if (v && !v.startsWith('/')) e[`channels.items.${i}.${field}`] = `${name}必须以 / 开头`
    }
    lead('rtsp_path', '主码流路径')
    lead('rtsp_sub_path', '子码流路径')

    // 去重要按**补完斜杠之后**的值比。后端就是先 `lead()` 再占位的，所以
    // `live` 和 `/live` 在它眼里是同一条路由；这里按原样比就会漏掉这一对，
    // 结果是界面放行、后端把整项丢掉。
    const path  = normalizePath(it.rtsp_path)
    const subPath = normalizePath(it.rtsp_sub_path)

    // 与**前面**的项比：后端顺着数组占位，撞上任何一个先来的就丢**后一个**，
    // 所以"哪一项会被丢"和这里的判据一致（报在靠后的那一项上）。
    for (let k = 0; k < i; k++) {
      const prev = form.channels.items[k]
      if (it.source === 'display' && prev.source === 'display' &&
          it.display_index === prev.display_index && !e[`channels.items.${i}.display_index`]) {
        e[`channels.items.${i}.display_index`] =
          `第 ${k + 1} 路已经占了显示器 ${it.display_index} —— 同一块屏只能建一路`
      }
      if (path && path === normalizePath(prev.rtsp_path) &&
          !e[`channels.items.${i}.rtsp_path`]) {
        e[`channels.items.${i}.rtsp_path`] = `与第 ${k + 1} 路的主码流路径重复`
      }
      if (subPath && subPath === normalizePath(prev.rtsp_sub_path) &&
          !e[`channels.items.${i}.rtsp_sub_path`]) {
        e[`channels.items.${i}.rtsp_sub_path`] = `与第 ${k + 1} 路的子码流路径重复`
      }
    }
    // 再和通道 1 那两条**已经占住**的路由比一次 —— 后端的 `used` 里是从
    // `rtsp.path` / `rtsp.sub_path` 起头的。同一份表单，直接读。
    if (path && path === normalizePath(form.rtsp.path) &&
        !e[`channels.items.${i}.rtsp_path`]) {
      e[`channels.items.${i}.rtsp_path`] = '与通道 1 的主码流路径重复'
    }
    if (subPath && subPath === normalizePath(form.rtsp.sub_path) &&
        !e[`channels.items.${i}.rtsp_sub_path`]) {
      e[`channels.items.${i}.rtsp_sub_path`] = '与通道 1 的子码流路径重复'
    }
  })

  // 两个服务抢同一个端口，后起的必然 bind 失败。后端也会拦，但在这里说更清楚。
  if (form.rtsp.enabled && form.web.enabled && form.rtsp.port === form.web.port) {
    e['rtsp.port'] = '与 Web 端口冲突，两者不能相同'
  }
  return e
})

const errorCount = computed(() => Object.keys(errors.value).length)

/**
 * 出错项按**表单顺序**列出来给人看。
 *
 * 只用一条"有 N 项不合法"是不够的：用户看到的是一句话，而要找的那一项可能
 * 在下面几百像素之外、还可能是折叠在某个段落里的。把标签和人话一起列在横幅里，
 * "哪里错了"和"错在哪"就都在眼前。
 */
const errorList = computed(() =>
  Object.entries(errors.value).map(([path, msg]) => ({ path, label: label(path), msg })),
)

/**
 * 用户按过保存、但被校验拦下了。
 *
 * 它只影响横幅的样式（警告 → 错误），判据本身仍然是 `errorCount` ——
 * 拦下之后用户改好了，横幅该自己消失，不需要再点一次保存。
 */
const saveAttempted = ref(false)

/** 表单根节点，用于"滚到第一个出错的输入框"。 */
const formEl = ref<HTMLFormElement | null>(null)

/**
 * 滚到第一个标红的输入框。
 *
 * 用 `.invalid` 这个类当选择器，而不是给每个输入框挂一个 ref：类是由
 * `errors` 算出来的，和红线标注同源，因此不可能出现"滚到了 A、标红的是 B"。
 */
function scrollToFirstError() {
  const el = formEl.value?.querySelector<HTMLElement>('.invalid')
  el?.scrollIntoView({ behavior: 'smooth', block: 'center' })
  el?.focus?.()
}

/**
 * 把控制台暴露在局域网上、同时又不设门 —— 这一项的风险远高于其它任何配置：
 * 同一网段里任何人都能读配置、改配置、看桌面画面。
 *
 * 判据要用**改写之后**的效果值：用户可能正打算在这一次保存里把登录打开
 * （`webPasswordInput` 里填了口令），那就不该再报"敞着"。
 */
const exposedWithoutAuth = computed(() => {
  if (!form.web.enabled) return false
  if (form.web.host !== '0.0.0.0' && form.web.host !== '::') return false
  if (!form.web.auth_enabled) return true
  return !hasStoredWebPassword.value && webPasswordInput.value.length === 0
})

/** 表单里填的地址，给 NVR 手工添加通道时抄。 */
const rtspPreview = computed(() => {
  const p = form.rtsp.path || '/live'
  // 浏览器不知道本机的局域网 IP，只能用访问本页的地址来近似；真实的推流地址
  // 以状态页上后端给的 stats.rtsp_url 为准。
  return `rtsp://${location.hostname}:${form.rtsp.port}${p.startsWith('/') ? p : '/' + p}`
})

function buildPayload(): AppConfig {
  const payload = JSON.parse(JSON.stringify(form)) as AppConfig
  // 用户没动口令框，就把哨兵原样发回去。
  //
  // 这里**不能**改成"干脆不发这个键"：后端的 take() 对缺失键是保留默认值，
  // 而 password 那一项的默认值是空串，于是 "" != "__unchanged__" 成立，
  // 结果是把已存的口令清掉 —— 表现就是"改个帧率顺手保存，认证就失效了"。
  //
  // 两套凭据各发各的哨兵。混用会让"改 Web 口令"顺手把 NVR 那份也改掉。
  // 档位发的是**推导出来的**那一个：手改过具体字段就发 `custom`，否则后端会
  // 照着档位把刚改的值覆盖回去（"改了帧率、保存、帧率又变回去"就是这么来的）。
  payload.capture.quality = effectiveQuality()
  // 同一条纪律：发**推导出来的**那一档。手改过 `threads` 或 `gop_seconds`
  // 就发 `custom`，否则后端会照着档位把刚改的值覆盖回去。
  payload.capture.latency = effectiveLatency()
  payload.web.password = webPasswordInput.value === '' ? SECRET_SENTINEL : webPasswordInput.value
  payload.onvif.password =
    onvifPasswordInput.value === '' ? SECRET_SENTINEL : onvifPasswordInput.value
  // `channels` 必须**每次都在**，哪怕一项都没有。
  //
  // 后端的 `config_to_json` 在 items 为空时根本不发这个键（那是有意的兼容
  // 边界，见 types.ts），而 `config_from_json` 对**缺失**的键是"保持原样"。
  // 于是"把最后一路删掉再保存"如果不显式发一个空数组，后端会认为这次改动
  // 没提通道这回事 —— 删掉的那一路原封不动地回来。空数组才是"清空"。
  //
  // 这一句不是多余的：`JSON.parse(JSON.stringify(form))` 复制的是**表单**，
  // 而表单里 `channels` 因为 `defaults()` 恒存在。留着它是为了钉住这个不变量
  // —— 将来若有人把 `defaults()` 里的 channels 改成可选，这里会立刻兜住。
  payload.channels = { items: (payload.channels?.items ?? []) as ChannelConfig[] }
  return payload
}

/**
 * 点分路径 → 人话。**认不出来的路径原样显示**，不是藏起来 —— 后端加了新字段
 * 而这张表没跟上时，用户至少还能看到 `rtsp.something_new` 这个名字，而不是
 * 一条莫名其妙的"有 1 项需要重启"却不说是什么。
 */
const FIELD_LABELS: Record<string, string> = {
  'capture.display_index': '显示器',
  'capture.fps': '帧率',
  'capture.bitrate_kbps': '码率',
  'capture.gop_seconds': '关键帧间隔',
  'capture.threads': '编码线程数',
  'capture.preset': '编码 preset',
  'capture.tune': '编码 tune',
  'capture.profile': 'H.264 档位',
  'capture.quality': '画质档位',
  'capture.latency': '延时档位',
  'capture.max_height': '分辨率上限',
  'rtsp.enabled': 'RTSP 开关',
  'rtsp.port': 'RTSP 端口',
  'rtsp.path': '主码流路径',
  'rtsp.sub_enabled': '子码流开关',
  'rtsp.sub_path': '子码流路径',
  'rtsp.sub_height': '子码流高度',
  'rtsp.sub_bitrate_kbps': '子码流码率',
  // 音频这一组是**冷**字段（要重开设备，见段落里那句「保存并立即应用」），
  // 所以它们在 `pending` 里经常出现 —— 少一条就是"有 1 项需要重启"说不出是哪项。
  'audio.enabled': '音频开关',
  'audio.system': '系统声音',
  'audio.microphone': '麦克风',
  'audio.device': '麦克风设备',
  'audio.bitrate_kbps': '音频码率',
  'audio.mic_gain': '麦克风增益',
  'audio.system_gain': '系统声音增益',
  'web.enabled': 'Web 开关',
  'web.host': 'Web 监听地址',
  'web.port': 'Web 端口',
  'web.static_root': '静态文件目录',
  'web.auth_enabled': 'Web 登录开关',
  'web.user': 'Web 登录用户名',
  'web.password': 'Web 登录口令',
  'onvif.enabled': 'ONVIF 开关',
  'onvif.user': 'ONVIF 用户名',
  'onvif.password': 'ONVIF 口令',
  'onvif.manufacturer': '厂商',
  'onvif.model': '型号',
  'onvif.firmware_version': '固件版本',
  'onvif.hardware_id': '硬件版本',
  'onvif.serial_number': '序列号',
  'onvif.device_uuid': '设备 UUID',
  'onvif.device_name': '设备名',
  'onvif.location': '位置',
  // `osd.items` 是**整体**一项：后端 `diff_config` 走的是 `config_to_json` 的两棵树，
  // 而数组在那里是当标量比的，所以"改了第 3 项的格式"报出来就是"水印项变了"。
  // 逐项 diff 要另写一份比较器，收益不值这个复杂度 —— 界面上那段就在眼前，
  // 改了什么自己看得见。（第 6 条风险里写明了这个取舍。）
  'osd.enabled': '水印开关',
  'osd.adaptive_color': '水印自适应配色',
  'osd.outline': '水印描边',
  'osd.plate_alpha': '水印底板不透明度',
  'osd.items': '水印项',
  // 通道 2 起。和 `osd.items` 一样是**整体**一项（后端 `diff_config` 把数组当
  // 标量比），所以「改了第 3 路的帧率」报出来就是"通道项变了"。
  'channels.items': '通道项',
  'adaptive.mode': '自适应模式',
  'adaptive.min_fps': '自适应帧率下限',
  // 时间这一组的四项里**只有后三项是"要重启才生效"的**：`offset_s` 与
  // `ntp_server` / `ntp_interval_s` 由启动时那一步和定时器读，改完要重启；
  // 而 `mode` 影响的是 `GetSystemDateAndTime` 报 Manual 还是 NTP，也是启动时定的。
  // 所以它们经常一起出现在 `pending` 里 —— 少一条就是"有 N 项需要重启"说不出是哪项。
  'time.mode': '对时模式',
  'time.ntp_server': 'NTP 服务器',
  'time.ntp_interval_s': '对时间隔',
  'time.offset_s': '设备时间偏移',
  // 操作日志这两项**在配置页上没有输入框**（要改就改配置文件或命令行），但标签
  // 照样得有：这两个字段是**冷的**（只在启动时读一次），一旦有人经 `POST /api/config`
  // 或直接编辑 config.json 把它们改了，它们就会出现在「要重启才生效」那份清单里
  // —— 而那时没有标签的话，清单上写的是一个 `log.audit_keep_days` 这样的裸键名。
  'log.audit': '操作日志开关',
  'log.audit_keep_days': '操作日志留存天数',
}

/** 水印项里每个字段的人话。用来拼 `第 N 项水印的<某个字段>`。 */
const OSD_ITEM_FIELD_LABELS: Record<string, string> = {
  enabled: '开关',
  type: '类型',
  text: '文字',
  position: '位置',
  x_pct: '横向位置',
  y_pct: '纵向位置',
  font_size: '字号',
  date_format: '日期格式',
  time_format: '时间格式',
}

/** 通道项里每个字段的人话。用来拼 `第 N 路的<某个字段>`。 */
const CHANNEL_FIELD_LABELS: Record<string, string> = {
  enabled: '开关',
  name: '名字',
  source: '源类型',
  display_index: '显示器',
  camera: '摄像头',
  quality: '画质档位',
  fps: '帧率',
  bitrate_kbps: '码率',
  max_height: '分辨率上限',
  sub_enabled: '子码流开关',
  sub_height: '子码流高度',
  sub_bitrate_kbps: '子码流码率',
  rtsp_path: '主码流路径',
  rtsp_sub_path: '子码流路径',
}

/**
 * 点分路径 → 人话。
 *
 * OSD 的每一项都有下标（`osd.items.2.font_size`），这里把它翻成
 * "第 3 项水印的字号"。**下标只在这句话里出现，不写进持久化的东西里** ——
 * 删掉一项之后所有下标都会往前挪，那一刻起旧提示里的序号就是错的。
 *
 * 通道项同理，**但序号要 +2**：`channels.items[0]` 是**通道 2**（通道 1 不在
 * 这个数组里）。这里写成"第 1 路"会和界面上那行标题的"通道 2"对不上，
 * 而这条提示的用处正是"让你找到那一行"。
 */
function label(path: string): string {
  const direct = FIELD_LABELS[path]
  if (direct) return direct
  const m = /^osd\.items\.(\d+)\.(.+)$/.exec(path)
  if (m) return `第 ${Number(m[1]) + 1} 项水印的${OSD_ITEM_FIELD_LABELS[m[2]] ?? m[2]}`
  const c = /^channels\.items\.(\d+)\.(.+)$/.exec(path)
  if (c) return `通道 ${Number(c[1]) + 2} 的${CHANNEL_FIELD_LABELS[c[2]] ?? c[2]}`
  return path
}

/** 一次保存里**当场生效**的字段 —— 和 pending 是互补的两半。 */
const appliedFields = ref<string[]>([])

/**
 * 校验不通过时那一套动作：点出横幅、滚到第一个错处。**不静默返回。**
 *
 * 原先这里是一句光秃秃的 `return`，而叠加了原生约束校验（`step` 把 1024 判成
 * stepMismatch）之后，用户那边的体验是"点了保存，什么都没发生，也没有一句话"。
 * 现在有两个入口会走这条路（普通保存 与 音视频那段的「保存并立即应用」），
 * 所以它单独一个函数 —— 复制一遍的话，将来只改一处就会让另一个入口重新变成
 * "什么都没发生"。
 */
async function rejectWithErrors() {
  saveAttempted.value = true
  saveError.value = ''
  saveOk.value = ''
  await nextTick()
  scrollToFirstError()
}

/**
 * 保存表单并做保存后的收尾。返回是否成功（失败时 `saveError` 已经填好）。
 *
 * 从 `onSave()` 里抽出来是为了给「保存并立即应用」用 —— 那一步要的是"先存下去、
 * 再重开管线"，而"存下去"这一半必须和普通保存**逐字一致**（重载表单、刷新待重启
 * 清单、重新问一次会话、清口令框）。抄一份的话，两处迟早只改一处。
 */
async function saveForm(): Promise<boolean> {
  saveAttempted.value = false
  saving.value = true
  saveError.value = ''
  saveOk.value = ''
  appliedFields.value = []
  const res = await saveConfig(buildPayload())
  saving.value = false
  if (!res.ok) {
    saveError.value = res.error
    return false
  }
  appliedFields.value = Array.isArray(res.data?.applied) ? res.data.applied : []
  // 保存成功后重新拉一次：后端会夹取越界值，界面应该显示真正生效的那份，
  // 而不是用户刚敲进去、可能已经被改掉的那份。顺带把**待重启清单**也刷新成
  // 后端的权威版本 —— 它比这次的回应更全（还包含前几次留下的）。
  await load()
  // 登录设置是**当场生效**的，所以这一次保存很可能改变了"现在这个会话还算不算
  // 数"：把登录打开之后，手上这个没有令牌的会话会被踢回登录页。重新问一次
  // /api/session，让顶栏和路由守卫按真实情况动作，而不是等下一次刷页面。
  await ensureSession(true)
  // 文案直接采用后端的说法：哪些算"已生效"只有后端知道（比如 ONVIF 关着的
  // 时候改口令并不生效），前端自己编一句就会和后端打架。
  saveOk.value = res.data?.note ?? '已保存。'
  webPasswordInput.value = ''
  onvifPasswordInput.value = ''
  return true
}

async function onSave() {
  if (errorCount.value > 0) {
    await rejectWithErrors()
    return
  }
  await saveForm()
}

async function onRestartOnvif() {
  restarting.value = true
  restartMsg.value = ''
  const res = await restartOnvif()
  restarting.value = false
  restartMsg.value = res.ok ? '已请求重启 ONVIF / WS-Discovery 响应器。' : `重启失败：${res.error}`
}

// ---- 生命周期：重启服务 / 恢复出厂 ------------------------------------------

/**
 * 这两个动作和上面那个「重启 ONVIF 响应器」**不是一回事**：那一个只重播一次
 * WS-Discovery 组播，画面一秒都不中断；这两个会把**进程本身**换掉。
 *
 * 已知的三条代价（按钮旁边要写全，不能只留一个"重启"字）：
 *   1. 当前录制中断并分段（新文件从 main.1.mp4 续）；
 *   2. 已连上的 RTSP 客户端断开，几秒后自己重连；
 *   3. **所有网页会话失效，必须重新登录** —— 令牌签的是每进程一份的随机密钥。
 *
 * 第 3 条决定了下面这个状态机的形状：新进程回来时我们能认出的第一个信号是
 * **401**，而它是"重启成功了"的证据，不是失败。
 */
type LifecyclePhase = 'idle' | 'restart' | 'reset' | 'done' | 'fail'

const lifecycle = ref<{ phase: LifecyclePhase; message: string }>({ phase: 'idle', message: '' })
const lifecycleBusy = computed(
  () => lifecycle.value.phase === 'restart' || lifecycle.value.phase === 'reset',
)
/** 恢复出厂的二次确认面板。不用 `window.confirm`：它在某些环境下会被静默吃掉。 */
const resetConfirmOpen = ref(false)

/**
 * 轮询的节奏，两处取舍都写在这里，免得以后有人凭手感调。
 *
 * `POLL_MS = 500`：控制台与裸形态是"子进程带 `--handoff 800` 起、等 800 ms 才
 * 去 bind"，所以旧进程退出到新进程监听之间至少空着 800 ms —— 500 ms 的间隔
 * 必然能采到"连不上"这一相，而那一相正是"它确实重启过"的唯一证据（见
 * `waitForBack`）。服务形态的空窗可能更短（看门狗原地重拉），但那条路上登录
 * 默认是开着的，401 那条判据与空窗长短无关。
 *
 * `POLL_LIMIT_MS = 40_000`：够慢的机器起两遍，但绝不无限转圈 ——"一直转圈"
 * 这个失败模式比报错坏得多，因为用户会一直等下去。
 */
const POLL_MS = 500
const POLL_LIMIT_MS = 40_000

type ProbeResult = 'ok' | 'unauthorized' | 'down' | 'other'

/**
 * 探一次 `/api/status`。**故意不走 `api.ts`。**
 *
 * 那里的 401 处理是"会话过期"，会调 `useAuth.noteUnauthorized()` 把
 * `authenticated` 置 false，而 `App.vue` 的 `watch(authenticated)` 会立刻把人
 * 送到 `/login?next=…` —— 我们等在登录页上要显示的那句"为什么被踢出来"就被
 * 那一跳冲掉了（它不带 `reason`）。这里只取状态码，一个副作用都不要。
 */
async function probeStatus(): Promise<ProbeResult> {
  try {
    const resp = await fetch('/api/status', {
      credentials: 'same-origin',
      headers: { Accept: 'application/json' },
      cache: 'no-store',
    })
    if (resp.status === 401) return 'unauthorized'
    return resp.ok ? 'ok' : 'other'
  } catch {
    // fetch 只在网络层失败时 reject。端口关着就是这一条，也正是我们要等的。
    return 'down'
  }
}

type BackOutcome = { kind: 'login' | 'ok' | 'timeout'; sawDown: boolean }

/**
 * 等新进程回来。两个判据都必须满足"只有新进程做得到"。
 *
 * - **401 ⇒ 一定是新进程**（旧令牌在新密钥下验不过，旧进程不可能给出 401）。
 *   重启必然换密钥，所以这是最常见的一种，也是唯一不需要额外前提的一种。
 * - **200 ⇒ 只有先看见过至少一次连不上才算数。** 请求被受理之后旧进程还要几百
 *   毫秒才真的退出，那期间它答得好好的；把"第一个 200"当成"它回来了"就会对着
 *   旧进程宣布成功。所以 `sawDown` 是这条判据的**前提**，不是修饰。
 */
async function waitForBack(): Promise<BackOutcome> {
  const deadline = Date.now() + POLL_LIMIT_MS
  let sawDown = false
  while (Date.now() < deadline) {
    const r = await probeStatus()
    if (r === 'unauthorized') return { kind: 'login', sawDown }
    if (r === 'down') sawDown = true
    else if (r === 'ok' && sawDown) return { kind: 'ok', sawDown }
    await new Promise((resolve) => setTimeout(resolve, POLL_MS))
  }
  return { kind: 'timeout', sawDown }
}

/**
 * 请求到底被受理了没有。
 *
 * **`res.ok` 回答不了这个问题。** 路由只在回调存在时才注册（`web_server.cpp`
 * 里那条纪律），所以后端要是没有这条路（前端比后端新 —— 装完升级包忘了重启，
 * 或者直接拿 dist 打了一个旧 exe），请求会落到静态兜底上、拿到 200 + 一份
 * index.html；而 `postJson` 把"200 但不是 JSON"也当成功（它对空体和解析失败
 * 都回 `{ok:true, data:undefined}`）。两条叠起来，"功能根本不存在"和"请求已
 * 受理"在 `res.ok` 上长得一模一样，界面会对着一个永远不来的重启转 40 秒。
 *
 * 所以判据是**回应里那个明确的信号位**，不是 HTTP 状态。
 */
function lifecycleAccepted(
  res: ApiResult<LifecycleResult>,
  flag: 'restarting' | 'resetting',
): string | null {
  if (!res.ok) return res.error
  const d = res.data
  const signalled = flag === 'restarting' ? d?.restarting === true : d?.resetting === true
  if (signalled) return null
  return (
    '后端没有回这个标记，说明这条接口在本机上没有被注册（多半是前端比后端新）。' +
    '请求可能落在了静态兜底上 —— 也就是什么都没发生，服务并没有重启。'
  )
}

/** 超时那句话要说真话，包括"它可能压根就没停过"这一种。 */
function lifecycleTimeoutMessage(sawDown: boolean, isReset: boolean): string {
  return (
    `等了 ${POLL_LIMIT_MS / 1000} 秒，没能确认服务${isReset ? '恢复完成' : '回来'}（` +
    (sawDown ? '期间看到过连接中断' : '期间一次都没看到连接中断') +
    '）。先刷新一下这个页面：如果它正常打开，说明服务其实一直在跑；' +
    '否则去看日志 %APPDATA%\\Desk2ONVIF\\desk2onvif.log（和 config.json 在同一个目录）。'
  )
}

/**
 * 两个动作的后半段是同一件事：等它回来，然后要么解释"你为什么被踢出来了"，
 * 要么在没开登录的机器上直接报完成。差别只在措辞，所以合在一个函数里。
 */
async function pollUntilBack(kind: 'restart' | 'reset'): Promise<void> {
  const back = await waitForBack()

  if (back.kind === 'timeout') {
    lifecycle.value = { phase: 'fail', message: lifecycleTimeoutMessage(back.sawDown, kind === 'reset') }
    return
  }

  if (back.kind === 'login') {
    // 401 只可能来自新进程。这一跳既是被踢出去，也是"重启完成"的证明 ——
    // 所以它不算失败，回到登录页之后由 LoginView 把前因后果说清楚。
    await router.replace({
      name: 'login',
      query: { next: '/config#system', reason: kind === 'reset' ? 'reset' : 'restarted' },
    })
    return
  }

  // kind === 'ok'：这台机器上没开网页登录，所以没有 401 可用，只能靠
  // "先断过、再通"来判 —— 而它照样是确定的。
  lifecycle.value = {
    phase: 'done',
    message: `${kind === 'reset' ? '恢复出厂' : '重启'}已完成，服务回来了（这台设备没开网页登录，所以会话没受影响）。`,
  }
}

async function onRestartService(): Promise<void> {
  if (lifecycleBusy.value) return
  resetConfirmOpen.value = false
  lifecycle.value = { phase: 'restart', message: '正在重启服务…' }

  const res = await restartService()
  const bad = lifecycleAccepted(res, 'restarting')
  if (bad) {
    lifecycle.value = { phase: 'fail', message: `重启请求没有被受理：${bad}` }
    return
  }

  // 先写下解释。理由见 lifecycle.ts：把人送到登录页的有两条路，谁都可能先到，
  // 而"重启会让会话失效"在请求被受理这一刻就已经是确定的了。
  setLifecycleNote(
    'restarted',
    '你刚才在配置页点了「重启服务」。重启会把网页会话的签名密钥整个换掉，' +
      '所以所有网页会话都失效了（不只是你这一台）—— 被送到这个登录页是意料之中的，' +
      '用原来的账号重新登录即可。正在录制的画面会从那一下分成两个文件，' +
      'NVR 那边断开几秒后会自己重连。',
  )
  await pollUntilBack('restart')
}

async function onFactoryReset(): Promise<void> {
  if (lifecycleBusy.value) return
  resetConfirmOpen.value = false
  lifecycle.value = { phase: 'reset', message: '正在恢复出厂设置…' }

  const res = await factoryReset()
  const bad = lifecycleAccepted(res, 'resetting')
  if (bad) {
    lifecycle.value = { phase: 'fail', message: `恢复出厂设置没有被受理：${bad}` }
    return
  }

  setLifecycleNote(
    'reset',
    '你刚才点了「恢复出厂设置」。配置已经回到全新状态：config.json 先备份成 ' +
      'config.json.bak 再删除、操作日志与调试日志清空、设备时间偏移归零、名称复位，' +
      '随后服务重启。重启换掉了会话密钥，所以你必须重新登录；' +
      '**口令也回到了默认值**（安装包形态是 admin / admin）—— 除非当时是用命令行参数指定的口令，' +
      '那种情况下参数照样生效。',
  )
  await pollUntilBack('reset')
}

// ---- 音视频（音频）---------------------------------------------------------

/**
 * 麦克风下拉框的设备列表。
 *
 * **每次进页面现拉，不缓存** —— 插拔一个耳机列表就变，缓存住的话下拉里会一直
 * 留着一台已经拔掉的设备，而选中它得到的是一条永远静音的音轨（SDP 和 ffprobe
 * 看上去都正常，所以没人会怀疑是设备的问题）。除了进页面，切到 `#audio` 这个
 * 锚点时也重拉一次，另外还有一个手动「重新读取」。
 */
const audioDevices = ref<AudioDevice[]>([])
const audioDevicesLoading = ref(false)
/**
 * 列表读不到的原因（接口没注册 / COM 起不来 / 网络失败）。
 *
 * 和"这台机器上就没插麦克风"是**两件事**：前者要说到人话，后者只是列表短。
 * 后端把枚举失败单独放在 `capture_error` 里也是同一个理由。
 */
const audioDevicesError = ref('')

async function loadAudioDevices() {
  audioDevicesLoading.value = true
  const res = await fetchAudioDevices()
  audioDevicesLoading.value = false
  if (!res.ok) {
    audioDevicesError.value = res.error
    audioDevices.value = []
    return
  }
  const list = Array.isArray(res.data?.capture) ? res.data.capture : []
  audioDevices.value = list.map((d) => ({
    id: str(d?.id, ''),
    // 名字空的设备照样列出来：`<option>` 里空字符串会变成一个看不见的选项，
    // 用户只会觉得"这个下拉框坏了"。
    name: str(d?.name, '') || '(未命名设备)',
    default: bool(d?.default, false),
  }))
  audioDevicesError.value = str(res.data?.capture_error, '')
}

/**
 * 配置里存着的麦克风**现在枚举不到**了（拔了、换了、或者枚举整个失败）。
 *
 * 不处理的话下拉会自己跳回「默认设备」—— 那是一次**静默改写**：用户只是想改个
 * 增益，保存下去连同"用哪只麦克风"一起被改掉，而界面上从头到尾一切正常。
 * 所以把它作为一个选项留在列表里，并写明为什么。
 */
const missingAudioDevice = computed(() => {
  const id = form.audio.device
  if (!id) return false
  return !audioDevices.value.some((d) => d.id === id)
})

const audioApplying = ref(false)
const audioApplyMsg = ref('')
/** 决定那句话的底色。`err` 里还分"第 1 步就失败"（什么都没变）和"只成功一半"。 */
const audioApplyTone = ref<'ok' | 'warn' | 'err'>('ok')

/**
 * 「保存并立即应用」= `POST /api/config` + `POST /api/reload`，两步都要成功。
 *
 * 为什么音频需要这一下：开关它、换设备、改增益都是**冷**字段（`capture_session`
 * 建管线时就把设备与 SDP 定死了），只保存的话画面上的音轨还是旧的那一条 ——
 * 界面上显示"已保存"、而耳朵里听不出任何变化，是最容易归因到别处的一种状态。
 *
 * 两步要分开报，**不能合成一句"应用失败"**：第 1 步失败时配置一个字都没改
 * （没有副作用）；第 2 步失败却已经把配置存下去了 —— 那是一个"看起来没成功、
 * 其实已经写进文件"的状态，不说清楚的话用户会再点一次，或者以为白改了。
 * 这个两步写法与状态页那个「降到 <下一档>」完全同源（`StatusView.onDowngrade`），
 * 不另发明一套。
 */
async function onAudioApply() {
  if (audioApplying.value) return
  // 校验没过时走和保存**同一个**入口：拦下来、点出清单、滚到第一个错处。
  if (errorCount.value > 0) {
    audioApplyMsg.value = ''
    await rejectWithErrors()
    return
  }
  audioApplying.value = true
  audioApplyMsg.value = ''
  const saved = await saveForm()
  if (!saved) {
    audioApplying.value = false
    audioApplyTone.value = 'err'
    audioApplyMsg.value =
      `第 1 步「保存配置」就失败了，所以没有重开管线：${saveError.value}。` +
      '配置一个字都没改，采集也没有中断。'
    return
  }
  const rl = await reloadPipeline()
  audioApplying.value = false
  if (!rl.ok) {
    audioApplyTone.value = 'err'
    audioApplyMsg.value =
      `只成功了一半：配置已经存下去了，但第 2 步「重开管线」失败 —— ${rl.error}。` +
      '音轨因此还是旧的那一条（音轨是建管线时定的，改配置不会自己换）。' +
      '可以按下面的「保存」之后再单独重试，或者去系统设置里重启服务。'
    return
  }
  audioApplyTone.value = 'ok'
  audioApplyMsg.value =
    rl.data?.live === false
      ? '配置已存下。当前没有在录制，所以没有需要重开的管线 —— 音频会在下次开始录制时按新配置建起来。'
      : '已按新配置重开了采集管线：录像已分段（新文件从 main.1.mp4 续），' +
        '已连上的 RTSP 与 NVR 会在数秒内自己重连。'
  // 成功那句话过一会儿自己收掉，免得它一直挂着 —— 用户下一次改完再看时，
  // 会以为这一句说的是**这一次**。失败的那两种**不自动收**（和状态页同一个取舍）。
  window.setTimeout(() => {
    if (audioApplyTone.value === 'ok') audioApplyMsg.value = ''
  }, 15000)
}

const videoApplying = ref(false)
const videoApplyMsg = ref('')
const videoApplyTone = ref<'ok' | 'warn' | 'err'>('ok')

/**
 * 采集与编码那一段的「保存并立即应用」。走的**是 `onAudioApply` 同一条路**
 * （`saveForm()` + `reloadPipeline()` 两步，两个都成功才算数），只有文案按
 * 这一段的对象换了名词 —— 所以这里刻意不复用 `onAudioApply`：它那几句写的是
 * "音轨还是旧的那一条"，而这一段改的是**编码器的构造参数**（线程数与关键帧
 * 间隔都是建编码器那一刻定死的），照抄会把用户指到一个不存在的地方去。
 *
 * 为什么延时档位需要这一下：它是 `capture.threads` / `capture.gop_seconds`
 * 的展开，与码率同理属于**要重启才生效**那一档（陷阱 51）。只保存的话，
 * 界面上写着「极致低延迟」而编码器还在按旧的线程数跑 —— 这正是本阶段要做的
 * 那个功能里最坏的一种失败：**每一处代码都做到了它承诺的事，只有用户关心的
 * 那个数字没动**。
 */
async function onVideoApply() {
  if (videoApplying.value) return
  if (errorCount.value > 0) {
    videoApplyMsg.value = ''
    await rejectWithErrors()
    return
  }
  videoApplying.value = true
  videoApplyMsg.value = ''
  const saved = await saveForm()
  if (!saved) {
    videoApplying.value = false
    videoApplyTone.value = 'err'
    videoApplyMsg.value =
      `第 1 步「保存配置」就失败了，所以没有重开管线：${saveError.value}。` +
      '配置一个字都没改，采集也没有中断。'
    return
  }
  const rl = await reloadPipeline()
  videoApplying.value = false
  if (!rl.ok) {
    videoApplyTone.value = 'err'
    videoApplyMsg.value =
      `只成功了一半：配置已经存下去了，但第 2 步「重开管线」失败 —— ${rl.error}。` +
      '编码器因此还按旧的线程数与关键帧间隔在跑（这两项是建编码器那一刻定的，' +
      '改配置不会自己换）。可以按下面的「保存」之后再单独重试，' +
      '或者去系统设置里重启服务。'
    return
  }
  videoApplyTone.value = 'ok'
  videoApplyMsg.value =
    rl.data?.live === false
      ? '配置已存下。当前没有在录制，所以没有需要重开的管线 —— 编码器会在下次开始录制时按新参数建起来。'
      : '已按新配置重开了采集管线：录像已分段（新文件从 main.1.mp4 续），' +
        '已连上的 RTSP 与 NVR 会在数秒内自己重连。'
  window.setTimeout(() => {
    if (videoApplyTone.value === 'ok') videoApplyMsg.value = ''
  }, 15000)
}

// ---- 时间 ------------------------------------------------------------------

/**
 * `GET /api/time` 最近一次读到的完整状态。null = 还没读到（或读失败了）。
 *
 * **与 `Stats` 里那六个字段是两个用途，不是两份真相**：那六个走 WebSocket 每秒
 * 刷（状态页顶部那一行要跟着动），这一份是这一页要的"点开那一刻的完整样子"
 * （含 `syncing`、成功/失败计数、`configured_offset_s`）。同源、同一次读取。
 */
const timeInfo = ref<TimeInfo | null>(null)
const timeLoading = ref(false)
/**
 * 读 `/api/time` 失败的原因。
 *
 * **和"读到了一份空的"必须分开**：老后端上压根没有这条接口（它在
 * `if (opt.time_get)` 后面注册），而"接口不存在"与"时间是 0 偏移"在界面上
 * 长得一模一样 —— 后者看着完全正常，于是没人会去查前者。
 */
const timeError = ref('')
/** 校准 / 归零 / 立刻对时：三个动作共用一把闸，避免连点出两次互相覆盖的写。 */
const timeBusy = ref(false)
const timeMsg = ref('')
const timeTone = ref<'ok' | 'warn' | 'err'>('ok')
/**
 * 手动模式那个绝对时间输入框。
 *
 * **刻意不预填当前设备时间。** 预填看着贴心，但它把两件事混在一起了：
 * 输入框里的值会被服务端按**它自己所在机器**的时区解析（`parse_local_datetime`），
 * 而我们能拿到的设备时间只有 Unix 秒。跨时区看这个页面时（浏览器与被控机不在
 * 一个时区），"预填之后原样提交"会静默地把钟挪掉几个小时 —— 而界面上一切正常。
 * 所以：输入框留空、用 placeholder 把当前设备时间显示出来当作参照，
 * 用户填什么就是他要什么；"对齐浏览器那个钟"另有一条不依赖时区的路
 * （见 `onTimeAlignToBrowser`）。
 */
const manualTimeInput = ref('')

const p2 = (n: number) => String(n).padStart(2, '0')

/**
 * Unix 秒 → `datetime-local` 的形状（`2026-10-08T15:30:00`）。
 *
 * **不能用 `toISOString()`**：那是 UTC，而 `<input type="datetime-local">` 的
 * 值按定义是本地时刻 —— 混用会在时区非零的地方整整差一个时区，且两边都不报错。
 */
function toLocalStamp(unixSec: number): string {
  const d = new Date(unixSec * 1000)
  return (
    `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())}` +
    `T${p2(d.getHours())}:${p2(d.getMinutes())}:${p2(d.getSeconds())}`
  )
}

/** Unix 秒 → 给人看的一句话（本机时区，秒级）。 */
function fmtStamp(unixSec: number): string {
  if (!unixSec) return '—'
  const d = new Date(unixSec * 1000)
  return (
    `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())} ` +
    `${p2(d.getHours())}:${p2(d.getMinutes())}:${p2(d.getSeconds())}`
  )
}

/** 偏移写成人话：`+1 小时 0 分 0 秒` 这种，比 `+3600` 好读得多。 */
function fmtOffset(sec: number): string {
  if (sec === 0) return '0 秒（与主机一致）'
  const sign = sec < 0 ? '−' : '+'
  let n = Math.abs(Math.round(sec))
  const d = Math.floor(n / 86400)
  n -= d * 86400
  const h = Math.floor(n / 3600)
  n -= h * 3600
  const m = Math.floor(n / 60)
  const s = n - m * 60
  const parts: string[] = []
  if (d) parts.push(`${d} 天`)
  if (h) parts.push(`${h} 小时`)
  if (m) parts.push(`${m} 分`)
  if (s || parts.length === 0) parts.push(`${s} 秒`)
  return `${sign}${parts.join(' ')}`
}

/**
 * 对时结果的人话。
 *
 * ⚠️ **"还没试过"有两种线值，两个都要在这里翻。** `NtpSyncer::Status::last_state`
 * 的初值是 `"never"`，而 `patch_live_counters` 那条路（`Stats`）在回调没注册时
 * 发的是**空串**（见 `useStats.emptyStats()` 里那句注释）。少翻一个的后果不是空白
 * —— 是 `?? s` 把线值原样印到界面上，用户看到的是一个英文单词 `never`。
 */
const SYNC_STATE_TEXT: Record<string, string> = {
  ok: '成功',
  timeout: '超时（服务器没有回包）',
  bad_reply: '回包不可用（太短，或服务器拒绝服务）',
  unreachable: '地址解析不了 / 网络不可达',
  never: '还没试过',
}

function syncStateText(s: string): string {
  if (!s) return '还没试过'
  return SYNC_STATE_TEXT[s] ?? s
}

/**
 * 那一格该不该标红。
 *
 * **"还没试过"不是失败**：`never` 与空串都只是"没有数据"，把它标红等于在一台
 * 从没配过 NTP 的设备上直接报一个红色错误 —— 那会让人去查一个不存在的问题
 * （与 `audio_state = nosignal` 要显示成"无信号"而不是"故障"是同一条纪律）。
 * 所以判据是**白名单式的**：只有明确属于失败的三种线值才标红。
 */
const SYNC_FAIL_STATES = ['timeout', 'bad_reply', 'unreachable']
const timeSyncFailed = computed(
  () => !!timeInfo.value && SYNC_FAIL_STATES.includes(timeInfo.value.last_sync_state),
)

/* ---------------------------------------------------------------------------
 * 让「主机时间 / 设备时间」两格**自己走**。
 *
 * 用户报的原话：这两格"不会实时变化，理论上应该要实时变化，这样就可以发现时间是
 * 和其他时间准确对准了"。这个用途决定了它不是装饰 —— 这两格要回答的是
 * **此刻**对不对得上，所以一个冻住的数在这里不是"略旧"，是**错的**：它会让
 * 用户拿一个几分钟前读到的秒去和手机上的钟比，然后得出"差了 12 秒"这种结论。
 * （状态页顶部那一行之所以看着会走，只是因为它渲染的是 WS 每秒推来的 `Stats`；
 * 这一页的 `timeInfo` 只在三处被写 —— 进页面、点「重新读取」、校准动作的回包 ——
 * 两次之间没有任何东西推进它。这就是那个 bug 的全部。）
 *
 * 走法是"记下**读数是什么时候拿到的**，之后按本地经过的时间往上推"：
 *
 *   - **只用差值，不用本机的绝对时刻。** 从另一台机器上打开这个页面时，浏览器
 *     的钟本身就可能与被控机差着几秒（甚至几分钟）；而"过了多少毫秒"在任何一台
 *     机器上都是准的，那个差在减法里自动消掉。所以这里算的是
 *     `读数 + (现在 − 拿到读数的那一刻)`，不是 `浏览器的钟`。
 *   - **秒的相位取 `host_now_ms`。** 老后端只发整秒（`host_now`），退回
 *     `host_now * 1000` 照样会走，但会带上最多 1 秒的固定相位 —— 也就是大约一半
 *     的时间里显示的秒比真值小 1。那正是这一页要回答的问题本身，所以在有毫秒的
 *     新版后端上要把它用上；旧后端不是"能做得更好而没做"，是做不到。
 *   - **设备时间按 `主机 + 偏移` 推，不各自独立地走。** `/api/time` 上
 *     `device_now == host_now + offset_s` 是后端一次采样算出来的（见 `main.cpp`
 *     里 `wo.time_get` 那段注释），前端跟着同一个关系推，两格之间就不可能漂出
 *     "偏移写着 0、而设备时间比主机快了 3 秒"这种自相矛盾的画面。
 * ------------------------------------------------------------------------- */

/**
 * 这一份读数是什么时候拿到的。
 *
 * `at` 是浏览器自己的毫秒钟（`Date.now()`），只用来算**差值**；`hostMs` 是那一刻
 * 的主机时间（毫秒）；`offsetS` 一起存下来，免得下面两个 computed 各自去
 * `timeInfo` 里再取一遍、并在 `timeInfo` 为空时各自判一次空。
 */
const clockBase = ref<{ at: number; hostMs: number; offsetS: number } | null>(null)
/**
 * 每一拍重算出来的"现在"。
 *
 * 它本身不给任何人看，存在的意义只是**给下面两个 computed 一个依赖** ——
 * 没有它，`Date.now()` 不在 Vue 的依赖图里，值算了就不会再算了。
 */
const clockTick = ref(Date.now())
let clockTimer: number | undefined

/** 后端给的主机时间（毫秒）。有 `host_now_ms` 就用它，没有就退回整秒。 */
function hostMsOf(t: TimeInfo): number {
  return t.host_now_ms ?? t.host_now * 1000
}

/**
 * 手上这一份读数是**取自哪一刻**（主机时间的整秒）。
 *
 * 界面上要把它显示出来：那两格自己走之后，"你现在看到的是什么时刻的数"就成了
 * 一个必须回答的问题（见模板里那个标题的注释）。
 */
const clockReadAtSec = computed<number | null>(() =>
  clockBase.value ? Math.floor(clockBase.value.hostMs / 1000) : null,
)

/** 此刻的主机时间（整秒）。`null` = 还没读到过读数，界面上显示"读取中…"。 */
const liveHostSec = computed<number | null>(() => {
  const t = timeInfo.value
  const b = clockBase.value
  if (!t || !b) return null
  return Math.floor((b.hostMs + (clockTick.value - b.at)) / 1000)
})

/**
 * 此刻的设备时间（整秒）= 主机时间 + 偏移。
 *
 * 加在**整秒**上、而不是加在毫秒上：后端那个 `device_now` 就是
 * `floor(host) + offset_s` 算出来的，照这个顺序算，两端逐秒一致；
 * 先加毫秒再取整会在秒边界上差出 1 秒 —— 恰好是这一页最不能出错的那个数。
 */
const liveDeviceSec = computed<number | null>(() => {
  const h = liveHostSec.value
  if (h === null) return null
  return h + Math.round(clockBase.value?.offsetS ?? 0)
})

/**
 * 排下一拍。
 *
 * **自校正的 `setTimeout`，不是 `setInterval`**：`setInterval` 的每一拍是
 * "上一拍 + 1000"，误差会累积，挂一下午能漂出一整秒；这里每一拍都把下一拍排到
 * **下一个显示秒的边界之后**，误差不累积。后台标签页被节流时两种都会晚，
 * 但这里的值是按 `Date.now()` 推的（不是按拍数累加的），所以回到前台时下一拍
 * 一读就是对的 —— 只是这一拍晚了，不会"补跑"出一串。
 *
 * 那 40 ms 的余量是刻意的：贴着边界排，浏览器那一拍早个几毫秒时读到的还是上一秒，
 * 于是那一秒看起来"少了一帧"。宁可晚 40 ms 换掉这个抖动。
 */
function scheduleClock() {
  if (clockTimer !== undefined) {
    window.clearTimeout(clockTimer)
    clockTimer = undefined
  }
  const b = clockBase.value
  if (!b) return // 还没读到过读数；读到时会再排一次（见下面的 watch）
  const live = b.hostMs + (Date.now() - b.at)
  const frac = ((live % 1000) + 1000) % 1000
  clockTimer = window.setTimeout(() => {
    clockTick.value = Date.now()
    scheduleClock()
  }, 1000 - frac + 40)
}

/**
 * 读数一换，基准就跟着换 —— 而且**立刻**重算一次、把下一拍重新对到相位上。
 *
 * 用 `watch` 而不是在三个赋值点上各写一遍：`timeInfo` 现在有三处写入
 * （`loadTime` / `runTimeAction` / `onTimeAlignToBrowser`），将来多半会有第四处；
 * 漏掉一处的症状是**从那一刻起这两格显示的是一个错的时刻**（还在按上一个基准走），
 * 而那看上去和"正常"一模一样。挂在 ref 上就漏不掉。
 *
 * `flush: 'sync'` 是为了不让"新的 `timeInfo` 配旧的基准"这一瞬间被渲染出去 ——
 * 默认的 pre 刷新下它只存在一个微任务，浏览器根本来不及画，但那是一条要靠推理
 * 才能排除的路径；同步触发把它变成不必推理的一件事。
 */
watch(
  timeInfo,
  (t) => {
    if (!t) {
      clockBase.value = null
      return
    }
    clockBase.value = { at: Date.now(), hostMs: hostMsOf(t), offsetS: t.offset_s }
    clockTick.value = Date.now()
    scheduleClock()
  },
  { flush: 'sync' },
)

/**
 * 偏移**被别的东西改掉**的时候，把这一份重新读一遍（于是一起重新对基准）。
 *
 * 这一半和上面那一半同样要紧，少了它比"钟不走"更坏：NTP 定时对时、NVR 的
 * `SetSystemDateAndTime`、以及这台机器上别的网页会话里的校准，都会在不碰这一页
 * 的情况下把设备时间挪走。不重读的话这两格会**继续以一个过期的偏移往前走** ——
 * 一个"看着在走、而且走得挺像样"的钟，比一个冻住的钟更容易让人相信。
 * （`Stats` 每秒推一次，本来就是现成的信号，`ConfigView` 早就 `useStats()` 了。）
 *
 * 只看**会改变设备时间**的那几个量：偏移本身、模式（切到 NTP 会让偏移归零）、
 * 以及"这个钟被改过没有"和最近一次对时的结果（NTP 对完，偏移在下一帧才变，
 * 拿这两个当提前量，界面少滞后一拍）。
 *
 * ⚠️ **必须写成"源数组"，不能写成 `() => [ ...这几个字段... ]`。**
 * 两者看着是同一种写法，触发的次数却差着一整个数量级：
 * `watch(fn, cb)` 是**单源**，判"变了没有"用的是 `Object.is(新值, 旧值)`，
 * 而 `fn` 每次返回的都是一个**新建的数组** —— 于是只要 `fn` 被重新求值一次，
 * 判据就成立。`fn` 什么时候被求值？它读了 `stats.value.*`，而 `stats.value`
 * 每一帧都被整个换掉（`useStats` 收 WS 那一帧时是整体替换的对象），
 * 所以它**每秒被求值一次**，这个 watch 也就**每秒触发一次**
 * （实测：这一页上 `/api/time` 的请求间隔是 1019 / 1025 / 1014 ms）。
 * 后果不是"多读了几次接口"这么轻 —— 是**下面那两格的走时被整个掩盖了**：
 * 每一秒都用刚读回来的时刻重置一次基准，界面照样在走，于是"钟自己走"
 * 这件事既没做到也看不出来（一条变异检验当场把这一条照了出来：把
 * `liveHostSec` 里的 `clockTick` 项乘 0、让它恒定，界面**照样每秒走一格**）。
 * 写成源数组（`watch([a, b, ...], cb)`）时 Vue 是**逐项**比的
 * （`newValue.some((v, i) => hasChanged(v, oldValue[i]))`），
 * 只有这五个量**本身**变了才触发 —— 那才是这里想要的意思。
 */
watch(
  [
    () => stats.value.device_offset_s,
    () => stats.value.time_mode,
    () => stats.value.time_synced,
    () => stats.value.last_sync_at,
    () => stats.value.last_sync_state,
  ],
  () => {
    void loadTime(true)
  },
)

/**
 * 读一次完整的时间状态。
 *
 * 失败时**不清空已有的那一份**：已经读到过的数比"什么都看不到"有用，
 * 而且读失败的原因已经单独显示了 —— 清空会让用户以为设备时间是空的。
 *
 * `quiet` 是给"用户没按任何按钮"的那些读取用的（上面那个 `stats` watch，
 * 以及将来任何自动重读）。它只做一件事：**不碰 `timeLoading`** ——
 * 那个标志唯一的作用是驱动「重新读取」按钮的禁用态与文案（`timeLoading ?
 * '读取中…' : '重新读取'`），而自动重读每次 NTP 对完都会来一次。不分开的话，
 * 那个按钮会在用户根本没碰它的时候反复闪成"读取中…"，看起来像界面在抽风。
 *
 * 错误横幅的行为**两种模式完全一样**：后台读失败同样要说话，否则"自动重读
 * 悄悄失败"之后，屏幕上那几个还在走的数就成了一个没人知道是假的的钟。
 */
async function loadTime(quiet = false) {
  if (!quiet) timeLoading.value = true
  const res = await fetchTime()
  if (!quiet) timeLoading.value = false
  if (!res.ok) {
    timeError.value = res.error
    return
  }
  timeError.value = ''
  timeInfo.value = res.data
}

/**
 * 手动校准 / 归零：两个动作的骨架**完全一样**（POST → 用回应里那份 `time`
 * 就地刷新，不再补一次 GET），所以合成一个，只有请求那一步不同。
 *
 * 回 200 却**不带 `time`** 时**也去补一次 GET**：那一份是"做完之后重新读的"，
 * 没有它的话界面上会一直显示改动之前的数 —— 而"点了按钮、数字没动"是最容易
 * 被读成"没生效"的一种。
 */
async function runTimeAction(
  act: () => Promise<ApiResult<{ ok: boolean; note?: string; error?: string; time?: TimeInfo }>>,
  fallbackNote: string,
) {
  if (timeBusy.value) return
  timeBusy.value = true
  timeMsg.value = ''
  const res = await act()
  timeBusy.value = false
  if (!res.ok) {
    timeTone.value = 'err'
    timeMsg.value = res.error
    return
  }
  if (res.data?.time) timeInfo.value = res.data.time
  else await loadTime()
  timeTone.value = 'ok'
  timeMsg.value = res.data?.note || fallbackNote
  window.setTimeout(() => {
    if (timeTone.value === 'ok') timeMsg.value = ''
  }, 15000)
}

/** 「设为这个时间」：把输入框里那个绝对时刻交给服务端（它按**它自己**的时区解析）。 */
function onTimeSetAbsolute() {
  const v = manualTimeInput.value.trim()
  if (!v) {
    timeTone.value = 'err'
    timeMsg.value = '先在上面填一个时间（形如 2026-10-08T15:30:00）。'
    return
  }
  runTimeAction(() => setTime({ device_now: v }), '设备时间已按填的那个时刻校准。')
}

/**
 * 「与浏览器时间对齐」。
 *
 * **发的是偏移，不是绝对时刻** —— 这是这一条唯一值得解释的地方。`Date.now()`
 * 是 Unix 毫秒（与时区无关），而服务端给的 `host_now` 也是 Unix 秒，两者相减
 * 得到的偏移同样与时区无关。走 `device_now` 那条路就得把浏览器的本地时刻
 * 交给服务端按**它的**时区解析：跨时区看这个页面时那会整整差一个时区，
 * 而且事后没有任何东西能看出来。
 *
 * 先**重读一次** `host_now` 再算：`timeInfo` 可能是几分钟前读的，拿一个陈旧
 * 基准算出来的偏移会平白带上那段间隔。之后到 POST 落地之间还剩下几百毫秒的
 * 偏差，而设备时间本来就是整秒 —— 无妨，写在这里免得有人拿它当精密对时。
 */
async function onTimeAlignToBrowser() {
  if (timeBusy.value) return
  const fresh = await fetchTime()
  if (!fresh.ok) {
    timeTone.value = 'err'
    timeMsg.value = `先读一次当前时间才能算偏移，但读取失败了：${fresh.error}`
    return
  }
  timeInfo.value = fresh.data
  const offset = Math.round(Date.now() / 1000) - fresh.data.host_now
  runTimeAction(
    () => setTime({ offset_s: offset }),
    `设备时间已对齐到浏览器这个钟（偏移 ${fmtOffset(offset)}）。`,
  )
}

/** 「归零」：偏移清成 0，设备时间回到主机时间。 */
function onTimeReset() {
  runTimeAction(() => resetTime(), '设备时间已恢复成主机时间（偏移归零）。')
}

/**
 * 「立刻对时」。
 *
 * **回 202 且不带结果**，所以这里只报"发出去了" —— 真结果（`last_sync_state`
 * / `last_sync_error`）要等后台那次 UDP 查询有结论。手动模式下这一次**只报
 * 结果、不会改动设备时间**，这句话必须写出来，否则用户会以为点完钟就该动。
 */
async function onTimeSyncNow() {
  if (timeBusy.value) return
  timeBusy.value = true
  timeMsg.value = ''
  const res = await syncTimeNow()
  timeBusy.value = false
  if (!res.ok) {
    timeTone.value = 'err'
    timeMsg.value = res.error
    return
  }
  timeTone.value = 'warn'
  timeMsg.value =
    res.data?.note ||
    '已经把一次对时查询排上队了（UDP 查询最长 2 秒）。查完按上面的「重新读取」看结果。'
}

/** 「重新读取」：把上面这些数拉成最新的。也是 NTP 对完之后取结果的那一步。 */
async function onTimeRefresh() {
  timeMsg.value = ''
  await loadTime()
}

// ---- 水印（OSD）------------------------------------------------------------

/** 后端 `OsdConfig::kMaxItems`。前端这一侧也用它，满了就不给加。 */
const OSD_MAX_ITEMS = 4

/**
 * 后端 `osd_overlay.cpp` 的两个常数：`kAutoFontDiv`（自动字号 = 帧高 / 它）与
 * `kMinFontPx`（字号下限）。**抄这两个数是为了让预览里的字和烧进画面的字一样大**
 * —— 它们是那份实现的参数，改了那边这里就得跟着改。
 */
const OSD_AUTO_FONT_DIV = 32
const OSD_MIN_FONT_PX = 16

const OSD_TYPE_LABELS: Record<string, string> = {
  time: '日期时间（走时）',
  name: '设备名称',
  text: '固定文字',
}

const OSD_POSITION_LABELS: Record<string, string> = {
  upper_left: '左上角',
  upper_right: '右上角',
  lower_left: '左下角',
  lower_right: '右下角',
  custom: '自定义（拖动画面上的标记）',
}

function addOsdItem() {
  if (form.osd.items.length >= OSD_MAX_ITEMS) return
  // 新项落在**左下角**：默认那两项已经占了左上（时间）和右下（名称），
  // 新项放进空着的角比叠在别人身上更容易看见自己加的东西。
  form.osd.items.push({
    enabled: true,
    type: 'text',
    text: '文字水印',
    position: 'lower_left',
    x_pct: 0,
    y_pct: 0,
    font_size: 0,
    date_format: '%Y-%m-%d',
    time_format: '%H:%M:%S',
  })
}

/**
 * 「删除」——**从列表里去掉**。与「关闭」（`enabled = false`，项还在）是两件事，
 * 用户明确要的是两者都有：关掉的项以后还能再打开，删掉的就得重新填一遍。
 */
function removeOsdItem(i: number) {
  form.osd.items.splice(i, 1)
}

// ---- 通道（通道 2 起）-------------------------------------------------------

/** 后端 `ChannelsConfig::kMaxChannels`。 */
const CHANNEL_MAX_ITEMS = 8

/** 画质档位的名字，用来在每一路的下拉框里显示。后端 `quality_tiers` 也给了一份，
 * 但那一份是给**通道 1** 的档位选择器用的（它带 `key` 和字段展开），这里只要名字。 */
const CHANNEL_QUALITY_LABELS: Record<string, string> = {
  high: '高清',
  balanced: '均衡',
  // 与后端 `ChannelConfig::quality` 的默认一致。
  smooth: '流畅',
  verylow: '极低',
  custom: '自定义',
}

/**
 * 检测到的显示器与摄像头。
 *
 * **只在挂载时抓一次**（下面的 `loadSources`）。这不是省事：后端算这份快照时
 * 真的要建一次 `IDXGIOutputDuplication` 去试"这块屏能不能捕获"，而同一块屏
 * 同时只允许一个 duplication —— 于是"顺手刷新一下"会在用户**正在预览**的
 * 时候把预览那条路弄死。要刷新就整页重载。
 */
const sources = ref<Sources | null>(null)
const sourcesError = ref('')

async function loadSources() {
  const res = await fetchSources()
  if (!res.ok) {
    // 抓不到**不是**致命错误：这一段的列表空着，但已经存在的通道照样能编辑
    // （每一行本来就是从配置里读出来的）。所以只记一句提示，不拦页面。
    sourcesError.value = res.error
    return
  }
  sources.value = res.data
  sourcesError.value = ''
}

/** 某块显示器是不是已经被某一项占了（占它的那一路的下标，没被占就给 -1）。 */
function displayOwner(index: number, except = -1): number {
  return form.channels.items.findIndex(
    (it, i) => i !== except && it.source === 'display' && it.display_index === index,
  )
}

/** 某台摄像头是不是已经被某一项占了。用友好名匹配，和后端建流时的判据同一套。 */
function cameraOwner(name: string, except = -1): number {
  return form.channels.items.findIndex(
    (it, i) => i !== except && it.source === 'webcam' && it.camera === name,
  )
}

/** 新加一项时给它一个不撞车的默认路径。用**下标 + 2**（= 通道号），与后端的
 * `lead()` 默认值同源；被占了就往后找一个空的。 */
function freePath(sub: boolean): string {
  const used = new Set(form.channels.items.map((it) => normalizePath(it.rtsp_path)))
  if (!sub) used.add(normalizePath(form.rtsp.path))
  else used.add(normalizePath(form.rtsp.sub_path))
  for (let n = form.channels.items.length + 2; n < 64; n++) {
    const p = (sub ? `/ch${n}_sub` : `/ch${n}`)
    if (!used.has(p)) return p
  }
  return ''
}

/**
 * 加一路。`preset` 决定这一路算哪一号通道 —— 它只影响**默认值**（路径、名字），
 * 序号本身由数组位置决定，这一点和后端一致（通道号 = items 下标 + 2）。
 */
function addChannel(source: 'display' | 'webcam', key: number | string) {
  if (form.channels.items.length >= CHANNEL_MAX_ITEMS) return
  const cam =
    source === 'webcam' ? (sources.value?.cameras ?? []).find((c) => c.name === key) : undefined
  form.channels.items.push({
    enabled: true,
    // 名字只是给人看的，后端在它为空时才补「通道 N」；这里先填一个能认出
    // 是哪块屏 / 哪台摄像头的，用户想改随时能改。
    name: source === 'display' ? `显示器 #${key}` : (cam?.name ?? String(key)),
    source,
    display_index: source === 'display' ? Number(key) : 0,
    camera: source === 'webcam' ? String(key) : '',
    // 通道 2 起默认**流畅**，与后端 `ChannelConfig::quality` 的默认一致。
    // 多路一起跑的时候每路都是一个独立编码器，默认给高了是拿 CPU 换没人看的画质。
    quality: 'smooth',
    fps: 20,
    bitrate_kbps: 1024,
    max_height: 1080,
    sub_enabled: true,
    sub_height: 360,
    sub_bitrate_kbps: 512,
    rtsp_path: freePath(false),
    rtsp_sub_path: freePath(true),
  })
}

function removeChannel(i: number) {
  form.channels.items.splice(i, 1)
}

// ---- 画面底图与拖动定位 -----------------------------------------------------

/**
 * 底图**按需抓取**，绝不自动。
 *
 * 用户明确要求"快照默认不抓拍、点了才显示"；而且这个页面上画面本身没什么好看的，
 * 自动抓只是让每次进配置页都多背一份 2880x1920 的 JPEG。
 *
 * 带 nonce 是因为**同一秒里连按两次**要不带的话浏览器会拿缓存里那张，而用户按
 * 第二次想看的恰恰是"画面变了没有"。
 *
 * **URL 必须是 `/api/snapshot.jpg`**，不能是 `/snapshot.jpg` —— 后者是给 NVR 的
 * Digest 路径，浏览器打开会弹一个系统原生的口令框（`web_server.cpp` 里那段注释
 * 就是为这件事写的）。
 */
const snapUrl = ref('')
const snapState = ref<'idle' | 'loading' | 'ok' | 'error'>('idle')
const snapError = ref('')

function takeSnapshot() {
  snapError.value = ''
  snapState.value = 'loading'
  snapUrl.value = `/api/snapshot.jpg?t=${Date.now()}`
}

const previewBox = ref<HTMLElement | null>(null)
/** 底图**渲染后**的尺寸。百分比换算用它，不用 naturalWidth（那是编码尺寸）。 */
const previewW = ref(0)
const previewH = ref(0)

function measurePreview() {
  const el = previewBox.value
  previewW.value = el?.clientWidth ?? 0
  previewH.value = el?.clientHeight ?? 0
}

function onSnapLoad() {
  measurePreview()
  snapState.value = 'ok'
}

function onSnapError() {
  snapState.value = 'error'
  snapError.value =
    '取画面失败。可能是没在采集（没有画面可抓），也可能是这次会话已失效 —— ' +
    '刷新页面重新登录一次再试。'
}

onMounted(() => window.addEventListener('resize', measurePreview))
onBeforeUnmount(() => {
  window.removeEventListener('resize', measurePreview)
  // 那个每秒自排的钟**必须**在这里停掉。留着它的话，离开这一页之后每一秒
  // 仍有一次回调在跑：`clockTick` 是组件自己的 ref，值变了没人渲染（组件已经
  // 卸了），但那一个定时器会**一直**挂在那儿 —— 在 `/` 和 `/status` 之间来回
  // 切几次就攒下几个，每个都在每秒唤醒一次主线程。它不会报错、也不会被任何
  // 断言看见，只会让整个页面慢慢变钝。这是"自排定时器"这一类写法的固定代价，
  // 所以清理和启动要挨着写。
  if (clockTimer !== undefined) {
    window.clearTimeout(clockTimer)
    clockTimer = undefined
  }
})

/** 编码器实际在推的尺寸 —— 水印的坐标就是以它为基准算的。 */
const encodedW = computed(() => stats.value.video_width)
const encodedH = computed(() => stats.value.video_height)

/** 预览相对编码尺寸的缩放。0 = 底图还没量出来。 */
const previewScale = computed(() => {
  if (previewH.value <= 0 || encodedH.value <= 0) return 0
  return previewH.value / encodedH.value
})

/**
 * 某一项在**编码帧**里的字号，逐字对应 `osd_overlay.cpp` 的 `font_px()`：
 * `font_size > 0 ? font_size : 帧高 / 32`，再和 16 取大。
 */
function encodedFontPx(it: OsdItemConfig): number {
  const h = encodedH.value
  if (h <= 0) return OSD_MIN_FONT_PX
  const raw = it.font_size > 0 ? it.font_size : Math.floor(h / OSD_AUTO_FONT_DIV)
  return Math.max(OSD_MIN_FONT_PX, raw)
}

/**
 * 标记的位置。
 *
 * **换算公式以 `osd_overlay.cpp` 的 `anchor()` 为准**，不在这里另写一套
 * （见那段代码：custom 是 `dst_w * x_pct / 100`，四角是 `max(6, px/2)` 的边距）。
 *
 * 两处是"同一件事的两种写法"而不是两份公式：
 *  · `custom` 直接给 CSS 百分比 —— 容器就是画面本身，`left: x%` 与
 *    `dst_w * x_pct / 100` 是同一个式子，再算一遍像素反而会引入取整误差；
 *  · 四角用 `right` / `bottom` —— 浏览器替我们减掉了标记自己的宽高，而 C++ 那边
 *    减的是 `mw`/`mh`（同一件事，只是那边得自己量文字宽度）。
 */
function markerStyle(it: OsdItemConfig): Record<string, string> {
  const scale = previewScale.value
  const px = Math.max(1, encodedFontPx(it) * (scale > 0 ? scale : 1))
  const style: Record<string, string> = { fontSize: `${px}px` }
  // 边距 `max(6, px/2)` 是**编码像素**，要跟着预览一起缩。
  const margin = (Math.max(6, encodedFontPx(it) / 2) * (scale > 0 ? scale : 1)).toFixed(1)

  if (it.position === 'custom') {
    style.left = `${it.x_pct}%`
    style.top = `${it.y_pct}%`
  } else if (it.position === 'upper_right') {
    style.right = `${margin}px`
    style.top = `${margin}px`
  } else if (it.position === 'lower_left') {
    style.left = `${margin}px`
    style.bottom = `${margin}px`
  } else if (it.position === 'lower_right') {
    style.right = `${margin}px`
    style.bottom = `${margin}px`
  } else {
    style.left = `${margin}px`
    style.top = `${margin}px`
  }
  return style
}

/**
 * 底图容器的宽高比 = **编码尺寸**的宽高比。
 *
 * 标记块是按容器的百分比摆的，而百分比就是编码帧上的百分比；两边比例不一致时
 * `object-fit: contain` 会加出上下黑边，于是标记落在黑边上 —— 屏幕上看着在左上，
 * 烧进画面却偏了。
 *
 * 取不到编码尺寸时（还没开始采集）**不给比例**，让容器按图片自己的比例撑开；
 * 那时也没有标记可拖（水印项的位置改不了画面），不会有人被误导。
 */
const stageStyle = computed(() => {
  if (encodedW.value <= 0 || encodedH.value <= 0) return {}
  return { aspectRatio: `${encodedW.value} / ${encodedH.value}` }
})

function clampPct(n: number): number {
  if (!Number.isFinite(n)) return 0
  return Math.min(100, Math.max(0, Math.round(n)))
}

/**
 * 拖动一个标记 → `position` 置 `custom`，并把像素位置反算成百分比。
 *
 * **拖的是文字的左上角**，与 `anchor()` 的 custom 分支指的是同一个点。
 * 百分比按**预览的宽高**算（`x_pct = round(px_x * 100 / 预览宽)`）——
 * 预览宽与编码宽是两个数，但两边都是"像素 ÷ 自己的宽度"，缩放自然抵消，
 * 所以缩过的预览上量出来的百分比就是编码帧上的百分比。
 *
 * 每次移动都写回表单（而不是拖完再写）：这样**画面上看到的位置自始至终就是
 * 即将烧进去的位置**，不存在"松手之后跳一下"。
 */
let osdDrag: { it: OsdItemConfig; dx: number; dy: number } | null = null

/**
 * 正在拖的是第几项。**只为了加一个高亮样式**（`.is-dragging`）——
 * 位置本身不从这里读，那个自始至终就是 `it.x_pct/y_pct` 本身。
 */
const osdDragIndex = ref<number | null>(null)

function onMarkerDown(it: OsdItemConfig, i: number, e: PointerEvent) {
  const box = previewBox.value
  if (!box) return
  const el = e.currentTarget as HTMLElement
  const er = el.getBoundingClientRect()
  // 记下"指针落在标记内部的偏移"。不记的话，一按下去标记的左上角就会跳到
  // 指针底下 —— 四角定位时标记离指针可能很远，那一跳尤其明显。
  osdDrag = { it, dx: e.clientX - er.left, dy: e.clientY - er.top }
  osdDragIndex.value = i
  try {
    el.setPointerCapture(e.pointerId)
  } catch {
    /* 捕获不到也能拖（只是移出标记就断），不因此拦住这一下。 */
  }
  e.preventDefault()
}

function onMarkerMove(e: PointerEvent) {
  const d = osdDrag
  const box = previewBox.value
  if (!d || !box) return
  const r = box.getBoundingClientRect()
  if (r.width <= 0 || r.height <= 0) return
  const x = e.clientX - r.left - d.dx
  const y = e.clientY - r.top - d.dy
  d.it.position = 'custom'
  d.it.x_pct = clampPct((x * 100) / r.width)
  d.it.y_pct = clampPct((y * 100) / r.height)
}

function onMarkerUp(e: PointerEvent) {
  const el = e.currentTarget as HTMLElement
  try {
    el.releasePointerCapture(e.pointerId)
  } catch {
    /* 见上：拿不到捕获也就无所谓释放。 */
  }
  osdDrag = null
  osdDragIndex.value = null
}

/**
 * 标记上显示的字。
 *
 * 时间那一项要**按格式串渲染出来**：直接显示 `%Y-%m-%d` 的话，用户量到的是
 * "格式串有多长"，而不是"时间有多长"，字号就没法判断了。
 *
 * 这里只认最常用的几个转换符，**它是一种示意** —— 真正的文字由后端按 strftime
 * 生成；认不出来的转换符原样留着（界面上看得出来，而不是悄悄变短）。
 */
function strftimeSample(dateFmt: string, timeFmt: string): string {
  const d = new Date()
  const two = (n: number) => String(n).padStart(2, '0')
  const map: Record<string, string> = {
    '%Y': String(d.getFullYear()),
    '%m': two(d.getMonth() + 1),
    '%d': two(d.getDate()),
    '%H': two(d.getHours()),
    '%M': two(d.getMinutes()),
    '%S': two(d.getSeconds()),
  }
  const sub = (s: string) => s.replace(/%[YmdHMS]/g, (m) => map[m] ?? m)
  // 与后端 `join_time()` 的拼法一致：两段之间一个空格，空的那段跳过。
  return [sub(dateFmt), sub(timeFmt)]
    .filter((s) => s.trim())
    .join(' ')
}

function sampleText(it: OsdItemConfig): string {
  if (it.type === 'name') return form.onvif.device_name || '设备名称'
  if (it.type === 'text') return it.text || '（空）'
  return strftimeSample(it.date_format, it.time_format) || '（时间格式为空）'
}
</script>

<template>
  <div class="page-head">
    <h1>配置</h1>
    <span class="hint">
      改完点「保存」写回后端。凭据（Web 登录、ONVIF 身份与口令）当场生效；
      分辨率、端口、码率、路径这些要重启程序 —— 保存后会逐项列出来。
    </span>
  </div>

  <div v-if="loading" class="banner">正在读取配置…</div>

  <template v-else-if="!loaded">
    <!-- 读不到就不给编辑：拿一份默认值去覆盖真实配置，比什么都不做糟得多。 -->
    <div class="banner is-err">
      <strong>读取配置失败。</strong>{{ loadError }}
      <br />为避免用一份凭空捏造的默认值覆盖掉真实配置，表单暂不可编辑。
    </div>
    <div class="actions">
      <button @click="load"><Icon name="refresh" :size="15" />重试</button>
    </div>
  </template>

  <!--
    `novalidate` 是**必须**的：原生约束校验和下面那个 errors 是两套独立判据，
    不一致时浏览器会**直接拦掉 submit**，`@submit.prevent` 的 handler 根本跑不到，
    于是用户看到的是"点了保存什么都没发生"。校验权只留一份，就是 errors。
    （这一条正是 1024 / 2048 保存不了的根因：`step="100"` 把它们判成
    stepMismatch，而服务器端从来是收的。）
  -->
  <form v-else ref="formEl" novalidate @submit.prevent="onSave">
    <!--
      出错横幅放在最前面，而且**优先于保存结果** —— 用户正在改的那一项出错时，
      上一条"已保存"的提示会让整页看起来是绿的，而红线的输入框在下面看不见。
    -->
    <div v-if="errorCount > 0" class="banner" :class="saveAttempted ? 'is-err' : 'is-warn'">
      <strong>有 {{ errorCount }} 项填写不合法，修好才能保存：</strong>
      <ul class="pending-list">
        <li v-for="e in errorList" :key="e.path">
          <strong>{{ e.label }}</strong> —— {{ e.msg }}
        </li>
      </ul>
      <template v-if="saveAttempted">已跳到第一个出错的输入框。</template>
      <template v-else>改好之后这里的提示会自己消失。</template>
    </div>
    <div v-else-if="saveError" class="banner is-err"><strong>保存失败。</strong>{{ saveError }}</div>
    <div v-else-if="saveOk" class="banner is-ok">{{ saveOk }}</div>

    <!--
      待重启清单。**不能因为"刚才那条保存提示已经显示过了"就省掉它** ——
      这个横幅是从 GET 拿的，所以刷新页面、过一会儿再回来，它还在。而保存提示
      是一次性的：存完刷新一下，提示没了、表单里却都是新值，看起来一切已生效。
    -->
    <div v-if="pendingFields.length" class="banner is-warn">
      <strong>有 {{ pendingFields.length }} 项已保存，但需要重启程序才会生效：</strong>
      <ul class="pending-list">
        <li v-for="p in pendingFields" :key="p">{{ label(p) }}</li>
      </ul>
      分辨率、端口、码率、路径这些要重建编码器、重新 bind 或重建 RTSP 会话，
      做一半的失败模式是「界面上显示改了、实际还在用旧的」—— 那比要重启一次糟得多。
      凭据（Web 登录、ONVIF 身份与口令）则是**当场生效**的，不需要重启。
    </div>

    <div v-if="appliedFields.length" class="banner is-ok">
      本次保存中已当场生效：{{ appliedFields.map(label).join('、') }}
    </div>

    <!--
      段落锚点。id 直接挂在 `<fieldset>` 上，不再套一层 `<section>` ——
      它本来就是个分组元素，多套一层只会多一层缩进。
      这几个 id 是**侧栏那种"一页 + 锚点"导航的落点**，改名字要同时改
      `App.vue` 的 MENU_GROUPS（那是菜单表的唯一定义处）。
    -->
    <fieldset id="video">
      <legend>采集与编码</legend>

      <!--
        画质档位放在这一组的最前面：**它是下面那几项的来源**，不是一个并列
        的选项。选中就把帧率/码率/分辨率上限/子码流一起改掉（后端也是这么做的，
        见 validate_config 里的展开）；手改其中任何一项，这里立刻显示「自定义」。
      -->
      <div class="grid">
        <label class="field">
          <span class="field-label">画质档位</span>
          <select :value="effectiveQuality()" @change="onTierChange">
            <option v-for="t in tiers" :key="t.key" :value="t.key">{{ t.label }}</option>
            <option value="custom">自定义</option>
          </select>
          <span v-if="currentTierDetail" class="hint">{{ currentTierDetail }}</span>
          <span v-else class="hint">
            下面是手填的具体值。改动了帧率 / 码率 / 分辨率上限 / 子码流开关中的任何一项，
            就会回到这里。
          </span>
        </label>

        <label class="field">
          <span class="field-label">分辨率上限（0 = 桌面原生）</span>
          <input
            v-model.number="form.capture.max_height"
            type="number"
            min="0"
            max="4320"
            step="1"
            :class="{ invalid: errors['capture.max_height'] }"
          />
          <span class="hint">只写上限：桌面比它小就不放大。宽度按桌面宽高比推，并取偶数</span>
          <span v-if="errors['capture.max_height']" class="field-error">{{
            errors['capture.max_height']
          }}</span>
        </label>
      </div>

      <!--
        延时档位。它和上面那个画质档位**正交** —— 两档展开的字段集合不相交
        （画质档管 帧率 / 码率 / 分辨率上限 / 子码流，延时档只管 编码线程数与
        关键帧间隔），所以「流畅 + 极致低延迟」这种组合天然合法，不需要任何
        优先级规则。

        用卡片组而不是第二个下拉框：这一档的**代价**必须和它的名字同时看得见。
        下拉框里塞不下三行话，只能显示当前选中那一项 —— 而「选下去之前先知道
        代价」恰好是这个功能唯一诚实的地方。

        数字全部现算自后端那张表，人话按 key 写死。表里**没有 custom**，所以
        第四张卡是界面自己补的（见 script 里 `latencyCards` 的注释）。
      -->
      <div v-if="latencyCards.length" class="latency-tiers">
        <label
          v-for="c in latencyCards"
          :key="c.key"
          class="latency-tier"
          :class="{ on: effectiveLatency() === c.key }"
        >
          <input
            type="radio"
            name="capture.latency"
            :checked="effectiveLatency() === c.key"
            @change="onLatencyChange(c.key)"
          />
          <span class="lt-name">{{ c.label }}<code class="lt-key">{{ c.key }}</code></span>
          <span class="lt-num">{{ c.note.numbers }}</span>
          <span v-if="c.note.gain" class="lt-line"><b>换来</b>{{ c.note.gain }}</span>
          <span v-if="c.note.cost" class="lt-line lt-cost"><b>代价</b>{{ c.note.cost }}</span>
        </label>

        <label class="latency-tier" :class="{ on: effectiveLatency() === 'custom' }">
          <input
            type="radio"
            name="capture.latency"
            :checked="effectiveLatency() === 'custom'"
            @change="onLatencyChange('custom')"
          />
          <span class="lt-name">自定义<code class="lt-key">custom</code></span>
          <span class="lt-num">{{ customLatencyNote.numbers }}</span>
          <span class="lt-line"><b>换来</b>线程数与关键帧间隔完全由下面两个框决定。</span>
          <span class="lt-line lt-cost"><b>代价</b>{{ customLatencyNote.landed }}</span>
        </label>
      </div>

      <!--
        够不到的那几项。档位表上写"能到多少"而这里写"地板在哪"，两条缺一条，
        用户就会拿一个够不到的数来对账 —— 那时他判定的不是"这个数字没变"，
        而是"这个功能没做"。
      -->
      <div v-if="latencyCards.length" class="hint latency-floor">
        <p>
          换了档位也<strong>不会</strong>变的部分（它们不在档位表上，写在这里免得你去
          对账一个够不到的数）：
        </p>
        <ul>
          <li>
            音频一开就有约 <strong>320 ms</strong> 的地板 —— 环回环形缓冲 300 ms
            加 AAC 启动填充 21 ms。
          </li>
          <li>NVR 侧的抖动缓冲我们看不见，也不在档位表上。</li>
        </ul>
        <p>
          另外三条：选档位会把「编码线程数」<strong>钉成表里的值</strong>，而 <code>0</code>
          （= 按延迟优先自动选、封顶 6）只属于「自定义」—— 所以从旧配置升上来的设备
          选了「均衡」之后线程数会从自动值变成 3，那是有意的，不是被改坏了。手改下面
          任一项（编码线程数 / GOP 间隔秒）就会回到「自定义」。这一段
          <strong>要重启才生效</strong>（线程数与关键帧间隔都是建编码器那一刻定的），
          所以段底给了一个「保存并立即应用」。
        </p>

        <!--
          网页预览那一半。**必须和上面分开写**：档位对"多久能看到第一幅画"和
          "看到之后有多滞后"的作用是两个不同的旋钮，合成一句的话，用户会把
          "等了 2 秒才出画"归到分片节奏上，去调一个不相干的档位。
        -->
        <p>预览页上那个「真实延迟」是两段拼起来的，档位对这两段的作用不一样：</p>
        <ul>
          <li>
            <strong>分片滞后（看到之后有多滞后）</strong>：后端<strong>不再等关键帧</strong>才切分片，
            而是按固定节奏（约每 0.25 秒）主动切一刀，所以这一项已经和关键帧间隔脱钩了。
            剩下的那一段是预览页自己的「播放目标」（默认 0.5 秒）—— 它跟着档位走，
            也可以在预览页上按本机情况单独覆盖。
            ⚠️ <strong>浏览器兼容性只验过 Chrome</strong>：不以关键帧开头的分片，
            Chrome 收得下、也解得出画面（本机实测过）；<strong>Firefox 在这台机器上装不了，未验</strong>。
            万一日后碰到某个浏览器被切碎之后不出画，退路是把后端那个主动切分片的开关关掉，
            回到按关键帧分片 —— 代价是这一项回到平均半个 GOP。
          </li>
          <li>
            <strong>首帧等待（中途连上预览，多久能看到第一幅画）</strong>：分片约每 0.25 秒到一个，
            但第一幅<strong>能显示</strong>的画面仍要等下一个关键帧，最坏就是当前的关键帧间隔
            （<code>{{ form.capture.gop_seconds }}</code> 秒）。这一项由<strong>关键帧间隔</strong>决定，
            和上面那条不是同一件事 —— 缩短它，这一项跟着小。
          </li>
        </ul>
      </div>

      <div class="grid" style="margin-top: 12px">
        <label class="field">
          <span class="field-label">显示器序号（--list-displays 的编号）</span>
          <input
            v-model.number="form.capture.display_index"
            type="number"
            min="0"
            step="1"
            :class="{ invalid: errors['capture.display_index'] }"
          />
          <span v-if="errors['capture.display_index']" class="field-error">{{
            errors['capture.display_index']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">帧率 fps（1–120）</span>
          <input
            v-model.number="form.capture.fps"
            type="number"
            min="1"
            max="120"
            step="1"
            :class="{ invalid: errors['capture.fps'] }"
          />
          <span v-if="errors['capture.fps']" class="field-error">{{ errors['capture.fps'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">码率 kbps（100–50000）</span>
          <input
            v-model.number="form.capture.bitrate_kbps"
            type="number"
            min="100"
            max="50000"
            step="1"
            :class="{ invalid: errors['capture.bitrate_kbps'] }"
          />
          <span v-if="errors['capture.bitrate_kbps']" class="field-error">{{
            errors['capture.bitrate_kbps']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">GOP 间隔 秒（1–10）</span>
          <input
            v-model.number="form.capture.gop_seconds"
            type="number"
            min="1"
            max="10"
            step="1"
            :class="{ invalid: errors['capture.gop_seconds'] }"
          />
          <span class="hint">它也是浏览器预览的延迟上界：分片只在关键帧处产生</span>
          <span v-if="errors['capture.gop_seconds']" class="field-error">{{
            errors['capture.gop_seconds']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">编码线程数</span>
          <input
            v-model.number="form.capture.threads"
            type="number"
            min="0"
            max="128"
            step="1"
            :class="{ invalid: errors['capture.threads'] }"
          />
          <span class="hint">0 = 按「延迟优先」自动选（封顶 6）</span>
          <span v-if="errors['capture.threads']" class="field-error">{{
            errors['capture.threads']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">预设 preset</span>
          <select v-model="form.capture.preset">
            <option>ultrafast</option>
            <option>superfast</option>
            <option>veryfast</option>
            <option>faster</option>
            <option>fast</option>
            <option>medium</option>
            <option>slow</option>
            <option>slower</option>
            <option>veryslow</option>
          </select>
        </label>

        <label class="field">
          <span class="field-label">调优 tune</span>
          <select v-model="form.capture.tune">
            <option>zerolatency</option>
            <option>film</option>
            <option>animation</option>
            <option>grain</option>
            <option>stillimage</option>
            <option>fastdecode</option>
          </select>
        </label>

        <label class="field">
          <span class="field-label">档次 profile</span>
          <select v-model="form.capture.profile">
            <option>baseline</option>
            <option>main</option>
            <option>high</option>
          </select>
          <span class="hint">ultrafast 会压到 Constrained Baseline，与选哪个无关</span>
        </label>
      </div>

      <h4 class="sub-head">让改动生效</h4>

      <p class="hint section-note">
        这一段里帧率 / 码率 / 分辨率上限 / 子码流 / <strong>延时档位</strong>都要
        <strong>重开管线</strong>才生效（编码参数与关键帧间隔都是建编码器那一刻定死的，
        与码率同理，见 README 的「为什么改码率要重启」）。下面的按钮 = 保存 + 重开管线，
        代价是<strong>录像分段</strong>（新文件从 main.1.mp4 续）加<strong>已连上的
        RTSP / NVR 断开重连</strong>。
      </p>

      <div class="actions">
        <button
          class="btn primary"
          type="button"
          :disabled="videoApplying || saving"
          @click="onVideoApply"
        >
          <Icon name="zap" :size="15" />{{ videoApplying ? '正在应用…' : '保存并立即应用' }}
        </button>
        <span class="hint">只想存下来、下次重启再生效的话，用页面底部那条「保存」</span>
      </div>

      <div
        v-if="videoApplyMsg"
        class="banner"
        :class="{
          'is-ok': videoApplyTone === 'ok',
          'is-warn': videoApplyTone === 'warn',
          'is-err': videoApplyTone === 'err',
        }"
        style="margin: 12px 0 0"
      >
        {{ videoApplyMsg }}
      </div>
    </fieldset>

    <!-- 通道 2 起。**这一段不是"视频设置的第二份"** —— 通道 1 永远由上面
         那一段管（`capture.*` / `rtsp.*`），这里只管"再开几路"。
         每一路走一个**自己的** ONVIF 端口（web 端口 + 通道号）和一条自己的
         RTSP 路径；RTSP 服务本身是共用的 554。 -->
    <fieldset id="channels">
      <legend>通道（多路输出）</legend>

      <p class="hint section-note">
        通道 1 就是上面「视频」那一段，不在这个列表里。<strong>每多一块显示器，这里会自动多一路</strong>
        —— 默认开着，NVR 上一扫就是好几路。摄像头要自己勾（见下面的"检测到的源"）。
        每一路都有自己的 ONVIF 端口（网页端口 + 通道号）和自己的录像文件。
      </p>

      <div v-if="sourcesError" class="banner is-warn">
        读不到本机有哪些显示器和摄像头（{{ sourcesError }}）。已经配好的通道照样能改，
        只是没法在这里新加 —— 刷新整页再试一次。
      </div>

      <h4 class="sub-head">检测到的源</h4>

      <template v-if="sources">
        <p class="hint">
          这一份是<strong>启动时</strong>算的快照，不再刷新：后端要试"这块屏能不能捕获"就
          必须真的建一次捕获对象，而一块屏同时只允许一个 —— 刷新会把你正在看的预览弄断。
        </p>

        <div
          v-for="d in sources.displays"
          :key="`d${d.index}`"
          class="source-row"
          :class="{ 'is-taken': displayOwner(d.index) >= 0 }"
        >
          <span class="source-name">{{ d.name }}</span>
          <span class="source-meta">{{ d.width }}×{{ d.height }} @ {{ d.x }},{{ d.y }}</span>
          <span class="spacer"></span>
          <span v-if="displayOwner(d.index) >= 0" class="hint">
            已作为通道 {{ displayOwner(d.index) + 2 }}
          </span>
          <span v-else-if="!d.capturable" class="hint">这块屏捕获不了（虚拟显示器 / 已断开）</span>
          <button
            v-else
            class="btn tiny"
            type="button"
            :disabled="form.channels.items.length >= CHANNEL_MAX_ITEMS"
            @click="addChannel('display', d.index)"
          >
            <Icon name="plus" :size="14" />加为通道
          </button>
        </div>

        <div
          v-for="c in sources.cameras"
          :key="`c${c.index}`"
          class="source-row"
          :class="{ 'is-taken': cameraOwner(c.name) >= 0 }"
        >
          <Icon name="camera" :size="15" />
          <span class="source-name">{{ c.name }}</span>
          <span class="source-meta">
            本机摄像头{{ c.is_default ? '（系统默认）' : '' }}
          </span>
          <span class="spacer"></span>
          <span v-if="cameraOwner(c.name) >= 0" class="hint">
            已作为通道 {{ cameraOwner(c.name) + 2 }}
          </span>
          <!-- 同一台摄像头**可以**开两路（不同画质 / 不同路径），所以这里不禁用。 -->
          <button
            v-else
            class="btn tiny"
            type="button"
            :disabled="form.channels.items.length >= CHANNEL_MAX_ITEMS"
            @click="addChannel('webcam', c.name)"
          >
            <Icon name="plus" :size="14" />加为通道
          </button>
        </div>

        <p v-if="!sources.displays.length && !sources.cameras.length" class="hint">
          一个都没检测到。
        </p>
        <p v-else-if="!sources.cameras.length" class="hint">
          没检测到本机摄像头{{ sources.camera_error ? `（${sources.camera_error}）` : '' }}。
        </p>
      </template>

      <h4 class="sub-head">通道 2 起（最多 {{ CHANNEL_MAX_ITEMS }} 路）</h4>

      <p v-if="errors['channels.items']" class="field-error">{{ errors['channels.items'] }}</p>

      <div v-if="!form.channels.items.length" class="hint">
        还没有额外的通道。上面那些显示器若没被勾选，会由<strong>自动发现</strong>接管 ——
        后端起不来的时候"一台机器几块屏就是几路"仍然成立。
      </div>

      <div v-for="(it, i) in form.channels.items" :key="i" class="channel-item">
        <div class="channel-item-head">
          <span class="channel-item-index">通道 {{ i + 2 }}</span>

          <label class="field check compact">
            <input v-model="it.enabled" type="checkbox" />
            <span class="track"></span>
            <span class="field-label">{{ it.enabled ? '启用' : '已关闭' }}</span>
          </label>

          <span class="spacer"></span>
          <button
            class="btn tiny danger"
            type="button"
            title="从列表里删掉这一路，保存后不可恢复"
            @click="removeChannel(i)"
          >
            <Icon name="x" :size="14" />移除
          </button>
        </div>

        <div class="grid">
          <label class="field">
            <span class="field-label">名字</span>
            <input v-model="it.name" type="text" placeholder="留空则用「通道 N」" />
            <span class="hint">只在网页和 ONVIF 里显示，NVR 那边用的是它自己存的名字</span>
          </label>

          <label class="field">
            <span class="field-label">源</span>
            <select v-model="it.source">
              <option value="display">显示器</option>
              <option value="webcam">本机摄像头</option>
            </select>
          </label>

          <label v-if="it.source === 'display'" class="field">
            <span class="field-label">显示器序号</span>
            <input
              v-model.number="it.display_index"
              type="number"
              min="0"
              max="64"
              :class="{ invalid: errors[`channels.items.${i}.display_index`] }"
            />
            <span v-if="errors[`channels.items.${i}.display_index`]" class="field-error">{{
              errors[`channels.items.${i}.display_index`]
            }}</span>
          </label>

          <label v-else class="field">
            <span class="field-label">摄像头</span>
            <select
              v-model="it.camera"
              :class="{ invalid: errors[`channels.items.${i}.camera`] }"
            >
              <option value="">（未选）</option>
              <option v-for="c in sources?.cameras ?? []" :key="c.index" :value="c.name">
                {{ c.name }}
              </option>
              <!-- 配置里存着的那个名字可能已经拔了 / 改了名：留着它当一个选项，
                   否则下拉框会落到一个不存在的值上，显示成空白，而用户会以为
                   配置坏了。 -->
              <option
                v-if="it.camera && !(sources?.cameras ?? []).some((c) => c.name === it.camera)"
                :value="it.camera"
              >
                {{ it.camera }}（当前不在线）
              </option>
            </select>
            <span v-if="errors[`channels.items.${i}.camera`]" class="field-error">{{
              errors[`channels.items.${i}.camera`]
            }}</span>
            <span v-else class="hint">拔了 / 改了名的摄像头会建不出这一路</span>
          </label>

          <label class="field">
            <span class="field-label">画质档位</span>
            <select v-model="it.quality">
              <option v-for="(text, key) in CHANNEL_QUALITY_LABELS" :key="key" :value="key">
                {{ text }}
              </option>
            </select>
            <span class="hint">选档位会覆盖下面四项；手改过就自动变成「自定义」</span>
          </label>

          <label class="field">
            <span class="field-label">帧率</span>
            <input
              v-model.number="it.fps"
              type="number"
              min="1"
              max="120"
              :class="{ invalid: errors[`channels.items.${i}.fps`] }"
            />
            <span v-if="errors[`channels.items.${i}.fps`]" class="field-error">{{
              errors[`channels.items.${i}.fps`]
            }}</span>
          </label>

          <label class="field">
            <span class="field-label">码率（kbps）</span>
            <input
              v-model.number="it.bitrate_kbps"
              type="number"
              min="100"
              max="100000"
              :class="{ invalid: errors[`channels.items.${i}.bitrate_kbps`] }"
            />
            <span v-if="errors[`channels.items.${i}.bitrate_kbps`]" class="field-error">{{
              errors[`channels.items.${i}.bitrate_kbps`]
            }}</span>
          </label>

          <label class="field">
            <span class="field-label">分辨率上限</span>
            <input
              v-model.number="it.max_height"
              type="number"
              min="0"
              max="4320"
              :class="{ invalid: errors[`channels.items.${i}.max_height`] }"
            />
            <span v-if="errors[`channels.items.${i}.max_height`]" class="field-error">{{
              errors[`channels.items.${i}.max_height`]
            }}</span>
            <span v-else class="hint">0 = 不缩放，按源的原生分辨率</span>
          </label>

          <label class="field check">
            <input v-model="it.sub_enabled" type="checkbox" />
            <span class="track"></span>
            <span class="field-label">开子码流</span>
          </label>

          <label class="field">
            <span class="field-label">子码流高度</span>
            <input
              v-model.number="it.sub_height"
              type="number"
              min="120"
              max="2160"
              :class="{ invalid: errors[`channels.items.${i}.sub_height`] }"
            />
            <span v-if="errors[`channels.items.${i}.sub_height`]" class="field-error">{{
              errors[`channels.items.${i}.sub_height`]
            }}</span>
          </label>

          <label class="field">
            <span class="field-label">子码流码率（kbps）</span>
            <input
              v-model.number="it.sub_bitrate_kbps"
              type="number"
              min="32"
              max="100000"
              :class="{ invalid: errors[`channels.items.${i}.sub_bitrate_kbps`] }"
            />
            <span v-if="errors[`channels.items.${i}.sub_bitrate_kbps`]" class="field-error">{{
              errors[`channels.items.${i}.sub_bitrate_kbps`]
            }}</span>
          </label>

          <label class="field">
            <span class="field-label">主码流路径</span>
            <input
              v-model="it.rtsp_path"
              type="text"
              :placeholder="`/ch${i + 2}`"
              :class="{ invalid: errors[`channels.items.${i}.rtsp_path`] }"
            />
            <span v-if="errors[`channels.items.${i}.rtsp_path`]" class="field-error">{{
              errors[`channels.items.${i}.rtsp_path`]
            }}</span>
            <span v-else class="hint">留空则用「通道 N」那个默认路径</span>
          </label>

          <label class="field">
            <span class="field-label">子码流路径</span>
            <input
              v-model="it.rtsp_sub_path"
              type="text"
              :placeholder="`/ch${i + 2}_sub`"
              :class="{ invalid: errors[`channels.items.${i}.rtsp_sub_path`] }"
            />
            <span v-if="errors[`channels.items.${i}.rtsp_sub_path`]" class="field-error">{{
              errors[`channels.items.${i}.rtsp_sub_path`]
            }}</span>
          </label>
        </div>

        <p class="hint">
          RTSP 地址：<code>rtsp://&lt;本机地址&gt;:{{ form.rtsp.port }}{{ it.rtsp_path || `/ch${i + 2}` }}</code>
          —— RTSP 服务是和通道 1 <strong>共用</strong>的，靠路径区分。
        </p>
      </div>

      <p class="hint section-note">
        每一路都是<strong>独立的编码器</strong>：加一路就多一份编码开销，也各录一份 MP4。
        通道那一段的增删改都要<strong>重启服务</strong>才生效 —— 保存之后下面这些按钮里
        「重启服务」那一条就是干这个的。
      </p>
    </fieldset>

    <!-- 水印（OSD）。这段是**热生效**的：保存之后画面下一帧就变，不用重启、
         也不分段（第三轮那段"要重建整条管线"的注释已经作废，见 Phase S）。 -->
    <fieldset id="osd">
      <legend>水印（OSD）</legend>

      <p class="hint section-note">
        水印是**烧进画面**的，不是叠加在网页上 —— 主码流、子码流、录像文件、快照、NVR 看到的
        都是同一份。这一段改完点保存就生效，画面下一帧就变，不用重启。
      </p>

      <div class="grid">
        <label class="field check">
          <input v-model="form.osd.enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">开启水印</span>
        </label>

        <label class="field check">
          <input v-model="form.osd.adaptive_color" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">自适应配色</span>
        </label>
        <span class="hint span-note">按水印那一小块的平均亮度选黑字或白字</span>

        <label class="field check">
          <input v-model="form.osd.outline" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">描边</span>
        </label>
        <span class="hint span-note">给文字加一圈反色边，任何背景上都读得出来</span>

        <label class="field">
          <span class="field-label">底板不透明度（0–255）</span>
          <input
            v-model.number="form.osd.plate_alpha"
            type="number"
            min="0"
            max="255"
            step="1"
            :class="{ invalid: errors['osd.plate_alpha'] }"
          />
          <span class="hint">0 = 不画底板（默认）；数值越大底色越实</span>
          <span v-if="errors['osd.plate_alpha']" class="field-error">{{
            errors['osd.plate_alpha']
          }}</span>
        </label>
      </div>

      <h4 class="sub-head">水印项（最多 {{ OSD_MAX_ITEMS }} 项）</h4>

      <p v-if="errors['osd.items']" class="field-error">{{ errors['osd.items'] }}</p>

      <div v-if="!form.osd.items.length" class="banner is-warn">
        一项都没有 —— 水印开关开着也不会画任何东西。
      </div>

      <div v-for="(it, i) in form.osd.items" :key="i" class="osd-item">
        <div class="osd-item-head">
          <span class="osd-item-index">第 {{ i + 1 }} 项</span>

          <label class="field check compact">
            <input v-model="it.enabled" type="checkbox" />
            <span class="track"></span>
            <span class="field-label">{{ it.enabled ? '显示中' : '已关闭' }}</span>
          </label>

          <!-- 「关闭」= enabled=false（项还在，以后能再打开）；「移除」= 从列表里
               删掉（要重新填一遍）。两件事都得有，所以两个按钮的图标、颜色和
               文案都不一样 —— 只靠位置区分的话，点错的那个正好是不可逆的。 -->
          <span class="spacer"></span>
          <button
            class="btn tiny"
            type="button"
            :title="it.enabled ? '暂时不画这一项，配置里还留着' : '恢复显示这一项'"
            @click="it.enabled = !it.enabled"
          >
            <Icon :name="it.enabled ? 'eye' : 'eye-off'" :size="14" />
            {{ it.enabled ? '关闭' : '打开' }}
          </button>
          <button
            class="btn tiny danger"
            type="button"
            title="从列表里删掉这一项，保存后不可恢复"
            @click="removeOsdItem(i)"
          >
            <Icon name="x" :size="14" />移除
          </button>
        </div>

        <div class="grid">
          <label class="field">
            <span class="field-label">类型</span>
            <select v-model="it.type">
              <option v-for="(text, key) in OSD_TYPE_LABELS" :key="key" :value="key">
                {{ text }}
              </option>
            </select>
          </label>

          <label v-if="it.type === 'text'" class="field">
            <span class="field-label">显示的文字</span>
            <input
              v-model="it.text"
              type="text"
              placeholder="例如：北门"
              :class="{ invalid: errors[`osd.items.${i}.text`] }"
            />
            <span v-if="errors[`osd.items.${i}.text`]" class="field-error">{{
              errors[`osd.items.${i}.text`]
            }}</span>
          </label>

          <!-- type=name 时不给输入框：内容跟着 `设备名` 走，这里再填一个是第二份
               真相，而它会和 NVR 改的那个名字打架。 -->
          <label v-else-if="it.type === 'name'" class="field">
            <span class="field-label">显示的文字</span>
            <input
              :value="form.onvif.device_name || '（设备名为空）'"
              type="text"
              readonly
              title="跟随「ONVIF 设置」里的设备名，改那里才改得动"
            />
            <span class="hint">跟随设备名（NVR 改名也会跟着变）</span>
          </label>
          <div v-else class="field">
            <span class="field-label">显示的文字</span>
            <span class="hint">走时，内容按下面两个格式串实时生成</span>
          </div>

          <label class="field">
            <span class="field-label">位置</span>
            <select v-model="it.position">
              <option v-for="(text, key) in OSD_POSITION_LABELS" :key="key" :value="key">
                {{ text }}
              </option>
            </select>
          </label>

          <template v-if="it.position === 'custom'">
            <label class="field">
              <span class="field-label">横向 %（0–100）</span>
              <input
                v-model.number="it.x_pct"
                type="number"
                min="0"
                max="100"
                step="1"
                :class="{ invalid: errors[`osd.items.${i}.x_pct`] }"
              />
            </label>
            <label class="field">
              <span class="field-label">纵向 %（0–100）</span>
              <input
                v-model.number="it.y_pct"
                type="number"
                min="0"
                max="100"
                step="1"
                :class="{ invalid: errors[`osd.items.${i}.y_pct`] }"
              />
            </label>
          </template>

          <label class="field">
            <span class="field-label">字号 px（0 = 自动）</span>
            <input
              v-model.number="it.font_size"
              type="number"
              min="0"
              max="512"
              step="1"
              :class="{ invalid: errors[`osd.items.${i}.font_size`] }"
            />
            <span class="hint">以编码尺寸算；自动 = 帧高 ÷ 32，下限 16</span>
            <span v-if="errors[`osd.items.${i}.font_size`]" class="field-error">{{
              errors[`osd.items.${i}.font_size`]
            }}</span>
          </label>

          <template v-if="it.type === 'time'">
            <label class="field">
              <span class="field-label">日期格式</span>
              <input v-model="it.date_format" type="text" placeholder="%Y-%m-%d" />
            </label>
            <label class="field">
              <span class="field-label">时间格式</span>
              <input v-model="it.time_format" type="text" placeholder="%H:%M:%S" />
            </label>
            <span class="hint span-note">
              标准 strftime：%Y 年 %m 月 %d 日 %H 时 %M 分 %S 秒。
              两段之间自动加一个空格；留空的那一段不显示。
            </span>
          </template>
        </div>
      </div>

      <div class="actions">
        <button
          class="btn"
          type="button"
          :disabled="form.osd.items.length >= OSD_MAX_ITEMS"
          :title="
            form.osd.items.length >= OSD_MAX_ITEMS
              ? `最多 ${OSD_MAX_ITEMS} 项，先移除一项再加`
              : '加一项固定文字水印'
          "
          @click="addOsdItem"
        >
          <Icon name="plus" :size="15" />加一项文字水印
        </button>
        <span class="hint">
          已用 {{ form.osd.items.length }} / {{ OSD_MAX_ITEMS }} 项（这是后端的上限）
        </span>
      </div>

      <h4 class="sub-head">拖动定位</h4>

      <p class="hint section-note">
        下面这块底图是**点一下才抓**的当前画面，不是自动刷新的 —— 抓一次就定格，
        拖上面的标记块来摆位置。拖过的项会自动变成「自定义」，百分比就是它左上角
        在画面上的位置。
      </p>

      <div class="actions">
        <button class="btn" type="button" :disabled="snapState === 'loading'" @click="takeSnapshot">
          <Icon name="refresh" :size="15" />
          {{ snapState === 'loading' ? '抓取中…' : snapState === 'ok' ? '重新取当前画面' : '取当前画面' }}
        </button>
        <span v-if="snapState === 'ok'" class="hint">
          {{ encodedW }}×{{ encodedH }} 的画面，显示成 {{ Math.round(previewW) }}×{{
            Math.round(previewH)
          }}（标记的大小和位置已经按这个比例缩过了）
        </span>
      </div>

      <p v-if="snapState === 'error'" class="banner is-err">{{ snapError }}</p>

      <!-- ⚠️ 这里的条件**不能**写成 `snapState !== 'ok'`，那是个死锁：
           `snapState` 变成 `'ok'` 的**唯一**来路就是这个 `<img>` 的 `@load`，
           而写成 `v-if="snapState !== 'ok'"` 之后这张 `<img>` 在 `loading` 期间
           根本不在 DOM 里 ⇒ 它永远不会 load ⇒ 状态永远停在 `loading`，
           底图、标记块全都不出现（按钮还会一直显示「抓取中…」并保持禁用）。
           判据只能是"有没有 URL / 是不是失败了" —— 有 URL 就把它渲染出来，
           剩下的事交给 `@load` / `@error`。
           （`PreviewView.vue` 那边用的是 `snapshotSrc !== '' && state !== 'fail'`，
           同一个道理。这个 bug 是 `config_probe.mjs` 第 [5] 段等 `.osd-stage img`
           等到超时才暴露出来的 —— 单看代码它很像"没抓拍就不显示"的正常写法。） -->
      <div v-if="!snapUrl || snapState === 'error'" class="stage osd-stage">
        <div class="stage-placeholder">
          <div class="ph-title">还没有画面</div>
          <div class="ph-text">点上面的「取当前画面」抓一张，然后在上面拖水印的位置。</div>
        </div>
      </div>

      <div v-else ref="previewBox" class="stage osd-stage" :style="stageStyle">
        <img :src="snapUrl" alt="当前画面" @load="onSnapLoad" @error="onSnapError" />

        <div
          v-for="(it, i) in form.osd.items"
          :key="`m${i}`"
          class="osd-marker"
          :class="{ 'is-off': !it.enabled, 'is-dragging': osdDragIndex === i }"
          :style="markerStyle(it)"
          :title="`第 ${i + 1} 项：${sampleText(it)}（拖动改位置）`"
          @pointerdown="onMarkerDown(it, i, $event)"
          @pointermove="onMarkerMove"
          @pointerup="onMarkerUp"
          @pointercancel="onMarkerUp"
        >
          {{ sampleText(it) }}
        </div>
      </div>

      <p v-if="snapState === 'ok' && !form.osd.items.length" class="hint">
        上面没有标记块可拖 —— 水印项那一列表是空的。
      </p>
      <p v-else-if="snapState === 'ok'" class="hint">
        标记块的位置就是**烧进画面后文字左上角**的位置。灰色的那几项是已关闭的，
        拖它们也可以（位置会记住，重新打开就是那个位置）。
      </p>
    </fieldset>

    <!-- 音视频（音频）。这一段与「图像设置」是分开的两段：视频那几项（画面）
         在 `#video`，这里只管声音。

         它和前两段的**生效方式不一样**：水印是热生效的（保存即变），音频是
         **冷**的 —— 开关它、换设备、改增益都要重开采集设备，而那是建管线那一刻
         定下来的。所以段底单独给一个「保存并立即应用」。 -->
    <fieldset id="audio">
      <legend>音视频</legend>

      <p class="hint section-note">
        画面那几项在「图像设置」里。这一段只管**声音**：录像是「纯视频」还是「视频 + 音频」，
        以及声音从哪儿来。<strong>音频默认是关的</strong> —— 打开它会改变每一台设备的 SDP，
        而真实 NVR 收不收只能实测。
      </p>

      <div class="grid">
        <label class="field check">
          <input v-model="form.audio.enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">录制音频（建一条音轨）</span>
        </label>
        <span class="hint span-note">
          关掉时**连音轨都不建**："有音轨但永远静音"和"没有音轨"对 NVR 是两回事，
          后者不会在通道上显示一个音频图标
        </span>
      </div>

      <!-- 陷阱 79 的那句话必须在**打开的那一刻**就在眼前，而不是躺在 README 里。
           打开的后果是"这台设备在 NVR 那边变了个样"，而异常现场的表现是
           "加了音频就没画面" —— 归因会稳稳地落到编码器上，而那条流一直是好的。 -->
      <div v-if="form.audio.enabled" class="banner is-warn">
        打开音频之后，这一路 RTSP/NVR 的协商结果与之前<strong>不再是同一个东西</strong>。
        若 NVR 取流异常（比如加了音频就没画面、或者通道报错），<strong>先关掉音频试试</strong>
        —— 这条流本身一直是好的。
      </div>

      <h4 class="sub-head">音频来源（两个可以同时勾）</h4>

      <p v-if="errors['audio.system']" class="field-error">{{ errors['audio.system'] }}</p>

      <div class="grid">
        <label class="field check">
          <input v-model="form.audio.system" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">系统声音（环回）</span>
        </label>
        <span class="hint span-note">
          录这台机器**正在播放**的声音。系统不出声时这一路可能收不到数据（这是正常的，
          不是故障），音轨的时间轴由补零撑住，所以不会因为"没人出声"而变短。
          具体的实时情况看「实时状态」页上的电平表
        </span>

        <label class="field check">
          <input v-model="form.audio.microphone" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">麦克风</span>
        </label>
        <span class="hint span-note">从采集端点录，设备在下面选</span>
      </div>

      <div class="grid" style="margin-top: 12px">
        <label class="field">
          <span class="field-label">麦克风设备</span>
          <select v-model="form.audio.device" :class="{ invalid: !!audioDevicesError }">
            <option value="">默认设备（跟随系统）</option>
            <option v-for="d in audioDevices" :key="d.id" :value="d.id">
              {{ d.name }}{{ d.default ? '（系统默认）' : '' }}
            </option>
            <!-- 存着的那台设备现在枚举不到（拔了/换了）。把它留在列表里，
                 否则 `<select>` 会自己跳到第一项 —— 一次静默改写：
                 用户只想改个增益，保存下去连"用哪只麦克风"一起被改掉了。 -->
            <option v-if="missingAudioDevice" :value="form.audio.device">
              {{ form.audio.device }}（当前枚举不到，可能已拔出）
            </option>
          </select>
          <span class="hint">
            空 = 系统默认端点（跟着系统走）。只有勾了「麦克风」才用得上
          </span>
          <span v-if="audioDevicesError" class="field-error">
            读不到设备列表：{{ audioDevicesError }}
          </span>
        </label>
      </div>

      <div class="actions" style="margin-top: 10px">
        <button class="btn" type="button" :disabled="audioDevicesLoading" @click="loadAudioDevices">
          <Icon name="refresh" :size="14" />{{ audioDevicesLoading ? '读取中…' : '重新读取设备列表' }}
        </button>
        <span class="hint">
          插拔耳机之后列表就变了，所以每次进这一页都现读一次；设备确实不在列表里时按这个
        </span>
      </div>

      <div class="grid" style="margin-top: 12px">
        <label class="field">
          <!-- 标签写法跟「子码流码率（kbps）」一致：单位在括号里、跟在词后面。
               这不只是好看 —— 探针（`tests/browser/config_probe.mjs`）是按
               **标签文字的包含关系**找输入框的，写成「音频码率 kbps」的话，
               它那条 `hasText: '码率 kbps'` 会同时命中「码率 kbps（100–50000）」
               和这一格，Playwright 的 strict 模式当场报"解析出 2 个元素"。
               探针宁可报这个错也不肯"随便挑一个"，这正是我们要的：它把
               "两格标签长得太像"这件事直接摆出来了。 -->
          <span class="field-label">音频码率（kbps）</span>
          <input
            v-model.number="form.audio.bitrate_kbps"
            type="number"
            min="8"
            max="512"
            step="1"
            :class="{ invalid: errors['audio.bitrate_kbps'] }"
          />
          <span class="hint">AAC。不占视频的码率预算，两路各自独立编码</span>
          <span v-if="errors['audio.bitrate_kbps']" class="field-error">{{
            errors['audio.bitrate_kbps']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">系统声音增益（0–8）</span>
          <input
            v-model.number="form.audio.system_gain"
            type="number"
            min="0"
            max="8"
            step="0.1"
            :class="{ invalid: errors['audio.system_gain'] }"
          />
          <span class="hint">1 = 原样。两个来源同时开时各自会再乘 1/√2，避免恒定削顶</span>
          <span v-if="errors['audio.system_gain']" class="field-error">{{
            errors['audio.system_gain']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">麦克风增益（0–8）</span>
          <input
            v-model.number="form.audio.mic_gain"
            type="number"
            min="0"
            max="8"
            step="0.1"
            :class="{ invalid: errors['audio.mic_gain'] }"
          />
          <span class="hint">0 = 这一路静音（音轨还在）。麦克风底噪大时先往下调</span>
          <span v-if="errors['audio.mic_gain']" class="field-error">{{
            errors['audio.mic_gain']
          }}</span>
        </label>
      </div>

      <h4 class="sub-head">让改动生效</h4>

      <p class="hint section-note">
        音频是<strong>冷</strong>字段：开关它、换设备、改增益都要<strong>重开采集设备</strong>
        （设备、编码器与 SDP 都是建管线那一刻定下来的），只保存的话画面上的音轨还是旧的那一条。
        下面的按钮 = 保存 + 重开管线，代价是<strong>录像分段</strong>（新文件从 main.1.mp4 续）
        加<strong>已连上的 RTSP / NVR 断开重连</strong>。
      </p>

      <div class="actions">
        <button class="btn primary" type="button" :disabled="audioApplying || saving" @click="onAudioApply">
          <Icon name="zap" :size="15" />{{ audioApplying ? '正在应用…' : '保存并立即应用' }}
        </button>
        <span class="hint">只想存下来、下次重启再生效的话，用页面底部那条「保存」</span>
      </div>

      <div
        v-if="audioApplyMsg"
        class="banner"
        :class="{
          'is-ok': audioApplyTone === 'ok',
          'is-warn': audioApplyTone === 'warn',
          'is-err': audioApplyTone === 'err',
        }"
        style="margin: 12px 0 0"
      >
        {{ audioApplyMsg }}
      </div>
    </fieldset>

    <!-- `time` = 「时间设置」的落点。
         **这一整段管的都是「设备时间」**（= 主机时间 + 偏移，水印上写的、ONVIF
         报给 NVR 的那个），不是这台机器的 Windows 系统时钟。后者要管理员权限、
         会连累这台机器上别的程序，而且那是用户的钟不是我们的 —— 取舍见 plan
         的「已确认的取舍」表。这段话在段首就要说，不能只写在 README 里：
         用户看到"改了时间"的第一反应是"我改了这台电脑的时间"。 -->
    <fieldset id="time">
      <legend>时间设置</legend>

      <p class="hint section-note">
        这里改的是<strong>设备时间</strong>：主机时间加上一个偏移，也就是
        <strong>水印上写的、ONVIF 报给 NVR 的那个时间</strong>。
        本机的 Windows 系统时钟<strong>不会被改动</strong> —— 那要管理员权限，
        而且会连累这台机器上别的程序。所以"设备时间与水印时间和这台电脑的系统时间
        差了几个小时"是一个正常状态，不是故障：它正是在把画面时间对齐到录像机。
      </p>

      <div v-if="timeError" class="banner is-warn">
        读不到时间信息：{{ timeError }}
        <br />
        老一些的后端上没有 <code>/api/time</code> 这条接口，那种情况下这一段读不出来，
        但配置里那四项照样能改、照样能保存。
      </div>

      <template v-else>
        <!-- 这个标题原本写的是"读取于上一次刷新" —— 那在以前是实话，那两格确实
             冻着不动。它们现在每秒自己走了，这句话就成了**会误导人的那种实话**：
             用户会以为自己看到的是"上次刷新那一刻的设备时间"。所以改成说清
             **读数取自哪一刻**（那才是这两格真正的基准），并明说它自己会走。 -->
        <h4 class="sub-head">
          当前（这两格每秒自己走；读数取于
          {{ clockReadAtSec === null ? '—' : fmtStamp(clockReadAtSec) }}）
        </h4>
        <!-- 老后端（没有 `host_now_ms` 那种）上，这两格照样会走，但秒的相位是
             整数秒推出来的 ⇒ 大约一半的时间里显示的秒会比真值小 1。这一页要回答
             的恰恰是"对准了没有"，所以那种情况下必须说出来 —— 否则用户会拿这个
             ±1 秒的显示去下判断，而问题在尺子上。新版后端不发这一条提示。 -->
        <p v-if="timeInfo && timeInfo.host_now_ms === undefined" class="hint" style="margin: 0 0 8px">
          注意：这个后端只报整秒，所以这两格的秒可能比真值慢不到 1 秒（大约一半的时间）。
          核对是否对准时请把这一项算进去 —— 它不是时间没对上。
        </p>

        <div class="grid">
          <label class="field">
            <span class="field-label">主机时间</span>
            <input
              type="text"
              readonly
              :value="liveHostSec === null ? '读取中…' : fmtStamp(liveHostSec)"
            />
            <span class="hint">这台机器的 Windows 系统时钟。我们不改它</span>
          </label>

          <label class="field">
            <span class="field-label">设备时间</span>
            <input
              type="text"
              readonly
              :value="liveDeviceSec === null ? '读取中…' : fmtStamp(liveDeviceSec)"
            />
            <span class="hint">水印上、ONVIF 里的那个时间</span>
          </label>

          <label class="field">
            <span class="field-label">偏移（设备 − 主机）</span>
            <input
              type="text"
              readonly
              :value="timeInfo ? fmtOffset(timeInfo.offset_s) : '读取中…'"
              :class="{ invalid: timeInfo ? Math.abs(timeInfo.offset_s) > 300 : false }"
            />
            <span class="hint">整秒。亚秒部分在 NTP 那条路上会被丢掉</span>
          </label>
        </div>

        <!-- 超过 5 分钟就把它说出来：这是一个会改变**证据内容**的开关
             （录像上的时间不再等于本机时钟），不能让它在界面上一副无害的样子。 -->
        <div v-if="timeInfo && Math.abs(timeInfo.offset_s) > 300" class="banner is-warn">
          设备时间与本机时间相差 {{ fmtOffset(timeInfo.offset_s) }}。
          画面上的水印、以及 ONVIF 报给 NVR 的时间<strong>都按设备时间走</strong>，
          而文件与日志的时间戳仍是本机时间 —— 两者现在差着这么多。
          如果这不是你要的（比如 NVR 替你对了一次时），按下面的「归零」。
        </div>

        <div class="grid" style="margin-top: 12px">
          <label class="field">
            <span class="field-label">对时模式（存进配置的是这一项）</span>
            <select v-model="form.time.mode">
              <option value="manual">手动（自己校准 / 由 NVR 来调）</option>
              <option value="ntp">NTP 自动对时</option>
            </select>
            <span class="hint">
              它同时决定 <code>GetSystemDateAndTime</code> 报 Manual 还是 NTP ——
              NVR 会照这个决定要不要来替我们对时
            </span>
          </label>

          <template v-if="form.time.mode === 'ntp'">
            <label class="field">
              <span class="field-label">NTP 服务器</span>
              <input v-model.trim="form.time.ntp_server" type="text" placeholder="ntp.aliyun.com" />
              <span class="hint">
                域名或 IP。可以填内网地址（不少 NVR 与路由器自带 NTP 服务）。
                没网的环境就是对不上，那时这一行会报"超时"而不是静默失败
              </span>
              <span v-if="errors['time.ntp_server']" class="field-error">
                {{ errors['time.ntp_server'] }}
              </span>
            </label>

            <label class="field">
              <span class="field-label">对时间隔（秒）</span>
              <input v-model.number="form.time.ntp_interval_s" type="number" step="1" min="0" max="86400" />
              <span class="hint">
                <strong>0 = 只在启动时对一次</strong>，不是"关掉 NTP"
              </span>
              <span v-if="errors['time.ntp_interval_s']" class="field-error">
                {{ errors['time.ntp_interval_s'] }}
              </span>
            </label>
          </template>
        </div>

        <p v-if="errors['time.mode']" class="field-error">{{ errors['time.mode'] }}</p>

        <!-- 四个数里**只有 `offset_s` 是"要重启才生效"的**：它是启动时灌进
             DeviceState 的初值，运行中改配置不会自己挪钟（要挪就得按下面那些
             按钮）。其余三项由启动那一步与定时器读，同样要重启。
             这句话必须在这里说 —— 否则用户改完偏移、按页面底部的「保存」，
             会看到"已保存"而钟纹丝不动。 -->
        <p class="hint section-note">
          <strong>这一段的四项配置都要重启才生效</strong> —— 它们是启动时读的
          （那一步同时也用 Windows 时间来启动对时定时器）。
          想<strong>现在就把钟改掉</strong>，用下面那组按钮，它们当场生效、不用重启。
        </p>

        <h4 class="sub-head">现在就对一次</h4>

        <p class="hint section-note">
          手动模式下这三个动作<strong>当场生效</strong>（写进配置、重启后还在）；
          NTP 模式下「校准」与「归零」会被后端拒绝 —— 那种模式下的钟归对时线程管，
          下一个周期就把手动设的值覆盖回去了，报"成功"才是骗人。
        </p>

        <div class="grid">
          <label class="field" v-if="form.time.mode === 'manual'">
            <span class="field-label">把设备时间设成</span>
            <input
              v-model="manualTimeInput"
              type="datetime-local"
              step="1"
              :placeholder="timeInfo ? toLocalStamp(timeInfo.device_now) : '2026-10-08T15:30:00'"
            />
            <span class="hint">
              形如 2026-10-08T15:30:00，按<strong>这台被控机器</strong>的时区解析。
              输入框不预填当前时间，是为了不把"你看到的那个钟"和"它机器上的时区"
              悄悄混在一起 —— 只想跟这个页面所在的电脑对齐的话，用右边那个按钮
            </span>
          </label>
        </div>

        <div class="actions">
          <button
            v-if="form.time.mode === 'manual'"
            class="btn primary"
            type="button"
            :disabled="timeBusy"
            @click="onTimeSetAbsolute"
          >
            <Icon name="check" :size="15" />设为这个时间
          </button>
          <button
            v-if="form.time.mode === 'manual'"
            class="btn"
            type="button"
            :disabled="timeBusy"
            @click="onTimeAlignToBrowser"
          >
            <Icon name="refresh" :size="15" />与浏览器时间对齐
          </button>
          <button
            v-if="form.time.mode === 'manual'"
            class="btn"
            type="button"
            :disabled="timeBusy"
            @click="onTimeReset"
          >
            <Icon name="rotate-ccw" :size="15" />归零（恢复主机时间）
          </button>
          <button class="btn" type="button" :disabled="timeBusy" @click="onTimeSyncNow">
            <Icon name="zap" :size="15" />立刻对时（NTP）
          </button>
          <button class="btn" type="button" :disabled="timeBusy || timeLoading" @click="onTimeRefresh">
            <Icon name="refresh" :size="14" />{{ timeLoading ? '读取中…' : '重新读取' }}
          </button>
        </div>

        <p class="hint">
          「立刻对时」<strong>不等待结果</strong>：真正的 UDP 查询在后台线程上
          （最长 2 秒 × 地址数），所以接口立刻回 202，结果要按「重新读取」来看。
          当前是手动模式时，这一次<strong>只报结果、不会改动设备时间</strong>。
        </p>

        <div
          v-if="timeMsg"
          class="banner"
          :class="{
            'is-ok': timeTone === 'ok',
            'is-warn': timeTone === 'warn',
            'is-err': timeTone === 'err',
          }"
        >
          {{ timeMsg }}
        </div>

        <h4 class="sub-head">对时状态</h4>

        <div class="grid">
          <label class="field">
            <span class="field-label">最近一次对时</span>
            <input
              type="text"
              readonly
              :value="timeInfo && timeInfo.last_sync_at ? fmtStamp(timeInfo.last_sync_at) : '还没试过'"
            />
            <span class="hint">「试过」不等于「成功」，结果看右边那一格</span>
          </label>

          <label class="field">
            <span class="field-label">结果</span>
            <input
              type="text"
              readonly
              :class="{ invalid: timeSyncFailed }"
              :value="timeInfo ? syncStateText(timeInfo.last_sync_state) : '—'"
            />
            <span class="hint">
              「还没试过」与"失败"是两回事，所以这两句必须分开写
            </span>
          </label>

          <label class="field" v-if="timeInfo && timeInfo.last_sync_state === 'ok'">
            <span class="field-label">对出来的偏移 / 层级</span>
            <input
              type="text"
              readonly
              :value="`${fmtOffset(timeInfo.last_sync_offset_s)}，stratum ${timeInfo.last_sync_stratum}`"
            />
            <span class="hint">
              这次读到的偏移<strong>不进配置</strong>（配置里存的是
              {{ fmtOffset(timeInfo.configured_offset_s) }}），重启后由启动那一步决定
            </span>
          </label>

          <label class="field" v-if="timeInfo && timeInfo.last_sync_error">
            <span class="field-label">失败原因</span>
            <input type="text" readonly :value="timeInfo.last_sync_error" />
          </label>

          <label class="field">
            <span class="field-label">累计成功 / 失败</span>
            <input
              type="text"
              readonly
              :value="timeInfo ? `${timeInfo.sync_ok_count} / ${timeInfo.sync_fail_count}` : '—'"
            />
            <span class="hint" v-if="timeInfo && timeInfo.syncing">此刻有一次查询在飞…</span>
          </label>
        </div>

        <p class="hint">
          这里显示的是<strong>读取那一刻</strong>的数、以及"这个钟被改过没有"。
          要看每秒跳动的偏移，去「实时状态」页顶部那一行 —— 那条走 WebSocket，
          每秒刷一次，与这一份是同一个来源。
        </p>
      </template>
    </fieldset>

    <!-- `system` = 「系统设置」的落点。它盖住的是 RTSP 与 Web 服务**两块** ——
         锚点落在前一块的头上，后一块紧接着在下面（相邻两段之间本来也没有别的东西）。
         为一个菜单项把两块勉强合成一块，反而会让这里的分组失去意义。 -->
    <fieldset id="system">
      <legend>RTSP</legend>
      <div class="grid">
        <label class="field check">
          <input v-model="form.rtsp.enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">开启 RTSP 推流</span>
        </label>

        <label class="field">
          <span class="field-label">端口（1–65535，默认 554）</span>
          <input
            v-model.number="form.rtsp.port"
            type="number"
            min="1"
            max="65535"
            step="1"
            :class="{ invalid: errors['rtsp.port'] }"
          />
          <span v-if="errors['rtsp.port']" class="field-error">{{ errors['rtsp.port'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">路径</span>
          <input
            v-model="form.rtsp.path"
            type="text"
            placeholder="/live"
            :class="{ invalid: errors['rtsp.path'] }"
          />
          <span v-if="errors['rtsp.path']" class="field-error">{{ errors['rtsp.path'] }}</span>
        </label>
      </div>

      <!--
        子码流单独一组。它不是"再开一个 RTSP"，而是**第二路编码器** ——
        两路的分辨率和码率是各自独立的，混在同一个 grid 里会让人以为填的是
        同一个东西的两半。
      -->
      <div class="grid" style="margin-top: 12px">
        <label class="field check">
          <input v-model="form.rtsp.sub_enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">开启子码流</span>
        </label>

        <label class="field">
          <span class="field-label">子码流路径</span>
          <input
            v-model="form.rtsp.sub_path"
            type="text"
            placeholder="/live_sub"
            :class="{ invalid: errors['rtsp.sub_path'] }"
          />
          <span v-if="errors['rtsp.sub_path']" class="field-error">{{
            errors['rtsp.sub_path']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">子码流高度（120–2160，宽度按主码流比例推）</span>
          <input
            v-model.number="form.rtsp.sub_height"
            type="number"
            min="120"
            max="2160"
            step="1"
            :class="{ invalid: errors['rtsp.sub_height'] }"
          />
          <span v-if="errors['rtsp.sub_height']" class="field-error">{{
            errors['rtsp.sub_height']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">子码流码率（kbps）</span>
          <input
            v-model.number="form.rtsp.sub_bitrate_kbps"
            type="number"
            min="32"
            max="100000"
            step="1"
            :class="{ invalid: errors['rtsp.sub_bitrate_kbps'] }"
          />
          <span v-if="errors['rtsp.sub_bitrate_kbps']" class="field-error">{{
            errors['rtsp.sub_bitrate_kbps']
          }}</span>
        </label>
      </div>

      <p class="hint">
        <strong>子码流是第二路编码器</strong>，专门伺候 NVR 的多画面网格：NVR 默认拉的是主码流，
        于是 16 路网格会同时拉 16 条全分辨率的流。开着它，网格走子码流、点开单画面才切主码流。
        它只有开了 RTSP 才有意义。
        <template v-if="stats.sub_width > 0">
          <br />当前实际尺寸 <code class="value">{{ stats.sub_width }}x{{ stats.sub_height }}</code
          >。
        </template>
      </p>

      <p class="hint" style="margin-bottom: 0">
        本机推流地址约等于 <code class="value">{{ rtspPreview }}</code>
        <template v-if="stats.rtsp_url">
          ；后端当前实际地址是 <code class="value">{{ stats.rtsp_url }}</code>（以这个为准）
        </template>
        <br />Windows 防火墙默认拦 554/TCP 入站，别的机器（包括 NVR）连不上时先放行该端口。
        子码流走同一个端口，不需要额外放行。
      </p>
    </fieldset>

    <fieldset>
      <legend>Web 服务</legend>
      <div class="grid">
        <label class="field check">
          <input v-model="form.web.enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">开启 Web 服务</span>
        </label>

        <label class="field">
          <span class="field-label">监听地址</span>
          <input
            v-model="form.web.host"
            type="text"
            placeholder="0.0.0.0"
            :class="{ invalid: errors['web.host'] }"
          />
          <span class="hint">改成 127.0.0.1 就只有本机能打开这个界面</span>
          <span v-if="errors['web.host']" class="field-error">{{ errors['web.host'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">端口（1–65535）</span>
          <input
            v-model.number="form.web.port"
            type="number"
            min="1"
            max="65535"
            step="1"
            :class="{ invalid: errors['web.port'] }"
          />
          <span v-if="errors['web.port']" class="field-error">{{ errors['web.port'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">静态资源目录</span>
          <input
            v-model="form.web.static_root"
            type="text"
            placeholder="web/dist"
            :class="{ invalid: errors['web.static_root'] }"
          />
          <span class="hint">前端构建产物的目录，相对后端进程的工作目录解析</span>
          <span v-if="errors['web.static_root']" class="field-error">{{
            errors['web.static_root']
          }}</span>
        </label>
      </div>

      <!--
        登录设置。和 ONVIF 那份凭据**故意分开**：NVR 里存着的那份是给机器用的
        （改一次要去每台 NVR 上改），网页这份是人手敲的。共用的后果是
        "为了改网页口令得把 NVR 全部重配一遍"，于是没人会去改它。
      -->
      <div class="grid" style="margin-top: 12px">
        <label class="field check">
          <input v-model="form.web.auth_enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">打开这个界面需要登录</span>
        </label>

        <label class="field">
          <span class="field-label">登录用户名</span>
          <input
            v-model="form.web.user"
            type="text"
            autocomplete="off"
            :class="{ invalid: errors['web.user'] }"
          />
          <span v-if="errors['web.user']" class="field-error">{{ errors['web.user'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">登录口令</span>
          <!-- 后端只回哨兵，真口令到不了浏览器，所以这里永远从空开始。 -->
          <input
            v-model="webPasswordInput"
            type="password"
            autocomplete="new-password"
            :placeholder="
              hasStoredWebPassword ? '已设置（留空则不修改）' : '未设置（留空则不修改）'
            "
          />
          <span class="hint">留空保持原口令不变；只有在这里输入新值才会改掉它。</span>
        </label>
      </div>

      <!--
        空口令在本服务里**不是**"不鉴权"：开关开着、口令是空的时候
        `SessionStore::effective()` 返回 false，于是前端会跳过登录页 ——
        也就是说它等于把门敞着，却让人以为有门。这条必须明说。
      -->
      <div v-if="form.web.auth_enabled && webPasswordEmpty && webPasswordInput === ''" class="banner is-warn" style="margin-top: 12px">
        <strong>登录是开着的，但口令是空的。</strong>
        空口令在这里不等于"不用登录"，而是"口令就是空" —— 任何知道用户名的人按一下
        回车就能进来。请在右边填一个口令再保存，或者干脆把登录关掉（那样前端会
        跳过登录页，至少不会让人误以为自己是安全的）。
      </div>

      <div v-if="exposedWithoutAuth" class="banner is-err" style="margin-top: 12px">
        <strong>这个控制台会对整个局域网敞开。</strong>
        监听地址是 <code class="value">{{ form.web.host }}</code>，同时没有启用登录 ——
        同一网段里任何一台机器都能读配置、改配置、看到你的桌面画面。
        两条退路任选：把监听地址改成 <code class="value">127.0.0.1</code>，
        或者把「需要登录」打开并设一个口令。
      </div>

      <p class="hint" style="margin-bottom: 0">
        登录设置<strong>当场生效</strong>，不需要重启。有两点要知道：
        改口令或者关掉登录<strong>不会把已经登进来的人踢下线</strong>（会话令牌签的是每进程
        一份的随机密钥，不是口令），要立刻清空所有人只能重启程序；
        反过来，<strong>打开登录会让当前这个会话立刻被要求重新登录</strong> ——
        存完这一下就会跳回登录页，用新口令进去即可。
      </p>
    </fieldset>

    <!--
      生命周期。放在「Web 服务」与「ONVIF」之间，不是随手放的：侧边栏那个
      「系统设置」的锚点打在 RTSP 那一段上，锚点之后的归属靠的是"同一组挨着
      放"—— 菜单这一层表达不了范围，只有版面能。
    -->
    <fieldset id="lifecycle">
      <legend>重启与恢复出厂</legend>

      <p class="hint" style="margin-top: 0">
        这两个动作换掉的是<strong>进程本身</strong>，与上面那个「重启 ONVIF 响应器」
        不是一回事（那一个只重播一次 WS-Discovery 组播，画面一秒都不中断）。
        三条代价每次都一样：<strong>当前录制中断并分段</strong>（新文件从
        <code class="value">main.1.mp4</code> 续）、<strong>已连上的 RTSP 客户端断开</strong>
        （几秒后自己重连）、<strong>所有网页会话失效，必须重新登录</strong> ——
        会话令牌签的是每进程一份的随机密钥，重启必然换掉它。
      </p>

      <div class="actions" style="margin-top: 14px">
        <button type="button" :disabled="lifecycleBusy" @click="onRestartService">
          <Icon name="refresh" :size="15" />{{ lifecycle.phase === 'restart' ? '正在重启…' : '重启服务' }}
        </button>
        <button
          type="button"
          class="danger"
          :disabled="lifecycleBusy"
          @click="resetConfirmOpen = !resetConfirmOpen"
        >
          <Icon name="alert" :size="15" />恢复出厂设置
        </button>
        <span class="hint">
          配置里那些"改了要重启才生效"的字段不用走这里 —— 保存条旁边那个
          「保存并立即应用」就够。这两个按钮管的是程序本身不对劲的时候：端口没放开、
          NVR 怎么都搜不到、或者配置文件被你手改坏了。
        </span>
      </div>

      <!--
        二次确认是一个内联面板，不是 window.confirm：后者在某些环境下会被静默
        吃掉（返回 false 且什么都不显示），那时按钮点了没反应，看起来像坏的。
        服务端也不信"我点了确认"——它只认请求体里那个字面量。
      -->
      <div v-if="resetConfirmOpen" class="banner is-err" style="margin: 12px 0 0">
        <strong>恢复出厂会把配置删掉，确定吗？</strong>
        <p style="margin: 8px 0 0">
          顺序是固定的：<code class="value">config.json</code> 先备份成
          <code class="value">config.json.bak</code>（后悔了可以自己改回来），然后删掉它；
          操作日志与调试日志一并清空；设备时间偏移归零、设备名称复位；最后重启
          —— 也就是上面那三条代价再走一遍。
        </p>
        <p style="margin: 8px 0 0">
          <strong>一条必须说清的边界：恢复的是配置文件。</strong>
          命令行上带的参数（端口、口令、<code class="value">--no-onvif</code> 之类）
          重启后照样生效，因为重启用的是同一条命令行。安装包形态确实会回到
          8080 端口与 admin / admin，但那是因为它的命令行本来就和默认值一致，
          不是"恢复出厂改了口径"。
        </p>
        <p style="margin: 8px 0 0">
          不碰防火墙规则，也不碰配置目录以外的任何东西。
        </p>
        <div class="actions" style="margin-top: 12px">
          <button type="button" class="danger" :disabled="lifecycleBusy" @click="onFactoryReset">
            <Icon name="alert" :size="15" />确认恢复出厂并重启
          </button>
          <button type="button" class="ghost" :disabled="lifecycleBusy" @click="resetConfirmOpen = false">
            取消
          </button>
        </div>
      </div>

      <div
        v-if="lifecycle.message"
        class="banner"
        :class="lifecycle.phase === 'fail' ? 'is-err' : lifecycle.phase === 'done' ? 'is-ok' : 'is-warn'"
        style="margin: 12px 0 0"
      >
        {{ lifecycle.message }}
      </div>
    </fieldset>

    <fieldset id="onvif">
      <legend>ONVIF</legend>
      <div class="grid">
        <label class="field check">
          <input v-model="form.onvif.enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">开启 ONVIF / WS-Discovery</span>
        </label>

        <label class="field">
          <span class="field-label">用户名</span>
          <input
            v-model="form.onvif.user"
            type="text"
            autocomplete="off"
            :class="{ invalid: errors['onvif.user'] }"
          />
          <span v-if="errors['onvif.user']" class="field-error">{{ errors['onvif.user'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">密码</span>
          <!-- 后端只回哨兵，真口令到不了浏览器，所以这里永远从空开始。 -->
          <input
            v-model="onvifPasswordInput"
            type="password"
            autocomplete="new-password"
            :placeholder="
              hasStoredOnvifPassword ? '已设置（留空则不修改）' : '未设置（留空则不修改）'
            "
          />
          <span class="hint">留空保持原密码不变；只有在这里输入新值才会改掉它。</span>
        </label>

        <label class="field">
          <span class="field-label">厂商</span>
          <input v-model="form.onvif.manufacturer" type="text" />
        </label>

        <label class="field">
          <span class="field-label">型号</span>
          <input v-model="form.onvif.model" type="text" />
        </label>

        <label class="field">
          <span class="field-label">固件版本</span>
          <input v-model="form.onvif.firmware_version" type="text" />
          <!-- 两个"版本号"摆在一起不解释会让人以为其中一个坏了：NVR 上显示的
               是这个可改的值，而设备信息页那个 version 是程序真实版本（只读）。 -->
          <span class="hint">
            NVR 上显示的是这个值；设备信息页显示的 version 是程序真实版本。
          </span>
        </label>

        <!-- ============ 以下三个由本机硬件决定，网页上改不动 ============
             用户明确选的取舍：安装好之后这三个值就固定死，避免用户乱改导致
             NVR 那边多出一台"新"设备。后端 `apply_config` 里有一道**无条件**
             回灌（只读锁），所以就算有人绕过前端手搓一个 POST 也改不动 ——
             这里 `disabled` 只是把这件事**说在前面**，不是那道防线本身。

             **逃生门是 config.json**：真要区分克隆出来的两台机器，就手改文件
             里的 `onvif.serial_number` / `onvif.device_uuid`。 -->
        <label class="field">
          <span class="field-label field-lock">
            <Icon name="lock" :size="13" />硬件 ID（只读）
          </span>
          <input class="id-readonly" v-model="form.onvif.hardware_id" type="text" disabled />
          <span class="hint">由本机硬件决定。要修改请直接编辑 config.json。</span>
        </label>

        <label class="field">
          <span class="field-label field-lock">
            <Icon name="lock" :size="13" />序列号（只读）
          </span>
          <input class="id-readonly" v-model="form.onvif.serial_number" type="text" disabled />
          <!-- 序列号**可以是算出来的**：第一次启动时后端按本机标识（机器名 +
               系统 MachineGuid）派生一个、当场写进 config.json，此后永远用
               文件里那份。不说明的话，"我没填过这个号，它是哪来的"会变成一个
               查不动的问题。 -->
          <span class="hint">
            首次启动时按本机标识（机器名 + 系统 MachineGuid）自动生成并写进 config.json，
            换网卡不变。要修改请直接编辑 config.json。
          </span>
        </label>

        <label class="field">
          <span class="field-label field-lock">
            <Icon name="lock" :size="13" />设备 UUID（只读）
          </span>
          <input class="id-readonly" v-model="form.onvif.device_uuid" type="text" disabled />
          <!-- NVR 拿它当主键，一变就会在列表里多出一台"新"摄像机 ——
               所以这个值比序列号更不能让用户在网页上手滑改掉。 -->
          <span class="hint">
            NVR 用它当设备主键，变了就会多出一台"新"摄像机。要修改请直接编辑 config.json。
          </span>
        </label>

        <label class="field">
          <span class="field-label">设备名称</span>
          <input v-model="form.onvif.device_name" type="text" />
        </label>

        <label class="field">
          <span class="field-label">位置</span>
          <input v-model="form.onvif.location" type="text" />
        </label>
      </div>

      <div class="actions" style="margin-top: 14px">
        <button type="button" :disabled="restarting" @click="onRestartOnvif">
          <Icon name="refresh" :size="15" />{{ restarting ? '正在重启…' : '重启 ONVIF 响应器' }}
        </button>
        <span class="hint">
          NVR 搜不到设备时先按这个（重启 WS-Discovery，需要重新发一次 Hello）。
        </span>
      </div>
      <div v-if="restartMsg" class="banner" style="margin: 12px 0 0">{{ restartMsg }}</div>
    </fieldset>

    <!--
      保存条粘在视口底部（`.save-bar`，见 styles.css）。
      **因为表单和保存按钮只有一份**：侧栏把这一页拆成几个锚点段落之后，用户
      完全可能停在中间某一段 —— 保存按钮要是只跟在最后那一段后面，锚点导航
      就等于制造了一个新的"找不到保存"。
    -->
    <div class="actions save-bar">
      <!--
        **故意不在 errorCount > 0 时禁用这个按钮。** 禁用等于让点击彻底无声：
        用户点一下、什么都没发生、也没有一句话 —— 那正是这次要修的毛病本身。
        让它可点，点了就弹出上面那条带清单的横幅并滚到第一个错处。
      -->
      <button type="submit" class="primary" :disabled="saving">
        <Icon name="save" :size="15" />{{ saving ? '保存中…' : '保存' }}
      </button>
      <button type="button" :disabled="saving" @click="load">
        <Icon name="refresh" :size="15" />放弃修改并重载
      </button>
    </div>
  </form>
</template>
