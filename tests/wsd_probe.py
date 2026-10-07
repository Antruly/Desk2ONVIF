#!/usr/bin/env python3
"""WS-Discovery 探针（UDP 3702）。

存在的理由只有一个：**这台机器上的 Windows 自己占着 3702。**
`fdPHost`（Function Discovery Host Provider）把它绑在 `0.0.0.0:3702` 上，
而"两个进程能不能同时收组播"这件事**无法从源码推断** —— libuvcpp 的文档里
那条"能共存但流量全归先绑者"的结论是作者拿 **TCP** 实测出来的，UDP 组播的
语义不同（Windows 上组播报文通常投递给所有绑了该端口并加入组的 socket，
但"通常"不等于"本机如此"）。所以这里实测。

三种模式：

  listen  只加入组、不打字，看能不能收到**任何**东西（包括 Windows 自己的
          流量）。收不到说明绑定/加组这一步就没成。
  probe   发一个 ONVIF 的 Probe，打印收到的所有 ProbeMatch。
          没有 Desk2ONVIF 在跑时，这里应该**一个回应都没有** ——
          这本身就是有用的信息：说明 Windows 不会替我们瞎答。
  spool   一边收一边把原始报文 dump 出来，用来核对 XML 形状。
  send    从一个**临时的、没绑定 3702 的** socket 发一个 Probe 就退出。
          配合 listen 用，回答的是那个真正要紧的问题：**别的机器发来的组播
          我们收得到吗**。自己发给自己的回声不能回答它 —— 回声走的是本
          socket 的回环路径，而监听 3702 的还有 Windows 的 fdPHost，
          "外部报文会不会被它抢走"只有换个 socket 发才看得出来。
  client  **验证被发现的完整往返**：从临时端口发 Probe，在同一个临时端口等
          ProbeMatch。这是真实 ONVIF 客户端的形状，也是唯一能验证"回应确实
          是单播"的形状 —— 绑 3702 的 probe 模式做不到，因为 Windows 上
          同一端口被多个 socket 绑定时，单播只投给其中一个。

用法:
    python -I tests/wsd_probe.py --mode listen --seconds 5
    python -I tests/wsd_probe.py --mode probe  --seconds 5

-I：隔离模式，不把脚本目录和当前目录塞进 sys.path。
"""

import argparse
import socket
import struct
import sys
import time
import uuid

MCAST = "239.255.255.250"
PORT = 3702
ALL_HOSTS = "0.0.0.0"

# ONVIF 的 NetworkVideoTransmitter —— 摄像机在 WS-Discovery 里的类型名。
# NVR 找摄像机发的就是这个。
PROBE_TMPL = """<?xml version="1.0" encoding="UTF-8"?>
<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope"
            xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing"
            xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"
            xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
  <e:Header>
    <w:MessageID>uuid:{mid}</w:MessageID>
    <w:To e:mustUnderstand="true">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>
    <w:Action e:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>
  </e:Header>
  <e:Body>
    <d:Probe>
      <d:Types>dn:NetworkVideoTransmitter</d:Types>
    </d:Probe>
  </e:Body>
</e:Envelope>"""


def local_ipv4s():
    """本机所有 IPv4 地址。组播加入是**按网卡**的，多网卡时挑错网卡就什么都收不到。"""
    out = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip not in out and not ip.startswith("127."):
                out.append(ip)
    except OSError:
        pass
    # 兜底：连一个外部地址看内核选哪条路（不发包）。
    if not out:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 53))
            out.append(s.getsockname()[0])
        except OSError:
            pass
        finally:
            s.close()
    return out


def make_socket(iface):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Windows 上有 SO_REUSEPORT 就直接失败，别用。
    if hasattr(socket, "SO_REUSEPORT"):
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except OSError:
            pass
    try:
        s.bind((ALL_HOSTS, PORT))
    except OSError as e:
        print(f"bind 0.0.0.0:{PORT} 失败：{e}")
        print("（Windows 上 fdPHost 已经绑着这个端口。SO_REUSEADDR 没能抢到共享）")
        return None

    if iface:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface))

    # 组播回环：本机自己发的包也要能收到（单机自测时必须有）。
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)

    # 加入组。mreq = 组地址 + 本地接口地址。传 0.0.0.0 = 由内核挑，
    # 多网卡时这会是"挑错网卡"的头号原因，所以尽量给明确的。
    for ip in ([iface] if iface else (local_ipv4s() or [ALL_HOSTS])):
        mreq = socket.inet_aton(MCAST) + socket.inet_aton(ip)
        try:
            s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            print(f"已加入 {MCAST}:{PORT}，接口 {ip}")
        except OSError as e:
            print(f"加入组失败（接口 {ip}）：{e}")

    s.settimeout(0.5)
    return s


