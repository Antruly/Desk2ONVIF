import { createRouter, createWebHistory } from 'vue-router'
import { ensureSession, useAuth } from './composables/useAuth'

/**
 * history 模式，不是 hash。
 *
 * 后端静态服务开了 SPA 回落（`uvcpp_web_static_options::spa_fallback`），
 * /config 这种没有对应文件的前端路由会回 index.html；用 hash 的话就白白多出
 * 一个 #，而且 NVR/浏览器里贴地址时容易贴丢后半截。
 */
export const router = createRouter({
  history: createWebHistory(),
  routes: [
    // **首页是预览。** 这个程序做出来就是为了看画面的，状态页和配置页是
    // "出了问题时才去的地方"。原来的默认页是状态页，也就是每次打开都得再点
    // 一次才能看到画面 —— 而那一次点击恰好是所有人每次都要做的。
    { path: '/', name: 'preview', component: () => import('./views/PreviewView.vue') },
    { path: '/status', name: 'status', component: () => import('./views/StatusView.vue') },
    { path: '/config', name: 'config', component: () => import('./views/ConfigView.vue') },
    // 登录页**在壳外面**（App.vue 里对它有特判），所以它自己不占导航栏的位置。
    { path: '/login', name: 'login', component: () => import('./views/LoginView.vue') },
    // 兜底：未知路径回首页。后端会把 index.html 给任何路径，这里再兜一次，
    // 免得用户手抖敲错地址看到一个空白页。
    { path: '/:pathMatch(.*)*', redirect: '/' },
  ],
})

// `useAuth()` 返回的是模块级的同一组 ref，所以这里取一次、全程共用。
const { authRequired, authenticated, sessionError } = useAuth()

/**
 * 进任何页面前先确认会话。
 *
 * ## 为什么要 `await ensureSession()` 而不是同步判
 *
 * 因为前端**看不到** cookie（后端标了 HttpOnly），"我登录了吗"这个问题只有
 * 后端答得上来。守卫里不等它，就会出现"刷新页面时先闪一下登录页、再跳回来"
 * —— 那一闪看起来就是"我被登出了"。
 *
 * ## 为什么没开登录时把 /login 弹回首页
 *
 * 那种情况下登录页是个死胡同：上面没有任何能让你前进的东西，而用户之所以会
 * 走到那里，多半是因为手输了一次 /login 或者收藏了旧地址。
 */
router.beforeEach(async (to) => {
  await ensureSession()

  // 后端没起来：让请求照常发出去，由各页自己的错误提示接管。
  // 在这里拦下来会得到"一个空白页 + 一句后端没启动"，而用户那时可能只是想
  // 打开配置页看一眼 —— 让页面自己报错更清楚，也更接近他期待看到的东西。
  if (sessionError.value) return true

  if (!authRequired.value) {
    return to.name === 'login' ? { path: '/' } : true
  }

  if (to.name === 'login') {
    // 已登录还去登录页 = 直接回首页。用户在地址栏敲 /login 想换账号时会被这
    // 一步挡住，所以登录页上给了「退出登录」的入口 —— 那才是换账号的路。
    return authenticated.value ? { path: '/' } : true
  }

  if (!authenticated.value) {
    // 把原目标带上，登录完直接回那儿，而不是一律丢到首页。
    return { name: 'login', query: { next: to.fullPath } }
  }

  return true
})
