<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { fetchTime, resetTime, setTime, syncTimeNow } from '../api'
import { useConfig } from '../composables/useConfig'
import type { TimeInfo } from '../types'

/**
 * 时间设置。
 *
 * **作用范围是"设备时间"（= 主机时间 + 偏移），不是 Windows 系统时钟。**
 * 水印上那个时间和 ONVIF `GetSystemDateAndTime` 报出去的都是它。好处是不需要
 * 管理员权限、不影响这台机器上别的程序；代价是它可以和系统时间对不上。
 */

const { config, load, save } = useConfig()

const form = reactive({
  mode: 'manual',
  ntp_server: 'ntp.aliyun.com',
  ntp_interval_s: 3600,
})
const offsetInput = ref(0)

const info = ref<TimeInfo | null>(null)
const saving = ref(false)
const busy = ref('')
const note = ref('')
const error = ref('')

/** 每 250 ms 走一下，让「设备时间」看起来像在走 —— 但它永远以 `info` 为基准。 */
const nowTick = ref(Date.now())
let timer = 0

async function refreshTime() {
  const res = await fetchTime()
  if (!res.ok) {
    error.value = res.error
    return
  }
  info.value = res.data
  form.mode = res.data.mode
  form.ntp_server = res.data.ntp_server
  form.ntp_interval_s = res.data.ntp_interval_s
  offsetInput.value = res.data.offset_s
}

onMounted(async () => {
  await load()
  await refreshTime()
  timer = window.setInterval(() => {
    nowTick.value = Date.now()
  }, 250)
})
onBeforeUnmount(() => {
  if (timer) window.clearInterval(timer)
})

const deviceMs = computed(() => {
  const t = info.value
  if (!t) return 0
  const base = (t.host_now_ms ?? t.host_now * 1000) + t.offset_s * 1000
  // `info` 是拉取那一刻的读数；用本机单调增加的 `nowTick` 推着走。
  const fetchedAt = (t.host_now_ms ?? t.host_now * 1000)
  return base + Math.max(0, nowTick.value - fetchedAt)
})

const deviceText = computed(() => {
  if (!deviceMs.value) return '—'
  const d = new Date(deviceMs.value)
  const p = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
})

const dirty = computed(() => {
  if (!info.value) return false
  return (
    form.mode !== info.value.mode ||
    form.ntp_server !== info.value.ntp_server ||
    form.ntp_interval_s !== info.value.ntp_interval_s
  )
})

async function onSaveMode() {
  // 空输入框给的是空串；直接发出去后端会以"类型不对"整份拒绝。
  if (!Number.isFinite(Number(form.ntp_interval_s))) {
    error.value = '「对时间隔」要填一个整数秒。'
    return
  }
  saving.value = true
  error.value = ''
  note.value = ''
  const out = await save({
    time: {
      mode: form.mode,
      ntp_server: form.ntp_server,
      ntp_interval_s: form.ntp_interval_s,
    },
  })
  saving.value = false
  if (!out.ok) {
    error.value = out.error ?? '保存失败。'
    return
  }
  note.value = out.note ?? '已保存，对时设置已生效。'
  await refreshTime()
}

async function applyOffset() {
  const n = Number(offsetInput.value)
  if (!Number.isFinite(n)) {
    error.value = '「手动偏移」要填一个整数秒（可以是负数）。'
    return
  }
  busy.value = 'offset'
  error.value = ''
  note.value = ''
  const res = await setTime({ offset_s: Math.trunc(n) })
  busy.value = ''
  if (!res.ok) {
    error.value = res.error
    return
  }
  note.value = res.data?.note ?? '设备时间已校准。'
  if (res.data?.time) info.value = res.data.time
  else await refreshTime()
}

async function alignToHost() {
  busy.value = 'reset'
  error.value = ''
  note.value = ''
  const res = await resetTime()
  busy.value = ''
  if (!res.ok) {
    error.value = res.error
    return
  }
  note.value = res.data?.note ?? '偏移已归零。'
  if (res.data?.time) info.value = res.data.time
  else await refreshTime()
  offsetInput.value = 0
}

async function syncNow() {
  busy.value = 'sync'
  error.value = ''
  note.value = ''
  const res = await syncTimeNow()
  busy.value = ''
  if (!res.ok) {
    error.value = res.error
    return
  }
  note.value = res.data?.note ?? '已请求对时，结果几秒后出现在下面。'
  // 202：查询还在飞；轮询几次把结果带回来。
  for (let i = 0; i < 8; ++i) {
    await new Promise((r) => window.setTimeout(r, 600))
    await refreshTime()
    if (info.value && !info.value.syncing) break
  }
}

