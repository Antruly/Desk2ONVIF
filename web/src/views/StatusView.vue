<script setup lang="ts">
import { computed, ref } from 'vue'
import { RouterLink } from 'vue-router'
import StatTile from '../components/StatTile.vue'
import Icon from '../components/Icon.vue'
import { useStats } from '../composables/useStats'
import { fetchConfig, reloadPipeline, saveQualityTier } from '../api'
import type { QualityTier } from '../types'
import { copyText, formatBytes, formatDuration, formatInt } from '../format'

const { stats, state, bitrateKbps, lastMessageAt } = useStats()

const copied = ref<string>('') // 记住复制的是哪一路，免得两个按钮同时亮"已复制"
/**
 * 复制失败的是哪一路。
 *
 * **记值而不是记一个 `ok|fail` 的状态字。** 状态字曾经写在这里，而下面那个
 * `v-else-if` 在 `v-for` 里面 —— 于是复制**任意一路**失败时，每一路都会显示
 * "复制失败，请手动选中上面的地址"，而用户不知道该去选中哪一行。判据本身没错，
 * 错在它是个全局量却长在了每一行上。
 */
const failedCopy = ref<string>('')
let copyResetTimer: number | null = null

/**
 * 要展示的码流地址。
 *
 * 以 rtsp_streams 为准，它在后端给的字段缺失时退回到只有主码流的那种老形状
 * —— 老的 /api/status 里只有 rtsp_url，而这个页面在与旧后端对话时不该整块空白。
 */
const streamUrls = computed(() => {
  const list = stats.value.rtsp_streams.filter((s) => s.url)
  if (list.length > 0) return list
  return stats.value.rtsp_url
    ? [
        {
          path: 'live',
          url: stats.value.rtsp_url,
          clients: stats.value.rtsp_clients,
          nalu_dropped: stats.value.nalu_dropped,
          max_nalu_bytes: stats.value.max_nalu_bytes,
          packets: 0,
          bytes: 0,
        },
      ]
    : []
})

/** 子码流那一行的说明文字。0 就是没开 —— 报"（未开启子码流）"而不是留空。 */
const subSize = computed(() =>
  stats.value.sub_width > 0 ? `${stats.value.sub_width}x${stats.value.sub_height}` : '',
)

/** 丢弃类计数合计。分开看容易漏，合成一条"链路有损"的提示更醒目。 */
const totalDrops = computed(
  () => stats.value.queue_dropped + stats.value.pool_exhausted + stats.value.nalu_dropped,
)

const lastSeen = computed(() => {
  if (!lastMessageAt.value) return '——'
  const d = new Date(lastMessageAt.value)
  const p = (n: number) => String(n).padStart(2, '0')
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
})

/** 每帧字节数：编码器是否真的按码率在上限附近工作，这个数比累计值直观。 */
const avgFrameBytes = computed(() =>
  stats.value.packets > 0 ? stats.value.bytes / stats.value.packets : 0,
)

/* ----------------------------------------------------------- ONVIF / 发现 */

/**
 * 收到过 Probe 但没回答的数量。
 *
 * **这个差不是错误计数。** 后端只在"类型匹配 + scope 有交集"时才回包
 * （`wsd.cpp` 里 type_ok / scope 两条过滤），所以别人问一个我们不匹配的类型
 * 时，这个差就会涨 —— 那是正确行为。真正要看的是 `seen` 本身：
 * **它是 0，说明根本没有 NVR 找过我们**，那才是"搜不到设备"的第一现场。
 */
const probesUnanswered = computed(() =>
  Math.max(0, stats.value.wsd_probes_seen - stats.value.wsd_probes_answered),
)

/** NVR 找过、我们回包了，但一个 SOAP 请求都没来 —— 十有八九是加了但连不上。 */
const discoveredButSilent = computed(
  () => stats.value.wsd_probes_answered > 0 && stats.value.onvif_requests === 0,
)

/* -------------------------------------------------------------------- 时间 */

