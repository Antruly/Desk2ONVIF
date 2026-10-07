import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 构建产物直接落到 web/dist —— 后端 --web-root 的默认值就是它，
// 改这里等于同时要改 C++ 那边的默认值，所以不要动。
export default defineConfig({
  plugins: [vue()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
  server: {
    proxy: {
      // 开发时前端跑在 5173，后端在 8080，靠这三条把请求转过去。
      '/api': { target: 'http://127.0.0.1:8080', changeOrigin: true },
      // ws: true 是必须的，否则 /ws/stats 的升级请求会被 Vite 当成普通 HTTP 请求挡掉，
      // 表现为前端一直"连不上"而看不出原因。
      '/ws': { target: 'http://127.0.0.1:8080', ws: true, changeOrigin: true },
      '/onvif': { target: 'http://127.0.0.1:8080', changeOrigin: true },
      // 快照不在上面三个前缀里，单独列一条；不代理的话开发时它会被 Vite 接走返回 404，
      // 而生产环境（后端直接服务）是好的 —— 那种"只有开发环境坏"的差异最难查。
      '/snapshot.jpg': { target: 'http://127.0.0.1:8080', changeOrigin: true },
    },
  },
})
