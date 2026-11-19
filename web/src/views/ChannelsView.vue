<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import Icon from '../components/Icon.vue'
import { channelSnapshotUrl, fetchChannels, fetchSources } from '../api'
import { useConfig } from '../composables/useConfig'
import { useStats } from '../composables/useStats'
import { formatInt } from '../format'
import type { ChannelConfig, ChannelStat, Sources } from '../types'

/**
 * 通道管理：NVR 式的"有哪几路"。
 *
 * 三件事：看（每路的状态 + 缩略图）、加（从显示器/摄像头里挑一个来源）、
 * 改（启用/停用、删除、点进详情）。**默认安装只有通道 1**（本机桌面），
 * 其余每一路都是用户在这里显式加出来的 —— 后端不再自动发现显示器。
 */

const router = useRouter()
const { stats } = useStats()
const { config, load: loadConfig, applyMedia, items, replaceChannel } = useConfig()

const pageError = ref('')
/** `/api/channels` 的首屏快照（WS 还没连上时用它，避免先闪一下空列表）。 */
const bootChannels = ref<ChannelStat[]>([])

onMounted(async () => {
  await loadConfig()
  const res = await fetchChannels()
  if (res.ok && Array.isArray(res.data.channels)) bootChannels.value = res.data.channels
  tick.value = Date.now()
  timer = window.setInterval(() => {
    tick.value = Date.now()
    // **每一拍重试一次**：缩略图 404 时 `noSignal` 被置位，而 `v-if` 会因为
    // 它把 `<img>` 摘掉 —— 不重置的话那一格再也不会重新请求，设备插回来了
    // 也还是「无画面」。状态文字由 `/ws/stats` 单独更新。
    noSignal.value = {}
  }, SNAPSHOT_PERIOD_MS)
})
onBeforeUnmount(() => {
  if (timer) window.clearInterval(timer)
})

/** 缩略图多久刷新一次。3 秒：比这更勤会把后端编码器拖成热点。 */
const SNAPSHOT_PERIOD_MS = 3000
const tick = ref(0)
let timer = 0

/** 这一路此刻有没有一张能显示的图。加载失败（404 = 没帧）就把它记下来。 */
const noSignal = ref<Record<number, boolean>>({})

function snapUrl(n: number): string {
  // `tick` 进 URL：img 的 src 不变时浏览器会一直用缓存里那张。
  return `${channelSnapshotUrl(n)}&v=${tick.value}`
}
function onSnapLoad(n: number) {
  if (noSignal.value[n]) noSignal.value = { ...noSignal.value, [n]: false }
}
function onSnapError(n: number) {
  noSignal.value = { ...noSignal.value, [n]: true }
}

/**
 * 这一格该不该画「无画面」。
 *
 * 两个来源缺一不可：缩略图 404（这一路没有帧），以及后端状态说来源掉线
 * （采集会话会**继续推最后一帧**，所以图是有的，但它已经冻住了 —— 那比
 * 一张空图更容易被误读成"画面正常"）。
 */
function isNoSignal(r: ChannelStat): boolean {
  const n = r.number ?? r.index
  return noSignal.value[n] === true || r.state.startsWith('无信号')
}

/**
 * 列表来源：WS 每秒推的那份为准；还没连上时从配置里拼一份骨架。
 *
 * 用配置拼的那份**没有实测 fps**，所以那些格子显示 "—" 而不是 0 ——
 * 0 会被读成"跑起来了但一帧都没有"。
 */
