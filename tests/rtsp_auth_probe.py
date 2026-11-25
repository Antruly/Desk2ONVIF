#!/usr/bin/env python3
"""RTSP Digest 鉴权探针：无凭据 → 401，错口令 → 401，对口令 → 200 + SDP。

只用标准库（socket / hashlib），不依赖 ffmpeg —— 这条链要验的恰好是
**ffmpeg 会替你隐藏的那一层**：一个允许匿名 DESCRIBE 的服务端，ffplay 照样
能出画面，而"必须带凭据"这件事在整条链路上没有任何症状。

用法:
    python -I -X utf8 tests/rtsp_auth_probe.py --port 8554
    python -I -X utf8 tests/rtsp_auth_probe.py --port 8554 \
        --path /live --path /live_sub --user admin --pass admin

`--path` 可以给多次（主码流 + 子码流 / 多通道）。每个路径依次验三件事：

    1. 不带 Authorization 的 DESCRIBE       → 401 + WWW-Authenticate: Digest
    2. 用错误口令算出来的 Digest            → 仍是大 401
    3. 用正确口令（MD5 / qop=auth）算的 Digest → 200，SDP 含 m=video

任何一项失败都会让退出码非 0。每一项都打印 PASS / FAIL，失败时会带上
状态码和响应开头，方便在 CI 日志里一眼看出是哪一步。
"""

import argparse
import hashlib
import re
import secrets
import socket
import sys
import time

_TIMEOUT = 10.0


class Fail(Exception):
    pass


def read_response(sock):
    """读一条 RTSP 响应：头部到空行，正文按 Content-Length 收满。"""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise Fail("连接在响应头之前被关闭")
        buf += chunk
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.decode("iso-8859-1").split("\r\n")
    m = re.match(r"RTSP/\d\.\d\s+(\d{3})", lines[0])
    if not m:
        raise Fail(f"不是合法的 RTSP 状态行：{lines[0]!r}")
    code = int(m.group(1))
    headers = {}
    for line in lines[1:]:
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        headers.setdefault(k.strip().lower(), []).append(v.strip())
    try:
        clen = int(headers.get("content-length", ["0"])[-1])
    except ValueError:
        clen = 0
    body = rest
    while len(body) < clen:
        chunk = sock.recv(4096)
        if not chunk:
            break
        body += chunk
    return code, headers, body[:clen]


def header(headers, name):
    vals = headers.get(name.lower())
    return vals[-1] if vals else ""


def parse_challenge(value):
    """`Digest realm="x", nonce="y", qop="auth"` → dict。"""
    scheme, _, rest = value.partition(" ")
    if scheme.lower() != "digest":
        raise Fail(f"挑战不是 Digest：{value!r}")
    # 只认带引号的参数（Live555 的格式），同时兼容不带引号的 algorithm。
    params = dict(re.findall(r'(\w+)\s*=\s*"([^"]*)"', rest))
    for k, v in re.findall(r"(\w+)\s*=\s*([^,\s]+)", rest):
        params.setdefault(k, v)
    if not params.get("realm") or not params.get("nonce"):
        raise Fail(f"Digest 挑战缺 realm/nonce：{value!r}")
    return params


def md5_hex(s):
    return hashlib.md5(s.encode("utf-8")).hexdigest()


def digest_header(user, password, method, uri, challenge, cnonce=None):
    realm = challenge["realm"]
    nonce = challenge["nonce"]
    qop = (challenge.get("qop") or "").split(",")[0].strip()
    nc = "00000001"
    cnonce = cnonce or secrets.token_hex(8)
    ha1 = md5_hex(f"{user}:{realm}:{password}")
    ha2 = md5_hex(f"{method}:{uri}")
    if qop:
        resp = md5_hex(f"{ha1}:{nonce}:{nc}:{cnonce}:{qop}:{ha2}")
        return (
            f'Digest username="{user}", realm="{realm}", nonce="{nonce}", '
            f'uri="{uri}", response="{resp}", qop={qop}, nc={nc}, '
            f'cnonce="{cnonce}"'
        )
    resp = md5_hex(f"{ha1}:{nonce}:{ha2}")
    return (
        f'Digest username="{user}", realm="{realm}", nonce="{nonce}", '
        f'uri="{uri}", response="{resp}"'
    )


