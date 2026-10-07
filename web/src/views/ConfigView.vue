<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { fetchConfig, restartOnvif, saveConfig } from '../api'
import { SECRET_SENTINEL } from '../types'
import type { AppConfig } from '../types'
import { useAuth } from '../composables/useAuth'
import { useStats } from '../composables/useStats'
import Icon from '../components/Icon.vue'

const { stats } = useStats()
const { ensureSession } = useAuth()

/** 与后端 config.h 里的默认值保持一致，只为在读取失败时有个形状参考。 */
function defaults(): AppConfig {
  return {
    capture: {
      display_index: 0,
      fps: 30,
      bitrate_kbps: 4000,
      gop_seconds: 2,
      threads: 0,
      preset: 'ultrafast',
      tune: 'zerolatency',
      profile: 'main',
    },
    rtsp: {
      enabled: false,
      port: 554,
      path: '/live',
      sub_enabled: true,
      sub_path: '/live_sub',
      sub_height: 360,
      sub_bitrate_kbps: 512,
    },
    web: {
      enabled: false,
      host: '0.0.0.0',
      port: 8080,
      static_root: 'web/dist',
      auth_enabled: true,
      user: 'admin',
      // 表单里不回填口令，这里只是形状参考。
      password: '',
    },
    onvif: {
      enabled: false,
      user: 'admin',
      password: '',
      manufacturer: 'Desk2ONVIF',
      model: 'Desk2ONVIF Virtual Camera',
      firmware_version: '1.0.0',
      hardware_id: '1.0',
      serial_number: '',
      device_name: 'Desk2ONVIF',
      location: 'Desktop',
    },
  }
}

const form = reactive<AppConfig>(defaults())
const loading = ref(true)
/** 配置没读到就不给编辑：拿默认值去覆盖一份真实配置，是最糟的失败方式。 */
const loaded = ref(false)
const loadError = ref('')
const saving = ref(false)
const saveError = ref('')
const saveOk = ref('')
/**
 * 已保存但**还没生效**的字段。
 *
 * 两个来源，缺一不可：`POST` 的回应给出"这次保存的结果"，`GET` 给出"累积到
 * 现在还欠着什么"。只有前者的话，用户存完刷新一下页面提示就没了，而表单里
 * 显示的又都是新值 —— 看起来一切已生效。
 */
const pendingFields = ref<string[]>([])
const restarting = ref(false)
const restartMsg = ref('')

/**
 * 口令框永远是空的，而且独立于 form —— 后端 GET 回来的是哨兵 "__unchanged__"，
 * 真口令永远不会到达浏览器，所以这里没有任何东西可以回填。
 * 用户不动这个框，提交时就发哨兵回去（含义：保持原值）。
 *
 * **两套凭据各有一份**（Web 登录 / ONVIF），它们在后端也是完全独立的两个字段。
 */
const onvifPasswordInput = ref('')
const hasStoredOnvifPassword = ref(false)
const webPasswordInput = ref('')
/**
 * Web 口令"存过没有"。
 *
 * 这里能（也只有这里能）分辨两件事，因为 GET 对**空的**口令不做脱敏：
 * 哨兵 = 存过，空串 = 就是空的。而"空的"在上面的服务里不是"不鉴权"，
 * 是"口令就是空" —— 所以这个区别必须显示出来（见下面的安全提示）。
 */
const hasStoredWebPassword = ref(false)
const webPasswordEmpty = ref(false)

function num(v: unknown, fallback: number): number {
  return typeof v === 'number' && Number.isFinite(v) ? v : fallback
}

function bool(v: unknown, fallback: boolean): boolean {
  return typeof v === 'boolean' ? v : fallback
}

function str(v: unknown, fallback: string): string {
  return typeof v === 'string' ? v : fallback
}

