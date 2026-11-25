#!/usr/bin/env python3
"""ONVIF SOAP 端点的 WS-Security UsernameToken 测试。

验的是**第二条鉴权通路**：报文在自己的 SOAP Header 里带
`<wsse:Security><wsse:UsernameToken>`，服务端在 HTTP Digest 缺失/不对时
应当接受它；而 Digest 那条路必须原样可用，`GetSystemDateAndTime` 的匿名
入口也不能被碰坏。

    python -I -X utf8 tests/onvif_wsse.py --host 127.0.0.1 --port 8080
    python -I -X utf8 tests/onvif_wsse.py --user admin --password admin

## 判据为什么落在"再读一次"上

`GetDeviceInformation` 成功时回 200 + 一份设备信息；失败时回 401 + Digest
挑战。**401 一定要带挑战** —— 这是既有行为，也是"WSSE 失败没有被改成
500"的直接证据。所有成功用例都再用严格 XML 解析一遍响应，避免"200 但
报文是坏的"混过去。

## 用到的公共件

`Client` / `Runner` / `envelope` / `op_body` / `parse_challenge` 都在
`tests/onvif_common.py`。按绝对路径显式加载（`-I` 不把脚本目录放进
sys.path，见那个文件的说明）。

## 这些用例**不**覆盖什么

* 真实 NVR 的实现差异（有的发不带 `Type` 的 Password、有的 Created 不带
  毫秒）—— 后者在本地 C++ 自检里验，前者 [1c] 顺带验一条；
* 证书/签名/SAML —— 本实现明确不碰；
* 服务端日志内容 —— 只能靠代码审查（日志里不许出现 nonce/Created/摘要）。
"""

import argparse
import base64
import datetime as _dt
import hashlib
import importlib.util as _ilu
import os
import sys
from xml.sax.saxutils import escape as _xml_escape

WSSE_NS = ("http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-wssecurity-secext-1.0.xsd")
WSU_NS = ("http://docs.oasis-open.org/wss/2004/01/"
          "oasis-200401-wss-wssecurity-utility-1.0.xsd")
PW_TEXT = ("http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-username-token-profile-1.0#PasswordText")
PW_DIGEST = ("http://docs.oasis-open.org/wss/2004/01/"
             "oasis-200401-wss-username-token-profile-1.0#PasswordDigest")
B64_BINARY = ("http://docs.oasis-open.org/wss/2004/01/"
              "oasis-200401-wss-soap-message-security-1.0#Base64Binary")


def _load_common():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "onvif_common.py")
    spec = _ilu.spec_from_file_location("onvif_common", path)
    mod = _ilu.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_common = _load_common()
NS = _common.NS
envelope = _common.envelope
Client = _common.Client
parse_challenge = _common.parse_challenge
Runner = _common.Runner
op_body = _common.op_body


def utc_created(offset_s=0):
    """设备侧 `wsu:Created`：UTC ISO8601，带毫秒。

    **带毫秒是故意的**：真实客户端多半这么写（`2026-11-25T12:34:56.789Z`），
    而服务端那个解析器最容易在这里写成"只认 20 个字符、见到 `.` 就拒"。
    """
    t = _dt.datetime.now(_dt.timezone.utc) + _dt.timedelta(seconds=offset_s)
    return t.strftime("%Y-%m-%dT%H:%M:%S.") + f"{t.microsecond // 1000:03d}Z"


def new_nonce():
    """16 字节随机数的 Base64。每次成功请求都必须换一个，否则会撞重放缓存。"""
    return base64.b64encode(os.urandom(16)).decode("ascii")


def password_digest(nonce_b64, created, password):
    """Base64(SHA1(Base64Decode(Nonce) + Created + Password))。"""
    raw = base64.b64decode(nonce_b64)
    return base64.b64encode(
        hashlib.sha1(raw + created.encode("utf-8")
                     + password.encode("utf-8")).digest()).decode("ascii")


