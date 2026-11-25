<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue'

/**
 * 常驻的全屏装饰背景。
 *
 * 只剩三层，从下往上：底色与极淡的顶部径向渐变 → 很淡的网格 → canvas 粒子
 * 星座。以前那三团光晕、斜向光束、扫描线和大字水印都去掉了 —— 它们每一层
 * 都在持续动画，而这一页的主业是播一路 1080p 的 H.264；背景不该和画面抢 GPU。
 *
 * 四条纪律：
 *
 * 1. **`pointer-events: none`，整层都不吃事件。** 它铺满全屏，一旦吃事件，
 *    底下所有东西都点不动 —— 而"看得见但点不到"正是它的全部职责。
 * 2. **30fps 上限。** rAF 仍然按屏幕刷新率唤醒，但只有距上一帧满 33ms 才真正
 *    重绘。粒子本来就是慢速漂移，60fps 和 30fps 肉眼分不出来，功耗差一倍。
 * 3. **尊重 `prefers-reduced-motion`。** 开启该设置时只画一帧静态图，不跑 rAF。
 * 4. **页面不可见时停掉 rAF。** 停的时候必须 `cancelAnimationFrame`，而不是让
 *    回调自己判 `document.hidden` 然后什么都不画 —— 后者照样每帧被唤醒一次，
 *    省下来的只有绘制开销，而唤醒本身就是这笔账的大头。
 *
 * 粒子数上限 50，密度按视口面积收缩；DPR 上限 1.5 —— 4K 屏上 DPR=2 会让
 * 画布像素数翻四倍，而这些小点本来就看不出多出来的分辨率。
 */

const canvasRef = ref<HTMLCanvasElement | null>(null)

const COUNT = 50
/** 连线距离。再大就糊成一片，再小就看不出"星座"。 */
const LINK_DIST = 112
/** 粒子移动上限。快过这个值就成了"下雪"，和"数据流动"的观感不同。 */
const MAX_V = 0.22
const FRAME_MS = 1000 / 30

type Particle = { x: number; y: number; vx: number; vy: number; r: number }

let raf = 0
let lastPaint = 0
let ctx: CanvasRenderingContext2D | null = null
let particles: Particle[] = []
let w = 0
let h = 0

const reduced =
  typeof window !== 'undefined' && window.matchMedia
    ? window.matchMedia('(prefers-reduced-motion: reduce)').matches
    : false

function spawn() {
  // 密度按面积走：宽屏上 50 个会显得空，窄屏上又会挤。下限 18，上限 50。
  const n = Math.max(18, Math.min(COUNT, Math.round((w * h) / 42000)))
  particles = Array.from({ length: n }, () => ({
    x: Math.random() * w,
    y: Math.random() * h,
    vx: (Math.random() - 0.5) * 2 * MAX_V,
    vy: (Math.random() - 0.5) * 2 * MAX_V,
    r: Math.random() * 1.3 + 0.8,
  }))
}

function onResize() {
  const c = canvasRef.value
  if (!c) return
  w = window.innerWidth
  h = window.innerHeight
  const dpr = Math.min(window.devicePixelRatio || 1, 1.5)
  c.width = Math.floor(w * dpr)
  c.height = Math.floor(h * dpr)
  c.style.width = `${w}px`
  c.style.height = `${h}px`
  ctx = c.getContext('2d')
  ctx?.setTransform(dpr, 0, 0, dpr, 0, 0)
  spawn()
  // 静态模式（或者标签页不可见时）尺寸变了也要把那一帧重画出来，
  // 否则窗口拉伸之后画布会是一块空白。
  if (reduced || document.hidden) paint(false)
}

