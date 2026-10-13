// 版面探针：**不起后端**，只起一个把 `web/dist` 端出去的静态小服务器。
//
// 为什么要有第二个夹具量同一件事
// ---------------------------------
// `shell_probe.mjs` 的 `[2b]` 段量的是同一组不变量（内容滚动时侧栏/顶栏不跟着
// 走），但它跑在**真的后端**上。而那个后端在裸形态（`--rtsp --http`）下无可避免
// 地会起一条 2880x1920 的 DXGI 采集 + x264 编码管线 —— 命令行里**没有任何开关**
// 能关掉它（已核过 `main.cpp` 的 `kFlagNames[]`：只有 `--no-rtsp`/`--no-http`/
// `--no-onvif` 这类，没有 `--no-record`/`--no-capture`）。这台机器上同时还有用户
// 自己在跑的 `D:\Desk2ONVIF\desk2onvif.exe`（PID 17008，同一条 2880x1920 管线），
// 于是 Chrome 在夹具里连着两次以 `0xC0000005` 死在启动那一刻。
//
// 而**版面不依赖采集**。这一条判的是 CSS 的层叠与滚动容器的归属，跟有没有画面、
// 有没有 RTSP、有没有 ONVIF 全都无关。所以这里把被依赖的东西缩到最小的那一份：
//   - 静态：直接在磁盘上读 `web/dist`（验的就是**真正会被发布出去**的那份产物）
//   - 接口：`/api/session` 答「没开登录」，于是路由守卫直接放行，**不用登录**
//   - `/api/config` 回 `{}` —— `ConfigView` 的 `mergeConfig()` 会补上 `defaults()`，
//     于是整张高表单照常渲染（这是它够用的全部理由）
//   - `/ws/stats` 故意不实现：它连不上只让侧栏底部的版本号空着，不影响版面
//
// 于是这个夹具**不需要 DXGI、不需要 RTSP、不需要 x264**，也就与那两条并发管线
// 的内存竞争彻底无关。
//
// 两个夹具的关系是「一条不变量、两把尺子」：这一把确定、便宜、随时能跑；
// `shell_probe.mjs` 那一把贵、要真后端，但它顺带证明了**真页面上**也确实滚得动。
// 两边都不许在对方红的时候还全绿。
//
// 用法：node tests/browser/layout_probe.mjs [--headed] [--keep] [--port 8143]

import { chromium } from 'playwright-core';   // 裸标识符：从 tests/browser/node_modules 解析
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const DIST = path.join(ROOT, 'web', 'dist');

function args() {
  const a = process.argv.slice(2);
  const o = { port: 8143, headless: true, keep: false };
  for (let i = 0; i < a.length; i++) {
    if (a[i] === '--port') o.port = +a[++i];
    else if (a[i] === '--headed') o.headless = false;
    else if (a[i] === '--keep') o.keep = true;
  }
  return o;
}
const A = args();
const BASE = `http://127.0.0.1:${A.port}`;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const outDir = path.join(ROOT, 'out', 'browser');
fs.mkdirSync(outDir, { recursive: true });

let ok = 0, bad = 0;
function check(cond, what, detail = '') {
  if (cond) { ok++; console.log(`  [OK]   ${what}`); }
  else { bad++; console.log(`  [FAIL] ${what}${detail ? `\n         ${detail}` : ''}`); }
}

// ------------------------------------------------------------ 打桩的后端
//
// 契约照 `web/src/api.ts` 的 `getJson()`：**HTTP 200 + 一份普通 JSON** 就是
// `{ok:true, data}`，没有外层信封。所以桩必须回 200，回 404/500 会被读成失败。

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.ico': 'image/x-icon',
  '.woff2': 'font/woff2',
  '.map': 'application/json; charset=utf-8',
};

const STUBS = {
  // 路由守卫就认这两个字段：`auth_required === false` ⇒ 一律放行。
  '/api/session': { auth_required: false, authenticated: true },

  // `{}` 就够 —— `ConfigView` 走的是 mergeConfig(res.data) + Object.assign，
  // 缺的字段全由 defaults() 补上，于是整张高表单照常渲染。
  '/api/config': {},

  '/api/audio/devices': { capture: [], render: [] },

  // 时间那一组读态字段。给全，免得界面里某个算术拿到 undefined。
  '/api/time': {
    host_now: 0, device_now: 0, offset_s: 0, synced: false, synced_at: 0,
    mode: 'manual', ntp_server: '', ntp_interval_s: 0, configured_offset_s: 0,
    last_sync_at: 0, last_sync_state: '', last_sync_error: '',
  },

  // `ConfigView` 只在重启流程里探它，探的也只是状态码。给一份能解析的就够。
  '/api/status': { version: '0.0.0-stub', running: false, ticks: 0, state: 'idle' },

  '/api/audit': { entries: [], files: 0, bytes: 0, keep_days: 180, dir: '' },
};

