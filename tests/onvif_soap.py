#!/usr/bin/env python3
"""ONVIF SOAP 端点测试：命名空间正确性 + Digest 挑战/应答 + Fault 形状。

为什么不用现成的 ONVIF 库（onvif-zeep 之类）：它们会**替我们**把错误的响应
"修好"—— 比如宽容地忽略未绑定的前缀。而这里要验的恰恰是那些东西。
所以只用标准库的 `http.client` 和 `xml.etree.ElementTree`，后者是严格的：
**未声明的前缀直接抛 ParseError**。

这正是已经踩过的那个坑的检测方式：`uvcpp_soap_dump_response` 只声明信封
自身的 `soap:` 前缀，不声明 inner_xml 里用到的前缀，于是产出未绑定前缀，
而报文"看起来"完全正常。贪心的正则匹配看不出这个。

用法:
    python -I -X utf8 tests/onvif_soap.py --host 127.0.0.1 --port 8080
    python -I -X utf8 tests/onvif_soap.py --user admin --password admin

`--user`/`--password` 是 **ONVIF Digest** 那一对（NVR 添加通道时填的）。
第 [9] 段要打 `/api/config`，那一条走的是 **Web 控制台登录**，凭据用
`--web-user`/`--web-password`（默认 admin/admin）—— 两套凭据刻意分开，
合起来会让"改了 Web 口令结果 NVR 也连不上了"这类问题在测试里隐身。

**`/onvif/*` 的请求一律不带会话 cookie**：那条路验的就是 Digest 本身，
顺手带上 cookie 的话，一个 Digest 校验写坏了的实现会因为 cookie 而被放行，
而测试全绿。

-I：隔离模式，不把脚本目录和当前目录塞进 sys.path。
-X utf8：`-I` 隐含 `-E`，PYTHONIOENCODING 在它下面不生效，重定向到文件时
中文会按 GBK 编码炸掉。
"""

import argparse
import hashlib
import http.client
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

NS = {
    "soap": "http://www.w3.org/2003/05/soap-envelope",
    "soap11": "http://schemas.xmlsoap.org/soap/envelope/",
    "tds": "http://www.onvif.org/ver10/device/wsdl",
    "trt": "http://www.onvif.org/ver10/media/wsdl",
    "tt": "http://www.onvif.org/ver10/schema",
    "ter": "http://www.onvif.org/ver10/error",
}

ENVELOPE = """<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:tds="http://www.onvif.org/ver10/device/wsdl"
               xmlns:trt="http://www.onvif.org/ver10/media/wsdl"
               xmlns:tt="http://www.onvif.org/ver10/schema">
  <soap:Body>{body}</soap:Body>
</soap:Envelope>"""


def envelope(body):
    return ENVELOPE.format(body=body).encode("utf-8")


def md5(s):
    return hashlib.md5(s.encode("utf-8")).hexdigest()


