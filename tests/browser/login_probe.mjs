// 登录流程 + 页面外观的真实浏览器验证。
//
// 为什么和 probe.mjs 分开、而且要单独存在：
//
// probe.mjs 是**故意绕开登录页**的 —— 它从 Node 里 POST /api/login 拿到 cookie、
// 用 addCookies() 注进浏览器，理由是"前端坏了不该挡住 MSE 的诊断"。那个取舍
// 是对的，但代价是**登录页本身从来没有被任何东西打开过**：路由守卫有没有生效、
// 输错口令会不会给提示、登录成功后跳不跳得回原来那一页、登录页长什么样 ——
// 这些在 probe.mjs 眼里全都不存在。
//
// 而登录页恰恰是"谁都能进"那个问题的修复本身。一个自己都没被打开过的修复，
// 和没做是一样的。
//
// 顺带把 /status 与 /config 也走一遍：它们和登录页一样，只有浏览器能回答
// "渲染出来了没有"。curl 拿到 200 只说明后端把 HTML 发出去了。
//
// 用法:
//   node tests/browser/login_probe.mjs [--port 8125] [--rtsp 8126]
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
  const o = { port: 8125, rtsp: 8126, headless: true, keep: false,
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
  // 临时配置：绝不动用户 %APPDATA% 里那份。
  const cfg = path.join(outDir, 'login-config.json');
  fs.rmSync(cfg, { force: true });
  const logFd = fs.openSync(path.join(outDir, 'login-backend.log'), 'w');
  return spawn(exe, [
    '--rtsp', '--rtsp-port', String(A.rtsp),
    '--http', '--http-port', String(A.port),
    '--web-root', path.join(ROOT, 'web', 'dist'),
    '--http-user', A.user, '--http-password', A.password,
    '--no-onvif',            // 本探针不碰 ONVIF，关掉可以省掉 3702 的干扰
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
      // 问 /api/session：它**匿名可达**，所以它能回答"后端起来了没"。
      // 用 /api/status 的话永远等不到 200（没 cookie 恒 401），表现是等满
      // 超时然后报"后端起不来"。
      const r = await fetch(`${BASE}/api/session`);
      if (r.ok) return true;
    } catch {}
    await sleep(300);
  }
  return false;
}

// ---------------------------------------------------------------- 主流程
let backend = null, browser = null;
let exitCode = 1;

