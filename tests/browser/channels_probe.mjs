// 多通道在**真实浏览器**里的验证（Phase AF）。
//
// 三件事在这里验，而它们在 curl / Python 探针里**全都看不见**：
//
//  [1] 「一次只看一路」是真的。判据不是"后端说它是"，而是**数出来的**：
//      切换之后页面上只留一条 `/ws/preview`，旧那条真的关了、并且此后
//      **一个分片都没再收到**，新那条在推。只看视频还在放是不够的 ——
//      "旧的那路还在后台推"这种实现同样能播，而它正是这一轮要避免的那件事
//      （CPU 与「真实延迟」读数要与单通道同口径）。
//
//  [2] 换通道换的是**真码流**。判据是解码出来的尺寸：两路源的尺寸不同，
//      切过去之后 `video.videoWidth/Height` 必须变成**新的那一路**的。
//      这一条挡的是"moov 用了旧通道那一份"这个**没有任何报错**的失败
//      （track id、timebase、SPS 全对不上，画面永远黑）—— 它是
//      `PreviewHub` 那份 init 缓存存在的唯一理由，也只有在这一条上才验得到。
//
//  [3] 配置页那条「勾一个摄像头 → 写一条 items[]」的路走通，且**发出去的
//      JSON 里**真有那一项（不是只看界面上多了一行）。以及 `/info` 上那三个
//      锁死的字段是**真的 `disabled`**，不是"看起来灰"。
//
// 用法:
//   node tests/browser/channels_probe.mjs [--port 8131] [--rtsp 8132]
//                                        [--user admin] [--password admin]
//                                        [--headed] [--keep]
//
// ⚠️ 需要本机**至少两个画面源**：一台摄像头 + 一块可捕获的显示器。
//   通道 1 是摄像头（`--camera`），通道 2 是显示器 #0。没有摄像头时这个探针
//   跑不了 —— 它会以退出码 2 明说，而不是悄悄全绿。

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

const EXE = path.join(ROOT, 'build', 'bin', 'desk2onvif.exe');

/**
 * 把界面上那一格 `12.3 MB` 读回成字节数 —— 和 `web/src/format.ts` 的
 * `formatBytes` 反着来（单位是 1024 进制，同一张表）。
 *
 * 存在的理由：要比"两次读数谁大"。直接把两段**字符串**比大小是不行的
 * （`'9.0 MB' > '10.0 MB'` 按字典序为真），而只判"有个数字"又挡不住写死的值。
 * 读不出来时返回 -1 —— 断言那边会红，而不是悄悄当成 0。
 */
function parseBytes(s) {
  const m = /([0-9]+(?:\.[0-9]+)?)\s*(B|KB|MB|GB|TB)/.exec(s);
  if (!m) return -1;
  const mult = { B: 1, KB: 1024, MB: 1024 ** 2, GB: 1024 ** 3, TB: 1024 ** 4 }[m[2]];
  return Math.round(parseFloat(m[1]) * mult);
}

let ok = 0, bad = 0;
function check(cond, what, detail = '') {
  if (cond) { ok++; console.log(`  [OK]   ${what}`); }
  else { bad++; console.log(`  [FAIL] ${what}${detail ? `\n         ${detail}` : ''}`); }
}

/** 跑一次外部命令，收它的 stdout（`--list-cameras` 用）。 */
function run(exe, argv, timeoutMs = 20000) {
  return new Promise((resolve) => {
    const p = spawn(exe, argv, { cwd: ROOT });
    let out = '', err = '';
    const t = setTimeout(() => { try { p.kill(); } catch {} resolve({ out, err, timedOut: true }); }, timeoutMs);
    p.stdout.on('data', (d) => { out += d.toString('utf8'); });
    p.stderr.on('data', (d) => { err += d.toString('utf8'); });
    p.on('close', (code) => { clearTimeout(t); resolve({ out, err, code }); });
  });
}

