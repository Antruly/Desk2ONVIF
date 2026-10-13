<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { fetchAudit } from '../api'
import type { AuditEntry, AuditPage } from '../types'
import { copyText, formatBytes } from '../format'

/**
 * 操作日志（只读）。
 *
 * ## 这一页要回答的问题，和它**不该**回答的
 *
 * 用户要的是「哪天、哪个 IP、把什么改了」。所以这里只列**改动类 + 登录**，
 * 不列查看类（开页面、看快照、读配置都不在）—— 那是后端定的，这一页照实呈现
 * 就好，但要**写出来**：不然用户找不到"我刚才点的那一下"会以为日志漏记。
 *
 * ## 三个状态必须分开画，这是这一页最容易做错的地方
 *
 * 同一个空表格对着三种完全不同的现实：
 *   1. **读不出来**（`ok:false`，目录没权限之类）—— 要去查目录权限；
 *   2. **没在记**（`enabled:false`，`--no-audit` 或 `log.audit=false`）——
 *      不是"没发生过"，是压根没写；
 *   3. **确实还没有记录**（`ok:true` + 空数组，刚恢复出厂就是这一种）——
 *      一切正常，等着就行。
 *
 * 三者都画成一张空表的话，用户会去做三件都不对的事。所以下面每一支都单独
 * 出话，而且**说清各自该往哪查**。这也是 `AuditPage.ok` 那句注释的落地：
 * `ok` 说的是「读到了没有」，不是「有没有记录」。
 *
 * ## 参数按**服务端回报的**那两个数说话
 *
 * 请求里的 `days` / `limit` 会被服务端夹到 [1,400] / [1,5000] 再回报回来。
 * 界面上那两个输入框留着用户填的原始值（他填过什么看得见），而**取数说明**
 * 一律用回报值 —— 请求 5000 天而实际只读了 400 天时，少掉的那一截看起来像
 * 「那段时间没有记录」，而那不是事实。夹过就明说夹过。
 *
 * ## 「复制一行」复制的是 `raw`
 *
 * 不是把五个字段拼回去的那份。审计的全部价值就在于"贴出去的这一行确实是
 * 日志里那一行"，而拼回去的字符串和磁盘上的那串字节不是同一份东西（分隔
 * 符、空格、字段顺序都可能不同）。后端专门为这件事多给了 `raw` 一个字段。
 */

const page = ref<AuditPage | null>(null)
/** HTTP 层的失败（连不上、500）。与 `page.ok === false` 是两回事，见文件头。 */
const loadError = ref('')
const loading = ref(true)

/**
 * 输入框里的值 —— **只有点「查询」时才影响到请求**。
 *
 * 不做输入即查：每敲一个字就扫一遍日志目录，键盘上按住不放就是自己给自己
 * 造一场 IO 风暴，而这个页面的读者本来就不急。
 *
 * 类型带上 `string`：`v-model.number` 在输入框被清空时给的是空串而不是数字，
 * 那个空串要在提交时才翻译成「没填，用默认值」。
 */
const daysInput = ref<number | string>(7)
const limitInput = ref<number | string>(500)
const keywordInput = ref('')

/** 上一次**真的发出去**的那组参数。用来在服务端夹过值时说出来。 */
const sent = ref({ days: 7, limit: 500 })

function intOr(v: number | string, fallback: number): number {
  const n = typeof v === 'number' ? v : Number(v.trim())
  // 只是个能转成整数的数就照发（0、负数也发）—— 夹取是服务端的活，而且它
  // 会把夹过之后的值回报回来，比在这里悄悄改掉用户填的数诚实。
  return Number.isFinite(n) ? Math.floor(n) : fallback
}

async function load(): Promise<void> {
  loading.value = true
  loadError.value = ''
  const days = intOr(daysInput.value, 7)
  const limit = intOr(limitInput.value, 500)
  const keyword = keywordInput.value.trim()
  sent.value = { days, limit }

  const res = await fetchAudit(days, limit, keyword)
  if (res.ok) {
    page.value = res.data
  } else {
    // 走到这里说明 HTTP 层就失败了（连不上、或 500）。**不把 page 清空** ——
    // 上一次读到的那一份比一张空表有用得多，而它旁边就有一句话说明它不是
    // 最新的。清掉的话，一次网络抖动看起来会像"设备上什么都没有"。
    loadError.value = res.error
  }
  loading.value = false
}

