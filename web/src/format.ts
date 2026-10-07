/** 显示层的格式化工具。只做展示，不参与任何判断。 */

/** 字节数 → 人话。用 1024 进制，和后端日志里的口径一致。 */
export function formatBytes(n: number): string {
  if (!Number.isFinite(n) || n <= 0) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let v = n
  let i = 0
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024
    i++
  }
  return `${v.toFixed(v >= 100 || i === 0 ? 0 : 1)} ${units[i]}`
}

/** 秒 → 00:00:00 / 1天02:03:04。超过一天还按小时堆会变成一串读不出来的数字。 */
export function formatDuration(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return '00:00:00'
  const s = Math.floor(seconds)
  const days = Math.floor(s / 86400)
  const hh = String(Math.floor((s % 86400) / 3600)).padStart(2, '0')
  const mm = String(Math.floor((s % 3600) / 60)).padStart(2, '0')
  const ss = String(s % 60).padStart(2, '0')
  return days > 0 ? `${days}天 ${hh}:${mm}:${ss}` : `${hh}:${mm}:${ss}`
}

/** 大整数加千分位，否则七位数的字节计数读不出来。 */
export function formatInt(n: number): string {
  if (!Number.isFinite(n)) return '0'
  return Math.round(n).toLocaleString('zh-CN')
}

/**
 * 复制到剪贴板。
 *
 * 不用 navigator.clipboard 一条路走到黑：它只在安全上下文里存在，而这个界面
 * 平时就是通过 http://192.168.x.x 打开的，那里 navigator.clipboard 是 undefined。
 * 所以退回 textarea + execCommand —— 这个老办法在非安全上下文里反而能用。
 */
export async function copyText(text: string): Promise<boolean> {
  if (!text) return false
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text)
      return true
    }
  } catch {
    // 落到下面的兜底路径
  }
  try {
    const ta = document.createElement('textarea')
    ta.value = text
    // 留在视口内但不可见：display:none 或 visibility:hidden 的元素无法被选中，
    // 那样 execCommand 会静默失败。
    ta.style.position = 'fixed'
    ta.style.top = '0'
    ta.style.opacity = '0'
    ta.setAttribute('readonly', '')
    document.body.appendChild(ta)
    ta.select()
    const ok = document.execCommand('copy')
    document.body.removeChild(ta)
    return ok
  } catch {
    return false
  }
}