def describe(data):
    """从 ProbeMatch 里抠出人看的那几个字段。不引 XML 库，因为要能对畸形报文
    也给出点信息 —— 那恰恰是最需要看原文的时候。"""
    def pick(tag):
        for pre in ("", "d:", "wsd:", "dn:", "w:", "a:"):
            start = data.find(b"<" + (pre + tag).encode() + b">")
            if start < 0:
                start = data.find(b"<" + (pre + tag).encode() + b" ")
            if start < 0:
                continue
            end = data.find(b">", start)
            if end < 0:
                continue
            inner_start = end + 1
            close = data.find(b"</", inner_start)
            if close < 0:
                continue
            return data[inner_start:close].decode("utf-8", "replace")
        return None

    parts = []
    for tag in ("MessageID", "Action", "Address", "Types", "XAddrs"):
        v = pick(tag)
        if v:
            parts.append(f"{tag}={v}")
    return "  ".join(parts) if parts else "(没认出任何字段)"


def check_shape(data):
    """用**严格 XML 解析器**过一遍 ProbeMatch，并回头验证 XAddrs 真的连得上。

    存在的理由是那个已经踩过的坑：libuvcpp 的 `uvcpp_soap_dump_response` 只声明
    信封自身的 `soap:` 前缀，**不声明 inner_xml 里用到的前缀**，于是产出未绑定的
    前缀 —— 而 ElementTree 遇到未绑定前缀是直接抛 ParseError 的。贪心的正则
    匹配看不出这个，因为报文"看起来"完全正常。

    第二项同样重要：XAddrs 里写了一个本机地址，那个地址**必须真的能连上**。
    多网卡机器上挑错网卡不会报任何错，只会在 NVR 界面上表现为"添加失败"。
    """
    import xml.etree.ElementTree as ET
    import urllib.request

    try:
        root = ET.fromstring(data)
    except ET.ParseError as e:
        print(f"    [FAIL] 严格 XML 解析失败：{e}")
        print("           （多半是 inner_xml 里用了没声明的前缀）")
        return
    print(f"    [OK]   严格 XML 解析通过，根元素 {root.tag}")

    NS = {
        "e": "http://www.w3.org/2003/05/soap-envelope",
        "w": "http://schemas.xmlsoap.org/ws/2004/08/addressing",
        "d": "http://schemas.xmlsoap.org/ws/2005/04/discovery",
    }
    xa = root.find(".//d:XAddrs", NS)
    sc = root.find(".//d:Scopes", NS)
    print(f"    [{'OK' if xa is not None else 'FAIL'}]   XAddrs 存在")
    print(f"    [{'OK' if sc is not None else 'FAIL'}]   Scopes 存在")
    if xa is None or not xa.text:
        return
    url = xa.text.strip().split()[0]
    try:
        # 这里只验证"地址可达"，不验证 SOAP 业务 —— 那一步由 curl 的用例覆盖，
        # 而且此刻 device_service 可能还没实现。
        u = urllib.request.urlopen(url, timeout=4)
        print(f"    [OK]   XAddrs 可达：GET {url} -> HTTP {u.status}")
        u.close()
    except urllib.error.HTTPError as e:
        # 4xx/5xx 也算可达：TCP 通了、服务在回答。
        print(f"    [OK]   XAddrs 可达：GET {url} -> HTTP {e.code}（服务在应答）")
    except Exception as e:  # noqa: BLE001
        print(f"    [FAIL] XAddrs 不可达：{url} —— {e}")
        print("           多网卡机器上挑错网卡就是这个现象，且不会有任何报错。")


