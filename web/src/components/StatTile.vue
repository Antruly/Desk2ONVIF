<script setup lang="ts">
/**
 * 单个指标块。活在实时面板里，所以数值一律 tabular-nums（在 styles.css 的
 * .num 里），否则每秒刷新时字宽变化会让整块面板左右抖。
 */
defineProps<{
  label: string
  value: string | number
  unit?: string
  sub?: string
  tone?: 'warn' | 'crit' | 'good'
  /**
   * 值本身是**文字**而不是数字时用（比如"已就绪"、操作名）。
   * 22px 的正文尺寸放一句话会撑破指标块，而缩小之后整块面板的节奏仍然是齐的。
   */
  small?: boolean
}>()
</script>

<template>
  <div class="tile">
    <div class="tile-label">{{ label }}</div>
    <div class="tile-value num" :class="[tone ? 'tone-' + tone : '', small ? 'small' : '']">
      {{ value }}<span v-if="unit" class="tile-unit">{{ unit }}</span>
    </div>
    <div v-if="sub" class="tile-sub">{{ sub }}</div>
  </div>
</template>
