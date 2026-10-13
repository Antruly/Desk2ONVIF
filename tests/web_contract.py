#!/usr/bin/env python3
"""Web 层契约检查：静态产物可服务 + /api/config 往返 + /api/status 字段。

为什么值得单独一个脚本：这几类错的**症状全是"页面上一处空白"**，
控制台一句错都不报。

  - dist 重建过、chunk 的 hash 变了，而 index.html 引用的还是旧名字
    → 后端安静地 200 一个不存在的文件名 / 或者 SPA 回落把 404 变成 200 + HTML
    → 浏览器拿一段 HTML 当 JS 解析 → 白屏
  - 后端加/改字段而前端读的名字没跟上 → 那一格显示 undefined
  - 新字段只写进了 to_json 而没进 from_json（或反过来）→ 存了不生效 / 取不到

这里逐条对账，不靠肉眼看。

用法:
    python -I -X utf8 tests/web_contract.py --port 8099
                                            [--user admin] [--password admin]

第 [1] 段是**匿名**跑的（它验的就是静态产物不被登录挡住），第 [2]、[3] 段
会先登录再打 `/api/*`。两条路刻意分开：顺手给静态那一段也带上 cookie 的话，
"静态资源匿名可达"这条就再也验不到了 —— 而它恰恰是登录层最容易踩坏的地方。
"""

import argparse
import http.client
import json
import os
import re
import socket
import sys
import time

NS = "http://www.onvif.org/ver10/media/wsdl"
COOKIE = "d2o_session"


class R:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, ok, what, detail=""):
        if ok:
            self.passed += 1
            print(f"  [OK]   {what}")
        else:
            self.failed += 1
            print(f"  [FAIL] {what}")
            if detail:
                for line in str(detail).splitlines():
                    print(f"         {line}")
        return ok


def get(port, path, cookie=None):
    hd = {"Cookie": cookie} if cookie else {}
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request("GET", path, headers=hd)
    r = c.getresponse()
    body = r.read()
    return r.status, dict((k.lower(), v) for k, v in r.getheaders()), body


def post(port, path, obj, cookie=None):
    body = json.dumps(obj).encode("utf-8")
    hd = {"Content-Type": "application/json",
          "Content-Length": str(len(body))}
    if cookie:
        hd["Cookie"] = cookie
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request("POST", path, body, hd)
    r = c.getresponse()
    return r.status, dict((k.lower(), v) for k, v in r.getheaders()), r.read()


def login(port, user, password):
    """登录并返回 cookie 串；服务端没开登录时返回 None。

    先问 /api/session（**匿名可达**）再决定要不要登录：对着一个
    `--no-http-auth` 起的服务硬登录，会把"没开登录"报成"登录失败"。
    """
    st, _, body = get(port, "/api/session")
    if st != 200:
        raise RuntimeError(f"/api/session → {st}，后端没起来或不是这个版本")
    sess = json.loads(body)
    if not sess.get("auth_required"):
        print("[auth] 服务端没开登录（auth_required 不是 true），/api/* 不带 cookie")
        return None
    st, hd, body = post(port, "/api/login", {"user": user, "password": password})
    if st != 200:
        raise RuntimeError(f"登录失败（HTTP {st}）：{body[:200]!r} —— "
                           f"默认 admin/admin，用 --user/--password 覆盖")
    raw = None
    for k, v in hd.items():
        if k == "set-cookie" and v.startswith(COOKIE + "="):
            raw = v.split(";", 1)[0]
    if raw is None:
        raise RuntimeError("登录返回 200 但没有 d2o_session cookie")
    print(f"[auth] 已登录：{user}")
    return raw


