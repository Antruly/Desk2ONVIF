/**
 * 「服务刚被重启过 / 刚恢复出厂」这条消息的载体。
 *
 * 为什么它得单独存在，而不是用路由的 query 传一句就完事：
 *
 * 重启进程会换掉会话密钥（令牌签的是每进程一份的随机密钥，见 `web_session.h`），
 * 所以**重启之后所有人都会被踢出去**。而"被踢出去"这件事在这棵树里有**两条**
 * 独立的路会走到登录页：
 *
 *   1. 配置页自己的轮询发现 `/api/status` 回 401，主动 `router.replace` 过去；
 *   2. 统计推送的 WebSocket 被服务端以 1008 关掉 → `useStats` 重连失败 →
 *      `useAuth.noteUnauthorized()` 把 `authenticated` 置 false →
 *      `App.vue` 的 `watch(authenticated)` 把人送到 `/login?next=…`。
 *
 * 第 2 条什么时候发生取决于 WS 的重连退避，谁也说不准它会不会抢在第 1 条前面。
 * 两条路都会走，但**只有一条**知道"为什么"。把消息放在路由 query 上就等于赌
 * 第 1 条先到 —— 赌输的症状是"用户等了 40 秒，然后被扔到一个没有任何解释的
 * 登录页"，而那正是这个功能唯一要避免的事。
 *
 * 所以消息放在一个**与导航顺序无关**的地方：模块作用域（SPA 内的路由跳转不会
 * 重新加载模块，所以它必然还在）。谁先跳过去都读得到。
 *
 * 不做 sessionStorage：那东西在一个标签页里跨刷新还在，于是"昨天重启过一次"
 * 会一直挂在今天的登录页上，而它看起来完全像一条刚刚发生的事。
 */

export type LifecycleReason = 'restarted' | 'reset'

interface LifecycleNote {
  reason: LifecycleReason
  /** 给用户看的一整句话。措辞由设置它的那一处负责（它才知道是重启还是恢复出厂）。 */
  text: string
}

let pending: LifecycleNote | null = null

/**
 * 在**按下按钮之后、还没确认结果之前**就该调用。
 *
 * 理由同上：那两条路谁都可能先到，等确认完结果再写就晚了。而"重启会让会话失效"
 * 这件事在请求被接受的那一刻就已经是确定的了 —— 所以先写下去不构成撒谎。
 */
export function setLifecycleNote(reason: LifecycleReason, text: string): void {
  pending = { reason, text }
}

/** 读，但不清。登录页可能被重挂（比如路由来回切），那条解释不该消失。 */
export function peekLifecycleNote(): LifecycleNote | null {
  return pending
}

/**
 * 清。在**登录成功之后**清 —— 那是"用户已经看到了、并且已经回到工作状态"的时刻。
 * 不清的话，下一次因为别的缘故（令牌过期、改了口令）被踢到登录页时，
 * 顶上会挂着一条张冠李戴的"服务刚重启过"。
 */
export function clearLifecycleNote(): void {
  pending = null
}
