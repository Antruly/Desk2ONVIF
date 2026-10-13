<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { RouterLink, RouterView, useRoute, useRouter } from 'vue-router'
import Icon from './components/Icon.vue'
import TechBackground from './components/TechBackground.vue'
import { useAuth } from './composables/useAuth'
import { useStats } from './composables/useStats'
import { scrollToHash } from './scroll'
import type { IconName } from './icons'

const route = useRoute()
const router = useRouter()
const { authenticated, authRequired, checked, user, logout } = useAuth()
const { stats, state, startStats, stopStats } = useStats()

interface MenuItem {
  /** 菜单上显示的字 */
  label: string
  /** 目标路由。带 `#` 的是**同一个页面**的段落（配置页那几个子项） */
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
 * 后面几个阶段（设备信息 `/info`、操作日志 `/logs`、水印 `#osd`、音视频
 * `#audio`、时间 `#time`、自适应 `#adaptive`）各自在这里**加一行**，而不是
 * 各改各的地方 —— 菜单散成几处之后，"哪一项点了没反应"就要靠人去试。
 *
 * 两条纪律：
 *
 * 1. **只放已经存在的路由和段落。** 指向一个还不存在的锚点，用户看到的是
 *    "点了没反应"（URL 变了，页面纹丝不动）—— 那正是这个功能最不该有的样子。
 *    所以 `#adaptive` 还在等它自己的段落（Phase W），**段落和它的这一行是
 *    同一次改动里一起进来的**。（`#audio` 与 `#time` 两段已经落地，各自的
 *    菜单行就是按这条纪律跟它们一起进来的。）
 * 2. **不要预先放一个空的「设备」组。** 一个点不开的分组标题读起来像界面坏了，
 *    比没有这一组更糟。所以「设备」组是**跟着它的第一个条目一起**加进来的
 *    （Phase O 的设备信息），操作日志在 Phase V 加进来时也走同一条路 ——
 *    分组和它里面的东西一次到位，中间不留空窗。
 *
 * 图标尽量复用已有的（`icons.ts` 那 24 个是给这个界面挑的），只为"没有合适
 * 的"补新的：
 *   · `monitor` 图像设置、`sliders` 系统设置 —— 与它们在旧版横向导航里的用法一致；
 *   · `shield` 的注释本来就写着"ONVIF / 鉴权"，拿它当 ONVIF 设置比新造一个合适；
 *   · 水印那一项用 `image`（画中一块牌子），不复用 `monitor` —— 两个菜单项撞
 *     同一个图标时，收起侧栏之后就完全分不出谁是谁了。
 */
const MENU_GROUPS: MenuGroup[] = [
  {
    title: '监控',
    items: [
      { label: '预览', to: '/', icon: 'eye' },
      { label: '实时状态', to: '/status', icon: 'activity' },
    ],
  },
  {
    title: '配置',
    items: [
      { label: '图像设置', to: '/config#video', icon: 'monitor' },
      // 「通道」紧挨着「图像设置」：它管的正是"图像设置那一路之外还有几路"，
      // 两者分开放在菜单两头时，用户会以为多通道是另一个功能而不是同一条管线的复制。
      // 图标用 `camera`（和「检测到的源」里摄像头那一行同一个），不复用 `monitor`
      // —— 收起侧栏之后两个 `monitor` 就完全分不出谁是谁了。
      { label: '通道', to: '/config#channels', icon: 'camera' },
      { label: '水印 OSD', to: '/config#osd', icon: 'image' },
      // 「音视频」这一行和 `ConfigView.vue` 里 `id="audio"` 那个段落是**同一次
      // 改动里一起进来的**：菜单行指向一个不存在的锚点，点下去就是"什么都没发生"
      // —— 而它和"这一段没做"在界面上长得一模一样。
      { label: '音视频', to: '/config#audio', icon: 'volume' },
      // 「时间设置」单独一项、不并进「系统设置」：它管的不是一个开关，而是
      // **画面上的时间**（水印和 ONVIF 报出去的那个），与 NVR 对时、取证对账
      // 直接相关 —— 这跟端口、口令那一类"系统设置"不是同一件事。
      { label: '时间设置', to: '/config#time', icon: 'clock' },
      { label: 'ONVIF 设置', to: '/config#onvif', icon: 'shield' },
      { label: '系统设置', to: '/config#system', icon: 'sliders' },
    ],
  },
  {
    // 「设备」和「配置」分开，是用户点名要的三分法（系统设置 / ONVIF 设置 /
    // 设备信息）里最后那一项：**这一组只有只读的东西**，所以它不属于"配置"。
    title: '设备',
    items: [
      { label: '设备信息', to: '/info', icon: 'info' },
      // 操作日志也归「设备」：它和设备信息一样是**只读**的，而且问的是
      // 同一类问题（"这台设备身上发生过什么"）。放进「配置」组会让人以为
      // 点进去能改什么。
      { label: '操作日志', to: '/logs', icon: 'list' },
    ],
  },
]

/** 当前地址的"路径 + 锚点"。锚点为空时就是路径本身。 */
const here = computed(() => route.path + route.hash)

/**
 * 高亮哪一项 —— **自己算，不用 RouterLink 的 `router-link-active`**。
 *
 * 原因是配置页那三个子项指向同一个 path：`router-link-active`（以及不少版本的
 * `router-link-exact-active`）只比到 path 这一层，于是在 `/config#system` 上
 * 「图像设置」「ONVIF 设置」「系统设置」会**同时**高亮 —— 三个都亮等于一个都没指。
 * 这里的判据是路径**加锚点**完全相同。
 *
 * 不带锚点的项按 path 比，且**不比查询串**：登录回跳会带一个 `?next=`，
 * 它不该影响哪一项高亮。
 */
function isActive(item: MenuItem): boolean {
  return item.to.includes('#') ? here.value === item.to : route.path === item.to
}

/**
 * 点的是当前这一项时，vue-router 判为重复导航、什么都不做 —— 而用户那时多半
 * 是把页面滚上去了、想再回到那一段。所以这里自己补一次滚动。
 *
 * 顺带收抽屉：`watch(route.fullPath)` 只对"真的换了地址"生效，重复点击不换地址。
 */
function onMenuClick(item: MenuItem) {
  if (item.to.includes('#') && here.value === item.to) scrollToHash(route.hash)
  drawerOpen.value = false
}

/** 侧栏折叠状态的存储键。带前缀，免得和同一个域下别的东西撞。 */
const COLLAPSE_KEY = 'd2o.sidebar.collapsed'

/**
 * localStorage 一律包 try/catch。
 *
 * 隐私模式、站点数据被禁用、存储配额被同一个域下的别的东西占满 —— 这三件事
 * 都会让 `getItem` / `setItem` **抛异常**，而它们中的每一件都没有理由让整个
 * 界面打不开。读不到就当"没折叠过"。
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
 * 窄屏时那个覆盖式抽屉开着没有。
 *
 * **"当前视口多宽"只有 CSS 说了算**，这里只存"开着还是关着"。折叠状态在
 * ≤720px 下由 CSS 强制忽略（见 `styles.css` 里那条媒体查询）—— 不然就得在
 * JS 里再存一份视口宽度，而那份副本会在转屏、缩放、窗口分屏之后过期，
 * 于是抽屉出现"JS 认为该折叠、CSS 认为该展开"的一半一半的样子。
 */
const drawerOpen = ref(false)

// 换页就收抽屉。用 fullPath 而不是 path：配置页那三个子项的 path 完全相同，
// 只有锚点不一样，掂 path 的话点菜单收不了抽屉。
watch(
  () => route.fullPath,
  () => {
    drawerOpen.value = false
  },
)

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

/**
 * 侧栏底部的版本号。`emptyStats()` 里它是**空串**而不是 "0.0.0"，
 * 所以后端还没答话时这里什么都不显示 —— 编一个像模像样的版本号比留空更坏。
 */
const version = computed(() => stats.value.version)

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
    <!-- 抽屉拉开时压在内容上的一层遮罩。点它 = 收抽屉，比"再点一次汉堡"顺手。 -->
    <div v-if="drawerOpen" class="side-scrim" @click="drawerOpen = false"></div>

