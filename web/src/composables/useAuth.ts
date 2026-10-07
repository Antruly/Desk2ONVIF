import { ref } from 'vue'
import { fetchSession, postLogin, postLogout, setUnauthorizedHandler } from '../api'
import type { LoginResult } from '../types'

/**
 * Web 控制台的登录状态。
 *
 * 做成模块级单例，理由和 `useStats` 一样：顶栏、路由守卫、以及每个页面都要
 * 读同一份状态，而"我登录了吗"这个问题**每次都要问后端**（cookie 是 HttpOnly
 * 的，JS 看不到它，也就无从自己判断有没有过期）。
 *
 * ## 权威在后端，前端只缓存结论
 *
 * 这份状态唯一的来源是 `GET /api/session`。前端**不**自己推断（比如"我登录过
 * 所以我现在还是登录的"）—— 令牌有效期 7 天，用户完全可能开着页面过夜，
 * 而推断出来的"已登录"会让他点任何东西都得到 401，且没有任何提示。
 */

const authRequired = ref(false)
const authenticated = ref(false)
const user = ref('')
/** 是否成功问过后端一次。false 时路由守卫不该做任何决定（会闪一下登录页）。 */
const checked = ref(false)
/** 问后端这件事本身失败了（后端没起来）。和"问到了、答案是未登录"是两回事。 */
const sessionError = ref('')

/**
 * 并发合流。
 *
 * 路由守卫和 App 的 onMounted 会在同一帧里各问一次，而这两个请求的响应顺序
 * 没有保证。不合流的话，先返回的那个把 `checked` 置真、后返回的又覆盖一次；
 * 更糟的是两次请求之间用户刚好登录了，于是"旧的那次"把 `authenticated`
 * 又改回 false —— 表现是刚登录就被踢回登录页，刷新一下又好了。
 */
let inflight: Promise<void> | null = null

async function doEnsure(): Promise<void> {
  const res = await fetchSession()
  if (!res.ok) {
    // 后端没起来 / 网络不通。**不要**把 authenticated 置成 false 之外的东西，
    // 也不要把 authRequired 猜成 true：猜错的方向是要么把用户锁在登录页外
    // 面，要么把他挡在门外，两个都很难查。
    sessionError.value = res.error
    checked.value = true
    return
  }
  sessionError.value = ''
  authRequired.value = res.data.auth_required === true
  // 没开登录时后端给的 authenticated 是 true（见 has_session 的 earliest
  // return），所以这里不用特判。
  authenticated.value = res.data.authenticated === true
  user.value = typeof res.data.user === 'string' ? res.data.user : ''
  checked.value = true
}

/** 问后端"我登录了吗"。幂等，且在并发时只发一个请求。 */
export function ensureSession(force = false): Promise<void> {
  if (!force && checked.value) return Promise.resolve()
  if (inflight) return inflight
  inflight = doEnsure().finally(() => {
    inflight = null
  })
  return inflight
}

export type LoginOutcome = { ok: true } | { ok: false; error: string; retryAfterS?: number }

/**
 * 登录。成功后**必须**再问一次 `/api/session` —— 后端在 200 里回的
 * `user` 是它自己那份，而"这台机器上到底要不要登录"这件事也该以后端为准。
 */
export async function login(name: string, password: string): Promise<LoginOutcome> {
  const res = await postLogin(name, password)
  if (!res.ok) {
    // 429 的正文里有 retry_after_s，登录页要靠它起倒计时。它不在失败响应的
    // 通用形状里（那是 `{error, message, …}` 的任一），所以从 body 上取。
    const body = res.body as LoginResult | undefined
    const wait = typeof body?.retry_after_s === 'number' ? body.retry_after_s : undefined
    return { ok: false, error: res.error, retryAfterS: wait }
  }
  if (res.data && res.data.ok === false) {
    // 理论上不会走到（失败都是 4xx），但后端真回了 200+ok:false 时不能当成成功。
    return { ok: false, error: res.data.error ?? '登录失败。' }
  }
  await ensureSession(true)
  // 后端说"不用登录"或者 cookie 没落地时，authenticated 会是 false 而
  // authRequired 也是 false —— 那种情况下把用户放进去是对的。
  if (authRequired.value && !authenticated.value) {
    return {
      ok: false,
      error: '登录请求成功了，但会话没有建立起来。浏览器可能禁用了 Cookie。',
    }
  }
  return { ok: true }
}

/**
 * 登出。
 *
 * 先清本地状态再发请求：请求失败（后端刚好挂了）不该让用户卡在"已登录"的
 * 界面上 —— 他点了退出，界面就该退出。
 */
export async function logout(): Promise<void> {
  authenticated.value = false
  user.value = ''
  await postLogout()
}

/**
 * 会话失效。由两条路触发：
 *   - `api.ts` 收到任何 `/api/*` 的 401
 *   - `/ws/stats` 被后端用 1008 关掉（WS 升级不经过 HTTP 的鉴权中间件，
 *     它有自己的守卫，见 web_server.cpp 的 `reject_ws`）
 *
 * 两处都要报，是因为它们的覆盖面不同：只连 WS 的页面（比如挂着不动的状态页）
 * 一个 HTTP 请求都不发，而只发 HTTP 的页面不会碰到 WS。
 */
export function noteUnauthorized(): void {
  if (!authRequired.value) {
    // 还不知道要不要登录就被 401 了，说明本地状态过期了。让守卫重新问一次。
    checked.value = false
  }
  authenticated.value = false
  user.value = ''
}

/** 已登录时顶栏显示的名字。未登录或没开登录时是空串。 */
export function currentUser(): string {
  return user.value
}

// api.ts 收到 401 时回调这里。挂在模块作用域，import 一次就生效。
setUnauthorizedHandler(noteUnauthorized)

export function useAuth() {
  return {
    authRequired,
    authenticated,
    user,
    checked,
    sessionError,
    ensureSession,
    login,
    logout,
    noteUnauthorized,
  }
}