/**
 * 设备时间与主机时间的差，以及"这个钟被改过没有"。
 *
 * 这一行显示的**不是"/api/time 那个数"**，而是 `Stats` 上的六个字段 —— 它跟着
 * WebSocket 每秒刷，所以 NVR 在后台调了 `SetSystemDateAndTime` 之后，这里几秒内
 * 就会自己变（而时间设置页是打开时才拉一次，那个数会陈旧）。两处同源、两种读法，
 * 理由写在 `types.ts` 的 `TimeInfo` 上。
 *
 * **阈值用 300 秒**（与后端打 WARN 的那条线一致）：小于它只报数，超过它才升级成
 * 一条要读的提示。理由是这一行是一个会改变**证据内容**的开关 —— 偏移一大，
 * 画面上的水印时间和文件时间戳就不是一回事了，那件事不能让它在界面上一副无害的样子。
 */
const TIME_WARN_S = 300

const timeOffsetS = computed(() => Math.round(stats.value.device_offset_s))

/** `+1 小时 0 分 0 秒` / `−30 分` / `0 秒`。符号用全角减号，和正号等宽。 */
const timeOffsetText = computed(() => {
  const s = timeOffsetS.value
  if (s === 0) return '0 秒'
  const sign = s < 0 ? '−' : '+'
  let r = Math.abs(s)
  const d = Math.floor(r / 86400)
  r -= d * 86400
  const h = Math.floor(r / 3600)
  r -= h * 3600
  const m = Math.floor(r / 60)
  const sec = r - m * 60
  const parts: string[] = []
  if (d) parts.push(`${d} 天`)
  if (h) parts.push(`${h} 小时`)
  if (m) parts.push(`${m} 分`)
  // 秒也要报：整秒是这条链路的精度下限（NTP 的亚秒会被丢掉），只报"0 分"等于
  // 把一个 40 秒的偏差显示成"没差"。
  if (sec || parts.length === 0) parts.push(`${sec} 秒`)
  return sign + parts.join(' ')
})

/**
 * 最近一次**对时**的时刻。0 = 从没对过 —— 不能印成 1970。
 *
 * ⚠️ **它是"对时线程最近动过没有"，不是"钟最近被改过没有"。** `Stats` 上这一组
 * 字段里**没有**那个数：`synced_at`（"钟被改的时刻"，手动校准 / NVR 对时 / NTP
 * 对时都算）只有 `GET /api/time` 才有。所以 NVR 在后台设了一次时间、而 NTP 从没
 * 跑过时，这里会是空的 —— 那是**如实**的（我们确实不知道钟什么时候被改的），
 * 不能拿对时时刻去顶替它，那会印出一个看着很可信的错数。
 */
