<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import BrandMark from '../components/BrandMark.vue'
import Icon from '../components/Icon.vue'
import { useAuth } from '../composables/useAuth'
import { useTheme } from '../composables/useTheme'
import { clearLifecycleNote, peekLifecycleNote } from '../lifecycle'

/**
 * 登录页。
 *
 * ## 它自己不能被"未登录"挡住
 *
 * 静态资源（含 index.html 和这个页面的 chunk）在后端是**匿名可达**的 ——
 * 否则未登录的人连输入框都拿不到。这一条在 `tests/web_auth.py` 的第 [3] 段
 * 是单独一项断言，因为它的失败形态特别隐蔽：页面是**空白**的，而不是 401。
 *
 * ## 口令错了和"被限速了"是两种失败
 *
 * 后端连续 5 次失败后把来源 IP 冷却 60 秒，之后**连正确的口令也拒**。这里必须
 * 把这两种分开显示，否则用户会在"我明明打对了"和"它说不对"之间反复试，
 * 而每试一次冷却都是重新算的 —— 越试越进不去。
 *
 * ## 版式
 *
 * 左边是产品说明，右边是表单。左边那块**不参与任何鉴权判断** —— 它只是一组
 * 静态文案；把"后端连不上"之类的状态也画在左边，会让两种失败看起来像装饰。
 */

const route = useRoute()
const router = useRouter()
const { authRequired, sessionError, ensureSession, login } = useAuth()
const { isLight, toggleTheme } = useTheme()

const name = ref('admin')
const password = ref('')
const busy = ref(false)
const errorText = ref('')
/** 被限速时的倒计时秒数。>0 时按钮禁用并显示秒数。 */
const cooldown = ref(0)
const passwordInput = ref<HTMLInputElement | null>(null)

let tickTimer: number | null = null

const canSubmit = computed(() => !busy.value && cooldown.value === 0 && password.value.length > 0)

/**
 * 「你为什么会被送到这个页面」。
 *
 * 两个来源，**模块里的那一条优先**：配置页在按下「重启服务 / 恢复出厂」时就把
 * 解释写进了 `lifecycle.ts`（那里写了为什么不能用路由 query 当唯一载体 ——
 * 把人踢到登录页的有两条互相竞争的路，赌哪条先到迟早会赌输）。
 *
 * 路由上的 `reason` 是退路，不是主路：它只在**刷新过登录页**之后还有用 ——
 * 那时模块状态还在（SPA 没重载）……但如果用户是手动把 URL 拷到新标签页打开的，
 * 模块状态就没了，只剩 URL 上这一句。两条路说的是同一件事，不会打架。
 */
const lifecycleNote = peekLifecycleNote()

const lifecycleBanner = computed<{ reason: 'restarted' | 'reset'; text: string } | null>(() => {
  if (lifecycleNote) return lifecycleNote
  const raw = route.query.reason
  const reason = Array.isArray(raw) ? raw[0] : raw
  if (reason === 'restarted') {
    return {
      reason: 'restarted',
      text: '服务刚刚重启过，会话令牌在那一下全部失效了（令牌签的是每进程一份的随机密钥）—— 用原来的账号重新登录即可。',
    }
  }
  if (reason === 'reset') {
    return {
      reason: 'reset',
      text: '服务刚刚恢复出厂设置并重启，会话令牌在那一下全部失效了。口令也回到了默认值（安装包形态是 admin / admin），除非当时是用命令行参数指定的。',
    }
  }
  return null
})

const submitLabel = computed(() => {
  if (cooldown.value > 0) return `请等待 ${cooldown.value} 秒`
  return busy.value ? '正在登录…' : '登 录'
})

function startCooldown(seconds: number) {
  cooldown.value = Math.max(1, Math.ceil(seconds))
  if (tickTimer !== null) window.clearInterval(tickTimer)
  tickTimer = window.setInterval(() => {
    cooldown.value -= 1
    if (cooldown.value <= 0) {
      cooldown.value = 0
      if (tickTimer !== null) {
        window.clearInterval(tickTimer)
        tickTimer = null
      }
    }
  }, 1000)
}