def describe(host, port, path, auth=None, timeout=_TIMEOUT):
    """发一条 DESCRIBE，返回 (status, headers, body)。每次新开连接。"""
    url = f"rtsp://{host}:{port}{path}"
    lines = [
        f"DESCRIBE {url} RTSP/1.0",
        "CSeq: 1",
        "User-Agent: d2o-rtsp-auth-probe",
        "Accept: application/sdp",
    ]
    if auth:
        lines.append(f"Authorization: {auth}")
    req = ("\r\n".join(lines) + "\r\n\r\n").encode("utf-8")
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(req)
        return read_response(sock)


class Report:
    def __init__(self):
        self.failed = 0

    def check(self, ok, what, detail=""):
        if ok:
            print(f"  [PASS] {what}")
        else:
            self.failed += 1
            print(f"  [FAIL] {what}")
            if detail:
                for line in str(detail).splitlines()[:6]:
                    print(f"         {line}")

    def note(self, text):
        print(f"         {text}")


def probe_path(host, port, path, user, password, r):
    print(f"\n--- {path} ---")
    base = f"DESCRIBE {path}"

    # 1) 匿名 → 401 + Digest 挑战
    try:
        st, hd, body = describe(host, port, path)
    except Exception as e:  # noqa: BLE001 —— 探针要把异常报成 FAIL，不是 traceback
        r.check(False, f"{base} 无凭据收到响应", e)
        return
    www = header(hd, "www-authenticate")
    r.check(st == 401, f"{base} 无凭据 → 401（实际 {st}）",
            f"响应开头：{body[:200]!r}")
    r.check(www.lower().startswith("digest"),
            f"{base} 401 带 WWW-Authenticate: Digest（实际 {www!r}）")
    challenge = None
    if www:
        try:
            challenge = parse_challenge(www)
            r.check(True,
                    f"{base} 挑战含 realm/nonce（realm={challenge['realm']!r}）")
        except Fail as e:
            r.check(False, f"{base} 解析 Digest 挑战", e)
    if challenge is None:
        return

    # 2) 错误口令 → 仍然 401
    bad = digest_header(user, password + "-definitely-wrong", "DESCRIBE",
                        f"rtsp://{host}:{port}{path}", challenge)
    try:
        st, hd, body = describe(host, port, path, auth=bad)
        r.check(st == 401, f"{base} 错误口令 → 401（实际 {st}）",
                f"响应开头：{body[:200]!r}")
    except Exception as e:  # noqa: BLE001
        r.check(False, f"{base} 错误口令收到响应", e)

    # 3) 正确口令 → 200 + m=video
    good = digest_header(user, password, "DESCRIBE",
                         f"rtsp://{host}:{port}{path}", challenge)
    try:
        st, hd, body = describe(host, port, path, auth=good)
    except Exception as e:  # noqa: BLE001
        r.check(False, f"{base} 正确口令收到响应", e)
        return
    text = body.decode("utf-8", "replace")
    r.check(st == 200, f"{base} 正确口令 → 200（实际 {st}）",
            f"响应开头：{body[:300]!r}")
    r.check("m=video" in text, f"{base} 的 SDP 含 m=video",
            text[:400])


def main():
    ap = argparse.ArgumentParser(
        description="RTSP Digest 鉴权探针（纯标准库）")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--path", action="append", default=None,
                    help="RTSP 路径，可给多次（默认 /live）")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--pass", dest="password", default="admin")
    ap.add_argument("--wait", type=float, default=0.0,
                    help="开始前等这么多秒（给采集把参数集吐出来）")
    a = ap.parse_args()

    paths = a.path or ["/live"]
    if a.wait > 0:
        time.sleep(a.wait)

    print(f"RTSP Digest 鉴权探针 → {a.host}:{a.port}")
    r = Report()
    for path in paths:
        if not path.startswith("/"):
            path = "/" + path
        probe_path(a.host, a.port, path, a.user, a.password, r)

    print()
    if r.failed:
        print(f"FAILED: {r.failed} 项")
        return 1
    print(f"PASSED: {len(paths)} 个路径 × 3 项")
    return 0


if __name__ == "__main__":
    sys.exit(main())
