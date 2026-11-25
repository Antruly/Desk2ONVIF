<script setup lang="ts">
import { useId } from 'vue'

/**
 * 品牌标记。
 *
 * 画法沿用 uvscope 的结构：一个带描边的圆角方块，里面是两道青/蓝渐变笔画。
 * 这里画的是**摄像机 + 镜头**（这个项目的本体是桌面推流），而不是直接复用
 * `camera` 图标 —— 图标是 24×24 的单笔画，放大到 36px 的徽标尺寸会显得空。
 *
 * 渐变 id 用 `useId()`：同一个页面里可能出现两个 BrandMark（侧栏 + 登录页
 * 不会同时出现，但以后难说），写死 id 时后一个会复用先一个的渐变定义。
 */
withDefaults(defineProps<{ size?: number }>(), { size: 34 })

const uid = useId()
</script>

<template>
  <svg
    :width="size"
    :height="size"
    viewBox="0 0 40 40"
    fill="none"
    aria-hidden="true"
    class="brand-mark"
  >
    <defs>
      <linearGradient :id="`d2o-mark-${uid}`" x1="7" y1="6" x2="33" y2="35" gradientUnits="userSpaceOnUse">
        <stop stop-color="var(--accent)" />
        <stop offset="1" stop-color="var(--accent-2)" />
      </linearGradient>
    </defs>
    <rect x="1" y="1" width="38" height="38" rx="11" fill="var(--brand-bg)" stroke="var(--brand-border)" />
    <rect
      x="8.5"
      y="12"
      width="16"
      height="12.5"
      rx="3"
      :stroke="`url(#d2o-mark-${uid})`"
      stroke-width="2.1"
    />
    <path
      d="M25 16.2l5.2-3.1a.9.9 0 0 1 1.35.78v8.24a.9.9 0 0 1-1.35.78L25 19.8z"
      :fill="`url(#d2o-mark-${uid})`"
    />
    <circle cx="16.5" cy="18.25" r="2.6" :stroke="`url(#d2o-mark-${uid})`" stroke-width="1.8" />
    <circle cx="27.9" cy="12.8" r="1.1" fill="var(--accent)" />
  </svg>
</template>
