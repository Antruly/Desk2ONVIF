<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { fetchAudioDevices } from '../api'
import { useConfig } from '../composables/useConfig'
import type { AudioDevice } from '../types'

/**
 * 音频设置。**它是整机一份**，不是每通道一份：
 *
 * 音轨只挂在通道 1（本机桌面）那条会话上，随 RTSP 主码流一起出去。通道 2 起
 * 的会话在 `options_for_channel` 里把 `audio.enabled` 强制关掉了 —— 一路桌面
 * 声音同时挂到 N 个通道上，NVR 那边会听到 N 份互相错位的回放。
 *
 * 改音频要**重开管线**（设备、编码器与 SDP 都是建管线时定下来的），所以这里的
 * 「应用」= 保存 + `POST /api/reload`。
 */

const { config, load, applyMedia } = useConfig()

const form = reactive({
  enabled: false,
  system: false,
  microphone: false,
  device: '',
  bitrate_kbps: 64,
  mic_gain: 1,
  system_gain: 1,
})

const devices = ref<AudioDevice[]>([])
const deviceError = ref('')
const applying = ref(false)
const note = ref('')
const error = ref('')

function sync() {
  const c = config.value
  if (!c) return
  form.enabled = c.audio.enabled
  form.system = c.audio.system
  form.microphone = c.audio.microphone
  form.device = c.audio.device
  form.bitrate_kbps = c.audio.bitrate_kbps
  form.mic_gain = c.audio.mic_gain
  form.system_gain = c.audio.system_gain
}

async function loadDevices() {
  deviceError.value = ''
  const res = await fetchAudioDevices()
  if (!res.ok) {
    deviceError.value = res.error
    return
  }
  devices.value = res.data.capture
  if (res.data.capture_error) deviceError.value = res.data.capture_error
}

onMounted(async () => {
  await load()
  sync()
  await loadDevices()
})

const dirty = computed(() => {
  const c = config.value
  if (!c) return false
  return (
    form.enabled !== c.audio.enabled ||
    form.system !== c.audio.system ||
    form.microphone !== c.audio.microphone ||
    form.device !== c.audio.device ||
    form.bitrate_kbps !== c.audio.bitrate_kbps ||
    form.mic_gain !== c.audio.mic_gain ||
    form.system_gain !== c.audio.system_gain
  )
})

/** 至少一个来源。打开总开关却一个源都不勾，是一条自相矛盾的配置。 */
const sourceOk = computed(() => form.system || form.microphone)

async function onApply() {
  if (applying.value || !dirty.value) return
  // 空输入框是空串，后端会以"类型不对"整份拒绝 —— 先挡住并说清是哪一项。
  for (const [label, v] of [
    ['AAC 码率', form.bitrate_kbps],
    ['麦克风增益', form.mic_gain],
    ['系统声音增益', form.system_gain],
  ] as const) {
    if (!Number.isFinite(Number(v))) {
      error.value = `「${label}」要填一个数字。`
      return
    }
  }
  applying.value = true
  error.value = ''
  note.value = ''
  const out = await applyMedia({
    audio: {
      enabled: form.enabled,
      system: form.system,
      microphone: form.microphone,
      device: form.device,
      bitrate_kbps: form.bitrate_kbps,
      mic_gain: form.mic_gain,
      system_gain: form.system_gain,
    },
  })
  applying.value = false
  if (!out.ok) {
    error.value = out.error ?? '应用失败。'
    return
  }
  note.value = out.note ?? '已保存，管线正在重开。'
  sync()
}
</script>

<template>
  <section class="page-head">
    <div>
      <h1>音频设置</h1>
      <p class="sub">
        系统声音与麦克风经 AAC 编码后进通道 1 的 RTSP 主码流与录像。它是
        <strong>整机一份</strong>，不按通道分。改完点「应用」，管线会重开一次。
      </p>
    </div>
  </section>

  <div v-if="error" class="banner is-err"><strong>应用失败。</strong>{{ error }}</div>
  <div v-else-if="note" class="banner is-ok">{{ note }}</div>

  <form v-if="config" class="settings-grid" @submit.prevent="onApply">
    <section class="panel">
      <h2><Icon name="volume" :size="16" />音轨</h2>

      <label class="toggle">
        <input v-model="form.enabled" type="checkbox" />
        <span>启用音频</span>
      </label>
      <p class="hint-block">
        默认关闭。打开会改变每一路设备的 SDP —— 如果 NVR 加了音频之后没画面，
        先把它关掉试试。关闭也同时关掉录像里的音轨（一个开关一条语义）。
      </p>

      <div class="field-row wrap">
        <label class="toggle">
          <input v-model="form.system" type="checkbox" />
          <span>系统声音（环回）</span>
        </label>
        <label class="toggle">
          <input v-model="form.microphone" type="checkbox" />
          <span>麦克风</span>
        </label>
      </div>
      <p v-if="form.enabled && !sourceOk" class="banner is-warn">
        开了总开关却一个来源都没勾 —— 这条音轨没有内容，后端会拒绝启动它。
      </p>

      <div class="field-row">
        <label class="field grow">
          <span>麦克风设备</span>
          <select v-model="form.device" :disabled="!form.microphone">
            <option value="">系统默认端点（推荐）</option>
            <option v-for="d in devices" :key="d.id" :value="d.id">
              {{ d.name }}{{ d.default ? '（默认）' : '' }}
            </option>
          </select>
        </label>
        <label class="field">
          <span>AAC 码率（kbps）</span>
          <input v-model.number="form.bitrate_kbps" type="number" min="8" max="512" />
        </label>
      </div>
      <p class="hint-block">
        设备留「系统默认」最省事：换耳机、拔麦之后系统默认端点会跟着走；
        写死了 id 的话就变成「设备不存在」。
        <span v-if="deviceError" class="is-err-text">（枚举设备时出错：{{ deviceError }}）</span>
      </p>

      <div class="field-row">
        <label class="field">
          <span>麦克风增益</span>
          <input v-model.number="form.mic_gain" type="number" min="0" max="8" step="0.1" />
        </label>
        <label class="field">
          <span>系统声音增益</span>
          <input v-model.number="form.system_gain" type="number" min="0" max="8" step="0.1" />
        </label>
      </div>
      <p class="hint-block">
        两个增益分开给：麦克风常常偏轻、系统环回常常偏响。1.0 = 原样，0 = 静音。
      </p>
    </section>

    <div class="save-bar inline-save">
      <span class="muted">{{ dirty ? '有未应用的改动' : '没有未应用的改动' }}</span>
      <button class="primary" type="submit" :disabled="applying || !dirty">
        <Icon name="zap" :size="15" />{{ applying ? '正在应用…' : '保存并应用' }}
      </button>
    </div>
    <p class="muted small">
      应用会重开采集管线：录像分段（新文件从 main.1.mp4 续）、RTSP 客户端断开
      数秒后重连。
    </p>
  </form>

  <div v-else class="empty-state">
    <Icon name="loader" :size="26" /><span>正在读取配置…</span>
  </div>
</template>