try {
  backend = startBackend();
  if (!await waitHttp()) throw new Error('后端 40 秒内没起来（看 out/browser/login-backend.log）');
  console.log(`[probe] 后端已就绪 ${BASE}（登录已开启，凭据 ${A.user}/***）`);

  browser = await chromium.launch({ channel: 'chrome', headless: A.headless });
  const ctx = await browser.newContext();
  const page = await ctx.newPage();

  // 全程收集。前端的 try/catch 会吃掉异常，但**没被 catch 的**会走到这里，
  // 而它们在控制台里只有一行，不看就等于没有。
  const pageErrors = [];
  const consoleErrors = [];
  page.on('pageerror', (e) => pageErrors.push(`${e} @ ${page.url()}`));
  // 记下**出错时页面在哪**。不记的话，一条 "WebSocket ... failed" 完全无法
  // 定位 —— 是在登录页连的、还是登录后被踢的、还是某个页面卸载时关的，
  // 三种原因三种处理方式，而消息本身一个字都不说。
  page.on('console', (m) => {
    if (m.type() === 'error') consoleErrors.push(`${m.text()} @ ${page.url()}`);
  });
  // 每一次 WS 连接的建立都记下来。**这是查"页面卸载了还在重连"的唯一手段**：
  // 那种连接不报错（后端只是拒绝它），只是安静地多出来一条，而它消耗的是
  // 真金白银 —— 用户在登录页上停着，预览页却在后台每 15 秒重试一次。
  const wsOpened = [];
  page.on('websocket', (ws) => wsOpened.push(`${ws.url()} @ ${page.url()}`));
  // 把失败请求的**具体地址**留下。控制台那条 "Failed to load resource: 401"
  // 不带 URL，而"登录页上有一个 /api/ 请求被拒"可能是设计如此（比如匿名探
  // 一次会话），也可能是某处在未登录时去拉数据 —— 两者的修法完全相反。
  const httpFails = [];
  page.on('response', (r) => {
    if (r.status() >= 400) httpFails.push(`${r.status()} ${r.request().method()} ${r.url()}`);
  });

  // ---- 1. 未登录访问首页 → 必须被守卫送回登录页 ------------------------
  console.log('\n[1] 路由守卫');
  await page.goto(`${BASE}/`, { waitUntil: 'networkidle' });
  await page.waitForURL(/\/login/, { timeout: 10000 }).catch(() => {});
  check(page.url().includes('/login'), `未登录访问 / → ${new URL(page.url()).pathname}`);
  check(await page.locator('form').count() > 0, '登录页上有表单');

  // 守卫要把原目标记在 next 上，登录完才知道该回哪 —— 这一条是
  // "带 next 的深链接"能工作的前提，而它只在直接敲地址时才走到。
  await page.goto(`${BASE}/config`, { waitUntil: 'networkidle' });
  await page.waitForURL(/\/login/, { timeout: 10000 }).catch(() => {});
  const nextParam = new URL(page.url()).searchParams.get('next') || '';
  check(nextParam === '/config', `守卫记住了原目标 next=${nextParam}`);

  // ---- 2. 登录页真的渲染出来了 ----------------------------------------
  console.log('\n[2] 登录页渲染');
  const nameBox = page.locator('input[type="text"]').first();
  const passBox = page.locator('input[type="password"]').first();
  const submit = page.locator('button[type="submit"]').first();
  check(await nameBox.count() === 1, '有用户名输入框');
  check(await passBox.count() === 1, '有口令输入框（type=password，不回显）');
  check(await submit.count() === 1, '有提交按钮');
  // 空表单时按钮该是禁用的 —— 否则用户会对着一个点了没反应的按钮反复点。
  check(await submit.isDisabled(), '空表单时提交按钮禁用');
  // 口令框不该被浏览器自动填上一个真口令（autocomplete 写错就会）。
  check(await passBox.getAttribute('autocomplete') === 'current-password',
        '口令框 autocomplete=current-password');
  const title = await page.locator('h1').first().innerText().catch(() => '');
  check(title.includes('Desk2ONVIF'), `页面标题渲染：${JSON.stringify(title)}`);
  await page.screenshot({ path: path.join(outDir, 'login_page.png'), fullPage: true });
  console.log(`[probe] 截图 ${path.join(outDir, 'login_page.png')}`);

  // ---- 3. 错误口令 → 必须给出提示，且停在这一页 ------------------------
  console.log('\n[3] 口令不对');
  await nameBox.fill(A.user);
  await passBox.fill(A.password + '-definitely-wrong');
  check(await submit.isEnabled(), '填了内容之后按钮可用');
  await submit.click();
  // 等提示出现。**不能只等固定时长然后断言 URL** —— 那样"提示慢了一拍"
  // 会被误判成"没有提示"。
  const errBanner = page.locator('.banner.is-err').first();
  let showedError = false;
  for (let i = 0; i < 40 && !showedError; i++) {
    showedError = await errBanner.count() > 0 && await errBanner.isVisible();
    if (!showedError) await sleep(150);
  }
  const errText = showedError ? (await errBanner.innerText()).trim() : '';
  check(showedError, `对立面给出错误提示：${JSON.stringify(errText.slice(0, 60))}`);
  check(page.url().includes('/login'), '口令错误时仍停在登录页');
  // 提示必须是人话，不是 "HTTP 401"。这一条防的是后端改了错误体形状而前端
  // 退化成打印状态码 —— 那种提示用户完全无从下手。
  check(/HTTP\s*401/.test(errText) === false, '提示不是裸的 "HTTP 401"');
  await page.screenshot({ path: path.join(outDir, 'login_error.png'), fullPage: true });

  // ---- 4. 正确口令 → 跳回原来想去的 /config ----------------------------
  console.log('\n[4] 口令正确');
  await passBox.fill(A.password);
  await submit.click();
  await page.waitForURL((u) => !u.pathname.includes('/login'), { timeout: 15000 })
    .catch(() => {});
  check(!page.url().includes('/login'), `登录后离开登录页 → ${new URL(page.url()).pathname}`);
  // 上面那次是带着 next=/config 被拦的，所以应当**回到 /config**，
  // 而不是一律丢到首页。
  check(new URL(page.url()).pathname === '/config',
        `登录后回到原目标（实得 ${new URL(page.url()).pathname}）`);

  // ---- 5. 会话持久：深链接不再被拦 ------------------------------------
  //
  // 这一条防的是"登录成功、下一个请求又是未登录"：cookie 没落地、path 写错、
  // 或 SameSite 挡了。刷新是最接近真实的一步 —— 用户登录完总会点几个页面。
  console.log('\n[5] 会话持久与三个页面');
  await page.reload({ waitUntil: 'networkidle' });
  check(!page.url().includes('/login'), '刷新 /config 仍在 /config（cookie 生效）');
  // 顶栏应当显示当前用户名，那是"我登录成了谁"的唯一提示。
  const bodyText = await page.locator('body').innerText();
  check(bodyText.includes(A.user), `顶栏显示登录用户 ${A.user}`);
  await page.screenshot({ path: path.join(outDir, 'page_config.png'), fullPage: true });

  const pages = [
    ['/status', 'stats', ['状态']],
    ['/', 'preview', ['预览']],
  ];
  for (const [route, shot, needles] of pages) {
    await page.goto(`${BASE}${route}`, { waitUntil: 'networkidle' });
    await sleep(1200);   // 让 WS 的首帧到达，页面从"连接中"变成有数
    const txt = await page.locator('body').innerText();
    check(!page.url().includes('/login'), `已登录访问 ${route} 不被拦`);
    check(needles.some((n) => txt.includes(n)), `${route} 有内容渲染（${needles[0]}）`);
    // 页面**不该是空的**：路由懒加载失败、组件抛错都会渲染出一片空白，
    // 而 HTTP 状态码照样是 200。
    check(txt.trim().length > 80, `${route} 不是空白页（${txt.trim().length} 字符）`);
    await page.screenshot({ path: path.join(outDir, `page_${shot}.png`), fullPage: true });
    console.log(`[probe] 截图 ${path.join(outDir, `page_${shot}.png`)}`);
  }

  // ---- 6. 退出登录 ----------------------------------------------------
  console.log('\n[6] 退出登录');
  const logoutBtn = page.locator('button', { hasText: '退出' }).first();
  check(await logoutBtn.count() > 0, '顶栏有退出按钮');
  if (await logoutBtn.count() > 0) {
    await logoutBtn.click();
    await page.waitForURL(/\/login/, { timeout: 10000 }).catch(() => {});
    check(page.url().includes('/login'), `退出后被送回登录页 → ${new URL(page.url()).pathname}`);
  }

  // ---- 7. 控制台干净 --------------------------------------------------
  console.log('\n[7] 浏览器控制台');
  console.log('  本次运行建立的 WS 连接：');
  for (const w of wsOpened) console.log(`    ${w}`);
  console.log('  4xx/5xx 的请求：');
  for (const f of httpFails) console.log(`    ${f}`);

  check(pageErrors.length === 0, `没有未捕获的页面异常`,
        pageErrors.slice(0, 3).join('\n         '));

  // 本次运行**唯一**允许出现的失败响应，是第 [3] 段故意打错的那一次口令。
  //
  // 这条断言值得单列：它防的是"某个页面在未登录时去拉一个要鉴权的接口" ——
  // 那种情况下浏览器只在控制台留一行不带 URL 的 "Failed to load resource: 401"，
  // 而页面看上去一切正常（那一格一直是空白的）。
  const unexpectedFails = httpFails.filter(
    (f) => !(f.startsWith('401 POST') && f.includes('/api/login')));
  check(unexpectedFails.length === 0, '除了那次故意打错的口令，没有别的失败响应',
        unexpectedFails.join('\n         '));

  // 只放行一种已知的浏览器噪声：WS 连接在**导航/卸载**时被销毁，Chromium
  // 会报 "broken close frame containing a reserved status code"。
  //
  // **这一条是量过的，不是猜的**（免得下一个人再查一遍）：
  //   - 服务端在未登录时发的是 `88 11 03f0 "unauthenticated"` —— FIN=1、
  //     opcode=0x8、未掩码、码 1008，逐字节合规；Chrome 单独连一次能正确
  //     解出 ev.code=1008，且**不报**任何控制台错误。这个码本身有**常驻**
  //     守卫（`tests/ws_probe.py` 的 [0] 段：不带 cookie 的升级必须被 1008
  //     拒掉），量它的那次一次性探针已经删了 —— 别在这里找它。
  //   - 客户端先关时，服务端回的是 `88 05 03e8 "bye"` —— 干净的 1000 回显。
  //   也就是说这条消息只在"页面正在走掉"的那一瞬间出现，而它不影响任何逻辑：
  //   前端在 onclose 里对已经放弃的 socket 有 `if (socket !== ws) return` 挡着。
  //   把它当失败，会让这个探针每次都红 —— 而**假失败比不测更坏**。
  const BENIGN = /broken close frame|WebSocket connection to .* failed/i;
  // "Failed to load resource: ... 401" 这条**没有 URL**，信息量严格少于上面
  // `httpFails` 那条断言（后者带方法、状态码和完整地址）。失败的请求已经由
  // 那条逐条盯着了，这里就不重复判 —— 留着它只会把"故意打错口令"那一项
  // 变成一条永远红的假失败。**不是放过一类错误，是换一条更严的判据。**
  const RESOURCE_FAIL = /Failed to load resource: the server responded with a status of \d+/i;
  const realErrors = consoleErrors.filter(
    (t) => !BENIGN.test(t) && !RESOURCE_FAIL.test(t) && !/favicon/i.test(t));
  const benign = consoleErrors.filter((t) => BENIGN.test(t));
  if (benign.length) console.log(`  已知噪声 ${benign.length} 条（WS 在导航时被销毁，见注释）`);
  check(realErrors.length === 0, `控制台没有别的 error 级输出（${realErrors.length} 条）`,
        realErrors.slice(0, 5).join('\n         '));

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