const rows = computed<ChannelStat[]>(() => {
  // **按通道号排序**，不是配置数组顺序：新加的一路拿到的是最小空号，可能
  // 比已有通道小，而 NVR 的界面是按号排的。后端那条数组只保证"有哪几路"。
  const byNumber = (a: ChannelStat, b: ChannelStat) =>
    (a.number ?? a.index) - (b.number ?? b.index)
  const live = stats.value.channels ?? []
  if (live.length > 0) return [...live].sort(byNumber)
  if (bootChannels.value.length > 0) return [...bootChannels.value].sort(byNumber)
  const out: ChannelStat[] = [
    {
      index: 1,
      number: 1,
      name: '本地桌面',
      source: 'display',
      state: '连接中…',
      fps: 0,
      bitrate_kbps: 0,
      record_file: '',
      record_bytes: 0,
      web_port: stats.value.web_port || 0,
      rtsp_path: config.value?.rtsp.path ?? '',
      rtsp_sub_path: config.value?.rtsp.sub_path ?? '',
      enabled: true,
      hardware_serial: '',
    },
  ]
  for (const c of items()) {
    out.push({
      index: c.number ?? 0,
      number: c.number ?? 0,
      name: c.name || `通道 ${c.number ?? ''}`,
      source: c.source,
      state: c.enabled ? '连接中…' : '已关闭',
      fps: 0,
      bitrate_kbps: 0,
      record_file: '',
      record_bytes: 0,
      web_port: (stats.value.web_port || 0) + (c.number ?? 0),
      rtsp_path: c.rtsp_path,
      rtsp_sub_path: c.rtsp_sub_path,
      enabled: c.enabled,
      hardware_id: c.hardware_id,
      hardware_serial: c.hardware_serial,
    })
  }
  return out
})

function sourceLabel(r: ChannelStat): string {
  return r.source === 'webcam' ? 'USB 摄像头' : '显示器采集'
}

/** 状态归类，只用来选颜色；文字仍然是后端给的那一整句。 */
function stateTone(state: string): 'ok' | 'bad' | 'idle' | 'wait' {
  if (state.startsWith('无信号')) return 'wait'
  if (state.startsWith('失败') || state.startsWith('未启用')) return 'bad'
  if (state === '运行中' || state === '采集中') return 'ok'
  return 'idle'
}

function goDetail(n: number) {
  router.push(`/channels/${n}`)
}

// ---------------------------------------------------------------------------
// 启用 / 停用 / 删除（都是"点一下立刻生效"）
// ---------------------------------------------------------------------------
const busy = ref<number | null>(null)
const actionError = ref('')
/** 待确认删除的通道号。**不用 window.confirm** —— 它在某些环境里会被静默吃掉。 */
const confirmDelete = ref<number | null>(null)

async function toggleEnabled(r: ChannelStat) {
  const n = r.number ?? r.index
  if (n === 1) return
  const c = items().find((x) => (x.number ?? 0) === n)
  if (!c) return
  busy.value = n
  actionError.value = ''
  const next: ChannelConfig = { ...c, enabled: !c.enabled }
  const out = await applyMedia({ channels: { items: replaceChannel(n, next) } })
  busy.value = null
  if (!out.ok) actionError.value = out.error ?? '操作失败。'
}

async function removeChannel(r: ChannelStat) {
  const n = r.number ?? r.index
  if (n === 1) return
  if (confirmDelete.value !== n) {
    confirmDelete.value = n
    return
  }
  confirmDelete.value = null
  busy.value = n
  actionError.value = ''
  const out = await applyMedia({
    channels: { items: items().filter((c) => (c.number ?? 0) !== n) },
  })
  busy.value = null
  if (!out.ok) actionError.value = out.error ?? '删除失败。'
}

// ---------------------------------------------------------------------------
// 添加通道
// ---------------------------------------------------------------------------
const addOpen = ref(false)
const sources = ref<Sources | null>(null)
const sourcesLoading = ref(false)
const sourcesError = ref('')
const kind = ref<'camera' | 'display'>('camera')
const pickedCamera = ref<number>(-1)
const pickedDisplay = ref<number>(-1)
const newName = ref('')
const adding = ref(false)
const addError = ref('')

async function openAdd() {
  addOpen.value = true
  addError.value = ''
  // **每次打开都现枚举一次摄像头**：后端对摄像头是逐请求枚举的（只有显示器
  // 那份是启动快照）。缓存住的话，刚插上的 USB 摄像头要重启页面才能看到 ——
  // 而这正是"插上就能加"最常见的失败方式。
  sourcesLoading.value = true
  sourcesError.value = ''
  const res = await fetchSources()
  sourcesLoading.value = false
  if (!res.ok) {
    sourcesError.value = res.error
    return
  }
  sources.value = res.data
  // 默认选第一台**还没被占用**的摄像头；没有就切到第一块能抓且没被占用的屏。
  const freeCam = res.data.cameras.findIndex((c) => cameraUsedBy(c) === null)
  if (freeCam >= 0) {
    kind.value = 'camera'
    pickedCamera.value = freeCam
  } else {
    kind.value = 'display'
    const d = res.data.displays.find(
      (x) => x.capturable && displayUsedBy(x) === null,
    )
    pickedDisplay.value = d ? d.index : -1
  }
}

