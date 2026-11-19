import { ref } from 'vue'
import { fetchConfig, reloadPipeline, saveConfig } from '../api'
import type { AppConfig, ChannelConfig } from '../types'

/**
 * 全站共用的**一份配置副本**。
 *
 * 为什么是模块级单例：通道管理页加了通道、详情页接着要按新号打开它；系统页
 * 改了端口，回来时通道页不该还显示旧值。每个页面各存一份 `AppConfig` 的结果
 * 是"同一次改动在 A 页生效、B 页没变" —— 而这类不一致最终都会表现成
 * "我改了，怎么没用"。
 *
 * ## 两种保存语义（这是新版界面的核心区分）
 *
 * - `save(patch)`：只落盘。系统设置页用（端口、口令这一类要重启才生效的东西，
 *   必须让用户看清楚、按下保存）。
 * - `applyMedia(patch)`：落盘 + `POST /api/reload`。**通道视频参数用**
 *   ——"点击即生效"，代价是这一路（以及共享的 RTSP 服务器）重开一次。
 *
 * 水印是**热生效**的：通道详情页改水印只调 `save()`，不重开管线。后端
 * `apply_config` 会把只有 `osd` 变化的那一次直接判成已生效。
 */

export interface SaveOutcome {
  ok: boolean
  error?: string
  /** 后端拼好的那句话（含"哪些当场生效 / 哪些要重启"） */
  note?: string
  /** 保存后仍然待生效的字段（点分路径） */
  pending: string[]
}

const config = ref<AppConfig | null>(null)
const loading = ref(false)
const loadError = ref('')
let inflight: Promise<AppConfig | null> | null = null

/** 拉一次权威配置。并发调用共用同一次请求。 */
async function load(force = false): Promise<AppConfig | null> {
  if (!force && config.value) return config.value
  if (inflight) return inflight
  loading.value = true
  loadError.value = ''
  inflight = (async () => {
    const res = await fetchConfig()
    if (res.ok) {
      const data = res.data as AppConfig
      config.value = data
      return data
    }
    loadError.value = res.error
    return null
  })()
  try {
    return await inflight
  } finally {
    inflight = null
    loading.value = false
  }
}

/**
 * 发一次**局部补丁**并回读。
 *
 * 后端 `config_from_json` 是以"当前生效值"为底合并的，所以补丁里没写的键
 * 一律不动 —— 这正是"只发电量改动的那一小块"能安全成立的原因。
 */
async function save(patch: unknown): Promise<SaveOutcome> {
  const res = await saveConfig(patch as AppConfig)
  if (!res.ok) {
    return { ok: false, error: res.error, pending: [] }
  }
  // 回读一次：后端会夹取越界值、补默认路径、分配通道号 —— 界面要显示真正
  // 生效的那份，而不是我们刚敲进去的那份。
  await load(true)
  return {
    ok: true,
    note: res.data?.note,
    pending: Array.isArray(res.data?.pending) ? res.data.pending : [],
  }
}

/**
 * 保存 + 重开管线。**通道视频/来源/路径类改动都走这条。**
 *
 * `reload` 的回调是异步的（后端 202），这里不假装它已经完成 —— 返回的
 * `note` 后面由调用方显示"正在重开管线…"，界面自己会在下一次 `/api/config`
 * 或 `/ws/stats` 刷新时拿到新参数。
 */
async function applyMedia(patch: unknown): Promise<SaveOutcome> {
  const r = await save(patch)
  if (!r.ok) return r
  const rr = await reloadPipeline()
  if (!rr.ok) {
    return {
      ok: false,
      error: `配置已保存，但重开管线失败：${rr.error}`,
      pending: r.pending,
    }
  }
  return { ...r, note: rr.data?.note ?? '已应用，管线正在重开。' }
}

/** 当前的通道 2 起列表（通道 1 不在里面）。 */
function items(): ChannelConfig[] {
  return config.value?.channels?.items ?? []
}

/** 按**稳定通道号**取配置项；通道 1 返回 null（它在顶层 `capture/rtsp/osd`）。 */
function channelByNumber(n: number): ChannelConfig | null {
  return items().find((c) => (c.number ?? 0) === n) ?? null
}

/**
 * 把修改应用到某一路，返回**完整的 items 数组**（后端对数组是整体替换）。
 *
 * 传进来的是"按号找到的那一项"的新版本；通道 1 不在这里，调用方走顶层补丁。
 */
function replaceChannel(n: number, next: ChannelConfig): ChannelConfig[] {
  return items().map((c) => ((c.number ?? 0) === n ? next : c))
}

export function useConfig() {
  return {
    config,
    loading,
    loadError,
    load,
    save,
    applyMedia,
    items,
    channelByNumber,
    replaceChannel,
  }
}
