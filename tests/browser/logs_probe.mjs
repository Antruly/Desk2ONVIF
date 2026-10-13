// 操作日志页（`/logs`）的真实浏览器验证。
//
// ## 为什么非要浏览器
//
// 这一页的**全部价值**在于「同一张空表格要能分辨出三种完全不同的现实」：
//   (1) **读不出来**（`ok:false`）、(2) **没在记**（`enabled:false`）、
//   (3) **确实还没有记录**（`ok:true` + 空数组）。
// 三者在 `/api/audit` 上都只是几个布尔和数组的差别，而在页面上必须长成三句
// 完全不同的话。只验接口的话，界面把三种都画成一张空表**照样全绿** —— 那正是
// 审计最不能出错的方向：「没记」和「记了但我读不到」是相反的两个答案。
//
// ## 它另外盯着的三条纪律
//
//   - **留存天数按服务端回报的值说话，不是写死的 180。** 所以本探针用
//     `--audit-keep-days 30` 起后端：页面上必须出现「保留 30 天」。而且 [6] 段
//     换一个 `7` 再量一次 —— **两次数值不同**才是「跟着服务端走」的证据，
//     只量一次的话写死的值也能过（陷阱 85 的纪律：这条断言是先想好怎么把它
//     弄红才写的）。
//   - **「复制一行」复制的是 `raw`**（磁盘上那一行的原文），不是把五列拼回去。
//   - **`/api/audit` 的键集与这一页读到的东西必须对得上。** 这一页读的是
//     `AuditPage` / `AuditEntry`，而后端是手写 JSON —— 少一个字段在页面上只表现为
//     一处空白（多半是「0 个文件 / 0 B」），控制台什么都不报。所以 [3] 段拿真实
//     回应逐键对账，而不是靠 fixture：fixture 是抄的，抄错了照样绿。
//
// ## 「读取出错」那一段用的是**真能到**的触发条件
//
// 不是「把 `logs` 目录删掉」—— 那个场景在本进程里到不了：只要审计开着，
// `note()` 就把当天的文件一直开在那里，而 `fopen(...,"ab")` 的共享模式没有
// `FILE_SHARE_DELETE` ⇒ Windows 拒绝删除或改名那个目录（两条都实测过：
// `Directory.Delete(dir,true)` → "being used by another process"、
// `Directory.Move` → "Access is denied"）。
// 能到的是「目录压根建不起来」：让 `logs` 这个位置被一个**同名文件**占掉，
// `create_directories` 就失败，而 `enabled_` 仍然是 true —— 这正是修复前
// 会安静地报成「这段时间没有任何操作记录」的那个状态。
//
// 用法:
//   node tests/browser/logs_probe.mjs [--port 8135] [--rtsp 8136]
//                                     [--user admin] [--password admin]
//                                     [--headed] [--keep]
//
// 端口 8135/8136：8133/8134 归 `out/v-audit-verify.ps1`，而这两个在此之前
// 没有任何文件用过（仓内实测：`tests/ scripts/ src/ web/src/ out/` 全文搜索零命中）。

import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

