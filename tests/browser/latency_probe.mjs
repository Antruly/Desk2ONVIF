// 端到端延时：**从 <video> 的像素里**读出"画面上那次变化是什么时候出现的"，
// 与它**真正发生在屏幕上的墙钟**相减。
//
// 为什么不能拿界面上那个数当判据：那个数正是本阶段要修的东西（陷阱 90 的第一面
// —— 旧算法报的是 `buffered.end − currentTime`，它的宾语是"我们手上数据的末尾"，
// 不是"画面的现在"，于是它稳定在 `TARGET_LATENCY` 上，怎么看都很合理）。拿被
// 被测对象自己报的数去验它自己，等于没验。所以这里另造一把尺子：一个**受控的
// 闪烁方块**（`tests\lat_flash.ps1`）按一张固定的时刻表翻面，每次翻面的墙钟记在
// 日志里；页面上用 `drawImage(<video>) + getImageData` 从像素里读出"翻面出现了"，
// 两者相减就是端到端真值。
//
// 三个数字的关系是 `真实延迟 = 分片滞后 + 缓冲余量`，所以断言分成两层：
//   ① 页面自报的「真实延迟」与像素量出来的值之差 < 门槛（默认 150 ms）
//      —— 这一条判的是**尺子诚不诚实**，换回旧算法它必须红。
//   ② 分片的到达节奏（WS 相邻两块的间隔）与像素延迟一起进摘要文件，
//      由 out\w-lat-mut-sweep.ps1 跨构建比较 —— 这一条判的是后端那个旋钮。
// 另有一条纵向断言（`realtime` 的真实延迟 < `quality`）也是跨构建比的，见摘要文件。
//
// ⚠️ **必须 headless。** 有头模式下 Chrome 的窗口会盖住方块所在的桌面，
// DXGI 抓到的就是"Chrome 自己"，画面里根本没有方块（而且视频里会套娃）。
//
// 为什么时刻表是**不等距、而且周期长**的：对齐要靠残差最小。等距的话差整整 k
// 个周期给出完全一样的残差，于是量出来的延迟会随对齐方式跳一个周期 —— 而它
// 看起来只是个偏大或偏小的正常数。**短周期的不等距只解决一半**：任何周期为 P
// 的时刻表，错 P 个翻面都给出完全相同的残差（实测周期 4 时 k0=0 与 k0=4 都是
// 103ms，而真值那一组是 19ms —— 光看 sd 分不出来，因为错的那一组也是"完美拟合"，
// 只是整体平移了 2 秒）。所以周期取 11，而一次测量只看得到 20 来个翻面 ⇒
// 那个孪生解落不进搜索范围，"残差最小"就唯一对应真值。
//
// 另外两处**量出来才加的**前提，缺一个对齐就崩（详见各自函数上的注释）：
// 采样在 `readyState < 2` 时直接跳过（没有当前帧时 `drawImage` 画黑，会被当成
// 真黑），以及跨播放头跳变的那几次观测要剔除（连上时页面会 seek 到直播边缘）。

import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

// ---------------------------------------------------------------- 参数
function args() {
  const a = process.argv.slice(2);
  const o = {
    port: 8139, rtsp: 8140,
    seconds: 24,          // 采样窗口
    interval: 10,         // 两次采样之间的等待（毫秒）；实际节拍还要加上 CDP 往返
    stimExtra: 12,        // 刺激源比采样窗口多跑这么久，免得收尾时它已经停了
    delay: 0,             // 起后端之后等这么久再接浏览器（测"中途连上"）
    tier: 'balanced',
    tag: '',
    user: 'admin', password: 'admin',
    pageTol: 150,         // ① 的门槛（毫秒）
    expectFirstFragmentMs: 0, // >0 时判"连上之后第一块分片多久到"
    headless: true,
    keep: false,
    dump: false,          // 把原始采样与对齐候选全写出来（对齐说不清的时候用）
  };
  for (let i = 0; i < a.length; i++) {
    if (a[i] === '--port') o.port = +a[++i];
    else if (a[i] === '--rtsp') o.rtsp = +a[++i];
    else if (a[i] === '--seconds') o.seconds = +a[++i];
    else if (a[i] === '--interval') o.interval = +a[++i];
    else if (a[i] === '--stim-extra') o.stimExtra = +a[++i];
    else if (a[i] === '--delay') o.delay = +a[++i];
    else if (a[i] === '--tier') o.tier = a[++i];
    else if (a[i] === '--tag') o.tag = a[++i];
    else if (a[i] === '--user') o.user = a[++i];
    else if (a[i] === '--password') o.password = a[++i];
    else if (a[i] === '--page-tol') o.pageTol = +a[++i];
    else if (a[i] === '--expect-first-fragment-ms') o.expectFirstFragmentMs = +a[++i];
    else if (a[i] === '--headed') o.headless = false;
    else if (a[i] === '--keep') o.keep = true;
    else if (a[i] === '--dump') o.dump = true;
  }
  return o;
}
const A = args();
const TAG = A.tag || A.tier;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const outDir = path.join(ROOT, 'out', 'browser', 'latency');
fs.mkdirSync(outDir, { recursive: true });
const stimLog = path.join(outDir, `flash-${TAG}.log`);
const stimReady = path.join(outDir, `flash-${TAG}.ready`);

for (const p of [stimLog, stimReady]) {
  try { fs.rmSync(p, { force: true }); } catch {}
}

