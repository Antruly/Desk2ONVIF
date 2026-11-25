<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { RouterLink, RouterView, useRoute, useRouter } from 'vue-router'
import BrandMark from './components/BrandMark.vue'
import Icon from './components/Icon.vue'
import TechBackground from './components/TechBackground.vue'
import { useAuth } from './composables/useAuth'
import { useStats } from './composables/useStats'
import { useTheme } from './composables/useTheme'
import type { IconName } from './icons'

const route = useRoute()
const router = useRouter()
const { authenticated, authRequired, checked, user, logout } = useAuth()
const { stats, state, startStats, stopStats } = useStats()
const { isLight, toggleTheme } = useTheme()

interface MenuItem {
  /** 菜单上显示的字 */
  label: string
  /** 目标路由。每一项都是**独立的页面**（旧版那套 `#` 锚点已经拆掉了） */
  to: string
  icon: IconName
}

interface MenuGroup {
  title: string
  items: MenuItem[]
}

/**
 * 菜单表 —— **唯一的一处定义**。
 *
 * 三条纪律：
 *
 * 1. **只放已经存在的路由。** 指向一个不存在的地址，用户看到的是"点了没反应"
 *    （URL 变了，页面纹丝不动）—— 那正是导航最不该有的样子。
 * 2. **不预先放空分组。** 一个点不开的分组标题读起来像界面坏了，比没有这一组
 *    更糟。分组和它里面的第一项一起进来。
 * 3. **图标尽量复用已有的**（`icons.ts` 那 24 个是给这个界面挑的），只为没有
 *    合适的补新的；收起侧栏之后图标就是唯一的辨识物，两个菜单项不能撞同一个。
 */
const MENU_GROUPS: MenuGroup[] = [
  {
    title: '监控',
    items: [
      { label: '实时预览', to: '/', icon: 'eye' },
      { label: '通道管理', to: '/channels', icon: 'camera' },
      { label: '运行状态', to: '/status', icon: 'activity' },
    ],
  },
  {
    title: '设置',
    items: [
      { label: '系统设置', to: '/settings/system', icon: 'sliders' },
      { label: 'ONVIF 设置', to: '/settings/onvif', icon: 'shield' },
      { label: '音频设置', to: '/settings/audio', icon: 'volume' },
      { label: '时间设置', to: '/settings/time', icon: 'clock' },
    ],
  },
  {
    // 只读的两项。放进「设置」组会让人以为点进去能改什么。
    title: '运维',
    items: [
      { label: '日志与告警', to: '/logs', icon: 'list' },
      { label: '设备信息', to: '/info', icon: 'info' },
    ],
  },
]

/**
 * 顶栏标题。**前缀匹配而不是全等**：`/channels/2` 应当让「通道管理」保持高亮，
 * 而顶栏要显示"通道 2"这件更具体的事。
 */
function isActive(item: MenuItem): boolean {
  if (item.to === '/') return route.path === '/'
  return route.path === item.to || route.path.startsWith(item.to + '/')
}

/** 路由 → 顶栏标题与分类。登录页在壳外面，不在这里。 */
const PAGE_META: Record<string, { title: string; eyebrow: string }> = {
  preview: { title: '实时预览', eyebrow: '监控 · 画面墙' },
  channels: { title: '通道管理', eyebrow: '监控 · 通道' },
  status: { title: '运行状态', eyebrow: '监控 · 遥测' },
  'settings-system': { title: '系统设置', eyebrow: '设置 · 服务' },
  'settings-onvif': { title: 'ONVIF 设置', eyebrow: '设置 · 设备身份' },
  'settings-audio': { title: '音频设置', eyebrow: '设置 · 音轨' },
  'settings-time': { title: '时间设置', eyebrow: '设置 · 设备时间' },
  logs: { title: '日志与告警', eyebrow: '运维 · 审计与排障' },
  info: { title: '设备信息', eyebrow: '运维 · 只读' },
}

const pageMeta = computed(() => {
  const name = typeof route.name === 'string' ? route.name : ''
  if (name === 'channel-detail') {
    const n = route.params.number
    return {
      title: `通道 ${Array.isArray(n) ? n[0] : (n ?? '')}`,
      eyebrow: '监控 · 通道详情',
    }
  }
  return PAGE_META[name] ?? { title: 'Desk2ONVIF', eyebrow: '本机控制台' }
})

/** 收抽屉。 */
function onMenuClick() {
  drawerOpen.value = false
}

/** 侧栏折叠状态的存储键。带前缀，免得和同一个域下别的东西撞。 */
const COLLAPSE_KEY = 'd2o.sidebar.collapsed'

/**
 * localStorage 一律包 try/catch。隐私模式、站点数据被禁用、存储配额被占满
 * 都会让 `getItem` / `setItem` 抛异常，而它们中的每一件都没有理由让界面打不开。
 */
function readCollapsed(): boolean {
  try {
    return window.localStorage.getItem(COLLAPSE_KEY) === '1'
  } catch {
    return false
  }
}

const collapsed = ref(readCollapsed())