def probe_as_client(iface, dest, seconds, dump):
    """以**真实 ONVIF 客户端**的形状发 Probe：从临时端口发，在临时端口收。

    为什么不能用绑 3702 的那个 socket（`--mode probe` 干的事）：Windows 上
    多个 socket 用 SO_REUSEADDR 绑同一个端口时，**单播报文只投给其中一个**，
    组播报文也只有一个能拿到。真实客户端绑的是临时端口，所以这里是照着它抄的。

    顺带这也是唯一能验证"回应是**单播**而不是组播"的形状 —— 如果实现把
    ProbeMatch 发回了 239.255.255.250，这个临时端口上一个字都收不到。
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
    if iface:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface))
    s.bind(("0.0.0.0", 0))
    s.settimeout(0.5)

    mid = uuid.uuid4()
    msg = PROBE_TMPL.format(mid=mid).encode()
    dst = (dest or MCAST, PORT)
    s.sendto(msg, dst)
    print(f"已从临时端口 {s.getsockname()[1]} 向 {dst[0]}:{dst[1]} 发出 Probe")
    print(f"MessageID uuid:{mid} —— 应答应当**单播**回到这个临时端口")

    print(f"等待应答 {seconds}s …")
    t0 = time.time()
    seen = 0
    matched = 0
    while time.time() - t0 < seconds:
        try:
            data, addr = s.recvfrom(65535)
        except socket.timeout:
            continue
        except OSError as e:
            print(f"recvfrom 出错：{e}")
            break
        seen += 1
        print(f"\n--- #{seen} 来自 {addr[0]}:{addr[1]}（{len(data)} 字节）")
        print(f"    {describe(data)}")
        if str(mid).encode() in data:
            matched += 1
            # 只用 GBK 里有的字符：控制台是 936 代码页，✓（U+2713）打不出来，
            # 会以 UnicodeEncodeError 把整个脚本掀翻 —— 而那时其实已经成功了。
            print("    RelatesTo 与本机 Probe 对得上 [OK]")
            check_shape(data)
        if dump:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.write("\n")

    s.close()
    print(f"\n共收到 {seen} 个应答（其中 MessageID 对得上的 {matched} 个）。")
    if matched == 0:
        print("没有应答。若 Desk2ONVIF 在跑，可能的原因：")
        print("  · 它不是从本网卡收到的组播（--iface 传的是哪张网卡？）")
        print("  · 3702 被别的软件独占 —— 但我们用的是 SO_REUSEADDR，多进程可共存")
    return 0 if matched else 1


def send_from_stranger(iface, dest):
    """从一个**不加入组、不绑 3702** 的临时端口发 Probe。

    这就是"局域网里另一台机器/NVR"的最小模型：它不是组播组的成员，
    只是往组地址发一个包。如果监听端收不到它，那么真实 NVR 的 Probe
    也一样收不到 —— 而那种故障在 NVR 界面上的表现只是"搜不到设备"。
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
    if iface:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface))
    s.bind(("0.0.0.0", 0))
    mid = uuid.uuid4()
    msg = PROBE_TMPL.format(mid=mid).encode()
    n = s.sendto(msg, (dest or MCAST, PORT))
    print(f"已从临时端口 {s.getsockname()[1]} 向 {dest or MCAST}:{PORT} 发出 {n} 字节")
    print(f"MessageID uuid:{mid} —— 监听端应当能收到它")
    s.close()
    return 0


def run(args):
    if args.mode == "send":
        return send_from_stranger(args.iface, args.dest)
    if args.mode == "client":
        return probe_as_client(args.iface, args.dest, args.seconds, args.dump)

    s = make_socket(args.iface)
    if s is None:
        return 1

    if args.mode in ("probe",):
        mid = uuid.uuid4()
        msg = PROBE_TMPL.format(mid=mid).encode()
        dest = (args.dest or MCAST, PORT)
        print(f"向 {dest[0]}:{dest[1]} 发 Probe（MessageID uuid:{mid}）")
        s.sendto(msg, dest)

    print(f"监听 {args.seconds}s …")
    t0 = time.time()
    seen = 0
    from_others = 0
    while time.time() - t0 < args.seconds:
        try:
            data, addr = s.recvfrom(65535)
        except socket.timeout:
            continue
        except OSError as e:
            print(f"recvfrom 出错：{e}")
            break
        seen += 1
        mine = args.mode == "probe" and str(mid).encode() in data
        tag = "本机回声" if mine else "外部"
        if not mine:
            from_others += 1
        print(f"\n--- #{seen} 来自 {addr[0]}:{addr[1]}（{tag}，{len(data)} 字节）")
        print(f"    {describe(data)}")
        if args.mode == "spool" or args.dump:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.write("\n")

    print(f"\n共收到 {seen} 个报文（其中非本机回声 {from_others} 个）。")
    if args.mode == "probe" and from_others == 0:
        print("没有任何外部回应 —— 如果没有 Desk2ONVIF 在跑，这是**正常**的：")
        print("说明 Windows 不会替我们回答 ONVIF 的 Probe（它只在守自己的设备类型）。")
    s.close()
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["listen", "probe", "spool", "send", "client"],
                    default="listen")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--iface", default=None, help="指定网卡 IP（多网卡时必给）")
    ap.add_argument("--dest", default=None, help="Probe 发到哪（默认组播地址）")
    ap.add_argument("--dump", action="store_true", help="把原始 XML 打出来")
    a = ap.parse_args()
    print(f"本机 IPv4：{local_ipv4s()}")
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
