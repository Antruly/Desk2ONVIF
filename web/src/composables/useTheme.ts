import { computed, ref } from 'vue'

/**
 * 深色 / 浅色主题。
 *
 * 做成模块级单例（与 `useAuth` / `useStats` 同一套做法）：外壳、登录页、以及
 * 背景层读的必须是同一个值，而这里没有 Pinia —— 一份模块级的 ref 就够了。
 *
 * 默认是深色。这台机器的界面大多数时间当监控画面墙用，深色底是产品的默认
 * 形态；浅色是给"白天在明亮办公室里看配置"准备的，不跟随系统自动切换 ——
 * 自动切换会让同一台设备在早晚呈现两种样子，而用户第一次打开时通常正在
 * 排查问题，界面不该在那时自己变。
 *
 * 只写 `data-theme` 一个属性，颜色全部由 CSS token 接管；JS 不碰任何具体颜色，
 * 唯一例外是浏览器地址栏的 `theme-color`（那是文档级元信息，没有 CSS 入口）。
 */

type Theme = 'dark' | 'light'

const STORAGE_KEY = 'd2o.theme'

function readTheme(): Theme {
  try {
    return window.localStorage.getItem(STORAGE_KEY) === 'light' ? 'light' : 'dark'
  } catch {
    return 'dark'
  }
}

const theme = ref<Theme>(typeof window === 'undefined' ? 'dark' : readTheme())
const isLight = computed(() => theme.value === 'light')

/** 把当前主题写到 <html> 上。`color-scheme` 让原生控件（滚动条、下拉列表）跟着走。 */
function apply() {
  if (typeof document === 'undefined') return
  const root = document.documentElement
  root.dataset.theme = theme.value
  root.style.colorScheme = theme.value
  document
    .querySelector('meta[name="theme-color"]')
    ?.setAttribute('content', theme.value === 'light' ? '#f3f5f9' : '#070a10')
}

// 模块第一次被 import 时立刻落地：主题要在首屏绘制之前就正确，否则会先闪一下
// 深色再变成浅色（或反过来）。
apply()

function setTheme(next: Theme) {
  theme.value = next
  apply()
  try {
    window.localStorage.setItem(STORAGE_KEY, next)
  } catch {
    /* 记不住就记不住：这一次切换照常生效，刷新后回到默认深色。 */
  }
}

function toggleTheme() {
  setTheme(theme.value === 'light' ? 'dark' : 'light')
}

export function useTheme() {
  return { theme, isLight, setTheme, toggleTheme }
}
