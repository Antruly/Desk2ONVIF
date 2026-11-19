<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import Icon from '../components/Icon.vue'
import { channelSnapshotUrl, setPreviewChannel } from '../api'
import { useConfig } from '../composables/useConfig'
import { useStats } from '../composables/useStats'
import type {
  ChannelConfig,
  ChannelStat,
  LatencyTier,
  OsdConfig,
  OsdItemConfig,
  QualityTier,
} from '../types'

/**
 * 某一路通道的详情：**图像参数 + 水印**。
 *
 * ## 这个页面的核心约定：没有「保存」按钮
 *
 * 视频参数与水印都是**改完自动生效**（防抖 450 ms）。用户的原话是"这些设置
 * 视频参数点击即生效，而不是要我保存；只有系统设置类才需要我保存"。
 *
 * 后端那侧的两条生效方式不同，这里如实跟着走：
 *   · 水印是**热生效**（`LiveOsd`，下一帧就变）→ 只 `save`；
 *   · 帧率/码率/分辨率/子码流/延时档要**重开这一路的管线** → `save + reload`。
 *     代价是这一路（和共享的 RTSP 服务器）断一两秒，界面上明说"正在重开"，
 *     不把它包装成一个无害的动作。
 */

const props = defineProps<{ number: string | number }>()
const router = useRouter()
const { config, load, save, applyMedia, items } = useConfig()
const { stats } = useStats()

const n = computed(() => {
  const v = Number(props.number)
  return Number.isFinite(v) && v >= 1 ? Math.trunc(v) : 1
})

const isLocal = computed(() => n.value === 1)

const liveRow = computed<ChannelStat | undefined>(() =>
  (stats.value.channels ?? []).find((c) => (c.number ?? c.index) === n.value),
)

const cfgItem = computed<ChannelConfig | null>(
  () => items().find((c) => (c.number ?? 0) === n.value) ?? null,
)

const pageError = computed(() => {
  if (!isLocal.value && cfgItem.value === null && config.value) {
    return `没有通道 ${n.value}。它可能已经被删除了。`
  }
  return ''
})

const displayName = computed(() => {
  if (isLocal.value) return '本地桌面'
  return cfgItem.value?.name || `通道 ${n.value}`
})

const onvifPort = computed(() => {
  const base = stats.value.web_port || config.value?.web.port || 0
  return base > 0 ? base + (isLocal.value ? 0 : n.value) : 0
})

/**
 * 本机对外的地址。从 `/api/status` 的 `rtsp_url` 里抠主机名 —— 那个地址是
 * 后端按"NVR 从哪个网段来"选出来的，比在界面上猜一个网卡地址准。
 * 没有 RTSP 时留占位符，让用户知道这里是"这台机器的地址"而不是字面量。
 */
const localHost = computed(() => {
  const m = /rtsp:\/\/([^/:]+)/.exec(stats.value.rtsp_url || '')
  return m?.[1] || '<本机IP>'
})

const rtspUrl = computed(() => {
  const path = isLocal.value
    ? config.value?.rtsp.path
    : cfgItem.value?.rtsp_path
  if (!path) return ''
  const port = config.value?.rtsp.port ?? 554
  return `rtsp://${localHost.value}:${port}${path}`
})

const stateText = computed(() => liveRow.value?.state ?? '连接中…')
const stateTone = computed(() => {
  const s = stateText.value
  if (s.startsWith('无信号')) return 'wait'
  if (s.startsWith('失败') || s.startsWith('未启用')) return 'bad'
  if (s === '运行中' || s === '采集中') return 'ok'
  return 'idle'
})

// ---------------------------------------------------------------------------
// 表单
// ---------------------------------------------------------------------------

interface Form {
  name: string
  enabled: boolean
  quality: string
  fps: number
  bitrate_kbps: number
  max_height: number
  latency: string
  gop_seconds: number
  threads: number
  sub_enabled: boolean
  sub_height: number
  sub_bitrate_kbps: number
  osd: OsdConfig
}