/** 后端可能少发字段（版本不一致时），逐个兜默认值，别让表单里出现 undefined。 */
function mergeConfig(raw: Partial<AppConfig> | undefined): AppConfig {
  const d = defaults()
  const c = raw ?? {}
  const cap = c.capture ?? ({} as AppConfig['capture'])
  const rtsp = c.rtsp ?? ({} as AppConfig['rtsp'])
  const web = c.web ?? ({} as AppConfig['web'])
  const onvif = c.onvif ?? ({} as AppConfig['onvif'])
  return {
    capture: {
      display_index: num(cap.display_index, d.capture.display_index),
      fps: num(cap.fps, d.capture.fps),
      bitrate_kbps: num(cap.bitrate_kbps, d.capture.bitrate_kbps),
      gop_seconds: num(cap.gop_seconds, d.capture.gop_seconds),
      threads: num(cap.threads, d.capture.threads),
      preset: str(cap.preset, d.capture.preset),
      tune: str(cap.tune, d.capture.tune),
      profile: str(cap.profile, d.capture.profile),
    },
    rtsp: {
      enabled: bool(rtsp.enabled, d.rtsp.enabled),
      port: num(rtsp.port, d.rtsp.port),
      path: str(rtsp.path, d.rtsp.path),
      sub_enabled: bool(rtsp.sub_enabled, d.rtsp.sub_enabled),
      sub_path: str(rtsp.sub_path, d.rtsp.sub_path),
      sub_height: num(rtsp.sub_height, d.rtsp.sub_height),
      sub_bitrate_kbps: num(rtsp.sub_bitrate_kbps, d.rtsp.sub_bitrate_kbps),
    },
    web: {
      enabled: bool(web.enabled, d.web.enabled),
      host: str(web.host, d.web.host),
      port: num(web.port, d.web.port),
      static_root: str(web.static_root, d.web.static_root),
      auth_enabled: bool(web.auth_enabled, d.web.auth_enabled),
      user: str(web.user, d.web.user),
      // 哨兵本身不进表单，它只是一个信号：后端存过口令。
      password: '',
    },
    onvif: {
      enabled: bool(onvif.enabled, d.onvif.enabled),
      user: str(onvif.user, d.onvif.user),
      password: '',
      manufacturer: str(onvif.manufacturer, d.onvif.manufacturer),
      model: str(onvif.model, d.onvif.model),
      firmware_version: str(onvif.firmware_version, d.onvif.firmware_version),
      hardware_id: str(onvif.hardware_id, d.onvif.hardware_id),
      serial_number: str(onvif.serial_number, d.onvif.serial_number),
      device_name: str(onvif.device_name, d.onvif.device_name),
      location: str(onvif.location, d.onvif.location),
    },
  }
}

async function load() {
  loading.value = true
  loadError.value = ''
  const res = await fetchConfig()
  if (!res.ok) {
    loadError.value = res.error
    loaded.value = false
    loading.value = false
    return
  }
  const merged = mergeConfig(res.data)
  Object.assign(form.capture, merged.capture)
  Object.assign(form.rtsp, merged.rtsp)
  Object.assign(form.web, merged.web)
  Object.assign(form.onvif, merged.onvif)
  // GET 的 password 字段只有两种可能：哨兵（存过）或空串（没存过）。
  // 无论哪种都不显示真值，只记一个"已设置"用于提示文案。
  const rawWeb = res.data?.web?.password
  const rawOnvif = res.data?.onvif?.password
  hasStoredWebPassword.value = rawWeb === SECRET_SENTINEL
  webPasswordEmpty.value = typeof rawWeb === 'string' && rawWeb.length === 0
  hasStoredOnvifPassword.value = rawOnvif === SECRET_SENTINEL
  // 待重启的项由后端在 GET 里一并给出（见 types.ts 里 `pending` 的说明）。
  // 老后端不发这个字段，所以缺省是空数组而不是报错。
  pendingFields.value = Array.isArray(res.data?.pending) ? res.data.pending : []
  webPasswordInput.value = ''
  onvifPasswordInput.value = ''
  loaded.value = true
  loading.value = false
}

onMounted(load)

type Errors = Record<string, string>

/**
 * 客户端校验。范围按约定写死，但这些只是"早一点告诉用户"，
 * 最终仍以后端 validate_config() 的夹取和它返回的错误为准。
 */