function args() {
  const a = process.argv.slice(2);
  const o = { port: 8135, rtsp: 8136, headless: true, keep: false,
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

/** 路径比大小写/斜杠无关 —— 后端报的是 `std::filesystem` 拼出来的那一份。 */
function normPath(s) {
  return String(s || '').replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase();
}
function samePath(a, b) { return normPath(a) === normPath(b); }

// ---------------------------------------------------------------- 后端
//
// 每一次都**换一个工作目录**：配置文件的父目录就是审计目录的父目录
// （`audit_dir = config_path.parent_path() / "logs"`）。不换的话几个阶段的日志
// 会混在同一个 `logs\` 里，「已关闭」那一段就验不干净。
function backendDirs(name) {
  const d = path.join(outDir, 'logs-probe', name);
  fs.rmSync(d, { recursive: true, force: true });
  fs.mkdirSync(d, { recursive: true });
  return { dir: d, cfg: path.join(d, `${name}.json`), logs: path.join(d, 'logs') };
}

let logFd = null;
let backend = null;

function startBackend(dirs, extra = []) {
  const exe = path.join(ROOT, 'build', 'bin', 'desk2onvif.exe');
  if (!fs.existsSync(exe)) throw new Error(`没有 ${exe}，先跑 scripts\\build.ps1`);
  if (logFd !== null) { try { fs.closeSync(logFd); } catch {} }
  logFd = fs.openSync(path.join(outDir, 'logs-backend.log'), 'w');
  return spawn(exe, [
    '--rtsp', '--rtsp-port', String(A.rtsp),
    '--http', '--http-port', String(A.port),
    '--web-root', path.join(ROOT, 'web', 'dist'),
    '--http-user', A.user, '--http-password', A.password,
    '--no-onvif',            // 本探针不碰 ONVIF，顺带省掉 3702 的干扰
    '--config', dirs.cfg,
    ...extra,
  ], { cwd: ROOT, stdio: ['ignore', logFd, logFd] });
}

/** `taskkill` 是**异步**的：不等它退完就往下走，等于赌端口已经放开。 */
async function killTree(p, timeoutMs = 8000) {
  if (!p || p.exitCode !== null || p.signalCode !== null) return;
  const done = new Promise((r) => p.once('exit', r));
  try { spawn('taskkill', ['/F', '/T', '/PID', String(p.pid)], { stdio: 'ignore' }); } catch {}
  await Promise.race([done, sleep(timeoutMs)]);
}

/**
 * 端口真的放开了没有 —— **问端口自己，别猜**。
 *
 * 实测过的那一次红（`[4]` 段第一次 `fetch` 报 `TypeError: fetch failed`，看起来
 * 像探针自己坏了）：`close()` 里只 `sleep(600)`，`taskkill` 还在路上，新后端起来
 * 时旧进程还占着 8135 ⇒ 新进程绑不上（产品侧那条 ≤5 秒的有界重试还没走完），
 * 而 `waitHttp()` 探到的是**正在退场的旧进程**、于是探测通过了；旧进程一死，
 * 后面每一个请求都打在空气上。**症状一路指不到原因**：新进程的日志文件被
 * 旧进程的尾巴占着，`/api/session` 那一步"明明是通的"。
 *
 * 判据用"这个端口现在能不能被绑上"，而不是"我 sleep 了多久"—— 后者在快机器上
 * 够、慢机器上不够，而它失败的样子（一个连不上但"刚才还通"的服务）恰好最不像
 * 超时不够用。
 *
 * ⚠️ **必须探 `0.0.0.0`。** 第一版探的是 `127.0.0.1`，实测量出来它在端口**真的
 * 被占着**的时候照样报 free —— 因为占用方（产品）绑的是 `0.0.0.0`，而 Windows
 * 允许把一个具体地址绑到已被 `0.0.0.0` 占用的端口上。也就是说那条守卫**永远
 * 不会红**，却看起来一直在守着（陷阱 85 的形状）。
 * 变异检验与"会不会假红"（TIME_WAIT）两条都在 `out/probe-tools/portfree-check.mjs`
 * 里量过：探 `0.0.0.0` 时被占着报 false、空着报 true、刚有连接进出过的端口仍报 true。
 */
function portFree(port) {
  return new Promise((resolve) => {
    const s = net.createServer();
    s.once('error', () => resolve(false));
    s.once('listening', () => s.close(() => resolve(true)));
    s.listen(port, '0.0.0.0');
  });
}

async function waitPortFree(port, timeoutMs = 15000) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    if (await portFree(port)) return true;
    await sleep(200);
  }
  return false;
}

async function waitHttp(timeoutMs = 40000) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    try {
      // /api/session 匿名可达，所以它答的是「后端起来了没」。
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

/** limit 与页面上的默认值保持一致 —— 两处 limit 不同的话 count 会天生对不上。 */
async function apiAudit(cookie, qs = 'days=7&limit=500') {
  const r = await fetch(`${BASE}/api/audit?${qs}`, { headers: { Cookie: `d2o_session=${cookie}` } });
  return { status: r.status, json: await r.json() };
}

/** 起一个后端、登录、把 cookie 装进浏览器上下文、打开 `/logs` 并等第一次读日志回来。 */
async function open(ctx, dirs, extra) {
  backend = startBackend(dirs, extra);
  if (!await waitHttp()) throw new Error('后端 40 秒内没起来（看 out/browser/logs-backend.log）');
  const cookie = await login();
  await ctx.clearCookies();
  await ctx.addCookies([{
    name: 'd2o_session', value: cookie, url: `${BASE}/`,
    httpOnly: true, sameSite: 'Lax',
  }]);
  const page = await ctx.newPage();
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));
  // 等**那一次真的读日志**，而不是等一个固定的毫秒数：`onMounted` 里那次
  // `load()` 什么时候回来，跟这一页要验的东西无关，但不等它就会读到空表。
  // waitForResponse 必须在 goto **之前**挂上，否则响应可能已经过去了。
  const first = page.waitForResponse((r) => r.url().includes('/api/audit'),
                                     { timeout: 20000 }).catch(() => null);
  await page.goto(`${BASE}/logs`, { waitUntil: 'domcontentloaded' });
  await first;
  await page.locator('.page-head h1').waitFor({ timeout: 20000 });
  return { page, cookie, pageErrors };
}