/** 画一帧。`move` 为 false 时只画当前位置 —— reduced-motion 的静态图走这条。 */
function paint(move: boolean) {
  if (!ctx) return
  ctx.clearRect(0, 0, w, h)

  if (move) {
    for (const p of particles) {
      p.x += p.vx
      p.y += p.vy
      // 出界回绕。比反弹更耐看：反弹会在边缘堆出一圈粒子。
      if (p.x < -10) p.x = w + 10
      else if (p.x > w + 10) p.x = -10
      if (p.y < -10) p.y = h + 10
      else if (p.y > h + 10) p.y = -10
    }
  }

  // 连线。O(n²)，n≤50，一帧一千多次比较，可以忽略。
  for (let i = 0; i < particles.length; i++) {
    const a = particles[i]
    for (let j = i + 1; j < particles.length; j++) {
      const b = particles[j]
      const dx = a.x - b.x
      const dy = a.y - b.y
      const d2 = dx * dx + dy * dy
      if (d2 > LINK_DIST * LINK_DIST) continue
      const alpha = (1 - Math.sqrt(d2) / LINK_DIST) * 0.2
      ctx.strokeStyle = `rgba(75, 227, 194, ${alpha.toFixed(3)})`
      ctx.lineWidth = 0.7
      ctx.beginPath()
      ctx.moveTo(a.x, a.y)
      ctx.lineTo(b.x, b.y)
      ctx.stroke()
    }
  }

  for (const p of particles) {
    ctx.fillStyle = 'rgba(124, 156, 255, 0.55)'
    ctx.beginPath()
    ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2)
    ctx.fill()
  }
}

function loop(now: number) {
  raf = window.requestAnimationFrame(loop)
  if (now - lastPaint < FRAME_MS) return
  lastPaint = now
  paint(true)
}

function start() {
  if (raf || reduced || document.hidden) return
  lastPaint = 0
  raf = window.requestAnimationFrame(loop)
}

function stop() {
  if (raf) window.cancelAnimationFrame(raf)
  raf = 0
}

function onVisibility() {
  if (document.hidden) stop()
  else start()
}

onMounted(() => {
  onResize()
  window.addEventListener('resize', onResize)
  document.addEventListener('visibilitychange', onVisibility)

  if (reduced) {
    // 静态一帧：背景还在，只是不动。
    paint(false)
    return
  }
  start()
})

onBeforeUnmount(() => {
  stop()
  window.removeEventListener('resize', onResize)
  document.removeEventListener('visibilitychange', onVisibility)
  // canvas 的像素缓冲是它自己持有的（不归 Vue 管），显式缩到 0 才算真的还回去。
  const c = canvasRef.value
  if (c) {
    c.width = 0
    c.height = 0
  }
  ctx = null
})
</script>

<template>
  <div class="bg" aria-hidden="true">
    <div class="bg-grid"></div>
    <canvas ref="canvasRef" class="particles"></canvas>
  </div>
</template>

<style scoped>
.bg {
  position: fixed;
  inset: 0;
  z-index: 0;
  overflow: hidden;
  /* 整层不吃事件。铺满全屏的装饰层一旦吃事件，下面所有东西都点不动。 */
  pointer-events: none;
  background:
    radial-gradient(
      ellipse 85% 55% at 50% -12%,
      color-mix(in srgb, var(--accent) 9%, transparent),
      transparent 65%
    ),
    linear-gradient(180deg, var(--bg) 0%, var(--bg-soft) 58%, var(--bg) 100%);
}

.bg-grid {
  position: absolute;
  inset: -1px;
  background-image:
    linear-gradient(var(--grid-line) 1px, transparent 1px),
    linear-gradient(90deg, var(--grid-line) 1px, transparent 1px);
  background-size: 42px 42px;
  /* 顶部和底部淡出，免得网格硬生生切在视口边缘上。 */
  mask-image: radial-gradient(ellipse 88% 70% at 50% 36%, #000 38%, transparent 100%);
  -webkit-mask-image: radial-gradient(ellipse 88% 70% at 50% 36%, #000 38%, transparent 100%);
}

.particles {
  position: absolute;
  inset: 0;
  display: block;
}
</style>