class Client:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.conn = http.client.HTTPConnection(host, port, timeout=8)
        # Web 控制台的会话 cookie。**只挂在 /api/* 上**，见 `_cookie_for`。
        self.cookie = None

    def _cookie_for(self, path):
        """这条路径要不要带会话 cookie。

        只有 `/api/*` 带。`/onvif/*` 走的是 HTTP Digest —— 那是另一套凭据，
        而本文件存在的意义之一就是验 Digest 本身。给 SOAP 请求也带上 cookie
        的话，一个 Digest 校验写坏了的实现会因为 cookie 而被放行，整个
        [2]/[3]/[4] 段变成空转，还全是绿的。
        """
        return self.cookie if (self.cookie and path.startswith("/api/")) else None

    def post(self, path, body, auth=None, ctype=None):
        headers = {
            "Content-Type": ctype or 'application/soap+xml; charset=utf-8',
            "Content-Length": str(len(body)),
        }
        if auth:
            headers["Authorization"] = auth
        ck = self._cookie_for(path)
        if ck:
            headers["Cookie"] = ck
        self.conn.request("POST", path, body=body, headers=headers)
        r = self.conn.getresponse()
        data = r.read()
        return r.status, dict((k.lower(), v) for k, v in r.getheaders()), data

    def get(self, path):
        headers = {"Accept": "application/json"}
        ck = self._cookie_for(path)
        if ck:
            headers["Cookie"] = ck
        self.conn.request("GET", path, headers=headers)
        r = self.conn.getresponse()
        return r.status, dict((k.lower(), v) for k, v in r.getheaders()), r.read()

    def post_json(self, path, obj):
        body = json.dumps(obj).encode("utf-8")
        return self.post(path, body, ctype="application/json")

    # ---- Web 会话（只给 /api/* 用）-------------------------------------
    def auth_state(self):
        """GET /api/session（匿名可达）。拿不到返回 None。"""
        st, _, b = self.get("/api/session")
        if st != 200:
            return None
        try:
            return json.loads(b)
        except ValueError:
            return None

    def web_login(self, user, password):
        """POST /api/login，成功则把会话 cookie 存进 self.cookie。

        返回 (cookie 或 None, 状态码, body)。
        """
        body = json.dumps({"user": user, "password": password}).encode("utf-8")
        st, hd, data = self.post("/api/login", body, ctype="application/json")
        if st != 200:
            return None, st, data
        # 这里**单独取一次头**而不是复用 post 的 dict：Set-Cookie 允许出现多条，
        # 收进 dict 会把"哪一条是会话"这件事灭掉（登录这一条恰好只有一条，
        # 但清 cookie 那条会有两条，将来复用这段代码时不想再踩）。
        raw = hd.get("set-cookie", "")
        for piece in [raw] if isinstance(raw, str) else list(raw):
            if piece.startswith("d2o_session="):
                self.cookie = piece.split(";", 1)[0]
                return self.cookie, st, data
        return None, st, data

    def digest(self, path, body, user, password, nonce, realm, qop, nc="00000001"):
        """按 RFC 2617 算一次 Authorization 头的值（qop=auth）。"""
        cnonce = "0a4f113b"
        ha1 = md5(f"{user}:{realm}:{password}")
        ha2 = md5(f"POST:{path}")
        resp = md5(f"{ha1}:{nonce}:{nc}:{cnonce}:{qop}:{ha2}")
        return (
            f'Digest username="{user}", realm="{realm}", nonce="{nonce}", '
            f'uri="{path}", response="{resp}", qop={qop}, nc={nc}, '
            f'cnonce="{cnonce}", algorithm=MD5'
        )


def parse_challenge(header):
    """从 `WWW-Authenticate: Digest ...` 里抠出 realm / nonce / qop。"""
    out = {}
    for m in re.finditer(r'(\w+)=(?:"([^"]*)"|([^,\s]+))', header or ""):
        out[m.group(1).lower()] = m.group(2) if m.group(2) is not None else m.group(3)
    return out


class Runner:
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

    def strict_xml(self, data, what):
        """严格解析 + 根元素必须是 SOAP Envelope。

        未绑定前缀在这里会直接抛 ParseError —— 这就是本文件存在的理由。
        """
        try:
            root = ET.fromstring(data)
        except ET.ParseError as e:
            self.check(False, f"{what}：严格 XML 解析", e)
            return None
        tag = root.tag
        if tag not in (f"{{{NS['soap']}}}Envelope", f"{{{NS['soap11']}}}Envelope"):
            self.check(False, f"{what}：根元素是 SOAP Envelope", f"实际 {tag}")
            return None
        self.check(True, f"{what}：严格 XML 解析 + 根元素 {tag.split('}')[1]}")
        return root

    def find(self, root, path):
        return root.find(path, NS)


