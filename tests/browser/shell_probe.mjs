// 外壳（左侧栏 / 锚点 / 抽屉）与"按需快照"的真实浏览器验证。
//
// 它为什么非要有，而不是看代码推断：这一批改动**全部只在浏览器里成立**。
//
//   - 折叠状态存的是 `localStorage`，而"存进去了"和"读回来了"是两件事；
//   - 锚点跳转要的是"元素进了视口且没被 sticky 顶栏盖住" —— 这是布局，
//     `scrollIntoView` 调没调过与它无关；
//   - ≤720px 的抽屉**完全由 CSS 媒体查询**决定，JS 里连视口宽度都没存
//     （见 App.vue 里那段注释），所以"汉堡能开合"只能量；
//   - 快照那条的判据干脆就是**网络请求计数**：进页面零次、点了才一次。
//   - 设备信息页 [5] 的分辨率那一格走的是 `/ws/stats` → `normalizeStats()` →
//     组件这条**四段白名单**链路（后端少发一个字段、或前端三处白名单漏一处，
//     在页面上都只表现为一个"未知"），而"复制按钮真的接上了"也只有真的点一下
//     才成立 —— 剪贴板在 headless 里默认没有权限，所以那条要显式授权，否则量到
//     的是浏览器策略而不是我们的接线。
//
// 这几条在 curl / Python 探针里一个都看不见。
//
// 用法:
//   node tests/browser/shell_probe.mjs [--port 8131] [--rtsp 8132]
//                                      [--user admin] [--password admin]
//                                      [--headed] [--keep]

import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

function args() {
  const a = process.argv.slice(2);
  const o = { port: 8131, rtsp: 8132, headless: true, keep: false,
              user: 'admin', password: 'admin' };
  for (let i = 0; i < a.length; i++) {
    if (a[i] === '--port') o.port = +a[++i];
    else if (a[i] === '--rtsp') o.rtsp = +a[++i];
    else if (a[i] === '--user') o.user = a[++i];
    else if (a[i] === '--password') o.password = a[++i];
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

// ---------------------------------------------------------------- 后端
function startBackend() {
  const exe = path.join(ROOT, 'build', 'bin', 'desk2onvif.exe');
  if (!fs.existsSync(exe)) throw new Error(`没有 ${exe}，先跑 scripts\\build.ps1`);
  // 临时配置：绝不动用户 %APPDATA% 里那份，也不动仓库里可能存在的 config.json。
  const cfg = path.join(outDir, 'shell-probe.json');
  fs.rmSync(cfg, { force: true });
  const logFd = fs.openSync(path.join(outDir, 'shell-backend.log'), 'w');
  return spawn(exe, [
    '--rtsp', '--rtsp-port', String(A.rtsp),
    '--http', '--http-port', String(A.port),
    '--web-root', path.join(ROOT, 'web', 'dist'),
    '--http-user', A.user, '--http-password', A.password,
    '--no-onvif',            // 本探针不碰 ONVIF，关掉省掉 3702 的干扰
    '--config', cfg,
  ], { cwd: ROOT, stdio: ['ignore', logFd, logFd] });
}

function killTree(p) {
  if (!p || p.exitCode !== null) return;
  try { spawn('taskkill', ['/F', '/T', '/PID', String(p.pid)], { stdio: 'ignore' }); } catch {}
}

async function waitHttp(timeoutMs = 40000) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    try {
      // /api/session 匿名可达，所以它答的是"后端起来了没"。
      const r = await fetch(`${BASE}/api/session`);
      if (r.ok) return true;
    } catch {}
    await sleep(300);
  }
  return false;
}