async function close(page) {
  if (page) await page.close().catch(() => {});
  const p = backend;
  backend = null;
  // 两道：**等进程真的退出**（确定的那个信号），再**问端口放开了没有**。
  // 前一道才是主判据；后一道是防"进程退了但端口还挂在 TIME_WAIT 上"。
  // 只 `sleep(600)` 那一版就是在这里输掉的 —— 见 `portFree` 的注释。
  await killTree(p);
  if (!await waitPortFree(A.port)) {
    throw new Error(`端口 ${A.port} 在 15 秒内没有被放开：上一个后端没退干净，` +
                    `后面的阶段全部不可信`);
  }
}

// ---------------------------------------------------------------- 页面读数

/** 按 `<h2>` 认卡片。**不按 hasText 认** —— 文本会在别处偶然出现。 */
function card(page, title) {
  return page.locator('.card')
    .filter({ has: page.locator('h2', { hasText: title }) })
    .first();
}

/** 只读行的「标签 / 值」两半（`span.hint` + `code.value`）。 */
async function readonlyRow(page, nth = 0) {
  return page.locator('.readonly-row').nth(nth).evaluate((el) => ({
    label: (el.querySelector('.hint')?.textContent || '').trim(),
    value: (el.querySelector('code.value')?.textContent || '').trim(),
  }));
}

/**
 * 表格里的**数据行**（跳过按天分组那一行小标题）。
 *
 * 分组行是 `<td colspan="6">`，数据行是 6 个 `td` —— 按这个分，别按文字分。
 */
async function dataRows(page) {
  return page.locator('.stream-table tbody tr').evaluateAll((trs) => trs
    .filter((tr) => tr.querySelectorAll('td').length === 6)
    .map((tr) => {
      const td = tr.querySelectorAll('td');
      const action = (td[3].querySelector('code.muted')?.textContent || '').trim();
      const full = td[3].innerText.trim();
      // 动作那一格是「人话 + 等宽小字的代码」：人话是插值文本节点，代码在它
      // 后面。不按 childNodes 下标取（那取决于 Vue 编译出来的空文本节点有几
      // 个），改成「把代码那一半从整格文本里摘掉」—— 两种排布都对。
      const label = action && full.endsWith(action)
        ? full.slice(0, full.length - action.length).trim()
        : full;
      return {
        time: td[0].innerText.trim(),
        source: td[1].innerText.trim(),
        identity: td[2].innerText.trim(),
        label,
        action,
        detail: td[4].innerText.trim(),
        hasCopyBtn: !!td[5].querySelector('button'),
      };
    }));
}

async function rowsOf(page, action) {
  return (await dataRows(page)).filter((r) => r.action === action);
}

async function refresh(page) {
  // `force: true` 跳过可点性检查（含「是否 disabled」）—— 正在读取时那个按钮
  // 是 disabled 的，正常 click 会一直等到超时才失败（默认 30 秒，整段卡死）。
  await page.locator('.page-head button').first()
    .click({ force: true, timeout: 1500 }).catch(() => {});
  await sleep(250);
}

/**
 * 等某个动作串的行出现（写日志与读日志之间隔着一次刷新）。
 * `pred` 用来在同一个动作串的**多行**里挑一行 —— 「同一件事发生过两次，
 * 第二次才是我要的那次」在这页上很常见（先发一个不生效的补丁、再发一个生效的）。
 */
async function findRow(page, action, pred = () => true, tries = 40) {
  for (let i = 0; i < tries; i++) {
    const r = (await dataRows(page)).find((x) => x.action === action && pred(x));
    if (r) return r;
    await refresh(page);
  }
  return null;
}

const waitRow = (page, action, tries = 40) => findRow(page, action, () => true, tries);

async function submitBtn(page) {
  return page.locator('form:has(#log-keyword) button[type="submit"]');
}