function toggleCollapsed() {
  collapsed.value = !collapsed.value
  try {
    window.localStorage.setItem(COLLAPSE_KEY, collapsed.value ? '1' : '0')
  } catch {
    /* 记不住就记不住：这一次的折叠照常生效，刷新之后回到展开。 */
  }
}

/**
 * 窄屏抽屉。**"当前视口多宽"只有 CSS 说了算**，这里只存"开着还是关着"；
 * ≤860px 下折叠状态由 CSS 强制忽略，免得 JS 与 CSS 各持一份会过期的视口判断。
 */
const drawerOpen = ref(false)

/** 用户菜单开着没有。换页时和点页面空白处都会收起来。 */
const userMenuOpen = ref(false)

watch(
  () => route.fullPath,
  () => {
    drawerOpen.value = false
    userMenuOpen.value = false
  },
)

/**
 * 会话丢掉的那一刻，把人送回登录页。
 *
 * **放在这里而不是各个调用点**：丢会话有两条完全不同的来路 —— REST 401 和
 * WebSocket 1008。两处都去 `router.push` 的结果是漏掉一处就变成"页面还在，
 * 但每个请求都失败"的空白页。它们只改 `authenticated`，导航由这里统一负责。
 */
watch(authenticated, (on) => {
  if (on) return
  if (!checked.value || !authRequired.value) return
  if (route.name === 'login') return
  router.replace({ name: 'login', query: { next: route.fullPath } })
})

/** 登录页**在壳外面**：没有导航，也没有连接指示灯。 */
const bare = computed(() => route.name === 'login')

/**
 * 统计推送只在"确实进得来"的时候连。未登录时 `/ws/stats` 会被 1008 关掉，
 * 而重连逻辑会照着退避表一路重试 —— 登录页会在后台不停握手、被拒、再握手。
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

const connText = computed(() =>
  state.value === 'online' ? '实时' : state.value === 'connecting' ? '连接中' : '已断开',
)

const daemonText = computed(() =>
  state.value === 'online'
    ? '守护进程在线'
    : state.value === 'connecting'
      ? '正在连接守护进程'
      : '守护进程未连接',
)

const daemonDotClass = computed(() =>
  state.value === 'online' ? '' : state.value === 'connecting' ? 'status-dot--warn' : 'status-dot--bad',
)

/**
 * 侧栏底部的版本行。`emptyStats()` 里 version 是**空串**而不是 "0.0.0"，
 * 所以后端还没答话时显示"—" —— 编一个像模像样的版本号比留空更坏。
 *
 * libuvcpp 不在这份统计里（后端没有上报它的版本），这里只标出运行时，
 * 不编一个库版本号。
 */
const versionLine = computed(() => `v${stats.value.version || '—'} · libuvcpp`)

/* ------------------------------------------------------------------ 顶栏时钟 */

const now = ref(new Date())
let clockTimer = 0

const pad2 = (n: number) => String(n).padStart(2, '0')
const WEEKDAYS = ['周日', '周一', '周二', '周三', '周四', '周五', '周六']

const clockText = computed(
  () => `${pad2(now.value.getHours())}:${pad2(now.value.getMinutes())}:${pad2(now.value.getSeconds())}`,
)
const dateText = computed(
  () =>
    `${now.value.getFullYear()}-${pad2(now.value.getMonth() + 1)}-${pad2(now.value.getDate())} ${
      WEEKDAYS[now.value.getDay()]
    }`,
)

/* ------------------------------------------------------------------ 用户菜单 */

const userMenuRef = ref<HTMLElement | null>(null)

const userInitial = computed(() => (user.value || (authRequired.value ? '未' : '本')).slice(0, 1))

function onDocumentClick(event: MouseEvent) {
  if (userMenuOpen.value && userMenuRef.value && !userMenuRef.value.contains(event.target as Node)) {
    userMenuOpen.value = false
  }
}

async function onLogout() {
  userMenuOpen.value = false
  await logout()
  // 登出之后必须回登录页 —— 不然当前页会立刻因为 401 而一片空白。
  await router.replace({ name: 'login' })
}

onMounted(() => {
  clockTimer = window.setInterval(() => {
    now.value = new Date()
  }, 1000)
  document.addEventListener('click', onDocumentClick)
})

onBeforeUnmount(() => {
  stopStats()
  if (clockTimer) window.clearInterval(clockTimer)
  document.removeEventListener('click', onDocumentClick)
})
</script>