// ---------------------------------------------------------------- 刺激源
// 方块在**主屏正中**按固定时刻表翻面，每次翻面的墙钟记进 stimLog，
// 第一次翻面时写 stimReady（探针据此知道"窗口已经画出来了"）。
function startStimulus() {
  const script = path.join(ROOT, 'tests', 'lat_flash.ps1');
  if (!fs.existsSync(script)) throw new Error(`没有 ${script}`);
  return spawn('powershell.exe', [
    '-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', script,
    '-Seconds', String(A.seconds + A.stimExtra),
    '-Log', stimLog,
    '-Ready', stimReady,
  ], { cwd: ROOT, windowsHide: true, stdio: 'ignore' });
}

async function waitReady(timeoutMs = 30000) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    if (fs.existsSync(stimReady)) {
      const n = parseFlips().length;
      if (n >= 1) return true;
    }
    await sleep(100);
  }
  return false;
}

// ---------------------------------------------------------------- 后端
async function startBackend() {
  const exe = path.join(ROOT, 'build', 'bin', 'desk2onvif.exe');
  if (!fs.existsSync(exe)) throw new Error(`没有 ${exe}，先跑 scripts\\build.ps1`);
  const webRoot = path.join(ROOT, 'web', 'dist');
  // 用临时配置：绝不动用户 %APPDATA% 里那份。**路径必须 ASCII** ——
  // 非 ASCII 的 --config 会让进程直接中止（见 out\finding-nonascii-config-abort.md）。
  const cfg = path.join(outDir, `cfg-${TAG}.json`);
  fs.rmSync(cfg, { force: true });
  const logFd = fs.openSync(path.join(outDir, `backend-${TAG}.log`), 'w');
  const p = spawn(exe, [
    '--rtsp', '--rtsp-port', String(A.rtsp),
    '--http', '--http-port', String(A.port),
    '--web-root', webRoot,
    '--config', cfg,
    '--latency', A.tier,
  ], { cwd: ROOT, stdio: ['ignore', logFd, logFd] });
  return p;
}

function killTree(p) {
  if (!p || p.exitCode !== null) return;
  try { spawn('taskkill', ['/F', '/T', '/PID', String(p.pid)], { stdio: 'ignore' }); } catch {}
}

async function waitHttp(port, timeoutMs = 40000) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    try {
      // 问 /api/session 而不是 /api/status —— 后者上了登录之后恒为 401。
      const r = await fetch(`http://127.0.0.1:${port}/api/session`);
      if (r.ok) return true;
    } catch {}
    await sleep(400);
  }
  return false;
}

