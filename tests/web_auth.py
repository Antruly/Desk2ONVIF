#!/usr/bin/env python3
"""Web 控制台登录的契约检查。

这一段很容易"看起来没问题"：登录页能打开、输对口令能进去，就以为成了。
真正会出错的是**这些边界**，而它们的症状全是"某个页面上少了个东西"：

  - 该拦的没拦（新加的接口默认公开，而它和"故意公开"长得一模一样）
  - 不该拦的拦了（连 index.html 都要登录 → 用户看到一片空白，没法登录）
  - 令牌校验只看格式不看签名（伪造一个未来的过期时间就进去了）
  - 401 上带了 `WWW-Authenticate` → 浏览器弹出原生口令框，和我们的登录页
    是两套东西，用户在里面输什么都不对
  - ONVIF 那条路被顺带拦了 → NVR 从此拉不到快照，而日志里全是正常的 401

用法:
    python -I -X utf8 tests/web_auth.py --port 8099

`-X utf8` 不是多余的：`-I` 隐含 `-E`，`PYTHONIOENCODING` 在它下面不生效，
于是重定向到文件时中文会按 GBK 编码炸掉（见陷阱 53）。
"""

import argparse
import http.client
import json
import sys
import time

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


def req(port, method, path, body=None, headers=None):
    hd = dict(headers or {})
    if body is not None and "Content-Length" not in hd:
        hd["Content-Length"] = str(len(body))
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request(method, path, body=body, headers=hd)
    r = c.getresponse()
    data = r.read()
    h = {}
    for k, v in r.getheaders():
        k = k.lower()
        # Set-Cookie 允许多条，用列表收着（合并成一个串会让"哪一条是清 cookie"
        # 这件事看不出来）。
        h.setdefault(k, []).append(v)
    c.close()
    return r.status, h, data


def cookie_of(headers):
    """从 Set-Cookie 里取 `d2o_session=...` 的那一段（只到第一个 `;`）。"""
    for v in headers.get("set-cookie", []):
        if v.startswith(COOKIE + "="):
            return v.split(";", 1)[0]
    return None


