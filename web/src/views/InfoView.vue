<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { useAuth } from '../composables/useAuth'
import { useStats } from '../composables/useStats'
import { fetchConfig } from '../api'
import type { AppConfig } from '../types'
import { copyText, formatBytes } from '../format'

/**
 * 设备信息（只读）。
 *
 * ## 为什么是只读页，而且**不新增接口**
 *
 * 用户要的是"这台设备是谁"——序列号、软件版本、加进 NVR 时该填什么。这些东西
 * 一共有两个来源，都已经存在：`/ws/stats`（版本号、端口、实际分辨率、RTSP 地址）
 * 与 `GET /api/config`（ONVIF 身份那一组、端口、路径）。为此单开一个
 * `/api/device-info` 只会多出一份**会漂移的**字段清单：两边各写一遍，加字段时
 * 漏一处，症状是"配置页改了型号、设备信息页还是旧的"，而两处看起来都对。
 *
 * ## 这里显示的尺寸是**实际编码尺寸**，不是配置值
 *
 * `stats.video_width/height` 是编码器真正在推的那个尺寸。配置里的
 * `capture.max_height` 是一个**上限**（0 = 桌面原生），桌面比上限小的时候并不放大
 * —— 所以拿配置值回答"现在推的是多大的画面"会答错，而错的那一侧看起来同样合理。
 * 未开始采集时这一对是 0，界面上说"未知"而不是编一个尺寸出来。
 *
 * ## 还没有的两行
 *
 * 计划里这一页还要有「设备时间」（要 `GET /api/time`，Phase U 才做）与
 * 「操作日志」（要 Phase V）。这里**故意不放占位行**：指向一个还不存在的接口，
 * 界面上要么永远是空白、要么得编一个值，两者都比没有更坏 —— 与 Phase N 定下的
 * 「菜单不指向还不存在的东西」是同一条纪律。它们跟着各自的阶段一起进来。
 */

const { stats } = useStats()
const { authRequired, user } = useAuth()

const cfg = ref<AppConfig | null>(null)
const loading = ref(true)
const loadError = ref('')

async function load(): Promise<void> {
  loading.value = true
  loadError.value = ''
  const res = await fetchConfig()
  if (res.ok) {
    cfg.value = res.data
  } else {
    // 这一页上有一半的行来自配置。读不到就说清楚是哪一半缺了 ——
    // 只把空行留在那里的话，看起来像"这台设备没有序列号"。
    loadError.value = res.error
  }
  loading.value = false
}

onMounted(load)

/** 空串显示成 `—`：`code.value` 是个有底色的方块，空着看起来像渲染坏了。 */
function show(v: string | undefined): string {
  return v && v.trim() ? v : '—'
}

// ---- 设备地址 ---------------------------------------------------------------
//
// 主机名**从 RTSP 推流地址里取**：那是后端自己选定的网卡地址（Live555 报的
// 真实局域网 IP），正是 NVR 要填的那一个。没开推流时退回浏览器当前的主机名
// —— 那只在"从这台机器本机打开"时等于设备地址，所以下面写了一句提醒。

const deviceHost = computed(() => {
  const m = /^rtsp:\/\/([^/:]+)/.exec(stats.value.rtsp_url)
  if (m) return m[1]
  return location.hostname || '127.0.0.1'
})

/** 后端实际绑定的端口优先（命令行可能覆盖过配置）。 */
const webPort = computed(() => stats.value.web_port || cfg.value?.web.port || 0)

const webBase = computed(
  () => `http://${deviceHost.value}${webPort.value ? ':' + webPort.value : ''}`,
)

/**
 * ONVIF 与网页**共用一个端口**：SOAP 挂在 `<同一个 http 端口>/onvif/service` 上，
 * 所以这里拼出来的地址与 `http.web.port` 是同一个数。NVR 手工添加通道时填的就是它。
 */
const onvifUrl = computed(() => `${webBase.value}/onvif/device_service`)

