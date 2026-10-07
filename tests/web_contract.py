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
    before = dict(rtsp)
    rtsp["sub_height"] = 288
    rtsp["sub_bitrate_kbps"] = 384
    st, _, body = api_post("/api/config", cfg)
    r.check(st == 200, f"POST /api/config → {st}", body[:300])
    try:
        res = json.loads(body)
    except ValueError:
        res = {}
    # 回应必须是**逐字段**的：一次保存里常常"这项当场生效、那项要重启"，
    # 只给一个 restart_required 布尔值时前端只能二选一，必有一种是在骗人。
    r.check(isinstance(res.get("applied"), list) and isinstance(res.get("pending"), list),
            f"POST 回应里有 applied/pending 两个数组（实际 "
            f"{type(res.get('applied')).__name__}/{type(res.get('pending')).__name__}）")
    # 子码流高度是**冷**字段：改它要重建编码器，所以它必须出现在 pending 里，
    # 而且**不能**出现在 applied 里 —— 报成已生效就是在骗人。
    r.check("rtsp.sub_height" in (res.get("pending") or []),
            f"rtsp.sub_height 进了 pending：{res.get('pending')}")
    r.check("rtsp.sub_height" not in (res.get("applied") or []),
            f"且没进 applied：{res.get('applied')}")
    r.check(res.get("restart_required") is True,
            f"有 pending 时 restart_required 为 true（实际 {res.get('restart_required')}）")

    st, _, body = api_get("/api/config")
    after_all = json.loads(body)
    after = after_all.get("rtsp", {})
    r.check(after.get("sub_height") == 288,
            f"sub_height 往返 {before.get('sub_height')} → {after.get('sub_height')}")
    r.check(after.get("sub_bitrate_kbps") == 384,
            f"sub_bitrate_kbps 往返 {before.get('sub_bitrate_kbps')} → {after.get('sub_bitrate_kbps')}")
    # 刚 POST 进去的那一项，在紧接着的 GET 里也必须在 pending 里 —— 这一条把
    # "GET 上那个数组"和"POST 回应的那个数组"接起来，防止它们各说各话。
    r.check("rtsp.sub_height" in (after_all.get("pending") or []),
            f"紧接着的 GET 里 pending 仍含 rtsp.sub_height：{after_all.get('pending')}")

    # ---- 3. /api/status --------------------------------------------------
    print("\n[3] /api/status —— 逐路字段")
    st, _, body = api_get("/api/status")
    r.check(st == 200, f"GET /api/status → {st}")
    s = json.loads(body)
    for key in ("nalu_dropped", "max_nalu_bytes", "rtsp_clients", "rtsp_url",
                "sub_width", "sub_height", "rtsp_streams",
                "cursor_frames", "pointer_only_frames"):
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
        mismatch = sorted(k for k in (http_fields & ws_fields)
                          if k != "ws_clients" and ws_j[k] != s[k])
        r.check(not mismatch, "同名字段的值一致", f"不一致: {mismatch}")
        # 这条是防"两边都缺同一个字段"：字段集相等可以由"都很小"满足。
        r.check(len(http_fields) >= 25, f"/api/status 字段数 {len(http_fields)} ≥ 25")

    print(f"\n{'=' * 60}\n通过 {r.passed} 项，失败 {r.failed} 项。")
    return 1 if r.failed else 0


if __name__ == "__main__":
    sys.exit(main())