def login(port, user="admin", password="admin"):
    body = json.dumps({"user": user, "password": password}).encode()
    st, hd, data = req(port, "POST", "/api/login", body,
                       {"Content-Type": "application/json"})
    if st != 200:
        return None, st, data
    return cookie_of(hd), st, data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin")
    a = ap.parse_args()
    r = R()

    # ---- 1. 没登录时该拦的必须拦得住 ------------------------------------
    print("\n[1] 未登录：/api/* 一律 401")
    for path in ("/api/status", "/api/config", "/api/onvif/restart"):
        method = "POST" if path.endswith("restart") else "GET"
        st, hd, body = req(a.port, method, path)
        r.check(st == 401, f"{method} {path} → {st}")
        try:
            j = json.loads(body)
        except ValueError:
            j = {}
        r.check(j.get("auth_required") is True,
                f"{method} {path} 的响应带 auth_required=true")

    # 404 也要 401：别把"这个接口存不存在"告诉没登录的人。
    st, _, _ = req(a.port, "GET", "/api/does-not-exist")
    r.check(st == 401, f"未登录访问不存在的 /api/ 路径也是 401（实际 {st}）")

    # ---- 2. 会话查询：匿名可访问，且不泄露用户名 -------------------------
    print("\n[2] /api/session 匿名可访问")
    st, _, body = req(a.port, "GET", "/api/session")
    r.check(st == 200, f"GET /api/session → {st}（它必须匿名可达）")
    j = json.loads(body)
    r.check(j.get("auth_required") is True, f"auth_required = {j.get('auth_required')}")
    r.check(j.get("authenticated") is False, f"authenticated = {j.get('authenticated')}")
    # 没登录时报用户名 = 把一个"这台机器上有个叫 X 的账号"白送给扫端口的人。
    r.check("user" not in j, f"未登录时响应里没有 user 字段（实际键：{sorted(j)}）")

    # ---- 3. 静态资源必须匿名可达 ----------------------------------------
    #
    # **这一条是最容易踩的**：把静态目录也一起拦掉的后果不是"更安全"，而是
    # 没登录的人连登录页本身都下载不到 —— 他面对的是一个 401 的空白页，
    # 连输入框都没有。
    print("\n[3] 静态资源匿名可达（否则登录页自己也下不来）")
    st, hd, body = req(a.port, "GET", "/")
    r.check(st == 200, f"GET / → {st}")
    r.check("text/html" in (hd.get("content-type", [""])[0]),
            f"Content-Type = {hd.get('content-type')}")
    r.check(b"<!DOCTYPE" in body[:200] or b"<html" in body[:200].lower(),
            "拿到的是 index.html 而不是一段错误文本")

    # ---- 4. ONVIF 那条路不能被顺带拦了 ----------------------------------
    #
    # NVR 走的是 Digest，它不该被要求"先去网页登录一次"。
    print("\n[4] ONVIF SOAP 端点仍走 Digest，不受 Web 登录影响")
    st, hd, _ = req(a.port, "POST", "/onvif/device_service",
                    b"<x/>", {"Content-Type": 'application/soap+xml; charset=utf-8'})
    r.check(st == 401, f"无凭据的 SOAP → {st}")
    wa = (hd.get("www-authenticate", [""])[0] or "")
    r.check(wa.lower().startswith("digest"), f"WWW-Authenticate 是 Digest 挑战：{wa[:60]}")

    # ---- 5. 口令校验 ----------------------------------------------------
    print("\n[5] 口令")
    st, hd, _ = req(a.port, "POST", "/api/login",
                    json.dumps({"user": a.user, "password": a.password + "-x"}).encode(),
                    {"Content-Type": "application/json"})
    r.check(st == 401, f"错口令 → {st}")
    r.check(cookie_of(hd) is None, "错口令时**不**下发 cookie")

    st, _, _ = req(a.port, "POST", "/api/login",
                   json.dumps({"user": a.user + "-x", "password": a.password}).encode(),
                   {"Content-Type": "application/json"})
    r.check(st == 401, f"对的口令 + 错的用户名 → {st}（用户名也参与校验）")

    # 格式不对的请求体要 400 而不是 500/崩掉。
    st, _, _ = req(a.port, "POST", "/api/login", b"not json",
                   {"Content-Type": "application/json"})
    r.check(st == 400, f"非法 JSON → {st}")

    # ---- 6. 登录成功：cookie 的属性 -------------------------------------
    print("\n[6] 登录成功")
    st, hd, body = req(a.port, "POST", "/api/login",
                       json.dumps({"user": a.user, "password": a.password}).encode(),
                       {"Content-Type": "application/json"})
    r.check(st == 200, f"POST /api/login → {st}：{body[:160]}")
    raw = None
    for v in hd.get("set-cookie", []):
        if v.startswith(COOKIE + "="):
            raw = v
    r.check(raw is not None, f"下发了 {COOKIE} cookie")
    cookie = cookie_of(hd)
    if raw is None:
        print("\n没法继续：登录拿不到 cookie。")
        print(f"\n通过 {r.passed} 项，失败 {r.failed} 项")
        return 1
    low = raw.lower()
    # HttpOnly：中间那个 XSS 偷不到 cookie。前端本来也不需要读它
    # （要问"我登录了吗"就调 /api/session）。
    r.check("httponly" in low, f"cookie 带 HttpOnly：{raw}")
    r.check("samesite=lax" in low, f"cookie 带 SameSite=Lax：{raw}")
    r.check("path=/" in low, f"cookie 的 Path=/：{raw}")
    # Max-Age 要和 SessionStore::kTtlSeconds（7 天）对上 —— 这两个数分处
    # C++ 和 Python 两侧，没有编译器帮着对账。
    r.check("max-age=604800" in low, f"Max-Age=604800（7 天）：{raw}")
    # secure 必须是 false：这个服务跑在明文 HTTP 上，置 true 的话浏览器直接
    # 不收这个 cookie，症状是"登录成功、下一个请求又是未登录"。
    r.check("secure" not in low.split(";")[0] and "; secure" not in low,
            f"**没有** Secure 属性（明文 HTTP 上置它会让人永远登不进去）：{raw}")

    # ---- 7. 带上 cookie 就放行 ------------------------------------------
    print("\n[7] 带 cookie：全部放行")
    for path in ("/api/status", "/api/config"):
        st, _, _ = req(a.port, "GET", path, headers={"Cookie": cookie})
        r.check(st == 200, f"GET {path} → {st}")
    st, _, body = req(a.port, "GET", "/api/session", headers={"Cookie": cookie})
    j = json.loads(body)
    r.check(j.get("authenticated") is True, f"/api/session authenticated = {j.get('authenticated')}")
    r.check(j.get("user") == a.user, f"/api/session 登录后带用户名：{j.get('user')}")

    # ---- 8. 令牌是签过名的，伪造的要被拒 --------------------------------
    print("\n[8] 令牌的签名与过期")
    token = cookie.split("=", 1)[1]
    expiry, _, sig = token.partition(".")
    r.check(len(expiry) == 16 and len(sig) == 32,
            f"令牌是 <16位过期时间>.<32位md5>（实得 {len(expiry)}.{len(sig)}）")

    # 改一位签名：格式全对，只有签名不对。只看格式的实现会在这里放行。
    flipped = "0" if sig[0] != "0" else "1"
    st, _, _ = req(a.port, "GET", "/api/status",
                   headers={"Cookie": f"{COOKIE}={expiry}.{flipped}{sig[1:]}"})
    r.check(st == 401, f"签名改一位 → {st}")

    # 自己编一个"一小时后过期"的时间，但不签名。
    future = format(int(time.time()) + 3600, "016x")
    st, _, _ = req(a.port, "GET", "/api/status",
                   headers={"Cookie": f"{COOKIE}={future}.{'0' * 32}"})
    r.check(st == 401, f"伪造未来的过期时间 → {st}（签发时间不参与校验就完蛋了）")

    # 过期时间就在过去：签名对也没用。签名由服务端算不出来（密钥是随机的），
    # 所以只能验"过期这一半" —— 用一个**过去**的时间配上正确的签名做不到，
    # 于是这里退一步：改过期时间但保留原签名，两者必须同时被验。
    past = format(int(time.time()) - 10, "016x")
    st, _, _ = req(a.port, "GET", "/api/status",
                   headers={"Cookie": f"{COOKIE}={past}.{sig}"})
    r.check(st == 401, f"改过期时间 → {st}")

    # 乱七八糟的值不能把服务打崩。
    for junk in ("", "abc", "....", "0" * 200, "zzzz.zzzz"):
        st, _, _ = req(a.port, "GET", "/api/status", headers={"Cookie": f"{COOKIE}={junk}"})
        if st != 401:
            r.check(False, f"畸形 cookie {junk!r} → {st}（应当是 401）")
            break
    else:
        r.check(True, "5 种畸形 cookie 全部 401（没有把服务打崩）")

    # ---- 9. 快照的两条路：给网页的那条不带 Digest 挑战 ------------------
    #
    # 这一条是有浏览器含义的：401 + `WWW-Authenticate` 会让 Chrome/Firefox
    # 弹一个**系统级**的用户名/口令框，它和我们的登录页毫无关系，用户在里面
    # 输什么都不对。所以给网页用的那条路径必须永远不发挑战。
    print("\n[9] 快照的两条路")
    st, hd, _ = req(a.port, "GET", "/api/snapshot.jpg")
    r.check(st == 401, f"未登录的 /api/snapshot.jpg → {st}")
    r.check("www-authenticate" not in hd,
            f"且**不带** WWW-Authenticate（否则浏览器会弹原生口令框）：{hd.get('www-authenticate')}")

    st, hd, _ = req(a.port, "GET", "/snapshot.jpg")
    r.check(st == 401, f"未登录的 /snapshot.jpg（NVR 那条） → {st}")
    wa = (hd.get("www-authenticate", [""])[0] or "")
    r.check(wa.lower().startswith("digest"),
            f"且**带** Digest 挑战（ONVIF 客户端靠它走第二步）：{wa[:60]}")

    st, hd, body = req(a.port, "GET", "/api/snapshot.jpg", headers={"Cookie": cookie})
    # 采集刚起来时可能还没有帧，那是 503；有帧就是 200 + JPEG。
    r.check(st in (200, 503), f"登录后 /api/snapshot.jpg → {st}")
    if st == 200:
        r.check(body[:2] == b"\xff\xd8", f"是 JPEG（头两字节 {body[:2]!r}）")
        r.check((hd.get("cache-control", [""])[0] or "").find("no-store") >= 0,
                f"带 Cache-Control: no-store：{hd.get('cache-control')}")

    # ---- 10. 登出 -------------------------------------------------------
    #
    # **登出只是让浏览器丢掉 cookie，服务端什么都没做。** 令牌是无状态的，
    # 拿着旧令牌的人照样进得来 —— 这一条**刻意断言"它仍然有效"**，把语义钉死：
    # 哪天有人以为登出做了更多（比如做了服务端吊销），这条会当场告诉他不是。
    # 真要立刻踢掉所有会话只有一条路：重启进程（签名密钥换了）。
    print("\n[10] 登出")
    st, hd, _ = req(a.port, "POST", "/api/logout")
    r.check(st == 200, f"POST /api/logout → {st}")
    clear = [v for v in hd.get("set-cookie", []) if v.startswith(COOKIE + "=")]
    r.check(bool(clear) and "max-age=0" in clear[0].lower(),
            f"回应里让 cookie 立即过期：{clear}")
    st, _, _ = req(a.port, "GET", "/api/status", headers={"Cookie": cookie})
    r.check(st == 200,
            f"旧令牌仍然有效（{st}）—— 无状态令牌的既定语义，不是缺陷")

    # ---- 11. 失败限速 ---------------------------------------------------
    #
    # 放在最后：它会把来源 IP 打进冷却，之后所有登录都会 429。
    print("\n[11] 失败限速")
    codes = []
    for _ in range(6):
        st, _, _ = req(a.port, "POST", "/api/login",
                       json.dumps({"user": a.user, "password": "definitely-wrong"}).encode(),
                       {"Content-Type": "application/json"})
        codes.append(st)
    r.check(codes[:5] == [401] * 5, f"前 5 次错口令都是 401：{codes}")
    r.check(codes[5] == 429, f"第 6 次被冷却 → {codes[5]}")

    st, hd, body = req(a.port, "POST", "/api/login",
                       json.dumps({"user": a.user, "password": a.password}).encode(),
                       {"Content-Type": "application/json"})
    r.check(st == 429, f"冷却期间**正确的**口令也拒绝 → {st}")
    r.check("retry-after" in hd, f"带 Retry-After 头：{hd.get('retry-after')}")
    try:
        j = json.loads(body)
        r.check(isinstance(j.get("retry_after_s"), int) and 0 < j["retry_after_s"] <= 60,
                f"响应体里的 retry_after_s = {j.get('retry_after_s')}")
    except ValueError:
        r.check(False, "冷却响应不是 JSON")

    print(f"\n通过 {r.passed} 项，失败 {r.failed} 项")
    return 0 if r.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