<template>
  <!-- 背景层在最底下，全局共用（登录页也要）。 -->
  <TechBackground />

  <!-- 登录页：只有它自己，没有壳。 -->
  <RouterView v-if="bare" />

  <div v-else class="shell" :class="{ 'shell--collapsed': collapsed, 'shell--nav-open': drawerOpen }">
    <!-- 抽屉拉开时压在内容上的一层遮罩。点它 = 收抽屉。 -->
    <div v-if="drawerOpen" class="mobile-scrim" @click="drawerOpen = false"></div>

    <aside class="sidebar">
      <div class="sidebar__brand">
        <RouterLink to="/" class="brand-button" aria-label="回到实时预览" @click="onMenuClick">
          <BrandMark :size="34" />
          <span class="brand-copy">
            <strong>Desk2ONVIF</strong>
            <small>PROFILE S CONSOLE</small>
          </span>
        </RouterLink>
        <button
          class="icon-button sidebar__collapse"
          type="button"
          :title="collapsed ? '展开菜单' : '收起菜单'"
          :aria-label="collapsed ? '展开菜单' : '收起菜单'"
          :aria-expanded="!collapsed"
          @click="toggleCollapsed"
        >
          <Icon :name="collapsed ? 'chevron-right' : 'chevron-left'" :size="16" />
        </button>
      </div>

      <nav class="sidebar__nav" aria-label="主导航">
        <section v-for="group in MENU_GROUPS" :key="group.title" class="nav-section">
          <div class="nav-section__label">{{ group.title }}</div>

          <RouterLink
            v-for="item in group.items"
            :key="item.to"
            :to="item.to"
            class="nav-item"
            :class="{ 'nav-item--active': isActive(item) }"
            :title="collapsed ? item.label : undefined"
            :aria-label="collapsed ? item.label : undefined"
            @click="onMenuClick"
          >
            <span class="nav-item__icon"><Icon :name="item.icon" :size="18" /></span>
            <!-- 折叠时整块隐藏，而不是缩成一个点；文字本身不删（读屏还要用）。 -->
            <span class="nav-item__label">{{ item.label }}</span>
          </RouterLink>
        </section>
      </nav>

      <!-- 底部状态块：一眼回答"后端在不在、跑的是哪个版本"。点它去运行状态页。 -->
      <div class="sidebar__foot">
        <RouterLink to="/status" class="daemon-chip" :title="`${daemonText} · ${versionLine}`" @click="onMenuClick">
          <span class="status-dot" :class="daemonDotClass" />
          <span class="daemon-chip__copy">
            <strong>{{ daemonText }}</strong>
            <small>{{ versionLine }}</small>
          </span>
        </RouterLink>
      </div>
    </aside>

    <main class="main">
      <header class="topbar">
        <button
          class="icon-button mobile-menu"
          type="button"
          aria-label="菜单"
          title="菜单"
          :aria-expanded="drawerOpen"
          @click="drawerOpen = !drawerOpen"
        >
          <Icon name="menu" :size="19" />
        </button>

        <div class="topbar__title">
          <span class="topbar__eyebrow">{{ pageMeta.eyebrow }}</span>
          <h1>{{ pageMeta.title }}</h1>
        </div>

        <div class="topbar__spacer"></div>

        <div class="topbar__clock" :title="`${dateText} ${clockText}`">
          <strong class="num">{{ clockText }}</strong>
          <small>{{ dateText }}</small>
        </div>

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
          <i class="dot"></i>统计推送：{{ connText }}
        </span>

        <button
          class="icon-button topbar__theme"
          type="button"
          :title="isLight ? '切换到深色主题' : '切换到浅色主题'"
          :aria-label="isLight ? '切换到深色主题' : '切换到浅色主题'"
          @click="toggleTheme"
        >
          <Icon :name="isLight ? 'moon' : 'sun'" :size="18" />
        </button>

        <div ref="userMenuRef" class="user-menu">
          <button
            class="user-button"
            type="button"
            :aria-expanded="userMenuOpen"
            :title="authRequired ? `当前登录：${user || '未知'}` : '本机访问（登录未启用）'"
            @click.stop="userMenuOpen = !userMenuOpen"
          >
            <span class="avatar">{{ userInitial }}</span>
            <span class="user-button__copy">
              <strong>{{ user || (authRequired ? '未登录' : '本机访问') }}</strong>
              <small>{{ authRequired ? '管理账号' : '登录未启用' }}</small>
            </span>
            <Icon name="chevron-down" :size="14" />
          </button>

          <Transition name="menu">
            <div v-if="userMenuOpen" class="dropdown">
              <div class="dropdown__head">
                <strong>{{ user || '本机访问' }}</strong>
                <span>
                  {{
                    authRequired
                      ? '会话由后端签发，退出后需要重新登录。'
                      : '这台机器的控制台没有启用登录，同一局域网内都能打开。'
                  }}
                </span>
              </div>
              <RouterLink class="dropdown__item" to="/info" @click="userMenuOpen = false">
                <Icon name="info" :size="16" />设备信息
              </RouterLink>
              <RouterLink
                v-if="authRequired"
                class="dropdown__item"
                to="/settings/system"
                @click="userMenuOpen = false"
              >
                <Icon name="sliders" :size="16" />系统设置
              </RouterLink>
              <template v-if="authRequired && authenticated">
                <div class="dropdown__sep"></div>
                <button class="dropdown__item dropdown__item--danger" type="button" @click="onLogout">
                  <Icon name="logout" :size="16" />退出登录
                </button>
              </template>
            </div>
          </Transition>
        </div>
      </header>

      <div class="content">
        <div class="page">
          <RouterView />
        </div>
      </div>
    </main>
  </div>
</template>
