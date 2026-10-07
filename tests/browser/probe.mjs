// 真实浏览器里的诊断：起后端 → 用本机已装的 Chrome 打开页面 → 把浏览器自己
// 说的话全部收回来。
//
// 为什么非要有这个，而不是读代码推断：Web UI 的毛病几乎全是"只在浏览器里才成立"
// 的那一类 —— MSE 的 addSourceBuffer 会因为 codecs 字符串和实际码流对不上而抛
// NotSupportedError、appendBuffer 在 readyState 不对时会抛 InvalidStateError、
// Vue 的模板错误只在控制台里有一行 `[Vue warn]`。这些在 curl 和 Python 探针里
// **全都看不见**：接口返回 200、字节也对，只有浏览器知道哪里不对。
//
// 关键手法是 addInitScript：它在页面自己的脚本之前跑，所以能先把
// MediaSource / SourceBuffer 的方法包一层再交给前端用。不这么做的话，
// 前端 catch 掉异常我们就什么都看不到 —— 而"播不出来"正是这个样子。

import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

// ---------------------------------------------------------------- 参数
function args() {
  const a = process.argv.slice(2);
  const o = { port: 8123, rtsp: 8124, seconds: 14, headless: true, keep: false,
              route: '/preview', delay: 0, user: 'admin', password: 'admin' };
  for (let i = 0; i < a.length; i++) {
    if (a[i] === '--port') o.port = +a[++i];
    else if (a[i] === '--rtsp') o.rtsp = +a[++i];
    else if (a[i] === '--seconds') o.seconds = +a[++i];
    // 后端起来之后**等这么久再把浏览器接上去**。真实用法就是这个形状：
    // start.bat 先跑着，人去开浏览器时进程已经跑了几分钟。任何"以进程启动
    // 时刻为原点"的东西（fMP4 的时间轴基准）都只在这条路上才露出来。
    else if (a[i] === '--delay') o.delay = +a[++i];
    else if (a[i] === '--route') o.route = a[++i];
    else if (a[i] === '--user') o.user = a[++i];
    else if (a[i] === '--password') o.password = a[++i];
    else if (a[i] === '--headed') o.headless = false;
    else if (a[i] === '--keep') o.keep = true;
  }
  return o;
}
const A = args();

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------------------------------------------------------------- 后端
const outDir = path.join(ROOT, 'out', 'browser');
fs.mkdirSync(outDir, { recursive: true });

async function startBackend() {
  const exe = path.join(ROOT, 'build', 'bin', 'desk2onvif.exe');
  if (!fs.existsSync(exe)) throw new Error(`没有 ${exe}，先跑 scripts\\build.ps1`);
  const webRoot = path.join(ROOT, 'web', 'dist');
  // 用临时配置：绝不动用户 %APPDATA% 里那份，也不动仓库里可能存在的 config.json
  const cfg = path.join(outDir, 'probe-config.json');
  fs.rmSync(cfg, { force: true });
  const logFd = fs.openSync(path.join(outDir, 'backend.log'), 'w');
  const p = spawn(exe, [
    '--rtsp', '--rtsp-port', String(A.rtsp),
    '--http', '--http-port', String(A.port),
    '--web-root', webRoot,
    '--config', cfg,
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
      // **问 /api/session 而不是 /api/status。** 后者上了登录之后恒为 401，
      // 用一个 401 当"起来了没"的判据，表现是等满 40 秒然后报"后端起不来"，
      // 而日志里那句"HTTP 服务已启动"早就打过了。
      const r = await fetch(`http://127.0.0.1:${port}/api/session`);
      if (r.ok) return true;
    } catch {}
    await sleep(400);
  }
  return false;
}

// ---------------------------------------------------------------- 登录
async function sessionInfo(port) {
  try {
    const r = await fetch(`http://127.0.0.1:${port}/api/session`);
    return r.ok ? await r.json() : null;
  } catch {
    return null;
  }
}

