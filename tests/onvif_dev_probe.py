#!/usr/bin/env python3
"""一个端口上的 ONVIF 设备自报家门：`GetDeviceInformation` + `GetProfiles` + `GetStreamUri`。

为什么单独写一个，而不是拿 `tests/onvif_soap.py` 凑：

那个文件是**一整套**验收（2800 行，里面会改配置、对时、读审计、重启采集）。
它在多通道这条路上要验的东西只有一件 —— **"端口 8101 上是不是另一台设备"**
—— 而跑整套的代价是它同时要求那个端口上有 Web 控制台（`/api/config` 之类），
而通道 2 起的端口**故意没有**那一套（见 README「通道 2 起的端点只有 ONVIF」）。
硬拿它去跑 8101，红的是"这个端点没有网页 API"，而不是产品有问题。

所以这里只要三件事，全部走 SOAP：

    GetDeviceInformation  → Manufacturer / Model / SerialNumber / HardwareId
    GetProfiles           → 每一路 profile 的 token 和分辨率
    GetStreamUri(profile) → 那个 profile 报出来的 RTSP 地址

判据（在本脚本里一眼能对上的两件）：

  * 两台设备的 `SerialNumber` **必须不同** —— 相同的话 NVR 会把它们当成同一台，
    后加的那一路直接覆盖掉先加的那一路（症状是"只能加进去一个通道"）。
  * `GetStreamUri` 报出来的地址里那个**路径**必须和这一路自己的 RTSP 路径一致 ——
    这才是"通道 2 真的推在 /ch2 上"的**正面**证据。光看 `/api/status` 里写着
    `/ch2` 是**我们自己的话**，不是流真的在那儿。

用法：
    python -I -X utf8 tests/onvif_dev_probe.py --port 8101
    python -I -X utf8 tests/onvif_dev_probe.py --port 8101 --json

退出码：0 = 三件事都拿到了；1 = 有任何一件没拿到或形状不对。
**退出码 1 时 stdout 上仍有那行 `PROBE ...`**（带 `err`），调用方据此报出
"是哪一个端口、坏在哪一步"，而不是只有一句"失败了"。

`-I`：隔离模式，不把脚本目录和当前目录塞进 sys.path。
`-X utf8`：`-I` 隐含 `-E`，`PYTHONIOENCODING` 在它下面不生效，重定向到文件时
中文会按 GBK 编码炸掉。
"""

import argparse
import hashlib
import http.client
import json
import re
import sys
import xml.etree.ElementTree as ET

NS = {
    "soap": "http://www.w3.org/2003/05/soap-envelope",
    "soap11": "http://schemas.xmlsoap.org/soap/envelope/",
    "tds": "http://www.onvif.org/ver10/device/wsdl",
    "trt": "http://www.onvif.org/ver10/media/wsdl",
    "tt": "http://www.onvif.org/ver10/schema",
}

# 信封上**把三个前缀全部声明出来**。这是踩过的那个坑：只声明信封自身的
# `soap:` 时，inner XML 里用到的 `tds:`/`trt:` 就是未绑定前缀，而
# `xml.etree` 对此**直接抛 ParseError**（它不做宽容补全）—— 顺带也就把
# "响应是不是合法 XML"这件事一起验了。
ENVELOPE = """<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:tds="http://www.onvif.org/ver10/device/wsdl"
               xmlns:trt="http://www.onvif.org/ver10/media/wsdl"
               xmlns:tt="http://www.onvif.org/ver10/schema">
  <soap:Body>{body}</soap:Body>
</soap:Envelope>"""

DEV_PATH = "/onvif/device_service"
MED_PATH = "/onvif/media_service"


class Client:
    def __init__(self, host, port):
        self.host = host
        self.port = port

    def post(self, path, body, auth=None):
        c = http.client.HTTPConnection(self.host, self.port, timeout=10)
        try:
            headers = {"Content-Type": 'application/soap+xml; charset=utf-8'}
            if auth:
                headers["Authorization"] = auth
            c.request("POST", path, body=body.encode("utf-8"), headers=headers)
            r = c.getresponse()
            data = r.read()
            # 头一律小写 —— `HTTPMessage` 的取值是不区分大小写的，但下面要按
            # 名字取，统一成小写省得记两种写法。
            hd = {k.lower(): v for k, v in r.getheaders()}
            return r.status, hd, data
        finally:
            c.close()

    @staticmethod
    def digest(path, body, user, password, nonce, realm, qop="auth", nc="00000001"):
        """RFC 2617 的 `Authorization` 头（qop=auth）。"""
        def md5(s):
            return hashlib.md5(s.encode("utf-8")).hexdigest()

        ha1 = md5(f"{user}:{realm}:{password}")
        ha2 = md5(f"POST:{path}")
        if qop:
            resp = md5(f"{ha1}:{nonce}:{nc}:{'0' * 8}:{qop}:{ha2}")
            tail = f'qop={qop}, nc={nc}, cnonce={"0" * 8}'
        else:
            resp = md5(f"{ha1}:{nonce}:{ha2}")
            tail = ""
        head = f'Digest username="{user}", realm="{realm}", nonce="{nonce}", uri="{path}"'
        return f"{head}, response=\"{resp}\"" + (f", {tail}" if tail else "")