const cameraList = computed(() => sources.value?.cameras ?? [])
const displayList = computed(() => sources.value?.displays ?? [])

/**
 * 已经被别的通道占用的来源。
 *
 * 一个 USB 摄像头同时只能被一个 reader 打开；一块屏同时只能有一个 DXGI
 * duplication。让用户重复添加的结果是"新加的这一路永远黑着"，而错误要到
 * 采集线程里才出现 —— 所以在选择器里就挡住，并说清是被哪一路占了。
 */
const channel1 = computed(
  () => (stats.value.channels ?? []).find((c) => (c.number ?? c.index) === 1),
)
const usedCameras = computed(() => {
  const m = new Map<string, number>()
  for (const c of items()) {
    if (c.hardware_id) m.set(c.hardware_id, c.number ?? 0)
  }
  return m
})
const usedDisplays = computed(() => {
  const m = new Map<number, number>()
  for (const c of items()) {
    if (c.source === 'display') m.set(c.display_index, c.number ?? 0)
  }
  // 通道 1 也在抓屏时占着一块。`source` 只有 `/api/status` 知道（配置里
  // 没有"通道 1 用的是什么源"这一项）。
  if (config.value && channel1.value?.source !== 'webcam') {
    m.set(config.value.capture.display_index, 1)
  }
  return m
})

function cameraUsedBy(c: { hardware_id?: string }): number | null {
  if (!c.hardware_id) return null
  return usedCameras.value.get(c.hardware_id) ?? null
}
function displayUsedBy(d: { index: number }): number | null {
  return usedDisplays.value.get(d.index) ?? null
}

const canAdd = computed(() => {
  if (kind.value === 'camera') {
    const c = cameraList.value[pickedCamera.value]
    return !!c && cameraUsedBy(c) === null
  }
  const d = displayList.value.find((x) => x.index === pickedDisplay.value)
  return !!d && d.capturable && displayUsedBy(d) === null
})

async function addChannel() {
  if (!canAdd.value || adding.value) return
  addError.value = ''
  adding.value = true

  let source = 'display'
  let displayIndex = 0
  let camera = ''
  let hardwareId = ''
  let hardwareSerial = ''
  if (kind.value === 'camera') {
    const c = cameraList.value[pickedCamera.value]
    source = 'webcam'
    // **存符号链接，不存友好名**：两台同型号摄像头的名字可能一模一样，
    // 而符号链接能把它们分开。没给链接时退回名字（老后端没有这个字段）。
    camera = c.hardware_id || c.name
    hardwareId = c.hardware_id || c.name
    hardwareSerial = c.hardware_serial || ''
  } else {
    const d = displayList.value.find((x) => x.index === pickedDisplay.value)!
    source = 'display'
    displayIndex = d.index
    hardwareId = d.hardware_id || `display:#${d.index}`
    hardwareSerial = d.hardware_serial || ''
  }

  const one: ChannelConfig = {
    enabled: true,
    // **0 = 让后端分配最小的空号。** 前端算号会和后端的占用规则打架。
    number: 0,
    name: newName.value.trim(),
    source,
    display_index: displayIndex,
    camera,
    hardware_id: hardwareId,
    hardware_serial: hardwareSerial,
    // 新通道默认「流畅」：NVR 的多画面网格多半用它，按「均衡」开会把 CPU 吃光。
    quality: 'smooth',
    fps: 20,
    bitrate_kbps: 1024,
    max_height: 1080,
    latency: 'balanced',
    gop_seconds: 2,
    threads: 3,
    sub_enabled: true,
    sub_height: 360,
    sub_bitrate_kbps: 512,
    rtsp_path: '',
    rtsp_sub_path: '',
  }

  const out = await applyMedia({ channels: { items: [...items(), one] } })
  adding.value = false
  if (!out.ok) {
    addError.value = out.error ?? '添加失败。'
    return
  }
  // 后端分配了号：回读配置，按硬件标识找回刚加的那一项。
  await loadConfig(true)
  const created = items()
    .filter((c) => (c.hardware_id || '') === hardwareId)
    .sort((a, b) => (b.number ?? 0) - (a.number ?? 0))[0]
  addOpen.value = false
  newName.value = ''
  if (created && (created.number ?? 0) > 0) goDetail(created.number as number)
}
</script>