const errors = computed<Errors>(() => {
  const e: Errors = {}
  const int = (path: string, v: unknown, lo: number, hi: number, name: string) => {
    const n = typeof v === 'number' ? v : NaN
    if (!Number.isFinite(n) || !Number.isInteger(n)) {
      e[path] = `${name}必须是整数`
    } else if (n < lo || n > hi) {
      e[path] = `${name}应在 ${lo}–${hi} 之间`
    }
  }

  int('capture.display_index', form.capture.display_index, 0, 64, '显示器序号')
  int('capture.fps', form.capture.fps, 1, 120, '帧率')
  int('capture.bitrate_kbps', form.capture.bitrate_kbps, 100, 50000, '码率')
  int('capture.gop_seconds', form.capture.gop_seconds, 1, 10, 'GOP 间隔')
  int('capture.threads', form.capture.threads, 0, 128, '编码线程数')
  int('rtsp.port', form.rtsp.port, 1, 65535, 'RTSP 端口')
  int('rtsp.sub_height', form.rtsp.sub_height, 120, 2160, '子码流高度')
  int('rtsp.sub_bitrate_kbps', form.rtsp.sub_bitrate_kbps, 32, 100000, '子码流码率')
  int('web.port', form.web.port, 1, 65535, 'Web 端口')

  if (!form.rtsp.path) e['rtsp.path'] = '路径不能为空'
  else if (!form.rtsp.path.startsWith('/')) e['rtsp.path'] = '路径必须以 / 开头'

  if (!form.rtsp.sub_path) e['rtsp.sub_path'] = '路径不能为空'
  else if (!form.rtsp.sub_path.startsWith('/')) e['rtsp.sub_path'] = '路径必须以 / 开头'
  // 两条 ServerMediaSession 挂同一个路径时只有一个能生效，而无论哪个赢，
  // 现象都是一条能正常播的流 —— 分不出它是主还是子。后端也会拦，这里先拦一道。
  else if (form.rtsp.sub_enabled && form.rtsp.sub_path === form.rtsp.path) {
    e['rtsp.sub_path'] = '不能与主码流路径相同'
  }

  if (!form.web.host.trim()) e['web.host'] = '监听地址不能为空'
  if (!form.web.static_root.trim()) e['web.static_root'] = '静态目录不能为空'
  // 开了登录却不给用户名，等于让人对着一个填什么都进不去的框。空口令后端会
  // 有一整套说法（见下面的安全提示），但用户名为空是真的没法登。
  if (form.web.auth_enabled && !form.web.user.trim()) {
    e['web.user'] = '开启登录时用户名不能为空'
  }

  if (form.onvif.enabled && !form.onvif.user.trim()) e['onvif.user'] = '开启 ONVIF 时用户名不能为空'

  // 两个服务抢同一个端口，后起的必然 bind 失败。后端也会拦，但在这里说更清楚。
  if (form.rtsp.enabled && form.web.enabled && form.rtsp.port === form.web.port) {
    e['rtsp.port'] = '与 Web 端口冲突，两者不能相同'
  }
  return e
})

const errorCount = computed(() => Object.keys(errors.value).length)

/**
 * 把控制台暴露在局域网上、同时又不设门 —— 这一项的风险远高于其它任何配置：
 * 同一网段里任何人都能读配置、改配置、看桌面画面。
 *
 * 判据要用**改写之后**的效果值：用户可能正打算在这一次保存里把登录打开
 * （`webPasswordInput` 里填了口令），那就不该再报"敞着"。
 */
const exposedWithoutAuth = computed(() => {
  if (!form.web.enabled) return false
  if (form.web.host !== '0.0.0.0' && form.web.host !== '::') return false
  if (!form.web.auth_enabled) return true
  return !hasStoredWebPassword.value && webPasswordInput.value.length === 0
})

/** 表单里填的地址，给 NVR 手工添加通道时抄。 */
const rtspPreview = computed(() => {
  const p = form.rtsp.path || '/live'
  // 浏览器不知道本机的局域网 IP，只能用访问本页的地址来近似；真实的推流地址
  // 以状态页上后端给的 stats.rtsp_url 为准。
  return `rtsp://${location.hostname}:${form.rtsp.port}${p.startsWith('/') ? p : '/' + p}`
})

