#!/usr/bin/env python3
"""假 SNTP 服务器 —— 用来把「对时」这条路上的四种结局**分别造出来**。

为什么非要有它：对时是一个**最容易假装成功**的功能。UDP 发到公网 123 的包在
没有网的环境里会被防火墙**静默丢弃**（UDP 没有 RST），于是「包发出去了」
和「服务器理我了」在客户端看来一模一样。而且真去问公网服务器的话，那条路
在网上通不通、服务器认不认我们，全是环境决定的 —— 今天绿明天红，红的时候
还指不出是哪一方的错。

所以这里自己造一台服务器：**它的时间轴是我们说了算的**。回包里的时间戳按
`本机时钟 + --offset` 填，于是「对出来的偏移」有一个**精确的期望值**，
而不是「差不多对」。这一条比什么都值钱：NTP 纪元常数（1900-01-01，差
`2208988800` 秒）要是写错了，量出来的偏移会是 86400 的整数倍偏差，而
`offset_s == N` 这个断言会当场抓住它；换成「偏移是个正数」这种断言，
两个方向都会放过。

四种结局分别是：

    --serve                 正常应答            → 客户端报 ok
    --serve --silent        收下但不回          → 客户端报 timeout
    --serve --stratum 0     KoD（拒绝服务）     → 客户端报 bad_reply
    --serve --reply-bytes 20  回包太短          → 客户端报 bad_reply
    --serve --zero-timestamps 时间戳全零        → 客户端报 bad_reply

`unreachable` 那一种不需要服务器（地址压根解析不出来），所以不在这里。

⚠️ **`timeout` 不能靠「不起服务器」来造。** 本机回环上没人监听时，Windows
会给一个 ICMP 端口不可达，而**连过（`connect`）的 UDP 套接字会把它报成
`WSAECONNRESET`** —— `ntp_client.cpp` 把那个归到 `unreachable`，不是 timeout。
也就是说「不起探针 → timeout」这条推断在本机是**错的**。要确定性地造出
timeout，只能是「收到了、但不回」：包确实送达了，`SO_RCVTIMEO` 到点，
`recv` 返回 `WSAETIMEDOUT`。`--silent` 就是这个。

用法：

    # 起服务器（Windows 上绑 123 端口**不需要管理员**）
    python -I -X utf8 tests/ntp_probe.py --serve --host 127.0.0.1 --offset 300 \
        --ready out/ntp-ready.txt --requests 1

    # 自检（先量一遍量具 —— 陷阱 82）
    python -I -X utf8 tests/ntp_probe.py --check

-I：隔离模式，不把脚本目录和当前目录塞进 sys.path。
-X utf8：`-I` 隐含 `-E`，PYTHONIOENCODING 在它下面不生效，重定向到文件时
中文会按 GBK 编码炸掉。
"""

import argparse
import socket
import struct
import sys
import time

# NTP 纪元（1900-01-01）到 Unix 纪元（1970-01-01）之间的秒数。
#
# 这个数在本文件里**只出现这一次**，而且它只用于「把本机时钟写成一个 NTP
# 时间戳」。被测的那个常数在 C++ 侧（`kNtpUnixEpochDelta`），两边**不是同一份
# 代码**，所以 `offset_s == N` 这个断言真的能验到它 —— 如果两边共用一份实现，
# 那这条断言就只是在验「同一个错被犯了两次」。
EPOCH_DELTA = 2208988800

NTP_PORT = 123


def to_ntp(unix_s):
    """把「自 Unix 纪元起的秒数」写成 64 位定标数（高 32 位秒、低 32 位小数）。"""
    v = unix_s + EPOCH_DELTA
    sec = int(v)
    frac = int((v - sec) * 4294967296.0)
    # 浮点边界上 frac 可能恰好取到 2**32（v 的小数部分极接近 1 时），
    # 那时应该进位而不是写出一个溢出的 u32。
    if frac >= 4294967296:
        sec += 1
        frac = 0
    return struct.pack(">II", sec, frac)


def from_ntp(raw):
    """读回一个 NTP 时间戳，换算成「自 Unix 纪元起的秒数」。"""
    sec, frac = struct.unpack(">II", raw)
    return sec - EPOCH_DELTA + frac / 4294967296.0


def build_reply(req, args, now):
    """按 `args` 造一个 48 字节的回包。

    `now` 是**服务器自己的**时钟读数（自 Unix 纪元起的秒数）。真实服务器就是
    这么干的：它按**自己的**钟填 T2/T3，客户端拿它和本机钟一比才知道差多少。
    这里再叠一个 `--offset`，于是「差多少」变成我们指定的那个数。
    """
    server_now = now + args.offset
    rep = bytearray(48)
    # LI=0（无闰秒警告）、VN=4、Mode=4（server）→ 高三位 000、中三位 100、
    # 低三位 100 = 0x24。客户端不检查这一格，但写成对的免得下次有人拿真
    # 服务器来比对时报一个假失败。
    rep[0] = 0x24
    rep[1] = args.stratum
    rep[2] = 4     # poll
    rep[3] = 0xEC  # precision = -20
    # 24..31 是 Originate：把客户端的 Transmit 原样抄回来。这是这条链上唯一
    # 的往返一致性凭据（真客户端会拿它核对「这个回包是不是我这次问的」）。
    rep[24:32] = req[40:48]
    if not args.zero_timestamps:
        stamp = to_ntp(server_now)
        rep[32:40] = stamp  # T2：服务器收到的时刻
        rep[40:48] = stamp  # T3：服务器发出的时刻
    if args.stratum == 0:
        # KoD（kiss-o'-death）：Stratum=0，Reference ID 里放 4 个 ASCII 的
        # 「别再来问了」。客户端只看 Stratum 就够判了，这里填上是为了拿真
        # 服务器抓包比对时长得像。
        rep[12:16] = b"RATE"
    return bytes(rep)


