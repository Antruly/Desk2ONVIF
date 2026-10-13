#!/usr/bin/env python3
"""验证 /ws/preview：确实是 fMP4 分片，而且拼起来真的能解码。

自己实现握手和帧解析，理由和 ws_probe.py 一样：现成的 WebSocket 库会在失败时
重试、会容忍协议上不合规的地方，而这条通道恰恰是"哪里不合规就整条流解不出来"
的那一类。

这一路真正会坏的地方只有三处，本探针逐个盯：

  1. **首帧必须是初始化段**（ftyp+moov）。少了它，或者晚了哪怕一块，
     MSE 会拒掉后面**所有**分片，而且报的错指不到真正的原因。
  2. **分片必须从 moof 开始。** 不是的话说明封装器把一片切成了两半。
  3. **中途连上来的客户端也要先拿到初始化段。** 前端是"打开页面才开始看"，
     不是"从进程启动就盯着"—— 而初始化段整个会话只产一次。

  4. **时间原点那一条控制帧**（`{"type":"t0","t0_wall_ms":…,"server_now_ms":…}`）。
     它是**文本**帧，和分片走的不是同一条路；前端靠它把播放位置换算成墙钟，
     才算得出「真实延迟」。少了它，页面上那个数就退化成缓冲余量（看着合理、
     其实差着一整个 GOP）—— 见 Phase W (a) 与陷阱 90 的第一面。

     ⚠️ **这一段必须先分辨文本帧再判二进制**：本探针原来是"见到文本帧就
     `break`"，那是按"这条通道只有二进制"写的。加了 t0 之后，那个 `break`
     会在连上后立刻退出接收循环 —— 症状是"分片 0 块"，看上去像封装器坏了，
     而真正的原因在这里。所以现在文本帧**解析并继续**，认不出来的才报红。

Web 控制台上了登录之后，这个脚本会**先登录再连** —— `/ws/preview` 的守卫写在
WS 处理器内部（升级请求绕过中间件链），没带有效 cookie 的升级会被
`close(1008, "unauthenticated")` 拒掉，症状是"连上了但一个二进制帧都没有"。

用法:
    python -I -X utf8 tests/preview_probe.py [--port 8080] [--seconds 8]
                                             [--out out/preview.mp4]
                                             [--user admin] [--password admin]
                                             [--cookie 'd2o_session=...']

-I 是刻意的：隔离模式，不把脚本目录和当前目录塞进 sys.path。
-X utf8：`-I` 隐含 `-E`，PYTHONIOENCODING 在它下面不生效，重定向到文件时
中文会按 GBK 编码炸掉。
"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import socket
import struct
import subprocess
import sys
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
COOKIE = "d2o_session"
FFPROBE = os.path.join("third_party", "ffmpeg", "bin", "ffprobe.exe")

_failed = 0


def check(ok, what, detail=""):
    global _failed
    print(f"  [{'OK' if ok else '!!'}] {what}")
    if detail:
        for line in str(detail).splitlines():
            print(f"        {line}")
    if not ok:
        _failed += 1
    return ok


# --------------------------------------------------------------- 登录那一段
def _api(host, port, method, path, obj=None, cookie=None):
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


def login_cookie(host, port, user, password, given=None):
    """拿到这次要用的 cookie。返回 None 表示服务端没开登录。

    先问 /api/session（匿名可达）：`auth_required` 是 false 就别登录了 ——
    对着一个 --no-http-auth 起的服务做登录，失败信息会把真正的原因盖住。
    """
    if given:
        print(f"[auth] 用命令行给的 cookie")
        return given
    try:
        st, _, b = _api(host, port, "GET", "/api/session")
        sess = json.loads(b) if st == 200 else None
    except (OSError, ValueError):
        return None
    if not sess or not sess.get("auth_required"):
        print("[auth] 服务端没开登录（auth_required 不是 true），不带 cookie")
        return None
    st, hd, b = _api(host, port, "POST", "/api/login",
                     {"user": user, "password": password})
    if st != 200:
        raise RuntimeError(f"登录失败（HTTP {st}）：{b[:200]!r} —— "
                           f"默认 admin/admin，用 --user/--password 覆盖")
    for v in hd.get("set-cookie", []):
        if v.startswith(COOKIE + "="):
            print(f"[auth] 已登录：{user}，拿到 {COOKIE} cookie")
            return v.split(";", 1)[0]
    raise RuntimeError("登录返回 200 但没有 d2o_session cookie")


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
    if cookie:
        lines.append(f"Cookie: {cookie}")
    sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("握手中对端就断了")
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    if "101" not in lines[0]:
        raise RuntimeError(f"升级失败：{lines[0]}\n{head.decode('latin-1')}")
    expect = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    got = None
    for ln in lines[1:]:
        if ln.lower().startswith("sec-websocket-accept:"):
            got = ln.split(":", 1)[1].strip()
    if got != expect:
        raise RuntimeError(f"Sec-WebSocket-Accept 不对：期望 {expect}，实得 {got}")
    return rest


def read_exact(sock, n, pre):
    if pre:
        take = min(n, len(pre))
        out, pre = pre[:take], pre[take:]
    else:
        out = b""
    while len(out) < n:
        chunk = sock.recv(min(1 << 20, n - len(out)))
        if not chunk:
            raise RuntimeError("连接在帧中间断了")
        out += chunk
    return out, pre


def read_frame(sock, pre):
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
    """Close 帧的 (关闭码, 原因)。**不带 cookie 时它会是 (1008, 'unauthenticated')**
    —— 不读这个码的话，那种情况在这里长得就像"服务端一句话不说把连接关了"，
    看起来像封装器坏了。"""
    if len(payload) < 2:
        return None, ""
    return struct.unpack("!H", payload[:2])[0], payload[2:].decode("utf-8", "replace")


def top_level_boxes(data):
    """把一段 fMP4 的顶层 box 类型列出来。长度字段坏了就会越界，直接抛。"""
    out = []
    i = 0
    while i + 8 <= len(data):
        size = struct.unpack("!I", data[i : i + 4])[0]
        kind = data[i + 4 : i + 8].decode("latin-1")
        out.append(kind)
        if size < 8:
            raise RuntimeError(f"box {kind!r} 的长度字段是 {size}，不合法")
        i += size
    if i != len(data):
        raise RuntimeError(f"顶层 box 加起来是 {i} 字节，实到 {len(data)}")
    return out


def collect(host, port, path, seconds, label, want=1 << 30, cookie=None):
    """收 seconds 秒，返回 (init 段, [分片...], 实际耗时, [文本帧...])。

    文本帧**不中断接收**（见文件头第 4 条）：收下来给调用方判，循环继续。
    """
    sock = socket.create_connection((host, port), timeout=10)
    sock.settimeout(seconds + 10)
    pre = handshake(sock, host, port, path, cookie=cookie)
    print(f"[{label}] 升级成功，接收 {seconds}s")

    init = None
    frags = []
    texts = []
    total = 0
    t0 = time.time()
    try:
        while time.time() - t0 < seconds:
            op, payload, pre = read_frame(sock, pre)
            if op == 0x8:
                code, reason = close_info(payload)
                print(f"[{label}] 对端发了 Close 帧：码={code} 原因={reason!r}"
                      + ("  ← 未登录/会话过期就是这个样子" if code == 1008 else ""))
                break
            if op == 0x9:
                sock.sendall(bytes([0x8A, len(payload)]) + payload)
                continue
            if op == 0x2:
                if init is None:
                    init = payload
                    print(f"[{label}] 首帧 {len(payload)} 字节，顶层 box："
                          f"{' '.join(top_level_boxes(payload))}")
                else:
                    frags.append(payload)
                    total += len(payload)
                    if total >= want:
                        break
            elif op == 0x1:
                texts.append((time.time(), payload))
                print(f"[{label}] 控制帧：{payload[:160]!r}")
    finally:
        sock.close()

    span = time.time() - t0
    print(f"[{label}] {span:.1f}s 内：init {'1' if init else '0'} 块，"
          f"分片 {len(frags)} 块，合计 {total} 字节，控制帧 {len(texts)} 条")
    return init, frags, span, texts


def t0_of(texts):
    """从收到的文本帧里挑出 t0 那条。返回 (字典, 收到时刻) 或 (None, None)。

    **认不出来的文本帧不算 t0 也不算通过** —— 调用方会把"有文本帧但没有
    t0"和"一条文本帧都没有"分开报，因为它们是两种不同的坏法。
    """
    for when, payload in texts:
        try:
            m = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            continue
        if isinstance(m, dict) and m.get("type") == "t0":
            return m, when
    return None, None


def ffprobe(path):
    if not os.path.exists(FFPROBE):
        return None
    p = subprocess.run(
        [FFPROBE, "-v", "error", "-show_entries",
         "stream=codec_name,width,height,nb_read_frames,avg_frame_rate",
         "-count_frames", "-of", "default=nw=1", path],
        capture_output=True, text=True,
    )
    return p.stdout.strip()


def decode_errors(path):
    """整段解码一遍，返回 [error] 级别的输出行。"""
    ffmpeg = FFPROBE.replace("ffprobe.exe", "ffmpeg.exe")
    if not os.path.exists(ffmpeg):
        return None
    p = subprocess.run(
        [ffmpeg, "-v", "error", "-i", path, "-f", "null", "-"],
        capture_output=True, text=True,
    )
    return [ln for ln in p.stderr.splitlines() if ln.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--late-after", type=float, default=3.0,
                    help="第二个客户端在第一个连上之后多久接入")
    ap.add_argument("--out", default=os.path.join("out", "preview.mp4"))
    ap.add_argument("--user", default="admin", help="Web 控制台用户名")
    ap.add_argument("--password", default="admin", help="Web 控制台口令")
    ap.add_argument("--cookie", default=None,
                    help="直接给 cookie（如 'd2o_session=...'），给了就不再登录")
    a = ap.parse_args()

    # **连接之前先登录。** /ws/preview 的守卫在处理器内部，没 cookie 的升级
    # 会拿到 101 + close(1008)，症状是"连上了、一个帧都没有"。
    cookie = login_cookie(a.host, a.port, a.user, a.password, a.cookie)

    print("=== 1. 第一个客户端：首帧是不是初始化段 ===")
    init, frags, span, texts = collect(a.host, a.port, "/ws/preview", a.seconds, "1",
                                       cookie=cookie)
    if not check(init is not None, "连上后收到了初始化段"):
        if cookie is None:
            print("\n没收到任何二进制帧。服务端是开着登录的吗？那这里必须有 cookie。")
        else:
            print("\n没收到任何二进制帧，后面的检查没有意义。")
        return 1
    try:
        boxes = top_level_boxes(init)
    except RuntimeError as e:
        check(False, "初始化段的 box 结构完整", e)
        return 1
    check(boxes == ["ftyp", "moov"], "初始化段恰好是 ftyp + moov", f"实得 {boxes}")

    print("\n=== 1b. 时间原点那一帧（前端算「真实延迟」的依据）===")
    # 这一段的判据是**墙钟**，不是媒体时间：`t0_wall_ms` 与 `server_now_ms`
    # 都必须是 Unix 毫秒，且两者之差就是"这个会话已经跑了多久"。
    # 换成媒体时间（0~几秒）或者计数器的值，下面第 2、4 条会当场红。
    t0_msg, t0_when = t0_of(texts)
    if not check(t0_msg is not None,
                 "收到了 t0 控制帧",
                 "" if t0_msg is not None else
                 (f"这一路收到 {len(texts)} 条文本帧，没有一条是 t0"
                  if texts else "一条文本帧都没收到")):
        print("        （前端拿不到时间原点时，「真实延迟」会显示 — 而不是一个错数）")
    else:
        print(f"       {t0_msg}")
        raw_t0 = t0_msg.get("t0_wall_ms")
        raw_now = t0_msg.get("server_now_ms")
        shapes_ok = (isinstance(raw_t0, int) and not isinstance(raw_t0, bool)
                     and isinstance(raw_now, int) and not isinstance(raw_now, bool))
        check(shapes_ok, "两个字段都是整数（不是字符串、不是浮点、不是布尔）",
              f"实得 {type(raw_t0).__name__} / {type(raw_now).__name__}")

        if shapes_ok:
            t0v, nowv = raw_t0, raw_now
            # ⚠️ 基准必须是**收到这一帧的那一刻**（`t0_when`），不是"跑检查的
            # 那一刻" —— 后者在 `collect()` 收满 8 秒之后，于是这一条会凭空
            # 多出一个等于接收时长的偏差（实测 8871 ms，看起来像"两边的钟
            # 不一样"，其实是尺子量错了地方）。
            local_ms = int(t0_when * 1000)
            check(t0v > 0 and nowv > 0, "两个数都是正数", f"t0={t0v} now={nowv}")
            # 1.6e12 = 2020-09-13。比这小就说明它不是 Unix 毫秒。
            check(t0v > 1_600_000_000_000,
                  "t0_wall_ms 是 Unix 毫秒量级（2020 年以后）",
                  f"实得 {t0v} —— 媒体时间是 0 到几秒，那个量级会落在这里")
            # 同一台机器、走回环，服务端的钟和本机的钟必须对得上。
            # 这一条同时证明了它**不是**媒体时间、也不是进程内的 steady 计数。
            skew = abs(local_ms - nowv)
            check(skew < 1000,
                  f"server_now_ms 与本机时钟一致（差 {skew} ms）",
                  f"收到帧时本机 {local_ms} vs 服务端的 {nowv}")
            age = nowv - t0v
            check(0 <= age <= 3_600_000,
                  f"t0_wall_ms 在 server_now_ms 之前、一小时之内（差 {age} ms）",
                  f"t0={t0v} now={nowv} —— 负数说明 0 点在未来，几十万说明量纲错了")

    print("\n=== 2. 分片是不是都从 moof 开始 ===")
    check(len(frags) > 0, f"收到了分片（{len(frags)} 块）")
    bad = [i for i, f in enumerate(frags) if f[4:8] != b"moof"]
    check(not bad, "每块分片都是 moof 开头的完整分片",
          f"第 {bad[:5]} 块不是" if bad else "")
    # 每块还得是 moof+mdat 两段，正好两段。
    shapes = [tuple(top_level_boxes(f)) for f in frags]
    check(all(s == ("moof", "mdat") for s in shapes),
          "每块分片恰好是 moof + mdat", f"实得 {shapes[:4]}")

    print("\n=== 3. init + 分片拼起来能不能解码 ===")
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "wb") as fh:
        fh.write(init)
        for f in frags:
            fh.write(f)
    print(f"       写出 {a.out}（{os.path.getsize(a.out)} 字节）")
    info = ffprobe(a.out)
    if info is None:
        print("       找不到 ffprobe，跳过解码检查")
    else:
        for line in info.splitlines():
            print(f"       {line}")
        nfr = None
        for line in info.splitlines():
            if line.startswith("nb_read_frames="):
                nfr = int(line.split("=", 1)[1])
        check("codec_name=h264" in info, "解出来是 H.264")
        check(nfr is not None and nfr > 0, f"解出了帧（{nfr} 帧）")
        errs = decode_errors(a.out)
        check(errs == [], "整段解码零错误输出",
              "\n".join(errs[:6]) if errs else "")

    print("\n=== 4. 中途连上来的客户端也要先拿到初始化段 ===")
    time.sleep(a.late_after)
    init2, frags2, _, texts2 = collect(a.host, a.port, "/ws/preview",
                                       max(a.seconds - 2, 4), "2", want=1 << 20,
                                       cookie=cookie)
    check(init2 is not None, "迟到的那一个也收到了初始化段")
    # 时间原点是**每会话一份**的：后连上来的客户端要拿到**同一个** 0 点。
    # 这条同时钉住"留一份给后来的人"那半边（只广播不留存，这里就是 None）。
    t0_msg2, _ = t0_of(texts2)
    check(t0_msg2 is not None, "迟到的那一个也收到了 t0 控制帧")
    if t0_msg2 is not None and t0_msg is not None:
        check(t0_msg2.get("t0_wall_ms") == t0_msg.get("t0_wall_ms"),
              "两份 t0 是同一个 0 点（一个会话只有一个时间原点）",
              f"{t0_msg.get('t0_wall_ms')} vs {t0_msg2.get('t0_wall_ms')}"
              if t0_msg2.get("t0_wall_ms") != t0_msg.get("t0_wall_ms") else "")
    if init2 is not None:
        check(init2 == init, "两份初始化段逐字节相同",
              f"{len(init2)} vs {len(init)} 字节" if init2 != init else "")
        check(len(frags2) > 0, f"迟到的那一个也收到了分片（{len(frags2)} 块）")
        try:
            boxes2 = top_level_boxes(init2)
            check(boxes2 == ["ftyp", "moov"], "它的初始化段结构正确", f"{boxes2}")
        except RuntimeError as e:
            check(False, "它的初始化段结构正确", e)

    print("\n=== 结论 ===")
    if _failed == 0:
        print("  全部通过。")
        return 0
    print(f"  {_failed} 项失败。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