def ws_first_frame(port, cookie):
    """连上 /ws/stats 并读回第一帧 JSON。

    握手和帧解析**直接复用 ws_probe.py**（同目录，import 无副作用）——
    自己再写一份的话，两份实现迟早会在 RFC 6455 的哪个细节上不一致，
    而那时失败信息指向的是探针而不是被测对象。
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from ws_probe import handshake, read_frame  # noqa: PLC0415 - 同目录探针，故意延迟导入

    sock = socket.create_connection(("127.0.0.1", port), timeout=20)
    try:
        pre = handshake(sock, "127.0.0.1", port, "/ws/stats", cookie=cookie)
        # 首帧是后端在登记之后立刻推的那一份（不等下一个整秒），所以这里
        # 读一帧就够，不需要挑时点。
        while True:
            opcode, payload, pre = read_frame(sock, pre)
            if opcode == 0x1:  # text
                return json.loads(payload.decode("utf-8"))
            if opcode == 0x8:  # close：把关闭码带出去，1008 说明是没登录
                raise RuntimeError(f"被关闭：{payload.hex()}")
    finally:
        sock.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--web-root", default="web/dist")
    ap.add_argument("--user", default="admin", help="Web 控制台用户名")
    ap.add_argument("--password", default="admin", help="Web 控制台口令")
    a = ap.parse_args()
    r = R()

    ck = login(a.port, a.user, a.password)

    def api_get(path):
        return get(a.port, path, cookie=ck)

    def api_post(path, obj):
        return post(a.port, path, obj, cookie=ck)

    # ---- 1. 静态产物：index.html 引用的 chunk 必须真的在磁盘上 ----------
    #
    # **这一整段故意不带 cookie。** 它验的就是"没登录的人也能把登录页本身
    # 下载下来"；顺手把 cookie 加上的话，这条断言会变成永远通过的空转。
    print("\n[1] 静态产物与 index.html 的引用逐一对账")
    st, hd, body = get(a.port, "/")
    r.check(st == 200, f"GET / → {st}")
    r.check("text/html" in hd.get("content-type", ""),
            f"Content-Type = {hd.get('content-type')}")
    html = body.decode("utf-8", "replace")
    refs = set(re.findall(r'(?:src|href)="(/assets/[^"]+)"', html))
    r.check(len(refs) > 0, f"index.html 引用了 {len(refs)} 个资源")

    # 磁盘基准。**先取它**，后面爬出来的名字要用它过滤。
    adir = os.path.join(a.web_root, "assets")
    on_disk = {"/assets/" + n for n in os.listdir(adir)} if os.path.isdir(adir) else set()
    if not on_disk:
        print(f"  [note] 读不到 {adir}：跳过『磁盘产物一个都不能少』这一项")

    # 入口 JS 里还有**动态 import** 出来的 chunk（三个视图页各一个）。
    # 它们不出现在 index.html 里，所以只查 index.html 会漏掉最主要的三个 ——
    # 而路由懒加载一旦 404 落到 SPA 回落上，症状就是"点进那个页面才白屏"。
    #
    # 从 JS 里认文件名而不是认路径：构建器三种写法都用得上 ——
    # `import("./StatusView-<hash>.js")`（相对）、`"assets/PreviewView-<hash>.css"`
    # （无前导斜杠）、`"/assets/index-<hash>.js"`（绝对）。统一按"引号/斜杠之后
    # 的文件名"匹配，再补回 /assets/ 前缀。
    #
    # **认出来的名字要和磁盘取交集**：minified 代码里任何以 .js/.css 结尾的字符串
    # 都会被抓到，不过滤的话会凭空造出几个不存在的引用，然后报一片假失败。
    # 反向的漏检由下面的"磁盘上一个都不能少"兜住。
    crawled = set()
    while True:
        todo = [x for x in sorted(refs) if x.endswith(".js") and x not in crawled]
        if not todo:
            break
        for ref in todo:
            crawled.add(ref)
            _, _, js = get(a.port, ref)
            for name in re.findall(r'["\'/]([A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:js|css))["\']',
                                   js.decode("utf-8", "replace")):
                cand = "/assets/" + name
                if not on_disk or cand in on_disk:
                    refs.add(cand)

    for ref in sorted(refs):
        st, hd, _ = get(a.port, ref)
        ct = hd.get("content-type", "")
        r.check(st == 200, f"{ref} → {st}")
        # **这一条是关键**：SPA 回落会把不存在的 .js 变成 200 + text/html，
        # 浏览器于是拿 HTML 当 JS 解析。状态码一样是 200，分不出对错。
        want = "javascript" if ref.endswith(".js") else "css"
        r.check(want in ct, f"{ref} Content-Type 含 {want}（实际 {ct}）")
        if ref.endswith(".js"):
            st2, _, b2 = get(a.port, ref)
            r.check(not b2.lstrip().startswith(b"<!DOCTYPE"),
                    f"{ref} 的内容是 JS 而不是回落的 HTML")

    # 磁盘上还有没有被引用到的产物吗？
    #
    # 这一条同时防两种错：dist 里有旧 hash 的残留（引用的名字对不上），
    # 以及**上面那个爬取器哪天静默失效**（正则不匹配了，于是一个 chunk 都验不到，
    # 而所有断言照样全绿）。用磁盘做基准，爬取器坏掉就会当场暴露。
    if on_disk:
        missing = sorted(on_disk - refs)
        r.check(not missing, f"磁盘上 {len(on_disk)} 个产物全被引用到",
                "未被引用（旧残留，或爬取器失效）：\n" + "\n".join(missing))

    # 页面路由走 SPA 回落
    for path in ("/config", "/preview"):
        st, hd, _ = get(a.port, path)
        r.check(st == 200 and "text/html" in hd.get("content-type", ""),
                f"{path} SPA 回落 → {st} {hd.get('content-type')}")

    # ---- 2. /api/config 往返（含子码流四个新字段）------------------------
    #
    # 从这一段开始要走鉴权。先顺手钉一条：**不带 cookie 时它必须是 401** ——
    # 下面所有断言都建立在"带上 cookie 才通"这个前提上，而这个前提本身
    # 一旦被谁改回公开，这里要能当场说清楚，而不是让失败看起来像字段写错。
    if ck:
        st, _, _ = get(a.port, "/api/config")
        r.check(st == 401, f"不带 cookie 的 GET /api/config → {st}（应当是 401）")

    print("\n[2] /api/config —— GET/POST 往返")
    st, _, body = api_get("/api/config")
    r.check(st == 200, f"GET /api/config → {st}")
    cfg = json.loads(body)
    rtsp = cfg.get("rtsp", {})
    for key, typ in (("enabled", bool), ("port", int), ("path", str),
                     ("sub_enabled", bool), ("sub_path", str),
                     ("sub_height", int), ("sub_bitrate_kbps", int)):
        r.check(key in rtsp, f"rtsp.{key} 存在")
        if key in rtsp:
            r.check(isinstance(rtsp[key], typ),
                    f"rtsp.{key} 类型是 {typ.__name__}（实际 {type(rtsp[key]).__name__}）")

    # 「已保存但还没生效」的清单必须挂在 GET 上。只放在 POST 的回应里的话，
    # 用户存完刷新一下页面提示就没了，而表单里显示的又都是新值 —— 看起来
    # 一切已生效。这一条验的就是"它活得比那次请求久"。
    r.check(isinstance(cfg.get("pending"), list),
            f"GET /api/config 带 pending 数组（实际 {type(cfg.get('pending')).__name__}）")

    # 改一个子码流的值再 POST 回来，确认它真的进了回应（而不是被静默丢掉）。
    #
    # ⚠️ **不能写死一个 288 / 384 就发过去。** 判据是"改一个**不同**的值才进
    # pending"，而子码流高度本来就可能已经是 288（那就是它的默认值）—— 那样
    # 这一次 POST 什么都没改、pending 自然是空的，而下面的断言会把它报成
    # "冷字段没进 pending"，指向完全反了。这是陷阱 60 的同一个形状：一条依赖
    # "环境碰巧不等于某个值"的断言，换台机器就恒真或恒假。
    #
    # 所以先把当前值读出来，再挑一个**不同**的。
    before = dict(rtsp)
    h_cands = [240, 288, 360, 144, 480]
    b_cands = [384, 512, 768, 256, 1024]
    h_new = next(v for v in h_cands if v != before.get("sub_height"))
    b_new = next(v for v in b_cands if v != before.get("sub_bitrate_kbps"))

    res, after_all, after, st_post = {}, {}, {}, None
    for _ in range(2):
        rtsp["sub_height"] = h_new
        rtsp["sub_bitrate_kbps"] = b_new
        st_post, _, body = api_post("/api/config", cfg)
        try:
            res = json.loads(body)
        except ValueError:
            res = {}
        st, _, body = api_get("/api/config")
        after_all = json.loads(body)
        after = after_all.get("rtsp", {})
        if "rtsp.sub_height" in (res.get("pending") or []):
            break
        # 没进 pending ⇒ 挑中的值恰好等于**运行值**。pending 比的是"保存值 vs
        # 运行值"，而不是"这次改了没有"：上一次跑这个测试 POST 进去的值会一直
        # 留在盘上，而进程没重启，于是"改成 384"这一次改的是保存值、运行的还是
        # 那个老值 —— 这一点在只跑一次的机器上永远看不到。
        # 此刻保存值已经是 h_new 了，换一个更远的必然和运行值不同。
        h_new, b_new = (360, 768) if h_new != 360 else (480, 1024)

    r.check(st_post == 200, f"POST /api/config → {st_post}")
    # 回应必须是**逐字段**的：一次保存里常常"这项当场生效、那项要重启"，
    # 只给一个 restart_required 布尔值时前端只能二选一，必有一种是在骗人。
    r.check(isinstance(res.get("applied"), list) and isinstance(res.get("pending"), list),
            f"POST 回应里有 applied/pending 两个数组（实际 "
            f"{type(res.get('applied')).__name__}/{type(res.get('pending')).__name__}）")
    # 子码流高度是**冷**字段：改它要重建编码器，所以它必须出现在 pending 里，
    # 而且**不能**出现在 applied 里 —— 报成已生效就是在骗人。
    r.check("rtsp.sub_height" in (res.get("pending") or []),
            f"rtsp.sub_height（改成 {h_new}）进了 pending：{res.get('pending')}")
    r.check("rtsp.sub_height" not in (res.get("applied") or []),
            f"且没进 applied：{res.get('applied')}")
    r.check(res.get("restart_required") is True,
            f"有 pending 时 restart_required 为 true（实际 {res.get('restart_required')}）")

    r.check(after.get("sub_height") == h_new,
            f"sub_height 往返 {before.get('sub_height')} → {after.get('sub_height')}（设的是 {h_new}）")
    r.check(after.get("sub_bitrate_kbps") == b_new,
            f"sub_bitrate_kbps 往返 {before.get('sub_bitrate_kbps')} → "
            f"{after.get('sub_bitrate_kbps')}（设的是 {b_new}）")
    # 刚 POST 进去的那一项，在紧接着的 GET 里也必须在 pending 里 —— 这一条把
    # "GET 上那个数组"和"POST 回应的那个数组"接起来，防止它们各说各话。
    r.check("rtsp.sub_height" in (after_all.get("pending") or []),
            f"紧接着的 GET 里 pending 仍含 rtsp.sub_height：{after_all.get('pending')}")

    # ---- 2b. 设备标识只读（硬件 ID / 序列号 / 设备 UUID）-----------------
    #
    # 用户明确选的取舍：这三个值装好之后**固定死**，网页上不许改 —— NVR 拿
    # 设备 UUID 当主键，一改那边就是"多出一台新摄像机"、原来那台永远离线。
    #
    # **正反面必须一起判。** 只判"改了没变"的话，把整个 `onvif` 段都冻上、
    # 甚至把 `apply_config` 写成"什么都不采纳"也能全绿（陷阱 85 的形状）。
    # 所以同一次 POST 里还改了两个**应当能改**的字段，它们必须真的变。
    print("\n[2b] 设备标识只读 —— 改不动，但同一次请求里别的字段照样能改")
    st, _, body = api_get("/api/config")
    ident = json.loads(body).get("onvif", {})
    locked_keys = ("hardware_id", "serial_number", "device_uuid")
    # 先量"读得到"。少了这一条，下面那句"值没变"在**请求失败**时也是绿的 ——
    # 那时读到的是空字典，什么都没变，当然"没变"（陷阱 91）。
    r.check(all(k in ident for k in locked_keys),
            "GET /api/config 的 onvif 段里有这三个字段",
            f"缺：{[k for k in locked_keys if k not in ident]}（实际键 {sorted(ident)}）")
    # 序列号与 UUID 都应当是**启动时固化下来的非空值** —— 空的就意味着"固化"
    # 这件事没发生，而空值恰好也是"改不动"的一个平凡解，分不出来。
    r.check(bool(ident.get("serial_number")) and bool(ident.get("device_uuid")),
            f"serial_number / device_uuid 都非空（{ident.get('serial_number')!r} / "
            f"{ident.get('device_uuid')!r}）",
            "空 = 启动时那次固化没落盘。而「锁住一个空值」是平凡通过。")

    hw_before, sn_before, uuid_before = (ident.get("hardware_id"),
                                         ident.get("serial_number"),
                                         ident.get("device_uuid"))
    model_before = ident.get("model")
    name_before = ident.get("device_name")
    cfg2 = json.loads(body)
    cfg2["onvif"]["hardware_id"] = "9.9"
    cfg2["onvif"]["serial_number"] = "HACKED0000000000"
    cfg2["onvif"]["device_uuid"] = "urn:uuid:deadbeef-0000-4000-8000-000000000000"
    # 反面对照：这两个**可改**（用户确认过 manufacturer/model/firmware_version/
    # device_name/location 仍然可改）。
    cfg2["onvif"]["model"] = "反面对照-型号"
    cfg2["onvif"]["device_name"] = "反面对照-名字"
    st_post, _, body = api_post("/api/config", cfg2)
    r.check(st_post == 200, f"POST 一个带设备标识改动的配置 → {st_post}")
    try:
        res2 = json.loads(body)
    except ValueError:
        res2 = {}
    got = (res2.get("config") or {}).get("onvif", {})
    r.check(got.get("hardware_id") == hw_before,
            f"硬件 ID 没被改掉（{hw_before!r} → {got.get('hardware_id')!r}）")
    r.check(got.get("serial_number") == sn_before,
            f"序列号没被改掉（{sn_before!r} → {got.get('serial_number')!r}）")
    r.check(got.get("device_uuid") == uuid_before,
            f"设备 UUID 没被改掉（{uuid_before!r} → {got.get('device_uuid')!r}）")

    # 反面对照：这两个必须**真的变了**。
    r.check(got.get("model") == "反面对照-型号",
            f"同一次请求里可改的 model 确实变了（实际 {got.get('model')!r}）",
            "没变说明锁得太宽 —— 整个 onvif 段被冻上了，那用户连设备名都改不了。")
    r.check(got.get("device_name") == "反面对照-名字",
            f"同一次请求里可改的 device_name 确实变了（实际 {got.get('device_name')!r}）")

    # **被拒绝要说出来。** 静默忽略的后果是用户看到"保存成功"、值却没变，
    # 只能以为程序坏了 —— 而下一个人读代码时会以为这个校验没做。
    note = res2.get("note") or ""
    r.check("未采纳" in note or "固定" in note,
            f"回应里说明了设备标识没被采纳：{note!r}")
    # 而且**不能**报成"已生效"：那是在骗人（值根本没变，用户下次刷新就发现了）。
    for k in locked_keys:
        r.check(f"onvif.{k}" not in (res2.get("applied") or []),
                f"onvif.{k} 没被报成 applied：{res2.get('applied')}")
        r.check(f"onvif.{k}" not in (res2.get("pending") or []),
                f"onvif.{k} 也没进 pending（它压根没被采纳）：{res2.get('pending')}")

    # 把反面对照改回去，别把测试用的字符串留在配置里。
    st, _, body = api_get("/api/config")
    cfg3 = json.loads(body)
    cfg3["onvif"]["model"] = model_before
    cfg3["onvif"]["device_name"] = name_before
    st_post, _, body = api_post("/api/config", cfg3)
    try:
        back = (json.loads(body).get("config") or {}).get("onvif", {})
    except ValueError:
        back = {}
    r.check(st_post == 200 and back.get("model") == model_before,
            f"反面对照已改回（model = {back.get('model')!r}）")

    # ---- 2c. 多通道（Phase AC 的 HTTP 一半）-------------------------------
    #
    # `tests/channels_check.cpp` 验的是 `validate_config` 的 notes 和边角情形，
    # **那些在 HTTP 上看不见** —— `apply_config` 把 notes 送进 `D2O_WARN` 就没了。
    # 这一段只验"从浏览器那边看得见的东西"：`channels` 这个键在什么情况下出现、
    # 值往不往返、超限有没有真的被截断。两个文件各管一半。
    print("\n[2c] 多通道配置：channels 键什么时候在、值往返、超限截断")
    st, _, body = api_get("/api/config")
    r.check(st == 200, f"GET /api/config → {st}")
    cfgc = json.loads(body)
    # 先证"读到了东西"：请求失败时 cfgc 是 {}，下面所有"键不在"都恒真（陷阱 91）。
    r.check("rtsp" in cfgc and "capture" in cfgc,
            "读到的配置里有 rtsp / capture（量具没瞎）",
            f"实际键：{sorted(cfgc)}")

    # 把通道清空，建立**已知的**起点 —— 上一次跑这个脚本留下的通道会一直在盘上，
    # 不清的话下面"清空之后键就不在"这一条在第二次跑时才红，而红的位置看起来
    # 像"省略逻辑坏了"。`items: []` 是明说清空（与"没写这个键"分得开，
    # 后者保留原值 —— 那一条由 channels_check.cpp 的 [2] 段钉着）。
    st_p, _, body = api_post("/api/config", {"channels": {"items": []}})
    r.check(st_p == 200, f"POST {{channels:{{items:[]}}}} → {st_p}")
    st, _, body = api_get("/api/config")
    cfgc = json.loads(body)
    r.check("rtsp" in cfgc, "清空之后再读一次，配置仍然是完整的（不是读失败）")
    r.check("channels" not in cfgc,
            "一项都没有时 GET /api/config **不出现** channels 键",
            "兼容边界：这个键的存在与否会被 diff_config 直接比较，多一个空数组"
            "就会让每台已装机器的配置在第一次启动时报一次『配置变了』。")

    # 写两条通道。**不写 name / rtsp_path**（让后端按序号补），
    # 而且故意把 fps 写成一个**不等于**任何档位的值 —— 这才验得出档位展开。
    tiers_c = [t for t in (cfgc.get("quality_tiers") or [])
               if isinstance(t, dict) and t.get("key") == "high"]
    if not tiers_c:
        print("  [note] 拿不到 high 档的定义，跳过档位展开那两条")
    two = [
        {"source": "display", "display_index": 1, "enabled": False},   # 关掉第 2 块屏
        {"source": "webcam", "camera": "", "quality": "high", "fps": 3},
    ]
    st_p, _, body = api_post("/api/config", {"channels": {"items": two}})
    r.check(st_p == 200, f"写两条通道（一条显示器、一条摄像头）→ {st_p}")
    try:
        res_c = json.loads(body)
    except ValueError:
        res_c = {}
    st, _, body = api_get("/api/config")
    got_c = (json.loads(body).get("channels") or {}).get("items")
    r.check(isinstance(got_c, list) and len(got_c) == 2,
            f"读回来是两条（实际 {len(got_c) if isinstance(got_c, list) else got_c!r}）")
    got_c = got_c if isinstance(got_c, list) else []
    # 读回来的这两条，除了逐项判据之外还有一个用处：它们的**键集**就是"后端往
    # 通道项里发哪些名字"的第一手证据，[4b] 拿它去认前端读的 `rtsp_path`。
    # 在这里收、而不是在 [4b] 现取一次配置，是因为 [2c] 末尾会把通道清空 ——
    # 那时再读就没有通道项可读了。
    ch_keys = set()
    for _it in got_c:
        if isinstance(_it, dict):
            ch_keys |= set(_it)
    if len(got_c) != 2:
        print("  [note] 条数不对，后面按项判的都跳过")
    else:
        d0, c1 = got_c[0], got_c[1]
        r.check(d0.get("source") == "display" and d0.get("display_index") == 1,
                f"第一条是显示器 1 的覆盖项（实际 {d0.get('source')!r} / "
                f"{d0.get('display_index')!r}）")
        r.check(d0.get("enabled") is False,
                f"而且 enabled:false 真的落盘了（实际 {d0.get('enabled')!r}）",
                "这是『关掉某个自动发现的显示器』唯一的手段。")
        r.check(d0.get("name") == "通道 2" and d0.get("rtsp_path") == "/ch2",
                f"没写的名字与路径按序号补上了（{d0.get('name')!r} / "
                f"{d0.get('rtsp_path')!r}）",
                "序号 = 在 items 里的位置 + 2：通道 1 不是 items 的一项。")
        r.check(c1.get("source") == "webcam" and c1.get("rtsp_path") == "/ch3",
                f"第二条是摄像头、默认路径 /ch3（实际 {c1.get('source')!r} / "
                f"{c1.get('rtsp_path')!r}）")
        # 档位展开：写进去的 fps=3 必须被 high 档的值覆盖掉，而且**不是**3。
        if tiers_c:
            t_hi = tiers_c[0]
            r.check(c1.get("quality") == "high",
                    f"档位标签原样保留（实际 {c1.get('quality')!r}）")
            r.check(c1.get("fps") == t_hi["fps"]
                    and c1.get("bitrate_kbps") == t_hi["bitrate_kbps"]
                    and c1.get("max_height") == t_hi["max_height"]
                    and bool(c1.get("sub_enabled")) == t_hi["sub_enabled"],
                    f"三项都被 high 档展开成 {t_hi['fps']}/{t_hi['bitrate_kbps']}/"
                    f"{t_hi['max_height']}（实际 {c1.get('fps')}/"
                    f"{c1.get('bitrate_kbps')}/{c1.get('max_height')}）",
                    f"写进去的是 fps=3。没被展开说明档位对通道不起作用 —— "
                    f"而『选了高性能但帧率还是 3』正是最难查的那一类。")
            r.check(c1.get("fps") != 3,
                    "而且那个 3 确实被改掉了（不是碰巧等于档位值）")

    # 超限截断：发 12 条，只该留下 8 条。
    many = [{"source": "display", "display_index": i, "rtsp_path": f"/m{i}"}
            for i in range(12)]
    st_p, _, body = api_post("/api/config", {"channels": {"items": many}})
    r.check(st_p == 200, f"发 12 条通道 → {st_p}")
    st, _, body = api_get("/api/config")
    n_kept = len(((json.loads(body).get("channels") or {}).get("items")) or [])
    r.check(n_kept == 8, f"只留下 8 条（实际 {n_kept}）",
            "上限是 8：每个通道一路独立编码器加一个 ONVIF 端口，再多只是让人"
            "一次性把机器配死、然后归因到『这软件卡』。")

    # 收尾：把通道清回空的。**必须清** —— 后面 [3] 的字段集对账、[4c] 的
    # applied/pending 对账、[5] 的自适应自洽都读这一份配置；盘上留着两条通道，
    # 那些段落的结果就取决于"这一段测试跑没跑过"。而且这一份是**盘上那一份**，
    # 会留给下一次跑本脚本（开头那一段就是为它写的"先清空、建立已知起点"）。
    #
    # （原注释写的是"这一轮 AD 还没做，运行期还不知道 channels 是什么" —— 那是
    #   当时的事实，AD 做完之后就不成立了；清空这个动作本身的理由一直有效。）
    st_p, _, body = api_post("/api/config", {"channels": {"items": []}})
    st, _, body = api_get("/api/config")
    r.check(st_p == 200 and "channels" not in json.loads(body),
            f"通道已清空、channels 键也随之消失（POST {st_p}）")

    # ---- 3. /api/status --------------------------------------------------
    print("\n[3] /api/status —— 逐路字段")
    st, _, body = api_get("/api/status")
    r.check(st == 200, f"GET /api/status → {st}")
    s = json.loads(body)
    for key in ("nalu_dropped", "max_nalu_bytes", "rtsp_clients", "rtsp_url",
                "video_width", "video_height",
                "sub_width", "sub_height", "rtsp_streams",
                "cursor_frames", "pointer_only_frames",
                "adaptive_dropped", "osd_refresh_frames", "osd_hot_applied",
                "drops_recent",
                "perf_state", "perf_duty", "perf_target_fps", "perf_hint",
                "perf_suggested_quality"):
        r.check(key in s, f"status.{key} 存在")
    streams = s.get("rtsp_streams")
    r.check(isinstance(streams, list), f"rtsp_streams 是数组（实际 {type(streams).__name__}）")
    if isinstance(streams, list) and streams:
        for i, e in enumerate(streams):
            for key in ("path", "url", "clients", "nalu_dropped",
                        "max_nalu_bytes", "packets", "bytes"):
                r.check(key in e, f"rtsp_streams[{i}].{key} 存在")
        # 前端 StatusView 读的 label 是按下标认的：0 主、1 子。
        r.check(streams[0].get("url", "").startswith("rtsp://"),
                f"第 0 路地址 = {streams[0].get('url')}")
        # 旧键必须和逐路那条对得上，否则同一个页面上两个数会打架。
        r.check(s.get("rtsp_url") == streams[0].get("url"),
                "rtsp_url 与 rtsp_streams[0].url 一致")
        r.check(s.get("rtsp_clients") == sum(e.get("clients", 0) for e in streams),
                "rtsp_clients 等于各路之和")
        r.check(s.get("nalu_dropped") == sum(e.get("nalu_dropped", 0) for e in streams),
                "nalu_dropped 等于各路之和")
        if len(streams) > 1:
            r.check(s.get("max_nalu_bytes") ==
                    max(e.get("max_nalu_bytes", 0) for e in streams),
                    "max_nalu_bytes 是各路的最大值（不是求和）")
            r.check(streams[0].get("path") != streams[1].get("path"),
                    f"两路路径不同：{streams[0].get('path')} / {streams[1].get('path')}")

    # ---- 4. /ws/stats 与 /api/status 必须是同一套字段 ---------------------
    #
    # 为什么非要有这一段，而不是满足于上面 [3] 已经对过账了：
    #
    # 前端读统计有**两条路** —— 开页面时 GET 一次 /api/status，之后靠 /ws/stats
    # 每秒推一帧。而它收到 WS 帧时做的是**整体替换**（useStats 里
    # `stats.value = normalizeStats(parsed)`，normalizeStats 逐字段兜底、返回
    # 一个完整的对象）。也就是说：**只要某个字段不在 WS 帧里，它就会在页面
    # 连上 WS 的下一秒被重置成初值**，GET 拿到的真值活不过一帧。
    #
    # 这正是实际发生过的：`preview_*` / `wsd_*` / `onvif_*` 当初只加在
    # `GET /api/status` 那个 handler 里，而 WS 那一帧由另一个函数
    # （`stats_json`）生成。于是预览页上"预览链路自检"长期显示
    # "分片产出 0 / 初始化段未产生"，状态页显示"WS-Discovery 没在监听" ——
    # 而**画面明明在播**。当时 [3] 段是绿的，因为它比的是 HTTP 那份，
    # 恰好是唯一没坏的那条路。
    #
    # 判据取"两边字段集相等"而不是"包含"：多出来的字段同样算漂移（说明有人
    # 只改了一边），而相等这条判据不需要维护一份白名单。
    print("\n[4] /ws/stats 的字段集与 /api/status 一致")
    ws_j = None
    try:
        ws_j = ws_first_frame(a.port, ck)
        r.check(isinstance(ws_j, dict) and len(ws_j) > 0,
                f"收到 WS 首帧，{len(ws_j) if isinstance(ws_j, dict) else 0} 个字段")
    except Exception as e:  # noqa: BLE001 - 探针里任何异常都只该变成一条失败
        r.check(False, f"读 /ws/stats 首帧：{type(e).__name__}: {e}")

    if isinstance(ws_j, dict) and ws_j:
        http_fields = set(s)
        ws_fields = set(ws_j)
        only_http = sorted(http_fields - ws_fields)
        only_ws = sorted(ws_fields - http_fields)
        r.check(not only_http,
                "WS 帧不缺字段（缺了就会在连上后被打回初值）",
                f"只在 HTTP 里: {only_http}")
        r.check(not only_ws, "WS 帧没有多余的字段", f"只在 WS 里: {only_ws}")
        # 这一组是当初真的漏掉的那批。单独点一次名，是为了让回归时的失败
        # 信息直接说出"漏的是预览/发现那几个"，而不是一句"字段集不等"。
        for key in ("preview_clients", "preview_ready", "preview_fragments",
                    "preview_fragments_unwatched", "preview_clients_kicked",
                    "wsd_running", "wsd_probes_seen", "wsd_probes_answered",
                    "onvif_requests", "web_port"):
            r.check(key in ws_fields, f"WS 帧里有 {key}")

        # 值也要对得上 —— 只比字段名的话，"推了一堆 0"照样能过，而这一段的
        # 全部意义就是防"推的是初值"。`ws_clients` 例外：本探针的连接建了又断，
        # 它本来就该变。
        #
        # ⚠️ 这里只能比**在两次 HTTP 取样之间没有变过**的那批字段。原来是无条件
        # 逐字段比，而 [3] 那次 /api/status 取样（上面第 310 行）与这里读到的 WS
        # 首帧隔着几毫秒 —— 于是凡是**时刻在走的**字段都在赌"这几毫秒里它没动"。
        # 实测（`out\t-regress\counter-race-probe.py`，600 轮，同一个二进制）：
        # **17/600 = 2.8%** 的轮次会红，且分成两种签名 ——
        #   A 14/17：恰好 `preview_fragments` / `preview_fragments_unwatched`
        #            （Phase W 之后每 ~206 ms 切一块分片，实测 204 ms）；
        #   B  3/17：`ticks` / `elapsed_s` / `effective_fps` / `bytes` /
        #            `keyframes` / `content_frames` / `cursor_frames` /
        #            `osd_refresh_frames` / `packets` / `rtsp_streams` ——
        #            整个**每秒一次**的报表块一起翻新（1 Hz，实测 0.5%）。
        # B 与分片毫无关系，也就是说这条断言**在 Phase W 之前就是竞速的**
        # （≈0.75%），Phase W 只是把占多数的那一根计数器从"每 GOP 一次"变成
        # "每 206 ms 一次"，把它抬到 2.8% —— 它是被 Phase W **照出来**的老缺陷，
        # 不是 Phase W 引入的回归。
        #
        # 修法：用"两次 HTTP 取样一致"当**稳定性的判据**，而不是维护一张会漂移的
        # 白名单。取 s 之后再取一次 s2，只有 s[k] == s2[k] 的字段参与比较 ——
        # 对单调递增的计数器这是**严格**的：一个单调量在 t1 与 t2 上相同，就说明
        # 它在 [t1,t2] 上没有变过，而 WS 那一帧正落在窗内。对只在 1 Hz 报表里
        # 重算的那几个（`effective_fps` 之类），要求它在这几毫秒里变一次又变回
        # 原值 —— 这条链上不会发生。判别力不降：WS 若推了一堆 0，稳定的那几十个
        # 字段会**全部**对不上。
        s2 = {}
        try:
            st2, _, body2 = api_get("/api/status")
            r.check(st2 == 200, f"第二次 GET /api/status（取稳定字段用）→ {st2}")
            if st2 == 200:
                s2 = json.loads(body2)
        except Exception as e:  # noqa: BLE001 - 同 [4] 开头那条：异常只该变成一条失败
            r.check(False, f"第二次 GET /api/status：{type(e).__name__}: {e}")
        stable = {k for k in (http_fields & set(s2)) if s2[k] == s[k]}
        # 这一条是防"上面那次取样失败 ⇒ stable 空了 ⇒ 下面那条空转着通过"，
        # 也就是陷阱 91 的形状：吞掉的异常和期望结果长得一模一样。
        r.check(len(stable) >= 25, f"两次取样之间没变过的字段有 {len(stable)} 个 ≥ 25")
        mismatch = sorted(k for k in (stable & ws_fields)
                          if k != "ws_clients" and ws_j[k] != s[k])
        r.check(not mismatch, "稳定字段在两条路上的值一致", f"不一致: {mismatch}")
        # 这条是防"两边都缺同一个字段"：字段集相等可以由"都很小"满足。
        r.check(len(http_fields) >= 25, f"/api/status 字段数 {len(http_fields)} ≥ 25")

        # `version` 的**形状**。上面两条字段集断言只证明"两边都有这个键"，
        # 证明不了"那个值是个能用的版本号" —— 空串在两条路上一样相等，
        # 而空串正是前端 emptyStats() 的初值（后端的字段加了、补丁点没接上
        # 时就会是它）。值本身对不对由两个验收脚本做**三方一致**对账
        # （包名 == exe 的 VersionInfo == 这里），这一条只管形状。
        ver = s.get("version")
        r.check(isinstance(ver, str) and re.fullmatch(r"\d+\.\d+\.\d+", ver or "") is not None,
                f"/api/status 的 version 形如 semver（实际 {ver!r}）",
                "空串 = patch_live_counters 那一行没接上；非 semver = 来源不是 project(... VERSION)")
        r.check(ws_j.get("version") == ver,
                f"/ws/stats 的 version 与 HTTP 一致（WS {ws_j.get('version')!r}）")

    # ---- 4b. 前端读的字段名，后端发的里面得有 ----------------------------
    #
    # [3]/[4] 管的是**后端两侧自洽**（HTTP 与 WS 的字段集相等），管不到"前端读的
    # 名字对不对"。而前端把字段名写错时的症状是：那一格永远空白、或者永远是 0，
    # 控制台**一句错都不报**（minify 之后 `.perf_suggest_quality` 就是一个普通
    # 的 undefined 成员访问，没有类型、没有检查）。
    #
    # 这在预览/发现那批字段上真的发生过一次：后端只往 `GET /api/status` 里加，
    # WS 那一帧没有 —— 于是页面一连上 WS 就被打回初值。当时 [3] 是全绿的，
    # 因为它比的是后端的两条路，恰好是唯一没坏的地方。
    #
    # 判据按**命名约定**取：以 perf_/wsd_/preview_/onvif_/audio_/drops_/
    # adaptive_/osd_/nalu_/rtsp_/video_/sub_/ws_ 开头的标识符，以及
    # capture./rtsp./web./onvif./osd./adaptive. 开头的点分路径，在这份前端代码里
    # 只可能是统计或配置字段（这些前缀是它自己的约定，不是常见词的碎片）。
    # 于是"前端出现、后端不认识"就是一个可以直接断言的集合差。
    #
    # `video_` 是后补进来的：`video_width`/`video_height` 这对字段在设备信息页
    # 加进来之前**一直不在这个名单里**，也就是说前端要是把这对名字写错了
    # （比如写成 `width`/`height`），这一段看不见 —— 而它的症状正是这一段要防的
    # 那一种（那一格永远空白/0，控制台一句错都不报）。
    # 实测这一版产物上是 71 个标识符、零个多余的，所以它不靠白名单过活。
    #
    # **覆盖是部分的，说清楚**：它只看得见带这些前缀的名字。像 `quality` /
    # `max_height` 这种短名字（`capture.quality` 看得见，但前端里一处裸写的
    # `quality` 看不见）漏得掉。这条防的是"改后端字段名时忘了改前端"，而
    # 那类改动几乎总是发生在带前缀的字段上。
    print("\n[4b] 前端读的字段名，后端发的里面得有")
    known = set(s) | set(cfg)
    for grp in ("capture", "rtsp", "web", "onvif", "osd", "adaptive"):
        known |= {f"{grp}.{k}" for k in (cfg.get(grp) or {})}
        known |= set(cfg.get(grp) or {})
    # **列表型的配置段（`channels.items`）也要算数。** 上面那个循环只认字典型
    # 的段，而一条通道项的键（`rtsp_path` / `rtsp_sub_path` / `max_height`…）
    # 一个都进不了 `known` —— 于是前端读 `rtsp_path` 会被判成"后端不认识"，
    # 而后端明明就在发它（`channels_json_node`）。2026-10-09 实测就是这么红的。
    #
    # 这里用的是 [2c] **刚刚真的读回来的**那份键集，不是从当前快照 `cfg` 里现
    # 推一份：`cfg` 是 [2] 取的，那时通道还没写进去，而且在 [2c] 末尾又被清空
    # 了 —— 拿 `cfg` 铺会铺出个空集，看着像做了覆盖，其实一次都不生效。
    known |= ch_keys
    pat_bare = re.compile(r"\b(?:perf|wsd|preview|onvif|audio|drops|adaptive|osd|"
                          r"nalu|rtsp|video|sub|ws)_[A-Za-z0-9_]+")
    pat_dot = re.compile(r"\b(?:capture|rtsp|web|onvif|osd|adaptive)\.[a-z_][a-z0-9_]*")
    # 已知"长得像字段名、其实不是"的标识符放这里。**现在是空的**，而它存在
    # 是为了让将来遇到误报的人有一条明确的路（加一行），而不是把整段关掉。
    allow = set()
    found, unknown = set(), {}
    for ref in sorted(x for x in refs if x.endswith(".js")):
        try:
            _, _, js = get(a.port, ref)
        except OSError:
            continue
        text = js.decode("utf-8", "replace")
        for pat in (pat_bare, pat_dot):
            for m in pat.finditer(text):
                name = m.group(0)
                found.add(name)
                if name not in known and name not in allow:
                    unknown.setdefault(name, ref)
    r.check(len(found) > 20,
            f"从产物里认出 {len(found)} 个字段名（太少说明爬取/正则失效了，"
            f"这一段的断言会变成空转）")
    r.check(not unknown,
            f"认出来的字段名后端全都认识（认了 {len(found)} 个）",
            "这些在前端出现、后端不认识：" +
            "，".join(f"{k}（在 {v}）" for k, v in sorted(unknown.items())[:8]) +
            "。要么是前端把名字写错了（症状是那一格永远是空的/0，而控制台不报错），"
            "要么它根本不是字段名 —— 那就加进上面那个 allow 集合里")

    # ---- 4c. 水印：改完**立刻**生效（进 applied、不进 pending）-----------
    #
    # 这一段和 [9] 段那批"改了要重启"的字段是**反着**的：改 `osd.*` 之后它必须
    # 当场出现在 `applied` 里、并且**不在** `pending` 里。
    #
    # 为什么值得单独盯着：水印从前是**冷**的（画面上的水印用采集启动时那份快照，
    # `SessionOptions::osd` 在 `run()` 顶上取一次），改它要重建整条管线 ——
    # 也就是说 `osd.*` 进 `pending` 曾经是**正确**的行为，`tests/onvif_soap.py`
    # 里那条断言当初就是那么写的。Phase S 把水印改成热生效（`LiveOsd`，采集循环
    # 每拍取当期那一份），存盘的那份和运行侧那份**当场就一致**，`osd.*` 自然不再
    # 进 pending。两个探针里各留一条**相反方向**的断言，是为了让下一个人一眼看出
    # 这是刻意的改判，而不是哪一边写错了。
    #
    # ⚠️ **必须放在 [4] 之后。** [4] 的值一致性断言（`ws_j[k] == s[k]`）比的是
    # 前面那次 GET 读到的 `s` 与 WS 那一帧；这一段会真的改一次配置，而配置一改
    # 就有计数器跟着动（`osd_hot_applied`）。摆在 [4] 之前的话，[4] 会报一条
    # 与它自己毫不相干的"同名字段的值不一致"。
    print("\n[4c] 水印：改完立刻生效（进 applied、不进 pending）")

    st_o, _, body_o = api_get("/api/config")
    osd_cur = {}
    if st_o == 200:
        try:
            osd_cur = (json.loads(body_o).get("osd") or {})
        except ValueError:
            osd_cur = {}
    if not osd_cur:
        print("  [note] 读不到 /api/config 的 osd 段 —— [4c] 整段跳过")
    else:
        # 改哪一项：**读现值再取反**，不写死。
        #
        # 写死一个值（比如一律设成 False）在出厂默认上是对的，但这段探针也会
        # 跑在"已经跑过一轮、值被翻过来了"的配置上 —— 那时 POST 进去的和存盘的
        # 一模一样，采集侧按设计**不会动**（`capture_session` 比的是序列化之后的
        # 文本，陷阱 91 那个"防抖吞掉"的形状），于是下面那条肯定式断言会红，
        # 而红的是尺子，不是产品。
        want = not bool(osd_cur.get("outline"))
        # 基线**现读**，不用 [3] 段那份快照 `s` —— 理由见下面那两段（这是量出来的）。
        st_b, _, body_b = api_get("/api/status")
        base, base_ticks = None, None
        if st_b == 200:
            try:
                sb = json.loads(body_b)
            except ValueError:
                sb = {}
            base = sb.get("osd_hot_applied")
            base_ticks = sb.get("ticks")
        st_p, _, body_p = api_post("/api/config", {"osd": {"outline": want}})
        r.check(st_p == 200, f"POST /api/config {{osd.outline: {want}}} → {st_p}")
        res = {}
        try:
            res = json.loads(body_p)
        except ValueError:
            res = {}
        applied = res.get("applied") or []
        pending = res.get("pending") or []
        r.check("osd.outline" in applied,
                f"osd.outline 进了 applied（这次改的就是它）：{applied}",
                "水印是热生效的（LiveOsd）—— 不进 applied 说明 apply_config 里"
                "那句 running_cfg.osd = next.osd 没跟上，于是界面显示『要重启才"
                "生效』，而画面其实已经变了")
        r.check("osd.outline" not in pending,
                f"osd.outline **不**在 pending 里：{pending}")
        # 回读一次：POST 的回应是它**自报**的，存盘那份得真的是新值。
        st_v, _, body_v = api_get("/api/config")
        osd_new = {}
        if st_v == 200:
            try:
                osd_new = (json.loads(body_v).get("osd") or {})
            except ValueError:
                osd_new = {}
        r.check(bool(osd_new.get("outline")) == want,
                f"回读 /api/config 的 osd.outline = {osd_new.get('outline')}"
                f"（设的是 {want}）")

        # ---- 否定式断言必须配一个肯定式的伴（陷阱 91）--------------------
        #
        # "不在 pending 里"单独看是**空的**：把 `apply_config` 里那三行整个删掉，
        # `osd.*` 也不会进 pending（什么都没发生，也就无所谓待不待重启）。
        # 真正要证明的是**采集侧真的换上了新配置**，而那件事只有一个计数器说得出来。
        #
        # ⚠️ 不能用 `osd_rebuilt_frames`（陷阱 87）：它只在"桌面静止、要从无水印的
        # 桌面图重合成"那条路上涨，在一台正在被录的、画面在动的桌面上恒为 0。
        #
        # ⚠️⚠️ **这条判据和它的基线在 2026-10-08 各改过一次，别改回去。**
        #
        #   (1) 基线**现读**（就在上面那次 `api_get("/api/status")`），不再用 [3]
        #       段那份快照 `s`；
        #   (2) 判"涨了"（`now_hot > base`），不判"恰好 +1"（`now_hot == base + 1`）。
        #
        # 原写法（基线取 `s`、判 `== base + 1`）在 `tests/rtsp_sub_e2e.ps1` 上红过
        # 一次，报的是"osd_hot_applied 停在 4（基线 1），而采集在跑"。**红的是尺子，
        # 不是产品**，两条都在日志里对得上：
        #
        #   * `onvif_soap.py` 的 [11] 段刚连发了几次 `SetOSD`/`CreateOSD`/`DeleteOSD`，
        #     `run.err` 里那四行"水印配置已热生效"（15:06:51.652 / 52.169 / 52.213 /
        #     52.729）说明这个数在此之前**已经涨过三次**；
        #   * 而 `/api/status` 里那个数是采集侧**每秒推一次**的快照 —— `capture_session`
        #     只在 `run()` 顶上（:824）与每秒报表（:1498）调 `stats_hook_`。所以 [3] 段
        #     读到的 `1` 是 52.049 那一帧推的，比它晚的两次改动都还没被推出来。
        #
        # 于是 POST 那一条**真的生效了**（52.729 那行就是它），但 `base + 1 == 2`
        # 这个等式在那台服务器上**根本不可能成立**。这是陷阱 87 的第六种形状：判据
        # 依赖了一个没写进前提的环境条件（"这一段之前没人动过 OSD"、"这个数不会比
        # 自己晚一秒"）。换成一条与它无关的：**我 POST 之后，它必须比我现在读到的大。**
        #
        # `stats_` 在 `run()` 顶上会被整个重置（`capture_session.cpp:684`），所以真
        # 发生分段重启时这个数会**倒退**。那同样不是产品的问题（这一次运行的
        # `[6]`/`[7]` 两段就会真的 reload），单独认出来并明说跳过，不报成失败 ——
        # 但也**不报成通过**：跳过是"这一次没验到"，不是"验过了"。
        now_hot, ticks_moved, rewound = base, False, False
        for _ in range(12):
            time.sleep(0.25)
            st_n, _, body_n = api_get("/api/status")
            if st_n != 200:
                continue
            try:
                sn = json.loads(body_n)
            except ValueError:
                continue
            now_hot = sn.get("osd_hot_applied", now_hot)
            if (isinstance(base, int) and isinstance(now_hot, int)
                    and now_hot < base):
                rewound = True
                break
            if isinstance(base_ticks, int) and isinstance(sn.get("ticks"), int):
                ticks_moved = ticks_moved or sn["ticks"] > base_ticks
            if (isinstance(base, int) and isinstance(now_hot, int)
                    and now_hot > base):
                break
        if not isinstance(base, int) or not isinstance(now_hot, int):
            print("  [note] /api/status 里没有 osd_hot_applied —— "
                  "热生效那条跳过（字段缺失本身已由 [3] 报出）")
        elif rewound:
            print(f"  [note] 采集侧这期间**重开了一段**（stats_ 在 run() 顶上归零，"
                  f"osd_hot_applied {base} → {now_hot} 是倒退）—— 热生效那条跳过。"
                  f"单元/单点判据在 out\\phase-s-probe.py，那里没有别的写入者")
        elif now_hot > base:
            r.check(True, f"采集侧真的换上了新水印"
                          f"（osd_hot_applied {base} → {now_hot}）")
        elif not ticks_moved:
            print(f"  [note] 采集会话没在跑（ticks 一直是 {base_ticks}）—— "
                  f"热生效那条跳过")
        else:
            r.check(False,
                    f"画面上的水印没有换上新配置：osd_hot_applied 停在 "
                    f"{now_hot}（基线 {base}），而采集在跑"
                    f"（ticks 从 {base_ticks} 涨过了）",
                    "写 running_cfg.osd 和调 LiveOsd::set() 是两件事，漏了后者"
                    "就是这里不涨")
        # 收尾把它翻回去。**不翻也不会红**（下一轮读到的现值变了，而上面那两行
        # 本来就是取反的），这里只是不在这份配置上留下无谓的痕迹。
        api_post("/api/config", {"osd": {"outline": not want}})

    # ---- 5. 自适应降载：字段之间的**自相矛盾检查** ------------------------
    #
    # [4] 管的是"两条路上字段集相等"（防漏推），这一段管的是"推下来的值本身
    # 自洽"。两者都要有：字段名都在、但值全是初值，是 [4] 挡不住的那一种坏。
    #
    # 判据刻意挑**与负载无关**的那几条：这个脚本在别人的机器上、或者在录制
    # 正吃满 CPU 的时候也要能跑。所以：
    #   * `perf_target_fps` 必须等于 `fps / perf_duty` —— 这两个数由两段不同的
    #     代码算出来（一个来自自适应控制器、一个来自配置），对不上就说明有一个
    #     没接上；
    #   * `perf_hint` 的有无必须和 `perf_duty` 一致 —— 提示文案只有后端一份
    #     真相，空串的含义就是"duty 是 1 且最近没丢过"。
    # 「轻载下应该 ok / duty 1」那条放在最后单独一条，它的失败信息里会写明
    # 这是**负载相关**的判断（先看 perf_hint 和 drops_recent），而不是接线坏了。
    print("\n[5] 自适应降载：perf_* 字段之间的自洽")
    st_c, _, body_c = api_get("/api/config")
    fps_cfg = 0
    pend_c = []
    if st_c == 200:
        try:
            _c = json.loads(body_c)
            fps_cfg = int((_c.get("capture") or {}).get("fps") or 0)
            pend_c = _c.get("pending") or []
        except (ValueError, TypeError, AttributeError):
            fps_cfg = 0
    r.check(fps_cfg > 0, f"从 /api/config 读到 capture.fps = {fps_cfg}")

    state_p = s.get("perf_state")
    duty_p = s.get("perf_duty")
    tfps_p = s.get("perf_target_fps")
    hint_p = s.get("perf_hint")

    if not isinstance(duty_p, int):
        # 这几个字段只由**采集循环**每秒填一次。会话还没跑起来时 /api/status
        # 只带补丁出来的那批字段（`last_stats` 还是空的），这里再报一遍缺失
        # 只会掩盖 [3] 已经报过的那条。明说跳过，不要静默。
        print("  [note] /api/status 里没有 perf_duty —— 采集会话还没产出过统计，"
              "[5] 的取值断言跳过（字段缺失本身已由 [3] 报出）")
    else:
        r.check(state_p in ("ok", "throttled", "overloaded"),
                f"perf_state = {state_p!r} 是 ok / throttled / overloaded 之一")
        r.check(duty_p >= 1,
                f"perf_duty = {duty_p!r} 是 >= 1 的整数（0 会让 tick % duty 除零）")
        r.check(isinstance(hint_p, str), f"perf_hint 是字符串（实际 {type(hint_p).__name__}）")

        if duty_p >= 1 and fps_cfg > 0 and isinstance(tfps_p, int) and tfps_p > 1:
            # ⚠️ **这一条的前提是"配置里的 fps 就是跑着的那一份"**，而这不总
            # 成立：`/api/config` 会挂一个 `pending` 列表，点名了"改了但要重启
            # 才生效"的字段。本脚本 [7] 段会换一个画质档位、`/api/reload` 让它
            # 当场生效、再**还原**回原来那一档 —— 还原只写了配置，运行侧还停在
            # 那一档上（档位里的 fps / 码率不是热生效的）。于是配置说 25，采集
            # 循环的基础帧率是 30（另一个档位的值），拿 25 去除 duty 比的是一个
            # 从来没被用过的数。
            #
            # 2026-10-09 实测：同一个实例上**连跑两次**本脚本，第二次必红
            # （`perf_target_fps = 15` 对 `fps(25) // duty(2) = 12`）。跑一次
            # 是绿的 —— 驱动它的那个前提破坏发生在第一次的 [7] 段里。
            #
            # 这里**不是"跳过"**：跳过会让这一条在两个前提之间永久空转（陷阱
            # 91）。pending 说运行侧不是这个帧率时，要验的是**反面** —— 运行侧
            # 那一档确实不是配置里的这个数。这条照样能红（pending 谎报时会红）。
            lo = tfps_p * duty_p          # target = base // duty ⇒ base ∈ [lo, lo+duty-1]
            if "capture.fps" in pend_c:
                r.check(not (lo <= fps_cfg <= lo + duty_p - 1),
                        f"capture.fps 在 pending 里，运行侧基础帧率确实不是配置里的"
                        f" {fps_cfg}（由 target {tfps_p} × duty {duty_p} 反推是 "
                        f"{lo}~{lo + duty_p - 1}）",
                        "pending 点名了 capture.fps 要重启才生效，可运行侧的帧率就"
                        "是配置里那个数 —— 那么那条 pending 是假的")
            else:
                r.check(tfps_p == fps_cfg // duty_p,
                        f"perf_target_fps = {tfps_p} 等于 fps({fps_cfg}) // duty({duty_p})"
                        f" = {fps_cfg // duty_p}")
            # 状态名和 duty 是一对：duty 为 1 就是 ok，duty > 1 只能是 throttled
            # 或 overloaded。反过来（duty > 1 却报 ok）会让前端的横幅条件永远不成立。
            if duty_p == 1:
                r.check(state_p == "ok", f"duty = 1 时 perf_state = {state_p!r}（应为 ok）")
                r.check(hint_p == "" or hint_p.startswith("已恢复实时"),
                        "duty = 1 时 perf_hint 只在「刚恢复」时才有字",
                        f"实际：{hint_p!r}")
            else:
                r.check(state_p in ("throttled", "overloaded"),
                        f"duty = {duty_p} 时 perf_state = {state_p!r}（不该是 ok）")
                r.check(hint_p != "", "duty > 1 时 perf_hint 必须有一句解释",
                        f"实际：{hint_p!r}")

        # 建议档位：**只能在 overloaded 时非空**。duty = 1 却建议降档，说明算
        # 建议那一段读的是过期的配置（"重算而不是按点击特判"要防的就是它）。
        sq = s.get("perf_suggested_quality")
        r.check(isinstance(sq, str), f"perf_suggested_quality 是字符串（实际 {type(sq).__name__}）")
        r.check(sq in ("", "high", "balanced", "smooth", "verylow"),
                f"perf_suggested_quality = {sq!r} 是空串或一个已知档位")
        if state_p != "overloaded":
            r.check(sq == "", f"非 overloaded 时不给降档建议（实际 {sq!r}）")

        # 「轻载下应该 ok / duty 1」这一条**原来的写法是错的**，改在这里。
        #
        # 原文直接 `check(state == "ok" and duty == 1)`，只在失败信息里写一句
        # "这是负载相关的判断"。问题是**它从来没有验过自己的前提** —— "轻载"是
        # 断言里假设的，不是量出来的。2026-10-08 实测到它的两种命运：同一个二进制、
        # 同一份配置，刚起服务器就拿它打 → `ok / 1` 通过（`out\wc-diag\idle_probe.ps1`
        # 量得：探测器全程 1.63 秒、118 项 0 失败）；而 `rtsp_sub_e2e.ps1` 的 [3] 段
        # 拿它去打一个**刚被三路并发拉流压过**的会话 → `throttled / 2` 失败，可那一轮
        # `rtsp_sub_e2e` 自己的 217 项**全绿**，服务端日志里还留着降载的完整依据
        # （`自适应 12 fps（1/2）`、`缓存池空闲 1/6`）。也就是说这条断言量的其实是
        # **这台机器当时忙不忙**，不是产品对不对 —— 陷阱 87 的同一个形状。
        #
        # 换成一条**与负载无关**、但同样抓得住"平白无故降了载"的判据：
        # 控制器**只有**看到计数器增量时才会升 duty（`adaptive.observe` 的
        # `bad = pool_starved || dropped >= 阈值`，而两条路各自都会让
        # `queue_dropped` 或 `pool_exhausted` 加一 —— 池子空那一支见
        # `capture_session.cpp` 里 `++stats_.pool_exhausted; starved_this_second = true;`
        # 两行是挨着写的）。所以 **duty > 1 ⟹ 两个累计计数里至少一个 > 0**，
        # 这条在全负载范围里都成立；而它一旦不成立，就是"一帧没丢、池子一次没空，
        # 却把帧率降了一半" —— 真正的失控，也正是原来那条想抓的东西。
        #
        # duty == 1 那一侧不需要在这里再断言一次 state：上面 `duty_p == 1` 的分支
        # 已经判过"duty = 1 时 state 必须是 ok"了。
        if duty_p > 1:
            qd = s.get("queue_dropped")
            pe = s.get("pool_exhausted")
            r.check(isinstance(qd, int) and isinstance(pe, int),
                    f"queue_dropped / pool_exhausted 是整数"
                    f"（实际 {type(qd).__name__} / {type(pe).__name__}）")
            if isinstance(qd, int) and isinstance(pe, int):
                r.check(qd + pe > 0,
                        f"duty = {duty_p} 有依据：累计 队列丢弃({qd}) + 池耗尽({pe}) > 0",
                        "两个都是 0 却降了载 —— 控制器只在计数器动过的那一秒才会升 "
                        "duty，所以这一条与负载无关，红了就是真失控，别拿'机器忙'去解释它")
            print(f"  [note] 这一跑不是轻载：duty = {duty_p}、state = {state_p!r}、"
                  f"hint = {hint_p!r}、丢弃 {qd} + 池耗尽 {pe}"
                  f" —— 「轻载应为 ok/1」那条断言已按陷阱 87 改掉（它量的是机器忙不忙）")

    # ---- 6. /api/reload --------------------------------------------------
    #
    # **这一段故意放在最后**：它真的会把采集链拆了重建（录制分段、RTSP 客户端
    # 断开重连），跑在前面的段会因此看到一次"流断了"。放在最后，加上下面那圈
    # 「等到它重开完并确认还在出帧」的等待，这一段结束时服务是**热**的。
    #
    # 盯的是两个**都很安静**的故障：
    #   * 路由没注册 → 请求落到静态兜底上，SPA 回落给一份 200 + index.html。
    #     前端 `JSON.parse` 报的错和真正的原因毫无关系，而用户看到的是"点了一下
    #     按钮、什么都没发生"。
    #   * 回调接上了但信号没送到采集侧 → 202 回得很漂亮，管线一动不动。**这是
    #     "一键降档"最坏的那一种失败：按钮弹了、看着成功了、画质没变。**
    #
    # 所以这里不满足于"接口有响应"，而是要求**看到管线真的重开了一遍**。
    # `elapsed_s` 是当前这一段的秒数，重开时归零 —— 于是"比 POST 之前小"就是
    # 重开的直接证据，而且这条判据**与负载无关**（不像"应当 N 秒内回来"）。
    # 之后再确认 `ticks` 还在涨：重开完就死掉的管线同样能满足上面那一条。
    print("\n[6] POST /api/reload")
    st_b, _, body_b = api_get("/api/status")
    before = {}
    if st_b == 200:
        try:
            before = json.loads(body_b)
        except ValueError:
            before = {}
    elapsed_before = before.get("elapsed_s")

    st_r, _, body_r = api_post("/api/reload", {})
    r.check(st_r == 202, f"POST /api/reload → 202（实际 {st_r}）",
            "200 多半是它根本没注册、落到了 SPA 兜底上；响应开头："
            f"{body_r[:80]!r}")
    jr = {}
    try:
        jr = json.loads(body_r)
    except ValueError:
        pass
    r.check(jr.get("ok") is True, f"回应是 JSON 且 ok=true（实际 {body_r[:80]!r}）")
    r.check(isinstance(jr.get("live"), bool),
            f"live 是布尔（实际 {jr.get('live')!r}）")
    r.check(isinstance(jr.get("note"), str) and jr["note"] != "",
            "note 非空 —— 「录制会分段、RTSP 会断开重连」这句代价不能省",
            f"实际：{jr.get('note')!r}")

    if jr.get("live") is not True:
        # 没在录的时候这个请求**本来就是空操作**（分段循环每一趟都用当前配置
        # 重算选项），所以这里没有可观察的重开，也不该报失败。明说跳过。
        print("  [note] 服务端报 live=false（此刻没有采集管线在跑）——"
              "「重开真的发生了」那两条跳过，路由本身已在上面验过")
    elif not isinstance(elapsed_before, (int, float)) or elapsed_before < 2:
        print(f"  [note] POST 之前 elapsed_s = {elapsed_before!r}，太小/读不到 ——"
              "「重开真的发生了」那两条跳过（判据是「它变小了」，基准太小就分不出来）")
    else:
        # 等 `elapsed_s` 倒退。每 0.4s 问一次、最多 10 秒；正常情况下 1~2 秒
        # 就会看到（统计是每秒推一次的）。
        seen = None
        lost = False
        deadline = time.time() + 10
        while time.time() < deadline:
            time.sleep(0.4)
            try:
                st_s, _, body_s = api_get("/api/status")
            except OSError:
                lost = True
                break
            if st_s != 200:
                continue
            try:
                cur = json.loads(body_s)
            except ValueError:
                continue
            now_e = cur.get("elapsed_s")
            if isinstance(now_e, (int, float)) and now_e < elapsed_before:
                seen = cur
                break
        r.check(seen is not None,
                f"重开后 elapsed_s 归零（POST 前是 {elapsed_before}）",
                "**等的时候后端整个不答了** —— 要么重开把进程带崩了，要么这次"
                "运行的时间预算刚好用完（--seconds）。"
                "两者都不是「重开成功」，也都不是这一段能分辨的"
                if lost else
                "10 秒内一直是「没变小」：回调接了但信号没送到采集侧，"
                "或者 run() 没在这一趟开头把位清干净、多开了空的一段。"
                "这正是「按钮弹了、看着成功、画质没变」那一种")

        if seen is not None:
            t0 = seen.get("ticks")
            time.sleep(2.0)
            st_s, _, body_s = api_get("/api/status")
            cur = {}
            if st_s == 200:
                try:
                    cur = json.loads(body_s)
                except ValueError:
                    cur = {}
            t1 = cur.get("ticks")
            r.check(isinstance(t0, int) and isinstance(t1, int) and t1 > t0,
                    f"重开之后 ticks 还在涨（{t0} → {t1}）",
                    "归零了但不再出帧，说明新的一段起来就死了。重开的收尾是"
                    "完整的，所以旧那一段的文件仍然能播 —— 光看文件看不出来")

    # ---- 7. 画质档位：一键降档那两步的两半 --------------------------------
    #
    # 状态页那个「降到 <下一档>」按钮 = `POST /api/config`（档位）+
    # `POST /api/reload`，两步都要成功才算数。所以两半都得盯住，而它们的失败
    # 都**极其安静**：
    #   * 只存了标签、没展开 → 界面显示"流畅"而帧率还是 25；后端的
    #     `validate_config` 负责展开，这一段盯着它别被谁改掉；
    #   * 展开了但重开没接上 → 用户点了按钮、画质一点没变，而 API 全绿 ——
    #     这是"一键降档"最坏的那一种失败。
    #
    # **放在 [6] 之后是刻意的**：那次重开让"运行侧基线"跟上了存盘的那一份
    # （`main.cpp` 里 `sync_options_to_config` 那一句），于是这里 POST 进去的
    # 每一项都必然与运行值不同、必然进 `pending`。放在前面的话，pending 里有没有
    # 东西取决于"上一次跑这个脚本留下了什么"，断言会时真时假（陷阱 60 的形状）。
    print("\n[7] 画质档位：{capture:{quality}} 的展开，以及「存档位 + 重开」两步")

    st, _, body = api_get("/api/config")
    cfg7 = {}
    if st == 200:
        try:
            cfg7 = json.loads(body)
        except ValueError:
            cfg7 = {}
    tiers = cfg7.get("quality_tiers")
    r.check(isinstance(tiers, list) and len(tiers) >= 2,
            f"GET /api/config 带 quality_tiers 且不止一档"
            f"（实际 {type(tiers).__name__}，"
            f"{len(tiers) if isinstance(tiers, list) else 0} 档）",
            "档位表**从后端发**：状态页的降档按钮要靠它把 key 翻成中文名，"
            "配置页的档位下拉也要用它。没有它前端只能显示裸 key —— 而"
            "『降到 smooth』这种话用户看不懂")
    tiers = tiers if isinstance(tiers, list) else []

    bad_shape = [
        t for t in tiers
        if not isinstance(t, dict)
        or not all(isinstance(t.get(k), typ) for k, typ in
                   (("key", str), ("label", str), ("fps", int),
                    ("bitrate_kbps", int), ("max_height", int),
                    ("sub_enabled", bool)))
    ]
    r.check(tiers and not bad_shape,
            f"每一档都有 key/label/fps/bitrate_kbps/max_height/sub_enabled"
            f"（{len(tiers)} 档，形状不对的 {len(bad_shape)} 个）",
            f"不合形状的：{bad_shape[:2]}")
    # **线值是英文，翻译只做在显示层**（陷阱 80）。把 key 也翻成中文的话，
    # 后端的 `validate_config` 认不出来会静默改成 custom —— 界面上看着"设了"，
    # 实际一个字段都没被管。反过来 label 必须是中文，否则等于没翻译。
    r.check(all(t["key"].isascii() for t in tiers), "档位 key 全是 ASCII（线值不翻译）")
    r.check(any(not t["label"].isascii() for t in tiers), "档位 label 是中文（显示层用它）")

    cap0 = dict(cfg7.get("capture") or {})
    sub0 = (cfg7.get("rtsp") or {}).get("sub_enabled")
    q0 = cap0.get("quality")
    target = next((t for t in tiers if t["key"] != q0), None)

    if target is None:
        print(f"  [note] 档位表里没有一档和当前档位（{q0!r}）不同 —— 跳过往返")
    else:
        key = target["key"]
        # 这次改动**必然**动到哪几项。逐个算出来而不是写死一张清单：写死的话，
        # 换了档位表就会漏掉一项，而漏掉的表现是"pending 里没有它"被当成
        # "档位没展开"报出来，指向完全反了。
        changed = [f for f, now, want in (
            ("capture.fps", cap0.get("fps"), target["fps"]),
            ("capture.bitrate_kbps", cap0.get("bitrate_kbps"), target["bitrate_kbps"]),
            ("capture.max_height", cap0.get("max_height"), target["max_height"]),
            ("rtsp.sub_enabled", sub0, target["sub_enabled"]),
        ) if now != want]

        st_p, _, body_p = api_post("/api/config", {"capture": {"quality": key}})
        r.check(st_p == 200, f"POST /api/config {{capture:{{quality:{key}}}}} → {st_p}")
        res7 = {}
        try:
            res7 = json.loads(body_p)
        except ValueError:
            pass

        st, _, body = api_get("/api/config")
        got = {}
        if st == 200:
            try:
                got = json.loads(body)
            except ValueError:
                got = {}
        gcap = got.get("capture") or {}
        r.check(gcap.get("quality") == key,
                f"quality 往返：{q0!r} → {gcap.get('quality')!r}（设的是 {key!r}）")
        for f, want, shown in (
            ("fps", target["fps"], "帧率"),
            ("bitrate_kbps", target["bitrate_kbps"], "码率"),
            ("max_height", target["max_height"], "分辨率上限"),
        ):
            r.check(gcap.get(f) == want,
                    f"档位「{target['label']}」把 capture.{f}（{shown}）展开成 {want}"
                    f"（实际 {gcap.get(f)!r}）",
                    "`validate_config` 必须在存盘前按档位表重写这四项，"
                    "否则「选了流畅、帧率还是 25」")
        r.check((got.get("rtsp") or {}).get("sub_enabled") == target["sub_enabled"],
                f"档位「{target['label']}」把 rtsp.sub_enabled 展开成 "
                f"{target['sub_enabled']}（实际 {(got.get('rtsp') or {}).get('sub_enabled')!r}）")

        pend7 = res7.get("pending") or []
        r.check("capture.quality" in pend7,
                f"capture.quality 进了 pending：{pend7}",
                "档位是个**标签**，`sync_options_to_config` 同步不到它 —— 少了"
                "`main.cpp` 里那一行显式赋值，用户点完降档会看到"
                "「画质档位：需要重启」，而那个档位刚刚才被完整应用过")
        missing = [f for f in changed if f not in pend7]
        r.check(not missing,
                f"四项具体值里变了的（{changed}）都进了 pending",
                f"少了 {missing}；pending 实际是 {pend7}")

        # 第二步：重开。**这一步才是"当场生效"** —— 只存档位的话，画质要等
        # 下次重启才变，而按钮上写的是「立即生效」。
        st_r, _, body_r = api_post("/api/reload", {})
        jr7 = {}
        try:
            jr7 = json.loads(body_r)
        except ValueError:
            pass
        r.check(st_r == 202, f"第二步 POST /api/reload → 202（实际 {st_r}）")

        if jr7.get("live") is not True:
            print("  [note] 服务端报 live=false（没有采集管线在跑）——"
                  "「重开后 pending 清空」跳过：没在录的时候重开本来就是空操作，"
                  "运行侧基线不会被重算")
        else:
            # 等 pending 里的这几项消失。统计是每秒推一次、重开在 run() 的下一趟
            # 开头跟上，所以正常 1~2 秒；给 10 秒。
            cleared = False
            deadline = time.time() + 10
            last = None
            while time.time() < deadline:
                time.sleep(0.4)
                st, _, body = api_get("/api/config")
                if st != 200:
                    continue
                try:
                    cur = json.loads(body)
                except ValueError:
                    continue
                last = cur.get("pending")
                if isinstance(last, list) and not (set(changed) & set(last)) \
                        and "capture.quality" not in last:
                    cleared = True
                    break
            r.check(cleared,
                    f"重开之后 pending 里的档位字段清空了（等之前是 {pend7}）",
                    f"10 秒后仍是 {last!r}。**这一条就是「按钮弹了、看着成功、"
                    "画质没变」**：`/api/reload` 回了 202，但那一趟没有按新配置"
                    "重算运行侧基线 ⇒ 界面上会永久挂着一条不存在的待重启项，"
                    "而用户会去重启一次、然后发现没有任何变化")

        # 还原。**没还原的话，下一次跑这个脚本时"当前档位"就是上一次留下的那一档**
        # —— 上面那些"必然进 pending"的断言会退化成看运气（依然是陷阱 60 的形状）。
        restore = dict(cap0)
        st_p2, _, body_p2 = api_post(
            "/api/config", {"capture": restore, "rtsp": {"sub_enabled": sub0}})
        r.check(st_p2 == 200, f"还原原来那一档 → {st_p2}")
        st, _, body = api_get("/api/config")
        back = {}
        if st == 200:
            try:
                back = json.loads(body)
            except ValueError:
                back = {}
        bcap = back.get("capture") or {}
        r.check(bcap.get("quality") == q0 and bcap.get("fps") == cap0.get("fps")
                and bcap.get("bitrate_kbps") == cap0.get("bitrate_kbps")
                and bcap.get("max_height") == cap0.get("max_height")
                and (back.get("rtsp") or {}).get("sub_enabled") == sub0,
                f"还原回 {q0!r} / {cap0.get('fps')} fps / {cap0.get('bitrate_kbps')} kbps"
                f" / max_height {cap0.get('max_height')} / 子码流 {sub0}",
                f"实际 {bcap.get('quality')!r} / {bcap.get('fps')} fps / "
                f"{bcap.get('bitrate_kbps')} kbps / {bcap.get('max_height')} / "
                f"{(back.get('rtsp') or {}).get('sub_enabled')!r}"
                " —— 测试把被测配置改坏了，后面的脚本会跟着一起错")

    # ---- 7b. 延时档位：与画质档位正交的第二张展开表 --------------------
    #
    # 单开一段而不是并进 [7]：这两张表**必须互不覆盖**，而"互不覆盖"这件事只有
    # 把两组字段放进**同一次 POST** 里才验得到。分两段各验一半的话，
    # "后展开的那一段把前一段整个盖掉"这种错会让两边各自的断言都绿。
    print("\n[7b] 延时档位：{capture:{latency}} 的展开，与画质档位正交")

    st, _, body = api_get("/api/config")
    cfg7b = {}
    if st == 200:
        try:
            cfg7b = json.loads(body)
        except ValueError:
            cfg7b = {}
    lats = cfg7b.get("latency_tiers")
    r.check(isinstance(lats, list) and len(lats) >= 2,
            f"GET /api/config 带 latency_tiers 且不止一档"
            f"（实际 {type(lats).__name__}，"
            f"{len(lats) if isinstance(lats, list) else 0} 档）",
            "档位表**从后端发**：配置页那几张卡片上的每一句话几乎都是算出来的"
            "（流水线几帧、@这个帧率约等于多少毫秒、关键帧几秒一个），全部现算"
            "自它。前端写死一份的后果是后端改了表、界面上还是旧数字 —— 而那个"
            "数字正是用户唯一能拿去对账的东西")
    lats = lats if isinstance(lats, list) else []

    bad_lat = [
        t for t in lats
        if not isinstance(t, dict)
        or not all(isinstance(t.get(k), typ) for k, typ in
                   (("key", str), ("label", str),
                    ("threads", int), ("gop_seconds", int)))
    ]
    r.check(lats and not bad_lat,
            f"每一档都有 key/label/threads/gop_seconds"
            f"（{len(lats)} 档，形状不对的 {len(bad_lat)} 个）",
            f"不合形状的：{bad_lat[:2]}")
    if lats:
        r.check(all(t["key"].isascii() for t in lats),
                "档位 key 全是 ASCII（线值不翻译）")
        r.check(any(not t["label"].isascii() for t in lats),
                "档位 label 是中文（显示层用它）")
        # **表里没有 custom。** 这不是洁癖：`find_latency_tier("custom")` 一旦
        # 命中，`validate_config` 就会走进展开那个分支 —— 而 custom 的语义恰恰是
        # "谁都不动"。于是它会拿表里那两个常数把用户手填的值盖掉，症状是
        # "我在自定义里填了 5，保存回来变成 1"，而界面上看不出任何异常。
        # 第四张卡（自定义）因此必须是**前端自己补的**，不能从这张表来。
        r.check(all(t["key"] != "custom" for t in lats),
                "档位表里没有 custom（它是界面自己补的那一张卡）",
                f"实际 keys：{[t['key'] for t in lats]}")

    lcap0 = dict(cfg7b.get("capture") or {})
    l0 = lcap0.get("latency")
    r.check(isinstance(l0, str) and bool(l0),
            f"capture.latency 是个非空字符串（实际 {l0!r}）",
            "这个字段在配置层是**纯新增**的（threads / gop_seconds 早就有、"
            "下游一行都不用改），所以它最容易出的错是"
            "「加了字段、忘了在里面那条读写链上接一下」—— GET 回来是空串，"
            "而三张卡片一张都选不中")
    r.check(l0 == "custom" or any(t["key"] == l0 for t in lats),
            f"当前档位 {l0!r} 要么在档位表里、要么是 custom",
            f"表里的 key：{[t['key'] for t in lats]}")

    target_l = next((t for t in lats if t["key"] != l0), None)
    if target_l is None:
        print(f"  [note] 档位表里没有一档和当前档位（{l0!r}）不同 —— 跳过往返")
    else:
        changed_l = [f for f, now, want in (
            ("capture.threads", lcap0.get("threads"), target_l["threads"]),
            ("capture.gop_seconds", lcap0.get("gop_seconds"), target_l["gop_seconds"]),
        ) if now != want]

        st_p, _, body_p = api_post("/api/config",
                                   {"capture": {"latency": target_l["key"]}})
        r.check(st_p == 200,
                f"POST /api/config {{capture:{{latency:{target_l['key']}}}}} → {st_p}")
        res7b = {}
        try:
            res7b = json.loads(body_p)
        except ValueError:
            pass

        st, _, body = api_get("/api/config")
        got = {}
        if st == 200:
            try:
                got = json.loads(body)
            except ValueError:
                got = {}
        gcap = got.get("capture") or {}
        r.check(gcap.get("latency") == target_l["key"],
                f"latency 往返：{l0!r} → {gcap.get('latency')!r}"
                f"（设的是 {target_l['key']!r}）")
        r.check(gcap.get("threads") == target_l["threads"],
                f"档位「{target_l['label']}」把 capture.threads 展开成 "
                f"{target_l['threads']}（实际 {gcap.get('threads')!r}）",
                "`validate_config` 必须在存盘前按档位表重写这一项 —— threads 是"
                "帧级并行下编码流水线的深度，也就是这一档**唯一真正的旋钮**。"
                "少了这一行，档位就只是一个写得进去、读得出来的名字（陷阱 28）")
        r.check(gcap.get("gop_seconds") == target_l["gop_seconds"],
                f"档位「{target_l['label']}」把 capture.gop_seconds 展开成 "
                f"{target_l['gop_seconds']}（实际 {gcap.get('gop_seconds')!r}）",
                "第二个旋钮，管的是「多久能看到第一帧」（分片只在关键帧处产生），"
                "**不是**「看到之后有多滞后」—— 这两件事在界面上是分开写的")

        pend = res7b.get("pending") or []
        r.check("capture.latency" in pend,
                f"capture.latency 进了 pending：{pend}",
                "档位是个**标签**，`sync_options_to_config` 同步不到它 —— 少了 "
                "`main.cpp` 里那一行显式赋值，用户改完档位会看到"
                "「延时档位：需要重启」（这是对的，它确实要重启），"
                "但**两个具体字段不进 pending**，于是界面上写着已生效、"
                "编码器还按旧的线程数在跑")
        missing_l = [f for f in changed_l if f not in pend]
        r.check(not missing_l,
                f"两个具体值里变了的（{changed_l}）都进了 pending",
                f"少了 {missing_l}；pending 实际是 {pend}")

    # ---- 与画质档位正交 -------------------------------------------------
    #
    # **这一段是「两张档位表没有打架」的唯一证据。** 两张表展开的字段集合不相交
    # （画质那组是 fps / bitrate_kbps / max_height / sub_enabled；延时这组是
    # threads / gop_seconds），所以「流畅 + 极致低延迟」这种组合天然合法、不需要
    # 任何优先级规则 —— 但「不相交」是**设计意图**，不是运行时保证。
    # 写两次**反向**的 POST：只验一个方向的话，`validate_config` 里
    # 「后一段把前一段整个覆盖掉」这种错看不出来（两组各自的断言都会绿）。
    qtiers = [t for t in (cfg7b.get("quality_tiers") or [])
              if isinstance(t, dict) and t.get("key") != "custom"
              and all(isinstance(t.get(k), typ) for k, typ in
                      (("fps", int), ("bitrate_kbps", int),
                       ("max_height", int), ("sub_enabled", bool)))]
    real_l = next((t for t in lats if t["key"] == "realtime"), None)
    qual_l = next((t for t in lats if t["key"] == "quality"), None)
    if len(qtiers) >= 2 and real_l and qual_l:
        for q, lt, tag in ((qtiers[0], real_l, "正方向"),
                           (qtiers[1], qual_l, "反方向")):
            st_p, _, _ = api_post(
                "/api/config",
                {"capture": {"quality": q["key"], "latency": lt["key"]}})
            r.check(st_p == 200,
                    f"[正交·{tag}] POST {{quality:{q['key']}, "
                    f"latency:{lt['key']}}} → {st_p}")
            st, _, body = api_get("/api/config")
            got = {}
            if st == 200:
                try:
                    got = json.loads(body)
                except ValueError:
                    pass
            gc = got.get("capture") or {}
            r.check(gc.get("fps") == q["fps"]
                    and gc.get("bitrate_kbps") == q["bitrate_kbps"]
                    and gc.get("max_height") == q["max_height"]
                    and (got.get("rtsp") or {}).get("sub_enabled") == q["sub_enabled"],
                    f"[正交·{tag}] 画质那四项落成「{q['label']}」的值"
                    f"（fps {gc.get('fps')!r} / {gc.get('bitrate_kbps')!r} kbps / "
                    f"max_height {gc.get('max_height')!r} / 子码流 "
                    f"{(got.get('rtsp') or {}).get('sub_enabled')!r}）")
            r.check(gc.get("threads") == lt["threads"]
                    and gc.get("gop_seconds") == lt["gop_seconds"],
                    f"[正交·{tag}] 同时延时那两项落成「{lt['label']}」的值"
                    f"（threads {gc.get('threads')!r} / "
                    f"gop {gc.get('gop_seconds')!r}）",
                    "两组必须**同时**落对。任何一组被另一组盖掉，这里就是红的 —— "
                    "而这正是「两个档位表打架」唯一可观测的后果："
                    "用户选了「流畅 + 极致低延迟」，回来一看帧率对了、"
                    "线程数却被画质那一段顺手改回了 6（或者反过来）")
    else:
        print("  [note] 档位表不齐（画质非 custom 档 "
              f"{len(qtiers)} 个 / realtime {bool(real_l)} / "
              f"quality {bool(qual_l)}）—— 跳过正交那两条")

    # ---- 手改具体字段时，谁说了算 ---------------------------------------
    #
    # 计划里原本写的是「再手改 threads → latency 变成 custom」。**动手前查过代码：
    # 后端不做这件事。** `validate_config` 里只有「档位 → 字段」这一个方向；
    # 反过来那一半在前端（`ConfigView.vue` 的 `effectiveLatency()` 推出当前值落在
    # 哪一档、`buildPayload()` 把 `custom` 一起发上来）。
    #
    # 这不是缺陷，是分工 —— 但**分工的分界线必须钉住**：只要 `latency` 还是一个
    # 认识的档位，档位就赢，后端拿表里的值把用户手填的盖回去。少了这一条，
    # 「手填的 threads 会悄悄活下来」和「档位是权威的」这两种实现
    # 在别的断言下都是绿的。
    if lats:
        t_win = lats[0]
        bump = t_win["threads"] + 5
        api_post("/api/config",
                 {"capture": {"latency": t_win["key"], "threads": bump}})
        st, _, body = api_get("/api/config")
        got = {}
        if st == 200:
            try:
                got = json.loads(body)
            except ValueError:
                pass
        gc = got.get("capture") or {}
        r.check(gc.get("threads") == t_win["threads"],
                f"latency={t_win['key']} 时手改 threads={bump} 被拉回 "
                f"{t_win['threads']}（实际 {gc.get('threads')!r}）",
                "档位表是**权威的**：它展开的字段以它为准。前端要保住手填的值，"
                "必须**同时**把 latency 发成 custom（`buildPayload()` 就是这么做的）"
                "—— 只发 threads 的话，用户会看到自己填的数被静默改掉，"
                "而且改完还显示「已保存」")
        # 反过来说：后端**不做**「手改字段就翻 custom」这件事。写下来的理由是
        # 如果哪天有人给它加上了，这条会当场变红，而不是等到前端那半个逻辑
        # 变成一段没人敢删的冗余代码才被发现。
        r.check(gc.get("latency") == t_win["key"],
                f"而且 latency 仍是 {t_win['key']!r}（没被后端悄悄翻成 custom）")
    else:
        print("  [note] 档位表是空的 —— 跳过了「档位赢」那两条")

    # ---- 不认识的档位要说出来 -------------------------------------------
    st, _, body = api_get("/api/config")
    before_u = {}
    if st == 200:
        try:
            before_u = json.loads(body)
        except ValueError:
            before_u = {}
    bcap_u = before_u.get("capture") or {}

    api_post("/api/config", {"capture": {"latency": "zero"}})
    st, _, body = api_get("/api/config")
    got = {}
    if st == 200:
        try:
            got = json.loads(body)
        except ValueError:
            pass
    gc = got.get("capture") or {}
    r.check(gc.get("latency") == "custom",
            f"不认识的档位值被改成 custom（实际 {gc.get('latency')!r}）",
            "静默当成 custom 的话，用户会以为档位在管着、实际一个字段都没被管 —— "
            "而这正是「改了没反应」最难查的那一类。⚠️ **「为什么」那一半只进日志**"
            "（`apply_config` 把 notes 走 `D2O_WARN`，HTTP 上没有这个字段），"
            "所以「那句 note 里列出了合法值」由 `tests/latency_tier_check.cpp` 的 "
            "[2] 段盯着 —— 那里直接调 `validate_config` 拿 notes。"
            "两个断言各管一半，缺一条都会让另一半变成没人验的承诺")
    r.check(gc.get("threads") == bcap_u.get("threads")
            and gc.get("gop_seconds") == bcap_u.get("gop_seconds"),
            f"而 custom 一个字段都不动（threads 仍是 {bcap_u.get('threads')!r}、"
            f"gop 仍是 {bcap_u.get('gop_seconds')!r}）",
            f"实际 threads {gc.get('threads')!r} / "
            f"gop {gc.get('gop_seconds')!r}。打错一个字就把用户手填的两个数"
            "按某张表重写一遍，是这个功能最不该有的一种「顺手做好事」")

    # 还原。**必须还原到 [7b] 开始之前的那一份** —— 后面的 [8]/[9] 和这条 e2e 的
    # RTSP 断言都跑在这份配置上，「测试把被测配置改坏了」会让它们跟着一起错
    # （与 [7] 结尾那条还原同一个理由）。
    st_p, _, _ = api_post(
        "/api/config",
        {"capture": dict(lcap0),
         "rtsp": {"sub_enabled": (cfg7b.get("rtsp") or {}).get("sub_enabled")}})
    r.check(st_p == 200,
            f"还原 {l0!r} / threads {lcap0.get('threads')} / "
            f"gop {lcap0.get('gop_seconds')} / quality {lcap0.get('quality')!r} "
            f"→ {st_p}")

    # ---- 8. /api/audio/devices：配置页那个麦克风下拉框的来源 ----------
    #
    # 这一段的判据**全是结构性的**，一条都不依赖"这台机器上插了什么设备"：
    # 设备数与设备名随机器变，而"两个方向分开列""每台设备有非空 id""至多一台
    # 是默认"是后端对前端的承诺，换哪台机器都成立（陷阱 87：宾语是机器的断言
    # 只能报告、不能判）。
    print("\n[8] /api/audio/devices —— 麦克风下拉框的来源")
    st8, hd8, body8 = api_get("/api/audio/devices")
    r.check(st8 == 200, f"GET /api/audio/devices → {st8}（实际 {st8}）")
    # **缓存是要命的**：插拔一个耳机列表就变，缓存住的话下拉里会一直留着一台
    # 已经拔掉的设备 —— 选中它得到的是一条永远静音的音轨，而 SDP 和 ffprobe
    # 看上去都完全正常。这条断言盯的就是那个 no-store。
    r.check("no-store" in hd8.get("cache-control", "").lower(),
            f"Cache-Control 含 no-store（实际 {hd8.get('cache-control')!r}）",
            "设备列表按设计是**每次现枚举**的，浏览器缓存住它 = 用户在下拉里"
            "看到一台已经拔掉的设备")

    dev8 = {}
    try:
        dev8 = json.loads(body8)
    except ValueError as exc:
        r.check(False, f"/api/audio/devices 的正文是合法 JSON（{exc}）")
    r.check(isinstance(dev8.get("capture"), list) and isinstance(dev8.get("render"), list),
            f"capture / render 都是数组"
            f"（实际 {type(dev8.get('capture')).__name__} / "
            f"{type(dev8.get('render')).__name__}）",
            "两个方向**必须分开列**：混成一栏的话用户会在麦克风那里选到一个"
            "扬声器，然后得到一条永远静音的音轨")
    cap8 = dev8.get("capture") if isinstance(dev8.get("capture"), list) else []
    ren8 = dev8.get("render") if isinstance(dev8.get("render"), list) else []
    print(f"  [note] 本机枚举到：采集 {len(cap8)} 台、渲染 {len(ren8)} 台")

    bad_ep = [
        e for e in cap8 + ren8
        if not isinstance(e, dict)
        or not isinstance(e.get("id"), str) or not e.get("id")
        or not isinstance(e.get("name"), str) or not e.get("name")
        or not isinstance(e.get("default"), bool)
    ]
    r.check(not bad_ep,
            f"每台设备都有非空的 id / name 和布尔的 default（{len(cap8 + ren8)} 台）",
            f"不合形状的：{bad_ep[:2]}")
    # **id 一个都不能是空串。** 前端那个下拉框里"默认设备（跟随系统）"这一项
    # 是用 `value=""` 表示的，而它是**前端自己加的**、不来自这份列表。后端要是
    # 也回一个空 id，两个 option 就会撞在同一个 value 上，选中哪一个都分不出来。
    r.check(all(e.get("id") for e in cap8 + ren8 if isinstance(e, dict)),
            "没有哪台设备的 id 是空串（空串在界面上是「跟随系统」那一项）")
    # 至多一台默认。**判"至多"而不是"恰好"**：默认设备是另问一次
    # GetDefaultAudioEndpoint 得到的，那一次失败时后端会给出一组全非默认的设备
    # —— 那是这台机器 COM 的状态，不是后端的错（陷阱 87）。而"两台同时是默认"
    # 在任何情况下都不该出现：那就说明比较用的那个 id 写错了。
    for name, lst in (("采集", cap8), ("渲染", ren8)):
        ndef = len([e for e in lst if isinstance(e, dict) and e.get("default") is True])
        r.check(ndef <= 1, f"{name}端点里至多一台被标为系统默认（实际 {ndef} 台）")
    ids_cap = {e.get("id") for e in cap8 if isinstance(e, dict)}
    ids_ren = {e.get("id") for e in ren8 if isinstance(e, dict)}
    r.check(not (ids_cap & ids_ren),
            f"采集与渲染两组的 id 不重叠（重叠 {len(ids_cap & ids_ren)} 个）",
            "同一个端点 id 出现在两边 = 拿同一个 eDataFlow 枚举了两次")

    # 错误键**只在真出错时才出现**。恒存在（哪怕多半是空串）的话，前端就得靠
    # "空串算不算有错"来判断 —— 那是两边各写一次的口头约定。所以判据是：
    # 键在 ⟹ 值是非空字符串。
    for key in ("capture_error", "render_error"):
        if key in dev8:
            r.check(isinstance(dev8[key], str) and dev8[key].strip() != "",
                    f"{key} 出现了就是非空字符串（实际 {dev8[key]!r}）",
                    "枚举失败必须说出来：只回一个空数组的话，「这台机器上没插"
                    "麦克风」和「COM 起不来」在下拉框里长得一模一样")

    print(f"\n{'=' * 60}\n通过 {r.passed} 项，失败 {r.failed} 项。")
    return 1 if r.failed else 0


if __name__ == "__main__":
    sys.exit(main())

