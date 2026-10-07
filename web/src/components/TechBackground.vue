<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue'

/**
 * 常驻的全屏装饰背景。
 *
 * 五层，从下往上：底色渐变 → 网格 → 三团漂移的光晕 → 三道斜向光束 →
 * 扫描线 + 大字水印 → canvas 粒子星座。
 *
 * ## 三条纪律
 *
 * 1. **`pointer-events: none`，整层都不吃事件。** 它铺满全屏，一旦吃事件，
 *    底下所有东西都点不动 —— 而这一层"看得见但点不到"正是它的全部职责。
 * 2. **尊重 `prefers-reduced-motion`。** 这里有一大片常驻动画，对前庭敏感的
 *    人不是"看着花哨"而是真的会难受。开启该设置时只画一帧静态图，不跑 rAF。
 * 3. **页面不可见时停掉 rAF。** 用户切走标签页之后还在满速画 60fps 的粒子，
 *    纯粹是在烧笔记本的电 —— 而这一幕恰恰是最常见的（把这一页挂在后台当监控）。
 *
 * 粒子数故意压得很少（60）。它和 `filter: blur()` 那几层不同，是每帧真的在
 * 重画 canvas；而这一页的主业是播一路 1080p 的 H.264。
 */

const canvasRef = ref<HTMLCanvasElement | null>(null)

/** 星座的连接距离。再大就糊成一片，再小就看不出"星座"。 */
const LINK_DIST = 120
const COUNT = 60
/** 粒子上限速度。快过这个值就成了"下雪"，和"数据流动"的观感不同。 */
const MAX_V = 0.28
/** 光标影响半径。 */
const CURSOR_R = 170

type P = { x: number; y: number; vx: number; vy: number; r: number }

let raf = 0
let ctx: CanvasRenderingContext2D | null = null
let particles: P[] = []
let w = 0
let h = 0
let dpr = 1
const cursor = { x: -1e4, y: -1e4, active: false }

function onResize() {
  const c = canvasRef.value
  if (!c) return
  w = window.innerWidth
  h = window.innerHeight
  // 上限 1.5：4K 屏上 DPR=2 会让画布像素数翻四倍，而这些粒子本来就带模糊，
  // 多出来的分辨率肉眼看不出来。
  dpr = Math.min(window.devicePixelRatio || 1, 1.5)
  c.width = Math.floor(w * dpr)
  c.height = Math.floor(h * dpr)
  c.style.width = `${w}px`
  c.style.height = `${h}px`
  ctx = c.getContext('2d')
  ctx?.setTransform(dpr, 0, 0, dpr, 0, 0)

  // 粒子密度按面积走：宽屏上 60 个会显得空，窄屏上又会挤。
  const n = Math.max(28, Math.min(COUNT, Math.round((w * h) / 26000)))
  particles = Array.from({ length: n }, () => ({
    x: Math.random() * w,
    y: Math.random() * h,
    vx: (Math.random() - 0.5) * 2 * MAX_V,
    vy: (Math.random() - 0.5) * 2 * MAX_V,
    r: Math.random() * 1.5 + 0.9,
  }))
}