/**
 * 一路 RTSP 地址。优先用后端报的（那是 Live555 生成的、一定是真的），
 * 拿不到时按配置拼一份 —— 后端刚起来、WS 还没推第一帧时就是这个情况。
 */
function rtspUrl(index: number, path: string): string {
  const s = stats.value.rtsp_streams[index]
  if (s && s.url) return s.url
  const r = cfg.value?.rtsp
  if (!r || !r.enabled) return ''
  return `rtsp://${deviceHost.value}:${r.port}/${path}`
}

const mainRtsp = computed(() => rtspUrl(0, cfg.value?.rtsp.path ?? 'live'))
const subRtsp = computed(() => {
  const r = cfg.value?.rtsp
  // 子码流关着的时候**不显示**这一行：给一个 NVR 拉不到的地址，比不给更坏。
  if (r && !r.sub_enabled) return ''
  return rtspUrl(1, r?.sub_path ?? 'live_sub')
})

// ---- 各行 -------------------------------------------------------------------

const identityRows = computed(() => {
  const o = cfg.value?.onvif
  if (!o) return []
  return [
    { label: '制造商', value: o.manufacturer, note: '', frozen: false },
    { label: '型号', value: o.model, note: '', frozen: false },
    {
      label: '固件版本',
      value: o.firmware_version,
      // 这一页上**有两个"版本"**：这一行是配置里那个可改的值（NVR 上显示的就是
      // 它），而本页顶部那个 version 是程序真实版本、只读。两个数摆在一起不
      // 解释，会让人以为其中一个坏了。
      note: 'NVR 上显示的就是这个值；顶部那个 version 是程序真实版本',
      frozen: false,
    },
    {
      label: '硬件 ID',
      value: o.hardware_id,
      note: '由本机硬件固定，网页上改不了（要改请编辑 config.json）',
      frozen: true,
    },
    {
      label: '序列号',
      value: o.serial_number,
      // 这句话存在是因为序列号**可以是算出来的**：第一次启动时后端按本机标识
      // （机器名 + 系统 MachineGuid）派生一个、当场写进 config.json，此后
      // 永远用文件里那份。不说明的话，"我没填过这个号，它是哪来的"会变成
      // 一个查不动的问题。
      note: '首次启动时按本机标识自动生成并写进 config.json，换网卡不变',
      frozen: true,
    },
    {
      label: '设备 UUID',
      value: o.device_uuid,
      // NVR 拿它当主键 —— 这个值变了，NVR 那边就是"多出一台新摄像机"。
      note: 'NVR 用它当设备主键，一变就会多出一台"新"摄像机；网页上改不了',
      frozen: true,
    },
    { label: '设备名称', value: o.device_name, note: '', frozen: false },
    { label: '位置', value: o.location, note: '', frozen: false },
  ]
})

// ---- 通道 -------------------------------------------------------------------
//
// 一个通道一行。**这里的每一格都来自 `/ws/stats` 的 `channels[]`**（后端算好的
// 通道号、端口、路径、状态），前端不自己推 `web.port + N` 那条规则 —— 那条规则
// 只该在后端有一份。前端再推一遍的话，两边对不上的症状是"设备信息页说 8098、
// NVR 却连在 8099"，而两个数看起来都对。

/**
 * 服务端口可能是命令行覆盖过的（`--http-port`），所以以后端报的为准 ——
 * 这与上面 `webPort` 同一条纪律。
 */
const rtspPort = computed(() => cfg.value?.rtsp.port ?? 554)

/** 一路的 RTSP 主码流地址。RTSP 服务是所有通道**共用**的，靠路径区分。 */
function channelRtsp(path: string): string {
  if (!path) return ''
  return `rtsp://${deviceHost.value}:${rtspPort.value}${path}`
}

