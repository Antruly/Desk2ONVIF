<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import Icon from '../components/Icon.vue'
import { factoryReset, restartService } from '../api'
import { useConfig } from '../composables/useConfig'
import { useAuth } from '../composables/useAuth'
import { FACTORY_RESET_CONFIRM } from '../types'

/**
 * 系统设置：**这一类才需要点保存**。
 *
 * 用户的原话是"只有系统设置类才需要我保存"。这一页管的是端口、登录口令、
 * 自适应降载、审计留存和进程生命周期 —— 它们要么影响整个服务（端口、口令），
 * 要么根本不是热改的（进程重启），所以不能像通道参数那样点一下就生效。
 */

const router = useRouter()
const { config, load, save } = useConfig()
const { ensureSession } = useAuth()

const form = reactive({
  host: '0.0.0.0',
  port: 8080,
  auth_enabled: true,
  user: 'admin',
  password: '',
  adaptive_mode: 'throttle',
  adaptive_min_fps: 5,
  audit: true,
  audit_keep_days: 180,
})

const saving = ref(false)
const saveNote = ref('')
const saveError = ref('')
const pending = ref<string[]>([])

function sync() {
  const c = config.value
  if (!c) return
  form.host = c.web.host
  form.port = c.web.port
  form.auth_enabled = c.web.auth_enabled
  form.user = c.web.user
  form.password = ''
  form.adaptive_mode = c.adaptive.mode
  form.adaptive_min_fps = c.adaptive.min_fps
  form.audit = c.log.audit
  form.audit_keep_days = c.log.audit_keep_days
  pending.value = Array.isArray(c.pending) ? c.pending : []
}

onMounted(async () => {
  await load()
  sync()
})

const dirty = computed(() => {
  const c = config.value
  if (!c) return false
  return (
    form.host !== c.web.host ||
    form.port !== c.web.port ||
    form.auth_enabled !== c.web.auth_enabled ||
    form.user !== c.web.user ||
    form.password !== '' ||
    form.adaptive_mode !== c.adaptive.mode ||
    form.adaptive_min_fps !== c.adaptive.min_fps ||
    form.audit !== c.log.audit ||
    form.audit_keep_days !== c.log.audit_keep_days
  )
})