function onQuery(): void {
  void load()
}

onMounted(() => {
  void load()
})

// ---- 顶部那句留存策略 -------------------------------------------------------

/**
 * 「保留 180 天」这句话在 `keep_days === 0` 时**是假的**（那个值是"永不剪枝"），
 * 而用户改过这个数之后界面上照样写 180 才是最坏的一种错。所以按实际值拼。
 */
const retention = computed(() => {
  const p = page.value
  if (!p) return ''
  const keep =
    p.keep_days > 0 ? `保留 ${p.keep_days} 天` : '不自动清理（留存天数设成了 0）'
  return `${keep}；当前 ${p.files} 个文件 / ${formatBytes(p.bytes)}；恢复出厂会清空。`
})

/** 服务端夹过值没有。夹过就要说出来，理由见文件头。 */
const clamped = computed(() => {
  const p = page.value
  if (!p) return ''
  if (p.days !== sent.value.days && p.limit !== sent.value.limit) {
    return `你填的是 ${sent.value.days} 天 / ${sent.value.limit} 条，服务端按 ${p.days} 天 / ${p.limit} 条读。`
  }
  if (p.days !== sent.value.days) {
    return `你填的是 ${sent.value.days} 天，服务端按 ${p.days} 天读。`
  }
  if (p.limit !== sent.value.limit) {
    return `你填的是 ${sent.value.limit} 条，服务端按 ${p.limit} 条读。`
  }
  return ''
})

/** 读满一页了 —— 更早的记录这次没读进来。不说的话看起来像"就这些了"。 */
const truncated = computed(() => {
  const p = page.value
  return p !== null && p.count > 0 && p.count >= p.limit
})

/** 三种"一张空表"里，当前是哪一种。 */
const emptyKind = computed<'none' | 'no-record' | 'no-match' | 'off' | 'unreadable'>(() => {
  const p = page.value
  if (!p) return 'none'
  if (!p.enabled) return 'off'
  if (!p.ok) return 'unreadable'
  if (p.count > 0) return 'none'
  return keywordInput.value.trim() !== '' ? 'no-match' : 'no-record'
})

// ---- 按天分组 ---------------------------------------------------------------

interface DayGroup {
  day: string
  rows: AuditEntry[]
}

/**
 * 后端已经是**倒序**（新的在前），所以这里只把**相邻**的同一天归到一起。
 *
 * 不排序：再排一遍等于让这一页对"后端是不是倒序"这件事拥有一份独立判断，
 * 而两份判断迟早会不一致 —— 那时列表会看起来"顺序怪怪的"而没有人查得出原因。
 * 顺序归后端定，这一页只负责画。
 */
const groups = computed<DayGroup[]>(() => {
  const out: DayGroup[] = []
  for (const e of page.value?.entries ?? []) {
    const last = out[out.length - 1]
    if (last && last.day === e.day) last.rows.push(e)
    else out.push({ day: e.day, rows: [e] })
  }
  return out
})

// ---- 动作串的人话 ----------------------------------------------------------

/**
 * 动作串 → 人话。
 *
 * **代码本身照样显示在旁边**（等宽小字），因为用户拿着它去搜、去对文档才对得
 * 上 —— 与 `format.ts` 里 preset/tune 那套是同一个纪律：翻译只发生在显示层，
 * 值本身不翻译。认不出来的一律原样显示，不编一个"未知操作"糊过去。
 */
const ACTION_LABELS: Record<string, string> = {
  startup: '程序启动',
  shutdown: '程序退出',
  'config.set': '改了配置',
  reload: '重开采集管线',
  restart: '重启服务',
  'factory-reset': '恢复出厂',
  'time.set': '手动校准时间',
  'time.reset': '时间偏移归零',
  'time.sync': '立即对时',
  'login.ok': '登录成功',
  'login.fail': '登录失败',
  'ntp.sync': 'NTP 对时',
  'onvif.SetSystemDateAndTime': 'NVR 调了时间',
  'onvif.SetVideoEncoderConfiguration': 'NVR 改了编码参数',
  'onvif.SetVideoSourceConfiguration': 'NVR 改了视频源',
  'onvif.SetOSD': 'NVR 改了水印',
  'onvif.CreateOSD': 'NVR 加了水印',
  'onvif.DeleteOSD': 'NVR 删了水印',
}