const channelRows = computed(() =>
  (stats.value.channels ?? []).map((c) => ({
    index: c.index,
    name: c.name || `通道 ${c.index}`,
    // ONVIF 端口：通道 1 是 `web.port`，通道 N 是 `web.port + N`。后端就是这么
    // 报出来的（`channels[].web_port`），这里原样显示。
    onvifPort: c.web_port ? String(c.web_port) : '未知',
    rtsp: channelRtsp(c.rtsp_path),
    state: c.state || '未知',
    source: c.source === 'webcam' ? '本机摄像头' : c.source === 'display' ? '显示器' : c.source,
  })),
)

// ---- 录像占用 ---------------------------------------------------------------
//
// 用户点名要求把这一项摆到界面上："别让人自己去发现磁盘满了"。**只读上报** ——
// 起停录像的开关在命令行上，这一轮不开接口（见 README「要不要录」那一节）。

/** 按当前码率粗算每小时写多少。视频码率 + 音频那一路（如果开着）。 */
function hourlyBytes(bitrateKbps: number, withAudio: boolean): number {
  const audioKbps = withAudio ? (cfg.value?.audio.bitrate_kbps ?? 0) : 0
  return ((bitrateKbps + audioKbps) * 1000 * 3600) / 8
}

const recordingRows = computed(() => {
  const audio = !!cfg.value?.audio.enabled
  return (stats.value.channels ?? [])
    .filter((c) => c.record_file || c.record_bytes > 0)
    .map((c) => ({
      index: c.index,
      name: c.name || `通道 ${c.index}`,
      file: c.record_file,
      bytes: c.record_bytes,
      size: formatBytes(c.record_bytes),
      // 这一句是**估算**，用配置里的码率算的；录像文件写的是实际字节数。
      // 两者对不上的时候（自适应降帧、桌面静止时几乎不产帧）以左边那一列为准，
      // 所以文案里写明是"约"。
      hourly: c.bitrate_kbps > 0 ? `约 ${formatBytes(hourlyBytes(c.bitrate_kbps, audio))}` : '未知',
    }))
})

/** 所有通道加起来一共占了多少。 */
const recordingTotal = computed(() =>
  (stats.value.channels ?? []).reduce((n, c) => n + (c.record_bytes || 0), 0),
)

const mainRes = computed(() => {
  const w = stats.value.video_width
  const h = stats.value.video_height
  return w > 0 && h > 0 ? `${w} × ${h}` : ''
})

const subRes = computed(() => {
  const w = stats.value.sub_width
  const h = stats.value.sub_height
  return w > 0 && h > 0 ? `${w} × ${h}` : ''
})

const runRows = computed(() => {
  const rows: { label: string; value: string; note: string; tone: string }[] = []
  rows.push({
    label: '软件版本',
    // 空串 = WS 还没推过第一帧（或后端没起来）。**不编一个版本号** ——
    // 一个看着正常的数字没人会去怀疑它。
    value: stats.value.version || '未知',
    note: stats.value.version ? '' : '后端还没答话，或统计通道还没连上',
    tone: stats.value.version ? '' : 'tone-warn',
  })
  rows.push({
    label: '主码流分辨率',
    value: mainRes.value || '未知',
    note: mainRes.value ? '编码器实际在推的尺寸' : '采集还没产出过画面',
    tone: mainRes.value ? '' : 'tone-warn',
  })
  if (cfg.value?.rtsp.sub_enabled) {
    rows.push({
      label: '子码流分辨率',
      value: subRes.value || '未知',
      note: subRes.value ? '' : '子码流开着，但还没编码出画面',
      tone: subRes.value ? '' : 'tone-warn',
    })
  }
  rows.push({
    label: 'Web 端口',
    value: webPort.value ? String(webPort.value) : '未知',
    note: '',
    tone: '',
  })
  rows.push({
    label: '网页登录',
    value: authRequired.value ? '已启用' : '未启用',
    note: authRequired.value
      ? user.value
        ? `当前登录：${user.value}`
        : '当前会话的用户名未知'
      : '同一局域网内任何人都能打开这一页并改配置',
    tone: authRequired.value ? '' : 'tone-warn',
  })
  rows.push({
    label: 'WS-Discovery',
    value: stats.value.wsd_running ? '正在监听 3702' : '未监听',
    note: stats.value.wsd_running
      ? 'NVR 点「搜索设备」能自动发现这台机器'
      : 'NVR 搜不到这台设备，只能在它上面手工填地址',
    tone: stats.value.wsd_running ? '' : 'tone-warn',
  })
  return rows
})