function defaultOsd(): OsdConfig {
  return {
    enabled: true,
    adaptive_color: true,
    outline: true,
    plate_alpha: 0,
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
  }
}

function cloneOsd(o: OsdConfig | undefined): OsdConfig {
  if (!o) return defaultOsd()
  return JSON.parse(JSON.stringify(o)) as OsdConfig
}

const form = reactive<Form>({
  name: '',
  enabled: true,
  quality: 'balanced',
  fps: 25,
  bitrate_kbps: 2048,
  max_height: 0,
  latency: 'balanced',
  gop_seconds: 2,
  threads: 3,
  sub_enabled: true,
  sub_height: 360,
  sub_bitrate_kbps: 512,
  osd: defaultOsd(),
})

/**
 * 把权威配置抄进表单。**只在"没有未应用的本地改动"时调**（见 `flush`）——
 * 否则用户正敲到一半的帧率会被一次回读覆盖成旧值。
 */
function syncFromConfig() {
  const c = config.value
  if (!c) return
  if (isLocal.value) {
    form.name = '本地桌面'
    form.enabled = true
    form.quality = c.capture.quality
    form.fps = c.capture.fps
    form.bitrate_kbps = c.capture.bitrate_kbps
    form.max_height = c.capture.max_height
    form.latency = c.capture.latency
    form.gop_seconds = c.capture.gop_seconds
    form.threads = c.capture.threads
    form.sub_enabled = c.rtsp.sub_enabled
    form.sub_height = c.rtsp.sub_height
    form.sub_bitrate_kbps = c.rtsp.sub_bitrate_kbps
    form.osd = cloneOsd(c.osd)
    return
  }
  const it = cfgItem.value
  if (!it) return
  form.name = it.name
  form.enabled = it.enabled
  form.quality = it.quality
  form.fps = it.fps
  form.bitrate_kbps = it.bitrate_kbps
  form.max_height = it.max_height
  form.latency = it.latency ?? 'balanced'
  form.gop_seconds = it.gop_seconds ?? 2
  form.threads = it.threads ?? 3
  form.sub_enabled = it.sub_enabled
  form.sub_height = it.sub_height
  form.sub_bitrate_kbps = it.sub_bitrate_kbps
  form.osd = cloneOsd(it.osd)
}

// ---------------------------------------------------------------------------
// 自动应用（防抖 + 串行）
// ---------------------------------------------------------------------------
const DEBOUNCE_MS = 450
const applying = ref(false)
const applyNote = ref('')
const applyError = ref('')
let timer = 0
let dirty = 0
let pendingMedia = false
let queued = false

/** 用户动了一下。`media = true` 表示这一次要重开管线。 */
function touch(media: boolean) {
  ++dirty
  if (media) pendingMedia = true
  if (timer) window.clearTimeout(timer)
  timer = window.setTimeout(() => {
    timer = 0
    void flush()
  }, DEBOUNCE_MS)
}

function buildPatch(): Record<string, unknown> {
  if (isLocal.value) {
    // 通道 1 的参数住在顶层 `capture` / `rtsp` / `osd`。
    return {
      capture: {
        quality: form.quality,
        fps: form.fps,
        bitrate_kbps: form.bitrate_kbps,
        max_height: form.max_height,
        latency: form.latency,
        gop_seconds: form.gop_seconds,
        threads: form.threads,
      },
      rtsp: {
        sub_enabled: form.sub_enabled,
        sub_height: form.sub_height,
        sub_bitrate_kbps: form.sub_bitrate_kbps,
      },
      osd: form.osd,
    }
  }
  const it = cfgItem.value
  if (!it) return {}
  const next: ChannelConfig = {
    ...it,
    name: form.name,
    enabled: form.enabled,
    quality: form.quality,
    fps: form.fps,
    bitrate_kbps: form.bitrate_kbps,
    max_height: form.max_height,
    latency: form.latency,
    gop_seconds: form.gop_seconds,
    threads: form.threads,
    sub_enabled: form.sub_enabled,
    sub_height: form.sub_height,
    sub_bitrate_kbps: form.sub_bitrate_kbps,
    osd: form.osd,
  }
  return {
    channels: { items: items().map((c) => ((c.number ?? 0) === n.value ? next : c)) },
  }
}