async function login(port, user, password) {
  const r = await fetch(`http://127.0.0.1:${port}/api/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ user, password }),
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

// ---------------------------------------------------------------- 化验
/** 刺激源日志：每行 `序号 墙钟毫秒 b|w`。 */
function parseFlips() {
  let txt = '';
  try { txt = fs.readFileSync(stimLog, 'utf8'); } catch { return []; }
  const out = [];
  for (const line of txt.split(/\r?\n/)) {
    const m = line.trim().match(/^(\d+)\s+(\d+)\s+([bw])$/);
    if (m) out.push({ i: +m[1], ms: +m[2], s: m[3] });
  }
  return out;
}

/** 像素里读到的翻面：类 0 = 黑、1 = 白、2 = 不确定（背景灰正是这一类）。
 *  时刻取"旧色的最后一个采样"与"新色的第一个采样"的中点 —— 变化的真值落在
 *  这个区间里，取中点是无偏的（取右端则系统性偏大半个采样周期）。
 *
 *  **跳变要剔除，这一条是量出来的。** 页面连上时会把播放头对到直播边缘
 *  （实测 `currentTime` 在 0.5 秒内从 0 跳到 4.93 —— 那一下画面上换掉的是
 *  好几秒的内容），跨越这一刻的"翻面"不是"内容到得早晚"的证据，而是播放器
 *  自己挪了一下。真值也对得上：不剔除时 22 次观测里有 2 次是这种伪翻面，
 *  它们把对齐搜索从正确的 k0 上拽走（见 summary 里 `jumpDropped` 那个数）。
 *  判据用播放头与墙钟是否同步推进 —— 它同时也是"这段流有没有卡顿"的定义。 */
function observedTransitions(samples) {
  const out = [];
  let lastIdx = -1, lastCls = -1, dropped = 0;
  for (let i = 0; i < samples.length; i++) {
    const cls = samples[i][3];
    if (cls === 2) continue;
    if (lastIdx >= 0 && cls !== lastCls) {
      const dt = samples[i][0] - samples[lastIdx][0];
      const dct = samples[i][1] - samples[lastIdx][1];
      if (Math.abs(dct - dt) <= 250) {
        out.push({
          t: Math.round((samples[lastIdx][0] + samples[i][0]) / 2),
          cls, a: samples[lastIdx][0], b: samples[i][0],
        });
      } else {
        dropped++;
      }
    }
    lastIdx = i; lastCls = cls;
  }
  out.dropped = dropped;
  return out;
}

/** 把观察到的翻面序列对齐到刺激源序列上，返回残差最小的那个偏移。
 *
 *  **两个判据缺一不可**：
 *  1. `d > 0` —— 内容不可能在它产生之前就被看见。这是物理定律，不是调参；
 *     它一次砍掉一半的候选（k0 偏大的那一半）。
 *  2. 残差 sd 最小。时刻表取周期 11（见 `tests\lat_flash.ps1` 的文件头），
 *     而一次测量里观测到的翻面只有 20 来个 ⇒ "平移一整个周期"的孪生解
 *     （k0±11）落不进搜索范围，所以唯一的 sd 最小点就是真值。
 *     周期 4 的时候做不到这一点 —— 那时 k0 与 k0+4 的残差 sd **完全相同**
 *     （实测 103ms vs 103ms，而真值那一组是 19ms），光看 sd 分不出来。
 *
 *  `secondSd` 是第二名（不含胜出者）的残差 sd。"对齐唯一"那条断言就比它：
 *  胜出的 sd 必须不到第二名的**一半**，否则说明这组观测根本区分不出两个偏移，
 *  此时测量出的延迟偏移一整个周期也是可能的 —— 那种数不能拿去判产品。 */
function align(trans, flips) {
  const N = trans.length;
  if (N < 4) return null;
  let best = null, second = null;
  const all = [];
  for (let k0 = 0; k0 + N <= flips.length; k0++) {
    let sum = 0, sum2 = 0, ok = true;
    for (let j = 0; j < N; j++) {
      const d = trans[j].t - flips[k0 + j].ms;
      if (!(d > 0 && d < 15000)) { ok = false; break; }
      sum += d; sum2 += d * d;
    }
    if (!ok) continue;
    const mean = sum / N;
    const sd = Math.sqrt(Math.max(0, sum2 / N - mean * mean));
    const cand = { k0, mean, sd, d: trans.map((x, j) => x.t - flips[k0 + j].ms) };
    all.push({ k0, mean, sd, d: cand.d });
    if (best === null || sd < best.sd) { second = best; best = cand; }
    else if (second === null || sd < second.sd) { second = cand; }
  }
  if (best === null) return null;
  best.secondSd = second ? second.sd : null;
  best.all = all;
  return best;
}

const mean = (v) => (v.length ? v.reduce((a, b) => a + b, 0) / v.length : null);
const median = (v) => {
  if (!v.length) return null;
  const s = [...v].sort((a, b) => a - b);
  const h = Math.floor(s.length / 2);
  return s.length % 2 ? s[h] : (s[h - 1] + s[h]) / 2;
};

// ---------------------------------------------------------------- 主流程
let backend, browser, stimulus;
const fail = [];
const ok = (cond, what, detail = '') => {
  console.log(`  [${cond ? 'OK' : '!!'}] ${what}`);
  if (detail) console.log(`        ${detail}`);
  if (!cond) fail.push(what);
};

const summary = {
  tag: TAG, tier: A.tier, seconds: A.seconds,
  meanPixelMs: null, meanPageMs: null, sdPixelMs: null,
  fragCount: 0, fragMeanGapMs: null, firstFragmentMs: null,
  firstPictureMs: null, playheadStart: null, playheadEnd: null,
  flips: 0, observed: 0, alignK0: null, alignSecondSd: null,
  appendErrors: 0, maxReadyState: 0, videoW: 0, videoH: 0, ok: false,
};

try {
  console.log(`[lat] 起后端 :${A.port}（RTSP ${A.rtsp}，档位 ${A.tier}）`);
  backend = await startBackend();
  if (!(await waitHttp(A.port))) throw new Error('后端起不来，看 out/browser/latency/backend-*.log');
  console.log('[lat] 后端就绪');

  let cookieVal = null;
  {
    const r = await fetch(`http://127.0.0.1:${A.port}/api/session`);
    const sess = r.ok ? await r.json() : null;
    if (sess && sess.auth_required) {
      cookieVal = await login(A.port, A.user, A.password);
      const chk = await fetch(`http://127.0.0.1:${A.port}/api/status`,
                              { headers: { Cookie: `d2o_session=${cookieVal}` } });
      if (!chk.ok) throw new Error(`拿到 cookie 但 /api/status → ${chk.status}`);
      console.log(`[lat] 已登录（user=${A.user}）`);
    }
  }

  if (A.delay > 0) {
    console.log(`[lat] 故意等 ${A.delay}s 再开浏览器（中途连上的形状）`);
    await sleep(A.delay * 1000);
  }

  // 刺激源必须在浏览器之前起来：DXGI 一旦抓到"桌面静止"就不产帧，
  // 而方块就是让它动起来的那件事。
  console.log('[lat] 起刺激源（主屏正中的闪烁方块）');
  stimulus = startStimulus();
  if (!(await waitReady())) throw new Error('刺激源没有就绪（out/browser/latency/flash-*.log）');
  console.log('[lat] 刺激源就绪，方块已在翻面');

  browser = await chromium.launch({ channel: 'chrome', headless: A.headless });
  if (!A.headless) {
    console.log('[lat] ⚠️ --headed：Chrome 窗口会盖住方块，量出来的是 Chrome 自己');
  }
  const context = await browser.newContext();
  if (cookieVal) {
    await context.addCookies([{
      name: 'd2o_session', value: cookieVal,
      url: `http://127.0.0.1:${A.port}/`, httpOnly: true, sameSite: 'Lax',
    }]);
  }
  const page = await context.newPage();

  // 插桩：MSE 的调用与异常、以及 WS 上到底收到了什么。前端的 try/catch 会把
  // 异常吃掉，但吃之前它总得调用这些 API。
  await page.addInitScript(() => {
    window.__d2o = { ms: [], sb: [], errors: [] };
    const E = window.__d2o.errors;
    const origMS = window.MediaSource;
    if (origMS) {
      const origAdd = origMS.prototype.addSourceBuffer;
      origMS.prototype.addSourceBuffer = function (mime) {
        const rec = { mime, ok: null, err: null };
        window.__d2o.ms.push(rec);
        try { const sb = origAdd.call(this, mime); rec.ok = true; return sb; }
        catch (e) { rec.ok = false; rec.err = String(e); E.push(`addSourceBuffer(${mime}) 抛了：${e}`); throw e; }
      };
    }
    const SBP = window.SourceBuffer;
    if (SBP) {
      const origApp = SBP.prototype.appendBuffer;
      SBP.prototype.appendBuffer = function (buf) {
        const n = buf?.byteLength ?? buf?.length ?? -1;
        const rec = { n, err: null };
        window.__d2o.sb.push(rec);
        try { return origApp.call(this, buf); }
        catch (e) { rec.err = String(e); E.push(`appendBuffer(${n}B) 抛了：${e}`); throw e; }
      };
    }

    // **"画面第一次真的有了内容"这件事只能从页面内部量。** 探针在 Node 侧的采样
    // 循环要等播放态出来才开始，那时首帧早就过了；而"中途连上预览要多久才出画"
    // 正是这一档要改善的两件事之一（另一件是 `gop_seconds`，见 `--expect-first-fragment-ms`）。
    // `<video>` 是 Vue 挂上去的，所以从 addInitScript 起一直 rAF 轮询等它出现。
    window.__d2o.firstDataAt = 0;   // readyState >= 2：手上有一帧可画了
    window.__d2o.firstMovedAt = 0;  // currentTime > 0：播放头真的动起来了
    const tick = () => {
      const v = document.querySelector('video');
      if (v) {
        const t = Date.now();
        if (!window.__d2o.firstDataAt && v.readyState >= 2) window.__d2o.firstDataAt = t;
        if (!window.__d2o.firstMovedAt && v.currentTime > 0) window.__d2o.firstMovedAt = t;
      }
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  });

  const wsInfo = {
    openAt: 0, openAtAny: 0, initAt: 0, initBytes: 0, firstFragAt: 0, firstFragBytes: 0,
    firstFragHead: '', fragAt: [], textFrames: [], t0: null, closeAt: 0,
    otherSockets: 0, statsTextFrames: 0,
  };
  page.on('websocket', (ws) => {
    // ⚠️ 页面上有**两条** WS：`/ws/preview`（要量的那条）和 `/ws/stats`（状态推送，
    // 每秒一帧 JSON）。早先这里写的是"页面上只有一条 WS，所以第一条就是它" ——
    // 那是**推出来的**，而且推错了（第一次跑就看见 `textFrames` 里躺着两帧 stats
    // 的 JSON）。它不只是"多收了两帧"那么轻：`openAt` 是 `firstFragmentMs` 与
    // `firstPictureMs` 的**原点**，而谁先连上取决于两个组件谁先 mount、两条握手
    // 谁先完成 —— 拿了 stats 那条的原点，这两个数会**一起偏大**，而看起来只是
    // "稍微慢一点"（陷阱 90 的第一面：宾语错了，数看着很合理）。所以按 URL 认，
    // 不按顺序认。`openAtAny` 留着做诊断：它能回答"两条谁先连上"，而那正是
    // 这段代码原先在赌的那件事。
    if (!wsInfo.openAtAny) wsInfo.openAtAny = Date.now();
    if (!ws.url().includes('/ws/preview')) {
      wsInfo.otherSockets++;
      ws.on('framereceived', (f) => { if (typeof f.payload === 'string') wsInfo.statsTextFrames++; });
      return;
    }
    if (!wsInfo.openAt) wsInfo.openAt = Date.now();
    ws.on('framereceived', (f) => {
      const now = Date.now();
      const p = f.payload;
      if (typeof p === 'string') {
        // t0 那一帧文本。**分不出文本和二进制就是一个 InvalidStateError，
        // 而症状是"预览黑屏"** —— 所以这里必须分辨。
        wsInfo.textFrames.push(p.slice(0, 200));
        try {
          const o = JSON.parse(p);
          if (o && o.type === 't0') wsInfo.t0 = o;
        } catch {}
        return;
      }
      const head = Buffer.from(p.subarray(4, 8)).toString('latin1');
      if (head === 'ftyp') {
        if (!wsInfo.initAt) { wsInfo.initAt = now; wsInfo.initBytes = p.length; }
      } else {
        if (!wsInfo.firstFragAt) {
          wsInfo.firstFragAt = now;
          wsInfo.firstFragBytes = p.length;
          wsInfo.firstFragHead = head;
        }
        wsInfo.fragAt.push(now);
      }
    });
    ws.on('close', () => { if (!wsInfo.closeAt) wsInfo.closeAt = Date.now(); });
  });

  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));

  console.log(`[lat] 打开 http://127.0.0.1:${A.port}/preview`);
  await page.goto(`http://127.0.0.1:${A.port}/preview`, { waitUntil: 'domcontentloaded' });

  // 这一页进页面就自动开播（onMounted → start()），所以正常情况下不用点。
  // 但要给它几秒；没起来就找那个「开始预览」按钮点一下（自动播放被拦时的退路）。
  let overlayUp = false;
  for (let i = 0; i < 30; i++) {
    overlayUp = await page.evaluate(() => !!document.querySelector('.stage-overlay .chip'));
    if (overlayUp) break;
    await sleep(500);
  }
  if (!overlayUp) {
    const clicked = await page.evaluate(() => {
      const bs = [...document.querySelectorAll('button')];
      const b = bs.find((x) => /开始预览|开始|预览/.test(x.textContent || ''));
      if (!b) return '';
      b.click();
      return (b.textContent || '').trim();
    });
    console.log(`[lat] 叠层还没出来，点了「${clicked}」`);
    for (let i = 0; i < 20 && !overlayUp; i++) {
      await sleep(500);
      overlayUp = await page.evaluate(() => !!document.querySelector('.stage-overlay .chip'));
    }
  }
  ok(overlayUp, '预览进入播放态（页面上出现了三个读数）');

  // 装采样器。尺寸在第一次拿到 videoWidth 时按比例定（编码尺寸可能被
  // max_height 缩过，写死会和实际帧对不上）。
  const installed = await page.evaluate(() => {
    const v = document.querySelector('video');
    if (!v) return '没有 <video>';
    const cv = document.createElement('canvas');
    const ctx = cv.getContext('2d', { willReadFrequently: true });
    const st = { vw: 0, vh: 0, patch: 0, err: '' };
    st.sample = function () {
      if (!v.videoWidth) return null;
      // `readyState < 2` 时视频还没有当前帧，`drawImage` 画出来是**黑**的 ——
      // 那会被后面的死区判成"确凿的黑"，于是"还没有数据"变成一个货真价实的
      // 翻面。实测正是它和播放头的初始跳变一起造出了两个伪翻面。
      if (v.readyState < 2) return null;
      if (st.vw !== v.videoWidth) {
        st.vw = v.videoWidth; st.vh = v.videoHeight;
        cv.width = 480;
        cv.height = Math.max(2, Math.round((480 * st.vh) / st.vw));
        // 方块是 min(桌面宽,高) 的 1/4，居中；取它的 40% 见方做采样区，
        // 稳稳落在方块内部（离边缘还有 30% 的余量，躲开压缩振铃）。
        const sq = Math.min(cv.width, cv.height) * 0.25;
        st.patch = Math.max(8, Math.round(sq * 0.4));
      }
      const pw = st.patch, ph = st.patch;
      const x = Math.round((cv.width - pw) / 2), y = Math.round((cv.height - ph) / 2);
      let lum = -1;
      try {
        ctx.drawImage(v, 0, 0, cv.width, cv.height);
        const d = ctx.getImageData(x, y, pw, ph).data;
        let sum = 0;
        for (let i = 0; i < d.length; i += 4) {
          sum += 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
        }
        lum = Math.round(sum / (d.length / 4));
      } catch (e) { st.err = String(e); return null; }
      // 死区：背景是 (96,96,96) 的灰（亮度 96），**故意让它落进"不确定"**，
      // 于是"方块根本没被相机看到"会表现为"一个翻面都没有"，而不是一堆假翻面。
      const cls = lum >= 150 ? 1 : (lum <= 60 ? 0 : 2);
      const chip = (prefix) => {
        for (const el of document.querySelectorAll('.stage-overlay .chip')) {
          const t = (el.textContent || '').trim();
          if (t.startsWith(prefix)) {
            const m = t.match(/(-?\d+(?:\.\d+)?)\s*s/);
            return m ? Math.round(parseFloat(m[1]) * 1000) : -1;
          }
        }
        return -1;
      };
      return [
        Date.now(),
        Math.round(v.currentTime * 1000),
        lum, cls,
        chip('真实延迟'), chip('分片滞后'), chip('缓冲'),
        v.readyState,
      ];
    };
    window.__lat = st;
    return '';
  });
  if (installed) throw new Error(`装采样器失败：${installed}`);

  console.log(`[lat] 采样 ${A.seconds}s（间隔约 ${A.interval}ms）`);
  const samples = [];
  const tEnd = Date.now() + A.seconds * 1000;
  while (Date.now() < tEnd) {
    let s = null;
    try { s = await page.evaluate(() => window.__lat.sample()); } catch {}
    if (s) samples.push(s);
    await sleep(A.interval);
  }

  const diag = await page.evaluate(() => {
    const v = document.querySelector('video');
    const d = window.__d2o || {};
    return {
      hasVideo: !!v,
      vw: v?.videoWidth ?? 0, vh: v?.videoHeight ?? 0,
      readyState: v?.readyState ?? 0,
      paused: v?.paused ?? null,
      currentTime: v ? +v.currentTime.toFixed(2) : null,
      ms: d.ms || [],
      appendCount: (d.sb || []).length,
      appendErrors: (d.sb || []).filter((x) => x.err).length,
      msErrors: d.errors || [],
      samplerErr: window.__lat?.err || '',
      patch: window.__lat?.patch || 0,
      firstDataAt: d.firstDataAt || 0,
      firstMovedAt: d.firstMovedAt || 0,
    };
  });

  console.log(`[lat] 采样 ${samples.length} 点；patch=${diag.patch}px，` +
              `video ${diag.vw}x${diag.vh} readyState=${diag.readyState} ` +
              `appendBuffer ${diag.appendCount} 次`);

  // ------------------------------------------------------------ 对账
  const flips = parseFlips();
  const trans = observedTransitions(samples);
  const al = align(trans, flips);

  const lumps = samples.map((s) => s[2]);

  // **两个均值必须量同一段时间，否则比出来的是启动瞬态。** 像素那一侧只在
  // "有可用翻面"的地方存在，所以窗口就取第一次到最后一次可用翻面的区间；页面
  // 那一侧同一区间取平均。不这么做的话，页面均值里会混进播放头还停在 0 的那
  // 一秒 —— 那时缓冲已经攒了几秒、`真实延迟` 读到 4200~5000ms，而那是
  // "还没开始播"，不是延迟（实测它把均值从 ~700ms 抬到 1494ms，于是断言 ①
  // 报出一个 1224ms 的差值，而逐次对账每一笔都是 620~680ms）。
  const winLo = trans.length ? trans[0].a : 0;
  const winHi = trans.length ? trans[trans.length - 1].b : Infinity;
  const inWin = (s) => s[0] >= winLo && s[0] <= winHi && s[7] === 4;
  const pageVals = samples.filter(inWin).map((s) => s[4]).filter((x) => x >= 0);
  const lagVals = samples.filter(inWin).map((s) => s[5]).filter((x) => x >= 0);
  const bufVals = samples.filter(inWin).map((s) => s[6]).filter((x) => x >= 0);
  const pageAllVals = samples.map((s) => s[4]).filter((x) => x >= 0);

  const pxMean = al ? al.mean : null;
  const pxSd = al ? al.sd : null;
  const pgMean = mean(pageVals);

  const fragGaps = [];
  for (let i = 1; i < wsInfo.fragAt.length; i++) fragGaps.push(wsInfo.fragAt[i] - wsInfo.fragAt[i - 1]);
  const firstFragMs = wsInfo.firstFragAt && wsInfo.openAt ? wsInfo.firstFragAt - wsInfo.openAt : null;
  const firstPictureMs = diag.firstDataAt && wsInfo.openAt ? diag.firstDataAt - wsInfo.openAt : null;
  const firstMovedMs = diag.firstMovedAt && wsInfo.openAt ? diag.firstMovedAt - wsInfo.openAt : null;

  Object.assign(summary, {
    meanPixelMs: pxMean === null ? null : Math.round(pxMean * 10) / 10,
    sdPixelMs: pxSd === null ? null : Math.round(pxSd * 10) / 10,
    meanPageMs: pgMean === null ? null : Math.round(pgMean * 10) / 10,
    meanPageAllMs: pageAllVals.length ? Math.round(mean(pageAllVals) * 10) / 10 : null,
    pageWindowSamples: pageVals.length,
    meanLagMs: lagVals.length ? Math.round(mean(lagVals) * 10) / 10 : null,
    meanBufMs: bufVals.length ? Math.round(mean(bufVals) * 10) / 10 : null,
    fragCount: wsInfo.fragAt.length,
    fragMeanGapMs: fragGaps.length ? Math.round(mean(fragGaps)) : null,
    firstFragmentMs: firstFragMs,
    firstPictureMs,
    firstMovedMs,
    firstFragmentBytes: wsInfo.firstFragBytes,
    firstFragmentHead: wsInfo.firstFragHead,
    initBytes: wsInfo.initBytes,
    t0Seen: !!wsInfo.t0,
    // 诊断：页面上另外那条 WS（`/ws/stats`）的存在感。`wsOpenSkewMs` = 两条连接
    // 建立时刻之差（负 = 预览那条先连上）。它一改，下面那两个 `first*Ms` 就换锚点。
    wsOtherSockets: wsInfo.otherSockets,
    wsStatsTextFrames: wsInfo.statsTextFrames,
    wsOpenSkewMs: wsInfo.openAtAny && wsInfo.openAt ? wsInfo.openAt - wsInfo.openAtAny : null,
    flips: flips.length,
    observed: trans.length,
    jumpDropped: trans.dropped || 0,
    alignK0: al ? al.k0 : null,
    alignSecondSd: al && al.secondSd !== null ? Math.round(al.secondSd * 10) / 10 : null,
    samples: samples.length,
    samplesPct: samples.length / (A.seconds * 1000 / (A.interval + 3)),
    lumMin: lumps.length ? Math.min(...lumps) : null,
    lumMax: lumps.length ? Math.max(...lumps) : null,
    appendCount: diag.appendCount,
    appendErrors: diag.appendErrors,
    maxReadyState: Math.max(0, ...samples.map((s) => s[7] || 0)),
    videoW: diag.vw, videoH: diag.vh,
    playheadStart: samples.length ? samples[0][1] : null,
    playheadEnd: samples.length ? samples[samples.length - 1][1] : null,
    pageErrors, msErrors: diag.msErrors,
    textFrames: wsInfo.textFrames.slice(0, 3),
  });

  if (A.dump) {
    const dumpPath = path.join(outDir, `dump-${TAG}.json`);
    fs.writeFileSync(dumpPath, JSON.stringify({
      tag: TAG, tier: A.tier, t0: wsInfo.t0, wsOpenAt: wsInfo.openAt,
      samples, trans, flips,
      alignAll: al ? al.all.map((c) => ({ k0: c.k0, mean: Math.round(c.mean), sd: Math.round(c.sd) })) : [],
      alignBest: al ? { k0: al.k0, mean: Math.round(al.mean), sd: Math.round(al.sd), d: al.d.map(Math.round) } : null,
      fragAt: wsInfo.fragAt,
    }, null, 1), 'utf8');
    console.log(`[lat] 原始数据 ${dumpPath}`);
  }

  console.log('\n=== 延时结论 ===');
  console.log(`  刺激源翻面 ${flips.length} 次，像素里认出 ${trans.length} 次翻面` +
              `（另有 ${trans.dropped || 0} 次跨播放头跳变，已剔除）`);
  if (al) {
    console.log(`  对齐 k0=${al.k0}（残差 sd=${pxSd.toFixed(1)}ms，` +
                `次优 sd=${al.secondSd === null ? '—' : al.secondSd.toFixed(1) + 'ms'}）`);
    console.log(`  像素量出的真实延迟  mean=${pxMean.toFixed(1)}ms`);
  }
  console.log(`  页面自报的真实延迟  mean=${pgMean === null ? '—' : pgMean.toFixed(1) + 'ms'}` +
              `（同一区间 ${summary.pageWindowSamples} 点；分片滞后 ${summary.meanLagMs}ms ` +
              `+ 缓冲 ${summary.meanBufMs}ms）`);
  console.log(`  全程均值（含起播瞬态，只作诊断）= ${summary.meanPageAllMs}ms`);
  console.log(`  WS：初始化段 ${wsInfo.initBytes}B，分片 ${wsInfo.fragAt.length} 块，` +
              `相邻间隔均值 ${summary.fragMeanGapMs}ms，` +
              `连上到第一块分片 ${firstFragMs}ms` +
              `（另有一条 WS：文本帧 ${wsInfo.statsTextFrames} 帧，` +
              `两条连接建立时刻相差 ${summary.wsOpenSkewMs}ms，负 = 预览先连上）`);
  console.log(`  预览起播：连上到画面有内容 ${firstPictureMs}ms，` +
              `播放头真的动了 ${firstMovedMs}ms` +
              `（两者都由页面内部量 —— 探针在 Node 侧的采样循环那时候还没开始跑）`);

  ok(!!wsInfo.t0, 'WS 上收到了 t0（后端把"媒体 0 点的墙钟"送到了页面）',
     wsInfo.t0 ? JSON.stringify(wsInfo.t0) : `文本帧：${JSON.stringify(wsInfo.textFrames.slice(0, 2))}`);
  ok(diag.appendCount > 0 && diag.appendErrors === 0,
     `appendBuffer 没抛异常（共 ${diag.appendCount} 次）`,
     diag.msErrors.slice(0, 3).join(' | '));
  ok(summary.maxReadyState >= 4, `readyState 到过 4（实得 ${summary.maxReadyState}）`);
  ok(diag.vw > 0 && diag.vh > 0, `videoWidth 有值（${diag.vw}x${diag.vh}）`);
  ok(summary.playheadEnd > summary.playheadStart,
     `播放位置在前进（${summary.playheadStart}ms → ${summary.playheadEnd}ms）`);
  ok(diag.samplerErr === '', '像素采样没有抛异常', diag.samplerErr);
  ok(trans.length >= 8, `像素里认出至少 8 次翻面（实得 ${trans.length}）`,
     `亮度范围 ${summary.lumMin}~${summary.lumMax}（背景灰是 96，方块是 0 或 255）`);
  ok(summary.lumMax !== null && summary.lumMin !== null &&
     summary.lumMax - summary.lumMin > 100,
     `采样区确实在方块里（亮度跨度 ${summary.lumMax - summary.lumMin}）`,
     '跨度小说明采到的是背景 —— 方块多半被别的窗口盖住了');
  ok(al !== null, '翻面序列能对齐到刺激源时刻表');
  if (al) {
    ok(al.sd <= 80, `对齐残差够小（sd=${al.sd.toFixed(1)}ms ≤ 80ms）`);
    ok(al.secondSd !== null && al.sd * 2 < al.secondSd,
       `对齐唯一（残差 sd 不到第二名的一半：` +
       `${al.sd.toFixed(1)} vs ${al.secondSd === null ? '—' : al.secondSd.toFixed(1)}ms）`,
       `时刻表周期取 11（330/590/…/540），而一次测量只看得到 20 来个翻面，` +
       `所以"平移一整个周期"的孪生解落不进搜索范围 —— 不唯一就说明这组观测` +
       `区分不出两个偏移，量出来的数不能拿去判产品`);
    ok(pxMean > 0 && pxMean < 10000, `像素延迟落在合理区间（${pxMean.toFixed(1)}ms）`);
  }
  ok(pgMean !== null, '页面报出了真实延迟（不是「—」）');
  if (al && pgMean !== null) {
    const diff = Math.abs(pxMean - pgMean);
    ok(diff < A.pageTol,
       `① 页面自报的真实延迟与像素量出的值之差 ${diff.toFixed(1)}ms < ${A.pageTol}ms`,
       `像素 ${pxMean.toFixed(1)}ms vs 页面 ${pgMean.toFixed(1)}ms`);
  }
  if (pgMean !== null && summary.meanLagMs !== null && summary.meanBufMs !== null) {
    const d = Math.abs(pgMean - (summary.meanLagMs + summary.meanBufMs));
    ok(d < 250, `三个读数自洽：真实延迟 ≈ 分片滞后 + 缓冲余量（差 ${d}ms）`);
  }
  // ①b：**① 在 balanced / realtime 档上区分不出新旧公式**，这一条补上那个缺口。
  //
  // ① 拿"页面自报值"和"像素量出的值"比，容差 150ms；而旧公式（`buffered.end −
  // currentTime`）报出来的数**恰好就是「缓冲余量」那一项**，两者之差正好等于
  // 「分片滞后」。实测：balanced 档分片滞后 106ms < 150ms ⇒ 换成旧公式 ① 照样绿
  // （实测页面上那个数从 700 掉到 552，差值 137.2ms，**通过**）。也就是说 ①
  // 只在"分片滞后 > 容差"的档位上才有鉴别力 —— 这是陷阱 87 的形状：判据依赖了
  // 一个没写进前提的环境条件（这里的"环境"是档位）。
  //
  // 真正与档位无关的判据是**两个 chip 是不是同一个数**：新公式下
  // `真实延迟 = 分片滞后 + 缓冲余量`，所以它**恒大于**「缓冲余量」；旧公式下两者
  // 逐位相同（`total = d * 1000`，`buffered = d`）。实测差值：新公式
  // realtime 24.0 / balanced 106.0 / quality 210.6ms，旧公式 **0.0ms**（精确）。
  // 判据只用"严格大于 + 一个几毫秒的 epsilon"，不设下限 —— 下限会重新把这个判据
  // 挂到"这台机器的分片滞后有多大"上，那正是要躲开的东西。
  if (pgMean !== null && summary.meanBufMs !== null) {
    const gap = pgMean - summary.meanBufMs;
    ok(gap > 5,
       `①b 「真实延迟」和「缓冲余量」是两个数（差 ${gap.toFixed(1)}ms > 5ms）`,
       `两个 chip 相等（${pgMean} vs ${summary.meanBufMs}）说明页面报的还是旧公式 ` +
       `「buffered.end − currentTime」—— 它稳定在追帧目标上，和链路好坏无关`);
  }
  // ②：**分片节奏**这一条是后端 (b) 那一半的直接量。
  //
  // 计划里 ② 写的是"开/关 `frag_custom` 两个构建量出的延迟有可分辨的差"——
  // 那是**跨构建**的比较，一次运行里没有参照物，所以它判不出来。但 (b) 真正
  // 建立的性质在一次运行里就量得到，而且**与档位无关**：主动切分让分片按
  // `fps/4` 帧一刀切（≈240ms），**不再等关键帧**。于是"相邻分片间隔"这个数
  // 在开着时是 ~250ms（三个档实测 208 / 222 / 229ms），关掉时直接变成该档的
  // GOP（balanced 档 gop=2s 实测 **2001ms**）。而 GOP 最短的那一档是 realtime
  // 的 1 秒 —— 所以 600ms 这个门槛在**每一档**上都分得开，不挂在档位上。
  //
  // ⚠️ 变异证据（陷阱 85）：`capture_session.cpp:327` 加 `false &&` 关掉主动
  // 切分、重编 —— 这一条红（2001ms），同时 `meanPixelMs` 从 696 涨到 2171、
  // 分片数从 92 掉到 11、连上到第一块分片从 270 涨到 1208ms。**但 ① 和 ①b
  // 都照样绿**（它们量的是"页面那个数诚不诚实"，与分片节奏无关）—— 也就是说
  // 没有这一条的话，(b) 整个可以一行不生效而探针全绿。
  {
    const gap = summary.fragMeanGapMs;
    ok(gap !== null && gap < 600,
       `② 分片节奏不跟着关键帧走（相邻间隔均值 ${gap}ms < 600ms，${summary.fragCount} 块）`,
       `600ms 以上说明分片又只在关键帧处切了（` +
       `balanced 档 gop=2s 实测 2001ms）—— 后端那次主动 flush 没生效`);
    ok(firstFragMs !== null && firstFragMs < 900,
       `②b 连上之后第一块分片来得快（${firstFragMs}ms < 900ms）`,
       `关掉主动切分时它等于"等到下一个关键帧"，两次变异实测 1208 / 1434ms` +
       `（balanced/gop=2s）。**注意这一条不是每次都红**：变异时这个数等于"连上的` +
       `那一刻离下一个关键帧还有多远"，而那是随机的 —— 恰好贴着关键帧连上就会` +
       `量出一个小数。**真正稳的是 ②**（它把整段 92 块分片的间隔都平均了：开着` +
       `222ms、关掉 2001ms，9 倍差距）。所以跑变异检验时以 ② 为准，②b 只作为` +
       `佐证；只有 ②b 红而 ② 绿，那说明的是"这次连得巧"，不是"改动生效了"` +
       `—— 首帧等待由 gop_seconds 决定，但**分片能不能立刻到**由这次 flush 决定`);
  }
  if (A.expectFirstFragmentMs > 0) {
    ok(firstFragMs !== null && firstFragMs <= A.expectFirstFragmentMs,
       `连上之后第一块分片在 ${A.expectFirstFragmentMs}ms 内到达`,
       `实得 ${firstFragMs}ms（${summary.firstFragmentBytes}B，头 ${summary.firstFragmentHead}）`);
  } else {
    console.log(`  ℹ️ 未给 --expect-first-fragment-ms，第一块分片只报告不判：${firstFragMs}ms`);
  }
  // 这一条判的是**物理顺序**，不是性能：数据没到之前，画面不可能有内容。
  // 它红了只有两种解释 —— 插桩把时刻记错了，或者 openAt 取错了（比如记到了
  // `/ws/stats` 那条连接上）。两种都是尺子坏了，而它报出来的样子像产品慢。
  // 所以判据取「有没有量到」+「顺序对不对」，**不判绝对值**（绝对值与机器、
  // 网络、档位都有关，跨机器跑会假红 —— 陷阱 87）。
  ok(firstPictureMs !== null && firstFragMs !== null && firstPictureMs >= firstFragMs,
     `画面第一次有内容不早于第一块分片（图片 ${firstPictureMs}ms ≥ 分片 ${firstFragMs}ms）`,
     `firstDataAt=${diag.firstDataAt} firstFragAt=${wsInfo.firstFragAt} openAt=${wsInfo.openAt}` +
     `（null 说明页面里那次 rAF 轮询没等到 readyState ≥ 2 —— 不是"很快就出画了"）`);
  if (diag.msErrors.length) {
    console.log('\n--- 插桩记下的异常 ---');
    diag.msErrors.slice(0, 10).forEach((e) => console.log('  ' + e));
  }
  if (pageErrors.length) {
    console.log('\n--- 未捕获的页面异常 ---');
    pageErrors.slice(0, 10).forEach((e) => console.log('  ' + e.split('\n')[0]));
  }

  summary.ok = fail.length === 0;
  const sumPath = path.join(outDir, `summary-${TAG}.json`);
  fs.writeFileSync(sumPath, JSON.stringify(summary, null, 2), 'utf8');
  console.log(`[lat] 摘要 ${sumPath}`);
  console.log(`[lat] SUMMARY ${JSON.stringify(summary)}`);

  console.log(`\n${fail.length === 0 ? '全部通过。' : `失败 ${fail.length} 项。`}`);
  if (A.keep) { console.log('[lat] --keep：后端与刺激源留着'); backend = null; stimulus = null; }
  process.exitCode = fail.length === 0 ? 0 : 1;
} catch (e) {
  console.error('[lat] 出错：', e);
  process.exitCode = 2;
} finally {
  if (browser) await browser.close().catch(() => {});
  killTree(stimulus);
  killTree(backend);
}
