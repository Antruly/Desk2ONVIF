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
    // 只读的设备信息页。它和配置页分开，是因为**这两件事的读者不一样**：
    // 配置页要做决定（改什么），这一页只要抄几个数去填 NVR。混在一页里，
    // "这里到底能不能改"会变成每读一行都要判一次的问题。
    { path: '/info', name: 'info', component: () => import('./views/InfoView.vue') },
    // 操作日志（只读）。和上面同一类：它的读者是**来查一件事的人**，
    // 而不是来改东西的人。
    { path: '/logs', name: 'logs', component: () => import('./views/LogsView.vue') },
    // 登录页**在壳外面**（App.vue 里对它有特判），所以它自己不占导航栏的位置。
    { path: '/login', name: 'login', component: () => import('./views/LoginView.vue') },
    // 兜底：未知路径回首页。后端会把 index.html 给任何路径，这里再兜一次，
    // 免得用户手抖敲错地址看到一个空白页。
    { path: '/:pathMatch(.*)*', redirect: '/' },
  ],

  /**
   * 锚点跳转。
   *
   * 侧边栏把配置页展开成了"图像设置 / ONVIF 设置 / 系统设置"几个子项，它们
   * 指向的是**同一个页面**的不同段落（`/config#video` 这样）。表单和保存按钮
   * 只有一份，这正是不给它们各做一个页面的理由。
   *
   * 三条分支的次序不是随手写的：
   *   1. `saved` 优先 —— 浏览器前进/后退时用户期待的是"回到我刚才看到的地方"，
   *      而不是"回到锚点"。丢掉它，后退键在长页面上就等于是"回到页面顶部"。
   *   2. `to.hash` —— 目标段落滚进视口。偏移要**自己从 CSS 取**，理由见下面
   *      那段（`scroll-margin-top` 在这一条路上不生效 —— 实测过）。
   *   3. 其余回顶 —— 换页时若保留上一页的滚动位置，会落在新页面的中间，
   *      看起来像"这页没加载完"。
   *
   * `prefers-reduced-motion` 下用 `'auto'`：平滑滚动对前庭敏感的人是实打实的
   * 不适来源，而 `styles.css` 里那个把所有 transition/animation 归零的媒体查询
   * **管不到** `scroll-behavior`（它不是 transition），所以这里必须自己判一次。
   *
   * ⚠️ **光有它不够。** 它只在"路由真的变化"时被调用，而配置页有两处不满足：
   * 表单是异步加载的（这一刻页面还很短，滚不到目标位置），以及在 `/config` 上
   * 再点另一个子项时 path 没变。那两处由 `views/ConfigView.vue` 与 `App.vue`
   * 显式调 `scrollToHash()` 补上，那一头是幂等的，重复滚一次没有副作用。
   */
  scrollBehavior(to, _from, saved) {
    if (saved) return saved
    if (to.hash) {
      const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches
      const behavior = reduced ? 'auto' : 'smooth'

      // 目标还不存在 —— 配置页的表单是异步加载的，路由变化这一刻页面上只有
      // 一句"正在读取配置"。什么都不做，等数据回来由页面自己补一次
      // （`ConfigView` 的 `load()` 末尾与 `scroll.ts` 都走那条路）。
      const el = document.getElementById(to.hash.replace(/^#/, ''))
      if (!el) return false

      // ⚠️ **这个偏移必须自己取，不能指望 CSS 的 `scroll-margin-top` 兜住。**
      //
      // vue-router 拿到 `el` 之后是**自己算**元素位置的
      // （`getElementPosition`：`elRect.top - docRect.top - offset.top`），
      // 它**根本不调 `scrollIntoView`** —— 而 `scroll-margin` 只有
      // `scrollIntoView` 和原生的片段跳转认。所以在这条路上它是死的。
      //
      // 实测（`tests/browser/shell_probe.mjs`）：同一个 `#system`，
      // 直接打开 `/config#system` 落点是 **72px**（`scrollToHash()` 那条路赢，
      // 它用的是 `scrollIntoView`），而在 `/config` 上再点一次「系统设置」
      // 落点是 **0px** —— 段落标题正好压在顶栏底下。也就是说两条路给出的落点
      // 差整整一个 `scroll-margin-top`，而**哪条赢取决于两个 nextTick 的先后**
      // （路由的滚动排在导航确认之后，页面的那一次排在 `route.hash` 的 watch
      // 之后），于是同一个动作有时对有时错。
      //
      // 取**计算值**而不是写死 72：那个数只该在 `styles.css` 里有一份，
      // 抄到这里就等于埋一处迟早会漂移的副本。
      const top = parseFloat(getComputedStyle(el).scrollMarginTop) || 0
      return { el: to.hash, top, behavior }
    }
    return { top: 0 }
  },
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