    <aside class="sidebar" :class="{ 'is-collapsed': collapsed, 'is-open': drawerOpen }">
      <div class="side-head">
        <button
          class="side-toggle"
          type="button"
          :title="collapsed ? '展开菜单' : '收起菜单'"
          :aria-label="collapsed ? '展开菜单' : '收起菜单'"
          :aria-expanded="!collapsed"
          @click="toggleCollapsed"
        >
          <Icon :name="collapsed ? 'chevron-right' : 'chevron-left'" :size="16" />
        </button>
      </div>

      <nav class="side-nav">
        <div v-for="group in MENU_GROUPS" :key="group.title" class="side-group">
          <div class="side-group-title">{{ group.title }}</div>

          <RouterLink
            v-for="item in group.items"
            :key="item.to"
            :to="item.to"
            class="side-link"
            :class="{ 'is-active': isActive(item) }"
            :title="collapsed ? item.label : undefined"
            :aria-label="collapsed ? item.label : undefined"
            @click="onMenuClick(item)"
          >
            <Icon :name="item.icon" :size="17" />
            <!-- 折叠时整块隐藏，而不是缩成一个点：留着的话那 56px 里会挤成
                 一条竖着的残影。文字本身**不删**（读屏和浏览器内查找还要用它）。 -->
            <span class="side-label">{{ item.label }}</span>
          </RouterLink>
        </div>
      </nav>

      <div v-if="version" class="side-foot" :title="`软件版本 ${version}`">v{{ version }}</div>
    </aside>

    <main class="workspace">
      <header class="topbar">
        <!-- 只在 ≤720px 出现（CSS 控制），它取代的是原来那条横向导航。 -->
        <button
          class="hamburger"
          type="button"
          :aria-expanded="drawerOpen"
          aria-label="菜单"
          title="菜单"
          @click="drawerOpen = !drawerOpen"
        >
          <Icon name="menu" :size="19" />
        </button>

        <span class="brand">
          <Icon name="camera" :size="21" :stroke="1.7" class="brand-mark" />
          Desk2ONVIF
          <span class="brand-sub">Profile S</span>
        </span>

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

      <div class="page">
        <RouterView />
      </div>
    </main>
  </div>
</template>