/**
 * 数值输入的兜底。
 *
 * `<input type="number">` 被清空时 `v-model.number` 给的是空串，而后端对类型
 * 错误是**整份拒绝**（`config_from_json` 抛类型错误）。把那串空值发出去的结果
 * 是"改一下帧率，整页弹一个看不懂的 JSON 类型错误" —— 所以在这里先挡住，
 * 并说清楚是哪一项。
 */
function checkForm(): string {
  const num = (v: unknown) => (typeof v === 'number' ? v : Number(v))
  const fail = (label: string) => `「${label}」不是一个有效数字。`
  const range = (label: string, v: unknown, lo: number, hi: number) => {
    const x = num(v)
    if (!Number.isFinite(x)) return fail(label)
    if (x < lo || x > hi) return `「${label}」要在 ${lo}–${hi} 之间（现在是 ${x}）。`
    return ''
  }
  for (const [label, v, lo, hi] of [
    ['帧率', form.fps, 1, 120],
    ['码率', form.bitrate_kbps, 100, 100000],
    ['高度上限', form.max_height, 0, 4320],
    ['编码线程', form.threads, 0, 128],
    ['关键帧间隔', form.gop_seconds, 1, 60],
    ['子码流高度', form.sub_height, 120, 2160],
    ['子码流码率', form.sub_bitrate_kbps, 32, 100000],
    ['底板不透明度', form.osd.plate_alpha, 0, 255],
  ] as const) {
    const e = range(label, v, lo, hi)
    if (e) return e
  }
  for (const it of form.osd.items) {
    for (const [label, v, lo, hi] of [
      ['水印字号', it.font_size, 0, 512],
      ['水印 X', it.x_pct, 0, 100],
      ['水印 Y', it.y_pct, 0, 100],
    ] as const) {
      const e = range(label, v, lo, hi)
      if (e) return e
    }
  }
  return ''
}

async function flush() {
  if (applying.value) {
    queued = true
    return
  }
  if (!config.value) return
  if (!isLocal.value && !cfgItem.value) return

  const invalid = checkForm()
  if (invalid) {
    // 不应用、也不清 pending：用户改完这个数之后 `touch()` 会再排一次。
    applyError.value = invalid
    return
  }

  applying.value = true
  applyError.value = ''
  const media = pendingMedia
  pendingMedia = false
  const seq = dirty
  const patch = buildPatch()

  const out = media ? await applyMedia(patch) : await save(patch)
  applying.value = false

  if (!out.ok) {
    applyError.value = out.error ?? '应用失败。'
  } else {
    applyNote.value = media
      ? out.note ?? '已应用，管线正在重开。'
      : '已生效（下一帧就变，不用重启）'
    window.setTimeout(() => {
      if (!applying.value) applyNote.value = ''
    }, 2600)
    // 没有新的本地改动时才抄回权威值（后端会夹取越界值、展开档位）。
    if (dirty === seq) syncFromConfig()
  }

  if (queued) {
    // **不要在这里无条件把 `pendingMedia` 置 true**：排队的那次可能只是
    // 拖了一下水印，而它不该触发一次管线重开。`touch()` 已经按改动类型设过了。
    queued = false
    void flush()
  }
}

// ---------------------------------------------------------------------------
// 档位表（从后端来；没有时退回同一组字面量）
// ---------------------------------------------------------------------------
const qualityTiers = computed<QualityTier[]>(() => {
  const t = config.value?.quality_tiers
  if (t && t.length > 0) return t
  return [
    { key: 'high', label: '高性能', fps: 30, bitrate_kbps: 8192, max_height: 0, sub_enabled: true },
    { key: 'balanced', label: '均衡', fps: 25, bitrate_kbps: 4096, max_height: 0, sub_enabled: true },
    { key: 'smooth', label: '流畅', fps: 20, bitrate_kbps: 1024, max_height: 1080, sub_enabled: true },
    { key: 'verylow', label: '极低', fps: 15, bitrate_kbps: 512, max_height: 720, sub_enabled: true },
  ]
})

