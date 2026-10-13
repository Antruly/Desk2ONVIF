// 「性能不足」横幅 + 一键降档按钮的端到端验证。
//
// 为什么非要有这个：`probe.mjs` 只证明页面能渲染、没有控制台报错，而**这条横幅
// 在健康负载下永远不出现**。也就是说「页面能开」和「性能告警能用」是两件事，
// 前者全绿的时候后者一个像素都没被验过 —— 而它恰恰是这一页上唯一一个
// "要用户现在做点什么"的东西，做错了没人会发现。
//
// 手法：把机器**真的**压到 overloaded，而不是伪造 stats。伪造的办法有（往
// /api/status 前面插一层代理），但那样验的是"前端拿到这个 JSON 会怎么画"，
// 而这里不知道的恰恰是"后端什么时候会给出这个 JSON、按钮点下去之后配置和
// 运行中的管线到底有没有跟着变"。
//
// **压出 overloaded 比想象的难，这一条是实测出来的**（第一次跑这个脚本就是
// 这么失败的）：只用 `--preset veryslow --threads 1` 压不出 overloaded，
// 它稳定停在 `throttled`、duty 在 2 和 3 之间来回。原因是自适应**按设计**
// 在这种机器上就够用了 —— 编码器实测 ~9.7 fps，而默认下限是 5 fps，于是
// duty 爬到 3（目标 8.3 fps）就不再"跟不上"，干净满 10 秒又降回 2，
// 在 2/3 之间循环，永远到不了 duty 上限，也就永远不报 overloaded。
// 也就是说：**在那台机器上，"性能不足"这个状态本来就不成立，报不出来是对的。**
//
// 要走到 overloaded，唯一诚实的路子是让**帧率下限高于编码器的实测吞吐**：
// 让用户明确要求一个这台机器给不了的帧率（`adaptive.min_fps`），
// 于是 `max_duty_ = fps/min_fps` 变小、duty 顶到上限之后**仍然**跟不上 ——
// 这正是这个状态的定义。所以下面那份种子配置里写了 `min_fps`：
//
//   adaptive.cpp 的口径 —— 池子见底就记这一秒"跟不上"，`duty` 每秒 +1，
//   上限 = base_fps/min_fps = 25/12 = 2；到上限之后**再**连着 2 秒跟不上
//   才报 overloaded。所以稳定在 ~2-3 秒后出现，不是碰运气。
//
// 判据有两条腿，缺一条都不算数：
//   1. 浏览器里真的画出横幅和按钮（含代价那句话）；
//   2. 点下去之后**服务端**的配置真的变成新档位、且那一档真的离开了 pending
//      （= 管线真的按新档位重开过，而不是只写了个要重启才生效的配置）。

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
    port: 8127,
    rtsp: 8128,
    // 轮询 overloaded 的上限。给足是因为这台机器上负载可能被别的东西干扰
    // （比如同时在跑构建），而多等十几秒远好过一次假的"没触发"。
    wait: 60,
    preset: 'veryslow',
    threads: 1,
    // 帧率下限。**这个数是"压出 overloaded"的关键**，理由见文件头：
    // 它必须**高于**这台机器上编码器的实测吞吐，否则自适应会自己撑住，
    // 状态最多到 throttled。本机 2880x1920 + veryslow + 1 线程实测 ~9.7 fps，
    // 所以 12 能压出来、5（默认值）压不出来。
    minFps: 12,
    seconds: 180,
    headless: true,
    keep: false,
    user: 'admin',
    password: 'admin',
  };
  for (let i = 0; i < a.length; i++) {
    if (a[i] === '--port') o.port = +a[++i];
    else if (a[i] === '--rtsp') o.rtsp = +a[++i];
    else if (a[i] === '--wait') o.wait = +a[++i];
    else if (a[i] === '--preset') o.preset = a[++i];
    else if (a[i] === '--threads') o.threads = +a[++i];
    else if (a[i] === '--min-fps') o.minFps = +a[++i];
    else if (a[i] === '--seconds') o.seconds = +a[++i];
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
  // 用临时配置：绝不动用户 %APPDATA% 里那份，也不动仓库里可能存在的 config.json。
  // **每次都要重写而不是"没有才写"**：上一次跑完留在盘上的画质档位会让这一次的
  // "下一档"从流畅变成极低，于是按钮文案对不上，看起来像前端坏了
  // （陷阱 34 的邻居）。
  //
  // 种子只写一个字段：`adaptive.min_fps`。其余全部靠 `config_from_json` 合并到
  // 默认值上 —— 手抄一份完整默认配置出来，等于在这里放一份会和产品漂移的副本。
  const cfg = path.join(outDir, 'overload-config.json');
  fs.writeFileSync(cfg, JSON.stringify({ adaptive: { mode: 'throttle', min_fps: A.minFps } }, null, 2));
  const logFd = fs.openSync(path.join(outDir, 'overload-backend.log'), 'w');
  const p = spawn(exe, [
    '--rtsp', '--rtsp-port', String(A.rtsp),
    '--http', '--http-port', String(A.port),
    '--web-root', webRoot,
    '--config', cfg,
    // 这三条就是"压不动"的成因，也是这个脚本唯一比 probe.mjs 多出来的东西。
    '--preset', A.preset,
    '--threads', String(A.threads),
    // **不能省**：不给 --seconds 时进程是常驻的，脚本跑完只能靠 taskkill 收场，
    // 而"被 taskkill 掉的"留不下收尾统计。给一个远大于本次时长的预算，
    // 既是兜底也让正常路径能自己收尾。
    '--seconds', String(A.seconds),
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

// ---------------------------------------------------------------- 登录 / HTTP
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

/// 带着 cookie 的 GET。**不用页面里的 fetch 去问**：那样量到的是"前端以为
/// 服务端是什么"，而这一步要的恰恰是服务端**独立于浏览器**的真相 ——
/// 浏览器里显示"已降到流畅"而盘上还是均衡，正是这个脚本要抓的那一类谎。
async function getJson(port, cookie, url) {
  const r = await fetch(`http://127.0.0.1:${port}${url}`, {
    headers: cookie ? { Cookie: `d2o_session=${cookie}` } : {},
  });
  if (!r.ok) throw new Error(`GET ${url} → ${r.status}`);
  return await r.json();
}

// ---------------------------------------------------------------- 主流程
const fail = [];
const ok = (cond, what, detail = '') => {
  console.log(`  [${cond ? 'OK' : '!!'}] ${what}`);
  if (detail) console.log(`        ${detail}`);
  if (!cond) fail.push(what);
};

const report = { console: [], pageErrors: [], requests: [] };
let backend, browser;

/// 轮询页面里的某个表达式，直到它返回真值。失败时把最后一次的值带回来 ——
/// "等超时了"和"等到了但值不对"在断言里是两回事，得能分开看。
async function waitForValue(page, fn, timeoutMs, label) {
  const t0 = Date.now();
  let last = null;
  while (Date.now() - t0 < timeoutMs) {
    last = await page.evaluate(fn).catch(() => null);
    if (last) return { hit: true, value: last, waited: Date.now() - t0 };
    await sleep(500);
  }
  return { hit: false, value: last, waited: Date.now() - t0, label };
}

try {
  console.log(`[overload] 起后端 :${A.port}（RTSP ${A.rtsp}，preset=${A.preset}，` +
              `threads=${A.threads}，adaptive.min_fps=${A.minFps}）`);
  backend = await startBackend();
  if (!(await waitHttp(A.port))) throw new Error('后端起不来，看 out/browser/overload-backend.log');
  console.log('[overload] 后端就绪');

  let cookie = null;
  {
    const sess = await getJson(A.port, null, '/api/session');
    if (sess && sess.auth_required) {
      cookie = await login(A.port, A.user, A.password);
      const chk = await fetch(`http://127.0.0.1:${A.port}/api/status`,
                              { headers: { Cookie: `d2o_session=${cookie}` } });
      if (!chk.ok) throw new Error(`拿到 cookie 但 /api/status → ${chk.status}`);
      console.log(`[overload] 已登录（user=${A.user}）`);
    } else {
      console.log('[overload] 服务端没开登录，直接往下走');
    }
  }

  // ------------------------------------------------------------ 1. 真的压到 overloaded
  //
  // 每一秒打一行，因为"没触发"这个失败的最可能原因是**这台机器比预期快**
  // （换了台机器、或者 veryslow 在这个 x264 版本上没那么慢）。一行都不打的话，
  // 失败信息只有"超时"，而看不出它到底到过 throttled 没有。
  const expected = await getJson(A.port, cookie, '/api/config');
  const tiers = Array.isArray(expected.quality_tiers) ? expected.quality_tiers : [];
  const q0 = expected.capture?.quality;
  ok(tiers.length >= 2, `后端给了画质档位表（${tiers.length} 档）`);
  const idx0 = tiers.findIndex((t) => t.key === q0);
  const wantTier = idx0 >= 0 && idx0 + 1 < tiers.length ? tiers[idx0 + 1] : null;
  ok(wantTier !== null, `默认档「${q0}」有下一档可降`, wantTier ? `${wantTier.key}（${wantTier.label}）` : '');

  console.log(`[overload] 等 perf_state 变成 overloaded（最多 ${A.wait}s）`);
  let status = null;
  let sawState = new Set();
  {
    const t0 = Date.now();
    while (Date.now() - t0 < A.wait * 1000) {
      status = await getJson(A.port, cookie, '/api/status');
      sawState.add(status.perf_state);
      console.log(
        `[overload] t=${((Date.now() - t0) / 1000).toFixed(0).padStart(2)}s ` +
        `state=${String(status.perf_state).padEnd(10)} duty=${status.perf_duty} ` +
        `target=${status.perf_target_fps}fps 实测=${Number(status.effective_fps ?? 0).toFixed(1)}fps ` +
        `建议=${status.perf_suggested_quality || '（无）'}`);
      if (status.perf_state === 'overloaded') break;
      await sleep(1000);
    }
  }

  const reached = status && status.perf_state === 'overloaded';
  ok(reached, '压到了 overloaded', reached ? '' : `到过的状态：${[...sawState].join(', ')}`);
  if (!reached) {
    // **不触发就不许通过。** 这条路径要是没被走到，下面那些断言一个都没验到，
    // 而它们会"全绿"—— 那比不测更坏。
    console.log(`\n[overload] 这台机器在 --preset ${A.preset} --threads ${A.threads}、` +
                `adaptive.min_fps=${A.minFps} 下仍然跟得上，overloaded 这条路径没有被触发。`);
    console.log('           这不是通过：横幅和按钮一个都没验到。');
    console.log('           判据：上面每行的「实测=」是编码器真的吐得出多少帧 —— 它必须' +
                `低于 ${A.minFps}（= adaptive.min_fps）才会顶到 duty 上限。`);
    console.log('           本机偏低时用 --min-fps 调高再跑（例如 --min-fps 20）。');
    process.exitCode = 3;
    throw new Error('__skip__');
  }
  ok(status.perf_duty > 1, `perf_duty > 1（实得 ${status.perf_duty}）`);
  ok(status.perf_suggested_quality === (wantTier && wantTier.key),
     `建议的下一档就是档位表里的下一档`,
     `实得 ${status.perf_suggested_quality}，期望 ${wantTier && wantTier.key}`);
  ok(typeof status.perf_hint === 'string' && status.perf_hint.length > 0,
     'perf_hint 有内容（横幅正文由后端拼）', status.perf_hint);

  // ------------------------------------------------------------ 2. 浏览器里的横幅
  browser = await chromium.launch({ channel: 'chrome', headless: A.headless });
  const context = await browser.newContext();
  if (cookie) {
    await context.addCookies([{
      name: 'd2o_session',
      value: cookie,
      url: `http://127.0.0.1:${A.port}/`,
      httpOnly: true,
      sameSite: 'Lax',
    }]);
  }
  const page = await context.newPage();
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

  console.log(`[overload] 打开 http://127.0.0.1:${A.port}/status`);
  await page.goto(`http://127.0.0.1:${A.port}/status`, { waitUntil: 'domcontentloaded' });

  // 横幅要等两件事：WS/HTTP 把 overloaded 那一帧送到，以及档位表取回来
  // （按钮上的中文名靠它）。两者都到了才算画全。
  const banner = await waitForValue(page, () => {
    const b = [...document.querySelectorAll('.banner')]
      .find((x) => x.className.includes('is-err'));
    if (!b) return null;
    const btn = b.querySelector('button');
    if (!btn || !btn.innerText.trim()) return null;
    return {
      text: b.innerText.replace(/\s+/g, ' ').trim(),
      button: btn.innerText.trim(),
      disabled: !!btn.disabled,
    };
  }, 20000, '横幅');

  ok(banner.hit, '状态页出现了 is-err 横幅', banner.hit ? '' : `等了 ${banner.waited}ms 没出现`);
  const bt = banner.value || {};
  ok((bt.text || '').includes('本设备性能不足以实时编码'),
     '横幅第一句写着"本设备性能不足以实时编码"', bt.text || '(无)');
  ok((bt.text || '').includes(status.perf_hint),
     '横幅正文就是后端给的 perf_hint（前端没有第二份真相）');
  ok((bt.text || '').includes(`实际每 ${status.perf_duty} 拍出一帧`),
     '横幅带上了"每几拍出一帧"的说明', bt.text || '(无)');

  const wantLabel = wantTier ? wantTier.label : '';
  ok(bt.button === `降到「${wantLabel}」并立即生效`,
     `按钮文案是「降到「${wantLabel}」并立即生效」`, `实得「${bt.button}」`);
  ok(bt.disabled === false, '按钮此刻可点');
  // 代价那句话（陷阱 48 的邻居：这一页唯一会让画面断掉的操作，代价不能只写在
  // tooltip 里 —— tooltip 在点之前根本不出现）。
  ok((bt.text || '').includes('会现场重开采集管线'), '按钮旁边写了"会现场重开采集管线"');
  ok((bt.text || '').includes('当前录像分段'), '代价里说明了"当前录像分段"');
  ok((bt.text || '').includes('NVR'), '代价里说明了 NVR 会断开重连');

  const shotBefore = path.join(outDir, 'overload_banner.png');
  await page.screenshot({ path: shotBefore, fullPage: true });
  console.log(`[overload] 截图 ${shotBefore}`);

  // ------------------------------------------------------------ 3. 点它
  const clicked = await page.evaluate(() => {
    const b = [...document.querySelectorAll('.banner.is-err')]
      .find((x) => x.className.includes('is-err'));
    const btn = b && b.querySelector('button');
    if (!btn) return null;
    btn.click();
    return btn.innerText.trim();
  });
  ok(clicked !== null, `点了按钮`, clicked ? `「${clicked}」` : '');

  // 结果那句话渲染在**横幅外面**（横幅在 perf_state 变回 ok 时会消失），所以
  // 从整个页面里找。「已降到「」这个前缀只有成功那条有 —— 失败那条写的是
  // "没有降档"/"降档只成功了一半"，不会被这两条断言误判成成功。
  const result = await waitForValue(page, () => {
    const t = document.body.innerText.replace(/\s+/g, ' ');
    const m = [...document.querySelectorAll('.banner')]
      .map((x) => x.innerText.replace(/\s+/g, ' ').trim())
      .find((x) => x.includes('已降到「'));
    return m ? { banner: m, all: t.includes('已降到「') } : null;
  }, 20000, '降档结果');

  ok(result.hit, '页面给出了降档结果', result.hit ? `等了 ${result.waited}ms` : `等了 ${result.waited}ms 没出现`);
  const rb = (result.value || {}).banner || '';
  ok(rb.includes(`已降到「${wantLabel}」`), `结果是"已降到「${wantLabel}」"`, rb || '(无)');
  ok(rb.includes('重开了采集管线'), '结果里说明了管线已重开');
  ok(rb.includes('录像已分段'), '结果里说明了录像已分段');

  const shotAfter = path.join(outDir, 'overload_after_click.png');
  await page.screenshot({ path: shotAfter, fullPage: true });
  console.log(`[overload] 截图 ${shotAfter}`);

  // ------------------------------------------------------------ 4. 服务端独立复核
  //
  // 这一节是**整个脚本的重点**：前端说"已降到流畅"是一句话，而这句话为真
  // 需要两件事同时成立 —— 配置真的落成了新档位的具体值，**并且**运行中的
  // 管线真的按新值重开过（判据是那几项离开了 pending）。只查前者的话，
  // 一个"只 POST 了配置、没 POST reload"的前端会全绿，而那正是这个按钮
  // 最可能写出的一种错。
  const want = wantTier || {};
  const t0 = Date.now();
  let cfg = null;
  let cleared = null;
  while (Date.now() - t0 < 15000) {
    cfg = await getJson(A.port, cookie, '/api/config');
    const pend = Array.isArray(cfg.pending) ? cfg.pending : [];
    const fields = ['capture.fps', 'capture.bitrate_kbps', 'capture.max_height'];
    if (cfg.capture?.quality === want.key && fields.every((f) => !pend.includes(f))) {
      cleared = { pend, waited: Date.now() - t0 };
      break;
    }
    await sleep(500);
  }

  ok(cfg.capture?.quality === want.key,
     `服务端配置的档位真的变成了 ${want.key}`, `实得 ${cfg.capture?.quality}`);
  ok(cfg.capture?.fps === want.fps, `fps 展开成了档位值 ${want.fps}`, `实得 ${cfg.capture?.fps}`);
  ok(cfg.capture?.bitrate_kbps === want.bitrate_kbps,
     `bitrate_kbps 展开成了档位值 ${want.bitrate_kbps}`, `实得 ${cfg.capture?.bitrate_kbps}`);
  ok(cfg.capture?.max_height === want.max_height,
     `max_height 展开成了档位值 ${want.max_height}`, `实得 ${cfg.capture?.max_height}`);
  ok(cleared !== null,
     '新档位离开了 pending（= 管线真的按新档位重开过，不是"存了但等重启"）',
     cleared ? `用了 ${cleared.waited}ms，剩下 pending=[${cleared.pend.join(', ')}]`
             : `15 秒后仍是 pending=[${(cfg.pending || []).join(', ')}]`);

  // ------------------------------------------------------------ 报告
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
  // 页面自己的 script 抛异常意味着"这一页在真实浏览器里是坏的"，不管断言过没过。
  ok(report.pageErrors.length === 0, '页面没有未捕获异常');
  ok(report.console.filter((m) => m.startsWith('error:')).length === 0,
     '控制台没有 error 级消息');

  console.log(`\n${fail.length === 0 ? '全部通过。' : `失败 ${fail.length} 项。`}`);
  if (A.keep) { console.log('[overload] --keep：后端留着'); backend = null; }
  process.exitCode = fail.length === 0 ? 0 : 1;
} catch (e) {
  if (String(e.message) === '__skip__') {
    // 已经在上面把话说清楚了，别再打一遍栈。
  } else {
    console.error('[overload] 出错：', e);
    process.exitCode = 2;
  }
} finally {
  if (browser) await browser.close().catch(() => {});
  killTree(backend);
}