// ---- 复制 -------------------------------------------------------------------

/**
 * 复制的是哪一行。
 *
 * `copied` / `failed` 记的是**值本身**而不是一个全局的状态字 —— 状态字会让
 * "复制失败"这一句同时出现在每一行上（一个 `v-if` 在 `v-for` 里、判的却是
 * 全局状态），于是用户不知道该去手工选中哪一行。定时器也要留着上一次的 id
 * 并清掉：连点两下时，第一次的定时器会把第二次的「已复制」提前抹掉。
 */
const copied = ref('')
const failed = ref('')
let resetTimer: number | null = null

async function onCopy(text: string): Promise<void> {
  if (!text) return
  const ok = await copyText(text)
  copied.value = ok ? text : ''
  failed.value = ok ? '' : text
  if (resetTimer !== null) window.clearTimeout(resetTimer)
  resetTimer = window.setTimeout(() => {
    copied.value = ''
    failed.value = ''
    resetTimer = null
  }, 2000)
}
</script>

<template>
  <div class="page-head">
    <h1>设备信息</h1>
    <span class="hint">
      这一页全是只读的。要给 NVR 添加通道，看下面第一张卡片；要改这些值，去「ONVIF 设置」。
    </span>
    <button :disabled="loading" @click="load">
      <Icon name="refresh" :size="14" />{{ loading ? '读取中…' : '重新读取' }}
    </button>
  </div>

  <div v-if="loadError" class="banner is-warn">
    读取配置失败（{{ loadError }}）。下面带「—」或写着未知的行多半是因为这个 ——
    统计类的行（版本、分辨率）仍然有效。
    <button @click="load">重试</button>
  </div>

  <!--
    这一张是**用户真正要问的那个问题**（"加进 NVR 时填什么"），所以它排在最前面，
    而不是埋在一堆身份字段后面。
  -->
  <div class="card">
    <h2>加进 NVR 时填这些</h2>
    <p class="hint">
      海康/大华这类 NVR 里手工添加 IP 通道时，要的是下面三样。设备能自动搜到的话
      （NVR 点「搜索设备」看得到本机）就什么都不用填，选上即可。
    </p>

    <div class="readonly-row">
      <span class="hint" style="min-width: 6em">地址</span>
      <code class="value">{{ onvifUrl }}</code>
      <button @click="onCopy(onvifUrl)"><Icon name="copy" :size="14" />复制</button>
      <span v-if="copied === onvifUrl" class="hint tone-good">已复制</span>
      <span v-else-if="failed === onvifUrl" class="hint tone-warn">
        复制失败，请手动选中上面的地址
      </span>
    </div>

    <div class="readonly-row">
      <span class="hint" style="min-width: 6em">用户名</span>
      <code class="value">{{ show(cfg?.onvif.user) }}</code>
      <button v-if="cfg?.onvif.user" @click="onCopy(cfg.onvif.user)">
        <Icon name="copy" :size="14" />复制
      </button>
      <span v-if="copied === cfg?.onvif.user" class="hint tone-good">已复制</span>
      <span v-else-if="failed === cfg?.onvif.user" class="hint tone-warn">
        复制失败，请手动选中上面的用户名
      </span>
    </div>

    <div class="readonly-row">
      <span class="hint" style="min-width: 6em">口令</span>
      <!--
        **口令永远不显示在这里。** `GET /api/config` 回来的是哨兵 `__unchanged__`
        （真值根本到不了浏览器），所以"显示一个打码的框"也无从显示起 ——
        与其画一个假的，不如说清楚去哪儿看。
      -->
      <span class="hint">
        与「ONVIF 设置」里设的那一份相同（出厂是 admin）。口令不在这台设备的任何
        页面上回显，忘了就在「ONVIF 设置」里重设一个。
      </span>
    </div>

    <p class="hint">
      上面的地址取自 RTSP 推流地址里那个网卡（后端自己选的）。要是你现在是从这台
      机器本机打开的，而 RTSP 又没开，这里显示的就只是 <code>localhost</code> 之类
      —— NVR 在别的机器上时，把它换成这台设备的局域网 IP。
    </p>
  </div>

  <div class="card">
    <h2>设备标识</h2>
    <p class="hint">
      这几项是 ONVIF 身份，NVR 添加通道后按它们显示通道名与厂商信息。
    </p>

    <p v-if="loading" class="hint">正在读取配置…</p>
    <p v-else-if="!cfg" class="hint">配置没读到，这几项暂时不可用。</p>
    <template v-else>
      <div v-for="row in identityRows" :key="row.label" class="readonly-row">
        <span class="hint" style="min-width: 6em">{{ row.label }}</span>
        <!--
          三个锁死的字段用**真的 `disabled` 输入框**，不是 `<code>` 加一句说明。
          两者在这里不是风格差异：`<code>` 只是"没做成输入框"，而下一个人往这
          一页加编辑功能时，它不会挡住任何事。`disabled` 是浏览器层面的事实 ——
          选中不了、改不了、也提交不出去（见 config_probe 那条同类判据）。
          配置页那几个框同样是 `disabled`，两处一致。
        -->
        <input
          v-if="row.frozen"
          class="value"
          type="text"
          :value="show(row.value)"
          disabled
          :title="row.note"
        />
        <code v-else class="value">{{ show(row.value) }}</code>
        <button v-if="row.value" @click="onCopy(row.value)">
          <Icon name="copy" :size="14" />复制
        </button>
        <span v-if="copied === row.value" class="hint tone-good">已复制</span>
        <span v-else-if="failed === row.value" class="hint tone-warn">复制失败</span>
        <span v-if="row.note" class="hint">{{ row.note }}</span>
      </div>
    </template>
  </div>

  <div class="card">
    <h2>通道</h2>
    <p class="hint">
      每个通道一个 ONVIF 端口，NVR 里一路填一个地址。RTSP 是所有通道共用的，
      靠路径区分。
    </p>

    <p v-if="loading" class="hint">正在读取配置…</p>
    <p v-else-if="!channelRows.length" class="hint">
      统计通道还没推过一帧，所以还不知道有哪几路。等一秒再看，或点上面的「重新读取」。
    </p>
    <template v-else>
      <div v-for="c in channelRows" :key="c.index" class="channel-line">
        <div class="readonly-row">
          <span class="hint" style="min-width: 6em">通道 {{ c.index }}</span>
          <code class="value">{{ c.name }}</code>
          <span class="hint">{{ c.source }} · {{ c.state }}</span>
        </div>
        <div class="readonly-row">
          <span class="hint" style="min-width: 6em">ONVIF 端口</span>
          <code class="value">{{ c.onvifPort }}</code>
        </div>
        <div class="readonly-row">
          <span class="hint" style="min-width: 6em">RTSP</span>
          <template v-if="c.rtsp">
            <code class="value">{{ c.rtsp }}</code>
            <button @click="onCopy(c.rtsp)"><Icon name="copy" :size="14" />复制</button>
            <span v-if="copied === c.rtsp" class="hint tone-good">已复制</span>
            <span v-else-if="failed === c.rtsp" class="hint tone-warn">复制失败</span>
          </template>
          <span v-else class="hint tone-warn">这一路还没报出 RTSP 路径</span>
        </div>
      </div>
    </template>
  </div>

  <div class="card">
    <h2>录像占用</h2>
    <p class="hint">
      每一路各录一份 MP4。这里是只读上报 —— 起停录像在启动参数上，网页上不开这个开关。
    </p>

    <p v-if="!recordingRows.length" class="hint">
      当前没有在录的通道（或者还没推过一帧统计）。
    </p>
    <template v-else>
      <!-- 复用状态页那张表的样式：四列在窄屏上照样会挤，让它自己横向滚动。 -->
      <div class="table-scroll">
        <table class="stream-table">
          <thead>
            <tr>
              <th>通道</th>
              <th>文件</th>
              <th>当前大小</th>
              <th>每小时约</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="r in recordingRows" :key="r.index">
              <td>{{ r.name }}</td>
              <td><code>{{ r.file || '—' }}</code></td>
              <td>{{ r.size }}</td>
              <td>{{ r.hourly }}</td>
            </tr>
          </tbody>
        </table>
      </div>
      <div class="readonly-row">
        <span class="hint" style="min-width: 6em">合计</span>
        <code class="value">{{ formatBytes(recordingTotal) }}</code>
        <span class="hint">
          「每小时约」是<strong>按配置里的码率估的</strong>；桌面静止时实际写得更少，
          以「当前大小」为准。分段之后旧文件不会被删掉，磁盘会一直涨。
        </span>
      </div>
    </template>
  </div>

  <div class="card">
    <h2>地址与服务</h2>

    <div class="readonly-row">
      <span class="hint" style="min-width: 6em">设备地址</span>
      <code class="value">{{ deviceHost }}</code>
      <span class="hint">
        {{ stats.rtsp_url ? '取自 RTSP 推流地址（后端自己选的网卡）' : 'RTSP 没开，取自你现在访问的主机名' }}
      </span>
    </div>

    <div class="readonly-row">
      <span class="hint" style="min-width: 6em">Web 控制台</span>
      <code class="value">{{ webBase }}</code>
      <button @click="onCopy(webBase)"><Icon name="copy" :size="14" />复制</button>
      <span v-if="copied === webBase" class="hint tone-good">已复制</span>
      <span v-else-if="failed === webBase" class="hint tone-warn">复制失败</span>
    </div>

    <div class="readonly-row">
      <span class="hint" style="min-width: 6em">RTSP 主码流</span>
      <template v-if="mainRtsp">
        <code class="value">{{ mainRtsp }}</code>
        <button @click="onCopy(mainRtsp)"><Icon name="copy" :size="14" />复制</button>
        <span v-if="copied === mainRtsp" class="hint tone-good">已复制</span>
        <span v-else-if="failed === mainRtsp" class="hint tone-warn">复制失败</span>
      </template>
      <span v-else class="hint tone-warn">推流没打开，NVR 拉不到流</span>
    </div>

    <div class="readonly-row">
      <span class="hint" style="min-width: 6em">RTSP 子码流</span>
      <template v-if="subRtsp">
        <code class="value">{{ subRtsp }}</code>
        <button @click="onCopy(subRtsp)"><Icon name="copy" :size="14" />复制</button>
        <span v-if="copied === subRtsp" class="hint tone-good">已复制</span>
        <span v-else-if="failed === subRtsp" class="hint tone-warn">复制失败</span>
      </template>
      <span v-else class="hint">没开子码流</span>
    </div>

    <p class="hint">
      子码流是给 NVR 的多画面网格用的另一路编码，分辨率低、占带宽小；它只能从
      RTSP 取，网页预览用的始终是主码流。
    </p>
  </div>

  <div class="card">
    <h2>运行</h2>
    <div v-for="row in runRows" :key="row.label" class="readonly-row">
      <span class="hint" style="min-width: 6em">{{ row.label }}</span>
      <code class="value">{{ row.value }}</code>
      <span v-if="row.note" class="hint" :class="row.tone">{{ row.note }}</span>
    </div>
  </div>
</template>