const latencyTiers = computed<LatencyTier[]>(() => {
  const t = config.value?.latency_tiers
  if (t && t.length > 0) return t
  return [
    { key: 'realtime', label: '极致低延迟', threads: 1, gop_seconds: 1 },
    { key: 'balanced', label: '均衡', threads: 3, gop_seconds: 2 },
    { key: 'quality', label: '优先画质', threads: 6, gop_seconds: 4 },
  ]
})

function pickQuality(t: QualityTier) {
  form.quality = t.key
  form.fps = t.fps
  form.bitrate_kbps = t.bitrate_kbps
  form.max_height = t.max_height
  form.sub_enabled = t.sub_enabled
  touch(true)
}

function pickLatency(t: LatencyTier) {
  form.latency = t.key
  form.threads = t.threads
  form.gop_seconds = t.gop_seconds
  touch(true)
}

/** 手改任何具体数值 → 档位标签翻成 custom（否则后端会按档位再覆盖回去）。 */
function markQualityCustom() {
  form.quality = 'custom'
  touch(true)
}
function markLatencyCustom() {
  form.latency = 'custom'
  touch(true)
}

// ---------------------------------------------------------------------------
// 水印
// ---------------------------------------------------------------------------
const POSITIONS: { key: string; label: string }[] = [
  { key: 'upper_left', label: '左上' },
  { key: 'upper_right', label: '右上' },
  { key: 'lower_left', label: '左下' },
  { key: 'lower_right', label: '右下' },
  { key: 'custom', label: '自定义（百分比）' },
]
const TYPES: { key: string; label: string }[] = [
  { key: 'time', label: '日期时间' },
  { key: 'name', label: '通道名称' },
  { key: 'text', label: '固定文字' },
]

const MAX_OSD_ITEMS = 4

function addOsdItem() {
  if (form.osd.items.length >= MAX_OSD_ITEMS) return
  const one: OsdItemConfig = {
    enabled: true,
    type: 'text',
    text: '',
    position: 'upper_left',
    x_pct: 0,
    y_pct: 0,
    font_size: 0,
    date_format: '%Y-%m-%d',
    time_format: '%H:%M:%S',
  }
  form.osd.items.push(one)
  touch(false)
}
function removeOsdItem(i: number) {
  form.osd.items.splice(i, 1)
  touch(false)
}
function touchOsd() {
  touch(false)
}

// ---------------------------------------------------------------------------
// 快照
// ---------------------------------------------------------------------------
const SNAP_MS = 2000
const tick = ref(0)
let snapTimer = 0
const snapBroken = ref(false)

const snapUrl = computed(
  () => `${channelSnapshotUrl(n.value)}&v=${tick.value}`,
)

onMounted(async () => {
  await load()
  syncFromConfig()
  tick.value = Date.now()
  snapTimer = window.setInterval(() => {
    tick.value = Date.now()
    // 被 `v-if` 摘掉的 `<img>` 不会自己重试（见 ChannelsView 同一段注释）。
    snapBroken.value = false
  }, SNAP_MS)
})

onBeforeUnmount(() => {
  if (snapTimer) window.clearInterval(snapTimer)
  if (timer) window.clearTimeout(timer)
})

// 在同一页里从 /channels/2 跳到 /channels/3：重新抄一遍配置。
watch(n, () => {
  // **取消还在防抖里的那一次应用**：它带着上一路的表单值，而 `flush()` 会用
  // 当前的 `n` 去拼补丁 —— 那等于把通道 2 的帧率写到通道 3 上。
  if (timer) {
    window.clearTimeout(timer)
    timer = 0
  }
  pendingMedia = false
  dirty = 0
  applyError.value = ''
  syncFromConfig()
  snapBroken.value = false
})

async function openLive() {
  await setPreviewChannel(n.value)
  router.push('/')
}
</script>