function actionLabel(a: string): string {
  const hit = ACTION_LABELS[a]
  if (hit) return hit
  // NVR 那边还有一批 `onvif.<操作名>` 是通用桥接写出来的（`onvif.Set*` 里
  // 没逐条列的那些）。它们同样是"录像机把设备改了"，认得出这一层就够了。
  if (a.startsWith('onvif.')) return 'NVR 操作'
  return a
}

/** `login.fail` 这类要看得出来：一页日志里最该被一眼扫到的就是它。 */
function isWarnAction(a: string): boolean {
  return a === 'login.fail'
}

// ---- 复制一行 ---------------------------------------------------------------

/**
 * 记的是**哪一行**（组内下标）而不是值本身：审计里两条一模一样的行是可能的
 * （同一秒、同一动作），按值记会让两行同时亮起来 —— 而用户正需要知道刚才
 * 复制的是哪一条。定时器要留着上一次的 id 清掉，否则连点两下时第一次的
 * 定时器会把第二次的「已复制」提前抹掉（InfoView 里同一个写法）。
 */
const copiedKey = ref('')
const failedKey = ref('')
let resetTimer: number | null = null

function rowKey(day: string, i: number): string {
  return `${day}#${i}`
}

async function onCopy(text: string, key: string): Promise<void> {
  const ok = await copyText(text)
  copiedKey.value = ok ? key : ''
  failedKey.value = ok ? '' : key
  if (resetTimer !== null) window.clearTimeout(resetTimer)
  resetTimer = window.setTimeout(() => {
    copiedKey.value = ''
    failedKey.value = ''
    resetTimer = null
  }, 2000)
}
</script>