async function onSubmit() {
  if (!canSubmit.value) return
  busy.value = true
  errorText.value = ''

  const res = await login(name.value.trim(), password.value)

  busy.value = false
  if (res.ok) {
    // 口令不留在这份状态里，提交完立刻清掉。
    password.value = ''
    // "服务刚重启过"那条解释到此为止。不清的话，下一次因为别的缘故（令牌过期、
    // 改了口令）被踢到登录页时，顶上会挂着一条张冠李戴的说明。
    clearLifecycleNote()
    // 登录前想去哪就回哪；没带 next 就回首页（首页是预览）。
    const raw = route.query.next
    const next = Array.isArray(raw) ? raw[0] : raw
    // `replace` 而不是 `push`：登录页不该留在后退栈里。
    await router.replace(typeof next === 'string' && next.startsWith('/') ? next : '/')
    return
  }

  errorText.value = res.error
  password.value = ''
  if (res.retryAfterS && res.retryAfterS > 0) startCooldown(res.retryAfterS)
  // 口令清掉之后焦点留在口令框上，用户可以直接重打 —— 而不用先点一下它。
  nextTick(() => passwordInput.value?.focus())
}

/** 重新问一次后端。后端刚起来时用户会对着登录框刷新，不如给个按钮。 */
async function retrySession() {
  await ensureSession(true)
}

onMounted(() => {
  // 用 JS 聚焦，**不用 `autofocus` 属性**：那个属性只在文档解析时对静态
  // HTML 生效，对 Vue 动态插入的元素不可靠（各浏览器行为还不一致）。
  if (authRequired.value) nextTick(() => passwordInput.value?.focus())
})

onBeforeUnmount(() => {
  if (tickTimer !== null) window.clearInterval(tickTimer)
})
</script>