function serveStatic(req, res, pathname) {
  // SPA 回落：`/config` 这类站内地址在磁盘上没有对应文件，回落给 index.html。
  let rel = decodeURIComponent(pathname).replace(/^\/+/, '');
  let file = path.resolve(DIST, rel);
  if (!file.startsWith(DIST)) { res.writeHead(403).end('forbidden'); return; }
  if (!rel || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    file = path.join(DIST, 'index.html');
  }
  if (!fs.existsSync(file)) { res.writeHead(404).end('no build'); return; }
  const body = fs.readFileSync(file);
  res.writeHead(200, {
    'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
    'Content-Length': body.length,
    'Cache-Control': 'no-store',
  });
  res.end(body);
}

function startStub() {
  const server = http.createServer((req, res) => {
    const pathname = new URL(req.url, BASE).pathname;
    if (Object.prototype.hasOwnProperty.call(STUBS, pathname)) {
      const body = Buffer.from(JSON.stringify(STUBS[pathname]));
      res.writeHead(200, {
        'Content-Type': 'application/json; charset=utf-8',
        'Content-Length': body.length,
        'Cache-Control': 'no-store',
      });
      res.end(body);
      return;
    }
    if (pathname.startsWith('/api/')) {
      res.writeHead(404, { 'Content-Type': 'application/json' });
      res.end('{"error":"layout_probe 没有这个桩"}');
      return;
    }
    serveStatic(req, res, pathname);
  });
  // `/ws/stats` 与 `/ws/preview` 的 upgrade 不接。不接的话 Node 默认把连接关掉，
  // 前端那边表现为一次失败的 WS —— 侧栏的版本号因此空着，版面一点不受影响。
  server.on('upgrade', (req, socket) => socket.destroy());
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(A.port, '127.0.0.1', () => resolve(server));
  });
}

/**
 * 轮询到 `window.scrollY` 不再变为止。
 *
 * 平滑滚动**没有"滚完了"这个时刻**，只有一个可观测的终点（陷阱 98）。
 * 固定睡 N 毫秒再读，在动画还没停时会截到一张半路上的图 —— 而"还在滚"和
 * "压根没滚"报出来是同一个 `false`。
 */
async function stableScrollY(page, cap = 60) {
  let prev = -1, same = 0;
  for (let i = 0; i < cap; i++) {
    const y = await page.evaluate(() => Math.round(window.scrollY));
    if (y === prev) { if (++same >= 2) return y; } else { same = 0; prev = y; }
    await sleep(50);
  }
  return prev;
}

// ---------------------------------------------------------------- 主流程
let server = null, browser = null, ctx = null;
let exitCode = 1;