function buildPayload(): AppConfig {
  const payload = JSON.parse(JSON.stringify(form)) as AppConfig
  // 用户没动口令框，就把哨兵原样发回去。
  //
  // 这里**不能**改成"干脆不发这个键"：后端的 take() 对缺失键是保留默认值，
  // 而 password 那一项的默认值是空串，于是 "" != "__unchanged__" 成立，
  // 结果是把已存的口令清掉 —— 表现就是"改个帧率顺手保存，认证就失效了"。
  //
  // 两套凭据各发各的哨兵。混用会让"改 Web 口令"顺手把 NVR 那份也改掉。
  payload.web.password = webPasswordInput.value === '' ? SECRET_SENTINEL : webPasswordInput.value
  payload.onvif.password =
    onvifPasswordInput.value === '' ? SECRET_SENTINEL : onvifPasswordInput.value
  return payload
}

/**
 * 点分路径 → 人话。**认不出来的路径原样显示**，不是藏起来 —— 后端加了新字段
 * 而这张表没跟上时，用户至少还能看到 `rtsp.something_new` 这个名字，而不是
 * 一条莫名其妙的"有 1 项需要重启"却不说是什么。
 */
const FIELD_LABELS: Record<string, string> = {
  'capture.display_index': '显示器',
  'capture.fps': '帧率',
  'capture.bitrate_kbps': '码率',
  'capture.gop_seconds': '关键帧间隔',
  'capture.threads': '编码线程数',
  'capture.preset': '编码 preset',
  'capture.tune': '编码 tune',
  'capture.profile': 'H.264 档位',
  'rtsp.enabled': 'RTSP 开关',
  'rtsp.port': 'RTSP 端口',
  'rtsp.path': '主码流路径',
  'rtsp.sub_enabled': '子码流开关',
  'rtsp.sub_path': '子码流路径',
  'rtsp.sub_height': '子码流高度',
  'rtsp.sub_bitrate_kbps': '子码流码率',
  'web.enabled': 'Web 开关',
  'web.host': 'Web 监听地址',
  'web.port': 'Web 端口',
  'web.static_root': '静态文件目录',
  'web.auth_enabled': 'Web 登录开关',
  'web.user': 'Web 登录用户名',
  'web.password': 'Web 登录口令',
  'onvif.enabled': 'ONVIF 开关',
  'onvif.user': 'ONVIF 用户名',
  'onvif.password': 'ONVIF 口令',
  'onvif.manufacturer': '厂商',
  'onvif.model': '型号',
  'onvif.firmware_version': '固件版本',
  'onvif.hardware_id': '硬件版本',
  'onvif.serial_number': '序列号',
  'onvif.device_name': '设备名',
  'onvif.location': '位置',
}

function label(path: string): string {
  return FIELD_LABELS[path] ?? path
}

/** 一次保存里**当场生效**的字段 —— 和 pending 是互补的两半。 */
const appliedFields = ref<string[]>([])

async function onSave() {
  if (errorCount.value > 0) return
  saving.value = true
  saveError.value = ''
  saveOk.value = ''
  appliedFields.value = []
  const res = await saveConfig(buildPayload())
  saving.value = false
  if (!res.ok) {
    saveError.value = res.error
    return
  }
  appliedFields.value = Array.isArray(res.data?.applied) ? res.data.applied : []
  // 保存成功后重新拉一次：后端会夹取越界值，界面应该显示真正生效的那份，
  // 而不是用户刚敲进去、可能已经被改掉的那份。顺带把**待重启清单**也刷新成
  // 后端的权威版本 —— 它比这次的回应更全（还包含前几次留下的）。
  await load()
  // 登录设置是**当场生效**的，所以这一次保存很可能改变了"现在这个会话还算不算
  // 数"：把登录打开之后，手上这个没有令牌的会话会被踢回登录页。重新问一次
  // /api/session，让顶栏和路由守卫按真实情况动作，而不是等下一次刷页面。
  await ensureSession(true)
  // 文案直接采用后端的说法：哪些算"已生效"只有后端知道（比如 ONVIF 关着的
  // 时候改口令并不生效），前端自己编一句就会和后端打架。
  saveOk.value = res.data?.note ?? '已保存。'
  webPasswordInput.value = ''
  onvifPasswordInput.value = ''
}