<template>
  <div class="auth-page">
    <div class="auth-page__noise"></div>

    <!-- 左栏：产品说明。纯静态，不参与登录状态判断。 -->
    <section class="auth-art">
      <div class="auth-art__top">
        <BrandMark :size="40" />
        <span class="auth-wordmark">Desk2ONVIF</span>
        <span class="chip chip--accent">Profile S</span>
      </div>

      <div class="auth-art__copy">
        <p class="eyebrow">本机桌面 · ONVIF 设备模拟</p>
        <h1>
          把桌面<br />
          <span class="text-gradient">变成一路摄像机。</span>
        </h1>
        <p class="auth-art__lead">
          采集本机桌面或 USB 摄像头，编码成 H.264，同时向 NVR 提供 ONVIF 与
          RTSP 两种入口。不需要额外的视频服务器，也不经过任何云端。
        </p>
      </div>

      <div class="auth-art__grid">
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="eye" :size="18" /></span>
          <div>
            <strong>多路实时预览</strong>
            <p>浏览器里直接看 fMP4，不开插件，也不用先连 NVR。</p>
          </div>
        </div>
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="shield" :size="18" /></span>
          <div>
            <strong>ONVIF Profile S</strong>
            <p>设备发现、媒体配置、RTSP 地址都由本机直接应答。</p>
          </div>
        </div>
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="monitor" :size="18" /></span>
          <div>
            <strong>本机离线部署</strong>
            <p>页面与接口都跑在同一台机器上，不依赖外部 CDN。</p>
          </div>
        </div>
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="list" :size="18" /></span>
          <div>
            <strong>审计与排障</strong>
            <p>谁改了什么、机器出了什么事，分成两份日志各自记录。</p>
          </div>
        </div>
      </div>

      <div class="auth-art__foot">
        <span><i class="status-dot" /> 本机控制台</span>
        <span class="mono">libuvcpp</span>
      </div>
    </section>

    <!-- 右栏：表单。 -->
    <section class="auth-panel">
      <div class="auth-actions">
        <button
          class="icon-button"
          type="button"
          :title="isLight ? '切换到深色主题' : '切换到浅色主题'"
          :aria-label="isLight ? '切换到深色主题' : '切换到浅色主题'"
          @click="toggleTheme"
        >
          <Icon :name="isLight ? 'moon' : 'sun'" :size="18" />
        </button>
      </div>

      <form class="auth-card" @submit.prevent="onSubmit">
        <div class="auth-card__head">
          <span class="eyebrow">安全登录</span>
          <h2>登录控制台</h2>
          <p>输入本机配置里的 Web 账号。凭据只在本机校验，不会发往外部服务。</p>
        </div>

        <!--
          为什么会被送到这里。**故意是独立的一块，不参与下面的 v-if/v-else-if 链**
          —— "服务刚重启过"和"后端连不上"是两件互不相干的事，串进同一条链会让其中
          一件盖掉另一件，而那时最需要的恰恰是同时看到两条。
        -->
        <div v-if="lifecycleBanner" class="inline-alert inline-alert--warning">
          <Icon name="alert" :size="16" />
          <span>
            <strong>{{ lifecycleBanner.reason === 'reset' ? '服务已恢复出厂并重启。' : '服务已重启。' }}</strong>
            {{ lifecycleBanner.text }}
          </span>
        </div>

        <!-- 后端根本没起来：这时登录是没有意义的重试，得说清楚。 -->
        <div v-if="sessionError" class="inline-alert inline-alert--danger">
          <Icon name="alert" :size="16" />
          <span>
            <strong>连不上后端。</strong>{{ sessionError }}
            <span class="actions" style="margin-top: 10px">
              <button type="button" class="ghost" @click="retrySession">
                <Icon name="refresh" :size="15" />重试
              </button>
            </span>
          </span>
        </div>

        <!-- 后台明确说了不需要登录（web.auth_enabled=false 或口令为空）。 -->
        <div v-else-if="!authRequired" class="inline-alert inline-alert--warning">
          <Icon name="info" :size="16" />
          <span>
            这台机器上的 Web 控制台<strong>没有启用登录</strong>，因此没有可输入的口令。
            <br />想开启登录，请在后端配置文件里设置
            <code class="value">web.auth_enabled</code> 与
            <code class="value">web.password</code>，或启动时用
            <code class="value">--http-user</code> / <code class="value">--http-password</code>。
            <span class="actions" style="margin-top: 12px">
              <button type="button" class="primary" @click="router.replace('/')">
                <Icon name="camera" :size="15" />进入控制台
              </button>
            </span>
          </span>
        </div>

        <template v-else>
          <div v-if="errorText" class="inline-alert inline-alert--danger">
            <Icon name="x" :size="16" />
            <span>{{ errorText }}</span>
          </div>
          <div v-else-if="cooldown > 0" class="inline-alert inline-alert--warning">
            <Icon name="clock" :size="16" />
            <span>
              失败次数过多，该地址已被暂时拒绝。<strong>这段时间里即使口令正确也会被拒</strong>
              —— 等倒计时走完再试。
            </span>
          </div>

          <label class="field">
            <span>用户名</span>
            <span class="input-shell">
              <Icon name="user" :size="17" />
              <input
                v-model="name"
                type="text"
                autocomplete="username"
                autocapitalize="off"
                spellcheck="false"
                :disabled="busy"
                placeholder="admin"
              />
            </span>
          </label>

          <label class="field">
            <span>口令</span>
            <span class="input-shell">
              <Icon name="lock" :size="17" />
              <input
                ref="passwordInput"
                v-model="password"
                type="password"
                autocomplete="current-password"
                :disabled="busy || cooldown > 0"
                placeholder="••••••"
              />
            </span>
          </label>

          <button type="submit" class="primary auth-submit" :disabled="!canSubmit">
            <Icon v-if="busy" name="loader" :size="16" />
            {{ submitLabel }}
          </button>
        </template>

        <div class="auth-card__meta">
          <span>
            凭据与 ONVIF 那一份<strong>不是同一套</strong>：这里是人手敲的
            <code class="value">web.user</code> / <code class="value">web.password</code>。
          </span>
          <span class="mono">本地会话</span>
        </div>
      </form>
    </section>
  </div>
</template>