try {
  if (!fs.existsSync(path.join(DIST, 'index.html'))) {
    throw new Error(`没有 ${DIST}\\index.html —— 先跑 npm --prefix web run build`);
  }
  server = await startStub();
  console.log(`[probe] 静态桩已就绪 ${BASE}（不连真后端，/api/* 全部打桩）`);

  browser = await chromium.launch({ channel: 'chrome', headless: A.headless });
  ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  const page = await ctx.newPage();
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));

  // ==================================================================
  // [1] 前提：整张配置表单真的渲染出来了
  // ==================================================================
  //
  // 这一条红了，下面三条**一个字都不能信** —— 页面上只有一个"正在读取配置"
  // 的话它也滚得动、侧栏也贴在顶上，但那说明不了任何事。
  console.log('\n[1] 配置页渲染');
  await page.goto(`${BASE}/config`, { waitUntil: 'domcontentloaded' });
  await page.locator('.sidebar').waitFor({ state: 'visible', timeout: 20000 });
  await page.locator('section#system, fieldset#system').first()
    .waitFor({ state: 'attached', timeout: 20000 });
  const shape = await page.evaluate(() => ({
    nodes: document.querySelectorAll('section[id], fieldset[id]').length,
    href: location.pathname,
    height: Math.round(document.documentElement.scrollHeight),
  }));
  check(shape.nodes >= 5 && shape.height > 1500,
        `配置表单渲染出来了（${shape.nodes} 个带 id 的段落，整页高 ${shape.height}px）`,
        `没有登录、路由守卫直接放行 —— 放行不了说明桩里的 /api/session 形状变了`);

  // ==================================================================
  // [2] 内容滚动时侧栏和顶栏**不跟着走**
  //
  // 与 `shell_probe.mjs` 的 `[2b]` 是同一组断言、同一段源码，故意逐字一样。
  //
  // 这一条盯的是用户实测报的那句话：「点菜单是滚动下拉，菜单自己也跟着滚」。
  // `shell_probe.mjs` 的 `[2]` 那几条判的是"目标段落有没有滚进来"，它们**在这一条
  // 红的时候照样全绿** —— 段落确实滚进来了，只是连侧栏一起滚出去了。
  //
  // 版面是靠 `position: sticky` 做的（`.sidebar` / `.topbar`），而 sticky 有一个
  // 出了名的失效方式：**祖先里只要有一个变成了滚动容器，sticky 就改成相对那个
  // 容器粘**。`html, body { overflow-x: hidden }` 正好是这么干的 —— 从 `html`
  // 那句会传播到视口（无害），而 `body` 那句不会被传播，于是 body 变成一个
  // 「高度等于内容、永远不滚」的滚动容器，侧栏粘在它上面，视觉上就等于没粘。
  // 所以这里量的是 `getBoundingClientRect().top`，不是 class、不是宽度。
  // ==================================================================
  console.log('\n[2] 侧栏/顶栏是否跟着内容滚');
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
  const settledY = await stableScrollY(page);
  const pin = await page.evaluate(() => {
    const sb = document.querySelector('.sidebar');
    const tb = document.querySelector('.topbar');
    const max = document.documentElement.scrollHeight - window.innerHeight;
    const ov = (el) => {
      if (!el) return null;
      const cs = getComputedStyle(el);
      return `${cs.overflowX}/${cs.overflowY}`;
    };
    // sticky 的**最近滚动祖先**：顺着 parentElement 往上，第一个 computed
    // overflow 不是 `visible` 的元素。它才是 `position: sticky` 真正去粘的那一层
    // —— 非根元素上任何非 `visible` 的 overflow 都会让它变成一个真正的滚动容器。
    // 走到 documentElement 为止（html 的 overflow 会传播到视口，那就是"视口"）。
    const scrollport = (el) => {
      for (let a = el && el.parentElement; a && a !== document.documentElement; a = a.parentElement) {
        const cs = getComputedStyle(a);
        if (cs.overflowX !== 'visible' || cs.overflowY !== 'visible') {
          const cls = typeof a.className === 'string' && a.className ? `.${a.className.split(' ')[0]}` : '';
          return `${a.tagName.toLowerCase()}${cls}(${cs.overflowX}/${cs.overflowY})`;
        }
      }
      return '视口';
    };
    return {
      y: Math.round(window.scrollY),
      max: Math.round(max),
      sideTop: sb ? Math.round(sb.getBoundingClientRect().top) : null,
      barTop: tb ? Math.round(tb.getBoundingClientRect().top) : null,
      sidePort: scrollport(sb),
      barPort: scrollport(tb),
      htmlOverflow: ov(document.documentElement),
      bodyOverflow: ov(document.body),
    };
  });
  // 这一行不是断言，是**诊断** —— 红的时候它要能一眼指出**是哪一层**让 sticky
  // 失效的，而不是留给人从头查一遍。
  //
  // ⚠️ 原先这里打的是 `document.scrollingElement`，并配了一句"sticky 失效时它
  // 会是 `body`、正常时是 `html`"。**那句话是错的，而且被它自己第一次的读数当场
  // 证伪**：`scrollingElement` 在 standards 模式下**恒为 `documentElement`**，
  // 谁在滚都一样 —— 它答不了"谁在滚"，所以它不是诊断。红的那一次它打的是
  // `scrollingElement=html`，与正常时一字不差，等于什么都没说。
  // 真正的答案是「sticky 的最近滚动祖先」：非根元素上任何非 `visible` 的
  // overflow 都会让它变成一个**真正的滚动容器**，sticky 于是改成相对它粘。
  // 实测（`out\layout-aux-check.mjs`，在浏览器里逐个改这一个变量）：
  //   body 留 hidden  → 侧栏 top=-4698（没粘），最近滚动祖先 = `body(hidden/auto)`
  //   body 改 visible → 侧栏 top=0（粘住），最近滚动祖先 = 视口
  //   只改 html、body 不动 → 仍然是 -4698（**改 html 没用，问题全在 body**）
  console.log(`  [note] 侧栏最近滚动祖先=${pin.sidePort} 顶栏=${pin.barPort}`
            + ` html overflow=${pin.htmlOverflow} body overflow=${pin.bodyOverflow}`);
  // 前提：这一页真的滚得动。滚不动的话下面两条**恒为真** ——
  // 那就是陷阱 60 的形状（"它应该不存在"在它本来就不存在的机器上恒真）。
  check(settledY > 200 && pin.y > 200,
        `前提：配置页真的滚下去了（scrollY=${pin.y} / 最多 ${pin.max}）`);
  check(pin.sideTop !== null && Math.abs(pin.sideTop) <= 2,
        `滚到底之后侧栏仍贴在视口顶部（top=${pin.sideTop}，scrollY=${pin.y}）`);
  check(pin.barTop !== null && Math.abs(pin.barTop) <= 2,
        `滚到底之后顶栏仍贴在视口顶部（top=${pin.barTop}）`);

  await page.screenshot({ path: path.join(outDir, 'layout_scrolled.png') });
  await page.evaluate(() => window.scrollTo(0, 0));
  await stableScrollY(page);

  check(pageErrors.length === 0, '没有未捕获的页面异常',
        pageErrors.slice(0, 3).join('\n         '));

  exitCode = bad ? 1 : 0;
} catch (e) {
  console.error(`\n[probe] 探针自己出错：${e && e.stack ? e.stack : e}`);
  exitCode = 2;
} finally {
  if (browser && !A.keep) await browser.close().catch(() => {});
  if (server && !A.keep) server.close();
}

console.log(`\n${'='.repeat(60)}\n通过 ${ok} 项，失败 ${bad} 项。`);
process.exit(exitCode);
