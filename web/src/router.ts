import { createRouter, createWebHistory } from 'vue-router'
import { ensureSession, useAuth } from './composables/useAuth'

/**
 * history 模式，不是 hash。
 *
 * 后端静态服务开了 SPA 回落（`uvcpp_web_static_options::spa_fallback`），
 * `/channels/2` 这种没有对应文件的前端路由会回 index.html；用 hash 的话就白白
 * 多出一个 #，而且 NVR/浏览器里贴地址时容易贴丢后半截。
 *
 * ## 页面为什么这么分（而不是一页锚点）
 *
 * 旧版把图像、通道、水印、音视频、时间、ONVIF、系统设置全塞在 `/config` 一页，
 * 靠锚点滚动。用户的原话是"太臃肿，全挤压在一个页面"。现在按**读者要做的决定**
 * 拆开：
 *
 *   · `/`           —— 看画面
 *   · `/channels`   —— 管有哪几路
 *   · `/channels/:n`—— 管某一路（视频参数 + 水印），**改动自动生效**
 *   · `/settings/*` —— 管这台机器（这些仍然需要点保存）
 *   · `/logs`       —— 查发生过什么
 *
 * `:n` 用的是**稳定通道号**，不是数组下标：删掉通道 3 之后 `/channels/4`
 * 仍然是原来那一路。
 */
export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', name: 'preview', component: () => import('./views/PreviewView.vue') },
    { path: '/channels', name: 'channels', component: () => import('./views/ChannelsView.vue') },
    {
      path: '/channels/:number(\\d+)',
      name: 'channel-detail',
      component: () => import('./views/ChannelDetailView.vue'),
      props: true,
    },
    { path: '/status', name: 'status', component: () => import('./views/StatusView.vue') },
    { path: '/info', name: 'info', component: () => import('./views/InfoView.vue') },
    { path: '/logs', name: 'logs', component: () => import('./views/LogsView.vue') },
    {
      path: '/settings/system',
      name: 'settings-system',
      component: () => import('./views/SystemSettingsView.vue'),
    },
    {
      path: '/settings/onvif',
      name: 'settings-onvif',
      component: () => import('./views/OnvifSettingsView.vue'),
    },
    {
      path: '/settings/audio',
      name: 'settings-audio',
      component: () => import('./views/AudioSettingsView.vue'),
    },
    {
      path: '/settings/time',
      name: 'settings-time',
      component: () => import('./views/TimeSettingsView.vue'),
    },
    // 旧地址：书签和习惯都指向 `/config`，别让它们落到首页。
    { path: '/config', redirect: '/channels' },
    // 登录页**在壳外面**（App.vue 里对它有特判），所以它自己不占导航栏的位置。
    { path: '/login', name: 'login', component: () => import('./views/LoginView.vue') },
    // 兜底：未知路径回首页。
    { path: '/:pathMatch(.*)*', redirect: '/' },
  ],

  /**
   * 换页回到顶部。没有锚点了，所以这里只剩这一条规则 —— 保留 `saved` 是为了
   * 浏览器前进/后退仍然回到用户刚才看的位置。
   *
   * `prefers-reduced-motion` 下用 `'auto'`：平滑滚动对前庭敏感的人是实打实的
   * 不适来源。
   */
  scrollBehavior(_to, _from, saved) {
    if (saved) return saved
    const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches
    return { top: 0, behavior: reduced ? 'auto' : 'smooth' }
  },
})

// `useAuth()` 返回的是模块级的同一组 ref，所以这里取一次、全程共用。
const { authRequired, authenticated, sessionError } = useAuth()

/**
 * 进任何页面前先确认会话。
 *
 * 必须 `await ensureSession()` 而不是同步判：前端看不到 cookie（后端标了
 * HttpOnly），"我登录了吗"只有后端答得上来。不等它就会出现"刷新页面时先闪
 * 一下登录页、再跳回来"。
 */
router.beforeEach(async (to) => {
  await ensureSession()

  // 后端没起来：让请求照常发出去，由各页自己的错误提示接管。
  if (sessionError.value) return true

  if (!authRequired.value) {
    return to.name === 'login' ? { path: '/' } : true
  }

  if (to.name === 'login') {
    return authenticated.value ? { path: '/' } : true
  }

  if (!authenticated.value) {
    // 把原目标带上，登录完直接回那儿。
    return { name: 'login', query: { next: to.fullPath } }
  }

  return true
})