// ---------------------------------------------------------------- 配置
//
// 探针自己写一份配置，**只为把通道 2 的分辨率钉死**：不钉的话它由画质档位
// （`smooth` → max_height 1080）间接决定，而 [2] 那条"切过去之后尺寸变了"
// 就依赖"两路源的原始尺寸恰好不同"—— 那是个关于**这台机器**的事实，不是
// 关于代码的。写死之后，这条断言在任何机器上都成立。
//
// `quality: "custom"` 是关键：档位会把 max_height 覆盖回档位值（`expand_quality`），
// 写 720 却用 `smooth` 档的话，后端会把它改回 1080。
function writeConfig() {
  const cfg = path.join(outDir, 'channels-probe.json');
  fs.rmSync(cfg, { force: true });
  fs.writeFileSync(cfg, JSON.stringify({
    channels: {
      items: [{
        source: 'display', display_index: 0,
        quality: 'custom', fps: 15, bitrate_kbps: 800, max_height: 720,
        sub_enabled: true, sub_height: 360, sub_bitrate_kbps: 300,
        rtsp_path: '/ch2', rtsp_sub_path: '/ch2_sub',
      }],
    },
  }, null, 2), 'utf8');
  return cfg;
}

function startBackend(cfg, cam) {
  if (!fs.existsSync(EXE)) throw new Error(`没有 ${EXE}，先跑 scripts\\build.ps1`);
  const rec = path.join(outDir, 'channels-probe.mp4');
  fs.rmSync(rec, { force: true });
  const logFd = fs.openSync(path.join(outDir, 'channels-backend.log'), 'w');
  return spawn(EXE, [
    '--camera', cam,
    '--rtsp', '--rtsp-port', String(A.rtsp),
    '--http', '--http-port', String(A.port),
    '--web-root', path.join(ROOT, 'web', 'dist'),
    '--http-user', A.user, '--http-password', A.password,
    // 录一份，好让 `/info` 的「录像占用」那张表有东西可显示（通道 1 是
    // `channels-probe.mp4`，通道 2 是 `channels-probe-ch2.mp4`）。
    '--record', rec,
    // 只关 WS-Discovery，不关 ONVIF：3702 上还站着 Windows 自己的 WS-Discovery，
    // 两边一起抢组播时探针会收到一堆无关的 ProbeMatch，而这里根本不验发现。
    // （`--no-onvif` 会连带关 wsd，但它也会把 `/onvif/*` 一起关掉，没必要。）
    '--no-wsd',
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

async function getJson(url, cookie) {
  const r = await fetch(url, { headers: { Cookie: `d2o_session=${cookie}` } });
  if (!r.ok) throw new Error(`GET ${url} → ${r.status}`);
  return r.json();
}

/** 直接问后端要当前配置 —— 判"存进去了没有"必须问它，不能只看页面。 */
const getConfig = (cookie) => getJson(`${BASE}/api/config`, cookie);

// ---------------------------------------------------------------- 主流程
let backend = null, browser = null;
let exitCode = 1;

try {
  // ---- 0. 本机得有两个源，否则这个探针验不了 ---------------------------
  const cams = await run(EXE, ['--list-cameras']);
  const camNames = [...cams.out.matchAll(/^\s*#(\d+)\s+(.+?)(\s*（默认）)?\s*$/gm)]
    .map((m) => m[2].trim());
  if (!camNames.length) {
    console.error('[probe] 本机一个摄像头都没有，这个探针需要"摄像头 + 显示器"两个源。');
    console.error(`         --list-cameras 说：${(cams.out + cams.err).trim().slice(0, 300)}`);
    process.exit(2);
  }
  const cam = camNames[0];
  console.log(`[probe] 摄像头 = ${cam}`);

  const cfg = writeConfig();
  backend = startBackend(cfg, cam);
  if (!await waitHttp()) throw new Error('后端 40 秒内没起来（看 out/browser/channels-backend.log）');
  const cookie = await login();
  console.log(`[probe] 后端已就绪 ${BASE}（登录已开启，凭据 ${A.user}/***）`);

  // 后端起完之后先确认"真的有两路"。只有一路时下面的断言会以"没有选择器"
  // 的形式失败，而那个症状读起来像前端坏了 —— 在这里先说清楚是哪一边的问题。
  //
  // ⚠️ **必须轮询，不能在登录后立刻取一次。** `/api/status` 一拿到 cookie 就
  // 200 了，而通道是采集线程起来之后才填进 board 的（实测差 100 ms：10.040
  // 登录成功，10.127 才「捕获会话已建立」）。取一次的写法会稳定地读到空数组，
  // 于是这个探针永远在第一步就退出 —— 而它读起来像"多通道根本没建起来"。
  let startupChans = [];
  for (let i = 0; i < 60; i++) {
    const status = await getJson(`${BASE}/api/status`, cookie);
    startupChans = Array.isArray(status?.channels) ? status.channels : [];
    if (startupChans.length >= 2) break;
    await sleep(500);
  }
  console.log(`[probe] 后端报的通道：${startupChans
    .map((c) => `${c.index}:${c.source}/${c.state}`).join(' | ') || '（一个都没有）'}`);
  if (startupChans.length < 2) {
    console.error(`[probe] 后端只建起了 ${startupChans.length} 路 —— 这个探针要两路`
                  + '（摄像头 + 显示器 #0）。看 out/browser/channels-backend.log');
    process.exit(2);
  }

  browser = await chromium.launch({ channel: 'chrome', headless: A.headless });
  const ctx = await browser.newContext();
  await ctx.addCookies([{
    name: 'd2o_session', value: cookie, url: `${BASE}/`,
    httpOnly: true, sameSite: 'Lax',
  }]);
  const page = await ctx.newPage();
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));

  // `/ws/preview` 的每一条连接都记一笔：首帧（init）、之后的分片数与字节数、
  // 有没有关掉。**"只有一路在推"这条结论全靠这张表**。
  const socks = [];
  page.on('websocket', (ws) => {
    if (!/\/ws\/preview/.test(ws.url())) return;
    const rec = { url: ws.url(), init: 0, frags: 0, bytes: 0, closed: false, openedAt: Date.now() };
    socks.push(rec);
    ws.on('framereceived', (f) => {
      const p = f.payload;
      if (typeof p === 'string') return;   // 文本帧不是媒体数据
      // 第一条二进制帧是 ftyp+moov（init segment），之后才是分片。
      if (rec.init === 0) { rec.init = p.length; return; }
      rec.frags++;
      rec.bytes += p.length;
    });
    ws.on('close', () => { rec.closed = true; });
  });

  // ---- [1] 预览页的通道选择器 ------------------------------------------
  console.log('\n[1] 预览页的通道选择器');
  await page.goto(`${BASE}/`, { waitUntil: 'networkidle' });

  // 选择器要等 `/ws/stats` 推来第一帧 `channels[]` 才渲染得出来 —— 它不是
  // 从 `/api/config` 读的（那里根本没有"这台机器有几路"这个信息）。
  await page.locator('.channel-pick').waitFor({ state: 'visible', timeout: 30000 });
  const picks = page.locator('.channel-pick button');
  const nPicks = await picks.count();
  check(nPicks >= 2, `选择器里有 ${nPicks} 个通道按钮（至少 2 个）`);
  const pickLabels = await picks.allInnerTexts();
  console.log(`         按钮：${pickLabels.map((s) => s.trim()).join(' | ')}`);

  // 进页面会自动开始播（这一页存在的理由就是看画面）。没有的话补一下 ——
  // 不补的话下面所有媒体断言都会以"静止"的形式失败。
  const startBtn = page.locator('button', { hasText: '开始预览' });
  if (await startBtn.count() > 0) await startBtn.first().click();

  // 等第一条预览连接真的开始收分片。桌面完全静止时帧很稀（水印刷新每秒一帧），
  // 所以窗口给得宽一些，失败信息里写清楚"收到了什么"。
  const waitFrags = async (rec, min, ms) => {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
      if (rec && rec.frags >= min) return true;
      await sleep(250);
    }
    return false;
  };

  // 先进来的那条是通道 1。
  const t0 = Date.now();
  while (Date.now() - t0 < 30000 && socks.length === 0) await sleep(200);
  const first = socks[0];
  check(!!first, '页面上开了 `/ws/preview` 连接');
  check(await waitFrags(first, 3, 30000),
        `通道 1 的分片在到达（${first?.frags ?? 0} 块 ${first?.bytes ?? 0}B）`,
        '桌面完全静止 + 水印也关着时 DXGI 不产帧；这一段等 30 秒');
  check(!!first && first.init > 0, `通道 1 收到了 init 段（${first?.init ?? 0}B）`);

  const dims1 = await page.evaluate(() => {
    const v = document.querySelector('video');
    return v ? { w: v.videoWidth, h: v.videoHeight } : null;
  });
  check(!!dims1 && dims1.w > 0 && dims1.h > 0,
        `通道 1 解码出了画面（${dims1?.w}×${dims1?.h}）`);

  // ---- [2] 切到通道 2：只有新那一路在推 ---------------------------------
  console.log('\n[2] 切到通道 2');
  const beforeSwitch = { count: socks.length, frags: first?.frags ?? 0 };
  // 按下去之后前端会先关掉旧连接、再 POST、再开新的（顺序写在 PreviewView 里：
  // 反过来的话旧连接会收到一个**别的通道的 moov**，Chrome 会当场废掉整个
  // SourceBuffer，症状是切过去从此全黑）。
  await picks.nth(1).click();
  const t1 = Date.now();
  while (Date.now() - t1 < 30000 && socks.length < beforeSwitch.count + 1) await sleep(200);
  const second = socks[socks.length - 1];
  check(socks.length > beforeSwitch.count,
        `切换后开了一条新的预览连接（共 ${socks.length} 条）`);
  check(!!second && second !== first, '新连接是另一条（不是复用旧的那条）');
  check(!!first && first.closed, '切换时**旧的那条被关掉了**',
        '不关的话它就是第二条一直在推的流，而"一次只看一路"正是为了不做这件事');

  const fragsAfterSwitch = first?.frags ?? 0;
  await sleep(6000);
  check((first?.frags ?? 0) === fragsAfterSwitch,
        `切换之后旧那条**再没收到过任何分片**（停在 ${fragsAfterSwitch} 块）`,
        `实得 ${first?.frags ?? 0} 块 —— 变了就说明旧通道还在被推`);
  check(await waitFrags(second, 3, 30000),
        `通道 2 的分片在到达（${second?.frags ?? 0} 块 ${second?.bytes ?? 0}B）`);

  // **换的是真码流**：解码尺寸必须变成通道 2 的。init 缓存用错那一份时，
  // 这里会一直是通道 1 的尺寸（或者干脆没有画面）—— 而界面上什么错都不报。
  let dims2 = null;
  for (let i = 0; i < 60; i++) {
    dims2 = await page.evaluate(() => {
      const v = document.querySelector('video');
      return v ? { w: v.videoWidth, h: v.videoHeight } : null;
    });
    if (dims2 && dims1 && dims2.w > 0 && (dims2.w !== dims1.w || dims2.h !== dims1.h)) break;
    await sleep(500);
  }
  check(!!dims2 && dims2.w > 0 && dims2.h > 0 && !!dims1 &&
        (dims2.w !== dims1.w || dims2.h !== dims1.h),
        `切换后解码尺寸变成了通道 2 的（${dims1?.w}×${dims1?.h} → ${dims2?.w}×${dims2?.h}）`,
        '两路源的尺寸不同（通道 2 被配置钉成 max_height=720），所以"没变"就等于' +
        '新的 init 段没生效 —— 那种情况下画面是黑的，而接口一个错都不报');

  // 后端那一侧的真相也核一次：它报的活动通道就是 2。
  const pc = await getJson(`${BASE}/api/preview-channel`, cookie);
  check(pc.channel === 2, `后端记的活动通道也变成了 2（实得 ${pc.channel}）`);

  // ---- [3] 配置页：勾一个摄像头 → 发出 items[] --------------------------
  console.log('\n[3] 配置页的「通道」段');
  const posts = [];
  page.on('request', (req) => {
    if (req.method() === 'POST' && req.url().endsWith('/api/config')) {
      let body = null;
      try { body = req.postDataJSON(); } catch { body = req.postData(); }
      posts.push(body);
    }
  });

  await page.goto(`${BASE}/config#channels`, { waitUntil: 'networkidle' });
  await page.locator('#channels').waitFor({ state: 'visible', timeout: 20000 });

  // 「检测到的源」里两种都要有：显示器（名字是后端 `describe()` 给的）与摄像头。
  const rows = page.locator('#channels .source-row');
  const nRows = await rows.count();
  const rowText = await rows.allInnerTexts();
  check(nRows >= 2, `「检测到的源」列出了 ${nRows} 行（显示器 + 摄像头）`,
        rowText.map((s) => s.replace(/\s+/g, ' ').trim()).join(' || '));
  check(rowText.some((s) => /DISPLAY|显卡|Intel|AMD|NVIDIA/i.test(s)),
        '有显示器的行（名字来自后端 `describe()`）');
  check(rowText.some((s) => s.includes(cam)),
        `有摄像头的行（${cam}）`);

  // 3a. **`label()` 认得通道项** —— 这一条挡的是"后端报 `channels.items.2.fps`，
  //     界面上原样显示那个键名"。做法是故意造一个只有前端会拦下的错
  //     （把通道 2 的主码流路径改成通道 1 的），然后看横幅里写的是不是人话。
  const pathInput = page
    .locator('#channels .channel-item').first()
    .locator('label.field')
    .filter({ has: page.locator('.field-label', { hasText: '主码流路径' }) })
    .locator('input');
  const origPath = await pathInput.inputValue();
  await pathInput.fill('/live');
  await page.locator('button[type="submit"]').first().click();
  await sleep(900);
  const banner = page.locator('.banner').first();
  const bannerVisible = (await banner.count()) > 0 && (await banner.isVisible());
  const bannerText = bannerVisible ? (await banner.innerText()).trim() : '';
  check(/通道 2/.test(bannerText) && /路径/.test(bannerText),
        `校验横幅说了人话：「${bannerText.replace(/\s+/g, ' ').slice(0, 70)}」`,
        '写的是通道号和字段名（`channels.items.0.rtsp_path` 那个下标在界面上' +
        '对应的是**通道 2**，+2 这一下漏了就会指到不存在的一路）');
  check(posts.length === 0, '被拦下的这一次**没有**发出 POST /api/config');
  await pathInput.fill(origPath);

  // 3b. 勾一台摄像头：写一条 `items[]`。
  const camRow = page
    .locator('#channels .source-row')
    .filter({ hasText: cam })
    .first();
  const addBtn = camRow.locator('button');
  check((await addBtn.count()) > 0, `摄像头那一行有「加为通道」按钮`);
  const nItemsBefore = await page.locator('#channels .channel-item').count();
  await addBtn.first().click();
  await sleep(300);
  const nItemsAfter = await page.locator('#channels .channel-item').count();
  check(nItemsAfter === nItemsBefore + 1,
        `点一下多了一路（${nItemsBefore} → ${nItemsAfter}）`);

  await page.locator('button[type="submit"]').first().click();
  for (let i = 0; i < 80 && posts.length === 0; i++) await sleep(100);
  check(posts.length === 1, `点保存发出了 POST /api/config（${posts.length} 次）`);
  const sent = posts[posts.length - 1];
  const items = sent?.channels?.items;
  check(Array.isArray(items) && items.length === nItemsAfter,
        `请求体里 channels.items 有 ${nItemsAfter} 项（实得 ${Array.isArray(items) ? items.length : '不是数组'}）`);
  check(Array.isArray(items) && items.some((it) => it.source === 'webcam' && it.camera === cam),
        `其中一项是 source=webcam / camera=${cam}`,
        JSON.stringify(items));

  // 存进去了没有，问后端 —— 只看界面上多了一行是不够的。
  await sleep(900);
  const stored = await getConfig(cookie);
  const sItems = stored?.channels?.items ?? [];
  check(sItems.some((it) => it.source === 'webcam' && it.camera === cam),
        `GET /api/config 里读回来也有那一项（${sItems.length} 项）`);
  // 反面：显示器那一项**没被**摄像头的默认值污染。`display_index` 对摄像头项
  // 没有意义（默认 0），拿它认领会让"配了一路摄像头"顺手吞掉显示器 #0 ——
  // 而后端 `plan_channels` 里那条认领规则正是靠 `source == "display"` 挡住的。
  check(sItems.some((it) => it.source === 'display' && it.display_index === 0),
        '显示器的项原样还在（没被摄像头项顶掉）');

  // ---- [4] 设备信息页：锁死的三个字段 + 每通道一行 + 录像占用 ----------
  console.log('\n[4] 设备信息页');
  await page.goto(`${BASE}/info`, { waitUntil: 'networkidle' });

  const frozen = ['硬件 ID', '序列号', '设备 UUID'];
  for (const label of frozen) {
    const row = page.locator('.readonly-row').filter({ hasText: label }).first();
    const input = row.locator('input');
    const cnt = await input.count();
    // **判据是 `disabled` 属性**，不是"看起来灰"。样式可以改、类名可以改，
    // 而 `disabled` 是浏览器层面的事实：选中不了、改不了、也提交不出去。
    const st = cnt > 0
      ? await input.first().evaluate((el) => ({
          disabled: el.disabled,
          attr: el.hasAttribute('disabled'),
        }))
      : null;
    check(cnt > 0 && st?.disabled === true && st?.attr === true,
          `「${label}」是一个带 disabled 的输入框`,
          cnt === 0 ? '这一行里没有 <input>（是不是又变回 <code> 了？）' : JSON.stringify(st));
  }
  // 反面：可改的那几个**不该**是禁用输入框 —— 全页锁死也会让上面三条全绿。
  const modelRow = page.locator('.readonly-row').filter({ hasText: '型号' }).first();
  check((await modelRow.locator('input[disabled]').count()) === 0,
        '「型号」不是禁用输入框（可改的那几项没被一起锁掉）');

  // 每通道一行：这一条同时是 `types.ts` / `useStats.ts` / 后端三处同步的判据
  // —— `channels` 数组在 `normalizeStats` 里掉过一次的话，这张卡就是空的。
  const lines = page.locator('.channel-line');
  const nLines = await lines.count();
  check(nLines >= 2, `「通道」卡片里有 ${nLines} 路的行（至少 2 路）`,
        '空的就说明 `stats.channels` 没活到渲染那一刻（`useStats` 少一处就是这症状）');
  const lineText = (await lines.allInnerTexts()).join(' || ').replace(/\s+/g, ' ');
  console.log(`         ${lineText.slice(0, 300)}`);
  check(/通道 1/.test(lineText) && /通道 2/.test(lineText), '两路都有（通道 1 / 通道 2）');
  check(/rtsp:\/\//.test(lineText), '列出了 RTSP 地址');
  check(new RegExp(String(A.port)).test(lineText),
        `列出了 ONVIF 端口（通道 1 = ${A.port}）`);

  // 录像占用那张表：`--record` 开着，通道 1 的文件就该在表里。
  const recTable = page.locator('.stream-table');
  const recText = (await recTable.count()) > 0
    ? (await recTable.first().innerText()).replace(/\s+/g, ' ')
    : '';
  check((await recTable.count()) > 0, '有「录像占用」那张表');
  console.log(`         ${recText.slice(0, 200)}`);
  check(/channels-probe\.mp4/.test(recText),
        '表里有通道 1 的录像文件名（channels-probe.mp4）',
        '`record_file` 没传到界面上的话，用户就只能自己去目录里翻 —— ' +
        '而"别让人自己去发现"正是这一条的要求');
  check(/[0-9]/.test(recText) && /(B|KB|MB|GB)/.test(recText), '表里有当前大小');

  // 大小那一格**必须真的在动**。
  //
  // 只判"有个数字、带个单位"是不够的：界面里写死一句 `0 B` 照样过，而这一条
  // 要回答的正是"磁盘会不会满"。一个不动的数比没有这个数**更糟** —— 它看起来
  // 像个答案。所以判据是**两次数出来的字节**（后端每秒推一帧 `channels`，
  // 所以这一格自己会刷新，不用我们刷新页面）。
  //
  // 通道 1 是摄像头、恒定产帧，8 秒 ≈ 2 MB，远超 `formatBytes` 在 MB 档上的
  // 分辨率（0.1 MB），不会因为四舍五入而抖。
  const recRow = page.locator('.stream-table tbody tr', { hasText: 'channels-probe.mp4' });
  const sizeText = async () => (await recRow.first().locator('td').nth(2).innerText()).trim();
  const before = parseBytes(await sizeText());
  await sleep(8000);
  const after = parseBytes(await sizeText());
  check(before > 0 && after > before,
        `录像大小在涨（${await sizeText()}；8 秒前 ${before} B → 现在 ${after} B）`,
        '两次读数没有变大 —— 要么这个数是写死的，要么这一路根本没有在录。' +
        '两种情况用户都会看到"磁盘是安全的"，而磁盘不是。');

  await page.screenshot({ path: path.join(outDir, 'channels_probe_info.png'), fullPage: true });
  console.log(`[probe] 截图 ${path.join(outDir, 'channels_probe_info.png')}`);

  check(pageErrors.length === 0, '没有未捕获的页面异常',
        pageErrors.slice(0, 3).join('\n         '));

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