function step() {
  raf = requestAnimationFrame(step)
  if (!ctx) return

  ctx.clearRect(0, 0, w, h)

  for (const p of particles) {
    // 光标吸引：给半径内的粒子一个很小的速度增量（不是直接位移，否则松手时
    // 会看到它们"粘"在原地）。上限靠 MAX_V 兜住。
    if (cursor.active) {
      const dx = cursor.x - p.x
      const dy = cursor.y - p.y
      const d2 = dx * dx + dy * dy
      if (d2 < CURSOR_R * CURSOR_R && d2 > 1) {
        const d = Math.sqrt(d2)
        p.vx += (dx / d) * 0.012
        p.vy += (dy / d) * 0.012
      }
    }

    // 阻尼 + 限速。没有阻尼的话光标扫过之后粒子会一直飞下去。
    p.vx *= 0.995
    p.vy *= 0.995
    const sp = Math.hypot(p.vx, p.vy)
    if (sp > MAX_V) {
      p.vx = (p.vx / sp) * MAX_V
      p.vy = (p.vy / sp) * MAX_V
    }

    p.x += p.vx
    p.y += p.vy

    // 出界回绕。比反弹更耐看：反弹会在边缘堆出一圈粒子。
    if (p.x < -10) p.x = w + 10
    else if (p.x > w + 10) p.x = -10
    if (p.y < -10) p.y = h + 10
    else if (p.y > h + 10) p.y = -10
  }

  // 连线。O(n²)，n≤60，一帧不到 1800 次比较。
  for (let i = 0; i < particles.length; i++) {
    const a = particles[i]
    for (let j = i + 1; j < particles.length; j++) {
      const b = particles[j]
      const dx = a.x - b.x
      const dy = a.y - b.y
      const d2 = dx * dx + dy * dy
      if (d2 > LINK_DIST * LINK_DIST) continue
      const alpha = (1 - Math.sqrt(d2) / LINK_DIST) * 0.22
      ctx.strokeStyle = `rgba(0, 212, 255, ${alpha.toFixed(3)})`
      ctx.lineWidth = 0.7
      ctx.beginPath()
      ctx.moveTo(a.x, a.y)
      ctx.lineTo(b.x, b.y)
      ctx.stroke()
    }
  }

  for (const p of particles) {
    ctx.fillStyle = 'rgba(120, 220, 255, 0.5)'
    ctx.beginPath()
    ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2)
    ctx.fill()
  }

  // 光标本身也画一个柔和的点，让"吸引"这件事有个可见的中心。
  if (cursor.active) {
    const g = ctx.createRadialGradient(cursor.x, cursor.y, 0, cursor.x, cursor.y, 90)
    g.addColorStop(0, 'rgba(0, 212, 255, 0.16)')
    g.addColorStop(1, 'rgba(0, 212, 255, 0)')
    ctx.fillStyle = g
    ctx.beginPath()
    ctx.arc(cursor.x, cursor.y, 90, 0, Math.PI * 2)
    ctx.fill()
  }
}

function onPointerMove(e: PointerEvent) {
  cursor.x = e.clientX
  cursor.y = e.clientY
  cursor.active = true
}

function onPointerLeave() {
  cursor.active = false
  cursor.x = -1e4
  cursor.y = -1e4
}

/**
 * 只在标签页可见时跑。
 *
 * 停的时候必须 `cancelAnimationFrame`，而不是让 `step` 自己判 `document.hidden`
 * 然后什么都不画 —— 后者照样每帧被唤醒一次，省下来的只有绘制开销，而唤醒本身
 * 就是这笔账的大头。
 */
function onVisibility() {
  if (document.hidden) {
    if (raf) cancelAnimationFrame(raf)
    raf = 0
  } else if (!raf && !reduced) {
    step()
  }
}

const reduced =
  typeof window !== 'undefined' && window.matchMedia
    ? window.matchMedia('(prefers-reduced-motion: reduce)').matches
    : false

onMounted(() => {
  onResize()
  window.addEventListener('resize', onResize)
  document.addEventListener('visibilitychange', onVisibility)

  if (reduced) {
    // 静态一帧：背景还在，只是不动。
    if (ctx) {
      for (const p of particles) {
        ctx.fillStyle = 'rgba(120, 220, 255, 0.4)'
        ctx.beginPath()
        ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2)
        ctx.fill()
      }
    }
    return
  }

  window.addEventListener('pointermove', onPointerMove, { passive: true })
  window.addEventListener('pointerleave', onPointerLeave, { passive: true })
  document.addEventListener('mouseleave', onPointerLeave)
  step()
})

onBeforeUnmount(() => {
  if (raf) cancelAnimationFrame(raf)
  raf = 0
  window.removeEventListener('resize', onResize)
  window.removeEventListener('pointermove', onPointerMove)
  window.removeEventListener('pointerleave', onPointerLeave)
  document.removeEventListener('mouseleave', onPointerLeave)
  document.removeEventListener('visibilitychange', onVisibility)
  // 画布是这一层里最占内存的东西，而它是 `<canvas>` 自己持有的（不受 Vue
  // 的响应式管理），显式缩到 0 才算真的还回去。
  const c = canvasRef.value
  if (c) {
    c.width = 0
    c.height = 0
  }
})
</script>

<template>
  <div class="bg" aria-hidden="true">
    <div class="bg-grid"></div>
    <div class="orb orb-1"></div>
    <div class="orb orb-2"></div>
    <div class="orb orb-3"></div>
    <div class="beams">
      <span class="beam"></span>
      <span class="beam"></span>
      <span class="beam"></span>
    </div>
    <div class="watermark">
      <span>DESK2ONVIF</span>
      <span>PROFILE S</span>
      <span>RTSP · ONVIF</span>
    </div>
    <div class="scanline"></div>
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
    radial-gradient(ellipse 80% 55% at 50% -10%, rgba(0, 90, 140, 0.28), transparent 70%),
    linear-gradient(180deg, #070b14 0%, #060a12 55%, #070b14 100%);
}

