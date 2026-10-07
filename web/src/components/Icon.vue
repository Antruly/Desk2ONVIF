<script setup lang="ts">
import { computed } from 'vue'
import { ICON_PATHS } from '../icons'
import type { IconName } from '../icons'

const props = withDefaults(
  defineProps<{
    name: IconName
    /** 边长，像素。 */
    size?: number
    /** 线宽。小尺寸下细一点更清楚，大尺寸下粗一点更稳。 */
    stroke?: number
  }>(),
  { size: 18, stroke: 1.8 },
)

const path = computed(() => ICON_PATHS[props.name])
const px = computed(() => `${props.size}px`)
</script>

<template>
  <svg
    :width="px"
    :height="px"
    viewBox="0 0 24 24"
    fill="none"
    :stroke-width="stroke"
    stroke="currentColor"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
    focusable="false"
    :class="{ 'is-spin': name === 'loader' }"
  >
    <path :d="path" />
  </svg>
</template>

<style scoped>
svg {
  display: block;
  flex: none;
}

.is-spin {
  animation: iconSpin 0.9s linear infinite;
}

@keyframes iconSpin {
  to {
    transform: rotate(360deg);
  }
}
</style>