<template>
  <section class="page-head">
    <div>
      <h1>通道管理</h1>
      <p class="sub">
        每一路通道绑定一个来源（本机桌面 / 显示器 / USB 摄像头）。新加的通道按
        <strong>稳定通道号</strong>分配端口与 RTSP 路径，删掉别的通道不会让它换号。
      </p>
    </div>
    <button class="primary" type="button" @click="openAdd">
      <Icon name="plus" :size="16" />添加通道
    </button>
  </section>

  <div v-if="pageError" class="banner is-err"><strong>操作失败。</strong>{{ pageError }}</div>
  <div v-if="actionError" class="banner is-err"><strong>操作失败。</strong>{{ actionError }}</div>

  <div class="channel-grid">
    <article
      v-for="r in rows"
      :key="r.number ?? r.index"
      class="channel-card"
      :class="{ 'is-off': r.enabled === false }"
    >
      <div class="channel-thumb" @click="goDetail(r.number ?? r.index)">
        <img
          v-if="!isNoSignal(r)"
          :src="snapUrl(r.number ?? r.index)"
          :alt="r.name"
          loading="lazy"
          @load="onSnapLoad(r.number ?? r.index)"
          @error="onSnapError(r.number ?? r.index)"
        />
        <div v-else class="no-signal">
          <Icon name="eye-off" :size="26" />
          <span>无画面</span>
        </div>
        <span class="channel-no">CH {{ r.number ?? r.index }}</span>
        <span class="state-pill" :class="`is-${stateTone(r.state)}`">{{ r.state }}</span>
      </div>

      <div class="channel-body">
        <div class="channel-title">
          <h3>{{ r.name }}</h3>
          <span class="tag">{{ sourceLabel(r) }}</span>
        </div>

        <dl class="kv compact">
          <div>
            <dt>序列号</dt>
            <dd class="mono">{{ r.hardware_serial || '未提供' }}</dd>
          </div>
          <div>
            <dt>实测</dt>
            <dd>
              <template v-if="stateTone(r.state) === 'ok'">
                {{ formatInt(r.fps) }} fps · {{ formatInt(r.bitrate_kbps) }} kbps
              </template>
              <template v-else>—</template>
            </dd>
          </div>
          <div>
            <dt>ONVIF 端口</dt>
            <dd class="mono">{{ r.web_port || '—' }}</dd>
          </div>
          <div>
            <dt>RTSP</dt>
            <dd class="mono truncate" :title="r.rtsp_path">{{ r.rtsp_path || '未开启' }}</dd>
          </div>
        </dl>

        <div class="channel-actions">
          <button class="ghost" type="button" @click="goDetail(r.number ?? r.index)">
            <Icon name="sliders" :size="15" />图像与设置
          </button>
          <template v-if="(r.number ?? r.index) !== 1">
            <button
              class="ghost"
              type="button"
              :disabled="busy === (r.number ?? r.index)"
              @click="toggleEnabled(r)"
            >
              <Icon :name="r.enabled === false ? 'play' : 'pause'" :size="15" />
              {{ r.enabled === false ? '启用' : '停用' }}
            </button>
            <button
              class="ghost is-danger"
              type="button"
              :disabled="busy === (r.number ?? r.index)"
              @click="removeChannel(r)"
            >
              <Icon name="x" :size="15" />
              {{ confirmDelete === (r.number ?? r.index) ? '确认删除' : '删除' }}
            </button>
          </template>
          <span v-else class="hint">通道 1 是本机桌面，不可删除</span>
        </div>
      </div>
    </article>
  </div>

  <!-- 添加通道 -->
  <div v-if="addOpen" class="modal-scrim" @click.self="addOpen = false">
    <div class="modal" role="dialog" aria-modal="true" aria-label="添加通道">
      <header class="modal-head">
        <h2>添加通道</h2>
        <button class="ghost" type="button" @click="addOpen = false">
          <Icon name="x" :size="16" />
        </button>
      </header>

      <div class="modal-body">
        <div v-if="sourcesLoading" class="muted">正在枚举本机设备…</div>
        <div v-else-if="sourcesError" class="banner is-err">
          <strong>枚举设备失败。</strong>{{ sourcesError }}
        </div>

        <template v-else>
          <div class="seg-tabs">
            <button
              type="button"
              :class="{ 'is-on': kind === 'camera' }"
              @click="kind = 'camera'"
            >
              USB 摄像头（{{ cameraList.length }}）
            </button>
            <button
              type="button"
              :class="{ 'is-on': kind === 'display' }"
              @click="kind = 'display'"
            >
              显示器（{{ displayList.length }}）
            </button>
          </div>

          <div v-if="kind === 'camera'">
            <p v-if="cameraList.length === 0" class="muted">
              这台机器上没有枚举到摄像头。插上之后关掉这个对话框再打开一次即可
              （摄像头列表是每次打开时现枚举的）。
            </p>
            <label
              v-for="(c, i) in cameraList"
              :key="c.hardware_id || c.name"
              class="source-row"
              :class="{ 'is-disabled': cameraUsedBy(c) !== null }"
            >
              <input
                v-model="pickedCamera"
                type="radio"
                :value="i"
                :disabled="cameraUsedBy(c) !== null"
              />
              <span class="source-main">
                <strong>{{ c.name }}</strong>
                <span class="mono muted">
                  序列号：{{ c.hardware_serial || '未提供' }}
                  <template v-if="c.is_default"> · 本机默认</template>
                  <template v-if="cameraUsedBy(c) !== null">
                    · 已被 CH {{ cameraUsedBy(c) }} 使用
                  </template>
                </span>
              </span>
            </label>
          </div>

          <div v-else>
            <label
              v-for="d in displayList"
              :key="d.index"
              class="source-row"
              :class="{ 'is-disabled': !d.capturable || displayUsedBy(d) !== null }"
            >
              <input
                v-model="pickedDisplay"
                type="radio"
                :value="d.index"
                :disabled="!d.capturable || displayUsedBy(d) !== null"
              />
              <span class="source-main">
                <strong>#{{ d.index }} · {{ d.name }}</strong>
                <span class="muted">
                  {{ d.width }}×{{ d.height }}
                  <template v-if="!d.capturable"> · 抓不到（间接显示驱动）</template>
                  <template v-if="displayUsedBy(d) !== null">
                    · 已被 CH {{ displayUsedBy(d) }} 使用
                  </template>
                </span>
              </span>
            </label>
          </div>

          <label class="field">
            <span>通道名称（可留空）</span>
            <input v-model="newName" type="text" placeholder="例如：大门口" maxlength="48" />
          </label>

          <div class="hint-block">
            新通道默认「流畅」档，之后可以在详情页随时改（改完点一下即生效）。
            通道号由后端分配，删掉别的通道不会让它换号。
            <br />
            NVR 的<strong>自动发现</strong>只在服务启动时广播；新加的这一路如果搜不到，
            用详情页里的 ONVIF 地址手动添加，或者重启一次服务。
          </div>

          <div v-if="addError" class="banner is-err"><strong>添加失败。</strong>{{ addError }}</div>
        </template>
      </div>

      <footer class="modal-foot">
        <button class="ghost" type="button" @click="addOpen = false">取消</button>
        <button
          class="primary"
          type="button"
          :disabled="!canAdd || adding"
          @click="addChannel"
        >
          <Icon name="plus" :size="15" />{{ adding ? '正在添加…' : '添加并应用' }}
        </button>
      </footer>
    </div>
  </div>
</template>