async function onSave() {
  if (saving.value) return
  for (const [label, v] of [
    ['端口', form.port],
    ['帧率下限', form.adaptive_min_fps],
    ['审计留存天数', form.audit_keep_days],
  ] as const) {
    if (!Number.isFinite(Number(v))) {
      saveError.value = `「${label}」要填一个数字。`
      return
    }
  }
  saveError.value = ''
  saveNote.value = ''
  saving.value = true
  const web: Record<string, unknown> = {
    host: form.host,
    port: form.port,
    auth_enabled: form.auth_enabled,
    user: form.user,
  }
  // **口令留空 = 不改。** 发一个空串会把它真的改成空口令（而这个服务里空口令
  // 是合法值，所以不会报错 —— 只会静默地变成"谁都能进"）。
  if (form.password) web.password = form.password
  const out = await save({
    web,
    adaptive: { mode: form.adaptive_mode, min_fps: form.adaptive_min_fps },
    log: { audit: form.audit, audit_keep_days: form.audit_keep_days },
  })
  saving.value = false
  if (!out.ok) {
    saveError.value = out.error ?? '保存失败。'
    return
  }
  saveNote.value = out.note ?? '已保存。'
  pending.value = out.pending
  form.password = ''
  await ensureSession(true)
  sync()
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
type Phase = 'idle' | 'restart' | 'reset' | 'done' | 'fail'
const phase = ref<Phase>('idle')
const phaseMsg = ref('')
const resetOpen = ref(false)
const resetText = ref('')

async function onRestart() {
  phase.value = 'restart'
  phaseMsg.value = '正在重启服务…'
  const res = await restartService()
  if (!res.ok) {
    phase.value = 'fail'
    phaseMsg.value = `重启请求失败：${res.error}`
    return
  }
  phase.value = 'done'
  phaseMsg.value =
    '重启请求已发出。服务重新起来之后所有网页会话都会失效，请重新登录。'
  window.setTimeout(() => router.push('/login'), 2500)
}

async function onFactoryReset() {
  if (resetText.value !== FACTORY_RESET_CONFIRM) return
  phase.value = 'reset'
  phaseMsg.value = '正在恢复出厂…'
  const res = await factoryReset()
  if (!res.ok) {
    phase.value = 'fail'
    phaseMsg.value = `恢复出厂失败：${res.error}`
    return
  }
  resetOpen.value = false
  phase.value = 'done'
  phaseMsg.value = '恢复出厂请求已发出。配置文件会被删掉，服务随后重启。'
  window.setTimeout(() => router.push('/login'), 2500)
}
</script>

<template>
  <section class="page-head">
    <div>
      <h1>系统设置</h1>
      <p class="sub">端口、登录、自适应降载、审计留存与进程生命周期。这一类改动需要保存。</p>
    </div>
  </section>

  <div v-if="saveError" class="banner is-err"><strong>保存失败。</strong>{{ saveError }}</div>
  <div v-else-if="saveNote" class="banner is-ok">{{ saveNote }}</div>
  <div v-if="pending.length" class="banner is-warn">
    <strong>有 {{ pending.length }} 项已保存，但需要重启程序才生效：</strong>
    <span class="mono">{{ pending.join('、') }}</span>
  </div>

  <form v-if="config" class="settings-grid" @submit.prevent="onSave">
    <section class="panel">
      <h2><Icon name="sliders" :size="16" />网络与登录</h2>
      <div class="field-row">
        <label class="field grow">
          <span>监听地址</span>
          <input v-model="form.host" type="text" placeholder="0.0.0.0" />
          <small>0.0.0.0 = 局域网可访问；127.0.0.1 = 只本机</small>
        </label>
        <label class="field">
          <span>端口</span>
          <input v-model.number="form.port" type="number" min="1" max="65535" />
        </label>
      </div>

      <label class="toggle">
        <input v-model="form.auth_enabled" type="checkbox" />
        <span>网页控制台需要登录</span>
      </label>
      <div class="field-row">
        <label class="field grow">
          <span>用户名</span>
          <input v-model="form.user" type="text" autocomplete="username" />
        </label>
        <label class="field grow">
          <span>口令（留空 = 不修改）</span>
          <input
            v-model="form.password"
            type="password"
            autocomplete="new-password"
            placeholder="保持当前口令"
          />
        </label>
      </div>
      <p class="hint-block">
        口令与开关注入当场生效；<strong>端口和监听地址要重启</strong>。改口令不会
        把已登录的人踢下线（要踢掉所有人只能重启进程）。
      </p>
    </section>

    <section class="panel">
      <h2><Icon name="zap" :size="16" />性能自适应</h2>
      <div class="field-row">
        <label class="field grow">
          <span>降载模式</span>
          <select v-model="form.adaptive_mode">
            <option value="off">不干预</option>
            <option value="throttle">自动丢帧（不够时只提示）</option>
            <option value="auto">自动降档（不用确认）</option>
          </select>
        </label>
        <label class="field">
          <span>帧率下限</span>
          <input v-model.number="form.adaptive_min_fps" type="number" min="1" max="60" />
        </label>
      </div>
      <p class="hint-block">
        自动降档会在无人操作时重建管线（录像分段、RTSP 客户端重连）；「不干预」
        只是不在界面上提示。
      </p>
    </section>

    <section class="panel">
      <h2><Icon name="list" :size="16" />审计日志</h2>
      <label class="toggle">
        <input v-model="form.audit" type="checkbox" />
        <span>记录操作日志（谁、从哪来、改了什么）</span>
      </label>
      <label class="field">
        <span>留存天数（0 = 永不剪枝）</span>
        <input v-model.number="form.audit_keep_days" type="number" min="0" max="3650" />
      </label>
    </section>

    <div class="save-bar inline-save">
      <span class="muted">
        {{ dirty ? '有未保存的改动' : '没有未保存的改动' }}
      </span>
      <button class="primary" type="submit" :disabled="saving || !dirty">
        <Icon name="save" :size="15" />{{ saving ? '保存中…' : '保存' }}
      </button>
    </div>
  </form>

  <section v-if="config" class="panel danger-zone">
    <h2><Icon name="alert" :size="16" />维护</h2>
    <p class="muted">
      下面两个动作会让<strong>进程本身</strong>重启：录制分段、RTSP 客户端断开重连、
      所有网页会话失效。只想让通道参数生效不用走这里 —— 那些是自动的。
    </p>

    <div v-if="phase !== 'idle'" class="banner" :class="phase === 'fail' ? 'is-err' : 'is-ok'">
      {{ phaseMsg }}
    </div>

    <div class="danger-actions">
      <button class="ghost" type="button" :disabled="phase === 'restart' || phase === 'reset'" @click="onRestart">
        <Icon name="refresh" :size="15" />重启服务
      </button>
      <button class="ghost is-danger" type="button" @click="resetOpen = true">
        <Icon name="alert" :size="15" />恢复出厂
      </button>
    </div>

    <div v-if="resetOpen" class="confirm-box">
      <p>
        恢复出厂会<strong>删掉配置文件与审计日志</strong>，然后重启服务。这个动作
        不可撤销。输入 <code>{{ FACTORY_RESET_CONFIRM }}</code> 确认：
      </p>
      <div class="field-row">
        <input v-model="resetText" type="text" :placeholder="FACTORY_RESET_CONFIRM" />
        <button
          class="ghost is-danger"
          type="button"
          :disabled="resetText !== FACTORY_RESET_CONFIRM"
          @click="onFactoryReset"
        >
          确认恢复出厂
        </button>
        <button class="ghost" type="button" @click="resetOpen = false">取消</button>
      </div>
    </div>
  </section>

  <div v-else class="empty-state">
    <Icon name="loader" :size="26" /><span>正在读取配置…</span>
  </div>
</template>