async function onRestartOnvif() {
  restarting.value = true
  restartMsg.value = ''
  const res = await restartOnvif()
  restarting.value = false
  restartMsg.value = res.ok ? '已请求重启 ONVIF / WS-Discovery 响应器。' : `重启失败：${res.error}`
}
</script>

<template>
  <div class="page-head">
    <h1>配置</h1>
    <span class="hint">
      改完点「保存」写回后端。凭据（Web 登录、ONVIF 身份与口令）当场生效；
      分辨率、端口、码率、路径这些要重启程序 —— 保存后会逐项列出来。
    </span>
  </div>

  <div v-if="loading" class="banner">正在读取配置…</div>

  <template v-else-if="!loaded">
    <!-- 读不到就不给编辑：拿一份默认值去覆盖真实配置，比什么都不做糟得多。 -->
    <div class="banner is-err">
      <strong>读取配置失败。</strong>{{ loadError }}
      <br />为避免用一份凭空捏造的默认值覆盖掉真实配置，表单暂不可编辑。
    </div>
    <div class="actions">
      <button @click="load"><Icon name="refresh" :size="15" />重试</button>
    </div>
  </template>

  <form v-else @submit.prevent="onSave">
    <div v-if="saveError" class="banner is-err"><strong>保存失败。</strong>{{ saveError }}</div>
    <div v-else-if="saveOk" class="banner is-ok">{{ saveOk }}</div>
    <div v-else-if="errorCount > 0" class="banner is-warn">
      有 {{ errorCount }} 项填写不合法，修好才能保存（下面标红的位置）。
    </div>

    <!--
      待重启清单。**不能因为"刚才那条保存提示已经显示过了"就省掉它** ——
      这个横幅是从 GET 拿的，所以刷新页面、过一会儿再回来，它还在。而保存提示
      是一次性的：存完刷新一下，提示没了、表单里却都是新值，看起来一切已生效。
    -->
    <div v-if="pendingFields.length" class="banner is-warn">
      <strong>有 {{ pendingFields.length }} 项已保存，但需要重启程序才会生效：</strong>
      <ul class="pending-list">
        <li v-for="p in pendingFields" :key="p">{{ label(p) }}</li>
      </ul>
      分辨率、端口、码率、路径这些要重建编码器、重新 bind 或重建 RTSP 会话，
      做一半的失败模式是「界面上显示改了、实际还在用旧的」—— 那比要重启一次糟得多。
      凭据（Web 登录、ONVIF 身份与口令）则是**当场生效**的，不需要重启。
    </div>

    <div v-if="appliedFields.length" class="banner is-ok">
      本次保存中已当场生效：{{ appliedFields.map(label).join('、') }}
    </div>

    <fieldset>
      <legend>采集与编码</legend>
      <div class="grid">
        <label class="field">
          <span class="field-label">显示器序号（--list-displays 的编号）</span>
          <input
            v-model.number="form.capture.display_index"
            type="number"
            min="0"
            step="1"
            :class="{ invalid: errors['capture.display_index'] }"
          />
          <span v-if="errors['capture.display_index']" class="field-error">{{
            errors['capture.display_index']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">帧率 fps（1–120）</span>
          <input
            v-model.number="form.capture.fps"
            type="number"
            min="1"
            max="120"
            step="1"
            :class="{ invalid: errors['capture.fps'] }"
          />
          <span v-if="errors['capture.fps']" class="field-error">{{ errors['capture.fps'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">码率 kbps（100–50000）</span>
          <input
            v-model.number="form.capture.bitrate_kbps"
            type="number"
            min="100"
            max="50000"
            step="100"
            :class="{ invalid: errors['capture.bitrate_kbps'] }"
          />
          <span v-if="errors['capture.bitrate_kbps']" class="field-error">{{
            errors['capture.bitrate_kbps']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">GOP 间隔 秒（1–10）</span>
          <input
            v-model.number="form.capture.gop_seconds"
            type="number"
            min="1"
            max="10"
            step="1"
            :class="{ invalid: errors['capture.gop_seconds'] }"
          />
          <span class="hint">它也是浏览器预览的延迟上界：分片只在关键帧处产生</span>
          <span v-if="errors['capture.gop_seconds']" class="field-error">{{
            errors['capture.gop_seconds']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">编码线程数</span>
          <input
            v-model.number="form.capture.threads"
            type="number"
            min="0"
            max="128"
            step="1"
            :class="{ invalid: errors['capture.threads'] }"
          />
          <span class="hint">0 = 按「延迟优先」自动选（封顶 6）</span>
          <span v-if="errors['capture.threads']" class="field-error">{{
            errors['capture.threads']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">预设 preset</span>
          <select v-model="form.capture.preset">
            <option>ultrafast</option>
            <option>superfast</option>
            <option>veryfast</option>
            <option>faster</option>
            <option>fast</option>
            <option>medium</option>
            <option>slow</option>
            <option>slower</option>
            <option>veryslow</option>
          </select>
        </label>

        <label class="field">
          <span class="field-label">调优 tune</span>
          <select v-model="form.capture.tune">
            <option>zerolatency</option>
            <option>film</option>
            <option>animation</option>
            <option>grain</option>
            <option>stillimage</option>
            <option>fastdecode</option>
          </select>
        </label>

        <label class="field">
          <span class="field-label">档次 profile</span>
          <select v-model="form.capture.profile">
            <option>baseline</option>
            <option>main</option>
            <option>high</option>
          </select>
          <span class="hint">ultrafast 会压到 Constrained Baseline，与选哪个无关</span>
        </label>
      </div>
    </fieldset>

    <fieldset>
      <legend>RTSP</legend>
      <div class="grid">
        <label class="field check">
          <input v-model="form.rtsp.enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">开启 RTSP 推流</span>
        </label>

        <label class="field">
          <span class="field-label">端口（1–65535，默认 554）</span>
          <input
            v-model.number="form.rtsp.port"
            type="number"
            min="1"
            max="65535"
            step="1"
            :class="{ invalid: errors['rtsp.port'] }"
          />
          <span v-if="errors['rtsp.port']" class="field-error">{{ errors['rtsp.port'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">路径</span>
          <input
            v-model="form.rtsp.path"
            type="text"
            placeholder="/live"
            :class="{ invalid: errors['rtsp.path'] }"
          />
          <span v-if="errors['rtsp.path']" class="field-error">{{ errors['rtsp.path'] }}</span>
        </label>
      </div>

      <!--
        子码流单独一组。它不是"再开一个 RTSP"，而是**第二路编码器** ——
        两路的分辨率和码率是各自独立的，混在同一个 grid 里会让人以为填的是
        同一个东西的两半。
      -->
      <div class="grid" style="margin-top: 12px">
        <label class="field check">
          <input v-model="form.rtsp.sub_enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">开启子码流</span>
        </label>

        <label class="field">
          <span class="field-label">子码流路径</span>
          <input
            v-model="form.rtsp.sub_path"
            type="text"
            placeholder="/live_sub"
            :class="{ invalid: errors['rtsp.sub_path'] }"
          />
          <span v-if="errors['rtsp.sub_path']" class="field-error">{{
            errors['rtsp.sub_path']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">子码流高度（120–2160，宽度按主码流比例推）</span>
          <input
            v-model.number="form.rtsp.sub_height"
            type="number"
            min="120"
            max="2160"
            step="2"
            :class="{ invalid: errors['rtsp.sub_height'] }"
          />
          <span v-if="errors['rtsp.sub_height']" class="field-error">{{
            errors['rtsp.sub_height']
          }}</span>
        </label>

        <label class="field">
          <span class="field-label">子码流码率（kbps）</span>
          <input
            v-model.number="form.rtsp.sub_bitrate_kbps"
            type="number"
            min="32"
            max="100000"
            step="32"
            :class="{ invalid: errors['rtsp.sub_bitrate_kbps'] }"
          />
          <span v-if="errors['rtsp.sub_bitrate_kbps']" class="field-error">{{
            errors['rtsp.sub_bitrate_kbps']
          }}</span>
        </label>
      </div>

      <p class="hint">
        <strong>子码流是第二路编码器</strong>，专门伺候 NVR 的多画面网格：NVR 默认拉的是主码流，
        于是 16 路网格会同时拉 16 条全分辨率的流。开着它，网格走子码流、点开单画面才切主码流。
        它只有开了 RTSP 才有意义。
        <template v-if="stats.sub_width > 0">
          <br />当前实际尺寸 <code class="value">{{ stats.sub_width }}x{{ stats.sub_height }}</code
          >。
        </template>
      </p>

      <p class="hint" style="margin-bottom: 0">
        本机推流地址约等于 <code class="value">{{ rtspPreview }}</code>
        <template v-if="stats.rtsp_url">
          ；后端当前实际地址是 <code class="value">{{ stats.rtsp_url }}</code>（以这个为准）
        </template>
        <br />Windows 防火墙默认拦 554/TCP 入站，别的机器（包括 NVR）连不上时先放行该端口。
        子码流走同一个端口，不需要额外放行。
      </p>
    </fieldset>

    <fieldset>
      <legend>Web 服务</legend>
      <div class="grid">
        <label class="field check">
          <input v-model="form.web.enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">开启 Web 服务</span>
        </label>

        <label class="field">
          <span class="field-label">监听地址</span>
          <input
            v-model="form.web.host"
            type="text"
            placeholder="0.0.0.0"
            :class="{ invalid: errors['web.host'] }"
          />
          <span class="hint">改成 127.0.0.1 就只有本机能打开这个界面</span>
          <span v-if="errors['web.host']" class="field-error">{{ errors['web.host'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">端口（1–65535）</span>
          <input
            v-model.number="form.web.port"
            type="number"
            min="1"
            max="65535"
            step="1"
            :class="{ invalid: errors['web.port'] }"
          />
          <span v-if="errors['web.port']" class="field-error">{{ errors['web.port'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">静态资源目录</span>
          <input
            v-model="form.web.static_root"
            type="text"
            placeholder="web/dist"
            :class="{ invalid: errors['web.static_root'] }"
          />
          <span class="hint">前端构建产物的目录，相对后端进程的工作目录解析</span>
          <span v-if="errors['web.static_root']" class="field-error">{{
            errors['web.static_root']
          }}</span>
        </label>
      </div>

      <!--
        登录设置。和 ONVIF 那份凭据**故意分开**：NVR 里存着的那份是给机器用的
        （改一次要去每台 NVR 上改），网页这份是人手敲的。共用的后果是
        "为了改网页口令得把 NVR 全部重配一遍"，于是没人会去改它。
      -->
      <div class="grid" style="margin-top: 12px">
        <label class="field check">
          <input v-model="form.web.auth_enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">打开这个界面需要登录</span>
        </label>

        <label class="field">
          <span class="field-label">登录用户名</span>
          <input
            v-model="form.web.user"
            type="text"
            autocomplete="off"
            :class="{ invalid: errors['web.user'] }"
          />
          <span v-if="errors['web.user']" class="field-error">{{ errors['web.user'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">登录口令</span>
          <!-- 后端只回哨兵，真口令到不了浏览器，所以这里永远从空开始。 -->
          <input
            v-model="webPasswordInput"
            type="password"
            autocomplete="new-password"
            :placeholder="
              hasStoredWebPassword ? '已设置（留空则不修改）' : '未设置（留空则不修改）'
            "
          />
          <span class="hint">留空保持原口令不变；只有在这里输入新值才会改掉它。</span>
        </label>
      </div>

      <!--
        空口令在本服务里**不是**"不鉴权"：开关开着、口令是空的时候
        `SessionStore::effective()` 返回 false，于是前端会跳过登录页 ——
        也就是说它等于把门敞着，却让人以为有门。这条必须明说。
      -->
      <div v-if="form.web.auth_enabled && webPasswordEmpty && webPasswordInput === ''" class="banner is-warn" style="margin-top: 12px">
        <strong>登录是开着的，但口令是空的。</strong>
        空口令在这里不等于"不用登录"，而是"口令就是空" —— 任何知道用户名的人按一下
        回车就能进来。请在右边填一个口令再保存，或者干脆把登录关掉（那样前端会
        跳过登录页，至少不会让人误以为自己是安全的）。
      </div>

      <div v-if="exposedWithoutAuth" class="banner is-err" style="margin-top: 12px">
        <strong>这个控制台会对整个局域网敞开。</strong>
        监听地址是 <code class="value">{{ form.web.host }}</code>，同时没有启用登录 ——
        同一网段里任何一台机器都能读配置、改配置、看到你的桌面画面。
        两条退路任选：把监听地址改成 <code class="value">127.0.0.1</code>，
        或者把「需要登录」打开并设一个口令。
      </div>

      <p class="hint" style="margin-bottom: 0">
        登录设置<strong>当场生效</strong>，不需要重启。有两点要知道：
        改口令或者关掉登录<strong>不会把已经登进来的人踢下线</strong>（会话令牌签的是每进程
        一份的随机密钥，不是口令），要立刻清空所有人只能重启程序；
        反过来，<strong>打开登录会让当前这个会话立刻被要求重新登录</strong> ——
        存完这一下就会跳回登录页，用新口令进去即可。
      </p>
    </fieldset>

    <fieldset>
      <legend>ONVIF</legend>
      <div class="grid">
        <label class="field check">
          <input v-model="form.onvif.enabled" type="checkbox" />
          <span class="track"></span>
          <span class="field-label">开启 ONVIF / WS-Discovery</span>
        </label>

        <label class="field">
          <span class="field-label">用户名</span>
          <input
            v-model="form.onvif.user"
            type="text"
            autocomplete="off"
            :class="{ invalid: errors['onvif.user'] }"
          />
          <span v-if="errors['onvif.user']" class="field-error">{{ errors['onvif.user'] }}</span>
        </label>

        <label class="field">
          <span class="field-label">密码</span>
          <!-- 后端只回哨兵，真口令到不了浏览器，所以这里永远从空开始。 -->
          <input
            v-model="onvifPasswordInput"
            type="password"
            autocomplete="new-password"
            :placeholder="
              hasStoredOnvifPassword ? '已设置（留空则不修改）' : '未设置（留空则不修改）'
            "
          />
          <span class="hint">留空保持原密码不变；只有在这里输入新值才会改掉它。</span>
        </label>

        <label class="field">
          <span class="field-label">厂商</span>
          <input v-model="form.onvif.manufacturer" type="text" />
        </label>

        <label class="field">
          <span class="field-label">型号</span>
          <input v-model="form.onvif.model" type="text" />
        </label>

        <label class="field">
          <span class="field-label">固件版本</span>
          <input v-model="form.onvif.firmware_version" type="text" />
        </label>

        <label class="field">
          <span class="field-label">硬件 ID</span>
          <input v-model="form.onvif.hardware_id" type="text" />
        </label>

        <label class="field">
          <span class="field-label">序列号</span>
          <input v-model="form.onvif.serial_number" type="text" placeholder="留空 = 按机器名派生" />
          <span class="hint">ONVIF 的 UUID 必须跨重启不变，否则 NVR 会当成新设备重复添加</span>
        </label>

        <label class="field">
          <span class="field-label">设备名称</span>
          <input v-model="form.onvif.device_name" type="text" />
        </label>

        <label class="field">
          <span class="field-label">位置</span>
          <input v-model="form.onvif.location" type="text" />
        </label>
      </div>

      <div class="actions" style="margin-top: 14px">
        <button type="button" :disabled="restarting" @click="onRestartOnvif">
          <Icon name="refresh" :size="15" />{{ restarting ? '正在重启…' : '重启 ONVIF 响应器' }}
        </button>
        <span class="hint">
          NVR 搜不到设备时先按这个（重启 WS-Discovery，需要重新发一次 Hello）。
        </span>
      </div>
      <div v-if="restartMsg" class="banner" style="margin: 12px 0 0">{{ restartMsg }}</div>
    </fieldset>

    <div class="actions">
      <button type="submit" class="primary" :disabled="saving || errorCount > 0">
        <Icon name="save" :size="15" />{{ saving ? '保存中…' : '保存' }}
      </button>
      <button type="button" :disabled="saving" @click="load">
        <Icon name="refresh" :size="15" />放弃修改并重载
      </button>
    </div>
  </form>
</template>
