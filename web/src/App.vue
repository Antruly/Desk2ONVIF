<script setup lang="ts">
import { computed, onBeforeUnmount, watch } from 'vue'
import { RouterLink, RouterView, useRoute, useRouter } from 'vue-router'
import Icon from './components/Icon.vue'
import TechBackground from './components/TechBackground.vue'
import { useAuth } from './composables/useAuth'
import { useStats } from './composables/useStats'

const route = useRoute()
const router = useRouter()
const { authenticated, authRequired, checked, user, logout } = useAuth()
const { state, startStats, stopStats } = useStats()

/**
 * 会话丢掉的那一刻，把人送回登录页。
 *
 * **放在这里而不是各个调用点**：丢会话有两条完全不同的来路 —— REST 拿到 401
 * （`api.ts` 的 handleUnauthorized）和 WebSocket 被 1008 关掉（`useStats` /
 * 预览页）。两处都去 `router.push` 的结果是漏掉一处就变成"页面还在，但每个
 * 请求都失败"的空白页。它们只需要改 `authenticated` 这一个状态，导航由这里
 * 统一负责。
 */
watch(authenticated, (on) => {
  if (on) return
  // 还没问过后端（初次加载）或者本来就允许匿名访问时，"未认证"是正常状态，
  // 不是会话丢了。
  if (!checked.value || !authRequired.value) return
  if (route.name === 'login') return
  router.replace({ name: 'login', query: { next: route.fullPath } })
})

/**
 * 登录页**在壳外面**：它不该有导航栏（那上面的每一个链接都会把刚进来的用户
 * 弹回登录页），也不该有连接指示灯（那时还没连上任何东西）。
 */
const bare = computed(() => route.name === 'login')

/**
 * 统计推送只在"确实进得来"的时候连。
 *
 * 不这么做的后果很具体：未登录时 `/ws/stats` 会被后端用 1008 关掉，而
 * `useStats` 的重连逻辑会照着退避表一路重试 —— 于是登录页会在后台不停地
 * 握手、被拒、再握手，日志里刷满拒绝记录，而用户只看到登录框。
 */
const shouldStream = computed(() => checked.value && (!authRequired.value || authenticated.value))

watch(
  shouldStream,
  (on) => {
    if (on) startStats()
    else stopStats()
  },
  { immediate: true },
)

onBeforeUnmount(stopStats)

const connText = computed(() =>
  state.value === 'online' ? '实时' : state.value === 'connecting' ? '连接中' : '已断开',
)

async function onLogout() {
  await logout()
  // 登出之后必须回登录页 —— 不然当前页会立刻因为 401 而一片空白。
  await router.replace({ name: 'login' })
}
</script>

<template>
  <!-- 背景层在最底下，全局共用（登录页也要）。 -->
  <TechBackground />

  <!-- 登录页：只有它自己，没有壳。 -->
  <RouterView v-if="bare" />

  <div v-else class="app">
    <header class="topbar">
      <span class="brand">
        <Icon name="camera" :size="21" :stroke="1.7" class="brand-mark" />
        Desk2ONVIF
        <span class="brand-sub">Profile S</span>
      </span>

      <nav class="nav">
        <RouterLink to="/">
          <Icon name="eye" :size="16" />预览
        </RouterLink>
        <RouterLink to="/status">
          <Icon name="activity" :size="16" />状态
        </RouterLink>
        <RouterLink to="/config">
          <Icon name="sliders" :size="16" />配置
        </RouterLink>
      </nav>

      <span class="spacer"></span>

      <!-- 状态色永远配文字标签：只靠颜色表意对色觉障碍不友好，也扛不住灰阶截图。 -->
      <span
        v-if="shouldStream"
        class="pill"
        :class="{
          'is-online': state === 'online',
          'is-connecting': state === 'connecting',
          'is-offline': state === 'offline',
        }"
        :title="state === 'online' ? '统计推送正常' : '统计推送未连接，正在自动重连'"
      >
        <Icon name="signal" :size="14" />统计推送：{{ connText }}
      </span>

      <template v-if="authRequired && authenticated">
        <span class="pill" title="当前登录的账号">
          <Icon name="user" :size="14" />{{ user || '已登录' }}
        </span>
        <button class="ghost" title="退出登录" @click="onLogout">
          <Icon name="logout" :size="15" />退出
        </button>
      </template>
    </header>

    <main class="page">
      <RouterView />
    </main>
  </div>
</template>
