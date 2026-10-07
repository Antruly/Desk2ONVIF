import type { AppConfig, LoginResult, SaveConfigResult, SessionInfo, Stats } from './types'

/**
 * HTTP 接口封装。
 *
 * 原则是**不要抛裸异常**：调用方需要的是"给用户看的一句话"，不是 stack。
 * 所以每个失败都返回 { ok:false, error }，把 HTTP 状态、后端正文、网络故障
 * 统一成一条中文消息。
 */

export type ApiResult<T> =
  | { ok: true; data: T }
  | {
      ok: false
      error: string
      status?: number
      /**
       * 失败响应体**如果**是 JSON，原样带出来。
       *
       * 存在的理由很具体：429（登录失败次数过多）的正文里有 `retry_after_s`，
       * 而登录页要靠它起倒计时。只保留一句 error 文本的话，用户看到的是
       * "请稍后再试"，却不知道要等多久 —— 于是他会立刻再试一次，
       * 而每试一次冷却都重新算，越试越进不去。
       */
      body?: unknown
    }

/**
 * 会话失效时的回调。由 `useAuth` 在模块初始化时挂上。
 *
 * 为什么用回调而不是让这里 `import { useAuth }`：`/api/session` 自己也要走
 * 这条路，直接互相 import 会成环（api → useAuth → api）。成环在 ESM 里不一定
 * 报错，但初始化顺序变得依赖 import 语句的书写顺序 —— 那种 bug 只在改无关
 * 代码时才发作。
 */
type UnauthHandler = () => void
let onUnauthorized: UnauthHandler | null = null

export function setUnauthorizedHandler(h: UnauthHandler | null): void {
  onUnauthorized = h
}

/**
 * 会话 cookie 由浏览器自己带着走（后端标了 HttpOnly，JS 读不到也不该读）。
 *
 * `credentials: 'same-origin'` 是必须显式写的**默认值**：将来哪天这个前端被挪到
 * 别的源上（比如 Vite dev server 走 proxy 改成直连），默认行为会从
 * same-origin 变成 omit，症状是"登录成功、下一个请求又是未登录"，而且
 * 网络面板里一切正常（请求发出去了、响应也回来了）。写出来，不改。
 */
const CRED: RequestInit = { credentials: 'same-origin' }

/** 失败响应解析出来的两半：给人看的一句话，和结构化信息（如果有）。 */
interface FailureDetail {
  error: string
  body?: unknown
}

/**
 * 从失败响应里挖出后端想说的话。
 *
 * 错误体的形状没有冻结，所以按 error/message/detail/msg 依次找；都不是就把
 * 正文原样截断显示 —— 只说"HTTP 400"会让人完全无从下手，宁可显示一段原始 JSON。
 */
async function describeFailure(resp: Response): Promise<FailureDetail> {
  let text = ''
  try {
    text = await resp.text()
  } catch {
    text = ''
  }
  if (text) {
    let parsed: unknown
    try {
      parsed = JSON.parse(text)
    } catch {
      // 不是 JSON —— 当纯文本用，没有结构化信息可带。
      return { error: text.length > 400 ? `${text.slice(0, 400)}…` : text }
    }
    const j = parsed as Record<string, unknown>
    const clipped = text.length > 400 ? `${text.slice(0, 400)}…` : text
    for (const key of ['error', 'message', 'detail', 'msg']) {
      const v = j[key]
      // 消息字段找到了也照样带上整个对象：`retry_after_s` 这类字段不在消息里，
      // 而它恰恰是调用方唯一需要的结构信息。
      if (typeof v === 'string' && v.trim()) return { error: v, body: j }
    }
    return { error: clipped, body: j }
  }
  return { error: `HTTP ${resp.status}${resp.statusText ? ' ' + resp.statusText : ''}` }
}

/**
 * 401 的统一处理。
 *
 * **`/api/login` 和 `/api/session` 排除在外**：前者的 401 就是"口令不对"，
 * 是最正常的一种失败；后者是匿名接口，根本不会 401。把它们的 401 也当成
 * "会话过期"，用户会在登录页上被自己的一次手误踢回登录页。
 */
function handleUnauthorized(status: number, url: string): void {
  if (status !== 401) return
  if (url.startsWith('/api/login') || url.startsWith('/api/session')) return
  onUnauthorized?.()
}

async function getJson<T>(url: string): Promise<ApiResult<T>> {
  try {
    const resp = await fetch(url, { ...CRED, headers: { Accept: 'application/json' } })
    if (!resp.ok) {
      handleUnauthorized(resp.status, url)
      const f = await describeFailure(resp)
      return { ok: false, error: f.error, status: resp.status, body: f.body }
    }
    return { ok: true, data: (await resp.json()) as T }
  } catch (err) {
    // fetch 只在网络层失败时 reject。"请求发不出去"最常见的原因就是后端没起来，
    // 这句话得说出来，否则用户会以为是界面坏了。
    return { ok: false, error: `请求失败：${(err as Error).message}（后端没启动或地址不对？）` }
  }
}

async function postJson<T>(url: string, body: unknown): Promise<ApiResult<T>> {
  try {
    const resp = await fetch(url, {
      ...CRED,
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
      body: JSON.stringify(body),
    })
    if (!resp.ok) {
      handleUnauthorized(resp.status, url)
      const f = await describeFailure(resp)
      return { ok: false, error: f.error, status: resp.status, body: f.body }
    }
    // 成功的 POST 不保证有 JSON 体（可能只回 200 空体），空体不该算失败。
    const text = await resp.text()
    if (!text.trim()) return { ok: true, data: undefined as T }
    try {
      return { ok: true, data: JSON.parse(text) as T }
    } catch {
      return { ok: true, data: undefined as T }
    }
  } catch (err) {
    return { ok: false, error: `请求失败：${(err as Error).message}（后端没启动或地址不对？）` }
  }
}

/** 一次性快照。WS 断了的时候也能看到数，不至于整页空白。 */
export function fetchStatus(): Promise<ApiResult<Stats>> {
  return getJson<Stats>('/api/status')
}

export function fetchConfig(): Promise<ApiResult<AppConfig>> {
  return getJson<AppConfig>('/api/config')
}

export function saveConfig(cfg: AppConfig): Promise<ApiResult<SaveConfigResult>> {
  return postJson<SaveConfigResult>('/api/config', cfg)
}

/** 重启 ONVIF / WS-Discovery 响应器。NVR 搜不到设备时先按这个。 */
export function restartOnvif(): Promise<ApiResult<unknown>> {
  return postJson<unknown>('/api/onvif/restart', {})
}

/* ---------- 会话 ---------- */

/** 匿名可达。返回 `authRequired` 为 false 时，这台机器上没开登录。 */
export function fetchSession(): Promise<ApiResult<SessionInfo>> {
  return getJson<SessionInfo>('/api/session')
}

/**
 * 登录。成功时 cookie 由 `Set-Cookie` 落到浏览器里，这里不需要也不该处理它
 * （后端标了 HttpOnly，JS 根本读不到）。
 */
export function postLogin(user: string, password: string): Promise<ApiResult<LoginResult>> {
  return postJson<LoginResult>('/api/login', { user, password })
}

/**
 * 登出。
 *
 * **服务端是无状态的，它什么都没做** —— 令牌签的是每进程一份的随机密钥，
 * 不是口令，所以"登出"只是让浏览器丢掉 cookie，拿着旧令牌的人照样进得来。
 * 真要立刻吊销所有会话只有一条路：重启后端进程。
 */
export function postLogout(): Promise<ApiResult<unknown>> {
  return postJson<unknown>('/api/logout', {})
}
