// 配置页的真实浏览器验证：**表单能不能把用户敲的值存进去**。
//
// 它存在的直接原因是那条 bug："码率上限设 1024 / 2048 保存不了"。
// 根因不在服务器端（`config.cpp` 一直是收的），而在前端：
//
//   - `step="100"` 让 1024 成了 HTML5 约束校验里的 `stepMismatch`；
//   - 原生校验不过时浏览器**直接拦掉 submit**，`@submit.prevent` 的 handler
//     根本跑不到；
//   - 于是用户点了保存，什么都没有发生，也没有一句话。
//
// 这类缺陷在 curl 和 Python 探针里**全都看不见** —— `POST /api/config` 手写
// 一遍照样 200。只有真浏览器会替用户把那一按拦下来。这就是这个探针非有不可的
// 理由，和 probe.mjs（MSE）、login_probe.mjs（路由守卫）是同一个道理。
//
// 而且判据不能只写"1024 能存进去"：那只盖住了这一个数。要写成**结构性的**——
// 配置表单里**任何**数字输入框的 step 都不能比 1 粗（见 [1] 段那段注释：比 1 细的
// step 只会拒绝亚整数粒度，落不到整数域上），以及表单必须带 novalidate。
// 否则下一个人把某个 step 改回 100，这个探针会安静地全绿。
//
// 用法:
//   node tests/browser/config_probe.mjs [--port 8129] [--rtsp 8130]
//                                       [--user admin] [--password admin]
//                                       [--headed] [--keep]

import { chromium } from 'playwright-core';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