async function login() {
  const r = await fetch(`${BASE}/api/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ user: A.user, password: A.password }),
  });
  if (!r.ok) throw new Error(`POST /api/login → ${r.status}`);
  const list = typeof r.headers.getSetCookie === 'function'
    ? r.headers.getSetCookie()
    : [r.headers.get('set-cookie')].filter(Boolean);
  for (const v of list) {
    if (v && v.startsWith('d2o_session=')) return v.split(';', 1)[0].split('=', 2)[1];
  }
  throw new Error('登录返回 200 但没有 d2o_session cookie');
}

// ---------------------------------------------------------------- 小工具

/** 菜单项按**显示文字**定位 —— 用户就是靠这个认的。 */
function menuLink(page, label) {
  return page.locator('.side-link', { hasText: label });
}

async function collapsed(page) {
  return page.locator('.sidebar').evaluate((el) => el.classList.contains('is-collapsed'));
}

/**
 * 目标段落"进了视口、而且没被顶栏盖住"。
 *
 * 只判 `top >= 0 && top < innerHeight` 不够：`scroll-margin-top: 72px` 是给
 * sticky 顶栏留的，它要是没生效，段落的标题就会正好压在顶栏底下 —— 而那时
 * 上面那个判据**照样绿**。所以拿顶栏的实际高度当门槛。
 *
 * 还要处理一种正常情况：目标段落底下剩的内容比一屏还少时，浏览器滚不下去，
 * 段落就只能停在视口中间偏下。这时门槛自动退化成"在视口里"。
 */
async function anchorInView(page, id) {
  return page.evaluate((anchorId) => {
    const el = document.getElementById(anchorId);
    if (!el) return { found: false };
    const bar = document.querySelector('.topbar');
    const barH = bar ? bar.getBoundingClientRect().height : 0;
    const r = el.getBoundingClientRect();
    const vh = window.innerHeight;
    return {
      found: true, top: Math.round(r.top), barH: Math.round(barH), vh,
      inView: r.top >= 0 && r.top < vh && r.bottom > 0,
      notCovered: r.top >= barH - 1,
    };
  }, id);
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
let backend = null, browser = null, ctx = null;
let exitCode = 1;

try {
  backend = startBackend();
  if (!await waitHttp()) throw new Error('后端 40 秒内没起来（看 out/browser/shell-backend.log）');
  const cookie = await login();
  console.log(`[probe] 后端已就绪 ${BASE}（登录已开启，凭据 ${A.user}/***）`);

  browser = await chromium.launch({ channel: 'chrome', headless: A.headless });
  ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  await ctx.addCookies([{
    name: 'd2o_session', value: cookie, url: `${BASE}/`,
    httpOnly: true, sameSite: 'Lax',
  }]);
  const page = await ctx.newPage();
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));

  // ==================================================================
  // [1] 侧栏：菜单表、折叠、折叠状态的持久化
  // ==================================================================
  console.log('\n[1] 左侧栏与折叠');
  await page.goto(`${BASE}/`, { waitUntil: 'domcontentloaded' });
  await page.locator('.sidebar').waitFor({ state: 'visible', timeout: 20000 });

  const labels = await page.locator('.side-link .side-label').allInnerTexts();
  // 这个表是"侧栏里有过什么"的**最低**覆盖，不是全集：`App.vue` 的菜单比它长
  // （还有水印 OSD / 音视频 / 自适应降载 / 时间设置）。每加一个阶段就把它加进来，
  // 但别反过来按它去断言"菜单恰好是这七项" —— 那样每加一项都要改断言，而改到最后
  // 这条断言就只剩"我抄了一遍 App.vue"这个作用。
  const want = ['预览', '实时状态', '图像设置', 'ONVIF 设置', '系统设置', '设备信息', '操作日志'];
  check(want.every((w) => labels.map((s) => s.trim()).includes(w)),
        `菜单里有这七项：${want.join(' / ')}`, `实得 ${JSON.stringify(labels)}`);

  // 每一项都必须指向**真的存在**的地方 —— 一个点下去没反应（或者 404）的菜单项
  // 比没有这一项更糟，而这正是本阶段反复强调的那条纪律。
  const hrefs = await page.locator('.side-link').evaluateAll(
    (els) => els.map((e) => e.getAttribute('href')));
  check(hrefs.every((h) => h && h.startsWith('/')),
        `菜单项的 href 都是站内地址`, JSON.stringify(hrefs));
  // 这两条**随着阶段推进改过两次**：Phase O 把 `/info` 做出来，Phase V 把 `/logs`
  // 做出来，于是两者现在**都该**在菜单里。原文写的是"/logs 不该在（它属于 Phase V，
  // 还没做）"—— 那是当时的事实，而 Phase V 交付的那一刻它就该翻过来。
  // ⚠️ 这一条值得记一笔：**否定式断言会随着被否定的东西出现而变成谎话，而它自己
  // 不会报错**（它只是从"验到了"变成"恒真"再到"恒假"）。原文里那句注释预见到了
  // 这一刻，但没有任何机制提醒人回来改 —— 靠的是实现这个阶段的人自己想起来。
  check(hrefs.includes('/info'), '菜单里有 /info（Phase O 已交付）', JSON.stringify(hrefs));
  check(hrefs.includes('/logs'), '菜单里有 /logs（Phase V 已交付）', JSON.stringify(hrefs));

  const groupTitles = (await page.locator('.side-group-title').allInnerTexts()).map((s) => s.trim());
  check(groupTitles.length === 3
          && groupTitles.includes('监控') && groupTitles.includes('配置') && groupTitles.includes('设备'),
        `分组标题是「监控」「配置」「设备」三个：${JSON.stringify(groupTitles)}`);
  // 「设备」组是**跟着第一个条目一起**加进来的：空分组标题读起来像界面坏了。
  // 所以这里同时判"组在"和"组里有东西"。
  const deviceGroup = page.locator('.side-group', { has: page.locator('.side-group-title', { hasText: '设备' }) });
  check(await deviceGroup.locator('.side-link').count() >= 1,
        '「设备」组里有条目（不是只有一个空标题）');

  check(await collapsed(page) === false, '默认是展开的');
  await page.locator('.side-toggle').click();
  await sleep(400);
  check(await collapsed(page) === true, '点折叠按钮之后收起了');

  const stored = await page.evaluate(() => {
    try { return window.localStorage.getItem('d2o.sidebar.collapsed'); } catch { return '<异常>'; }
  });
  check(stored === '1', `折叠状态写进了 localStorage（实得 ${JSON.stringify(stored)}）`);

  await page.reload({ waitUntil: 'domcontentloaded' });
  await page.locator('.sidebar').waitFor({ state: 'visible', timeout: 20000 });
  await sleep(300);
  check(await collapsed(page) === true, '刷新之后仍然是折叠的（localStorage 存的被读回来了）');

  // 宽度真的是 56px 那条窄栏 —— 只判 class 的话，CSS 被改坏了照样绿。
  const narrowW = await page.locator('.sidebar').evaluate((el) => Math.round(el.getBoundingClientRect().width));
  check(narrowW >= 40 && narrowW <= 80, `折叠后侧栏真的变窄了（${narrowW}px）`);

  await page.locator('.side-toggle').click();
  await sleep(400);
  check(await collapsed(page) === false, '再点一次展开，并写回 localStorage（为 [2] 留下展开状态）');

  // 侧栏底部的版本号：数据来自 /ws/stats，所以这里要等它连上。
  //
  // 它同时是 Phase M 那条链路的**唯一一处前端出口** —— `stats.version` 从后端
  // 经 `emptyStats()` / `normalizeStats()` / `types.ts` 三处白名单才到这里，
  // 漏一处的症状就是这个角落永远空着。
  let version = '';
  for (let i = 0; i < 25 && !version; i++) {
    version = (await page.locator('.side-foot').count())
      ? (await page.locator('.side-foot').innerText()).trim() : '';
    if (!version) await sleep(200);
  }
  check(/^v\d+\.\d+\.\d+$/.test(version),
        `侧栏底部显示了版本号（实得 ${JSON.stringify(version)}）`);

  // ==================================================================
  // [2] 锚点：点菜单 → URL 带锚点、目标段落滚进视口、高亮唯一
  // ==================================================================
  console.log('\n[2] 锚点跳转');
  await page.goto(`${BASE}/config`, { waitUntil: 'domcontentloaded' });
  await page.locator('form').first().waitFor({ state: 'attached', timeout: 20000 });
  await sleep(600);

  // 先滚到页面底部：这样下面那两条"滚过去了"才有信息量。
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
  await sleep(300);

  await menuLink(page, '系统设置').click();
  // 配置页的表单是异步加载的，而锚点要靠 load() 之后的 nextTick 再滚一次，
  // 所以这里必须等 —— 不等就是在量一个还没发生的事。
  await sleep(1200);
  check(page.url().endsWith('/config#system'),
        `URL 带上了 #system（实得 ${page.url()}）`);
  const sys = await anchorInView(page, 'system');
  check(sys.found && sys.inView && sys.notCovered,
        `#system 滚进了视口、且没被顶栏盖住（top=${sys.top} 顶栏高=${sys.barH} 视口=${sys.vh}）`);

  // **已经在 /config 上再点另一个子项**：path 没变、组件不重挂，
  // `scrollBehavior` 到底会不会被调用取决于 vue-router 的版本行为。
  // 这一条就是专门盯着 ConfigView 里那个 `watch(() => route.hash)` 的。
  await menuLink(page, '图像设置').click();
  await sleep(1200);
  check(page.url().endsWith('/config#video'),
        `换一项后 URL 变成 #video（实得 ${page.url()}）`);
  const vid = await anchorInView(page, 'video');
  check(vid.found && vid.inView && vid.notCovered,
        `#video 也跟着滚过去了（top=${vid.top}）`);

  // 高亮唯一：三个子项指向同一个 path，容易一起亮。
  await menuLink(page, 'ONVIF 设置').click();
  await sleep(1200);
  const active = await page.locator('.side-link.is-active').evaluateAll(
    (els) => els.map((e) => (e.textContent || '').trim()));
  check(active.length === 1 && active[0] === 'ONVIF 设置',
        `高亮的菜单项有且只有一项，且是「ONVIF 设置」：${JSON.stringify(active)}`);
  const onv = await anchorInView(page, 'onvif');
  check(onv.found && onv.inView && onv.notCovered, `#onvif 进了视口（top=${onv.top}）`);

  // 三个锚点元素都得真的在 DOM 里 —— 上面那三条在"元素不存在"时都会红，
  // 但红的理由（找不到 id）和"没滚过去"是两回事，分开说清楚。
  const anchors = await page.evaluate(() =>
    ['video', 'system', 'onvif'].filter((i) => document.getElementById(i)));
  check(anchors.length === 3, `三个锚点元素都在（${JSON.stringify(anchors)}）`);

  // ==================================================================
  // [2b] 内容滚动时侧栏和顶栏**不跟着走**
  //
  // 这一条盯的是用户实测报的那句话：「点菜单是滚动下拉，菜单自己也跟着滚」。
  // 上面 [2] 那几条判的是"目标段落有没有滚进来"，它们**在这一条红的时候照样全绿**
  // —— 段落确实滚进来了，只是连侧栏一起滚出去了。
  //
  // 版面是靠 `position: sticky` 做的（`.sidebar` / `.topbar`），而 sticky 有一个
  // 出了名的失效方式：**祖先里只要有一个变成了滚动容器，sticky 就改成相对那个
  // 容器粘**。`html, body { overflow-x: hidden }` 正好是这么干的 —— 从 `html`
  // 那句会传播到视口（无害），而 `body` 那句不会被传播，于是 body 变成一个
  // 「高度等于内容、永远不滚」的滚动容器，侧栏粘在它上面，视觉上就等于没粘。
  // 所以这里量的是 `getBoundingClientRect().top`，不是 class、不是宽度。
  // ==================================================================
  console.log('\n[2b] 侧栏/顶栏是否跟着内容滚');
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
  const settledY = await stableScrollY(page);
  const pin = await page.evaluate(() => {
    const sb = document.querySelector('.sidebar');
    const tb = document.querySelector('.topbar');
    const max = document.documentElement.scrollHeight - window.innerHeight;
    // sticky 的**最近滚动祖先**：顺着 parentElement 往上，第一个 computed
    // overflow 不是 `visible` 的元素。它才是 `position: sticky` 真正去粘的那一层。
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
    };
  });
  // 这一行不是断言，是**诊断** —— 红的时候它要能一眼指出是哪一层让 sticky 失效的。
  // 与 `layout_probe.mjs` 的 `[2]` 段逐字一样（那一段有完整的推理与被推翻的旧说法）。
  // ⚠️ 原先这里打的是 `document.scrollingElement`，配了一句"失效时它是 `body`、正常时
  // 是 `html`"。**那句话是错的**：`scrollingElement` 在 standards 模式下恒为
  // `documentElement`，谁在滚都一样 —— 它答不了"谁在滚"，所以它不是诊断。
  // 实测（`out\layout-aux-check.mjs` 的 D/E 两行，都是 `[没粘] top=-4698`）：
  //   body 留 hidden  → 最近滚动祖先 = `body(hidden/auto)`
  //   body 改 visible → 最近滚动祖先 = 视口
  console.log(`  [note] 侧栏最近滚动祖先=${pin.sidePort} 顶栏=${pin.barPort}`);
  // 前提：这一页真的滚得动。滚不动的话下面两条**恒为真** ——
  // 那就是陷阱 60 的形状（"它应该不存在"在它本来就不存在的机器上恒真）。
  check(settledY > 200 && pin.y > 200,
        `前提：配置页真的滚下去了（scrollY=${pin.y} / 最多 ${pin.max}）`);
  check(pin.sideTop !== null && Math.abs(pin.sideTop) <= 2,
        `滚到底之后侧栏仍贴在视口顶部（top=${pin.sideTop}，scrollY=${pin.y}）`);
  check(pin.barTop !== null && Math.abs(pin.barTop) <= 2,
        `滚到底之后顶栏仍贴在视口顶部（top=${pin.barTop}）`);
  // 滚回顶部，免得影响后面的段落（[3] 会换视口尺寸重来）。
  await page.evaluate(() => window.scrollTo(0, 0));
  await stableScrollY(page);

  // ==================================================================
  // [3] ≤720px：侧栏变成覆盖式抽屉，汉堡开合
  // ==================================================================
  console.log('\n[3] 窄屏抽屉');
  await page.setViewportSize({ width: 640, height: 820 });
  await sleep(500);

  const hamVisible = await page.locator('.hamburger').isVisible();
  check(hamVisible, '≤720px 时汉堡按钮出现了');

  // 抽屉默认必须是收起的：`display:none` 的遮罩 + `translateX(-100%)` 的侧栏。
  // 判 rect 而不是判 class —— class 在、CSS 被改坏了照样全绿。
  const shut = await page.locator('.sidebar').evaluate((el) => {
    const r = el.getBoundingClientRect();
    return { right: Math.round(r.right), w: Math.round(r.width) };
  });
  check(shut.right <= 1, `抽屉默认在屏幕外（right=${shut.right}，宽 ${shut.w}）`);
  check(await page.locator('.side-scrim').count() === 0,
        '没收起的时候遮罩不在 DOM 里（它是 v-if，不是 v-show）');

  await page.locator('.hamburger').click();
  await sleep(500);
  const open = await page.locator('.sidebar').evaluate((el) => {
    const r = el.getBoundingClientRect();
    return { left: Math.round(r.left), right: Math.round(r.right), w: Math.round(r.width) };
  });
  check(open.left >= -1 && open.right > 100,
        `点汉堡之后抽屉拉进来了（left=${open.left} right=${open.right}）`);
  check(await page.locator('.side-scrim').isVisible(), '遮罩跟着出现');

  await page.locator('.side-scrim').click({ position: { x: 600, y: 400 } });
  await sleep(500);
  const shut2 = await page.locator('.sidebar').evaluate((el) => Math.round(el.getBoundingClientRect().right));
  check(shut2 <= 1, `点遮罩把抽屉收回去了（right=${shut2}）`);

  // 抽屉里点一个菜单项也要顺手收起 —— 不收起的话用户会看见"页面变了、
  // 菜单还盖在上面"。
  await page.locator('.hamburger').click();
  await sleep(500);
  await menuLink(page, '实时状态').click();
  await sleep(700);
  const shut3 = await page.locator('.sidebar').evaluate((el) => Math.round(el.getBoundingClientRect().right));
  check(shut3 <= 1, `在抽屉里点菜单项之后它自己收起了（right=${shut3}）`);
  check(page.url().endsWith('/status'), `并且真的换了页（${page.url()}）`);
  await page.setViewportSize({ width: 1280, height: 900 });
  await sleep(300);

  // ==================================================================
  // [4] 预览页的快照：默认不抓、点了才抓
  // ==================================================================
  //
  // 判据就是请求计数。旧版那张常驻 `<img v-show>` + 每秒 interval 的话，
  // 下面"等 3.5 秒"这一段会数出 3 次 —— 而它在界面上**看不出来**：
  // 画面照样在那儿，只是服务端每秒被要一张 2880x1920 的 JPEG。
  console.log('\n[4] 快照按需抓拍');
  const preview = await ctx.newPage();
  const previewErrors = [];
  preview.on('pageerror', (e) => previewErrors.push(String(e)));
  const snaps = [];
  preview.on('request', (r) => {
    if (/snapshot/i.test(r.url())) snaps.push(r.url());
  });

  await preview.goto(`${BASE}/`, { waitUntil: 'domcontentloaded' });
  await preview.locator('.card', { hasText: '快照' }).first().waitFor({ timeout: 20000 });
  await sleep(3500);

  check(snaps.length === 0,
        `进页面 3.5 秒内一次都没要快照（实得 ${snaps.length} 次）`,
        snaps.slice(0, 3).join('\n         '));
  check(await preview.locator('img.snapshot').count() === 0,
        '没有 <img class="snapshot"> 留在 DOM 里');
  const ph = preview.locator('.card', { hasText: '快照' }).first().locator('.placeholder');
  check(await ph.isVisible() && /还没有抓拍/.test(await ph.innerText()),
        '占位块显示「还没有抓拍」');

  const snapBtn = preview.locator('.card', { hasText: '快照' }).first().locator('button');
  check(await snapBtn.count() === 1, '快照卡片里有一个按钮');
  await snapBtn.click();

  for (let i = 0; i < 60 && snaps.length === 0; i++) await sleep(100);
  check(snaps.length === 1,
        `点一次「抓拍」正好发一次请求（实得 ${snaps.length} 次）`,
        snaps.join('\n         '));
  // **必须是 /api/snapshot.jpg**：`/snapshot.jpg` 那条是 ONVIF 的
  // GetSnapshotUri 路径，回 401 + Digest 挑战，浏览器会弹一个**系统级**的
  // 口令框 —— 而那个框和我们的登录页是两套东西，用户在里面输什么都不对。
  check(snaps[0] && snaps[0].includes('/api/snapshot.jpg'),
        `请求的是 /api/snapshot.jpg（${snaps[0]}）`);

  const img = preview.locator('img.snapshot');
  await img.waitFor({ state: 'visible', timeout: 15000 }).catch(() => {});
  const shot = await preview.evaluate(() => {
    const el = document.querySelector('img.snapshot');
    if (!el) return null;
    return { w: el.naturalWidth, h: el.naturalHeight, complete: el.complete,
             src: (el.getAttribute('src') || '').slice(0, 40) };
  });
  check(shot && shot.w > 0 && shot.h > 0,
        `画面真的解码出来了（${shot ? `${shot.w}x${shot.h}` : '没有 <img>'}）`,
        shot ? '' : '图片没出来 —— 快照失败时占位块会换成「快照不可用」');

  // 再点一次：换个 nonce、重新发一次，而不是拿浏览器缓存糊弄过去。
  if (shot) {
    const before = snaps.length;
    await snapBtn.click();
    for (let i = 0; i < 60 && snaps.length === before; i++) await sleep(100);
    check(snaps.length === before + 1, `再点一次又发了一次（${before} → ${snaps.length}）`);
    // 比的是**最后两次**，不是 `snaps[before]` —— `before` 是点击前的**条数**，
    // 新增的那条正好落在下标 `before` 上，拿它和"最后一条"比就是自己和自己比。
    check(snaps[snaps.length - 1] !== snaps[snaps.length - 2],
          '两次的 URL 不一样（带上了新的时间戳，绕开浏览器缓存）');
  }

  // ==================================================================
  // [5] 设备信息页（只读 /info）
  // ==================================================================
  //
  // 这一段的**核心**是"主码流分辨率"那一格。它走的是这条链路：
  //   stats_json() → /ws/stats → normalizeStats() 的白名单 → 组件
  // 四段里任何一段漏了 `video_width`/`video_height`，页面上只表现为一个"未知"
  // —— 而"未知"这两个字看起来完全正常（后端刚起来、还没采集，也会是未知）。
  // 所以判据是**拿它跟 /api/status 的同一个字段对账**，而不是"它不是未知"。
  console.log('\n[5] 设备信息页');

  // 剪贴板在 headless 下默认没有权限。不显式授权的话，量到的是浏览器策略
  // （复制失败），而不是我们的接线 —— 那样这条断言就变成在测 Chrome。
  await ctx.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: BASE });

  await menuLink(page, '设备信息').click();
  await page.locator('.page-head h1').waitFor({ timeout: 20000 });
  await sleep(600);
  check(page.url().endsWith('/info'), `点「设备信息」进了 /info（实得 ${page.url()}）`);

  const h1 = (await page.locator('.page-head h1').innerText()).trim();
  check(h1 === '设备信息', `页面标题是「设备信息」（实得 ${JSON.stringify(h1)}）`);

  const activeLabel = (await page.locator('.side-link.is-active .side-label').allInnerTexts())
    .map((s) => s.trim());
  check(activeLabel.length === 1 && activeLabel[0] === '设备信息',
        `侧栏只高亮「设备信息」一项（实得 ${JSON.stringify(activeLabel)}）`);

  // 只读：这一页上不该有任何会改状态的东西。
  const pageBtns = (await page.locator('.page-head button, .card button').allInnerTexts())
    .map((s) => s.trim());
  check(!pageBtns.some((s) => /保存|应用|删除|重置/.test(s)),
        `页面上没有会改状态的按钮（实得 ${JSON.stringify(pageBtns)}）`);

  // ---- 逐行读出来 -----------------------------------------------------
  //
  // 行的形状是 `<span class="hint">标签</span><code class="value">值</code>`，
  // 后面可能再跟一个说明用的 `.hint`，所以标签取**第一个** `.hint`。
  //
  // ⚠️ 值有两种载体，**必须都认**：锁死的那几行（硬件 ID / 序列号 / 设备 UUID，
  // 见 InfoView 的 `identityRows`）是 `<input class="value" disabled>`，其余是
  // `<code class="value">`。输入框的**值不在 `textContent` 里** —— 它是个空元素，
  // 值挂在 `value` 属性上。只读 `code.value` 的写法在这几行上量到的永远是空串，
  // 而失败信息会写成「序列号有值（实得 ""）」，指向"后端没给序列号"这个错误的
  // 方向。2026-10-09 实测：本探针就是被这个坑红的，后端 `/api/config` 一直有值
  // （同一次回归里 `web_contract.py` 对同一个字段的非空断言是绿的）。
  //
  // `input` 在的时候就用它的 `value`（哪怕是空串），**不要**回落到 `code` ——
  // 那会把"输入框是空的"伪装成"值在 code 里"。
  async function readRows() {
    return page.locator('.readonly-row').evaluateAll((els) => els.map((e) => {
      const inp = e.querySelector('input.value');
      const val = inp ? inp.value : (e.querySelector('code.value')?.textContent || '');
      return {
        label: (e.querySelector('.hint')?.textContent || '').trim(),
        value: val.trim(),
      };
    }));
  }
  // 取不到就返回 undefined（而不是空串）—— "没有这一行"和"这一行是空的"
  // 是两件事，混在一起之后失败信息会指向错误的方向。
  const rowOf = async (name) => (await readRows()).find((r) => r.label === name);

  const allRows = await readRows();
  // 反空洞守卫：下面的 `rowOf` 找不到就返回 undefined，而 `!!serial` 这类判据
  // 在**一行都没渲染**的时候同样会红 —— 但那时红的理由是错的。所以先确认
  // 真的渲染出行来了。
  check(allRows.length >= 8, `页面上渲染出了足够多的只读行（实得 ${allRows.length} 行）`);

  // 序列号：`shell_probe` 是带 `--no-onvif` 起的，但 `cmd_run` 里那段身份推导
  // （`need_serial` → `make_serial(seed)` → 写回 `cfg.onvif.serial_number` → 存盘）
  // 的门是 **HTTP 服务开着**，不是 ONVIF 开着 —— 而 `config_get` 序列化的就是
  // 那个 cfg，所以这一行非空，和 ONVIF 开没开无关。
  const serial = await rowOf('序列号');
  check(!!serial && serial.value !== '' && serial.value !== '—',
        `序列号有值（实得 ${JSON.stringify(serial && serial.value)}）`);

  const st = await (await fetch(`${BASE}/api/status`, { headers: { Cookie: `d2o_session=${cookie}` } })).json();

  const ver = await rowOf('软件版本');
  check(!!ver && ver.value === st.version,
        `软件版本与 /api/status 一致（页面 ${JSON.stringify(ver && ver.value)} / 接口 ${JSON.stringify(st.version)}）`);

  // **本阶段的核心断言。** 见这一段开头的说明。
  const wantRes = st.video_width > 0 ? `${st.video_width} × ${st.video_height}` : '';
  let mainRes = '';
  for (let i = 0; i < 50 && mainRes !== wantRes; i++) {
    mainRes = ((await rowOf('主码流分辨率')) || {}).value || '';
    if (mainRes !== wantRes) await sleep(200);
  }
  check(mainRes === wantRes && wantRes !== '',
        `主码流分辨率显示的是实际编码尺寸（页面 ${JSON.stringify(mainRes)} / 接口 ${JSON.stringify(wantRes)}）`,
        mainRes === wantRes ? '' : 'video_width/video_height 没走到页面上：后端的 stats_json()、'
          + '前端 useStats.ts 的 emptyStats()/normalizeStats()、types.ts 三处白名单任缺一处都长这样');

  // ---- 地址类 ----------------------------------------------------------
  const addr = await rowOf('地址');
  check(!!addr && new RegExp(`^http://[^/]+:${A.port}/onvif/device_service$`).test(addr.value),
        `ONVIF 服务地址拼对了（实得 ${JSON.stringify(addr && addr.value)}，期望 http://<host>:${A.port}/onvif/device_service）`);

  const mainRtsp = await rowOf('RTSP 主码流');
  check(!!mainRtsp && mainRtsp.value === st.rtsp_url && mainRtsp.value.includes(`:${A.rtsp}/`),
        `RTSP 主码流用的是后端报的那个地址（页面 ${JSON.stringify(mainRtsp && mainRtsp.value)} / 接口 ${JSON.stringify(st.rtsp_url)}）`);

  // 「设备地址」那一格是**从 RTSP 地址里拆出来的主机名**，所以两者必须同源。
  const host = await rowOf('设备地址');
  check(!!host && !!mainRtsp && host.value === (/^rtsp:\/\/([^/:]+)/.exec(mainRtsp.value) || [])[1],
        `设备地址摘自 RTSP 地址（实得 ${JSON.stringify(host && host.value)}）`);

  // ---- 复制按钮真的接上了 ----------------------------------------------
  //
  // 剪贴板写了什么浏览器不让我们读回来（`clipboard-read` 在 headless 下也不总成），
  // 所以判的是**点击之后的可见结论**。而"只有这一行显示已复制"这一条，正是
  // InfoView 用**每行一份**状态（而不是一个全局状态字）换来的 —— 全局状态字
  // 会让每一行同时亮起来（StatusView 早先就是这么写的，见那里的注释）。
  const nvrCard = page.locator('.card', { hasText: '加进 NVR' }).first();
  const addrRow = nvrCard.locator('.readonly-row').first();
  const copyBtn = addrRow.locator('button');
  check(await copyBtn.count() === 1, '「地址」那一行有复制按钮');
  await copyBtn.click();
  let copiedHint = '';
  for (let i = 0; i < 20 && !copiedHint; i++) {
    copiedHint = (await addrRow.locator('.hint').allInnerTexts()).map((s) => s.trim())
      .filter((s) => /已复制|复制失败/.test(s)).join('');
    if (!copiedHint) await sleep(150);
  }
  check(/已复制/.test(copiedHint),
        `点复制之后这一行给出了结论（实得 ${JSON.stringify(copiedHint)}）`,
        /复制失败/.test(copiedHint)
          ? '剪贴板写入被浏览器拒了 —— 先看 ctx.grantPermissions 那一行还在不在'
          : '点了之后什么都没出现：onCopy 没接上');
  check(await page.locator('.hint.tone-good').count() === 1,
        `只有点过的那一行显示「已复制」（实得 ${await page.locator('.hint.tone-good').count()} 行）`,
        '多于一行 = 状态是全局的而不是每行一份');

  // ---- 刷新/收藏夹那条路 -------------------------------------------------
  //
  // 从菜单点进去和直接打开 `/info` 是**两条**路：后者要经过 SPA 回落 + 路由
  // 懒加载，而菜单那条在同一个 SPA 里不会重新拉 chunk。少验一条，坏的那条
  // 就只会在用户收藏了地址之后才暴露。
  await page.goto(`${BASE}/info`, { waitUntil: 'domcontentloaded' });
  await page.locator('.page-head h1').waitFor({ timeout: 20000 });
  check((await page.locator('.page-head h1').innerText()).trim() === '设备信息',
        '直接打开 /info 也能渲染（刷新/收藏夹那条路）');

  await page.screenshot({ path: path.join(outDir, 'shell_info.png'), fullPage: true });

  await preview.screenshot({ path: path.join(outDir, 'shell_preview.png'), fullPage: true });
  await page.screenshot({ path: path.join(outDir, 'shell_probe.png'), fullPage: true });
  console.log(`[probe] 截图 ${path.join(outDir, 'shell_probe.png')}`);

  check(pageErrors.length === 0 && previewErrors.length === 0, '没有未捕获的页面异常',
        [...pageErrors, ...previewErrors].slice(0, 3).join('\n         '));

  exitCode = bad ? 1 : 0;
} catch (e) {
  console.error(`\n[probe] 探针自己出错：${e && e.stack ? e.stack : e}`);
  exitCode = 2;
} finally {
  if (browser && !A.keep) await browser.close().catch(() => {});
  if (backend && !A.keep) killTree(backend);
}

console.log(`\n${'='.repeat(60)}\n通过 ${ok} 项，失败 ${bad} 项。`);
process.exit(exitCode);