.bg-grid {
  position: absolute;
  inset: -1px;
  background-image:
    linear-gradient(rgba(0, 212, 255, 0.035) 1px, transparent 1px),
    linear-gradient(90deg, rgba(0, 212, 255, 0.035) 1px, transparent 1px);
  background-size: 50px 50px;
  /* 顶部和底部淡出，免得网格硬生生切在视口边缘上。 */
  mask-image: radial-gradient(ellipse 90% 70% at 50% 40%, #000 40%, transparent 100%);
  -webkit-mask-image: radial-gradient(ellipse 90% 70% at 50% 40%, #000 40%, transparent 100%);
}

/* 三团大的模糊光晕。blur(80px) 是贵的那一档，但它们各自只动 transform
   （合成层上的操作，不触发重排重绘），所以代价可以接受。 */
.orb {
  position: absolute;
  border-radius: 50%;
  filter: blur(80px);
  opacity: 0.5;
  will-change: transform;
}

.orb-1 {
  width: 460px;
  height: 460px;
  top: -140px;
  left: -100px;
  background: radial-gradient(circle, rgba(0, 212, 255, 0.3), transparent 70%);
  animation: drift1 22s ease-in-out infinite;
}

.orb-2 {
  width: 380px;
  height: 380px;
  bottom: -120px;
  right: -80px;
  background: radial-gradient(circle, rgba(167, 139, 250, 0.28), transparent 70%);
  animation: drift2 26s ease-in-out infinite;
}

.orb-3 {
  width: 300px;
  height: 300px;
  top: 42%;
  left: 58%;
  background: radial-gradient(circle, rgba(0, 150, 199, 0.22), transparent 70%);
  animation: drift3 30s ease-in-out infinite;
}

@keyframes drift1 {
  0%,
  100% {
    transform: translate(0, 0) scale(1);
  }
  50% {
    transform: translate(90px, 70px) scale(1.14);
  }
}

@keyframes drift2 {
  0%,
  100% {
    transform: translate(0, 0) scale(1);
  }
  50% {
    transform: translate(-110px, -60px) scale(1.1);
  }
}

@keyframes drift3 {
  0%,
  100% {
    transform: translate(0, 0) scale(1);
  }
  50% {
    transform: translate(-70px, 90px) scale(0.9);
  }
}

.beams {
  position: absolute;
  inset: 0;
}

.beam {
  position: absolute;
  top: -20%;
  height: 140%;
  width: 160px;
  transform: skewX(-20deg);
  background: linear-gradient(
    90deg,
    transparent,
    rgba(0, 212, 255, 0.05),
    rgba(167, 139, 250, 0.035),
    transparent
  );
  animation: beamSweep 19s linear infinite;
}

.beam:nth-child(1) {
  animation-delay: 0s;
}
.beam:nth-child(2) {
  animation-delay: -6.5s;
}
.beam:nth-child(3) {
  animation-delay: -13s;
}

@keyframes beamSweep {
  0% {
    left: -220px;
    opacity: 0;
  }
  12% {
    opacity: 1;
  }
  88% {
    opacity: 1;
  }
  100% {
    left: 102%;
    opacity: 0;
  }
}

/* 大字水印。字要大、要淡、要慢 —— 它的作用是让空白区域不那么空，
   而不是让人去读它。 */
.watermark {
  position: absolute;
  inset: 0;
}

.watermark span {
  position: absolute;
  left: 0;
  white-space: nowrap;
  font-size: clamp(64px, 11vw, 150px);
  font-weight: 700;
  letter-spacing: 0.12em;
  color: rgba(0, 212, 255, 0.028);
  animation: wmScroll 46s linear infinite;
}

.watermark span:nth-child(1) {
  top: 12%;
  animation-delay: 0s;
}
.watermark span:nth-child(2) {
  top: 44%;
  animation-delay: -15s;
}
.watermark span:nth-child(3) {
  top: 76%;
  animation-delay: -30s;
}

@keyframes wmScroll {
  0% {
    transform: translateX(-30%);
  }
  100% {
    transform: translateX(30%);
  }
}

/* 一根 2px 的扫描线从上到下扫。这一根比其它几层都"实"，所以透明度压得很低。 */
.scanline {
  position: absolute;
  left: 0;
  right: 0;
  height: 2px;
  background: linear-gradient(90deg, transparent, var(--primary), transparent);
  opacity: 0.14;
  animation: scan 7s linear infinite;
}

@keyframes scan {
  0% {
    top: -2px;
  }
  100% {
    top: 100%;
  }
}

.particles {
  position: absolute;
  inset: 0;
  display: block;
}
</style>
