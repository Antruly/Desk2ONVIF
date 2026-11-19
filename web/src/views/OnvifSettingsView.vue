<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { restartOnvif } from '../api'
import { useConfig } from '../composables/useConfig'

/**
 * ONVIF 身份与凭据。
 *
 * 和系统设置一样需要「保存」，但它的生效方式是**热**的：身份、口令、设备名
 * 都在 Web 循环线程上被读，改完立刻生效（见后端 `apply_onvif_identity`）。
 * 保存之后**建议点一次「重新广播」**，否则 NVR 手里那份还是旧名字 —— 它不会
 * 主动重新 Probe 一台已经认识的设备。
 */

const { config, load, save } = useConfig()

const form = reactive({
  enabled: true,
  user: 'admin',
  password: '',
  device_name: 'Desk2ONVIF',
  location: 'Desktop',
  manufacturer: 'Desk2ONVIF',
  model: 'Desk2ONVIF Virtual Camera',
  firmware_version: '1.0.0',
})

const saving = ref(false)
const note = ref('')
const error = ref('')
const broadcastNote = ref('')
const broadcasting = ref(false)

function sync() {
  const c = config.value
  if (!c) return
  form.enabled = c.onvif.enabled
  form.user = c.onvif.user
  form.password = ''
  form.device_name = c.onvif.device_name
  form.location = c.onvif.location
  form.manufacturer = c.onvif.manufacturer
  form.model = c.onvif.model
  form.firmware_version = c.onvif.firmware_version
}

onMounted(async () => {
  await load()
  sync()
})

const dirty = computed(() => {
  const c = config.value
  if (!c) return false
  return (
    form.enabled !== c.onvif.enabled ||
    form.user !== c.onvif.user ||
    form.password !== '' ||
    form.device_name !== c.onvif.device_name ||
    form.location !== c.onvif.location
  )
})

async function onSave() {
  if (saving.value) return
  saving.value = true
  error.value = ''
  note.value = ''
  const onvif: Record<string, unknown> = {
    enabled: form.enabled,
    user: form.user,
    device_name: form.device_name,
    location: form.location,
  }
  // 口令留空 = 不改（同系统设置页那条注释）。
  if (form.password) onvif.password = form.password
  const out = await save({ onvif })
  saving.value = false
  if (!out.ok) {
    error.value = out.error ?? '保存失败。'
    return
  }
  note.value = out.note ?? '已保存。'
  form.password = ''
  sync()
}

async function onBroadcast() {
  broadcasting.value = true
  broadcastNote.value = ''
  const res = await restartOnvif()
  broadcasting.value = false
  broadcastNote.value = res.ok
    ? '已发出 Bye + Hello。NVR 会在数秒后重新收录本设备。'
    : `广播失败：${res.error}`
}
</script>

<template>
  <section class="page-head">
    <div>
      <h1>ONVIF 设置</h1>
      <p class="sub">
        本机在 NVR 里长什么样、用哪套凭据。身份与口令<strong>保存后立即生效</strong>；
        设备标识（硬件 ID / 序列号 / UUID）由本机硬件固定，界面上不可改。
      </p>
    </div>
  </section>

  <div v-if="error" class="banner is-err"><strong>保存失败。</strong>{{ error }}</div>
  <div v-else-if="note" class="banner is-ok">{{ note }}</div>
  <div v-if="broadcastNote" class="banner is-ok">{{ broadcastNote }}</div>

  <form v-if="config" class="settings-grid" @submit.prevent="onSave">
    <section class="panel">
      <h2><Icon name="shield" :size="16" />服务与凭据</h2>
      <label class="toggle">
        <input v-model="form.enabled" type="checkbox" />
        <span>启用 ONVIF 端点</span>
      </label>
      <p class="hint-block">
        ONVIF 和网页控制台共用同一个 HTTP 端口（<code>POST /onvif/device_service</code>），
        不需要另开端口。
      </p>

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
        这份凭据是给 <strong>NVR</strong> 用的，和网页登录那一份<strong>互相独立</strong>。
        改这里不会影响网页登录，也不会把 NVR 上已经存着的连接踢下线。
      </p>
    </section>

    <section class="panel">
      <h2><Icon name="camera" :size="16" />设备身份</h2>
      <div class="field-row">
        <label class="field grow">
          <span>设备名称</span>
          <input v-model="form.device_name" type="text" maxlength="48" />
          <small>同时是水印里「名称」那一项的内容</small>
        </label>
        <label class="field grow">
          <span>位置</span>
          <input v-model="form.location" type="text" maxlength="64" />
        </label>
      </div>

      <dl class="kv">
        <div><dt>厂商</dt><dd class="mono">{{ form.manufacturer }}</dd></div>
        <div><dt>型号</dt><dd class="mono">{{ form.model }}</dd></div>
        <div><dt>固件版本</dt><dd class="mono">{{ form.firmware_version }}</dd></div>
        <div><dt>硬件 ID</dt><dd class="mono break">{{ config.onvif.hardware_id }}</dd></div>
        <div><dt>序列号</dt><dd class="mono break">{{ config.onvif.serial_number || '（自动派生）' }}</dd></div>
        <div><dt>设备 UUID</dt><dd class="mono break">{{ config.onvif.device_uuid || '（自动派生）' }}</dd></div>
      </dl>
      <p class="hint-block">
        这三项由本机硬件固定，网页上改不了 —— 改了会让 NVR 把同一台机器当成
        “新摄像机”重复添加。确实要改请直接编辑 <code>config.json</code>。
      </p>
    </section>

    <div class="save-bar inline-save">
      <span class="muted">{{ dirty ? '有未保存的改动' : '没有未保存的改动' }}</span>
      <button class="primary" type="submit" :disabled="saving || !dirty">
        <Icon name="save" :size="15" />{{ saving ? '保存中…' : '保存' }}
      </button>
    </div>
  </form>

  <section v-if="config" class="panel">
    <h2><Icon name="signal" :size="16" />重新广播</h2>
    <p class="muted">
      改了设备名/口令之后，NVR 不会主动来认；点一下这里让它先 Bye 再 Hello。
      画面不会中断（不像「重启服务」）。
    </p>
    <button class="ghost" type="button" :disabled="broadcasting" @click="onBroadcast">
      <Icon name="refresh" :size="15" />{{ broadcasting ? '正在广播…' : '重新广播 WS-Discovery' }}
    </button>
  </section>

  <div v-else class="empty-state">
    <Icon name="loader" :size="26" /><span>正在读取配置…</span>
  </div>
</template>