def parse_challenge(header):
    out = {}
    for m in re.finditer(r'(\w+)\s*=\s*"([^"]*)"', header or ""):
        out[m.group(1).lower()] = m.group(2)
    return out


def op(name, ns, inner=""):
    return f'<{name} xmlns="{ns}">{inner}</{name}>'


def call(c, path, body, user, password):
    """打一次带 Digest 的 SOAP。返回 `(root, err)`。"""
    st, hd, data = c.post(path, body)
    if st != 401:
        return None, f"未鉴权那次应当是 401，实际 {st}：{data[:160]!r}"
    ch = parse_challenge(hd.get("www-authenticate", ""))
    if not ch.get("nonce") or not ch.get("realm"):
        return None, f"挑战里缺 nonce/realm：{hd.get('www-authenticate', '')!r}"
    auth = c.digest(path, body, user, password, ch["nonce"], ch["realm"],
                    ch.get("qop", "auth"))
    st, hd, data = c.post(path, body, auth=auth)
    if st != 200:
        return None, f"带摘要那次应当是 200，实际 {st}：{data[:160]!r}"
    try:
        root = ET.fromstring(data)
    except ET.ParseError as e:
        # 未绑定前缀会走到这里 —— 那正是本文件要能看见的形状。
        return None, f"响应不是合法 XML：{e}"
    if root.tag not in (f"{{{NS['soap']}}}Envelope", f"{{{NS['soap11']}}}Envelope"):
        return None, f"根元素不是 SOAP Envelope：{root.tag}"
    return root, None


def text_of(root, tag):
    # **两个 `find` 之间绝不能写 `or`。** ElementTree 的元素定义了 `__bool__`
    # （等于"有没有子元素"），所以 `<tds:Manufacturer>Desk2ONVIF</tds:Manufacturer>`
    # 这种**有文字、没子元素**的元素在 `or` 眼里是假 —— 于是一个明明答对了的
    # 响应会被判成空字符串。这个坑在这里已经踩过一次：探针报"没有 SerialNumber"，
    # 而设备其实答得好好的（Python 3.12 起还会就这件事打 DeprecationWarning）。
    for ns in ("tds", "tt"):
        el = root.find(f".//{ns}:{tag}", NS)
        if el is not None and el.text:
            return el.text
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin")
    ap.add_argument("--json", action="store_true",
                    help="只打一行 JSON（脚本调用方用这个）")
    a = ap.parse_args()

    c = Client(a.host, a.port)
    out = {"port": a.port, "err": ""}

    root, err = call(c, DEV_PATH, ENVELOPE.format(body=op("GetDeviceInformation", NS["tds"])),
                     a.user, a.password)
    if root is None:
        out["err"] = f"GetDeviceInformation：{err}"
    else:
        out["manufacturer"] = text_of(root, "Manufacturer")
        out["model"] = text_of(root, "Model")
        out["serial"] = text_of(root, "SerialNumber")
        out["hardware"] = text_of(root, "HardwareId")
        if not out["serial"]:
            out["err"] = "GetDeviceInformation 里没有 SerialNumber —— 两台设备就分不出来了"

    if not out["err"]:
        root, err = call(c, MED_PATH, ENVELOPE.format(body=op("GetProfiles", NS["trt"])),
                         a.user, a.password)
        if root is None:
            out["err"] = f"GetProfiles：{err}"
        else:
            profs = []
            for p in root.findall(".//trt:Profiles", NS):
                token = p.get("token", "")
                res = p.find(".//tt:Resolution", NS)
                w = h = ""
                if res is not None:
                    wel = res.find("tt:Width", NS)
                    hel = res.find("tt:Height", NS)
                    w = wel.text if wel is not None else ""
                    h = hel.text if hel is not None else ""
                uri = ""
                # 每个 profile 单独问一次 GetStreamUri —— 这才是"这一路真的推在
                # 那条路径上"的正面证据。
                body = ENVELOPE.format(body=op(
                    "GetStreamUri", NS["trt"],
                    f'<trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>'
                    f'<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>'
                    f'</trt:StreamSetup><trt:ProfileToken>{token}</trt:ProfileToken>'))
                r2, err2 = call(c, MED_PATH, body, a.user, a.password)
                if r2 is not None:
                    u = r2.find(".//tt:Uri", NS)
                    uri = u.text if u is not None and u.text else ""
                elif not out["err"]:
                    out["err"] = f"GetStreamUri({token})：{err2}"
                profs.append({"token": token, "width": w, "height": h, "uri": uri})
            out["profiles"] = profs
            if not profs and not out["err"]:
                out["err"] = "GetProfiles 回了一个 profile 都没有"

    if a.json:
        print("PROBE " + json.dumps(out, ensure_ascii=False))
    else:
        print(f"端口 {a.port}")
        if out["err"]:
            print(f"  [FAIL] {out['err']}")
        else:
            print(f"  Manufacturer {out['manufacturer']}")
            print(f"  Model        {out['model']}")
            print(f"  SerialNumber {out['serial']}")
            print(f"  HardwareId   {out['hardware']}")
            for p in out.get("profiles", []):
                print(f"  profile {p['token']}: {p['width']}x{p['height']}  {p['uri']}")
    return 1 if out["err"] else 0


if __name__ == "__main__":
    sys.exit(main())
