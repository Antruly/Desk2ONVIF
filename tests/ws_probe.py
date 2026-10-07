#!/usr/bin/env python3
"""最小的 WebSocket 客户端，用来确认 /ws/stats 真的在推。

自己实现握手和帧解析而不是装 websockets 包，是因为后者会**掩盖**协议层的
问题：它会在失败时重试、会容忍我们没有实现的东西。这里只要有两处不合规
就立刻炸给你看。

Web 控制台上了登录之后，这个脚本自己带了登录那一段。**这里同时补上了一块
web_auth.py 覆盖不到的盲区**：`/ws/*` 不经过中间件链，两条 WS 路由的守卫写在
处理器内部，所以"没登录的人连 WS 会怎样"只能在这里验 —— 参见 `anon_reject()`。

用法:
    python -I -X utf8 tests/ws_probe.py [--port 8080] [--path /ws/stats]
                                        [--seconds 4] [--clients 2]
                                        [--user admin] [--password admin]
                                        [--cookie 'd2o_session=...']
                                        [--skip-anon-check]

-I 是刻意的：隔离模式，不把脚本目录和当前目录塞进 sys.path。
-X utf8 不是多余的：`-I` 隐含 `-E`，PYTHONIOENCODING 在它下面不生效，
重定向到文件时中文会按 GBK 编码炸掉。
"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import socket
import struct
import sys
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
COOKIE = "d2o_session"


# --------------------------------------------------------------- HTTP 小工具
def api(host, port, method, path, obj=None, cookie=None):
    """发一个普通 HTTP 请求，返回 (状态码, {小写头: [值...]}, body 字节)。"""
    body = None
    hd = {}
    if obj is not None:
        body = json.dumps(obj).encode("utf-8")
        hd["Content-Type"] = "application/json"
        hd["Content-Length"] = str(len(body))
    if cookie:
        hd["Cookie"] = cookie
    c = http.client.HTTPConnection(host, port, timeout=10)
    c.request(method, path, body=body, headers=hd)
    r = c.getresponse()
    data = r.read()
    out = {}
    for k, v in r.getheaders():
        out.setdefault(k.lower(), []).append(v)
    c.close()
    return r.status, out, data


def session(host, port):
    """问一次 /api/session（匿名可达）。问不到就返回 None。"""
    try:
        st, _, b = api(host, port, "GET", "/api/session")
        if st != 200:
            return None
        return json.loads(b)
    except (OSError, ValueError):
        return None


def login(host, port, user, password):
    """POST /api/login，返回 (cookie 或 None, 状态码, body)。"""
    st, hd, b = api(host, port, "POST", "/api/login",
                    {"user": user, "password": password})
    if st != 200:
        return None, st, b
    for v in hd.get("set-cookie", []):
        if v.startswith(COOKIE + "="):
            return v.split(";", 1)[0], st, b
    return None, st, b


def resolve_cookie(a, label):
    """把这次要用的 cookie 定下来。

    返回 cookie 字符串（服务端开着登录）或 None（没开登录，不需要带）。
    登录失败直接抛 —— 那种情况下后面每个断言都会是假失败，不如当场说清楚。
    """
    if a.cookie:
        print(f"[{label}] 用命令行给的 cookie")
        return a.cookie
    s = session(a.host, a.port)
    if s is None:
        print(f"[{label}] /api/session 问不到（后端没起来？），先按未开登录继续")
        return None
    if not s.get("auth_required"):
        print(f"[{label}] 服务端没开登录（auth_required=false），不需要 cookie")
        return None
    ck, st, body = login(a.host, a.port, a.user, a.password)
    if ck is None:
        raise RuntimeError(
            f"登录失败（HTTP {st}）：{body[:200]!r}\n"
            f"  用户名/口令对不对？默认 admin/admin，命令行用 --user/--password 覆盖。")
    print(f"[{label}] 已登录：user={a.user}，拿到 {COOKIE} cookie")
    return ck


# ------------------------------------------------------------------ 握手 / 帧
def handshake(sock, host, port, path, cookie=None):
    key = base64.b64encode(os.urandom(16)).decode()
    lines = [
        f"GET {path} HTTP/1.1",
        f"Host: {host}:{port}",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Key: {key}",
        "Sec-WebSocket-Version: 13",
    ]
    # Cookie 是可选的：不用它的时候，报文的形状和加登录之前**逐字节相同**，
    # 失败时才不会怀疑"是不是多出来的那个头把服务端搞懵了"。
    if cookie:
        lines.append(f"Cookie: {cookie}")
    req = ("\r\n".join(lines) + "\r\n\r\n").encode()
    sock.sendall(req)

    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("握手中对端就断了")
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    status = lines[0]
    if "101" not in status:
        raise RuntimeError(f"升级失败：{status}\n{head.decode('latin-1')}")

    expect = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    got = None
    for ln in lines[1:]:
        if ln.lower().startswith("sec-websocket-accept:"):
            got = ln.split(":", 1)[1].strip()
    if got != expect:
        raise RuntimeError(f"Sec-WebSocket-Accept 不对：期望 {expect}，实得 {got}")
    return rest


def read_exact(sock, n, pre):
    """先从 pre 里拿，不够再 recv。pre 是握手时多读出来的字节。"""
    out = b""
    if pre:
        take = min(n, len(pre))
        out, pre = pre[:take], pre[take:]
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise RuntimeError("连接在帧中间断了")
        out += chunk
    return out, pre


def read_frame(sock, pre):
    """返回 (opcode, payload, pre)。只处理服务端发来的（不掩码）。"""
    hdr, pre = read_exact(sock, 2, pre)
    fin = hdr[0] & 0x80
    opcode = hdr[0] & 0x0F
    masked = hdr[1] & 0x80
    ln = hdr[1] & 0x7F
    if ln == 126:
        ext, pre = read_exact(sock, 2, pre)
        ln = struct.unpack("!H", ext)[0]
    elif ln == 127:
        ext, pre = read_exact(sock, 8, pre)
        ln = struct.unpack("!Q", ext)[0]
    if masked:
        raise RuntimeError("服务端不该给帧加掩码（RFC 6455 §5.1）")
    payload, pre = read_exact(sock, ln, pre)
    if not fin:
        raise RuntimeError("收到分片帧 —— 本探针没实现重组")
    return opcode, payload, pre


def close_info(payload):
    """从 Close 帧的载荷里取 (关闭码, 原因)。没有码时返回 (None, '')。

    RFC 6455 §5.5.1：前两字节是 big-endian 的关闭码，后面是 UTF-8 原因。
    **不读它就没法区分"服务端礼貌地拒绝了我们"和"连接自己断了"** —— 两者
    在本探针里长得一模一样（都是先收到一个 0x8）。
    """
    if len(payload) < 2:
        return None, ""
    code = struct.unpack("!H", payload[:2])[0]
    return code, payload[2:].decode("utf-8", "replace")


# ------------------------------------------------------------- 匿名必须被拒
def anon_reject(host, port, path, label="anon"):
    """**不带 cookie** 连一条 WS 路由，必须被 1008（POLICY_VIOLATION）拒掉。

    这是本文件里最要紧的一条断言，也是 web_auth.py 刻意不覆盖的一块：
    那个脚本只走 HTTP。`/ws/*` 在实现上**绕过中间件链**（WS 升级走
    `on_ws_upgrade`，那条链整个不参与），所以守卫是写在每个 WS 处理器内部的。
    中间件写对了、WS 忘了写，检查 HTTP 的脚本全绿，而任意一台能连到这台机器
    的人可以裸连 `/ws/preview` 把桌面画面拉走 —— 一点痕迹都没有。

    返回 (是否通过, 说明)。
    """
    sock = None
    try:
        sock = socket.create_connection((host, port), timeout=10)
        sock.settimeout(8)
        try:
            pre = handshake(sock, host, port, path, cookie=None)
        except RuntimeError as e:
            # 有的实现（以及将来可能改成的实现）会在升级阶段就回 401/403。
            # 那同样是"拒了"，一样算过。
            return True, f"升级阶段就被拒：{str(e).splitlines()[0]}"
        except (OSError, socket.timeout) as e:
            return True, f"连接被直接断掉：{e}"

        try:
            op, payload, _ = read_frame(sock, pre)
        except RuntimeError as e:
            return True, f"升级后立刻断流：{e}"
        if op != 0x8:
            return False, (f"**没有**被拒：收到 opcode={op} 的 "
                           f"{len(payload)} 字节载荷（应当是一个 Close 帧）")
        code, reason = close_info(payload)
        if code != 1008:
            return False, (f"**关闭码不对**：{code}（{reason!r}），"
                           f"期望 1008 POLICY_VIOLATION")
        return True, f"1008 {reason!r}"
    except OSError as e:
        return False, f"连不上：{e}"
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass


# -------------------------------------------------------------------- 主流程
def follow(host, port, path, seconds, label, cookie=None):
    sock = socket.create_connection((host, port), timeout=10)
    sock.settimeout(seconds + 10)
    pre = handshake(sock, host, port, path, cookie=cookie)
    print(f"[{label}] 升级成功，开始接收 {seconds}s")

    n = 0
    binary = 0
    t0 = time.time()
    last = None
    first_at = None
    last_at = None
    closed = None
    try:
        while time.time() - t0 < seconds:
            op, payload, pre = read_frame(sock, pre)
            if op == 0x8:
                closed = close_info(payload)
                print(f"[{label}] 对端发了 Close 帧：码={closed[0]} 原因={closed[1]!r}")
                break
            if op == 0x9:  # ping -> 必须回 pong
                sock.sendall(bytes([0x8A, len(payload)]) + payload)
                print(f"[{label}] 收到 ping，已回 pong")
                continue
            if op == 0xA:
                continue
            if op != 0x1:
                if op == 0x2:
                    binary += 1
                print(f"[{label}] 非文本帧 opcode={op}，{len(payload)} 字节")
                continue
            n += 1
            d = json.loads(payload)
            last = d
            now = time.time()
            if first_at is None:
                first_at = now
            last_at = now
            print(
                f"[{label}] #{n} elapsed={d.get('elapsed_s', 0):5.1f}s "
                f"packets={d.get('packets')} fps={d.get('effective_fps', 0):5.1f} "
                f"kbps={d.get('bytes', 0) * 8 / max(d.get('elapsed_s', 1), 1) / 1000:6.0f} "
                f"ws_clients={d.get('ws_clients', '?')}"
            )
    finally:
        sock.close()

    if closed is not None:
        print(f"[{label}] 被服务端关掉了（码 {closed[0]}）—— 带着 cookie 还被关，"
              f"多半是 cookie 无效或已过期")
        return False
    if n == 0:
        if binary:
            # `--path /ws/preview` 是拿错了工具：那条路推的是 fMP4 二进制帧，
            # 这个探针只认 /ws/stats 的 JSON 文本。不说清楚的话，这里报的是
            # "推送没生效"，看起来像后端坏了。
            print(f"[{label}] 收到 {binary} 个**二进制**帧 —— 这条路径不是 JSON 文本"
                  f"通道（/ws/preview 推的是 fMP4）")
            print(f"[{label}] 要验预览通道请用 preview_probe.py；本探针只验 /ws/stats")
            return False
        print(f"[{label}] 一条消息都没收到 —— 推送没生效")
        return False

    # 间隔要按**首尾之差**算，不能拿 seconds/n —— 第一条是连上时立刻补发的
    # 那一份快照，把它算进平均里会得出一个比真实节奏快 20% 的数。
    span = (last_at - first_at) if n > 1 else 0.0
    if n > 1:
        print(f"[{label}] 共 {n} 条，推送间隔 {(span / (n - 1)):.2f}s（期望 1.00s）")
    else:
        print(f"[{label}] 只收到 1 条 —— 只有连上时的补发，周期推送没生效")
        return False

    if last.get("ws_clients") != 1:
        print(f"[{label}] ws_clients={last.get('ws_clients')!r}，单客户端时应该是 1")
        return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--path", default="/ws/stats")
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("--clients", type=int, default=1)
    ap.add_argument("--user", default="admin", help="Web 控制台用户名")
    ap.add_argument("--password", default="admin", help="Web 控制台口令")
    ap.add_argument("--cookie", default=None,
                    help="直接给 cookie（如 'd2o_session=...'）；给了就不再登录")
    ap.add_argument("--skip-anon-check", action="store_true",
                    help="跳过『不带 cookie 必须被 1008 拒掉』那一段")
    a = ap.parse_args()

    cookie = resolve_cookie(a, "准备")

    ok = True

    # ---- 0. 守卫：没带 cookie 的升级必须被拒 ---------------------------
    #
    # 无论后面跑哪条路径，这一段都要跑。`--path` 之外**固定**再验一次
    # /ws/preview：它是两条 WS 里更敏感的那条（直接推桌面画面的 fMP4），
    # 而"守卫只写了 /ws/stats 那条"正是最可能漏的样子。
    if not a.skip_anon_check:
        s = session(a.host, a.port)
        if s is None or not s.get("auth_required"):
            print("\n[0] 匿名守卫：服务端没开登录，跳过（auth_required 不是 true）")
        else:
            print("\n[0] 匿名守卫：不带 cookie 的 WS 升级必须被 1008 拒掉")
            targets = ["/ws/preview"]
            if a.path not in targets:
                targets.append(a.path)
            for path in targets:
                good, detail = anon_reject(a.host, a.port, path)
                print(f"  [{'OK' if good else '!!'}] 匿名连 {path} → {detail}")
                ok = good and ok
            # 反面：带着刚登录拿到的 cookie 就应该能连上（否则上面那条"通过"
            # 可能是无差别的拒绝，也就是所有连接都被拒 —— 那同样是个 bug）。
            if cookie:
                try:
                    ok = anon_reject_control(a.host, a.port, a.path, cookie) and ok
                except RuntimeError as e:
                    print(f"  [!!] 带 cookie 的对照连接失败：{e}")
                    ok = False
            else:
                print("  [note] 没有 cookie（登录没开），跳过带 cookie 的对照")

    # ---- 1. 正常推送 ----------------------------------------------------
    print(f"\n[1] 带会话连 {a.path}，确认它真的在推")
    if a.clients == 1:
        ok = follow(a.host, a.port, a.path, a.seconds, "1", cookie) and ok
    else:
        # 多客户端：验证 ws_clients 计数和"每人都收到"。串行跑，因为
        # 这个探针是阻塞式的；串行的关键损失是看不到并发的连接数，
        # 但本轮要看的是表有没有被正确维护。
        for i in range(a.clients):
            ok = follow(a.host, a.port, a.path, a.seconds, str(i + 1), cookie) and ok
    return 0 if ok else 1


def anon_reject_control(host, port, path, cookie):
    """对照：带 cookie 的升级应当**成功**（101）而不是被关掉。

    没有这一条的话，`anon_reject` 的"通过"可能来自"服务端把所有连接都拒了"
    —— 那是另一个 bug，而它的症状和"守卫做对了"完全一样。
    """
    sock = socket.create_connection((host, port), timeout=10)
    sock.settimeout(8)
    try:
        pre = handshake(sock, host, port, path, cookie=cookie)
        print(f"  [OK] 带 cookie 连 {path} → 101 升级成功（不是无差别拒绝）")
        return True
    except (RuntimeError, OSError) as e:
        print(f"  [!!] 带 cookie 连 {path} 也被拒了：{e}")
        return False
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