def op_body(name, ns, inner=""):
    return f'<{name} xmlns="{ns}">{inner}</{name}>'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--user", default="admin",
                    help="ONVIF Digest 用户名（NVR 添加通道时填的那个）")
    ap.add_argument("--password", default="admin",
                    help="ONVIF Digest 口令")
    ap.add_argument("--web-user", default="admin",
                    help="Web 控制台用户名（第 [9] 段打 /api/* 用）")
    ap.add_argument("--web-password", default="admin",
                    help="Web 控制台口令（第 [9] 段打 /api/* 用）")
    a = ap.parse_args()

    c = Client(a.host, a.port)
    R = Runner()
    DEV = "/onvif/device_service"
    MED = "/onvif/media_service"

    # ---- 1. 匿名 GetSystemDateAndTime ----------------------------------
    print("\n[1] GetSystemDateAndTime 必须**匿名**可访问")
    body = envelope(op_body("GetSystemDateAndTime", NS["tds"]))
    st, hd, data = c.post(DEV, body)
    R.check(st == 200, f"HTTP 200（实际 {st}）", data[:200])
    root = R.strict_xml(data, "GetSystemDateAndTime")
    if root is not None:
        utc = R.find(root, ".//tds:UTCDateTime/tt:Year")
        loc = R.find(root, ".//tds:LocalDateTime/tt:Hour")
        tz = R.find(root, ".//tds:TimeZone/tt:TZ")
        R.check(utc is not None and utc.text.isdigit(),
                f"UTCDateTime/Year = {utc.text if utc is not None else None}")
        R.check(loc is not None, f"LocalDateTime 存在（Hour={loc.text if loc is not None else None}）")
        R.check(tz is not None and re.match(r"^PT-?\d", tz.text or ""),
                f"TimeZone 是 ISO8601 时长：{tz.text if tz is not None else None}")
        dt = R.find(root, ".//tds:DateTimeType")
        R.check(dt is not None and dt.text == "NTP",
                f"DateTimeType = {dt.text if dt is not None else None}（NTP：没有 SetSystemDateAndTime）")

    # ---- 2. 未鉴权 → 401 且必须带挑战 -----------------------------------
    print("\n[2] 未鉴权时必须 401，且**必须**带 WWW-Authenticate")
    body = envelope(op_body("GetDeviceInformation", NS["tds"]))
    st, hd, data = c.post(DEV, body)
    chal = hd.get("www-authenticate", "")
    R.check(st == 401, f"HTTP 401（实际 {st}）", data[:200])
    # 空挑战 = 不发这个头，而客户端拿到一个没有挑战的 401 只能放弃。
    R.check(bool(chal), "带 WWW-Authenticate 头", "（空挑战的 401 客户端无法应对）")
    R.check(chal.lower().startswith("digest"), f"是 Digest 挑战：{chal[:70]}…")
    fields = parse_challenge(chal)
    for k in ("realm", "nonce"):
        R.check(bool(fields.get(k)), f"挑战里有 {k}")

    # ---- 3. 正确摘要 → 200 ---------------------------------------------
    print("\n[3] 正确摘要必须通过")
    auth = c.digest(DEV, body, a.user, a.password,
                    fields["nonce"], fields["realm"], fields.get("qop", "auth"))
    st, hd, data = c.post(DEV, body, auth=auth)
    R.check(st == 200, f"HTTP 200（实际 {st}）", data[:300])
    root = R.strict_xml(data, "GetDeviceInformation")
    if root is not None:
        mf = R.find(root, ".//tds:Manufacturer")
        md = R.find(root, ".//tds:Model")
        R.check(mf is not None and mf.text, f"Manufacturer = {mf.text if mf is not None else None}")
        R.check(md is not None and md.text, f"Model = {md.text if md is not None else None}")

    # ---- 4. 错误摘要 → 401 ---------------------------------------------
    print("\n[4] 错误口令必须被拒")
    bad = c.digest(DEV, body, a.user, a.password + "-wrong",
                   fields["nonce"], fields["realm"], fields.get("qop", "auth"))
    st, hd, data = c.post(DEV, body, auth=bad)
    R.check(st == 401, f"HTTP 401（实际 {st}）")

    # ---- 5. 媒体服务：GetProfiles --------------------------------------
    print("\n[5] GetProfiles —— 跨命名空间，本文件最要紧的一项")
    body = envelope(op_body("GetProfiles", NS["trt"]))
    auth = c.digest(MED, body, a.user, a.password,
                    fields["nonce"], fields["realm"], fields.get("qop", "auth"))
    st, hd, data = c.post(MED, body, auth=auth)
    R.check(st == 200, f"HTTP 200（实际 {st}）", data[:300])
    root = R.strict_xml(data, "GetProfiles")
    token = None
    blocks = {}
    if root is not None:
        # <trt:Profiles> 里装 <tt:Name> —— 两个不同的命名空间。
        # `uvcpp_soap_dump_response` 那条路在这里必挂（tt: 未声明）。
        prof = R.find(root, ".//trt:Profiles")
        R.check(prof is not None, "找到 trt:Profiles")
        name = R.find(root, ".//trt:Profiles/tt:Name")
        R.check(name is not None and name.text, f"内层 tt:Name = {name.text if name is not None else None}")
        enc = R.find(root, ".//tt:VideoEncoderConfiguration/tt:Encoding")
        R.check(enc is not None and enc.text == "H264", f"Encoding = {enc.text if enc is not None else None}")
        res = R.find(root, ".//tt:VideoEncoderConfiguration/tt:Resolution/tt:Width")
        R.check(res is not None and res.text.isdigit(), f"分辨率宽 = {res.text if res is not None else None}")
        gov = R.find(root, ".//tt:H264/tt:GovLength")
        R.check(gov is not None and gov.text.isdigit(), f"GovLength = {gov.text if gov is not None else None}")
        if prof is not None:
            token = prof.get("token")
        R.check(bool(token), f"token = {token}")

        # ---- 主码流 + 子码流两个 profile --------------------------------
        # 两路必须**各自独立**：token 不同、分辨率不同。任何一处相同都说明
        # 后端把两路当成了一路（比如 set_video_size 是"给所有 profile 设同一个
        # 尺寸"的老写法），而现象是 NVR 把子码流当主码流拉 —— 画面是对的，
        # 只是带宽白烧，没有任何报错。
        blocks.update({p.get("token"): p for p in root.findall(".//trt:Profiles", NS)})
        if len(blocks) > 1:
            R.check(len(blocks) == 2, f"profile 数 = {len(blocks)}（主 + 子）")
            R.check(set(blocks) == {"main", "sub"}, f"token 集合 = {sorted(blocks)}")
            dims = {}
            for tok, blk in blocks.items():
                w = blk.find(".//tt:VideoEncoderConfiguration/tt:Resolution/tt:Width", NS)
                h = blk.find(".//tt:VideoEncoderConfiguration/tt:Resolution/tt:Height", NS)
                dims[tok] = (int(w.text), int(h.text)) if w is not None and h is not None else None
            R.check(all(d for d in dims.values()), f"两路都有分辨率：{dims}")
            if dims.get("main") and dims.get("sub"):
                mw, mh = dims["main"]
                sw, sh = dims["sub"]
                R.check(sh < mh, f"子码流比主码流矮：{sw}x{sh} < {mw}x{mh}")
                R.check(abs(mw / mh - sw / sh) < 0.02,
                        f"两路宽高比一致：{mw / mh:.4f} vs {sw / sh:.4f}")
        else:
            # 没开子码流也合法（--no-sub）。那就只有一路，且不报错。
            R.check(len(blocks) == 1, "只报了一路（未开子码流）")

    # ---- 6. GetStreamUri -------------------------------------------------
    print("\n[6] GetStreamUri —— 主机地址必须按请求来源反推")
    uri = None
    uris = {}
    for tok in (sorted(blocks) if root is not None and len(blocks) > 1 else [token]):
        if not tok:
            continue
        inner = f"<trt:ProfileToken>{tok}</trt:ProfileToken>"
        body = envelope(op_body("GetStreamUri", NS["trt"], inner))
        auth = c.digest(MED, body, a.user, a.password,
                        fields["nonce"], fields["realm"], fields.get("qop", "auth"))
        st, hd, data = c.post(MED, body, auth=auth)
        R.check(st == 200, f"[{tok}] HTTP 200（实际 {st}）", data[:300])
        sub_root = R.strict_xml(data, f"GetStreamUri[{tok}]")
        if sub_root is not None:
            u = R.find(sub_root, ".//trt:MediaUri/tt:Uri")
            u = u.text if u is not None else None
            uris[tok] = u
            R.check(bool(u and u.startswith("rtsp://")), f"[{tok}] Uri = {u}")
            # 多网卡机器上写死地址会让 NVR 去连一个它够不着的网段，
            # 而症状只是"添加失败"。这里至少确认回的不是 127.0.0.1。
            if u:
                host = u.split("//", 1)[1].split(":")[0]
                R.check(not host.startswith("127."), f"[{tok}] 主机 {host} 不是环回")
        if tok == token:
            uri = uris.get(tok)
    # 两路的地址必须是**两个不同的路径**。相同的话，NVR 拉"子码流"会拿到
    # 主码流的画面 —— 一切正常，只是没省到带宽，看不出任何异常。
    if len(uris) > 1:
        R.check(len(set(uris.values())) == len(uris), f"两路地址不同：{uris}")
        R.check(uris.get("main", "").rstrip("/").split("/")[-1] == "live",
                f"主码流路径 = {uris.get('main')}")
        R.check(uris.get("sub", "").rstrip("/").split("/")[-1] == "live_sub",
                f"子码流路径 = {uris.get('sub')}")

    # ---- 7. 不认识的接口 → ONVIF 形状的 Fault ----------------------------
    print("\n[7] 未实现的操作必须回结构良好的 ONVIF Fault")
    body = envelope(op_body("GetAnalyticsEngines", NS["trt"]))
    auth = c.digest(MED, body, a.user, a.password,
                    fields["nonce"], fields["realm"], fields.get("qop", "auth"))
    st, hd, data = c.post(MED, body, auth=auth)
    R.check(st == 500, f"HTTP 500（实际 {st}）")
    root = R.strict_xml(data, "Fault")
    if root is not None:
        val = R.find(root, ".//soap:Code/soap:Value")
        sub = R.find(root, ".//soap:Code/soap:Subcode/soap:Value")
        # **关键**：ONVIF 的"不支持"是 Receiver 不是 Sender（我们一开始写错了）。
        R.check(val is not None and val.text == "soap:Receiver",
                f"顶层码 = {val.text if val is not None else None}（应为 soap:Receiver）")
        R.check(sub is not None and sub.text == "ter:ActionNotSupported",
                f"子码 = {sub.text if sub is not None else None}")
        # ter: 前缀必须**被声明**，否则 ElementTree 早就抛了 —— 上面那步
        # strict_xml 已经覆盖。这里再确认它落在对的命名空间里。
        R.check(sub is not None and sub.tag == f"{{{NS['soap']}}}Value",
                "子码元素在 SOAP 命名空间里")

    # ---- 8. 畸形报文：带凭据回 Fault，不带凭据回 401 挑战 -----------------
    print("\n[8] 畸形报文的分岔：带凭据 → Fault，不带凭据 → 401 挑战")
    junk = b"this is not xml at all"
    # 带了凭据 = "已登录但报文写坏了"，必须回**结构良好的 Fault**，不能是断掉
    # 的 HTTP 响应 —— 部分 NVR 遇到畸形响应会直接放弃添加设备。
    auth = c.digest(DEV, junk, a.user, a.password,
                    fields["nonce"], fields["realm"], fields.get("qop", "auth"))
    st, hd, data = c.post(DEV, junk, auth=auth)
    R.check(st == 500, f"带凭据时 HTTP 500（实际 {st}）", data[:200])
    R.strict_xml(data, "畸形报文的 Fault")
    # 不带凭据 = 还没鉴权过，此刻该收到的是"请先鉴权"。回 Fault 的话，一个
    # 正在等挑战的客户端（curl --digest、以及多数 NVR 的第一次请求）会认为
    # 服务端坏了而放弃，永远走不到算摘要那一步。
    st, hd, data = c.post(DEV, junk)
    R.check(st == 401, f"不带凭据时 HTTP 401（实际 {st}）", data[:200])
    R.check(bool(hd.get("www-authenticate", "")), "401 上带 WWW-Authenticate 挑战")

    # ---- 9. 热生效的边界：身份当场生效，别的字段进 pending -----------------
    #
    # 这一段要证的是一件很具体的事：**"/api/config 说某项已生效"这句话是真的。**
    # 后端可以在回应里写 applied:["onvif.model"]，而实际什么都没改 —— 那正是
    # 这个功能要防的谎，只是换了个方向。所以这里不只读回应，还**再走一次 SOAP
    # 去看设备真的报什么**，全程不重启。
    #
    # 反过来，"要重启"那一侧也得验：改个子码流高度必须出现在 pending 里。
    # 两边都验才说明**边界画在正确的地方** —— 只验一边的话，一个把所有字段都
    # 报成 applied 的实现、和一个把所有字段都报成 pending 的实现，都能过。
    # ---- 8b. Web 会话登录 ------------------------------------------------
    #
    # 上面八段走的都是 /onvif/*（Digest）。第 [9] 段要打 `/api/config`，而那是
    # **Web 控制台的会话**挡着的 —— 上了登录之后，不带 cookie 的 /api/config
    # 一律 401，于是这一段会以"GET /api/config 拿不到"整体垮掉。
    #
    # 顺手钉一条：**不带 cookie 时必须 401**。它同时也在守一件容易被顺手改坏的
    # 事：cookie 只该管 /api/*，不该蔓延到 /onvif/* 上去。
    print("\n[8b] Web 会话登录（第 [9] 段要打 /api/config）")
    sess = c.auth_state()
    if sess is None:
        R.check(False, "GET /api/session → 拿不到（后端没起来？）")
    elif not sess.get("auth_required"):
        print("  [note] 服务端没开登录（auth_required=false），/api/* 匿名可达")
    else:
        st, _, _ = c.get("/api/config")
        R.check(st == 401, f"不带 cookie 的 GET /api/config → {st}（应当是 401）")
        ck, st, data = c.web_login(a.web_user, a.web_password)
        R.check(ck is not None,
                f"POST /api/login → {st}，拿到会话 cookie（user={a.web_user}）",
                data[:200])
        if ck is not None:
            st, _, _ = c.get("/api/config")
            R.check(st == 200, f"带上 cookie 的 GET /api/config → {st}")

    print("\n[9] 运行中改配置：ONVIF 身份当场生效，码流参数进 pending")

    def get_cfg():
        st, _, b = c.get("/api/config")
        if st != 200:
            return None
        try:
            return json.loads(b)
        except ValueError:
            return None

    def device_info(user, password):
        """取一次 GetDeviceInformation。**每次都重新要挑战** —— 换过口令之后
        旧的 nonce 会被刻意作废（陷阱 30 的反面用法），复用会得到一个假失败。"""
        b = envelope(op_body("GetDeviceInformation", NS["tds"]))
        st, hd, _ = c.post(DEV, b)
        f = parse_challenge(hd.get("www-authenticate", ""))
        if not f.get("nonce"):
            return None, 0
        auth = c.digest(DEV, b, user, password, f["nonce"], f["realm"], f.get("qop", "auth"))
        st, hd, data = c.post(DEV, b, auth=auth)
        if st != 200:
            return None, st
        try:
            r = ET.fromstring(data)
        except ET.ParseError:
            return None, st
        out = {}
        for tag in ("Manufacturer", "Model", "SerialNumber", "HardwareId"):
            el = r.find(f".//tds:{tag}", NS)
            out[tag] = el.text if el is not None else None
        return out, st

    cfg0 = get_cfg()
    if cfg0 is None:
        R.check(False, "GET /api/config 拿不到，热生效这段没法验")
    else:
        old_model = cfg0.get("onvif", {}).get("model", "")
        old_serial = cfg0.get("onvif", {}).get("serial_number", "")

        # -- 9a. 身份：POST 一份**局部**配置，只改这两项 -------------------
        # 局部 body 是合法输入（config_from_json 对缺失的键保留当前值），
        # 也正是 Web 界面之外最自然的改法：curl 打个补丁。
        st, _, b = c.post_json("/api/config", {
            "onvif": {"model": "HotSwap-E2E", "serial_number": "HOT-E2E-0001"}})
        R.check(st == 200, f"改 ONVIF 身份 → HTTP {st}", b[:200])
        try:
            res = json.loads(b)
        except ValueError:
            res = {}
        applied = res.get("applied") or []
        pending = res.get("pending") or []
        R.check("onvif.model" in applied and "onvif.serial_number" in applied,
                f"回应里 applied 含这两项：{applied}")
        R.check("onvif.model" not in pending and "onvif.serial_number" not in pending,
                f"且这两项**不在** pending 里：{pending}")

        # -- 9b. 真的生效了吗？再走一次 SOAP 看设备报什么 -------------------
        # **这一条才是重点。** 上面那些只看后端自己怎么说；只有这里是从
        # 一条真实的 ONVIF 请求里读回来的，中间没有重启。
        info, st = device_info(a.user, a.password)
        R.check(st == 200, f"改完之后 GetDeviceInformation → HTTP {st}")
        if info:
            R.check(info.get("Model") == "HotSwap-E2E",
                    f"设备报的 Model 立刻变成了 HotSwap-E2E（实际 {info.get('Model')}）"
                    " —— 没有重启")
            R.check(info.get("SerialNumber") == "HOT-E2E-0001",
                    f"SerialNumber 也立刻变了（实际 {info.get('SerialNumber')}）")

        # -- 9c. 冷字段：改子码流高度必须进 pending ------------------------
        st, _, b = c.post_json("/api/config", {"rtsp": {"sub_height": 288}})
        R.check(st == 200, f"改子码流高度 → HTTP {st}", b[:200])
        try:
            res = json.loads(b)
        except ValueError:
            res = {}
        R.check("rtsp.sub_height" in (res.get("pending") or []),
                f"rtsp.sub_height 在 pending 里：{res.get('pending')}")
        R.check("rtsp.sub_height" not in (res.get("applied") or []),
                f"且**不在** applied 里（它确实没生效）：{res.get('applied')}")
        R.check(res.get("restart_required") is True,
                f"restart_required = {res.get('restart_required')}（有 pending 就该是 true）")

        # -- 9d. 待重启清单要活过这次请求 ---------------------------------
        # POST 的回应是一次性的。刷新页面、过一会儿再回来，那份"还欠着什么"
        # 必须还在 —— 只有 GET 上有它，这个提示才跟配置本身一样活得久。
        cfg1 = get_cfg()
        R.check(cfg1 is not None and "rtsp.sub_height" in (cfg1.get("pending") or []),
                f"GET /api/config 里也带着 pending："
                f"{(cfg1 or {}).get('pending')}")

        # -- 9e. 口令：改完当场生效，旧的立刻作废 --------------------------
        # 这是热生效里最值钱的一项 —— 换口令不该把正在推的流掐断。
        newpw = "e2e-hot-" + md5(a.password)[:8]
        st, _, b = c.post_json("/api/config", {"onvif": {"password": newpw}})
        R.check(st == 200, f"改 ONVIF 口令 → HTTP {st}", b[:200])
        _, st_old = device_info(a.user, a.password)
        R.check(st_old == 401, f"旧口令立刻失效（实际 {st_old}）")
        info2, st_new = device_info(a.user, newpw)
        R.check(st_new == 200, f"新口令立刻可用（实际 {st_new}）")

        # -- 9f. 复原 ------------------------------------------------------
        # 用**明文原值**而不是哨兵：哨兵的含义是"保持当前值"，而当前值已经是
        # 刚设的那个新口令了 —— 拿哨兵复原等于没复原，而且这个错很安静。
        st, _, _ = c.post_json("/api/config", {
            "onvif": {"password": a.password, "model": old_model,
                      "serial_number": old_serial}})
        R.check(st == 200, f"还原身份与口令 → HTTP {st}")
        info3, st3 = device_info(a.user, a.password)
        R.check(st3 == 200 and info3 and info3.get("Model") == old_model,
                f"原口令恢复可用且 Model 回到 {old_model}（实际 "
                f"{None if not info3 else info3.get('Model')}）")

    print(f"\n{'=' * 60}\n通过 {R.passed} 项，失败 {R.failed} 项。")
    return 1 if R.failed else 0


if __name__ == "__main__":
    sys.exit(main())