const timeLastSyncText = computed(() => {
  const t = stats.value.last_sync_at
  if (!t) return ''
  const d = new Date(t * 1000)
  const p = (n: number) => String(n).padStart(2, '0')
  return `${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
})

/** 偏移非 0 才值得说一句。0 是默认值，也是绝大多数设备上的常态。 */
const timeOffsetNoteworthy = computed(() => timeOffsetS.value !== 0)

/* -------------------------------------------------------------------- 音频 */

/**
 * 音频四个状态的中文名。
 *
 * `audio_state` **不是布尔值**（`off|ok|nosignal|error`），四档各指一件事。
 * 最容易误判的是 `nosignal`：按设计它**不代表故障**（陷阱 81/87）——
 * 只勾了「系统声音」而系统一直没出声时，环回本来就可能一个包都不给，
 * 音轨的时间轴由补零撑住，长度不受影响。把它显示成红色的"故障"，
 * 用户会拿着一个正常读数去查一个不存在的问题。
 */
const AUDIO_STATE_LABELS: Record<string, string> = {
  off: '未开启',
  ok: '正常',
  nosignal: '无信号',
  error: '出错',
}

const audioStateLabel = computed(
  () => AUDIO_STATE_LABELS[stats.value.audio_state] ?? stats.value.audio_state,
)

const audioStateTone = computed<'good' | 'warn' | 'crit' | undefined>(() => {
  switch (stats.value.audio_state) {
    case 'ok':
      return 'good'
    case 'nosignal':
      return 'warn'
    case 'error':
      return 'crit'
    default:
      // `off` **故意不上色**：它是默认状态，不是"某件事出了问题"。
      // 给它配一个颜色的话，全新装好的机器一打开状态页就是一块警告，
      // 而那时什么都没错 —— 一个总是亮着的警示灯等于没有警示灯。
      return undefined
  }
})

/**
 * 音轨时长（秒）。
 *
 * `audio_sample_rate` 为 0 表示音频根本没开 —— **别拿 0 去除**（会得到
 * `Infinity` 或 `NaN`，而 `formatDuration` 对 `NaN` 会退成 `00:00:00`，
 * 于是一个除零被伪装成了一个正常的读数）。
 */
const audioDurationS = computed(() =>
  stats.value.audio_sample_rate > 0 ? stats.value.audio_frames / stats.value.audio_sample_rate : 0,
)

/**
 * 电平条的宽度百分比。
 *
 * 输入已经是 0..1 且混音器输出前钳过位，这里只防一手脏数据。**不给最小值**
 * —— 给一条 0.001 的电平凑一个 2% 的宽度，是让"有声音但很轻"和"完全没声音"
 * 在视觉上变得一样，而这两件事在排查麦克风时恰恰要分开。旁边的数字是精确的。
 */
function levelPct(v: number): number {
  if (!Number.isFinite(v) || v <= 0) return 0
  return Math.min(100, v * 100)
}

/**
 * 削顶判据。
 *
 * 阈值写 0.99 而不是 1.0：电平表本身就是钳过位的（混音器输出前 clamp 到 ±1），
 * 所以"正好 1.0"和"顶部被削掉"在读数上完全一样 —— 等到 1.0 才变红等于不报。
 */
function isHot(v: number): boolean {
  return v >= 0.99
}

/* --------------------------------------------------- 自适应降载（性能横幅） */

/**
 * 画质档位表。这一页要它**只为了一件事**：把"降到哪一档"写成中文名。
 *
 * 表由后端在 `GET /api/config` 上挂出来（`quality_tiers`），这里初始化时取一次
 * —— 它是静态元信息，运行中不会变。取不到不影响这一页的任何一个数字：
 * `tierLabel()` 会退化成显示原始 key，"降到 smooth" 仍然是一个能做的决定，
 * 比一个没有名字的按钮好。
 */
const tierTable = ref<QualityTier[]>([])
void fetchConfig().then((r) => {
  if (r.ok) tierTable.value = r.data.quality_tiers ?? []
})

/** 后端算出来的"下一档"。**只有 overloaded 时才有值**（见 main.cpp 的 stats hook）。 */
const suggestedTier = computed(() => stats.value.perf_suggested_quality)

function tierLabel(key: string): string {
  return tierTable.value.find((t) => t.key === key)?.label || key
}

/**
 * 一键降档的结果。**和 `stats.perf_state` 分开存**：重开管线之后
 * `perf_state` 立刻变回 `ok`（新会话还没跑满一秒），横幅会消失，而"刚才那次
 * 降档到底成没成"必须比横幅活得久 —— 否则用户点了按钮、横幅一消失，他就永远
 * 不知道发生了什么。所以结果渲染在横幅**外面**。
 */
const downgrade = ref<{ phase: 'idle' | 'busy' | 'ok' | 'fail'; message: string }>({
  phase: 'idle',
  message: '',
})

/**
 * 降档 = **两步，两步都成功才算数**：先存档位（`POST /api/config`），再让新的
 * 档位当场生效（`POST /api/reload`）。
 *
 * 只做第一步的后果是"配置改了、画面一点没变"，而这个按钮上写着"立即生效"
 * —— 那是这个项目里最不该出现的一类谎。反过来，第一步失败还硬着头皮往下走，
 * 重开的就是**旧档位**：画面会真的断一下、NVR 真的重连一次，然后一切照旧。
 * 那比什么都不做更糟，因为它看起来像是生效了。
 */
async function onDowngrade() {
  const tier = stats.value.perf_suggested_quality
  if (!tier || downgrade.value.phase === 'busy') return
  const label = tierLabel(tier)
  downgrade.value = { phase: 'busy', message: '' }

  const saved = await saveQualityTier(tier)
  if (!saved.ok) {
    downgrade.value = {
      phase: 'fail',
      message:
        `没有降档：第 1 步「保存画质档位」失败 —— ${saved.error}。` +
        `配置一个字都没改，采集也没有中断。`,
    }
    return
  }

  const rl = await reloadPipeline()
  if (!rl.ok) {
    downgrade.value = {
      phase: 'fail',
      message:
        `降档只成功了一半：档位「${label}」已经**存下**了，但第 2 步「重开管线」` +
        `失败 —— ${rl.error}。当前画面仍是原来的参数，要等下次重启程序才会用上新档位。`,
    }
    return
  }

  downgrade.value = {
    phase: 'ok',
    message:
      rl.data?.live === false
        ? `已存下档位「${label}」。当前没有在录制，所以没有需要重开的管线 —— ` +
          `配置会在下次开始录制时生效。`
        : `已降到「${label}」并按新档位重开了采集管线：录像已分段，` +
          `已连上的 NVR 会在数秒内自己重连。`,
  }
  // 成功这条看一眼就够了；失败那条不自动消失 —— 出错的事不该自己溜走。
  window.setTimeout(() => {
    if (downgrade.value.phase === 'ok') downgrade.value = { phase: 'idle', message: '' }
  }, 12000)
}

/* --------------------------------------------------------------- 交互 */

async function onCopyRtsp(url: string) {
  const ok = await copyText(url)
  copied.value = ok ? url : ''
  failedCopy.value = ok ? '' : url
  // 上一次的定时器要清掉：连点两下时，第一次那个会把第二次的提示提前抹掉。
  if (copyResetTimer !== null) window.clearTimeout(copyResetTimer)
  copyResetTimer = window.setTimeout(() => {
    copied.value = ''
    failedCopy.value = ''
    copyResetTimer = null
  }, 2000)
}

/**
 * 逐路的标签。**按序号写死名字是为了让界面比 /api/status 更好读**，但一旦哪天
 * 后端多出第三路，写死就会给它贴一个错的名字 —— 所以第三路之后回落到序号。
 */
function streamLabel(i: number): string {
  return i === 0 ? '主码流' : i === 1 ? '子码流' : `第 ${i} 路`
}
</script>

<template>
  <div class="page-head">
    <h1>运行状态</h1>
    <span class="hint">后端每秒推送一次；最后更新 {{ lastSeen }}</span>
  </div>

  <!-- WS 断开时后端仍在跑，只是这一页看不到数字。说清楚，别让人以为程序挂了。 -->
  <div v-if="state !== 'online'" class="banner is-warn">
    <strong>统计推送{{ state === 'connecting' ? '正在连接' : '已断开' }}。</strong>
    界面上的数字停在最后一次收到的值。会自动重连（退避 1 秒 → 30 秒）；
    若一直连不上，请确认后端进程还在。
  </div>

  <!--
    自适应降载的横幅。**这一页其余部分都是读数，只有这一条是"要你现在做点什么"**
    —— 所以它在最上面。

    文案由后端拼（`perf_hint`）："降到了多少、原来是多少"只有它知道，前端再拼
    一遍就是两份会漂移的真相（见 capture_session.cpp 里 perf_hint_for 的注释）。
    前端这里只负责两件后端不知道的事：把建议的那一档说成中文名、把代价说清楚。
  -->
  <div v-if="stats.perf_state === 'overloaded'" class="banner is-err">
    <strong>本设备性能不足以实时编码。</strong>
    {{ stats.perf_hint || '已经自动降低帧率，仍然跟不上。' }}
    <span v-if="stats.perf_duty > 1" class="hint">
      （实际每 {{ formatInt(stats.perf_duty) }} 拍出一帧，约
      {{ stats.perf_target_fps }} fps）
    </span>

    <div v-if="suggestedTier" class="actions" style="margin-top: 10px">
      <button :disabled="downgrade.phase === 'busy'" @click="onDowngrade">
        {{
          downgrade.phase === 'busy'
            ? '正在应用…'
            : `降到「${tierLabel(suggestedTier)}」并立即生效`
        }}
      </button>
      <!--
        代价写在按钮**旁边**而不是塞进 tooltip：点下去之后录像是真的要断的，
        而 tooltip 在点之前根本不出现。不写就是欺骗。
      -->
      <span class="hint">
        会现场重开采集管线：<strong>当前录像分段</strong>，已连上的 RTSP 客户端
        （NVR）会断开、数秒后自动重连。
      </span>
    </div>
    <div v-else class="hint" style="margin-top: 10px">
      已经在画质最低的一档了，没有更省的档位可降。要再省只能去「配置」里手工把
      分辨率或帧率调小。
    </div>
  </div>

  <!--
    已自动降帧率但仍然实时：不是故障，是保护生效了。**仍然要说出来** ——
    不然用户只会看到帧率莫名其妙变低，然后去找一个不存在的 bug。
  -->
  <div v-else-if="stats.perf_state === 'throttled'" class="banner is-warn">
    <strong>已自动降低帧率。</strong>
    {{ stats.perf_hint }}
  </div>

  <!-- 刚从降载里恢复过来那几秒的说明（后端特意拼的，别让它没人显示）。 -->
  <div v-else-if="stats.perf_hint" class="banner is-ok">
    {{ stats.perf_hint }}
  </div>

  <!--
    降档的结果。**渲染在横幅外面**：重开管线之后新会话立刻报 `perf_state = ok`，
    上面那条横幅会消失 —— 结果跟它一起消失的话，用户就永远不知道刚才那一下
    到底成没成。失败那条不自动消失，成功那条 12 秒后自己收起来。
  -->
  <div
    v-if="downgrade.phase === 'fail' || downgrade.phase === 'ok'"
    class="banner"
    :class="downgrade.phase === 'fail' ? 'is-err' : 'is-ok'"
  >
    {{ downgrade.message }}
  </div>

  <!--
    设备时间与主机时间的差。**它是一行读数，所以默认不占一条横幅** ——
    只有超过 5 分钟才升级成要读的提示（见 TIME_WARN_S 上面那段）。

    为什么放这一页而不是只放时间设置页：这里是**每秒刷**的那一份，NVR 在后台
    替我们对了一次时，几秒内就能看见；而配置页那个数是打开时才拉的。
  -->
  <div v-if="Math.abs(timeOffsetS) > TIME_WARN_S" class="banner is-warn">
    <strong>设备时间与主机时间相差 {{ timeOffsetText }}。</strong>
    画面上的水印、以及 ONVIF 报给 NVR 的时间<strong>都按设备时间走</strong>，
    而录像文件与日志的时间戳仍是本机时间 —— 两者现在差着这么多。
    <span v-if="timeLastSyncText" class="hint">
      （最近一次 NTP 对时：{{ timeLastSyncText }}）
    </span>
    <div class="actions" style="margin-top: 10px">
      <RouterLink class="btn" to="/config#time">
        <Icon name="clock" :size="15" />去「时间设置」校准
      </RouterLink>
    </div>
  </div>

  <p v-else-if="timeOffsetNoteworthy" class="hint section-note">
    <Icon name="clock" :size="13" />
    设备时间与主机时间相差 {{ timeOffsetText }}
    <template v-if="timeLastSyncText">（最近一次 NTP 对时：{{ timeLastSyncText }}）</template>
    。水印与 ONVIF 报的是<strong>设备时间</strong>；文件与日志的时间戳是本机时间。
  </p>

  <div class="card">
    <h2>编码与推流</h2>
    <div class="tiles">
      <StatTile label="有效帧率" :value="stats.effective_fps.toFixed(1)" unit="fps" />
      <StatTile label="码率（按累计字节推算）" :value="bitrateKbps.toFixed(1)" unit="kbps" />
      <StatTile label="已运行" :value="formatDuration(stats.elapsed_s)" />
      <StatTile label="累计流量" :value="formatBytes(stats.bytes)" />
      <StatTile
        label="编码包数"
        :value="formatInt(stats.packets)"
        :sub="`关键帧 ${formatInt(stats.keyframes)}`"
      />
      <StatTile
        label="画面变化帧"
        :value="formatInt(stats.content_frames)"
        :sub="`主循环 ${formatInt(stats.ticks)} 轮`"
      />
      <StatTile label="RTSP 观众" :value="formatInt(stats.rtsp_clients)" unit="个" />
      <StatTile
        label="统计订阅者"
        :value="formatInt(stats.ws_clients)"
        unit="个"
        :sub="`Web 端口 ${stats.web_port || '——'}`"
      />
    </div>
  </div>

  <!--
    音频。**这一段是"我的麦克风到底有没有在工作"的唯一反馈** —— 浏览器预览
    是没有声音的（fMP4 那一路是单流 + H.264 专用，见 README 的"没做的"），
    所以不开 NVR 就没有第二个地方能看它。这正是它必须在这儿的原因：
    用户打开音频之后最容易卡住的一步，恰好是唯一没法用预览验证的一步。
  -->
  <div class="card">
    <h2>音频</h2>

    <div class="tiles">
      <StatTile
        label="音频状态"
        :value="audioStateLabel"
        small
        :tone="audioStateTone"
        :sub="stats.audio_state === 'off' ? '默认关闭' : undefined"
      />
      <StatTile
        label="音轨时长"
        :value="formatDuration(audioDurationS)"
        :sub="stats.audio_sample_rate > 0 ? `采样率 ${stats.audio_sample_rate} Hz` : '音频没开'"
      />
      <StatTile label="AAC 包数" :value="formatInt(stats.audio_packets)" />
      <StatTile
        label="补零帧"
        :value="formatInt(stats.audio_zero_filled)"
        :sub="stats.audio_zero_filled > 0 ? '来源没供上，时间轴由补零撑住' : undefined"
      />
    </div>

    <!--
      电平表。`audio_state === 'off'` 时**不画**：两根永远是空的条读起来就是
      "没有声音"，而那时的事实是"没有音轨"—— 这两件事在这一页上必须分得开，
      否则"音频没开"会在用户眼里变成"音频开了但收不到声音"。
    -->
    <template v-if="stats.audio_state !== 'off'">
      <div class="meter">
        <span class="meter-name">L</span>
        <span class="meter-track">
          <span
            class="meter-fill"
            :class="{ 'is-hot': isHot(stats.audio_level_l) }"
            :style="{ width: levelPct(stats.audio_level_l) + '%' }"
          ></span>
        </span>
        <span class="meter-value">{{ stats.audio_level_l.toFixed(3) }}</span>
      </div>
      <div class="meter">
        <span class="meter-name">R</span>
        <span class="meter-track">
          <span
            class="meter-fill"
            :class="{ 'is-hot': isHot(stats.audio_level_r) }"
            :style="{ width: levelPct(stats.audio_level_r) + '%' }"
          ></span>
        </span>
        <span class="meter-value">{{ stats.audio_level_r.toFixed(3) }}</span>
      </div>
    </template>

    <p class="hint" style="margin-bottom: 0">
      <template v-if="stats.audio_state === 'off'">
        音频默认是关的 —— 打开它会改变每一台设备的 SDP，而 NVR 收不收只能实测。
        在「配置 → 音视频」里打开，改完按那一段的「保存并立即应用」
        （音频是冷字段，只保存不会重开采集设备）。
      </template>
      <template v-else-if="stats.audio_state === 'nosignal'">
        <strong>「无信号」按设计不代表故障。</strong>
        只勾了「系统声音」而系统一直没出声时，环回本来就可能一个包都不给 ——
        时间轴由补零撑住，音轨不会因此变短。若是麦克风这一路，再确认设备没被
        别的程序独占、也没被拔掉（插拔之后要重新读取一次设备列表）。
      </template>
      <template v-else-if="stats.audio_state === 'error'">
        <strong>音频编码器出错了。</strong>音轨从这里断掉，视频不受影响 ——
        日志里有原因，网页上重启一次服务可以重新建一条音轨。
      </template>
      <template v-else>
        电平是<strong>峰值</strong>（带释放，不是瞬时值），L/R 分开显示 ——
        混成一路的话"只有一边有声音"这种接线问题会看起来完全正常。
        到 <code class="value">0.99</code> 以上表示顶部被削掉了，把增益往回调。
      </template>
    </p>
  </div>

  <div class="card">
    <h2>RTSP 地址</h2>
    <div v-if="streamUrls.length === 0" class="readonly-row">
      <code class="value">（未开启推流）</code>
    </div>
    <div v-for="(s, i) in streamUrls" :key="s.url" class="readonly-row">
      <span class="hint" style="min-width: 4em">{{ streamLabel(i) }}</span>
      <code class="value">{{ s.url }}</code>
      <button @click="onCopyRtsp(s.url)">
        <Icon name="copy" :size="14" />复制
      </button>
      <span v-if="copied === s.url" class="hint tone-good">已复制</span>
      <!-- 非安全上下文（http 访问局域网 IP）下剪贴板不可用，得给出退路提示。 -->
      <span v-else-if="failedCopy === s.url" class="hint tone-warn">
        复制失败，请手动选中上面的地址
      </span>
    </div>
    <p class="hint">
      <template v-if="streamUrls.length === 0">
        未开启推流时这里是空的，去「配置」里打开 RTSP 并保存。给 NVR 手工添加通道时填主码流那个地址。
      </template>
      <template v-else>
        给 NVR 手工添加通道时填<strong>主码流</strong>那个地址；子码流由 ONVIF 的
        GetStreamUri 按 profile token 提供，不用手填。
      </template>
    </p>
    <p class="hint" style="margin-bottom: 0">
      <template v-if="subSize">
        子码流已开启（{{ subSize }}）。它在 NVR 的多画面网格里代替主码流，
        点开单画面时 NVR 才会切到主码流 —— 这是它唯一的作用。
      </template>
      <template v-else>
        子码流未开启。NVR 的多画面网格会去拉主码流，16 路网格就是 16 条全分辨率的流。
      </template>
    </p>
  </div>

  <!--
    ONVIF / WS-Discovery。
    「NVR 搜不到这台设备」时，这一块是唯一能分清"它没找过我们"和"找了但我们
    没答"的地方 —— 没有这几个数就只能抓包。
  -->
  <div class="card">
    <h2>ONVIF 与设备发现</h2>

    <div v-if="!stats.wsd_running" class="banner is-warn">
      <strong>WS-Discovery 没在运行。</strong>
      NVR 的"自动发现"永远搜不到这台设备（手工填 IP 添加仍然可以）。
      它跟着 ONVIF 走：ONVIF 关掉、或者启动时带了 <code class="value">--no-wsd</code>
      都会一起停。
    </div>

    <div v-if="discoveredButSilent" class="banner is-warn">
      <strong>NVR 已经发现过这台设备，但一个 ONVIF 请求都没打过来。</strong>
      通常卡在添加那一步：账号口令不对、或者防火墙拦了 Web 端口。
    </div>

    <div class="tiles">
      <StatTile
        label="收到的 Probe"
        :value="formatInt(stats.wsd_probes_seen)"
        :tone="stats.wsd_running && stats.wsd_probes_seen === 0 ? 'warn' : undefined"
        :sub="stats.wsd_running && stats.wsd_probes_seen === 0 ? '还没有设备找过' : undefined"
      />
      <StatTile
        label="已回答"
        :value="formatInt(stats.wsd_probes_answered)"
        :sub="probesUnanswered > 0 ? `未回答 ${formatInt(probesUnanswered)}（类型/scope 不匹配）` : undefined"
      />
      <StatTile label="SOAP 请求" :value="formatInt(stats.onvif_requests)" />
      <StatTile
        label="鉴权失败"
        :value="formatInt(stats.onvif_auth_failures)"
        :tone="stats.onvif_auth_failures > 0 ? 'warn' : undefined"
        :sub="stats.onvif_auth_failures > 0 ? 'NVR 用的口令和配置里不一致' : undefined"
      />
      <StatTile
        label="SOAP Fault"
        :value="formatInt(stats.onvif_faults)"
        :tone="stats.onvif_faults > 0 ? 'warn' : undefined"
      />
      <StatTile
        label="最近不支持的操作"
        :value="stats.onvif_last_unsupported || '——'"
        small
        :sub="stats.onvif_last_unsupported ? 'NVR 调了这个接口，我们没实现' : undefined"
      />
    </div>

    <p class="hint" style="margin-bottom: 0">
      「收到的 Probe」为 0 且 WS-Discovery 在运行时，先确认 NVR 那侧开了自动搜索，
      再确认 UDP 3702 没被防火墙拦掉 —— Windows 自身的
      <code class="value">fdPHost</code> 也监听这个端口，实测可以共存。
    </p>
  </div>

  <div class="card">
    <h2>浏览器预览通道</h2>
    <div class="tiles">
      <StatTile
        label="预览观看者"
        :value="formatInt(stats.preview_clients)"
        unit="个"
        :sub="`统计订阅者另有 ${formatInt(stats.ws_clients)} 个`"
      />
      <StatTile
        label="初始化段"
        :value="stats.preview_ready ? '已就绪' : '未产生'"
        small
        :tone="stats.preview_ready ? 'good' : 'warn'"
      />
      <StatTile
        label="产出分片"
        :value="formatInt(stats.preview_fragments)"
        :sub="`其中没人看时丢掉 ${formatInt(stats.preview_fragments_unwatched)}`"
      />
      <StatTile
        label="因积压断开"
        :value="formatInt(stats.preview_clients_kicked)"
        unit="次"
        :tone="stats.preview_clients_kicked > 0 ? 'warn' : undefined"
        :sub="stats.preview_clients_kicked > 0 ? '标签页被切走时会这样，切回来自动重连' : undefined"
      />
    </div>
    <p class="hint" style="margin-bottom: 0">
      分片只在<strong>关键帧</strong>处产生，所以「产出分片」不涨时预览必然是黑的，
      把关键帧间隔（GOP）调到 2 秒以内即可。「没人看时丢掉」是常态，不是故障 ——
      没有观看者时分片本来就没人要。
    </p>
  </div>

  <div class="card">
    <h2>链路质量</h2>
    <div v-if="totalDrops > 0" class="banner is-warn" style="margin-bottom: 12px">
      <strong>检测到丢弃。</strong>
      队列 {{ formatInt(stats.queue_dropped) }}、缓冲池 {{ formatInt(stats.pool_exhausted) }}、
      NALU {{ formatInt(stats.nalu_dropped) }}。持续增长说明编码跟不上采集，
      可以调低帧率或码率。
    </div>
    <div class="tiles">
      <StatTile
        label="队列丢弃"
        :value="formatInt(stats.queue_dropped)"
        :tone="stats.queue_dropped > 0 ? 'warn' : undefined"
      />
      <StatTile
        label="缓冲池耗尽"
        :value="formatInt(stats.pool_exhausted)"
        :tone="stats.pool_exhausted > 0 ? 'warn' : undefined"
      />
      <StatTile
        label="NALU 丢弃"
        :value="formatInt(stats.nalu_dropped)"
        :tone="stats.nalu_dropped > 0 ? 'warn' : undefined"
      />
      <StatTile
        label="DXGI 会话重建"
        :value="formatInt(stats.access_lost)"
        :sub="stats.access_lost > 0 ? '锁屏/切换分辨率会触发' : undefined"
      />
      <StatTile label="最大 NALU" :value="formatBytes(stats.max_nalu_bytes)" />
      <StatTile label="平均每包" :value="formatBytes(avgFrameBytes)" />
    </div>

    <!--
      逐路的表。**两路的码率差着近一个数量级**，上面那几个"合计"的数里，
      主码流会完全盖住子码流 —— 而"哪一路在丢"正是这张表要回答的问题。
    -->
    <div v-if="streamUrls.length > 1" class="table-scroll">
      <table class="stream-table">
        <thead>
          <tr>
            <th>码流</th>
            <th>路径</th>
            <th>观众</th>
            <th>已编码包</th>
            <th>流量</th>
            <th>NALU 丢弃</th>
            <th>最大 NALU</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="(s, i) in streamUrls" :key="s.url">
            <td>{{ streamLabel(i) }}</td>
            <td><code>{{ s.path || '——' }}</code></td>
            <td>{{ formatInt(s.clients) }}</td>
            <td>{{ formatInt(s.packets) }}</td>
            <td>{{ formatBytes(s.bytes) }}</td>
            <td :class="{ 'tone-warn': s.nalu_dropped > 0 }">{{ formatInt(s.nalu_dropped) }}</td>
            <td>{{ formatBytes(s.max_nalu_bytes) }}</td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>