def serve(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((args.host, args.port))
    print(f"[ntp-probe] 已绑定 {args.host}:{args.port} "
          f"offset={args.offset:+d}s stratum={args.stratum} "
          f"mode={'silent' if args.silent else 'normal'}", flush=True)
    if args.ready:
        # 握手：调用方等这个文件出现再开始打接口。不这么做的话，「服务器还没
        # 绑上、客户端先问了一次」会表现成一条 timeout —— 那是夹具的竞态，
        # 却长得像产品缺陷。
        with open(args.ready, "w", encoding="utf-8") as f:
            f.write(f"{args.host}:{args.port}\n")
    served = 0
    while args.requests <= 0 or served < args.requests:
        try:
            data, peer = s.recvfrom(1024)
        except OSError as e:            # 被 Ctrl+C / taskkill 打断
            print(f"[ntp-probe] recvfrom 失败：{e}", flush=True)
            break
        served += 1
        # 每一包都打一行：调用方靠它分辨「客户端根本没问」和「问了但我们没回」
        # —— 这两种在客户端的结局上长得一模一样（一个是 unreachable，一个是
        # timeout），而它们要修的地方完全不同。
        print(f"[ntp-probe] #{served} from {peer[0]}:{peer[1]} "
              f"len={len(data)} silent={args.silent}", flush=True)
        if args.silent:
            continue
        rep = build_reply(data, args, time.time())
        if args.reply_bytes > 0:
            rep = rep[:args.reply_bytes]
        s.sendto(rep, peer)
    s.close()
    print(f"[ntp-probe] 结束，共处理 {served} 个请求", flush=True)
    return 0


def selftest():
    """先量一遍量具（陷阱 82）：把**已知**的时刻写成时间戳再读回来。

    这一段不碰被测代码，它验的是「这个假服务器自己会不会算错」。少了它，
    C++ 侧量出一个偏差时，第一个要问的问题是「是我的客户端错了，还是假
    服务器错了」—— 而那时没有基线可比。
    """
    bad = 0

    def check(ok, what, detail=""):
        nonlocal bad
        if ok:
            print(f"  [OK]   {what}")
        else:
            bad += 1
            print(f"  [FAIL] {what}")
            if detail:
                print(f"         {detail}")

    # 1. 定标数的四舍五入误差必须远小于 1 秒 —— 客户端把它 llround 成整秒，
    #    所以这里差一点点本来无所谓；但差到 1 秒就会让 offset 断言假红。
    for t in (0.0, 1.5, 1759900000.0, 1759900000.75):
        back = from_ntp(to_ntp(t))
        check(abs(back - t) < 1e-4,
              f"往返 {t} → {back:.9f}（误差 {abs(back - t):.2e} 秒）")

    # 2. **纪元常数要单独量一次。** 上面那条是自洽的（同一份实现写、同一份
    #    读），常数错了它照样绿。所以拿一个**独立于本文件算法**的参照：
    #    NTP 时间戳 0 秒 = 1900-01-01，而 Unix 时间戳 0 秒 = 1970-01-01。
    check(from_ntp(struct.pack(">II", EPOCH_DELTA, 0)) == 0.0,
          "NTP 纪元常数：定标数 2208988800.0 恰好是 Unix 的 0 秒")
    check(abs(from_ntp(struct.pack(">II", EPOCH_DELTA + 86400, 0)) - 86400) < 1e-9,
          "差一天就是 86400 秒（偏移量纲没写错）")

    # 3. 回包的字节序是**大端**，而且秒在低地址。写成小端的症状是客户端算出
    #    一个荒唐的偏移，而回包长度、Stratum 全都正常。
    rep = build_reply(b"\x00" * 48, argparse.Namespace(
        offset=0, stratum=2, zero_timestamps=False), 1759900000.0)
    check(len(rep) == 48, f"回包 48 字节（实际 {len(rep)}）")
    check(rep[0] == 0x24, f"首字节 LI/VN/Mode = 0x24（实际 0x{rep[0]:02X}）")
    check(abs(from_ntp(rep[40:48]) - 1759900000.0) < 1e-4,
          "T3 读回来就是我们喂进去的时刻")

    print(f"\n[ntp-probe] 自检：{bad} 项失败")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serve", action="store_true", help="起服务器（否则什么都不做）")
    ap.add_argument("--check", action="store_true", help="自检：量一遍这个量具")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=NTP_PORT)
    ap.add_argument("--offset", type=int, default=0,
                    help="服务器时钟相对本机时钟的偏移（秒），回包按它填")
    ap.add_argument("--stratum", type=int, default=2,
                    help="回包自报的层级。0 = KoD（拒绝服务）")
    ap.add_argument("--silent", action="store_true",
                    help="收下请求但**不回**：确定性地造出客户端的 timeout")
    ap.add_argument("--reply-bytes", type=int, default=0,
                    help=">0 时只回这么多字节（造「回包太短」）")
    ap.add_argument("--zero-timestamps", action="store_true",
                    help="回包里 T2/T3 全零（造「没有时间戳」）")
    ap.add_argument("--requests", type=int, default=0,
                    help=">0 时处理完这么多请求就退出（脚本里免得手工收尾）")
    ap.add_argument("--ready", default=None, metavar="FILE",
                    help="绑定成功后写一个文件，调用方靠它避开竞态")
    a = ap.parse_args()

    if a.check:
        return selftest()
    if a.serve:
        return serve(a)
    print("[ntp-probe] 什么都没做：要么给 --serve，要么给 --check")
    return 2


if __name__ == "__main__":
    sys.exit(main())