async function login(port, user, password) {
  const r = await fetch(`http://127.0.0.1:${port}/api/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ user, password }),
  });
  if (!r.ok) throw new Error(`POST /api/login → ${r.status}（凭据：--user/--password）`);
  const list = typeof r.headers.getSetCookie === 'function'
    ? r.headers.getSetCookie()
    : [r.headers.get('set-cookie')].filter(Boolean);
  for (const v of list) {
    if (v && v.startsWith('d2o_session=')) return v.split(';', 1)[0].split('=', 2)[1];
  }
  throw new Error('登录返回 200 但没有 d2o_session cookie');
}

// ---------------------------------------------------------------- 主流程
const report = { console: [], pageErrors: [], requests: [], wsFrames: [] };
let backend, browser;

const fail = [];
const ok = (cond, what, detail = '') => {
  console.log(`  [${cond ? 'OK' : '!!'}] ${what}`);
  if (detail) console.log(`        ${detail}`);
  if (!cond) fail.push(what);
};

try {
  console.log(`[probe] 起后端 :${A.port}（RTSP ${A.rtsp}）`);
  backend = await startBackend();
  if (!(await waitHttp(A.port))) throw new Error('后端起不来，看 out/browser/backend.log');
  console.log('[probe] 后端就绪');

  // 先登录，再开浏览器。**从 Node 这边登录、把 cookie 注进上下文**，而不是
  // 去点登录页的按钮：探针要在"前端代码本身可能还是坏的时候"也能跑到它真正
  // 要诊断的那一段（MSE/预览），把登录页当成前置步骤会让两类失败混在一起。
  let cookieVal = null;
  {
    const sess = await sessionInfo(A.port);
    if (sess && sess.auth_required) {
      cookieVal = await login(A.port, A.user, A.password);
      // 先用 Node 自己验一次这个 cookie 真的能过。不验的话，一个无效的 cookie
      // 会一路走到最后，以"页面里没有 <video>"的形式报出来 —— 而真正的原因
      // 在登录这一步，中间的 401 只在 requests 那段里露一行。
      const chk = await fetch(`http://127.0.0.1:${A.port}/api/status`,
                              { headers: { Cookie: `d2o_session=${cookieVal}` } });
      if (!chk.ok) throw new Error(`拿到 cookie 但 /api/status → ${chk.status}`);
      console.log(`[probe] 已登录（user=${A.user}），cookie 会注进浏览器上下文`);
    } else {
      console.log('[probe] 服务端没开登录（auth_required 不是 true），直接开浏览器');
    }
  }

  if (A.delay > 0) {
    console.log(`[probe] 故意等 ${A.delay}s 再开浏览器（模拟"人过一会儿才打开页面"）`);
    await sleep(A.delay * 1000);
  }

  browser = await chromium.launch({ channel: 'chrome', headless: A.headless });
  // 显式建上下文（browser.newPage() 会建一个默认上下文，而默认上下文是
  // 加不进 cookie 的）。
  const context = await browser.newContext();
  if (cookieVal) {
    await context.addCookies([{
      name: 'd2o_session',
      value: cookieVal,
      url: `http://127.0.0.1:${A.port}/`,
      httpOnly: true,
      sameSite: 'Lax',
    }]);
  }
  const page = await context.newPage();

  // 在页面脚本之前插桩。这里记下来的东西是**全部**证据的来源：
  // 前端的 try/catch 会把异常吃掉，但吃之前它总得调用这些 API。
  await page.addInitScript(() => {
    window.__d2o = { ms: [], sb: [], errors: [] };
    const E = window.__d2o.errors;
    const origMS = window.MediaSource;
    if (origMS) {
      const origAdd = origMS.prototype.addSourceBuffer;
      origMS.prototype.addSourceBuffer = function (mime) {
        const rec = { mime, ok: null, err: null };
        window.__d2o.ms.push(rec);
        try {
          const sb = origAdd.call(this, mime);
          rec.ok = true;
          return sb;
        } catch (e) {
          rec.ok = false; rec.err = String(e);
          E.push(`addSourceBuffer(${mime}) 抛了：${e}`);
          throw e;
        }
      };
      const origEnd = origMS.prototype.endOfStream;
      origMS.prototype.endOfStream = function (...a) {
        E.push(`endOfStream(${a.join(',')}) 被调用`);
        return origEnd.apply(this, a);
      };
      this.addEventListener?.('error', () => {});
    }
    const SBP = window.SourceBuffer;
    if (SBP) {
      const origApp = SBP.prototype.appendBuffer;
      SBP.prototype.appendBuffer = function (buf) {
        const n = buf?.byteLength ?? buf?.length ?? -1;
        const rec = { n, readyState: this.updating, err: null, kind: null };
        window.__d2o.sb.push(rec);
        try {
          const r = origApp.call(this, buf);
          rec.kind = buf instanceof ArrayBuffer ? 'ArrayBuffer'
            : (typeof Blob !== 'undefined' && buf instanceof Blob) ? 'Blob' : '其它';
          return r;
        } catch (e) {
          rec.err = String(e);
          rec.kind = buf instanceof ArrayBuffer ? 'ArrayBuffer'
            : (typeof Blob !== 'undefined' && buf instanceof Blob) ? 'Blob' : '其它';
          E.push(`appendBuffer(${rec.kind}, ${n}B) 抛了：${e}`);
          throw e;
        }
      };
    }
  });

  page.on('console', (m) => {
    const t = m.type();
    if (t === 'debug' || t === 'verbose') return;
    report.console.push(`${t}: ${m.text()}`);
  });
  page.on('pageerror', (e) => report.pageErrors.push(String(e)));
  page.on('requestfailed', (r) =>
    report.requests.push(`${r.method()} ${r.url()} -> ${r.failure()?.errorText}`));
  page.on('response', (r) => {
    if (r.status() >= 400) report.requests.push(`${r.status()} ${r.request().method()} ${r.url()}`);
  });

  // 预览通道上到底收到了什么。Playwright 的 websocket 事件能拿到**原始帧**，
  // 所以"服务端发的是不是 fMP4""浏览器是不是收成了 Blob"这类问题一问就清楚。
  page.on('websocket', (ws) => {
    const url = ws.url();
    let init = null, frags = 0, bytes = 0;
    ws.on('framereceived', (f) => {
      const p = f.payload;
      const n = typeof p === 'string' ? Buffer.byteLength(p) : p.length;
      if (typeof p !== 'string' && init === null) {
        init = p;
        report.wsFrames.push(`${url} 首帧 ${n}B 开头=${p.subarray(4, 8).toString('latin1')}`);
      } else if (typeof p !== 'string') {
        frags++; bytes += n;
      }
    });
    ws.on('close', () =>
      report.wsFrames.push(`${url} 关闭：init=${init ? init.length + 'B' : '无'}，分片 ${frags} 块 ${bytes}B`));
  });

  console.log(`[probe] 打开 http://127.0.0.1:${A.port}${A.route}`);
  await page.goto(`http://127.0.0.1:${A.port}${A.route}`, { waitUntil: 'domcontentloaded' });

  // 预览是**按按钮才开始**的，不是打开页面就开始。这一条是第一次跑探针时用
  // 一小时换来的：不点，addSourceBuffer 一次都不会被调用，于是所有结论都指向
  // "什么也没发生"——而真实原因是"没人按开始"。诊断工具自己得先模拟出用户
  // 会做的那一下，否则量到的是空转。
  const clicked = await page.evaluate(() => {
    const bs = [...document.querySelectorAll('button')];
    const b = bs.find((x) => /开始|预览|播放|start/i.test(x.textContent || ''));
    if (!b) return bs.map((x) => (x.textContent || '').trim()).join(' | ');
    b.click();
    return 'clicked:' + (b.textContent || '').trim();
  });
  console.log(`[probe] 按钮：${clicked}`);

  // MSE 到底认哪个 codecs 串。前端首选 avc1.42C033，而它**不一定**被支持 ——
  // isTypeSupported 对这种串是挑剔的，而挑不中的表现就是返回空串、静默不播。
  const mse = await page.evaluate(() => {
    const t = (s) => { try { return MediaSource.isTypeSupported(s) } catch (e) { return 'throw' } };
    return {
      C033: t('video/mp4; codecs="avc1.42C033"'),
      E01E: t('video/mp4; codecs="avc1.42E01E"'),
      D401E: t('video/mp4; codecs="avc1.4D401E"'),
      bare: t('video/mp4'),
    };
  });
  console.log(`[probe] MSE 支持：${JSON.stringify(mse)}`);

  // 采样视频状态：一次快照分不出"还没开始"和"永远播不了"，得看它有没有在动。
  const samples = [];
  for (let i = 0; i < Math.ceil(A.seconds / 2); i++) {
    await sleep(2000);
    const s = await page.evaluate(() => {
      const v = document.querySelector('video');
      const d = window.__d2o || {};
      return {
        hasVideo: !!v,
        src: v ? (v.src || '').slice(0, 40) : null,
        readyState: v?.readyState ?? null,
        networkState: v?.networkState ?? null,
        currentTime: v ? +v.currentTime.toFixed(2) : null,
        w: v?.videoWidth ?? null, h: v?.videoHeight ?? null,
        paused: v?.paused ?? null,
        err: v?.error ? `${v.error.code}:${v.error.message}` : null,
        buffered: v && v.buffered.length
          ? [...Array(v.buffered.length)].map((_, i) =>
              `${v.buffered.start(i).toFixed(2)}~${v.buffered.end(i).toFixed(2)}`).join(',')
          : '',
        ms: (d.ms || []).map((x) => `${x.mime} ok=${x.ok}${x.err ? ' ERR=' + x.err : ''}`),
        sbCount: (d.sb || []).length,
        sbLast: (d.sb || []).slice(-3).map((x) => `${x.kind} ${x.n}B${x.err ? ' ERR=' + x.err : ''}`),
        msErrors: d.errors || [],
      };
    });
    samples.push(s);
    console.log(`[probe] t=${(i + 1) * 2}s readyState=${s.readyState} currentTime=${s.currentTime} ` +
                `${s.w}x${s.h} 分片已追加=${s.sbCount} 缓冲=${s.buffered || '-'}`);
  }

  const shot = path.join(outDir, `probe${A.route.replace(/\W+/g, '_')}.png`);
  await page.screenshot({ path: shot, fullPage: true });
  console.log(`[probe] 截图 ${shot}`);

  // ------------------------------------------------------------ 结论
  const last = samples[samples.length - 1] || {};
  const first = samples[0] || {};
  console.log('\n=== 浏览器侧结论 ===');
  ok(last.hasVideo, '页面里有 <video> 元素');
  ok((last.ms || []).length > 0, '前端调用了 addSourceBuffer',
     (last.ms || []).join(' | '));
  ok((last.ms || []).every((m) => !m.includes('ERR=')), 'addSourceBuffer 没抛异常');
  ok((last.sbCount || 0) > 0, `前端调用了 appendBuffer（${last.sbCount || 0} 次）`,
     (last.sbLast || []).join(' | '));
  ok((last.sbLast || []).every((x) => !x.includes('ERR=')), 'appendBuffer 没抛异常');
  ok(last.w > 0 && last.h > 0, `videoWidth 有值（${last.w}x${last.h}）`);
  ok((last.currentTime || 0) > first.currentTime || (last.currentTime || 0) > 0.3,
     `播放位置在前进（${first.currentTime} → ${last.currentTime}）`);
  ok(last.readyState >= 3, `readyState ≥ 3（实得 ${last.readyState}）`);

  if (last.msErrors?.length) {
    console.log('\n--- 插桩记下的异常 ---');
    last.msErrors.forEach((e) => console.log('  ' + e));
  }
  if (report.pageErrors.length) {
    console.log('\n--- 未捕获的页面异常 ---');
    report.pageErrors.forEach((e) => console.log('  ' + e.split('\n')[0]));
  }
  if (report.console.length) {
    console.log('\n--- 控制台 ---');
    report.console.slice(-25).forEach((e) => console.log('  ' + e));
  }
  if (report.requests.length) {
    console.log('\n--- 失败的请求 ---');
    [...new Set(report.requests)].forEach((e) => console.log('  ' + e));
  }
  if (report.wsFrames.length) {
    console.log('\n--- WebSocket ---');
    report.wsFrames.forEach((e) => console.log('  ' + e));
  }

  console.log(`\n${fail.length === 0 ? '全部通过。' : `失败 ${fail.length} 项。`}`);
  if (A.keep) { console.log('[probe] --keep：后端留着'); backend = null; }
  process.exitCode = fail.length === 0 ? 0 : 1;
} catch (e) {
  console.error('[probe] 出错：', e);
  process.exitCode = 2;
} finally {
  if (browser) await browser.close().catch(() => {});
  killTree(backend);
}