def token_header(user, password, *, kind="text", nonce=None, created=None,
                 include=("username", "password", "nonce", "created"),
                 type_attr=True):
    """拼 `<wsse:Security><wsse:UsernameToken>…` 这一段的 XML。

    `include` 用来造"缺字段"的报文；`kind="digest"` 时 `Password` 里放的是
    摘要而不是口令；`type_attr=False` 造"省略 Type"的合法报文（规范默认
    PasswordText）。
    """
    nonce = new_nonce() if nonce is None else nonce
    created = utc_created() if created is None else created
    if kind == "digest":
        secret = password_digest(nonce, created, password)
        ptype = PW_DIGEST
    else:
        secret = password
        ptype = PW_TEXT

    out = [f'<wsse:Security xmlns:wsse="{WSSE_NS}" xmlns:wsu="{WSU_NS}">',
           "<wsse:UsernameToken>"]
    if "username" in include:
        out.append(f"<wsse:Username>{_xml_escape(user)}</wsse:Username>")
    if "password" in include:
        attr = f' Type="{_xml_escape(ptype)}"' if type_attr else ""
        out.append(f"<wsse:Password{attr}>{_xml_escape(secret)}</wsse:Password>")
    if "nonce" in include:
        out.append(f'<wsse:Nonce EncodingType="{B64_BINARY}">'
                   f"{_xml_escape(nonce)}</wsse:Nonce>")
    if "created" in include:
        out.append(f"<wsu:Created>{_xml_escape(created)}</wsu:Created>")
    out.append("</wsse:UsernameToken></wsse:Security>")
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--user", default="admin",
                    help="ONVIF 用户名（与 Digest 同一份凭据）")
    ap.add_argument("--password", default="admin",
                    help="ONVIF 口令（与 Digest 同一份凭据）")
    a = ap.parse_args()

    c = Client(a.host, a.port)
    R = Runner()
    DEV = "/onvif/device_service"
    MED = "/onvif/media_service"

    def wsse_post(path, inner, header):
        """发一条带 SOAP Header 的请求。**不带 Authorization**。

        `inner` 是 **Body 里那个操作元素**（`op_body(...)` 的结果），不是
        完整信封 —— 信封由 `envelope()` 拼一次。以前这里把完整信封又包了
        一层，报文变成 Envelope 套 Envelope，服务端只会回 401/500。
        """
        return c.post(path, envelope(inner, header=header))

    def expect_401(st, hd, data, what):
        ok = R.check(st == 401, f"{what} → HTTP 401（实际 {st}）", data[:200])
        chal = hd.get("www-authenticate", "")
        R.check(bool(chal) and chal.lower().startswith("digest"),
                f"{what}：401 带 Digest 挑战", chal[:80])
        return ok

    def device_info_ok(header, what):
        inner = op_body("GetDeviceInformation", NS["tds"])
        st, hd, data = wsse_post(DEV, inner, header)
        if not R.check(st == 200, f"{what} → HTTP 200（实际 {st}）", data[:300]):
            return
        root = R.strict_xml(data, what)
        if root is not None:
            mf = R.find(root, ".//tds:Manufacturer")
            R.check(mf is not None and mf.text,
                    f"{what}：Manufacturer = "
                    f"{mf.text if mf is not None else None}")

    # ---- 0. 匿名入口不受影响 -------------------------------------------
    print("\n[0] GetSystemDateAndTime 仍然**匿名**可访问")
    time_inner = op_body("GetSystemDateAndTime", NS["tds"])
    st, hd, data = c.post(DEV, envelope(time_inner))
    R.check(st == 200, f"不带任何凭据 → HTTP 200（实际 {st}）", data[:200])
    R.strict_xml(data, "GetSystemDateAndTime")

    # **匿名优先**：哪怕 Header 里塞了一份错口令的 UsernameToken，这个操作的
    # 匿名入口也不该因此变成 401 —— 客户端要靠它拿时间来算 Digest，而它此刻
    # 还没有可信的凭据可以带。
    bad_hdr = token_header(a.user, a.password + "-wrong")
    st, hd, data = c.post(DEV, envelope(time_inner, header=bad_hdr))
    R.check(st == 200,
            f"带错口令的 UsernameToken 仍是 200（匿名优先，实际 {st}）",
            data[:200])

    # ---- 1. PasswordText ------------------------------------------------
    print("\n[1] PasswordText")
    device_info_ok(token_header(a.user, a.password), "1a 正确口令")

    # 规范里 Type 可省略、默认 PasswordText。真实客户端里有这么发的。
    device_info_ok(token_header(a.user, a.password, type_attr=False),
                   "1b 省略 Type（默认 PasswordText）")

    # 用户名里带 XML 特殊字符：序列化出去是 `&amp;`。这里用**一个不同的**
    # 用户名，所以预期 401 —— 它验的是"带实体的报文不会把服务端弄崩/弄成
    # 500"，以及实体解码后仍然走正常的用户名比较。实体还原本身（`&amp;`
    # → `&`）在 wss.cpp 的本机自检里用独立向量验过。
    if "&" not in a.user and "&" not in a.password:
        who = a.user + "&x"
        # 服务端存的是 a.user/a.password，所以这条会 401 —— 这里验的是
        # "带实体也不会 500/崩"，不是"用错用户名能登录"。
        body = op_body("GetDeviceInformation", NS["tds"])
        st, hd, data = wsse_post(DEV, body, token_header(who, a.password))
        R.check(st == 401, f"1c 用户名带 &amp; 时正常回 401（实际 {st}）",
                data[:200])

    # ---- 2. PasswordDigest ---------------------------------------------
    print("\n[2] PasswordDigest")
    device_info_ok(token_header(a.user, a.password, kind="digest"),
                   "2a 正确摘要（Created 带毫秒）")
    device_info_ok(token_header(a.user, a.password, kind="digest",
                                created=utc_created().split(".")[0] + "Z"),
                   "2b Created 不带毫秒也认")

    # ---- 3. 错口令 ------------------------------------------------------
    print("\n[3] 错口令必须被拒（两种 Password 形态）")
    for what, kind in (("3a PasswordText", "text"),
                       ("3b PasswordDigest", "digest")):
        body = op_body("GetDeviceInformation", NS["tds"])
        st, hd, data = wsse_post(
            DEV, body, token_header(a.user, a.password + "-wrong", kind=kind))
        expect_401(st, hd, data, what)

    # ---- 4. Nonce 重放 --------------------------------------------------
    print("\n[4] 同一 nonce 第二次必须被拒")
    header = token_header(a.user, a.password)
    body = op_body("GetDeviceInformation", NS["tds"])
    st1, _, d1 = wsse_post(DEV, body, header)
    R.check(st1 == 200, f"第一次 → HTTP 200（实际 {st1}）", d1[:200])
    st2, hd2, d2 = wsse_post(DEV, body, header)
    expect_401(st2, hd2, d2, "4a 原样重放")
    # 换一个 nonce（其余不变）必须又能用：证明拒绝的是 nonce，不是把用户拉黑。
    st3, _, d3 = wsse_post(DEV, body, token_header(a.user, a.password))
    R.check(st3 == 200, f"4b 换 nonce 后恢复 → HTTP 200（实际 {st3}）", d3[:200])

    # ---- 5. Created 窗口 -----------------------------------------------
    print("\n[5] Created 超出 ±5 分钟必须被拒")
    for what, off in (("5a 6 分钟前", -360), ("5b 6 分钟之后", 360)):
        body = op_body("GetDeviceInformation", NS["tds"])
        h = token_header(a.user, a.password, created=utc_created(off))
        st, hd, data = wsse_post(DEV, body, h)
        expect_401(st, hd, data, what)

    # ---- 6. 缺字段 / 没有 Security -------------------------------------
    print("\n[6] 缺字段一律 401（不是崩，也不是 500）")
    body = op_body("GetDeviceInformation", NS["tds"])
    for what, inc in (
            ("6a 缺 Nonce", ("username", "password", "created")),
            ("6b 缺 Created", ("username", "password", "nonce")),
            ("6c 缺 Password", ("username", "nonce", "created")),
            ("6d 缺 Username", ("password", "nonce", "created"))):
        st, hd, data = wsse_post(DEV, body,
                                 token_header(a.user, a.password, include=inc))
        expect_401(st, hd, data, what)
    st, hd, data = c.post(DEV, envelope(body))
    expect_401(st, hd, data, "6e 完全没有 Security 头")
    st, hd, data = wsse_post(DEV, body,
                             f'<wsse:Security xmlns:wsse="{WSSE_NS}"></wsse:Security>')
    expect_401(st, hd, data, "6f 有 Security 但没有 UsernameToken")

    # ---- 7. Header 上限 -------------------------------------------------
    print("\n[7] 超大 SOAP Header 要被上限挡住（401，不是 OOM/500）")
    huge = token_header(a.user, a.password)
    pad = "x" * (80 * 1024)
    huge = huge.replace("<wsse:UsernameToken>",
                        "<wsse:Padding>" + pad + "</wsse:Padding>"
                        "<wsse:UsernameToken>", 1)
    st, hd, data = wsse_post(DEV, body, huge)
    expect_401(st, hd, data, "7a 80KB Security 头")

    # ---- 8. Digest 那条路必须原样可用 ----------------------------------
    print("\n[8] HTTP Digest 仍然可用（两套凭据并存）")
    soap = envelope(body)
    st, hd, data = c.post(DEV, soap)
    ch = parse_challenge(hd.get("www-authenticate", ""))
    R.check(st == 401 and bool(ch.get("nonce")),
            f"8a 匿名 → 401 且拿到挑战（实际 {st}）", data[:200])
    if ch.get("nonce"):
        auth = c.digest(DEV, soap, a.user, a.password, ch["nonce"],
                        ch["realm"], ch.get("qop", "auth"))
        st, hd, data = c.post(DEV, soap, auth=auth)
        R.check(st == 200, f"8b 正确 Digest → HTTP 200（实际 {st}）", data[:300])
        R.strict_xml(data, "8b Digest GetDeviceInformation")

    # ---- 9. 媒体服务同一个 handle() ------------------------------------
    print("\n[9] 媒体服务端点也接受 WSSE")
    prof_body = op_body("GetProfiles", NS["trt"])
    st, hd, data = wsse_post(MED, prof_body, token_header(a.user, a.password))
    R.check(st == 200, f"GetProfiles（WSSE）→ HTTP 200（实际 {st}）", data[:300])
    root = R.strict_xml(data, "GetProfiles（WSSE）")
    if root is not None:
        prof = R.find(root, ".//trt:Profiles")
        R.check(prof is not None, "响应里找到 trt:Profiles")

    print(f"\n{'=' * 60}\n通过 {R.passed} 项，失败 {R.failed} 项，"
          f"跳过 {R.skipped} 项。")
    return 1 if R.failed else 0


if __name__ == "__main__":
    sys.exit(main())
