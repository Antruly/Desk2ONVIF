#!/usr/bin/env python3
"""ONVIF 测试的公共件：SOAP 信封、HTTP 客户端、断言计数器。

`tests/onvif_soap.py` 与 `tests/onvif_wsse.py` 都用这一份。抽出来不是为了
少打几个字，而是因为**两套凭据的测试必须对同一个请求形状下断言** —— 一边
发 SOAP 1.2、另一边发别的版本，或者一边用严格解析、一边用宽容解析，得到的
"通过"就不再是同一件事了。

## 为什么调用方要按**绝对路径**加载这个文件，而不是 `import onvif_common`

两个探针都按仓库惯例用 `python -I -X utf8 tests/xxx.py` 跑。`-I` 隐含
`-P`：**脚本目录不进 `sys.path`**（隔离模式的本意是不让当前目录里的同名
模块顶掉标准库）。所以 `import onvif_common` 会 ImportError。调用方用
`importlib.util.spec_from_file_location` 显式加载**这一个**文件 —— 不扩大
`sys.path`，也不会误加载别处的同名模块。
"""

import hashlib
import http.client
import json
import re
import xml.etree.ElementTree as ET

NS = {
    "soap": "http://www.w3.org/2003/05/soap-envelope",
    "soap11": "http://schemas.xmlsoap.org/soap/envelope/",
    "tds": "http://www.onvif.org/ver10/device/wsdl",
    "trt": "http://www.onvif.org/ver10/media/wsdl",
    "tt": "http://www.onvif.org/ver10/schema",
    "ter": "http://www.onvif.org/ver10/error",
    "wsse": "http://docs.oasis-open.org/wss/2004/01/"
            "oasis-200401-wss-wssecurity-secext-1.0.xsd",
    "wsu": "http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-wssecurity-utility-1.0.xsd",
}

# 信封上把 wsse/wsu 也声明好：WSSE 探针要在 Header 里用它们，而**前缀声明
# 只覆盖声明它的元素及其后代** —— 声明在 Envelope 上一次就够，Security
# 自己再声明一遍也不冲突（测试里那样写是为了让片段能单独看懂）。
ENVELOPE = """<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:tds="http://www.onvif.org/ver10/device/wsdl"
               xmlns:trt="http://www.onvif.org/ver10/media/wsdl"
               xmlns:tt="http://www.onvif.org/ver10/schema"
               xmlns:wsse="{wsse}"
               xmlns:wsu="{wsu}">
{header}  <soap:Body>{body}</soap:Body>
</soap:Envelope>"""


def envelope(body, header=""):
    """拼一份 SOAP 1.2 信封。`header` 是 `<soap:Header>` 的**内容**。

    空 header 时**不写** `<soap:Header>` 元素：现有那两百多条断言一直发的是
    不带 Header 的报文，保持原样最不容易把别的东西带歪。
    """
    h = f"  <soap:Header>{header}</soap:Header>\n" if header else ""
    return ENVELOPE.format(wsse=NS["wsse"], wsu=NS["wsu"],
                           header=h, body=body).encode("utf-8")


def op_body(name, ns, inner=""):
    return f'<{name} xmlns="{ns}">{inner}</{name}>'


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
        self.skipped = 0

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

    def skip(self, what, why):
        """一段**因为环境条件不成立**而没跑成 —— 报告，但不判（陷阱 87）。

        它单列一个计数、并进最后的汇总行，为的是一件很具体的事：**"跳过"不能
        长得像"通过"**。"这次没验到"和"这次验过了、是好的"在一份只有"通过 N /
        失败 0"的报告里完全同形，而它们的含义正好相反 —— 这正是陷阱 46 那一族
        （信号缺失时看着像成功）在测试自己身上的写法。
        """
        self.skipped += 1
        print(f"  [SKIP] {what}")
        for line in str(why).splitlines():
            print(f"         {line}")
        return False

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

    def findall(self, root, path):
        """`ElementTree.findall` 的命名空间版。

        和 `find` 成对：探针里凡是按前缀写路径的地方都该走这两个，免得
        一半用 `NS`、另一半靠默认命名空间 —— 后者在带前缀的响应上恒为空，
        而"空列表"和"查询失败"会长得一模一样。
        """
        return root.findall(path, NS)