const syncText = computed(() => {
  const t = info.value
  if (!t) return '—'
  if (t.syncing) return '正在对时…'
  if (!t.last_sync_state) return '还没有对过时'
  if (t.last_sync_state === 'ok') {
    return `成功（stratum ${t.last_sync_stratum}，偏移 ${t.last_sync_offset_s} 秒）`
  }
  return `失败：${t.last_sync_state}${t.last_sync_error ? ' — ' + t.last_sync_error : ''}`
})
</script>

<template>
  <section class="page-head">
    <div>
      <h1>时间设置</h1>
      <p class="sub">
        这里管的是<strong>设备时间</strong>（水印与 ONVIF 报出去的那个），
        不是 Windows 系统时钟。它 = 主机时间 + 偏移，不需要管理员权限。
      </p>
    </div>
  </section>

  <div v-if="error" class="banner is-err"><strong>操作失败。</strong>{{ error }}</div>
  <div v-else-if="note" class="banner is-ok">{{ note }}</div>

  <div v-if="config" class="settings-grid two-col">
    <section class="panel">
      <h2><Icon name="clock" :size="16" />设备时间</h2>
      <div class="device-clock">{{ deviceText }}</div>
      <dl class="kv">
        <div>
          <dt>与主机相差</dt>
          <dd class="mono">{{ info ? (info.offset_s >= 0 ? '+' : '') + info.offset_s + ' 秒' : '—' }}</dd>
        </div>
        <div>
          <dt>被改过没有</dt>
          <dd>{{ info?.synced ? '是' : '否（从未校准过）' }}</dd>
        </div>
        <div>
          <dt>配置里存着的偏移</dt>
          <dd class="mono">{{ info ? info.configured_offset_s + ' 秒' : '—' }}</dd>
        </div>
      </dl>
      <p class="hint-block">
        手动校准的偏移会写进配置，重启后仍在。NTP 模式下这里显示的是
        <strong>在用的</strong>偏移，配置里那个只是启动初值 —— 两者不一样是正常的。
      </p>

      <div class="field-row">
        <label class="field grow">
          <span>手动偏移（秒，可负）</span>
          <input v-model.number="offsetInput" type="number" step="1" />
        </label>
        <button class="ghost" type="button" :disabled="busy === 'offset'" @click="applyOffset">
          <Icon name="save" :size="15" />{{ busy === 'offset' ? '应用中…' : '应用偏移' }}
        </button>
        <button class="ghost" type="button" :disabled="busy === 'reset'" @click="alignToHost">
          <Icon name="rotate-ccw" :size="15" />与主机对齐
        </button>
      </div>
      <p class="hint-block">
        「与主机对齐」把偏移归零并清掉「已同步」标记。NTP 模式下手动设置会被
        下一次对时覆盖 —— 想固定时间就切到「手动」。
      </p>
    </section>

    <section class="panel">
      <h2><Icon name="signal" :size="16" />对时（SNTP）</h2>
      <div class="field-row">
        <label class="field">
          <span>模式</span>
          <select v-model="form.mode">
            <option value="manual">手动（含 NVR 对时）</option>
            <option value="ntp">NTP 周期对时</option>
          </select>
        </label>
        <label class="field grow">
          <span>服务器</span>
          <input v-model="form.ntp_server" type="text" :disabled="form.mode !== 'ntp'" />
        </label>
        <label class="field">
          <span>间隔（秒，0 = 只对一次）</span>
          <input
            v-model.number="form.ntp_interval_s"
            type="number"
            min="0"
            max="86400"
            :disabled="form.mode !== 'ntp'"
          />
        </label>
      </div>

      <dl class="kv">
        <div><dt>最近一次对时</dt><dd>{{ syncText }}</dd></div>
        <div>
          <dt>成功 / 失败</dt>
          <dd class="mono">{{ info ? `${info.sync_ok_count} / ${info.sync_fail_count}` : '—' }}</dd>
        </div>
      </dl>

      <div class="field-row">
        <button class="primary" type="button" :disabled="saving || !dirty" @click="onSaveMode">
          <Icon name="save" :size="15" />{{ saving ? '保存中…' : '保存对时设置' }}
        </button>
        <button class="ghost" type="button" :disabled="busy === 'sync'" @click="syncNow">
          <Icon name="refresh" :size="15" />{{ busy === 'sync' ? '正在对时…' : '立即对时' }}
        </button>
      </div>
      <p class="hint-block">
        模式决定 ONVIF `GetSystemDateAndTime` 报 Manual 还是 NTP —— NVR 会照这个
        决定要不要来替我们对时。
      </p>
    </section>
  </div>

  <div v-else class="empty-state">
    <Icon name="loader" :size="26" /><span>正在读取时间设置…</span>
  </div>
</template>