<template>
  <div class="page-head">
    <h1>操作日志</h1>
    <span class="hint">
      只读。这里记的是「谁、从哪、改了什么」，以及登录 —— 看页面、看快照这类
      <strong>查看</strong>操作不记。
    </span>
    <button :disabled="loading" @click="load()">
      <Icon name="refresh" :size="14" />{{ loading ? '读取中…' : '刷新' }}
    </button>
  </div>

  <!-- 连不上后端 / 500：与「日志里没有记录」是两回事，所以单独一块。 -->
  <div v-if="loadError" class="banner is-err">
    读不到操作日志（{{ loadError }}）。
    <template v-if="page">
      下面显示的是**上一次**读到的那一份，可能已经不是最新的。
    </template>
    <button @click="load()">重试</button>
  </div>

  <!-- 审计关着：最容易被误读成「没发生过任何事」的一种，所以说在最前面。 -->
  <div v-else-if="page && !page.enabled" class="banner is-warn">
    操作日志现在是<strong>关闭</strong>的（启动参数 <code>--no-audit</code>，
    或配置里的 <code>log.audit</code> 设成了 false）。下面没有记录<strong>不是</strong>
    因为这台设备上没发生过事，而是因为压根没在记。要开始记就把它打开并重启服务。
  </div>

  <!-- 读到了、但这次读失败（目录没权限之类）。元信息仍然有效，所以照样显示。 -->
  <div v-else-if="page && !page.ok" class="banner is-warn">
    日志读取出错（{{ page.error }}）。多半是日志目录的权限问题 ——
    下面给出的目录路径可以照着去查。这一份统计信息仍然是有效的。
  </div>

  <div v-if="page" class="card">
    <h2>留存与位置</h2>
    <p class="hint">{{ retention }}</p>

    <div class="readonly-row">
      <span class="hint" style="min-width: 6em">日志目录</span>
      <code class="value">{{ page.dir || '（未启用）' }}</code>
      <button v-if="page.dir" @click="onCopy(page.dir, 'dir')">
        <Icon name="copy" :size="14" />复制
      </button>
      <span v-if="copiedKey === 'dir'" class="hint tone-good">已复制</span>
      <span v-else-if="failedKey === 'dir'" class="hint tone-warn">复制失败</span>
    </div>

    <p class="hint">
      这一份是<strong>操作日志</strong>，和调试日志 <code>desk2onvif.log</code>
      是两份不同的文件：那个按 4 MB 轮转、里头有排查用的细节；这一份只记改动与
      登录，按天分文件、按上面的天数留存。口令<strong>永远不会</strong>出现在这里
      —— 改口令只会记成「改了配置」，不记值。
    </p>
  </div>

  <div class="card">
    <h2>记录</h2>

    <form class="row" novalidate @submit.prevent="onQuery">
      <label class="field-label" for="log-days">最近</label>
      <input id="log-days" v-model.number="daysInput" type="number" style="max-width: 6em" />
      <span class="hint">天</span>

      <label class="field-label" for="log-limit">最多</label>
      <input id="log-limit" v-model.number="limitInput" type="number" style="max-width: 7em" />
      <span class="hint">条</span>

      <label class="field-label" for="log-keyword">关键字</label>
      <input
        id="log-keyword"
        v-model="keywordInput"
        type="search"
        placeholder="IP、用户名、动作…"
        style="min-width: 12em"
      />

      <button type="submit" :disabled="loading">查询</button>
    </form>

    <p class="hint">
      关键字是<strong>区分大小写</strong>的整串匹配，在整行里找；留空就是不过滤。
      要查「谁改的码率」就填动作串 <code>config.set</code>，要查某个来源就填它的 IP。
    </p>

    <p v-if="clamped" class="hint tone-warn">{{ clamped }}</p>

    <p v-if="loading && !page" class="hint">正在读取…</p>

    <template v-else-if="page">
      <!-- 三种"空"各自的说法，见文件头那张清单。 -->
      <p v-if="emptyKind === 'no-record'" class="hint">
        这段时间里没有任何操作记录。这台设备上如果刚恢复过出厂，这就是正常的
        —— 恢复出厂会清空日志，之后从那一刻重新开始记。
      </p>
      <p v-else-if="emptyKind === 'no-match'" class="hint">
        最近 {{ page.days }} 天里没有匹配「{{ keywordInput }}」的记录。
        关键字区分大小写，换个写法或缩短一点再试。
      </p>
      <p v-else-if="emptyKind === 'unreadable'" class="hint">
        日志没能读出来，所以这里没有内容可显示（原因见上面那一行）。
      </p>
      <p v-else-if="emptyKind === 'off'" class="hint">
        操作日志已关闭，没有可显示的内容。
      </p>

      <template v-else>
        <p class="hint">
          读了最近 {{ page.days }} 天，共 {{ page.count }} 条（新的在前）。
        </p>
        <p v-if="truncated" class="hint tone-warn">
          已经读满 {{ page.limit }} 条的上限 —— 更早的记录这次没有读进来。
          把「最多」调大或缩小天数范围再看。
        </p>

        <div class="table-scroll">
          <table class="stream-table">
            <thead>
              <tr>
                <th>时间</th>
                <th>来源</th>
                <th>身份</th>
                <th>动作</th>
                <th>详情</th>
                <th></th>
              </tr>
            </thead>
            <!-- 按天分组：一天一个小标题。日志按天分文件，界面上也按天断开，
                 这样"哪天的"不用去读每一行的时间。 -->
            <tbody v-for="g in groups" :key="g.day">
              <tr>
                <td colspan="6" class="hint" style="padding-top: 14px">{{ g.day }}</td>
              </tr>
              <tr v-for="(e, i) in g.rows" :key="rowKey(g.day, i)">
                <td><code class="num">{{ e.time }}</code></td>
                <td>{{ e.source }}</td>
                <td>{{ e.identity }}</td>
                <td :class="isWarnAction(e.action) ? 'tone-warn' : ''">
                  {{ actionLabel(e.action) }}
                  <code class="muted">{{ e.action }}</code>
                </td>
                <td
                  style="white-space: normal; min-width: 16em"
                  :title="e.detail"
                >
                  {{ e.detail }}
                </td>
                <td>
                  <button
                    type="button"
                    :aria-label="`复制这一行：${e.raw}`"
                    @click="onCopy(e.raw, rowKey(g.day, i))"
                  >
                    <Icon name="copy" :size="14" />复制
                  </button>
                  <span v-if="copiedKey === rowKey(g.day, i)" class="hint tone-good">已复制</span>
                  <span
                    v-else-if="failedKey === rowKey(g.day, i)"
                    class="hint tone-warn"
                    >复制失败</span
                  >
                </td>
              </tr>
            </tbody>
          </table>
        </div>

        <p class="hint">
          「复制」复制的是磁盘上那一行的<strong>原文</strong>（不是把上面几列拼回去
          的），这样贴到别处的那一行就是日志里的那一行。
        </p>
      </template>
    </template>
  </div>
</template>