function args() {
  const a = process.argv.slice(2);
  const o = { port: 8129, rtsp: 8130, headless: true, keep: false,
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
  const cfg = path.join(outDir, 'config-probe.json');
  fs.rmSync(cfg, { force: true });
  const logFd = fs.openSync(path.join(outDir, 'config-backend.log'), 'w');
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

/** 直接问后端要当前配置 —— 判"存进去了没有"必须问它，不能只看页面。 */
async function getConfig(cookie) {
  const r = await fetch(`${BASE}/api/config`, { headers: { Cookie: `d2o_session=${cookie}` } });
  if (!r.ok) throw new Error(`GET /api/config → ${r.status}`);
  return r.json();
}

// ---------------------------------------------------------------- 定位
//
// 用**标签文字**定位，不给输入框加 data-* 属性。理由：标签就是用户用来认这一格
// 的东西，所以"标签改了、探针跟着要改"正是我们想要的耦合 —— 凭 id 定位的话，
// 界面把两个框换了个位置、探针照样绿，而用户看到的是另一个框。
function fieldInput(page, labelPrefix) {
  return page
    .locator('label.field')
    .filter({ has: page.locator('.field-label', { hasText: labelPrefix }) })
    .locator('input');
}

/** 记下所有 POST /api/config 的请求体，供"到底发没发出去"这条断言用。 */
function recordConfigPosts(page) {
  const posts = [];
  page.on('request', (req) => {
    if (req.method() === 'POST' && req.url().endsWith('/api/config')) {
      let body = null;
      try { body = req.postDataJSON(); } catch { body = req.postData(); }
      posts.push(body);
    }
  });
  return posts;
}

// ---------------------------------------------------------------- 主流程
let backend = null, browser = null;
let exitCode = 1;

try {
  backend = startBackend();
  if (!await waitHttp()) throw new Error('后端 40 秒内没起来（看 out/browser/config-backend.log）');
  const cookie = await login();
  console.log(`[probe] 后端已就绪 ${BASE}（登录已开启，凭据 ${A.user}/***）`);

  browser = await chromium.launch({ channel: 'chrome', headless: A.headless });
  const ctx = await browser.newContext();
  await ctx.addCookies([{
    name: 'd2o_session', value: cookie, url: `${BASE}/`,
    httpOnly: true, sameSite: 'Lax',
  }]);
  const page = await ctx.newPage();
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));
  const posts = recordConfigPosts(page);

  const cfgUrl = `${BASE}/config`;

  // ---- 1. 表单的结构性约束 -------------------------------------------
  //
  // **这一段才是真正防回归的那一条。** 下面 [2][3] 只验 1024/2048 两个数，
  // 而"某个输入框的 step 又被写成 100"这种事会从它们眼皮底下溜过去。
  console.log('\n[1] 表单不再用原生约束校验');
  await page.goto(cfgUrl, { waitUntil: 'networkidle' });
  const bitrate = fieldInput(page, '码率 kbps');
  await bitrate.waitFor({ state: 'visible', timeout: 20000 });

  check(await page.locator('form').first().getAttribute('novalidate') !== null,
        '表单带 novalidate（校验权只在脚本那一份）');

  const steps = await page.evaluate(() => {
    const form = document.querySelector('form');
    return [...form.querySelectorAll('input[type="number"]')].map((el) => ({
      // 用离它最近的那个标签文字来称呼它，报错时才说得清是哪一个
      label: (el.closest('label')?.querySelector('.field-label')?.textContent || '?').trim(),
      step: el.getAttribute('step'),
      min: el.getAttribute('min'),
      max: el.getAttribute('max'),
    }));
  });
  // **判据是"step 一律不得比 1 粗"，不是"一律必须等于 1"。**
  //
  // 原来写的是 `s.step !== null && s.step !== '1'`（放行 1 和"没写"），这把它要防的
  // 那条规则写窄了。真正会咬人的是**比 1 粗的 step**：`step` 不是"只允许整百/偶数"
  // 的表达方式，它的实际作用是**拒绝一切不是它倍数**的值，而 1024 / 2048 恰好不是
  // 100 的倍数 —— 那正是"保存不了"的根因。
  //
  // 反过来，**比 1 细**的 step 只拒绝亚整数粒度，而它只可能落在本来就有小数的字段上：
  // 这里就是两个增益（0–8，0.1 一档）。把它们一起判红，等于让"正确的东西"去迁就
  // 一条写宽了的规则 —— 而放宽规则之前要先问一句：放宽之后这条断言还挡得住原来那个
  // bug 吗？挡得住（100 / 32 / 2 全都 > 1，第一次跑就是靠它抓出那两处 `step="2"`）。
  //
  // `step="any"` 单独放行：它表示"不约束"，**任何值都不会被它拒绝**，比 1 还安全。
  // 真正要比的不是"粗细"，而是"它能不能拒绝一个用户可以正当输入的值"。
  const badSteps = steps.filter((s) => {
    if (s.step === null) return false;      // 不写 = 默认 1
    if (s.step === 'any') return false;     // 不约束，拒绝不了任何值
    const v = Number(s.step);
    return !Number.isFinite(v) || v <= 0 || v > 1;
  });
  check(badSteps.length === 0,
        `配置表单里 ${steps.length} 个数字输入框的 step 都不比 1 粗` +
        `（其中 ${steps.filter((s) => s.step !== null && s.step !== '1').length} 个比 1 细）`,
        badSteps.map((s) => `${s.label}: step=${s.step}`).join('\n         '));
  // 兜一条：这个选择器真的选中了东西。零个输入框时上面那条会**空集通过** ——
  // 陷阱 60 的形状（"它应该不存在"在本就不存在时恒真）。
  check(steps.length >= 8, `确实扫到了数字输入框（${steps.length} 个）`);

  // ---- 2. 1024 ---------------------------------------------------------
  //
  // 断言分三层，缺一不可：**发出去的请求体**、**后端读回来的值**、
  // **刷新之后输入框里显示的值**。只验中间那层的话，"界面显示 1024 而后端
  // 存的是别的"和"后端存对了但界面回显成旧值"两种毛病都漏掉。
  console.log('\n[2] 码率 1024');
  const before = posts.length;
  await bitrate.fill('1024');
  await page.locator('button[type="submit"]').first().click();
  for (let i = 0; i < 60 && posts.length === before; i++) await sleep(100);
  check(posts.length === before + 1, '点保存真的发出了 POST /api/config',
        `发出 ${posts.length - before} 次`);
  const sent = posts[posts.length - 1];
  check(sent?.capture?.bitrate_kbps === 1024,
        `请求体里的码率是 1024（实得 ${JSON.stringify(sent?.capture?.bitrate_kbps)}）`);
  // 越界值会被后端夹取，等它存完再读。
  await sleep(800);
  const stored = await getConfig(cookie);
  check(stored?.capture?.bitrate_kbps === 1024,
        `GET /api/config 读回来还是 1024（实得 ${stored?.capture?.bitrate_kbps}）`);
  // 档位：1024 不在任何一档里，所以前端该把 quality 翻成 custom —— 否则后端
  // 会照着"均衡"把 2048 覆盖回去，症状就是"改了码率、保存、码率又变回去"。
  check(stored?.capture?.quality === 'custom',
        `档位自动翻成 custom（实得 ${stored?.capture?.quality}）`);

  await page.reload({ waitUntil: 'networkidle' });
  const afterReload = fieldInput(page, '码率 kbps');
  await afterReload.waitFor({ state: 'visible', timeout: 20000 });
  await sleep(500);
  check(await afterReload.inputValue() === '1024',
        `刷新后输入框显示 1024（实得 ${await afterReload.inputValue()}）`);

  // ---- 3. 2048 ---------------------------------------------------------
  console.log('\n[3] 码率 2048');
  const before2 = posts.length;
  await afterReload.fill('2048');
  await page.locator('button[type="submit"]').first().click();
  for (let i = 0; i < 60 && posts.length === before2; i++) await sleep(100);
  check(posts.length === before2 + 1, '点保存真的发出了 POST /api/config');
  const sent2 = posts[posts.length - 1];
  check(sent2?.capture?.bitrate_kbps === 2048,
        `请求体里的码率是 2048（实得 ${JSON.stringify(sent2?.capture?.bitrate_kbps)}）`);
  await sleep(800);
  const stored2 = await getConfig(cookie);
  check(stored2?.capture?.bitrate_kbps === 2048,
        `GET /api/config 读回来还是 2048（实得 ${stored2?.capture?.bitrate_kbps}）`);

  // ---- 4. 越界值：必须看得见，而且不许发请求 ---------------------------
  //
  // 这一段的判据是**成对**的：既要"有一句人话"，也要"没把脏值发出去"。
  // 只验前者的话，一个"照发不误、然后靠后端夹取"的实现也会通过 ——
  // 而那正是用户看到的"我填了 99999，保存后它自己变成了 50000"。
  console.log('\n[4] 越界值 99999');
  const before3 = posts.length;
  const bitrate3 = fieldInput(page, '码率 kbps');
  await bitrate3.fill('99999');
  await page.locator('button[type="submit"]').first().click();
  await sleep(1200);
  check(posts.length === before3, '越界时**没有**发出 POST /api/config',
        `发出了 ${posts.length - before3} 次`);

  const banner = page.locator('.banner').first();
  const bannerVisible = await banner.count() > 0 && await banner.isVisible();
  const bannerText = bannerVisible ? (await banner.innerText()).trim() : '';
  check(bannerVisible && /码率/.test(bannerText),
        `出现可见的出错横幅并点名了「码率」：${JSON.stringify(bannerText.slice(0, 80))}`);
  // 横幅里要给出**范围**，不能只说"不合法" —— 用户得知道改成多少。
  check(/100/.test(bannerText) && /50000/.test(bannerText),
        '横幅里写了合法范围（100–50000）');
  // 顺带验一句"滚到第一个错处"：出错的输入框应该在视口里。
  const inView = await page.evaluate(() => {
    const el = document.querySelector('input.invalid');
    if (!el) return null;
    const r = el.getBoundingClientRect();
    return r.top >= -1 && r.bottom <= window.innerHeight + 1;
  });
  check(inView === true, '出错的输入框被滚进了视口', String(inView));

  // ---- 5. 水印（OSD）的往返：加 → 拖 → 关 → 存 → 删 -------------------
  //
  // 拖动定位这条链上有三份数：**拖出来的百分比**、**POST 出去的那两个数**、
  // **换算回像素之后标记落在哪**。三份必须是同一个表达式 —— `anchor()` 的
  // custom 分支把 `x_pct` 解释成"文字左上角在编码帧上的百分比、没有边距"，
  // 所以前端只能用 `round(像素 * 100 / 容器宽)` 这一条路去反算。任何一环换成
  // 另一套算法，症状都是"预览在左上、录出来偏右下" —— 而那个症状在 curl 和
  // Python 探针里一个字都看不出来（这也是这一段留在这儿、而不是塞进
  // web_contract.py 的理由）。
  console.log('\n[5] 水印：加一项 → 拖到左上 → 关闭一项 → 保存 → 删一项');
  await page.reload({ waitUntil: 'networkidle' });   // 顺手丢掉 [4] 里那个 99999
  await page.locator('#osd').scrollIntoViewIfNeeded();

  const cards = page.locator('.osd-item');
  const N0 = await cards.count();
  check(N0 === 2, `默认 2 项水印（实得 ${N0}）`);

  // 这一段里"第 N 项里的某个控件"要靠标签去认，不能按下标数 `select`/`input`：
  // 项里的控件是**随类型变的**（`text` 多一个输入框、`time` 多两个格式串、
  // `custom` 才有两个百分比），按下标定位的话，改一次模板就会指到别的控件上，
  // 而探针照样绿。
  const itemCtl = (card, labelPrefix, tag) =>
    card
      .locator('label.field')
      .filter({ has: page.locator('.field-label', { hasText: labelPrefix }) })
      .locator(tag);

  // 底图**点一下才抓**：用户明确要求"快照默认不抓拍、点了才显示"。
  check(await page.locator('.osd-stage img').count() === 0,
        '没点之前拖动区里没有 <img>（底图不自动抓）');
  await page.locator('button', { hasText: '取当前画面' }).first().click();
  await page.locator('.osd-stage img').waitFor({ state: 'visible', timeout: 20000 });
  await page.waitForFunction(() => {
    const i = document.querySelector('.osd-stage img');
    return !!(i && i.complete && i.naturalWidth > 0);
  }, null, { timeout: 20000 });
  check(await page.locator('.osd-marker').count() === N0,
        `底图出来之后 ${N0} 个标记块各就各位`);

  // 5.1 拖第 1 项到左上角。用真鼠标事件（pointerdown/move/up），不是直接改
  //     Vue 的状态 —— 后者会把 `onMarkerMove` 里那套换算整个跳过。
  const stage = page.locator('.osd-stage');
  const marker0 = page.locator('.osd-marker').first();

  // ⚠️ **量坐标之前必须把拖动区滚进视口，而且不能只滚 `#osd` 就了事。**
  // `page.mouse` 用的是视口坐标：目标跑到视口上方时，事件落在视口外、被
  // `<html>` 收走，**标记块一个 pointerdown 都收不到** —— 而报出来的症状是
  // "拖了但位置没变成自定义"，指向产品而不是探针。实测差得不多（`#osd` 滚到
  // 顶端之后 stage 还差 49 px 在视口外，标记块停在 y = −38.9），所以它是一个
  // 只在特定视口高度下出现的间歇失败，不是每次都炸。
  //
  // ⚠️ **第二条（2026-10-08 补）：顶对齐到 `top: 8px` 也不够 —— 那正好把
  // stage 塞进顶栏底下。** 顶栏是 `position: sticky; top: 0`，它**常驻**视口
  // 最上面那一条；于是 `mouse.move(mb.x, mb.y)` 抓到的不是标记块，是顶栏，
  // `pointerdown` 被顶栏吃掉、drag 从来没开始过 —— 而报出来还是"拖了但位置
  // 没变成自定义"那一句，一个字都不提顶栏。
  // **这一条是"侧栏/顶栏真的粘住"之后才露出来的**：修之前顶栏跟着内容滚出
  // 屏幕（top = −4698），那条带子是空的，`- 8` 碰巧能用。也就是说这是**正确的
  // 产品行为把一把尺子的隐含前提掀翻了**（陷阱 95 / 98 的同一族）。
  // 做法：量出顶上那条恒常覆盖带实际有多高，让 stage 顶从它**下面**过，
  // 而不是拿一个写死的 8px 去赌它不存在。
  await page.evaluate(() => {
    const stage = document.querySelector('.osd-stage');
    if (!stage) return;
    // 谁盖住了视口最上面那一条：凡贴到顶（`top <= 1`）又伸下来的都算。
    // `height` 那道守卫是为了把左侧竖栏（它也是从 0 开始的）排除掉 ——
    // 它不横跨工作区，够不着 stage。
    let cover = 0;
    for (const el of document.querySelectorAll('.topbar, header')) {
      const r = el.getBoundingClientRect();
      if (r.top <= 1 && r.bottom > cover && r.height < window.innerHeight / 2) {
        cover = r.bottom;
      }
    }
    window.scrollBy(0, stage.getBoundingClientRect().top - (cover + 8));
  });
  await sleep(200);

  const sb = await stage.boundingBox();
  const mb = await marker0.boundingBox();
  check(!!(sb && mb) && sb.width > 100, `拖动区有尺寸（${sb?.width}×${sb?.height}）`);
  // 兜一条"探针真的够得着它"：够不着的时候下面所有拖动断言都会失败，而那一条
  // 失败**读起来像产品坏了**。分开报，免得下次又去查一个没坏的东西。
  //
  // 要问的**不是"标记块在视口里吗"**（那只是必要条件，而且实测它独自一个是
  // 会骗人的：标记块明明在视口里、`y` 也是正的，`pointerdown` 却全被顶上那一层
  // 吃掉）。要问的是**"瞄准的那个点，命中的是谁"** —— `elementFromPoint` 直接把
  // 盖在上面那一层报出来，失败信息里就写着它的名字。
  const vh = page.viewportSize()?.height ?? 0;
  const hit = mb
    ? await page.evaluate(([x, y]) => {
        const el = document.elementFromPoint(x, y);
        if (!el) return 'null';
        const cls = typeof el.className === 'string' ? el.className.split(' ')[0] : '';
        return cls ? `${el.tagName.toLowerCase()}.${cls}` : el.tagName.toLowerCase();
      }, [mb.x + 1, mb.y + 1])
    : 'null';
  check(!!mb && mb.y >= 0 && mb.y + mb.height <= vh && mb.x >= 0 && hit.includes('osd-marker'),
        `鼠标瞄的那个点真的落在标记块上（y=${mb?.y?.toFixed(1)}，视口高 ${vh}，命中 ${hit}）`);
  const tx = sb.x + sb.width * 0.08;
  const ty = sb.y + sb.height * 0.1;
  await page.mouse.move(mb.x, mb.y);           // 抓住文字左上角 ⇒ 位移量 dx/dy = 0
  await page.mouse.down();
  await page.mouse.move(mb.x + 14, mb.y + 14, { steps: 4 });  // 先动一下，确保 pointermove 真的到了
  await page.mouse.move(tx, ty, { steps: 15 });
  await page.mouse.up();

  const card0 = cards.first();
  const pos0 = await itemCtl(card0, '位置', 'select').inputValue();
  check(pos0 === 'custom', `拖动把第 1 项的位置变成了自定义（实得 ${pos0}）`);
  const xv = Number(await itemCtl(card0, '横向 %', 'input').inputValue());
  const yv = Number(await itemCtl(card0, '纵向 %', 'input').inputValue());
  const mb2 = await marker0.boundingBox();
  const ex = Math.round(((mb2.x - sb.x) * 100) / sb.width);
  const ey = Math.round(((mb2.y - sb.y) * 100) / sb.height);
  // 判"输入框里的数 == 标记现在在哪"：这两条合起来才是"预览不撒谎"。
  // ±1 是留给亚像素取整的，不是留给公式差异的。
  check(Math.abs(xv - ex) <= 1, `横向 % 与标记实际位置一致（输入 ${xv}，量得 ${ex}）`);
  check(Math.abs(yv - ey) <= 1, `纵向 % 与标记实际位置一致（输入 ${yv}，量得 ${ey}）`);
  check(Math.abs(xv - 8) <= 2 && Math.abs(yv - 10) <= 2,
        `确实拖到了左上角附近（${xv}% / ${yv}%，目标 8% / 10%）`);

  // 5.2 加一项、关一项、再加到满
  await page.locator('button', { hasText: '加一项文字水印' }).click();
  check(await cards.count() === N0 + 1, `「加一项」之后变成 ${N0 + 1} 项`);
  await cards.nth(N0).locator('button', { hasText: '关闭' }).click();
  check(await cards.nth(N0).locator('input[type="checkbox"]').first().isChecked() === false,
        '「关闭」之后那一项的开关是关的');
  check(await cards.count() === N0 + 1,
        '「关闭」**不会**把项从列表里去掉（关掉和删掉是两件事）');
  await page.locator('button', { hasText: '加一项文字水印' }).click();
  check(await cards.count() === 4, `再加一项到 4 项（实得 ${await cards.count()}）`);
  check(await page.locator('button', { hasText: '加一项文字水印' }).isDisabled(),
        '满 4 项时「加一项」被禁用（后端 kMaxItems = 4）');

  // 5.3 存 → 发出去的请求体 → 后端读回来
  const before5 = posts.length;
  await page.locator('button[type="submit"]').first().click();
  for (let i = 0; i < 60 && posts.length === before5; i++) await sleep(100);
  check(posts.length === before5 + 1, '点保存发出了 POST /api/config');
  const osdSent = posts[posts.length - 1]?.osd;
  check(Array.isArray(osdSent?.items) && osdSent.items.length === 4,
        `请求体里有 4 项水印（实得 ${osdSent?.items?.length}）`);
  check(osdSent?.items?.[0]?.position === 'custom',
        `第 1 项 position=custom（实得 ${osdSent?.items?.[0]?.position}）`);
  check(osdSent?.items?.[0]?.x_pct === xv && osdSent?.items?.[0]?.y_pct === yv,
        `第 1 项的 x/y 就是拖出来的那两个数（发出去 ${osdSent?.items?.[0]?.x_pct}/` +
          `${osdSent?.items?.[0]?.y_pct}，界面显示 ${xv}/${yv}）`);
  check(osdSent?.items?.[N0]?.enabled === false, '关掉的那一项是带 enabled=false 发出去的');
  await sleep(900);
  const osdBack = (await getConfig(cookie))?.osd;
  check(osdBack?.items?.length === 4, `GET /api/config 读回 4 项（实得 ${osdBack?.items?.length}）`);
  check(osdBack?.items?.[0]?.position === 'custom' &&
        osdBack.items[0].x_pct === xv && osdBack.items[0].y_pct === yv,
        `读回来的第 1 项与发出去的一致（${JSON.stringify(osdBack?.items?.[0])}）`);
  check(osdBack?.items?.[N0]?.enabled === false, '读回来那一项也是关着的');

  // 5.4 删一项 —— 与「关闭」相反，这一下是不可逆的，必须是**整体替换**
  await cards.nth(N0).locator('button', { hasText: '移除' }).click();
  check(await cards.count() === 3, `「移除」把项从列表里去掉了（实得 ${await cards.count()}）`);
  const before6 = posts.length;
  await page.locator('button[type="submit"]').first().click();
  for (let i = 0; i < 60 && posts.length === before6; i++) await sleep(100);
  await sleep(900);
  const osdBack2 = (await getConfig(cookie))?.osd;
  check(osdBack2?.items?.length === 3,
        `删掉之后后端只剩 3 项（实得 ${osdBack2?.items?.length}）—— 列表是整体替换的，` +
        '删掉的项不会在下一份配置里又冒出来');

  // ---- 6. 音视频：麦克风下拉 = /api/audio/devices 的 capture ---------
  //
  // 这一段要回答的是"那个下拉框到底接没接上后端"。判据写成**两边对账**，
  // 而不是"至少有一个选项"：后者在接口坏掉、列表退回空数组时**照样绿**
  // （那时界面上只剩下前端自己加的那一项「默认设备」），而"勾了麦克风却
  // 一台设备都挑不到"正是这个功能最不该有的样子 —— 陷阱 60 的形状。
  console.log('\n[6] 音视频：麦克风下拉 = /api/audio/devices 的 capture');
  const devResp = await fetch(`${BASE}/api/audio/devices`,
                              { headers: { Cookie: `d2o_session=${cookie}` } });
  const devs = await devResp.json();
  // 报"实得什么"的时候别用 `typeof`：JS 里 `typeof []` 就是 `'object'`，那是句
  // 自相矛盾的话（通过的那次会打出"是数组（实得 object）"）。要报就报**实际拿到的东西**。
  check(Array.isArray(devs?.capture),
        `GET /api/audio/devices 的 capture 是数组` +
        (Array.isArray(devs?.capture) ? `（${devs.capture.length} 台）`
                                      : `（实得 ${JSON.stringify(devs?.capture)?.slice(0, 60)}）`));
  const capIds = (Array.isArray(devs?.capture) ? devs.capture : []).map((d) => d.id);

  // **用菜单点进去**，不写 `goto('/config#audio')`：菜单那一行和 `id="audio"`
  // 那个段落是同一次改动里一起进来的，走菜单顺带验了"点了真的落到这一段"。
  await page.locator('.side-link').filter({ hasText: '音视频' }).first().click();
  const audio = page.locator('#audio');
  await audio.waitFor({ state: 'visible', timeout: 20000 });

  // **先单独判 URL 的 hash。** 这一条是原来缺掉的鉴别器：菜单项写错锚点时 URL
  // 变了而页面纹丝不动，和"锚点对了、只是滚动没生效"是两件完全不同的事，
  // 而下面那条落点断言在两种情况下报的都是同一个 false。
  const hashAudio = await page.evaluate(() => location.hash);
  check(hashAudio === '#audio',
        `点菜单「音视频」之后 URL 的 hash 是 #audio（实得 ${JSON.stringify(hashAudio)}）`,
        '菜单项指向一个不存在的锚点时，URL 变了而页面纹丝不动 —— ' +
        '那和"这一段没做"在界面上长得一模一样');

  // 再判落点，而且**轮询到位置稳定为止**：两条滚动链（router 的 scrollBehavior
  // 与 ConfigView 里那个 watch）都是 `behavior: 'smooth'`，固定睡 400 ms 时
  // "还在滚"和"压根没滚"报出来是同一个 false。这里把中途每一次 scrollY 都记下来
  // 一起报出去 —— 轨迹是一条从 0 爬到目标值的线，还是从头到尾一个数，一眼可辨。
  const settle = await page.evaluate(async () => {
    const samples = [];
    let last = -1;
    const t0 = performance.now();
    for (let i = 0; i < 60; i++) {
      const el = document.querySelector('#audio');
      const r = el ? el.getBoundingClientRect() : null;
      const y = Math.round(window.scrollY);
      samples.push({ t: Math.round(performance.now() - t0), y, top: r ? Math.round(r.top) : null });
      if (y === last && i >= 3) break;
      last = y;
      await new Promise((res) => setTimeout(res, 50));
    }
    const el = document.querySelector('#audio');
    const r = el ? el.getBoundingClientRect() : null;
    // 上界宽容一点：顶栏是 sticky 的，锚点滚上来之后它的 top 允许被顶栏压住
    // 一小截。真正要排除的是"top 还在视口下方很远" —— 那才是没滚。
    return {
      elapsedMs: Math.round(performance.now() - t0),
      samples: samples.length,
      path: samples.map((s) => `${s.t}:${s.y}`).join(' '),
      top: r ? Math.round(r.top) : null,
      vh: window.innerHeight,
      scrollY: Math.round(window.scrollY),
      inView: r ? r.top >= -120 && r.top <= window.innerHeight : null,
    };
  });
  check(settle.inView === true,
        `点菜单「音视频」之后那一段滚进了视口（top=${settle.top} 视口高=${settle.vh} ` +
        `scrollY=${settle.scrollY}，${settle.elapsedMs}ms / ${settle.samples} 次采样后位置不再变）`,
        `滚动轨迹（毫秒:scrollY）${settle.path}`);

  const micSel = audio
    .locator('label.field')
    .filter({ has: page.locator('.field-label', { hasText: '麦克风设备' }) })
    .locator('select');
  check(await micSel.count() === 1, `麦克风下拉在（找到 ${await micSel.count()} 个 select）`);
  const optVals = await micSel.locator('option').evaluateAll((els) => els.map((e) => e.value));
  check(optVals[0] === '',
        `第一项是「默认设备（跟随系统）」、value 是空串（实得 ${JSON.stringify(optVals[0])}）`,
        '空值 = 跟着系统走。后端要是也回一个空 id，两个 option 会撞在同一个 value 上');
  // 存着的那台设备现在枚举不到时，前端会**额外**把它留在列表里
  // （否则 `<select>` 会自己跳到第一项 —— 一次静默改写）。
  const cfgAudio = (await getConfig(cookie))?.audio || {};
  const missing = !!cfgAudio.device && !capIds.includes(cfgAudio.device);
  const gotNonEmpty = optVals.filter((v) => v !== '');
  const wantNonEmpty = missing ? capIds.concat([cfgAudio.device]) : capIds;
  check(gotNonEmpty.length === wantNonEmpty.length &&
        gotNonEmpty.every((v, i) => v === wantNonEmpty[i]),
        `下拉里 ${gotNonEmpty.length} 台设备与接口返回的 ${capIds.length} 台一致` +
        (missing ? '（外加一台枚举不到的旧设备）' : ''),
        `接口 ${JSON.stringify(capIds)}，界面 ${JSON.stringify(gotNonEmpty)}`);

  // 陷阱 79 的那句话必须在**打开的那一刻**就在眼前。默认是关的，所以横幅
  // 此刻不该在；勾上之后必须出现，而且要点名"先关掉音频试试"。
  const audioOnLabel = audio
    .locator('label.field.check')
    .filter({ has: page.locator('.field-label', { hasText: '录制音频' }) });
  const audioOn = audioOnLabel.locator('input[type="checkbox"]');
  check(await audioOn.isChecked() === false,
        '音频默认是关的（「录制音频」没勾）',
        '默认打开会改变每一台设备的 SDP，而真实 NVR 收不收只能实测 —— ' +
        '这个决定不该替用户做');
  check(await audio.locator('.banner.is-warn').count() === 0,
        '还没打开，所以那条 SDP 警示横幅也不该在');
  // **点 label，不点 input**：这个复选框是自绘的（`.track` 压在原生 input 上），
  // Playwright 的 `check()` 会以 input 为命中目标，于是报"`.track` 拦截了指针
  // 事件" —— 那不是产品坏了，是这一格本来就是"点哪都能勾"的。
  await audioOnLabel.first().scrollIntoViewIfNeeded();
  await sleep(200);
  await audioOnLabel.first().click();
  check(await audioOn.isChecked() === true,
        '点一下把「录制音频」勾上了（自绘复选框：点标签哪一处都行）');
  const warn = audio.locator('.banner.is-warn').first();
  const warnVisible = await warn.count() > 0 && await warn.isVisible();
  const warnText = warnVisible ? (await warn.innerText()).trim() : '';
  check(warnVisible && /关掉音频/.test(warnText),
        `勾上「录制音频」后出现警示横幅并点名「先关掉音频试试」：` +
        `${JSON.stringify(warnText.slice(0, 60))}`,
        '打开音频会改变这一路设备的 SDP，而现场的表现是"加了音频就没画面" —— ' +
        '归因会稳稳落到编码器上，靠文档拦不住');
  // 音频是**冷**字段（要重开采集设备），所以这一段必须有自己的生效按钮。
  check(await audio.locator('button', { hasText: '保存并立即应用' }).count() === 1,
        '这一段有「保存并立即应用」（音频是冷字段，只保存不重开设备是没用的）');
  // 上面那次勾选是**没保存**的：这一段里一个 POST 都不该发出去。
  check(posts.length === before6 + 1,
        `这一段只发过 [5.4] 那一次保存（实发 ${posts.length - before6} 次）`,
        '只是勾一下复选框就自动存盘的话，用户改到一半的中间状态会被写进配置');

  // ---- 7. 时间设置：那两格必须**自己走** -----------------------------
  //
  // 用户报的原话：设备时间那一格"不会实时变化，理论上应该要实时变化，这样就可以
  // 发现时间是和其他时间准确对准了"。所以这一段量的是**它有没有在走** ——
  // 而这是这个探针里唯一一条**必须隔一段时间才能量**的判据。
  //
  // 判据的形状（为什么是"差 2~4 秒"而不是"不等于 0"）：读两次、中间睡 2.5 秒，
  // 拿两次的读数相减。**冻住的话差恰好是 0**，而正常走的差是 2 或 3
  // （2.5 秒里跨过两个还是三个整秒，取决于起点落在秒内的哪一处 —— 那是随机的，
  // 所以判据给的是区间，不是等号：陷阱 96 那一族，一条会随抽样运气变红的断言
  // 不能单用）。区间由**实测的间隔**推出来，不写死，见下。
  console.log('\n[7] 时间设置：主机/设备时间那两格自己走');
  await page.locator('.side-link').filter({ hasText: '时间设置' }).first().click();
  const hashTime = await page.evaluate(() => location.hash);
  check(hashTime === '#time',
        `点菜单「时间设置」之后 URL 的 hash 是 #time（实得 ${JSON.stringify(hashTime)}）`,
        '与 [6] 同一个鉴别器：URL 变了而段落没出现，和"这一段没做"报的是同一个 false');

  // 后端契约里那两条**结构性**不变量。它们不是"顺手测一下 API"，而是上面那两格
  // 之所以敢按"主机 + 偏移"推设备时间的**全部依据** —— 前端那个会走的钟就是照
  // 这两条关系写的，所以后端一旦不满足，界面上的读数会以一种看起来很正常的方式
  // 错掉（陷阱 85 那一族要防的正是这个）。
  const timeResp = await fetch(`${BASE}/api/time`,
                               { headers: { Cookie: `d2o_session=${cookie}` } });
  const tnow = await timeResp.json();
  const hostMsOk = typeof tnow?.host_now_ms === 'number' && tnow.host_now_ms > 0;
  check(hostMsOk && Math.floor(tnow.host_now_ms / 1000) === tnow.host_now,
        `host_now_ms 与 host_now 是同一个时刻的两种单位` +
        `（${tnow?.host_now_ms} / 1000 → ${hostMsOk ? Math.floor(tnow.host_now_ms / 1000) : '—'}，` +
        `host_now=${tnow?.host_now}）`,
        '这两个数由后端**同一次** system_clock::now() 换算而来。分别取两次的话，' +
        '页面上那个会走的钟就有了两个互相矛盾的基准');
  check(tnow?.device_now - tnow?.host_now === tnow?.offset_s,
        `device_now − host_now === offset_s（${tnow?.device_now} − ${tnow?.host_now} = ` +
        `${tnow?.device_now - tnow?.host_now}，offset_s=${tnow?.offset_s}）`,
        '前端按"主机 + 偏移"推设备时间。后端要是不满足这一条，' +
        '界面上两格之间的差会和"偏移"那一格对不上');

  // **两个读数必须在同一次 evaluate 里取。** 分两次 `inputValue()` 的话，中间那次
  // 往返有几毫秒，正好跨过一个整秒边界时两格会来自相邻两拍（一格的秒差 1），
  // 于是"两格之差 == 偏移"这条**结构性**断言会以一个约 1% 的概率随机变红 ——
  // 而那读起来像"偏移算错了"。同一个同步块里取则一拍都插不进来。
  const GRAB = () => {
    const grab = (label) => {
      for (const l of document.querySelectorAll('label.field')) {
        const s = l.querySelector('.field-label');
        if (s && s.textContent.trim() === label) {
          const i = l.querySelector('input');
          return i ? i.value.trim() : null;
        }
      }
      return null;
    };
    return { host: grab('主机时间'), device: grab('设备时间') };
  };
  // 先等到真的读到数（此前是「读取中…」）。`v-if="timeError"` 那一支下这两格
  // 根本不存在，所以这里取不到就直接报出来，而不是拿 null 去做减法。
  const stampRe = /^(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})$/;
  const toSec = (s) => {
    const m = stampRe.exec(s || '');
    return m ? Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]) / 1000 : null;
  };
  let A0 = null;
  for (let i = 0; i < 60; i++) {
    A0 = await page.evaluate(GRAB);
    if (toSec(A0.host) !== null && toSec(A0.device) !== null) break;
    await sleep(200);
  }
  const hA = toSec(A0?.host), dA = toSec(A0?.device);
  check(hA !== null && dA !== null,
        `两格都读出了时刻（主机 ${JSON.stringify(A0?.host)} / 设备 ${JSON.stringify(A0?.device)}）`,
        '老后端没有 /api/time 时这一段整块不渲染，那属于"这一段读不出来"，' +
        '不是"钟不走" —— 但两种都不该在这里静默跳过');

  if (hA !== null && dA !== null) {
    check(dA - hA === tnow.offset_s,
          `此刻设备时间 − 主机时间 === offset_s（${dA - hA} vs ${tnow.offset_s}）`,
          '两格是同一拍渲染的，所以这条应当是精确相等；差 1 就说明它们各自' +
          '独立地走、而不是按同一个偏移推出来的');

    const t0 = Date.now();
    await sleep(2500);
    const B = await page.evaluate(GRAB);
    const elapsed = (Date.now() - t0) / 1000;
    const hB = toSec(B?.host), dB = toSec(B?.device);

    check(hB !== null && dB !== null,
          `2.5 秒之后两格仍然读得出来（主机 ${JSON.stringify(B?.host)} / ` +
          `设备 ${JSON.stringify(B?.device)}）`);

    if (hB !== null && dB !== null) {
      // 期望的秒数差 = 实测间隔（取整），容差 ±1：跨过几个整秒取决于起点在秒内
      // 的位置，那是随机的。**冻住的那一版差恰好是 0**，离这个区间很远 ——
      // 所以这条断言既不会因为运气变红，也不会把"没走"放过去。
      const want = Math.round(elapsed);
      const lo = Math.max(1, want - 1), hi = want + 1;
      const dDev = dB - dA, dHost = hB - hA;
      check(dDev >= lo && dDev <= hi,
            `设备时间在这一段里走了 ${dDev} 秒（实测间隔 ${elapsed.toFixed(2)} 秒，` +
            `期望 ${lo}~${hi}）—— ${A0.device} → ${B.device}`,
            '**冻住的那一版这里恰好是 0**：界面上那个数只在进页面/点重新读取时' +
            '被写过一次，之后没有任何东西推进它。而这一段要回答的正是"此刻对准了' +
            '没有"，一个不走的数在这里不是"略旧"，是错的');
      check(dHost >= lo && dHost <= hi,
            `主机时间也走了 ${dHost} 秒（${A0.host} → ${B.host}）`,
            '两格由同一个钟驱动。只判设备那一格的话，' +
            '"主机格冻着、设备格在走"会被放过去');
      // 两格一起走的一个**结构性**后果：它们的差在整个观测窗口里不许变。
      // 这一条不依赖任何绝对时间，所以它不受上面那个区间的影响。
      check((dB - hB) === (dA - hA),
            `两格之差全程没变（${dA - hA} → ${dB - hB}）`,
            '两格各自按自己的节奏走的话，差会随时间漂 —— 而"偏移"那一格显示的' +
            '是这段时间里固定的一个数，两者很快就会对不上');
    }
  }

  await page.screenshot({ path: path.join(outDir, 'config_probe.png'), fullPage: true });
  console.log(`[probe] 截图 ${path.join(outDir, 'config_probe.png')}`);

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
