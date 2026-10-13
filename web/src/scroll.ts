/**
 * 把某个锚点滚进视口。
 *
 * 为什么有了 `router.ts` 的 `scrollBehavior` 还需要这个：
 *
 *   (a) **配置页的表单是异步加载的。** `scrollBehavior` 跑的时候 `load()` 还没
 *       回来，页面上只有一句"正在读取配置" —— 目标段落**根本不存在**，
 *       于是那一次滚动无声地什么都没做。等数据到了，位置也就没人再管了。
 *   (b) **已经在 `/config` 上再点另一个子项时，path 没变、只有 hash 变。**
 *       组件不会重新挂载，`scrollBehavior` 到底会不会被调用取决于 vue-router
 *       的版本行为 —— 而这正是"点了菜单没反应"那种缺陷，不能赌。
 *   (c) **点的就是当前这一项时，URL 一模一样**，vue-router 直接判为重复导航，
 *       什么都不做。用户把页面滚上去之后再点一次，期待的是滚回去。
 *
 * 所以做成一个显式的、**幂等**的动作，三处（App 的重复点击、ConfigView 的
 * 加载完成后、ConfigView 的 hash watch）都调它。重复滚一次没有副作用，
 * 因此和 `scrollBehavior` 重叠也无所谓 —— 而这份实现只有一处，
 * 不会出现"两边各写一遍、然后漂移"。
 *
 * 顶部那条状态栏是 sticky 的，段落标题会被它盖住。这个偏移**不由这里负责**，
 * 而是 CSS 里的 `section[id] { scroll-margin-top: … }` —— `scrollIntoView`
 * 认这个属性。写在这里只是免得下一个人以为漏了。
 *
 * ⚠️ 但 `router.ts` 的 `scrollBehavior` 是**另一条路**：vue-router 拿到 `el`
 * 之后自己算元素位置，不调 `scrollIntoView`，所以那边得**把 CSS 里那个数读回来**
 * 再当 `top` 偏移传给它——见那段注释和 `tests/browser/shell_probe.mjs` 里
 * 量到的 72 vs 0。
 */
export function scrollToHash(hash: string): void {
  if (!hash) return
  // 用 getElementById 而不是 querySelector(hash)：hash 里只要有一个 `:` 或
  // `.`，querySelector 会把它当选择器语法解析并抛异常。id 是我们自己的，
  // 但"刚好有个 id 长得像选择器"这种事故不值得留机会。
  const el = document.getElementById(hash.replace(/^#/, ''))
  if (!el) return
  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches
  el.scrollIntoView({ behavior: reduced ? 'auto' : 'smooth', block: 'start' })
}