<template>
  <section class="page-head">
    <div>
      <button class="ghost back" type="button" @click="router.push('/channels')">
        <Icon name="chevron-left" :size="15" />通道管理
      </button>
      <h1>
        <span class="channel-chip">CH {{ n }}</span>
        {{ displayName }}
      </h1>
      <p class="sub">
        <span class="state-pill" :class="`is-${stateTone}`">{{ stateText }}</span>
        <span class="muted"> · 改动自动生效，不需要保存</span>
      </p>
    </div>
    <div class="head-actions">
      <span v-if="applying" class="pill is-connecting"><Icon name="loader" :size="14" />正在应用…</span>
      <span v-else-if="applyNote" class="pill is-online"><Icon name="check" :size="14" />{{ applyNote }}</span>
      <button class="ghost" type="button" @click="openLive">
        <Icon name="play" :size="15" />打开实时预览
      </button>
    </div>
  </section>

  <div v-if="pageError" class="banner is-err"><strong>找不到这一路。</strong>{{ pageError }}</div>
  <div v-if="applyError" class="banner is-err"><strong>应用失败。</strong>{{ applyError }}</div>

  <div v-if="config" class="detail-layout">
    <!-- 左栏：身份 + 快照 -->
    <aside class="detail-side">
      <div class="panel">
        <h2><Icon name="info" :size="16" />身份</h2>
        <dl class="kv">
          <div><dt>通道号</dt><dd class="mono">{{ n }}</dd></div>
          <div>
            <dt>通道 ID</dt>
            <dd class="mono break">{{ isLocal ? (liveRow?.id || 'machine:本机') : (cfgItem?.id || '未分配') }}</dd>
          </div>
          <div>
            <dt>硬件序列号</dt>
            <dd class="mono break">
              {{ liveRow?.hardware_serial || cfgItem?.hardware_serial || '未提供' }}
            </dd>
          </div>
          <div><dt>来源</dt><dd>{{ isLocal ? '本机桌面（主屏）' : (cfgItem?.source === 'webcam' ? 'USB 摄像头' : '显示器采集') }}</dd></div>
          <div><dt>ONVIF 端口</dt><dd class="mono">{{ onvifPort || '—' }}</dd></div>
          <div><dt>RTSP 地址</dt><dd class="mono break">{{ rtspUrl || '未开启' }}</dd></div>
          <div>
            <dt>NVR 地址</dt>
            <dd class="mono break">http://{{ localHost }}:{{ onvifPort }}/onvif/device_service</dd>
          </div>
        </dl>
        <label v-if="!isLocal" class="field" style="margin-top: 12px">
          <span>通道名称</span>
          <input v-model="form.name" type="text" maxlength="48" @input="touch(true)" />
          <small>改名会同步到 ONVIF 设备名（重开管线后生效）</small>
        </label>

        <p v-if="!isLocal" class="hint-block">
          这一路绑定的是添加时那块硬件。设备被拔掉时它会显示「无信号」并每 3 秒
          重试一次，插回来自动接上。
        </p>
      </div>

      <div class="panel snapshot-panel">
        <h2><Icon name="eye" :size="16" />当前画面</h2>
        <div class="snapshot-stage">
          <!-- 来源掉线时**不画那张冻住的最后一帧**：采集会话会一直推它，
               所以图是有的，但显示出来等于告诉用户"画面还在"。 -->
          <img
            v-if="!snapBroken && !stateText.startsWith('无信号')"
            :src="snapUrl"
            :alt="displayName"
            @error="snapBroken = true"
            @load="snapBroken = false"
          />
          <div v-else class="no-signal">
            <Icon name="eye-off" :size="30" />
            <strong>无画面</strong>
            <span class="muted">{{ stateText }}</span>
          </div>
        </div>
        <button class="ghost wide" type="button" @click="openLive">
          <Icon name="play" :size="15" />切到这一路的实时预览
        </button>
        <a class="btn wide" :href="snapUrl" :download="`channel-${n}.jpg`" style="margin-top: 8px">
          <Icon name="image" :size="15" />保存一张截图
        </a>
      </div>
    </aside>

    <!-- 右栏：参数 -->
    <div class="detail-main">
      <section class="panel">
        <header class="panel-head">
          <h2><Icon name="monitor" :size="16" />主码流</h2>
          <!-- 说实话：现在重开的是整条管线（共享的 RTSP 服务器 + 所有通道），
               不是单独这一路。写成"只断这一路"会让用户在别的通道黑一下时
               以为出了故障。 -->
          <span class="hint">改完自动保存并重开管线（所有通道断流约 1–2 秒）</span>
        </header>

        <div class="tier-grid">
          <button
            v-for="t in qualityTiers"
            :key="t.key"
            type="button"
            class="tier-card"
            :class="{ 'is-on': form.quality === t.key }"
            @click="pickQuality(t)"
          >
            <strong>{{ t.label }}</strong>
            <span>{{ t.fps }} fps · {{ t.bitrate_kbps }} kbps</span>
            <span class="muted">
              {{ t.max_height === 0 ? '原尺寸' : '≤ ' + t.max_height + 'p' }}
              · {{ t.sub_enabled ? '带子码流' : '无子码流' }}
            </span>
          </button>
        </div>
        <p v-if="form.quality === 'custom'" class="hint">当前是自定义参数（手动改过具体值）。</p>

        <div class="field-row">
          <label class="field">
            <span>帧率（fps）</span>
            <input
              v-model.number="form.fps"
              type="number"
              min="1"
              max="120"
              @input="markQualityCustom"
            />
          </label>
          <label class="field">
            <span>码率（kbps）</span>
            <input
              v-model.number="form.bitrate_kbps"
              type="number"
              min="100"
              max="100000"
              step="100"
              @input="markQualityCustom"
            />
          </label>
          <label class="field">
            <span>高度上限</span>
            <select v-model.number="form.max_height" @change="markQualityCustom">
              <option :value="0">源的原生尺寸（不缩放）</option>
              <option :value="2160">2160p</option>
              <option :value="1440">1440p</option>
              <option :value="1080">1080p</option>
              <option :value="720">720p</option>
              <option :value="480">480p</option>
            </select>
          </label>
        </div>

        <header class="panel-head sub-head">
          <h3><Icon name="zap" :size="15" />延时</h3>
          <span class="hint">线程数 ≈ 流水线深度；关键帧间隔决定新观众多久出画</span>
        </header>
        <div class="tier-grid narrow">
          <button
            v-for="t in latencyTiers"
            :key="t.key"
            type="button"
            class="tier-card"
            :class="{ 'is-on': form.latency === t.key }"
            @click="pickLatency(t)"
          >
            <strong>{{ t.label }}</strong>
            <span>{{ t.threads }} 线程 · GOP {{ t.gop_seconds }}s</span>
          </button>
        </div>
        <div class="field-row">
          <label class="field">
            <span>编码线程</span>
            <input
              v-model.number="form.threads"
              type="number"
              min="0"
              max="128"
              @input="markLatencyCustom"
            />
          </label>
          <label class="field">
            <span>关键帧间隔（秒）</span>
            <input
              v-model.number="form.gop_seconds"
              type="number"
              min="1"
              max="60"
              @input="markLatencyCustom"
            />
          </label>
        </div>
        <p v-if="form.latency === 'custom'" class="hint">
          当前是自定义延时参数（手动改过线程数或关键帧间隔）。
        </p>
      </section>

      <section class="panel">
        <header class="panel-head">
          <h2><Icon name="camera" :size="16" />子码流</h2>
          <label class="toggle">
            <input v-model="form.sub_enabled" type="checkbox" @change="touch(true)" />
            <span>启用子码流（NVR 多画面网格用）</span>
          </label>
        </header>
        <div class="field-row">
          <label class="field" :class="{ 'is-muted': !form.sub_enabled }">
            <span>高度</span>
            <select v-model.number="form.sub_height" :disabled="!form.sub_enabled" @change="touch(true)">
              <option :value="720">720p</option>
              <option :value="480">480p</option>
              <option :value="360">360p</option>
              <option :value="288">288p</option>
              <option :value="240">240p</option>
            </select>
          </label>
          <label class="field" :class="{ 'is-muted': !form.sub_enabled }">
            <span>码率（kbps）</span>
            <input
              v-model.number="form.sub_bitrate_kbps"
              type="number"
              min="32"
              max="100000"
              step="50"
              :disabled="!form.sub_enabled"
              @input="touch(true)"
            />
          </label>
        </div>
      </section>

      <section class="panel">
        <header class="panel-head">
          <h2><Icon name="image" :size="16" />水印 OSD</h2>
          <span class="hint">热生效：下一帧就变，不重开管线</span>
        </header>

        <div class="field-row wrap">
          <label class="toggle">
            <input v-model="form.osd.enabled" type="checkbox" @change="touchOsd" />
            <span>启用水印</span>
          </label>
          <label class="toggle">
            <input v-model="form.osd.adaptive_color" type="checkbox" @change="touchOsd" />
            <span>自适应配色</span>
          </label>
          <label class="toggle">
            <input v-model="form.osd.outline" type="checkbox" @change="touchOsd" />
            <span>反色描边</span>
          </label>
          <label class="field inline">
            <span>底板不透明度</span>
            <input
              v-model.number="form.osd.plate_alpha"
              type="number"
              min="0"
              max="255"
              @input="touchOsd"
            />
          </label>
        </div>

        <div v-for="(it, i) in form.osd.items" :key="i" class="osd-item">
          <header class="osd-item-head">
            <strong>第 {{ i + 1 }} 项</strong>
            <label class="toggle">
              <input v-model="it.enabled" type="checkbox" @change="touchOsd" />
              <span>显示</span>
            </label>
            <button class="ghost is-danger" type="button" @click="removeOsdItem(i)">
              <Icon name="x" :size="14" />移除
            </button>
          </header>

          <div class="field-row wrap">
            <label class="field">
              <span>类型</span>
              <select v-model="it.type" @change="touchOsd">
                <option v-for="t in TYPES" :key="t.key" :value="t.key">{{ t.label }}</option>
              </select>
            </label>
            <label v-if="it.type === 'text'" class="field grow">
              <span>文字</span>
              <input v-model="it.text" type="text" maxlength="64" @input="touchOsd" />
            </label>
            <label class="field">
              <span>位置</span>
              <select v-model="it.position" @change="touchOsd">
                <option v-for="p in POSITIONS" :key="p.key" :value="p.key">{{ p.label }}</option>
              </select>
            </label>
            <label class="field">
              <span>字号（0 = 自动）</span>
              <input
                v-model.number="it.font_size"
                type="number"
                min="0"
                max="512"
                @input="touchOsd"
              />
            </label>
          </div>

          <div v-if="it.position === 'custom'" class="field-row">
            <label class="field">
              <span>左上角 X（%）</span>
              <input v-model.number="it.x_pct" type="number" min="0" max="100" @input="touchOsd" />
            </label>
            <label class="field">
              <span>左上角 Y（%）</span>
              <input v-model.number="it.y_pct" type="number" min="0" max="100" @input="touchOsd" />
            </label>
          </div>

          <div v-if="it.type === 'time'" class="field-row">
            <label class="field">
              <span>日期格式</span>
              <input v-model="it.date_format" type="text" maxlength="32" @input="touchOsd" />
            </label>
            <label class="field">
              <span>时间格式</span>
              <input v-model="it.time_format" type="text" maxlength="32" @input="touchOsd" />
            </label>
          </div>
        </div>

        <button
          class="ghost"
          type="button"
          :disabled="form.osd.items.length >= MAX_OSD_ITEMS"
          @click="addOsdItem"
        >
          <Icon name="plus" :size="15" />添加一项
          <span class="muted">（最多 {{ MAX_OSD_ITEMS }} 项）</span>
        </button>
      </section>
    </div>
  </div>

  <div v-else-if="!pageError" class="empty-state">
    <Icon name="loader" :size="26" />
    <span>正在读取配置…</span>
  </div>
</template>