/** 填表 → 点查询 → **等那一次 `/api/audit` 真的回来**。 */
async function submitQuery(page, { days, limit, keyword }) {
  if (days !== undefined) await page.fill('#log-days', String(days));
  if (limit !== undefined) await page.fill('#log-limit', String(limit));
  if (keyword !== undefined) await page.fill('#log-keyword', keyword);
  const btn = await submitBtn(page);
  // 上一次查询还在跑的话按钮是 disabled 的，等它可点再发 —— 否则这一下会被
  // 静默吃掉，于是断言的是一份**上一次**的结果。
  for (let i = 0; i < 40 && await btn.isDisabled(); i++) await sleep(100);
  const done = page.waitForResponse((r) => r.url().includes('/api/audit'),
                                    { timeout: 15000 }).catch(() => null);
  await btn.click();
  await done;
}

// ---------------------------------------------------------------- 主流程
let browser = null, ctx = null;
let exitCode = 1;
const allPageErrors = [];

try {
  browser = await chromium.launch({ channel: 'chrome', headless: A.headless });
  ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  // 剪贴板在 headless 下默认没有权限。不显式授权的话，量到的是浏览器策略
  // （复制失败），而不是我们的接线。
  await ctx.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: BASE });

  // ==================================================================
  // [1] 正常：留存策略、日志目录、记录表
  // ==================================================================
  console.log('\n[1] 正常路径：留存 30 天 / 日志目录 / 记录表');
  const A1 = backendDirs('logs-a');
  let { page, cookie, pageErrors } = await open(ctx, A1, ['--audit-keep-days', '30']);
  allPageErrors.push(...pageErrors);
  console.log(`[probe] 后端已就绪 ${BASE}（审计目录 ${A1.logs}）`);

  check((await page.locator('.page-head h1').innerText()).trim() === '操作日志',
        '页面标题是「操作日志」');

  // 先造一条**网页改配置**的记录 —— 用户点名的三件事（对时 / 改名 / 改码率）之一。
  //
  // 这里要发**两次**、而且要按这个顺序发，因为后端的规矩本来就是两步的：
  // `validate_config` 会把具名档位（`capture.quality`）辖下的四项 —— 帧率 / 码率 /
  // 分辨率上限 / 子码流 —— 按档位**覆盖回去**（`config.cpp` 里那段 `snap`）。
  // 所以「只改码率、档位还停在具名档上」是一个**故意的** no-op，不是 bug。
  // 真实前端不是这么发的：`ConfigView.vue::buildPayload()` 会把档位换成
  // **推导出来的**那一个（`effectiveQuality()`，手改过具体字段就是 `custom`）。
  // 只发第二次 ⇒ 末尾那条审计断言会绿在一个假前提上；只发第一次 ⇒ 它永远
  // 只看到「没有字段发生变化」。两次都发，才既证明了规矩、又验到了真东西。
  const cfgPost = (body) => fetch(`${BASE}/api/config`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Cookie: `d2o_session=${cookie}` },
    body: JSON.stringify(body),
  });

  const bare = await cfgPost({ capture: { bitrate_kbps: 3010 } });
  check(bare.ok, `POST /api/config 发一个「只改具体字段」的补丁（${bare.status}）`);
  const bareRow = await findRow(page, 'config.set', (r) => /没有字段发生变化/.test(r.detail));
  check(!!bareRow,
        '档位还停在具名档上时，只改具体字段是**故意**不生效的（审计如实记成「没有字段发生变化」）',
        `实得 config.set 的细节：${JSON.stringify((await rowsOf(page, 'config.set')).map((r) => r.detail))}`);

  const post = await cfgPost({ capture: { quality: 'custom', bitrate_kbps: 3000 } });
  check(post.ok, `POST /api/config 改码率成功（${post.status}）`);

  // ---- 留存与位置 ------------------------------------------------------
  const keepCard = card(page, '留存与位置');
  const retention = (await keepCard.locator('p.hint').first().innerText()).trim();
  // **本阶段的头号断言。** 后端拿的是 `--audit-keep-days 30`，所以页面上必须
  // 出现 30 —— 写死 180 的那一版在这里当场变红。
  check(/保留 30 天/.test(retention),
        `留存那一行按服务端回报的 30 天说话（实得 ${JSON.stringify(retention)}）`,
        '写死的 180 会在这里红；`--audit-keep-days 30` 是这条断言的量具');
  check(/恢复出厂会清空/.test(retention), '留存那一行说明了「恢复出厂会清空」');

  const dirRow = await readonlyRow(page, 0);
  check(dirRow.label === '日志目录',
        `只读行的标签是「日志目录」（实得 ${JSON.stringify(dirRow.label)}）`);
  check(samePath(dirRow.value, A1.logs),
        `日志目录就是配置目录下的 logs（页面 ${JSON.stringify(dirRow.value)} / 期望 ${A1.logs}）`);

  // ---- 复制目录 --------------------------------------------------------
  await page.locator('.readonly-row').first().locator('button').click();
  let dirCopied = '';
  for (let i = 0; i < 20 && !dirCopied; i++) {
    dirCopied = (await keepCard.locator('.hint').allInnerTexts())
      .map((s) => s.trim()).filter((s) => /已复制|复制失败/.test(s)).join('');
    if (!dirCopied) await sleep(150);
  }
  check(/已复制/.test(dirCopied),
        `点「复制」之后目录那一行给出了结论（实得 ${JSON.stringify(dirCopied)}）`,
        /复制失败/.test(dirCopied) ? '剪贴板写入被浏览器拒了 —— 先看 grantPermissions 那一行' : '');
  await sleep(2300);   // 那 2 秒的提示要自己消失，否则干扰下面「只有一处」那条

  // ---- 记录表 ----------------------------------------------------------
  let rows = await dataRows(page);
  check(rows.length >= 1, `表格里有数据行（实得 ${rows.length} 行）`);
  check(rows.every((r) => /^\d{2}:\d{2}:\d{2}$/.test(r.time)),
        `时间是 HH:MM:SS（实得 ${JSON.stringify(rows.slice(0, 3).map((r) => r.time))}）`);
  check(rows.every((r) => r.source !== '' && r.identity !== ''),
        '每一行都有来源和身份（空的话后端会写成 ? 或 -，不该出现空串）');
  check(rows.every((r) => r.hasCopyBtn), '每一行都有自己的复制按钮');

  const startup = await waitRow(page, 'startup');
  check(!!startup, '表里有 startup 那一行（程序启动）',
        `实得动作：${JSON.stringify((await dataRows(page)).map((r) => r.action).slice(0, 12))}`);
  if (startup) {
    check(startup.label === '程序启动',
          `startup 显示成人话「程序启动」（实得 ${JSON.stringify(startup.label)}）`);
    // 细节里带着**这一次**的版本、形态与监听端口 —— 排查「那天几点重启过、
    // 当时跑的是哪一份」靠的就是这一行，所以它必须是自己那次的事实。
    check(/版本/.test(startup.detail) && /形态/.test(startup.detail),
          `startup 的细节里有版本与形态（实得 ${JSON.stringify(startup.detail)}）`);
    check(startup.detail.includes(String(A.port)),
          `startup 的细节里带着本次的网页端口 ${A.port}（实得 ${JSON.stringify(startup.detail)}）`,
          '这一条把「这一行是这次起的」钉住了：换个端口跑，它必须跟着变');
  }

  const cfgRow = await findRow(page, 'config.set', (r) => r.detail.includes('capture.bitrate_kbps'));
  check(!!cfgRow, '改码率之后表里出现了点名了字段的那条 config.set 行');
  if (cfgRow) {
    // **口令纪律在这里的投影**：记的是**字段名**，不是值。
    check(cfgRow.detail.includes('capture.bitrate_kbps'),
          `config.set 的细节里点了字段名（实得 ${JSON.stringify(cfgRow.detail)}）`);
    check(!/\b3000\b/.test(cfgRow.detail),
          `config.set 里**没有**那个值本身（实得 ${JSON.stringify(cfgRow.detail)}）`,
          '记字段名不记值是硬纪律 —— 口令就藏在这一类值里');
    check(cfgRow.source === '127.0.0.1',
          `config.set 的来源是发起请求的那个 IP（实得 ${JSON.stringify(cfgRow.source)}）`);
    check(cfgRow.label === '改了配置',
          `config.set 显示成人话「改了配置」（实得 ${JSON.stringify(cfgRow.label)}）`);
  }

  // 按天分组的小标题 —— 日志按天分文件，界面上也按天断开。
  const dayHeads = (await page.locator('.stream-table tbody tr td[colspan="6"]').allInnerTexts())
    .map((s) => s.trim());
  check(dayHeads.length >= 1 && /^\d{4}-\d{2}-\d{2}$/.test(dayHeads[0]),
        `表格按天分组，小标题是日期（实得 ${JSON.stringify(dayHeads)}）`);

  // ---- 复制一行 --------------------------------------------------------
  rows = await dataRows(page);          // 上面刷新过，行数要以现在这份为准
  const copyBtns = page.locator('.stream-table tbody tr td:last-child button');
  check(await copyBtns.count() === rows.length,
        `每一行一个复制按钮（实得 ${await copyBtns.count()} / ${rows.length} 行）`);
  await copyBtns.first().click();
  let rowCopied = '';
  for (let i = 0; i < 20 && !rowCopied; i++) {
    rowCopied = (await page.locator('.hint.tone-good').allInnerTexts())
      .map((s) => s.trim()).join('');
    if (!rowCopied) await sleep(150);
  }
  check(/已复制/.test(rowCopied),
        `点行尾的复制按钮给出了结论（实得 ${JSON.stringify(rowCopied)}）`);
  const goodCount = await page.locator('.hint.tone-good').count();
  check(goodCount === 1, `只有点过的那一行显示「已复制」（实得 ${goodCount} 处）`,
        '多于一处 = 状态是全局的而不是每行一份');

  // ==================================================================
  // [2] 关键字过滤（与「没有匹配」那一支）
  // ==================================================================
  console.log('\n[2] 关键字过滤');
  // 先造一次登录失败，让 login.fail 也有一行 —— 它是这一页最该被一眼扫到的动作。
  const badLogin = await fetch(`${BASE}/api/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ user: A.user, password: 'definitely-not-the-password' }),
  });
  check(!badLogin.ok, `故意用错口令登录被拒（${badLogin.status}）`,
        '要是不被拒，下面 login.fail 那几条就没有被测对象');

  await submitQuery(page, { keyword: 'startup' });
  let filtered = await dataRows(page);
  check(filtered.length >= 1 && filtered.every((r) => r.action === 'startup'),
        `关键字 startup 过滤出且只过滤出 startup（实得 ${JSON.stringify(filtered.map((r) => r.action))}）`);

  await submitQuery(page, { keyword: 'login.fail' });
  filtered = await dataRows(page);
  check(filtered.length >= 1 && filtered.every((r) => r.action === 'login.fail'),
        `关键字 login.fail 过滤出登录失败（实得 ${JSON.stringify(filtered.map((r) => r.action))}）`,
        '一条都没有的话先看后端是不是按 IP 限流把这次失败吞了');
  if (filtered.length >= 1) {
    check(filtered[0].label === '登录失败',
          `login.fail 显示成人话「登录失败」（实得 ${JSON.stringify(filtered[0].label)}）`);
    // **口令纪律的第二处投影**：失败那一条同样不能带上试过的口令。
    check(!/definitely-not-the-password/.test(filtered[0].detail),
          `login.fail 的细节里没有那次试的口令（实得 ${JSON.stringify(filtered[0].detail)}）`,
          '口令一旦进日志就是事故 —— 这条是硬性的');
  }

  // 「没有匹配」与「确实没有记录」是**两种空**，页面上要说成两句不同的话。
  await submitQuery(page, { keyword: 'zzz-绝不可能出现的字符串' });
  const noMatchTxt = await card(page, '记录').innerText();
  check(/没有匹配/.test(noMatchTxt),
        '没有命中时说明的是「没有匹配」，不是「没有任何操作记录」',
        `实得：${JSON.stringify(noMatchTxt.slice(0, 300))}`);
  check(!/这段时间里没有任何操作记录/.test(noMatchTxt),
        '**没有**把「过滤后为空」说成「这段时间里没有任何操作记录」',
        '带着关键字去查是用户的动作，把那一下说成「设备上什么都没发生」是错的');

  // 清掉关键字要能回到全量 —— 否则用户会以为日志真的就这么少。
  await submitQuery(page, { keyword: '' });
  const backAll = await dataRows(page);
  check(backAll.length >= rows.length,
        `清掉关键字之后回到全量（${rows.length} → ${backAll.length}）`);

  await page.screenshot({ path: path.join(outDir, 'logs_probe.png'), fullPage: true });

  // ==================================================================
  // [3] 与 `/api/audit` 逐键对账
  // ==================================================================
  console.log('\n[3] 与 /api/audit 逐键对账');
  const a = await apiAudit(cookie);
  check(a.status === 200, `GET /api/audit 是 200（实得 ${a.status}）`);
  const KEY_SET = ['bytes', 'count', 'days', 'dir', 'enabled', 'entries', 'files', 'keep_days', 'limit', 'ok'];
  const gotKeys = Object.keys(a.json).sort();
  check(JSON.stringify(gotKeys) === JSON.stringify(KEY_SET),
        `回应的键集与这一页读的完全一致（实得 ${JSON.stringify(gotKeys)}）`,
        `期望 ${JSON.stringify(KEY_SET)} —— 少了谁，页面上就有一处是空白`);
  const ENTRY_KEYS = ['action', 'day', 'detail', 'identity', 'raw', 'source', 'time'];
  const e0 = a.json.entries && a.json.entries[0];
  check(!!e0 && JSON.stringify(Object.keys(e0).sort()) === JSON.stringify(ENTRY_KEYS),
        `每一条的键集也对得上（实得 ${JSON.stringify(e0 ? Object.keys(e0).sort() : null)}）`,
        `期望 ${JSON.stringify(ENTRY_KEYS)}`);
  check(a.json.enabled === true && a.json.ok === true,
        `这一份是「开着且读到了」（enabled=${a.json.enabled} ok=${a.json.ok}）`);
  check(a.json.keep_days === 30,
        `keep_days 是命令行给的 30（实得 ${a.json.keep_days}）`);

  // 页面上的行数必须与接口的 count 一致 —— 两处各自算一遍的话，
  // 「界面上少了三条」这种错不会有人发现。
  const shown = (await dataRows(page)).length;
  check(shown === a.json.count,
        `页面上画出来的行数与接口的 count 一致（页面 ${shown} / 接口 ${a.json.count}）`);

  // `raw` 就是磁盘上那一行的原文：五个字段用 " | " 连着。
  if (e0) {
    const parts = e0.raw.split(' | ');
    check(parts.length === 5 && parts[3] === e0.action && parts[2] === e0.identity,
          `raw 是那一行原文（分隔符 " | "，动作与身份对得上）：${JSON.stringify(e0.raw)}`);
    check(e0.raw.startsWith(e0.day + ' ' + e0.time),
          `raw 以 day + time 开头（${JSON.stringify(e0.raw.slice(0, 24))}）`);
  }

  await close(page);

  // ==================================================================
  // [4] 没在记：`--no-audit` → 「已关闭」，而不是「没有任何操作记录」
  // ==================================================================
  //
  // 这一支最容易被误读：一张空表和「这台设备上没发生过事」长得一模一样，
  // 而事实是压根没在记。
  console.log('\n[4] --no-audit：已关闭');
  const A4 = backendDirs('logs-off');
  ({ page, cookie, pageErrors } = await open(ctx, A4, ['--no-audit']));
  allPageErrors.push(...pageErrors);

  const offJson = (await apiAudit(cookie)).json;
  check(offJson.enabled === false,
        `接口报 enabled=false（实得 ${offJson.enabled}）`,
        '这一条要是不成立，下面那几条就是在前置条件错了的情况下绿的');

  const offBanner = page.locator('.banner.is-warn').first();
  const offBannerTxt = await offBanner.innerText();
  check(await offBanner.isVisible() && /关闭/.test(offBannerTxt) && /--no-audit/.test(offBannerTxt),
        '顶上的横幅明说是「关闭」的、并指出是哪个开关关的',
        `实得：${JSON.stringify(offBannerTxt.slice(0, 400))}`);

  const offTxt = await card(page, '记录').innerText();
  check(/已关闭/.test(offTxt),
        '记录那一段说的是「已关闭」',
        `实得：${JSON.stringify(offTxt.slice(0, 400))}`);
  check(!/这段时间里没有任何操作记录/.test(offTxt),
        '**没有**把「没在记」说成「这段时间里没有任何操作记录」',
        '这两句是相反的答案，审计里说错一句就等于伪造了一份记录');
  check(await page.locator('.stream-table').count() === 0,
        '关闭时不画表格（一张空表比一句话更容易被当成「没有记录」）');
  await close(page);

  // ==================================================================
  // [5] 读不出来：`logs` 那个位置被一个同名文件占了
  // ==================================================================
  //
  // 这是修复后**唯一到得了**的那种失败（理由见文件头）。判据是页面上说的是
  // **「读取出错」** —— 修复前这里回来的是 `ok:true / count:0`，于是页面
  // 报的是上面那句「这段时间里没有任何操作记录」。
  console.log('\n[5] 读取出错：logs 被同名文件占了');
  const A5 = backendDirs('logs-bad');
  fs.writeFileSync(A5.logs, '我不是目录\n');   // 占位：create_directories 会因此失败
  check(fs.statSync(A5.logs).isFile(), '(5) 前置：logs 是一个文件（不是目录）');

  ({ page, cookie, pageErrors } = await open(ctx, A5, []));
  allPageErrors.push(...pageErrors);

  const badJson = (await apiAudit(cookie)).json;
  check(badJson.enabled === true, `(5) 开关还开着（enabled=${badJson.enabled}）`);
  check(badJson.ok === false,
        `(5) 接口报 ok=false（实得 ${badJson.ok}）`,
        '修复前这里是 true —— 那正是这一页会把「读不到」说成「没有记录」的时刻');
  check(!!badJson.error,
        `(5) 接口给了 error 原文（${JSON.stringify(badJson.error)}）`);

  const badBannerTxt = await page.locator('.banner.is-warn').first().innerText();
  check(/读取出错/.test(badBannerTxt), '(5) 顶上的横幅给出了「读取出错」',
        `实得：${JSON.stringify(badBannerTxt.slice(0, 400))}`);
  // 横幅里必须带上**服务端这一次给的**原因。写死一句话的实现在这里红 ——
  // 而"读不到"最要紧的正是"为什么读不到"。
  check(!!badJson.error && badBannerTxt.includes(badJson.error),
        '(5) 横幅把服务端给的原因原样带出来了',
        `服务端 error：${JSON.stringify(badJson.error)}\n         横幅：${JSON.stringify(badBannerTxt.slice(0, 400))}`);

  // 「照着去查」要有一个打得开的路径 —— 它就在只读行里。
  const badDirRow = await readonlyRow(page, 0);
  check(samePath(badDirRow.value, A5.logs),
        `(5) 只读行给出的目录还是那一个（页面 ${JSON.stringify(badDirRow.value)}）`,
        `期望 ${A5.logs} —— 读不到的时候，这个路径是用户唯一的抓手`);

  const badTxt = await card(page, '记录').innerText();
  check(/没能读出来/.test(badTxt),
        '(5) 记录那一段说的是「没能读出来」',
        `实得：${JSON.stringify(badTxt.slice(0, 400))}`);
  check(!/这段时间里没有任何操作记录/.test(badTxt),
        '(5) **没有**把「读不到」说成「这段时间里没有任何操作记录」',
        '这是审计里最不能出错的那个方向');

  await page.screenshot({ path: path.join(outDir, 'logs_bad.png'), fullPage: true });
  await close(page);

  // ==================================================================
  // [6] 直接打开 /logs（刷新 / 收藏夹那条路）
  // ==================================================================
  //
  // 从菜单点进去和直接打开是**两条**路：后者要过 SPA 回落 + 路由懒加载，
  // 而菜单那条在同一个 SPA 里不会重新拉 chunk。少验一条，坏的那条就只在
  // 用户收藏了地址之后才暴露 —— 症状是点进那一页白屏。
  console.log('\n[6] 直接打开 /logs');
  const A6 = backendDirs('logs-direct');
  ({ page, cookie, pageErrors } = await open(ctx, A6, ['--audit-keep-days', '7']));
  allPageErrors.push(...pageErrors);
  check((await page.locator('.page-head h1').innerText()).trim() === '操作日志',
        '直接打开 /logs 也能渲染（SPA 回落 + 懒加载那条路）');
  const six = (await card(page, '留存与位置').locator('p.hint').first().innerText()).trim();
  check(/保留 7 天/.test(six),
        `换一个留存天数它也跟着变（保留 7 天，实得 ${JSON.stringify(six)}）`,
        '两次数值不同才是「按服务端回报值说话」的证据；只量一次的话写死的值也能过');
  await close(page);

  check(allPageErrors.length === 0, '没有未捕获的页面异常',
        allPageErrors.slice(0, 3).join('\n         '));

  exitCode = bad ? 1 : 0;
} catch (e) {
  console.error(`\n[probe] 探针自己出错：${e && e.stack ? e.stack : e}`);
  exitCode = 2;
} finally {
  if (backend) await killTree(backend);
  if (browser && !A.keep) await browser.close().catch(() => {});
  if (logFd !== null) { try { fs.closeSync(logFd); } catch {} }
}

console.log(`\n${'='.repeat(60)}\n通过 ${ok} 项，失败 ${bad} 项。`);
process.exit(exitCode);
