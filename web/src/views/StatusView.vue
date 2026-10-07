<script setup lang="ts">
import { computed, ref } from 'vue'
import StatTile from '../components/StatTile.vue'
import Icon from '../components/Icon.vue'
import { useStats } from '../composables/useStats'
import { copyText, formatBytes, formatDuration, formatInt } from '../format'

const { stats, state, bitrateKbps, lastMessageAt } = useStats()

const copied = ref<string>('') // 记住复制的是哪一路，免得两个按钮同时亮"已复制"
const copyState = ref<'idle' | 'ok' | 'fail'>('idle')

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

/* --------------------------------------------------------------- 交互 */

async function onCopyRtsp(url: string) {
  const ok = await copyText(url)
  copied.value = ok ? url : ''
  copyState.value = ok ? 'ok' : 'fail'
  window.setTimeout(() => {
    copied.value = ''
    copyState.value = 'idle'
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
      <span v-else-if="copyState === 'fail'" class="hint tone-warn">
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
