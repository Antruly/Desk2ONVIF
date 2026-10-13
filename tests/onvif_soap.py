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
import datetime as _dt
import hashlib
import http.client
import json
import os
import re
import subprocess
import sys
import threading
import time
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


def op_body(name, ns, inner=""):
    return f'<{name} xmlns="{ns}">{inner}</{name}>'


class NtpProbe:
    """一个假 SNTP 服务器（`tests/ntp_probe.py` 的子进程），并且**数它收到几个请求**。

    ## 为什么要在这里起，而不是让夹具脚本先起好

    这一个类存在的理由就是让第 [12] 段**自己就能跑**：

        python -I -X utf8 tests/onvif_soap.py --host 127.0.0.1 --port 8080 …

    一条命令，不需要任何前置步骤。挂在夹具上先起一个探针的话，夹具哪天忘了、
    或者改了参数，"对时"这一段就变成一条**永远绿的空转**（探针不在 → 客户端
    超时 → 断言里那个"没发起查询"照样成立），而它在报告上和"验过了"完全同形。
    这条纪律在 `tests\\ntp_check.cpp` 已经用过一次（"必须由夹具起头才有意义的
    单元测试，在夹具被跳过的时候不会有任何人发现"），这里是同一个理由。

    ## 为什么"数请求"这件事本身就是判据

    要证的是**两件相反的事**，而它们各自单独都验不出来：

      * NTP 模式下，NVR 的 `DateTimeType=NTP` **真的发起了一次查询**；
      * 手动模式下，同一个请求**一次查询都不发起**。

    第二件是纯否定式的，光看客户端的 `last_sync_at` 没变是**不够**的 ——
    "我们没发出去"和"发了但结果没落下来"报的是同一个读数。探针那一行
    `[ntp-probe] #N from …` 是这件事唯一的正面证据（本次收到了几个包）。
    两条合起来才说明"手动模式下是真的没问"。

    ## 起不来不是失败（陷阱 87）

    它绑的是 **127.0.0.1:123**（`ntp_client.cpp` 把端口写死成 123）。这台机器上
    多数时候是空的，但要是真被别的 NTP 服务占着，**红的是环境不是产品** ——
    所以 `start()` 失败时调用方打 `[SKIP]`，退出码不动。
    """

    def __init__(self, root):
        self.script = os.path.join(root, "tests", "ntp_probe.py")
        self.proc = None
        self._lines = []
        self._reader = None

    def start(self, offset_s, wait_s=6.0, silent=False):
        """起服务器并按它自己那行「已绑定」做握手。返回 (ok, 原因/诊断)。

        `silent=True` 起的是**黑洞**：它照收不误，但一个回包都不发。这是
        「对时失败」那条路上唯一能造出**确定性超时**的办法，理由写在
        `tests\\ntp_probe.py` 的文件头 —— 不启探针是不行的：环回上没人听的
        UDP 端口会让 ICMP `port unreachable` 把 `recvfrom` 打回来
        （`WSAECONNRESET`），客户端把它归成 `unreachable`，而不是 `timeout`。
        也就是说"根本不起服务器"量到的是**另一条分支**，而且是安静地量错了。
        """
        cmd = [sys.executable, "-I", "-X", "utf8", self.script,
               "--serve", "--host", "127.0.0.1", "--offset", str(offset_s)]
        if silent:
            cmd.append("--silent")
        # **同一个对象可以起第二次，但历史必须清掉。** 下面那个握手是
        # 「`_lines` 里出现过『已绑定』」—— 不清的话它会看见**上一次**那一行
        # 而立刻返回成功，于是第二次起的服务器还在绑端口的路上，测试已经
        # 开始发包了。症状是"新起的那个探针一个包都没收到"，而它指向的是
        # 客户端 —— 又一处「读数缺失被当成结论」。
        self._lines = []
        try:
            self.proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL, text=True, encoding="utf-8",
                errors="replace", bufsize=1)
        except OSError as e:
            return False, f"起不来：{e}"
        # 它每一行都 `flush=True`，所以逐行读是即时的（这个握手能成立就是靠
        # 那一句）。读在一个后台线程上：管道塞满时写端会阻塞，而我们要在自己
        # 这一侧随时能往下走。
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()

        deadline = time.time() + wait_s
        while time.time() < deadline:
            if any("已绑定" in ln for ln in self._lines):
                return True, ""
            if self.proc.poll() is not None:
                break
            time.sleep(0.05)
        return False, ("没有等到「已绑定」。它自己说：\n"
                       + "\n".join(self._lines[-6:] or ["（一个字都没输出）"]))

    def _drain(self):
        for line in self.proc.stdout:
            self._lines.append(line.rstrip("\n"))

    @property
    def requests(self):
        """它**收到过几个包**。只数 `#N` 那几行 —— 「已绑定」和结束语不是请求。"""
        return sum(1 for ln in self._lines if ln.startswith("[ntp-probe] #"))

    def log(self, n=4):
        return "\n".join(self._lines[-n:]) if self._lines else "（它一个字都没说）"

    def stop(self):
        if self.proc is None:
            return
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass
        self.proc = None


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
    ap.add_argument("--config", default=None, metavar="PATH", dest="config_path",
                    help="被测进程的**配置文件**路径（第 [11e2] 段判「防抖有没有"
                         "做事」用）。**不给就明说跳过，不判** —— 见那一段的"
                         "注释：猜一个路径会得到一条恒绿的断言")
    a = ap.parse_args()

    cfg_path = a.config_path

    def cfg_stamp():
        """配置文件现在长什么样：(修改时间纳秒, 大小, inode)。

        **不吞异常**：读不到（路径给错）要当场炸出来，而不是返回一个
        看起来像"没动过"的值 —— 那正好和「防抖生效了」报的是同一个结果
        （陷阱 91：被吞掉的异常和期望结果长得一模一样）。

        为什么**判配置文件而不是判日志**（这一段原先写的正是日志）：判据要
        落在"防抖省掉了什么"上，而防抖省掉的有两样 —— 一次存盘和一行日志。
        日志那条路要求被测进程真的在往文件里写日志，而**裸形态根本不建日志
        文件**（`Log::set_file` 只在 `cmd_console` 里调，见 `main.cpp:2778`；
        `--rtsp --http` 走的是 `cmd_run(..., RunForm::Bare)`，那条路的注释
        白纸黑字写着"没有单实例锁、没有日志文件"）。也就是说探针要是去读
        日志，`tests\rtsp_sub_e2e.ps1`（它就是这个形态）里那条路永远读不到
        东西。而**配置文件两条形态都写**：`on_osd_patch` → `apply_config` →
        `save_config`（`main.cpp:1859`）是**无条件**写的，写在任何 diff 之前；
        而 `save_config` 是"写 .tmp 再改名覆盖"，于是目标文件**每次都换一个
        新文件**（`config.cpp:141-173`）—— 时间和 inode 都会变。

        这比日志更强：它量的就是那句注释里说的"**白写一次盘**"本身，不是
        它的一个副产物。

        `st_ino` 也一起比，是为了挡住"时间戳没变但文件确实换了"这种在粗粒度
        文件系统上才有的假绿；NTFS 上三者都会变，多比一个不花什么。
        """
        st = os.stat(cfg_path)
        return (st.st_mtime_ns, st.st_size, st.st_ino)

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
        # `xsd:duration` 的符号在 `P` **之前**（`-PT5H`），不是 `PT-5H`。
        # 这个正则以前写成 `^PT-?\d`，正好只能通过那个写错的实现 ——
        # 因为本机是东八区，负号那一支从来没被走到过，两个错互相掩护。
        R.check(tz is not None and re.match(r"^-?PT\d", tz.text or ""),
                f"TimeZone 是 ISO8601 时长且符号在 P 前："
                f"{tz.text if tz is not None else None}")
        dt = R.find(root, ".//tds:DateTimeType")
        # ⚠️ **这一条的期望值在第四轮被翻过来了，从前是 `NTP`。**
        #
        # 旧判据问的是"这个钟被改过没有"（`DeviceState::synced()`：没被设过 → NTP）。
        # 那时它是对的 —— 设备上没有任何东西会自动对时，"没被设过"和"跟着主机走"
        # 确实是同一件事。接上真的 NTP 客户端之后这个等式断了：一次**成功的**对时
        # 也会把 `synced()` 置真（它记的是"被改过"，不是"谁在维护"），于是
        # NTP 模式下恰好报 `Manual` —— 一个说反了的事实陈述。
        #
        # 现在问的是"这台设备的钟由谁维护"，唯一的答案在配置里：`time.mode == "ntp"`
        # 报 `NTP`，否则 `Manual`。配置的默认值就是 `manual`，所以**刚起的进程**
        # 报 `Manual` 才是对的，而且它**与"这个钟被设过没有"无关**了 ——
        # 也就是说这一段从此不怕重跑（从前重跑会读到 [1b] 自己设下的 Manual，
        # 见 `out\phase-s-e2e-final.txt` 那一轮的备注）。
        #
        # 这是**行为改进的伴随改动**，不是回归：写在这里免得下一个人把它当回归查。
        R.check(dt is not None and dt.text == "Manual",
                f"DateTimeType = {dt.text if dt is not None else None}"
                f"（默认 time.mode=manual，所以应当是 Manual）",
                "读到 NTP：要么配置里 time.mode 被改成了 ntp，要么这个进程用的"
                "不是默认配置 —— 检查一下是不是用了别人的 --config")

    # ---- 1b. SetSystemDateAndTime：接受，并让 GetSystemDateAndTime 真的跟着走 ----
    #
    # 这一段验的是「接受 NVR 对时」那条取舍的**落点**，而它最容易犯的谎恰好
    # 最难一眼看穿：`SetSystemDateAndTime` 的回应体是空的，回 200 和"什么都
    # 没干"在报文上完全一样。所以判据一律落在**再读一次 GetSystemDateAndTime**，
    # 不落在 Set 的回应上。
    #
    # 断言按**偏移**而不按绝对时刻：设备时钟就是主机时钟在走，设完到读回来的
    # 间隙里它自己又走了几百毫秒。拿绝对时刻做等值比较，要么永远失败，要么
    # 得把容差放大到看不出错。
    #
    # ⚠️ 这一段跑完，设备停在 `DateTimeType = Manual`（见 [1] 那条的说明）。
    print("\n[1b] SetSystemDateAndTime：接受，且 GetSystemDateAndTime 真的跟着走")

    def soap_authed(path, body):
        """带 Digest 发一次 SOAP：先匿名打一枪拿挑战（和 [2]/[3] 同一套流程）。"""
        st, hd, data = c.post(path, body)
        if st == 200:
            return st, data
        ch = parse_challenge(hd.get("www-authenticate", ""))
        if not ch:
            return st, data
        auth = c.digest(path, body, a.user, a.password, ch["nonce"],
                        ch["realm"], ch.get("qop", "auth"))
        st2, _, data2 = c.post(path, body, auth=auth)
        return st2, data2

    def read_device_time():
        """读一次 `GetSystemDateAndTime`，返回 (UTC 六元组, 本地六元组, TZ, 类型)。

        两个日期块都先定位到**块**再往下找：报文里两块各有一套年月日时分秒，
        扁平地找 `Hour` 会拿到哪一块全看谁在前头 —— 服务端那两个辅助函数的
        注释里写着同一件事（这正是它们存在的理由）。
        """
        st, data = soap_authed(
            DEV, envelope(op_body("GetSystemDateAndTime", NS["tds"])))
        if st != 200:
            return None
        root = ET.fromstring(data)

        def fields(block_name):
            blk = R.find(root, ".//tds:" + block_name)
            if blk is None:
                return None
            out = []
            for tag in ("Year", "Month", "Day", "Hour", "Minute", "Second"):
                el = R.find(blk, "tt:" + tag)
                if el is None or not (el.text or "").strip().isdigit():
                    return None
                out.append(int(el.text))
            return tuple(out)

        tz = R.find(root, ".//tds:TimeZone/tt:TZ")
        typ = R.find(root, ".//tds:DateTimeType")
        return (fields("UTCDateTime"), fields("LocalDateTime"),
                tz.text if tz is not None else None,
                typ.text if typ is not None else None)

    def as_dt(f):
        return _dt.datetime(f[0], f[1], f[2], f[3], f[4], f[5],
                            tzinfo=_dt.timezone.utc)

    def off_seconds(f):
        """设备时钟相对**主机**时钟的偏移（秒）。

        主机时刻就在本地取：Python 和被测进程在同一台机器上，同一条时间轴。
        """
        return (as_dt(f) - _dt.datetime.now(_dt.timezone.utc)).total_seconds()

    def dt_fields(t):
        return ("<tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day>"
                "<tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute>"
                "<tt:Second>%d</tt:Second>"
                % (t.year, t.month, t.day, t.hour, t.minute, t.second))

    def set_time(inner):
        return soap_authed(
            DEV, envelope(op_body("SetSystemDateAndTime", NS["tds"], inner)))

    def check_cfg_offset(want, what, tol=5.0):
        """NVR 设完之后，**配置文件里那个偏移**要跟着走。

        ## 为什么这条断言必须存在

        "把设备时钟改成某个时刻"和"把这个偏移**留下来**"是两件事，落在两个地方：
        改时钟在 ONVIF 那一层（`device->set_device_time` —— 水印和
        `GetSystemDateAndTime` 下一帧就跟着走），落盘要走
        `OnvifServiceOptions::on_device_time_set` 回调进配置。少了这条断言，
        整条落盘路径（回调有没有接上、模式闸门、防抖闸门、`apply_config` 有
        没有真写进去）**一行都没被验过**；而它坏起来几乎没有任何症状 ——
        "设了时间"这件事的可见后果（水印、`GetSystemDateAndTime`）全都照样成立，
        直到**重启之后**设备时间弹回主机时间、一段连续录像带着两个时间基准，
        而水印上的时间正是取证时要和录像机对齐的那一项。

        ## 为什么判配置文件，而不是判 `/api/time`

        `/api/time` 的 `configured_offset_s` 报的**就是** `cfg.time.offset_s`，
        但它要先登录 Web 控制台、而且是同一个值的第二个读法。判文件才是
        "重启之后还在不在"那个问题的唯一直接证据（重启读的就是这个文件）。

        ## 不给 `--config` 时明说跳过（陷阱 60）

        路径猜不出来。猜一个不存在的路径会当场抛 `FileNotFoundError`（那还不算
        最坏）；真正危险的是"顺手"改成 `if os.path.exists(...)` 然后静默返回 ——
        那样读不到文件和"偏移被正确写成 0"报的是同一个结果。所以这条路只有两个
        状态：**给了路径就判**，或者**明说跳过**，没有第三种。
        """
        if not cfg_path:
            return R.skip(
                what,
                "没给 --config，读不到被测进程的配置文件 —— 这一条**没验**。"
                "`tests\\rtsp_sub_e2e.ps1` 里是显式传的，所以发布用的那条 e2e "
                "覆盖得到；单跑本文件时不传就是跳过。")
        # **不吞异常 —— 但也**不让它把整份探针带走**。
        #
        # `cfg_stamp` 那条的写法是"读不到就当场抛"，因为它的返回值是**比较用
        # 的一个值**：吞掉之后返回一个看起来像"没动过"的东西，正好和"防抖生效
        # 了"报同一个结果。这里不一样 —— 读失败是红、读到才有值，两种结果本来
        # 就分得开，所以更好的做法是把它报成**一条红断言**：仍然不吞（异常消息
        # 直接进报告），但不会让后面两百多条断言一起消失。
        #
        # 这不是假设出来的：变异检验（把 `o.on_device_time_set(...)` 那一句注释
        # 掉）第一次跑就是这个形状 —— 落盘那一步没了 ⇒ 配置文件**根本没被创建**
        # ⇒ `open()` 抛 `FileNotFoundError` ⇒ 探针从 [1b] 当场崩掉，报告停在
        # "偏移变成 +178.3 秒"那一行，**后面的 [2]…[12] 一条都没跑**；而在夹具
        # 层（`$ErrorActionPreference='Stop'`）它还会变成一句 `NativeCommandError`
        # 把整个 e2e 也带走。**一条断言失败和一份探针崩掉，在报告上是两回事。**
        try:
            with open(cfg_path, encoding="utf-8") as f:
                got = json.load(f)["time"]["offset_s"]
        except Exception as e:
            return R.check(False,
                           f"{what}：读不到配置文件里的 time.offset_s",
                           f"{type(e).__name__}: {e}\n路径 {cfg_path}\n"
                           f"（这个文件**根本不在**就是最直接的线索：NVR 设完"
                           f"时间之后它本该被重写一遍。）")
        return R.check(
            abs(got - want) <= tol,
            f"{what}：配置文件里 time.offset_s = {got}"
            f"（设的是 {want:+.0f}，容差 ±{tol:.0f}）",
            "这一条红 = NVR 设的时间**没有落盘**，重启之后设备时间会弹回主机"
            "时间。依次查：`wo.onvif.on_device_time_set` 有没有接上、"
            "`cfg.time.mode` 是不是 manual（NTP 模式下按设计不写盘）、"
            "以及防抖那一句有没有把这次改动挡掉。")

    base = read_device_time()
    if R.check(base is not None and base[0] and base[1],
               "基准：两个日期块都读得出来"):
        # 第四个（`DateTimeType`）在 [1] 那一节已经断言过了，这里不重复。
        utc0, loc0, tz0, _ = base
        off0 = off_seconds(utc0)
        R.check(abs(off0) < 5,
                f"基准偏移 {off0:+.1f} 秒（没被设过时应当贴着主机时钟）")
        # 设备自己报的 `LocalDateTime - UTCDateTime` 就是它认的时区偏移，拿它
        # 去构造下面的请求 —— 比在测试里另写一个 `xsd:duration` 解析器好：
        # **解析器不该出现在判据的路径上**，它写错了报出来的会是"服务端把 TZ
        # 认错了"，指向完全反了。
        tz_min = int(round((as_dt(loc0) - as_dt(utc0)).total_seconds() / 60))
        R.check(tz0 is not None and tz_min != 0,
                f"设备报的时区：TZ={tz0}，LocalDateTime-UTCDateTime = {tz_min} 分钟",
                "本机时区恰好是 UTC 的话，下面那条 TZ 往返断言就分辨不出"
                "「认出来了」和「没认出来、按 UTC 算」—— 本机不是 UTC")

        # -- 1b-1. Manual + UTCDateTime：真的往前走 ------------------------
        target = (_dt.datetime.now(_dt.timezone.utc)
                  + _dt.timedelta(seconds=180)).replace(microsecond=0)
        st, data = set_time("<tds:DateTimeType>Manual</tds:DateTimeType>"
                            "<tds:UTCDateTime>" + dt_fields(target)
                            + "</tds:UTCDateTime>")
        R.check(st == 200, f"Manual + UTCDateTime → HTTP {st}", data[:200])
        after1 = read_device_time()
        if R.check(after1 is not None and after1[0] is not None,
                   "设完之后读得回来"):
            R.check(after1[3] == "Manual",
                    f"DateTimeType 变成 {after1[3]}（此刻的钟是 NVR 在维护）",
                    "设过之后仍报 NTP 的话，ONVIF Device Manager 之类会一直"
                    "摆着「设置设备时间」的入口")
            off1 = off_seconds(after1[0])
            R.check(abs(off1 - 180) < 5,
                    f"偏移变成 {off1:+.1f} 秒（设的是 +180）")
            # 时钟走起来了（上面那条），这里问的是**另一个问题**：这一份偏移
            # 有没有被留下来。两条一起绿才叫"这一次设置完整地成了"。
            check_cfg_offset(180.0, "1b-1 NVR 设的偏移留住了")

            # -- 1b-2. NTP 是空操作 ----------------------------------------
            #
            # ⚠️ **「空操作」这个说法现在是模式相关的**（第四轮 Phase U 起）。
            # `DateTimeType=NTP` 在服务端那条分支上做的事，从"打一行日志"
            # 变成了"看 `time.mode` 决定"：
            #
            #   * 手动模式 —— 打一行「当前是手动模式，不发起对时」就返回，
            #     **连查询都不发**。所以这一条在这里成立。
            #   * NTP 模式 —— 真的去问配置里那台服务器，结果写进
            #     `last_sync_*`（`onvif_service.cpp` 的 `on_ntp_request`）。
            #     那时这一条会**红**，而红得对：钟确实被 NTP 改掉了。
            #
            # 本文件跑的是**全新进程**，`time.mode` 的默认值是 `manual`，
            # 所以下面这条断言成立。真正把两种模式都咬住的是第 [12] 段 ——
            # 它显式地两边都跑一遍。这里不动它，只把前提写下来：留着不写
            # 的话，下一个人换成一个 mode=ntp 的配置文件跑本文件，会看到
            # 这条红而以为产品坏了。
            st, data = set_time("<tds:DateTimeType>NTP</tds:DateTimeType>")
            R.check(st == 200,
                    f"DateTimeType=NTP → HTTP {st}（回 Fault 会让海康的添加"
                    f"向导停在时间那一步）", data[:200])
            after2 = read_device_time()
            if after2 is not None and after2[0] is not None:
                off2 = off_seconds(after2[0])
                R.check(abs(off2 - off1) < 5,
                        f"NTP 那一次什么都没改（偏移仍是 {off2:+.1f} 秒）")

            # -- 1b-3. LocalDateTime + TZ：自己报出去的串自己读得回来 --------
            # 把设备上一句刚报的 `TZ`（`tz0`）原样喂回去。这一条同时验两件事：
            # 解释得对，以及报出来的格式自己能读懂。这两件事**以前都是坏的**
            # —— `parse_tz_minutes` 只认 `UTC+08:00`/`+08:00` 那种写法，
            # 压根不认规范要求的 `xsd:duration`，于是真 NVR 送来 `PT8H` 时
            # 按 UTC 处理，钟一下子错 8 小时，而日志里只有一行 WARN。
            target2 = (_dt.datetime.now(_dt.timezone.utc)
                       - _dt.timedelta(seconds=120)).replace(microsecond=0)
            local2 = target2 + _dt.timedelta(minutes=tz_min)
            st, data = set_time(
                "<tds:DateTimeType>Manual</tds:DateTimeType>"
                "<tds:LocalDateTime>" + dt_fields(local2) + "</tds:LocalDateTime>"
                "<tds:TimeZone><tt:TZ>" + tz0 + "</tt:TZ></tds:TimeZone>")
            R.check(st == 200, f"LocalDateTime + TZ={tz0} → HTTP {st}", data[:200])
            after3 = read_device_time()
            if after3 is not None and after3[0] is not None:
                off3 = off_seconds(after3[0])
                R.check(abs(off3 + 120) < 6,
                        f"偏移变成 {off3:+.1f} 秒（设的是 -120 秒）",
                        f"TZ={tz0} 要是没被认出来（按 UTC 算），这里读到的会是 "
                        f"{off3 + tz_min * 60:+.1f} 秒 —— 差整整 {tz_min} 分钟")
                # 第二次，而且方向相反（+180 → −120）。**同一条断言在两个不同的
                # 值上各跑一次**是有意的：只跑一次的话，"写进去的值恰好等于那个
                # 常量"这类错误（回调传错、或者配置项被写死）分不出来。
                check_cfg_offset(-120.0, "1b-3 NVR 设的偏移留住了")

            # -- 1b-4. 越界的日期要 Fault，不能"夹一下" ---------------------
            # `xml_int` 的上下界是这一轮新加的。夹取比回 Fault 坏得多：NVR 会
            # 认定时间设好了，然后带着一个错的时标录下去。
            st, data = set_time(
                "<tds:DateTimeType>Manual</tds:DateTimeType><tds:UTCDateTime>"
                "<tt:Year>2026</tt:Year><tt:Month>13</tt:Month>"
                "<tt:Day>1</tt:Day><tt:Hour>0</tt:Hour>"
                "<tt:Minute>0</tt:Minute><tt:Second>0</tt:Second>"
                "</tds:UTCDateTime>")
            R.check(st == 500, f"13 月 → HTTP {st}（Fault，而不是夹成 12 月）",
                    data[:200])
            bad_root = R.strict_xml(data, "13 月的 Fault")
            if bad_root is not None:
                R.check(R.find(bad_root, ".//soap:Reason") is not None,
                        "Fault 里有 soap:Reason（结构良好，不是断掉的响应）")
            after_bad = read_device_time()
            if after_bad is not None and after_bad[0] is not None:
                offb = off_seconds(after_bad[0])
                R.check(abs(offb - off3) < 5,
                        f"被拒之后时钟没被动过（偏移仍是 {offb:+.1f} 秒）")

            # -- 1b-5. 恢复主机时间 ----------------------------------------
            st, data = set_time(
                "<tds:DateTimeType>Manual</tds:DateTimeType><tds:UTCDateTime>"
                + dt_fields(_dt.datetime.now(_dt.timezone.utc)
                            .replace(microsecond=0))
                + "</tds:UTCDateTime>")
            R.check(st == 200, f"恢复主机时间 → HTTP {st}", data[:200])
            after4 = read_device_time()
            if after4 is not None and after4[0] is not None:
                off4 = off_seconds(after4[0])
                R.check(abs(off4) < 5, f"偏移回到 {off4:+.1f} 秒（贴着主机时钟）")
                # 归零也要落盘 —— 这是"恢复主机时间"这个动作完整性的那一半。
                # 少这一条的话，"把偏移设成 0"这条路上任何一处写不进去都看不出来，
                # 而它的症状是重启之后又冒出那 3 分钟的偏移（配置文件里还留着
                # 上一次的 -120）。
                check_cfg_offset(0.0, "1b-5 归零也落盘了")

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

    def get_status():
        """取一次 `/api/status`。**每次现取，不缓存** —— 这里要读的正是会动的
        计数器（`ticks` / `osd_hot_applied`），缓一份下来等于把尺子冻住。"""
        st, _, b = c.get("/api/status")
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

        cfg_hw = (cfg0.get("onvif") or {}).get("hardware_id", "")
        cfg_uuid = (cfg0.get("onvif") or {}).get("device_uuid", "")

        # -- 9a. 身份：POST 一份**局部**配置，可改的改、锁死的拒 -----------
        # 局部 body 是合法输入（config_from_json 对缺失的键保留当前值），
        # 也正是 Web 界面之外最自然的改法：curl 打个补丁。
        #
        # ⚠️ 这一份**故意混着**可改的（`model`）和锁死的（`serial_number`、
        # `hardware_id`、`device_uuid`）—— 第五轮 Phase AB 的取舍是：设备标识
        # 由本机硬件固定，网页上不许改（`config.json` 仍是逃生门）。混在一起
        # 才验得了"锁只锁那三项"，而不是"整段 onvif 都不让写"。
        st, _, b = c.post_json("/api/config", {
            "onvif": {"model": "HotSwap-E2E", "serial_number": "HOT-E2E-0001",
                      "hardware_id": "HW-E2E-0001",
                      "device_uuid": "urn:uuid:00000000-0000-0000-0000-0000000000e2e"}})
        R.check(st == 200, f"改 ONVIF 身份 → HTTP {st}", b[:200])
        try:
            res = json.loads(b)
        except ValueError:
            res = {}
        applied = res.get("applied") or []
        pending = res.get("pending") or []
        R.check("onvif.model" in applied, f"回应里 applied 含可改的 onvif.model：{applied}")
        R.check("onvif.serial_number" not in applied
                and "onvif.hardware_id" not in applied
                and "onvif.device_uuid" not in applied,
                f"三个锁死的字段**没有**出现在 applied 里：{applied}")
        R.check("onvif.model" not in pending and "onvif.serial_number" not in pending,
                f"且这几项**不在** pending 里：{pending}")
        # **被拒必须说出来。** 静默忽略的后果不是"少了一个功能"，是下一个人
        # 以为这个校验没做、于是再去做一遍，或者以为保存坏了。
        #
        # 找的是 `note`（**单数**、一句话），不是一个 `notes` 数组：那条提示是
        # 拼进既有那句话里的，理由写在 web_server.cpp 那一段 —— 再开一个平行
        # 的数组就会出现"有的提示在前端、有的没人显示"。
        note = res.get("note") or ""
        R.check("固定" in note or "硬件" in note,
                f"回应里明说了这几项被拒（note={note!r}）")

        # -- 9b. 真的生效了吗？再走一次 SOAP 看设备报什么 -------------------
        # **这一条才是重点。** 上面那些只看后端自己怎么说；只有这里是从
        # 一条真实的 ONVIF 请求里读回来的，中间没有重启。
        info, st = device_info(a.user, a.password)
        R.check(st == 200, f"改完之后 GetDeviceInformation → HTTP {st}")
        if info:
            R.check(info.get("Model") == "HotSwap-E2E",
                    f"设备报的 Model 立刻变成了 HotSwap-E2E（实际 {info.get('Model')}）"
                    " —— 没有重启")
            # 反向那一半：同一份 body 里那三个锁死的字段，设备**必须**还在报
            # 原值。只验"applied 里没有它"是不够的 —— 那只证明后端**说**没改。
            R.check(info.get("SerialNumber") == old_serial,
                    f"SerialNumber 没有跟着变，还是原值（实际 {info.get('SerialNumber')}，"
                    f"原值 {old_serial}）")
            # 配置里 `hardware_id` 没写死过时是空串，而设备那边空元素解出来是
            # `None` —— 两边都是"没有值"，但那不是一个值，直接比会凭空红一条。
            if cfg_hw:
                R.check(info.get("HardwareId") == cfg_hw,
                        f"HardwareId 也没有跟着变（实际 {info.get('HardwareId')}，"
                        f"原值 {cfg_hw}）")
        cfg_locked = get_cfg()
        R.check(cfg_locked is not None
                and (cfg_locked.get("onvif") or {}).get("serial_number") == old_serial
                and (cfg_locked.get("onvif") or {}).get("hardware_id") == cfg_hw
                and (cfg_locked.get("onvif") or {}).get("device_uuid") == cfg_uuid,
                "GET /api/config 里那三项也还是原值（没被写进内存，也没落盘）")

        # -- 9c. 冷字段：改子码流高度必须进 pending ------------------------
        # **不能写死一个 288 就发过去**：子码流高度本来就可能已经是 288
        # （288 就是它的默认值），那样这一次 POST 什么都没改，pending 自然是
        # 空的 —— 而断言会把它报成"冷字段没进 pending"，指向完全反了。
        # 判据是"改一个**不同**的值才进 pending"，所以先把当前值读出来。
        # 这是陷阱 60 那条的同一个形状：一条依赖"环境碰巧不等于某个值"的
        # 断言，换台机器（或换个测试顺序）就恒真或恒假。
        cur_sub = ((cfg0.get("rtsp") or {}).get("sub_height"))
        new_sub = 240 if cur_sub == 288 else 288
        st, _, b = c.post_json("/api/config", {"rtsp": {"sub_height": new_sub}})
        R.check(st == 200, f"改子码流高度（{cur_sub} → {new_sub}）→ HTTP {st}",
                b[:200])
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

    # ---- 10. ONVIF 的 Set* ------------------------------------------------
    #
    # 判据一律落在**再读一次 /api/config** 上，不落在 Set* 的回应上：`Set*`
    # 的响应体是**空的**，回 200 和"什么都没干"在报文上完全一样。这和 [1b]
    # 对 `SetSystemDateAndTime` 的处理是同一条理由。
    #
    # ⚠️ 几何字段是**如实进 pending** 的，别指望在 `/api/status` 里看到新 fps：
    # 当场改分辨率要重建编码器，而重建会把 RTSP 客户端全踢掉（陷阱 51 ——
    # 这条链路上运行中改码率根本无解）。该看到的是它进了 pending，以及
    # `capture.quality` 被翻成了 `custom`。
    print("\n[10] Set*：改名当场生效，几何字段如实进 pending")

    def media_authed(body):
        """带 Digest 打一次媒体服务（和 [5]/[6]/[7] 同一套流程）。"""
        st, hd, data = c.post(MED, body)
        if st == 200:
            return st, data
        ch = parse_challenge(hd.get("www-authenticate", ""))
        if not ch:
            return st, data
        auth = c.digest(MED, body, a.user, a.password, ch["nonce"],
                        ch["realm"], ch.get("qop", "auth"))
        st2, _, data2 = c.post(MED, body, auth=auth)
        return st2, data2

    def profile_names():
        """从 `GetProfiles` 读回每一路**报出去**的 `tt:Name`（token → name）。"""
        st, data = media_authed(envelope(op_body("GetProfiles", NS["trt"])))
        if st != 200:
            return None
        try:
            root = ET.fromstring(data)
        except ET.ParseError:
            return None
        return {p.get("token"): (p.findtext("tt:Name", None, NS) or "")
                for p in root.findall(".//trt:Profiles", NS)}

    cfg_a = get_cfg()
    if cfg_a is None:
        R.check(False, "GET /api/config 拿不到，Set* 这一段没法验")
    else:
        cap0 = dict(cfg_a.get("capture") or {})
        rtsp0 = dict(cfg_a.get("rtsp") or {})
        name0 = (cfg_a.get("onvif") or {}).get("device_name", "")

        # 名字的**起点**：改名前先把它照下来，后面一律拿它比。
        # 写死 "main" 也能用，但那是一条"依赖出厂值恰好等于某个字面量"的
        # 断言（陷阱 60 的同一个形状）—— 换一份 --config 就恒假。
        names0 = profile_names()

        # **这台服务器有没有"真的 profile 表"。**
        # `OnvifServiceOptions::profiles` 是按 `opt.rtsp.streams` 逐个填的，
        # 而 streams 只在 `if (opt.rtsp.enabled)` 里建 —— 所以"没开 RTSP"就等于
        # "一路码流都没有"，`OnvifService` 答的是编译期那份占位表（`kFallback`，
        # 见 onvif_service.cpp）。占位表是 `static const`，**它没有"改名"这回事**，
        # 也不该有：这个配置下根本没有码流可以加，NVR 连不上任何东西。
        #
        # 取的是 `/api/config` 里那个**生效值**（main.cpp 在 web 起来之前就回写
        # 好了），不是 `--config` 传了什么，也不是运行期统计 —— 后者在采集起来
        # 之前是空的，拿它当判据会变成一条看时序的断言。
        has_profiles = bool(rtsp0.get("enabled"))

        # -- 10a. 回声不算改名 ---------------------------------------------
        # `Name` 等于我们自己在这一路的 `GetProfiles` 里**报出去**的那个值时，
        # 那是 NVR 把读到的整份配置写回来，不是改名。用户在它自己的编码参数
        # 页上改个码率，整份 `Configuration` 都会被送回来，而那个 `Name` 正是
        # 我们报的 "main" —— 认不出来的话，症状是"在 NVR 上改码率，画面上的
        # 名称变成了 main"，没有人会把这两件事联系起来。
        #
        # ⚠️ **这一条必须排在所有改名之前。** 一旦这一路被改成 NVR-CH01，它的
        # `tt:Name` 就跟着变了，此时再送一个 "main" 回来是**货真价实的新改名
        # 请求**（把名字从 NVR-CH01 改成 main），而不是回声。拿"名称没变"去
        # 判会得到一个指着产品的假失败 —— 第一版就是这么写的，三条红的。
        #
        # 判据是"**什么都没动**"，四样各查一遍：名称、报出去的名字、编码参数、
        # pending。只查名称的话，一个"顺手把整份配置重写一遍"的实现照样能过。
        pend0 = set(cfg_a.get("pending") or [])
        st, data = media_authed(envelope(op_body(
            "SetVideoEncoderConfiguration", NS["trt"],
            '<Configuration token="VideoEncoderConfig_main">'
            "<Name>main</Name>"
            "</Configuration>")))
        R.check(st == 200, f"回写自己报出去的 Name（main）→ HTTP {st}", data[:300])
        cfg_e = get_cfg() or {}
        R.check((cfg_e.get("onvif") or {}).get("device_name") == name0,
                f"回声没有把名称改成 main：device_name = "
                f"{(cfg_e.get('onvif') or {}).get('device_name')}（仍是 {name0}）")
        cap_e = cfg_e.get("capture") or {}
        diff_e = {k: (v, cap_e.get(k)) for k, v in cap0.items()
                  if cap_e.get(k) != v}
        R.check(not diff_e, f"回声没碰任何编码参数（前 → 后）：{diff_e}")
        R.check(set(cfg_e.get("pending") or []) == pend0,
                f"回声没往 pending 里加东西：{cfg_e.get('pending')}"
                f"（原本 {sorted(pend0)}）")
        names_e = profile_names()
        R.check(names_e == names0,
                f"我们报出去的 tt:Name 也没被回声改掉：{names_e}（原本 {names0}）")

        # -- 10b. 只带 Name 的最小报文 -------------------------------------
        # "只带一项"正是 `OnvifDevicePatch` 里那八个 `std::optional` 存在的
        # 理由：没提到的字段**一律不该被碰**。写成一个裸值 + has_xxx 布尔
        # （或者干脆无条件赋值）的话，症状是"NVR 只是改了个名字，帧率被打回了
        # 默认值"—— 而 NVR 那边只看得到"成功"。
        st, data = media_authed(envelope(op_body(
            "SetVideoEncoderConfiguration", NS["trt"],
            '<Configuration token="VideoEncoderConfig_main">'
            "<Name>NVR-CH01</Name>"
            "</Configuration>")))
        R.check(st == 200,
                f"SetVideoEncoderConfiguration（只带 Name）→ HTTP {st}", data[:300])
        root = R.strict_xml(data, "SetVideoEncoderConfiguration")
        if root is not None:
            body_el = root.find(".//soap:Body", NS)
            inner = list(body_el)[0] if body_el is not None and len(body_el) else None
            R.check(inner is not None and len(inner) == 0,
                    "响应体是**空的** Response 元素（Set* 不回配置）",
                    ET.tostring(inner) if inner is not None else None)

        cfg_b = get_cfg() or {}
        R.check((cfg_b.get("onvif") or {}).get("device_name") == "NVR-CH01",
                f"改名落到配置里：device_name = "
                f"{(cfg_b.get('onvif') or {}).get('device_name')}（设的是 NVR-CH01）")
        # **这一条是本段最值钱的断言。** 只报"名字变了"的话，一个把所有字段
        # 都重写一遍的实现照样能过。
        capb = cfg_b.get("capture") or {}
        untouched = {k: (cap0.get(k), capb.get(k)) for k in
                     ("fps", "bitrate_kbps", "max_height", "gop_seconds",
                      "quality", "profile")}
        R.check(all(x == y for x, y in untouched.values()),
                f"报文里没提到的编码参数一项都没变：{untouched}")
        R.check("onvif.device_name" not in (cfg_b.get("pending") or []),
                f"名称**不在** pending 里（它是热的）：{cfg_b.get('pending')}")
        # **改名在协议层唯一的证据。** NVR 改完通道名会回读一次配置，而报旧
        # 名字的话它会认为这次改名没落地 —— 有些 NVR 会就此把名字改回它自己
        # 的缓存值，用户看到的是"改了又跳回来"，而水印这边明明已经变了。
        names_a = profile_names()
        if has_profiles:
            R.check(names_a is not None and names_a.get("main") == "NVR-CH01",
                    f"下一份 GetProfiles 就报出新名字：{names_a}")
        else:
            # 没有码流 ⇒ 答的是占位表，名字是编译期常量，改不动也**没有可改的
            # 对象**。这一条盯的是"别为了让它跟着变而把兜底表弄成可写的"：
            # 那会把一个占位值和一个真实 profile 之间的界线抹掉，而界线一旦
            # 没了，`set_video_size(0, …)` 就会开始往那张表里写真实分辨率。
            R.check(names_a == names0,
                    f"没有码流时 GetProfiles 答的是占位表，名字原样不动："
                    f"{names_a}（原本 {names0}）")

        # -- 10c. 带几何字段：如实进 pending，并把档位翻成 custom ----------
        cur_gop = int(cap0.get("gop_seconds") or 2)
        new_gop = 3 if cur_gop != 3 else 5
        new_fps = 12
        gov = new_gop * new_fps      # GovLength 是**帧数**，配置里存的是秒
        st, data = media_authed(envelope(op_body(
            "SetVideoEncoderConfiguration", NS["trt"],
            '<Configuration token="VideoEncoderConfig_main">'
            "<Name>NVR-CH01</Name>"
            "<Resolution><Width>1280</Width><Height>720</Height></Resolution>"
            "<RateControl>"
            f"<FrameRateLimit>{new_fps}</FrameRateLimit>"
            "<EncodingInterval>1</EncodingInterval>"
            "<BitrateLimit>768</BitrateLimit></RateControl>"
            f"<H264><GovLength>{gov}</GovLength>"
            "<H264Profile>Main</H264Profile></H264>"
            "</Configuration>")))
        R.check(st == 200,
                f"SetVideoEncoderConfiguration（带几何字段）→ HTTP {st}", data[:300])
        cfg_c = get_cfg() or {}
        capc = cfg_c.get("capture") or {}
        R.check(capc.get("fps") == new_fps,
                f"capture.fps = {capc.get('fps')}（FrameRateLimit 设的是 {new_fps}）")
        R.check(capc.get("bitrate_kbps") == 768,
                f"capture.bitrate_kbps = {capc.get('bitrate_kbps')}（BitrateLimit 设的是 768）")
        R.check(capc.get("max_height") == 720,
                f"capture.max_height = {capc.get('max_height')}"
                "（Resolution/Height 设的是 720）")
        # GovLength 是**帧数**，配置里存的是**秒** —— 这个换算两边各写一次，
        # 差一个 fps 的话 NVR 设"每 36 帧一个 I 帧"会变成"每 36 秒一个"。
        R.check(capc.get("gop_seconds") == new_gop,
                f"GovLength {gov} 帧 ÷ {new_fps} fps = {new_gop} 秒"
                f"（实际 {capc.get('gop_seconds')}）")
        R.check(capc.get("profile") == "main",
                f"H264Profile Main → capture.profile = {capc.get('profile')}")
        # **档位必须翻成 custom。** 不翻的话 `validate_config` 会按档位表把
        # fps / bitrate_kbps / max_height 三项**重写**回档位值，而且发生在
        # 落盘之前 —— 这次改动连磁盘都留不下。症状是"NVR 改完毫无变化"。
        R.check(capc.get("quality") == "custom",
                f"capture.quality 翻成了 custom（实际 {capc.get('quality')}）"
                " —— 不翻的话这一次改动会被档位表展开掉")
        pend_c = cfg_c.get("pending") or []
        for f in ("capture.fps", "capture.bitrate_kbps", "capture.max_height"):
            R.check(f in pend_c, f"{f} 在 pending 里：{pend_c}")

        # -- 10d. SetVideoSourceConfiguration：认不出 token 也必须接受 ------
        # 我们报出去的源配置 token 是两路**共用**的一个（见 profile_block），
        # 所以 NVR 送回来的那个我们一律认不出来。回 Fault 会让它的添加流程
        # 当场失败（界面上是"加不上"，而不是"有个参数没生效"），所以这条路
        # 必须是"接受 + 按主码流处理 + 打一条 WARN"。
        st, data = media_authed(envelope(op_body(
            "SetVideoSourceConfiguration", NS["trt"],
            '<Configuration token="VideoSourceConfig">'
            "<Name>VideoSourceConfig</Name>"
            '<tt:Bounds x="0" y="0" width="1280" height="960"/>'
            "</Configuration>")))
        R.check(st == 200,
                f"SetVideoSourceConfiguration（我们不认识的 token）→ HTTP {st}"
                " —— 认不出也必须接受", data[:300])
        cfg_d = get_cfg() or {}
        R.check((cfg_d.get("capture") or {}).get("max_height") == 960,
                f"Bounds 的 height 属性被读到了：max_height = "
                f"{(cfg_d.get('capture') or {}).get('max_height')}（设的是 960）")
        # 源配置的 `Name` 是它自己的标签，**不是设备名**。拿它去改名的话，
        # NVR 把读到的整份配置写回来（用户在它自己的页面上改个码率就是这样）
        # 会把画面上的名称变成 "VideoSourceConfig"。
        R.check((cfg_d.get("onvif") or {}).get("device_name") == "NVR-CH01",
                f"SetVideoSourceConfiguration 的 Name 不当改名：device_name = "
                f"{(cfg_d.get('onvif') or {}).get('device_name')}")

        # -- 10e. 子码流那一侧：认得出 token 时改的是**它** ----------------
        # 只在开了子码流时才有这一路（`--no-sub`，或者像本文件默认那样没开
        # RTSP，都只有主码流 —— 子码流的编码器是 `if (opt.rtsp.enabled)` 里
        # 建的）。判据是"改的是子码流、主码流一个字节没动"：写反了的表现是
        # "NVR 改了子码流，主码流变了"，而 GetProfiles 两边都照常回应。
        #
        # `blocks` 是 [5] 段从 `GetProfiles` 里读回来的 token 表 —— 拿真实
        # 回应当条件，而不是猜"应该有两路"。
        if "sub" in blocks:
            sh0 = int(rtsp0.get("sub_height") or 288)
            new_sh = 240 if sh0 != 240 else 480
            st, data = media_authed(envelope(op_body(
                "SetVideoEncoderConfiguration", NS["trt"],
                '<Configuration token="VideoEncoderConfig_sub">'
                "<Name>NVR-CH02</Name>"
                f"<Resolution><Width>426</Width><Height>{new_sh}</Height>"
                "</Resolution>"
                "<RateControl><BitrateLimit>333</BitrateLimit></RateControl>"
                "</Configuration>")))
            R.check(st == 200, f"改子码流（token=VideoEncoderConfig_sub）→ HTTP {st}",
                    data[:300])
            cfg_f = get_cfg() or {}
            rtspf = cfg_f.get("rtsp") or {}
            capf = cfg_f.get("capture") or {}
            R.check(rtspf.get("sub_height") == new_sh,
                    f"子码流高度落到了 rtsp.sub_height：{rtspf.get('sub_height')}"
                    f"（设的是 {new_sh}）")
            R.check(rtspf.get("sub_bitrate_kbps") == 333,
                    f"子码流码率落到了 rtsp.sub_bitrate_kbps："
                    f"{rtspf.get('sub_bitrate_kbps')}（设的是 333）")
            # **主码流一个字节都不能动**：`video_source_config` 给两路配的是
            # 同一个源 token，认不出 token 时按主码流兜底，于是"子码流那次"
            # 很容易悄悄落到主码流上。
            R.check(capf.get("bitrate_kbps") == 768,
                    f"主码流码率没被动：capture.bitrate_kbps = "
                    f"{capf.get('bitrate_kbps')}（仍是 10b 设的 768）")
            R.check((cfg_f.get("onvif") or {}).get("device_name") == "NVR-CH02",
                    f"子码流那一份的 Name 也是改名：device_name = "
                    f"{(cfg_f.get('onvif') or {}).get('device_name')}")
            # 名字是**按 profile** 报的：改了哪一路就只有哪一路变。两路报同一个
            # 名字也一样能用，但"NVR 只改子码流、主码流跟着变"说明 token 解析
            # 落到了兜底那一支上，而那正是本段要防的错。
            names_f = profile_names() or {}
            R.check(names_f.get("sub") == "NVR-CH02",
                    f"子码流 profile 报出新名字：{names_f}")
            R.check(names_f.get("main") == "NVR-CH01",
                    f"主码流 profile 的名字没被动：{names_f}")
        else:
            print("  [note] 没有子码流 profile（没开 RTSP 或 --no-sub），10e 跳过")

        # -- 10f. 认不出的 token：说了"按主码流处理"，就整条路都按主码流 ----
        # 上面那条 WARN（"我们不认得…按主码流处理"）是一句**承诺**：几何字段落到
        # 主码流，名称也得落到主码流那一路 profile 上。只做前一半的话设备会自相
        # 矛盾 —— 水印和配置文件都改了，而 NVR 回读到的 `tt:Name` 还是老的；有些
        # NVR 会据此认定改名没落地，把名字改回它自己的缓存值。
        #
        # 这一路只在"NVR 存着一个上一次的旧 token"时才走得到，所以它**不会自己
        # 暴露**。判据是"配置里改了**且** profile 报的也改了"，只查前一半的话
        # 一个只改 `DeviceState` 的实现照样能过。
        bogus = "VideoEncoderConfig_nosuchstream"
        cur_name = ((get_cfg() or {}).get("onvif") or {}).get("device_name")
        new_name = "NVR-CH09" if cur_name != "NVR-CH09" else "NVR-CH08"
        st, data = media_authed(envelope(op_body(
            "SetVideoEncoderConfiguration", NS["trt"],
            f'<Configuration token="{bogus}">'
            f"<Name>{new_name}</Name>"
            "</Configuration>")))
        R.check(st == 200,
                f"认不出的 token（{bogus}）→ HTTP {st} —— 必须接受，不能回 Fault",
                data[:300])
        cfg_g = get_cfg() or {}
        R.check((cfg_g.get("onvif") or {}).get("device_name") == new_name,
                f"认不出 token 时名称照样改：device_name = "
                f"{(cfg_g.get('onvif') or {}).get('device_name')}"
                f"（设的是 {new_name}）")
        if has_profiles:
            names_g = profile_names() or {}
            R.check(names_g.get("main") == new_name,
                    f"并且落到了主码流那一路 profile 上：{names_g}")
        else:
            print("  [note] 没有码流 profile 表（没开 RTSP），10f 只验到配置那一半")

        # -- 10g. 复原 ------------------------------------------------------
        # 几何字段复原不了"运行值"（那要把本次改动放进 pending，等重启），但
        # **配置文件**要还回去：这个文件可能是别处传进来的（--config），留着
        # 这一堆 720/960/333 会让下一次跑测试从一个奇怪的状态起步。
        st, _, b = c.post_json("/api/config", {
            "capture": {k: cap0[k] for k in
                        ("fps", "bitrate_kbps", "max_height", "gop_seconds",
                         "quality", "profile") if k in cap0},
            "rtsp": {k: rtsp0[k] for k in
                     ("sub_height", "sub_bitrate_kbps") if k in rtsp0},
            "onvif": {"device_name": name0}})
        R.check(st == 200, f"还原编码参数与名称 → HTTP {st}", b[:200])

    # ---- [11] OSD（水印）的 ONVIF 表示 ------------------------------------
    #
    # ⚠️ **这一段逐字断言元素名与枚举字面量**，而不是"XML 能解析就算过"。
    # 理由写在 onvif_service.cpp 那段 OSD 注释里：`strict_xml` 只挡得住
    # "前缀没声明"，挡不住"元素名写错了" —— 后者在 NVR 那边是**静默**的
    # （OSD 页面空白，或者干巴巴一句"设备不支持"），而本地测试全绿。
    #
    # 两个最容易写错、偏偏本地看不出来的：
    #
    #   * 列表项叫 `OSDs`、单项叫 `OSD` —— schema 里真的是**两个不同的元素名**，
    #     统一成任何一个，那一个一定错；
    #   * `Capabilities` 下面那两个子元素是 `trt:` 的，不是 `tt:` 的。它们在
    #     media.wsdl 的内联 schema 里就地声明，而 `tt:` 这个前缀信封上早就
    #     声明过了（给别的元素用）—— 所以严格解析器照样放行。
    print("\n[11] OSD（水印）：元素名与枚举字面量逐字对齐 onvif_wsdl")

    cfg_o = get_cfg()
    if cfg_o is None:
        print("  [note] 拿不到 /api/config，[11] 整段跳过")
    else:
        osd0 = dict(cfg_o.get("osd") or {})
        items0 = osd0.get("items") or []
        dev0 = (cfg_o.get("onvif") or {}).get("device_name") or ""
        print(f"  （配置里现有 {len(items0)} 项水印，设备名 {dev0!r}）")

        def expect_fault(st_, data_, what, sub=None, subsub=None):
            """一条 ONVIF Fault：HTTP 500 + 结构良好的 SOAP Fault。

            和 [7] 段同一套判据，只是这里要的是 `Sender`（请求本身有问题）
            而不是 `Receiver`（设备没实现）。
            """
            R.check(st_ == 500, f"{what} → HTTP {st_}（Fault 走 500）", data_[:300])
            root_ = R.strict_xml(data_, what)
            if root_ is None:
                return
            val = R.find(root_, ".//soap:Code/soap:Value")
            got = val.text if val is not None else None
            R.check(got == "soap:Sender",
                    f"{what}：顶层码 = {got}（应为 soap:Sender）")
            if sub:
                s = R.find(root_, ".//soap:Code/soap:Subcode/soap:Value")
                got_s = s.text if s is not None else None
                R.check(got_s == sub,
                        f"{what}：子码 = {got_s}（应为 {sub}）")
            if subsub:
                # **再深一层是 `Subcode/Subcode/Value`**，不是并排两个 Value。
                # 写成并排的话是**非良构**的 SOAP，而两种写法真实设备都有 ——
                # 也就是说只有一个盯着层级的断言才能把这件事定下来。
                ss = R.find(root_, ".//soap:Code/soap:Subcode/soap:Subcode/soap:Value")
                got_ss = ss.text if ss is not None else None
                R.check(got_ss == subsub,
                        f"{what}：次子码 = {got_ss}（应为 {subsub}，"
                        "且必须嵌在 Subcode 里面）")

        # -- 11a. 媒体服务的 GetServiceCapabilities --------------------------
        # `OSD` 是 `trt:Capabilities` 开标签上的**属性**。不报它 = 对 NVR 说
        # "这台设备没有 OSD"，它的 OSD 页面就整个灰掉 —— 而 SOAP 侧一切正常，
        # 日志里一个字都没有。
        st, data = media_authed(envelope(op_body(
            "GetServiceCapabilities", NS["trt"])))
        R.check(st == 200, f"媒体 GetServiceCapabilities → HTTP {st}", data[:300])
        root = R.strict_xml(data, "媒体 GetServiceCapabilities")
        if root is not None:
            cap = R.find(root, ".//trt:Capabilities")
            R.check(cap is not None, "Capabilities 元素在 trt 命名空间里")
            osd_attr = cap.get("OSD") if cap is not None else None
            R.check((osd_attr or "").lower() == "true",
                    f"Capabilities/@OSD = {osd_attr}"
                    "（不报它，NVR 的 OSD 页面会整个灰掉）")
            pc = R.find(root, ".//trt:ProfileCapabilities")
            R.check(pc is not None and (pc.get("MaximumNumberOfProfiles") or "").isdigit(),
                    "ProfileCapabilities 是 trt: 元素，且带 MaximumNumberOfProfiles")
            R.check(R.find(root, ".//tt:ProfileCapabilities") is None,
                    "……而且没有再多留一个 tt:ProfileCapabilities")
            sc = R.find(root, ".//trt:StreamingCapabilities")
            sc_tcp = sc.get("RTP_TCP") if sc is not None else None
            R.check(sc is not None and sc_tcp == "true",
                    f"StreamingCapabilities 是 trt: 元素且 RTP_TCP=true"
                    f"（实际 {sc_tcp}）")

        # -- 11b. GetOSDOptions ---------------------------------------------
        # 这一份是 NVR 画它那个 OSD 编辑页面的**全部依据**：报了却不支持、
        # 或者支持却没报，两个方向都会让用户看到"这个选项不存在"。
        st, data = media_authed(envelope(op_body(
            "GetOSDOptions", NS["trt"],
            "<ConfigurationToken>VideoSourceConfig</ConfigurationToken>")))
        R.check(st == 200, f"GetOSDOptions → HTTP {st}", data[:300])
        root = R.strict_xml(data, "GetOSDOptions")
        max_total = 0
        if root is not None:
            opts = R.find(root, ".//trt:OSDOptions")
            R.check(opts is not None,
                    "OSDOptions 是 trt: 元素（声明在 media.wsdl 里）")
            mx = opts.find("tt:MaximumNumberOfOSDs", NS) if opts is not None else None
            mx_total = mx.get("Total") if mx is not None else None
            # `Total` 在 onvif.xsd 里是 `use="required"` —— 漏了它整份响应
            # schema 不过，而宽松的解析器一个字都不说。
            R.check(mx is not None and (mx_total or "").isdigit(),
                    f"MaximumNumberOfOSDs/@Total 必须有且是数字：{mx_total}")
            if mx is not None and (mx_total or "").isdigit():
                max_total = int(mx_total)
            R.check(max_total > 0, f"报出来的 OSD 上限 = {max_total}")

            top_types = [e.text for e in
                         (opts.findall("tt:Type", NS) if opts is not None else [])]
            R.check(top_types == ["Text"],
                    f"OSDType 只有 Text 一项：{top_types}")
            pos = [e.text for e in
                   (opts.findall("tt:PositionOption", NS) if opts is not None else [])]
            R.check(pos == ["UpperLeft", "UpperRight", "LowerLeft", "LowerRight",
                            "Custom"],
                    f"五个预定义位置一个不少、拼写一致：{pos}")

            tx = opts.find("tt:TextOption", NS) if opts is not None else None
            R.check(tx is not None, "TextOption 存在（支持 Text 就必须给）")
            if tx is not None:
                ttypes = [e.text for e in tx.findall("tt:Type", NS)]
                R.check(set(ttypes) == {"Plain", "Date", "Time", "DateAndTime"},
                        f"TextOption 的四个类型字面量：{ttypes}")
                fsr = tx.find("tt:FontSizeRange", NS)
                fs_lo = fsr.findtext("tt:Min", "", NS) if fsr is not None else ""
                fs_hi = fsr.findtext("tt:Max", "", NS) if fsr is not None else ""
                R.check(fs_lo.isdigit() and fs_hi.isdigit(),
                        f"FontSizeRange 的 Min/Max 是数字：{fs_lo}..{fs_hi}")
                dfs = [e.text or "" for e in tx.findall("tt:DateFormat", NS)]
                tfs = [e.text or "" for e in tx.findall("tt:TimeFormat", NS)]
                R.check(dfs and all(dfs), f"DateFormat 列表非空且无空串：{dfs}")
                R.check(tfs and all(tfs), f"TimeFormat 列表非空且无空串：{tfs}")
                # **报出去的格式串，我们自己得真的认。** 认不出的后果是 NVR 选
                # 了一个、我们静默保持原样 —— 用户看到的是"在 NVR 上改了日期
                # 格式，画面上没变"，而两边的日志都很干净。
                toks = set(re.findall(r"[A-Za-z]+", " ".join(dfs + tfs)))
                R.check(toks and toks <= {"yyyy", "yy", "MM", "dd", "HH", "mm", "ss"},
                        f"报出去的格式串只用了我们认得的记号：{sorted(toks)}")
                # 11e 会**真的**拿 `dd/MM/yyyy` 和 `HH:mm` 走一个往返，所以
                # 这一条不是纸面声明 —— 广告里的格式至少有两个被真实解析过。
                R.check("dd/MM/yyyy" in dfs and "HH:mm" in tfs,
                        "广告里的 dd/MM/yyyy 与 HH:mm 在列表里（11e 会实测它们）")

        # -- 11c. GetOSDs：列表项叫 OSDs，子元素名/命名空间/顺序一起钉死 ----
        def get_osds():
            st_, data_ = media_authed(envelope(op_body(
                "GetOSDs", NS["trt"],
                "<ConfigurationToken>VideoSourceConfig</ConfigurationToken>")))
            if st_ != 200:
                return None, st_, data_
            try:
                r_ = ET.fromstring(data_)
            except ET.ParseError:
                return None, st_, data_
            return r_.findall(".//trt:OSDs", NS), st_, data_

        osds, st, data = get_osds()
        R.check(st == 200 and osds is not None, f"GetOSDs → HTTP {st}", data[:300])
        if osds is not None:
            R.check(len(osds) == len(items0),
                    f"项数和配置里的一致：{len(osds)} 项（配置 {len(items0)} 项）")
            R.check(all(o.get("token") == f"OSD_{i}" for i, o in enumerate(osds)),
                    f"token 是 OSD_<下标>：{[o.get('token') for o in osds]}")
            # 报出去的源 token 必须和 `GetVideoSourceConfigurations` 里那一个
            # **同一个**：对不上的话 NVR 会认为这项水印挂在一个不存在的视频源
            # 上，回读时把它丢掉 —— 而它的 OSD 页面只是"少了一项"。
            vts = {o.findtext("tt:VideoSourceConfigurationToken", None, NS)
                   for o in osds}
            R.check(vts == {"VideoSourceConfig"},
                    f"每一项的 VideoSourceConfigurationToken 都是 VideoSourceConfig：{vts}")
            if osds:
                o0 = osds[0]
                got_children = [ch.tag for ch in o0]
                R.check(got_children ==
                        [f"{{{NS['tt']}}}VideoSourceConfigurationToken",
                         f"{{{NS['tt']}}}Type",
                         f"{{{NS['tt']}}}Position",
                         f"{{{NS['tt']}}}TextString"],
                        "子元素名/命名空间/顺序逐字对齐 onvif.xsd："
                        f"{[c.split('}')[-1] for c in got_children]}")
                t0 = o0.findtext("tt:Type", None, NS)
                R.check(t0 == "Text", f"外层 Type = {t0}（枚举只有 Text/Image/Extended）")
                p0 = o0.findtext("tt:Position/tt:Type", None, NS)
                R.check(p0 in ("UpperLeft", "UpperRight", "LowerLeft", "LowerRight",
                               "Custom"),
                        f"Position/Type 是规范里那五个字面量之一：{p0}")
                ts0 = o0.findtext("tt:TextString/tt:Type", None, NS)
                R.check(ts0 in ("Plain", "Date", "Time", "DateAndTime"),
                        f"TextString/Type 是规范里那四个之一：{ts0}")
            # 时间项两个格式都得给（规范对 DateAndTime 是 "shall be present"）
            for i, o in enumerate(osds):
                if o.findtext("tt:TextString/tt:Type", None, NS) in (
                        "Date", "Time", "DateAndTime"):
                    R.check(
                        o.findtext("tt:TextString/tt:DateFormat", None, NS) is not None
                        and o.findtext("tt:TextString/tt:TimeFormat", None, NS) is not None,
                        f"第 {i} 项（时间）DateFormat 与 TimeFormat 都在")
            # **名称项的 PlainText 必须等于当前设备名。** 这是"水印上那个名字"
            # 与"NVR 读到的名字"同源那一条在协议层的证据（两者都来自
            # `DeviceState::channel_name()`）；不同源的话用户会看到两个名字。
            for i, o in enumerate(osds):
                if i < len(items0) and items0[i].get("type") == "name":
                    got_pt = o.findtext("tt:TextString/tt:PlainText", None, NS)
                    R.check(got_pt == dev0,
                            f"第 {i} 项（名称）的 PlainText = {got_pt!r}"
                            f"（设备名 {dev0!r}）")

        # -- 11d. GetOSD：单项的元素叫 OSD（不是 OSDs）----------------------
        if items0:
            st, data = media_authed(envelope(op_body(
                "GetOSD", NS["trt"], "<OSDToken>OSD_0</OSDToken>")))
            R.check(st == 200, f"GetOSD OSD_0 → HTTP {st}", data[:300])
            root = R.strict_xml(data, "GetOSD")
            if root is not None:
                R.check(root.find(".//trt:OSD", NS) is not None,
                        "GetOSDResponse 里的元素叫 OSD")
                R.check(root.find(".//trt:OSDs", NS) is None,
                        "……而且没有多出一个 OSDs（两个名字都是 schema 定的）")
        else:
            print("  [note] 配置里一项水印都没有，11d/11e 跳过")

        # -- 11e. SetOSD：位置、格式、字号的往返 -----------------------------
        # 四样各验一遍，因为它们的换算全是**独立**的、各自都能单独写错：
        # 归一化坐标（0..1）↔ 百分比、ONVIF 的 .NET 记号 ↔ strftime、
        # pt ↔ px。任何一处写反，NVR 上那个数字都"看起来挺合理"。
        if items0:
            # --- 热生效的基线，必须在 SetOSD **之前**取 ----------------------
            #
            # 下面那条 `pending` 断言是**否定式**的（"osd.items 不在 pending 里"），
            # 而否定式断言必须配一个肯定式的伴 —— 不然"采集侧真的换上了"和
            # "这一整件事谁都没管"报的是同一句话（陷阱 91 的形状）。
            #
            # 伴就是 `/api/status` 的 `osd_hot_applied`：采集侧**每一次真正的**
            # 水印配置变更恰好 +1，与桌面动不动无关（`osd_rebuilt_frames` 才是
            # 看桌面动没动的那个，所以它不能拿来当断言，陷阱 87）。
            status_b = get_status() or {}
            hot_before = status_b.get("osd_hot_applied")
            ticks_before = status_b.get("ticks")
            # 改之前这一项长什么样。用来把"计数器没动"的两种成因分开：**真的
            # 没换上**，还是存盘的**本来就是这个值**（NVR 周期性重发同一份 OSD
            # 是常态，防抖按设计会吞掉它 —— 那时计数器不动是正确行为）。
            pre_items = ((get_cfg() or {}).get("osd") or {}).get("items") or []
            pre0 = pre_items[0] if pre_items else {}
            already = all(pre0.get(k) == want for k, want in (
                ("position", "custom"), ("x_pct", 25), ("y_pct", 75),
                ("date_format", "%d/%m/%Y"), ("time_format", "%H:%M"),
                ("font_size", 32)))

            # 报文建成一个变量，**下面 11e2 要原样再发一遍**（那一段验防抖）。
            # 抄一份字面量的话，将来只改一处就会出现"11e2 发的是一份**旧的**
            # 水印、于是存储那份和它不相等、防抖不触发"—— 那时 11e2 报红，
            # 而它报的是"防抖没做事"，与真正的原因（两份报文漂移了）无关。
            setosd_inner = (
                '<OSD token="OSD_0">'
                "<tt:VideoSourceConfigurationToken>VideoSourceConfig"
                "</tt:VideoSourceConfigurationToken>"
                "<tt:Type>Text</tt:Type>"
                "<tt:Position><tt:Type>Custom</tt:Type>"
                '<tt:Pos x="0.25" y="0.75"/></tt:Position>'
                "<tt:TextString><tt:Type>DateAndTime</tt:Type>"
                "<tt:DateFormat>dd/MM/yyyy</tt:DateFormat>"
                "<tt:TimeFormat>HH:mm</tt:TimeFormat>"
                "<tt:FontSize>24</tt:FontSize>"
                "</tt:TextString></OSD>")
            st, data = media_authed(envelope(op_body(
                "SetOSD", NS["trt"], setosd_inner)))
            R.check(st == 200, f"SetOSD（Custom 位置 + 两种格式 + 字号）→ HTTP {st}",
                    data[:300])
            cfg_e = get_cfg() or {}
            items_e = (cfg_e.get("osd") or {}).get("items") or []
            it0 = items_e[0] if items_e else {}
            R.check(it0.get("position") == "custom",
                    f"位置落到配置：{it0.get('position')}")
            R.check(it0.get("x_pct") == 25 and it0.get("y_pct") == 75,
                    f"归一化坐标 0.25/0.75 → 百分比 25/75："
                    f"{it0.get('x_pct')}/{it0.get('y_pct')}")
            R.check(it0.get("type") == "time",
                    f"DateAndTime 仍然是时间项：{it0.get('type')}")
            R.check(it0.get("date_format") == "%d/%m/%Y",
                    f"DateFormat dd/MM/yyyy → strftime %d/%m/%Y："
                    f"{it0.get('date_format')}")
            # `mm` 在小写时是**分钟**（.NET 里月份是大写 MM）—— 这一条就是
            # 专门盯着它的：写反的话这里会变成 %m（月份），而画面上照样在走。
            R.check(it0.get("time_format") == "%H:%M",
                    f"TimeFormat HH:mm → strftime %H:%M：{it0.get('time_format')}")
            R.check(it0.get("font_size") == 32,
                    f"FontSize 24pt → 32px（96/72）：{it0.get('font_size')}")
            pend_e = cfg_e.get("pending") or []
            # ⚠️ **这一条从前断言的恰好相反**（`"osd.items" in pend_e`），而那时
            # 它是对的：画面上的水印用的是采集启动时那份快照
            # （`SessionOptions::osd` 在 `run()` 顶上取一次），改它必须重建整条
            # 管线。Phase S 把水印改成**热生效**（`LiveOsd`：采集循环每拍取当期
            # 那一份，`osd_force_redraw` 负责让静止的桌面也重合成一次），于是
            # 存盘的那份和运行侧那份**当场就一致**，`osd.*` 自然不再进 pending。
            #
            # 留着旧断言的话，它报出来的是"水印改动进了 pending"，读起来像
            # 产品坏了 —— 所以这里把它翻过来，并把原因写在这儿，免得下一个
            # 看到 diff 的人把它当成一次回归又改回去。
            R.check("osd.items" not in pend_e,
                    f"水印改动**不**进 pending（热生效，下一帧就变）：{pend_e}")

            # ……但"不在 pending 里"这件事**单独看是空的**：`applied`/`pending`
            # 由调用方写 `running_cfg.osd` 决定，而"写了 `running_cfg` 却忘了
            # 让采集侧换上"完全写得出来 —— 那时界面照样说已生效，画面纹丝不动
            # （陷阱 91：否定式的断言必须有一个肯定式的伴）。
            #
            # 采样有上限，而且"没涨"要能分清是哪一种：采集会话**根本没在跑**时
            # 这条**明说跳过**，不判 —— 那是在拿"这台机器此刻在不在录"当判据
            # （陷阱 87 的形状）。
            hot_after = hot_before
            ticks_moved = False
            for _ in range(12):
                time.sleep(0.25)
                s_now = get_status()
                if s_now is None:
                    continue
                hot_after = s_now.get("osd_hot_applied")
                if isinstance(ticks_before, int) and isinstance(s_now.get("ticks"), int):
                    ticks_moved = ticks_moved or s_now["ticks"] > ticks_before
                if isinstance(hot_before, int) and hot_after == hot_before + 1:
                    break

            if hot_before is None or hot_after is None:
                print("  [note] /api/status 里没有 osd_hot_applied，"
                      "热生效那条跳过")
            elif hot_after == hot_before + 1:
                R.check(True, f"采集侧真的换上了新水印（osd_hot_applied "
                              f"{hot_before} → {hot_after}）")
            elif already:
                print("  [note] 改之前存盘的就是这一份，防抖按设计吞掉了它 —— "
                      "热生效那条跳过（想验它得先把它改成别的值）")
            elif not ticks_moved:
                print(f"  [note] 采集会话没在跑（ticks 一直是 {ticks_before}），"
                      f"热生效那条跳过")
            else:
                R.check(False,
                        f"画面上的水印没有换上新配置：osd_hot_applied 停在 "
                        f"{hot_after}（基线 {hot_before}），而采集在跑"
                        f"（ticks 从 {ticks_before} 涨过了）",
                        "写 `running_cfg.osd` 与 `live_osd.set()` 是两件事，"
                        "漏了后者就是这里不涨；也可能是防抖判据写反了")

            # **回读一次。** 只查配置文件的话，一个"把请求收下、写进配置、
            # 却每次 GetOSDs 都现编一份"的实现照样能过 —— 而 NVR 永远读不到
            # 自己刚设的值。
            osds_e, st_e, _ = get_osds()
            if osds_e:
                e0 = osds_e[0]
                got_p = e0.findtext("tt:Position/tt:Type", None, NS)
                R.check(got_p == "Custom",
                        f"回读的 Position/Type = {got_p}")
                pos_el = e0.find("tt:Position/tt:Pos", NS)
                gx = pos_el.get("x") if pos_el is not None else None
                gy = pos_el.get("y") if pos_el is not None else None
                R.check(gx == "0.250000" and gy == "0.750000",
                        f"回读的 Pos = x={gx} y={gy}（固定六位小数）")
                got_df = e0.findtext("tt:TextString/tt:DateFormat", None, NS)
                got_tf = e0.findtext("tt:TextString/tt:TimeFormat", None, NS)
                R.check(got_df == "dd/MM/yyyy" and got_tf == "HH:mm",
                        f"回读的格式 = {got_df} / {got_tf}")
                got_fs = e0.findtext("tt:TextString/tt:FontSize", None, NS)
                R.check(got_fs == "24",
                        f"回读的 FontSize 是 pt（32px → 24pt）：{got_fs}")
                # 时间项**不带** `PlainText`：带上的话 NVR 那个编辑框里会多出
                # 一段文字，而有些实现会把它当成"这一项是纯文字"进而丢掉格式。
                R.check(e0.find("tt:TextString/tt:PlainText", NS) is None,
                        "时间项不带 PlainText")
            else:
                R.check(False, f"改完再读 GetOSDs 拿到 {st_e} / 空列表", data[:300])

        # -- 11e2. 重复下发**同一份** OSD：防抖必须吞掉它 --------------------
        #
        # **这一段的唯一理由是「防抖整条坏掉时，11a~11j 一条都不会红」。**
        # 防抖是**消去工作量**的行为：把 `on_osd_patch` 顶上那句
        # `if (osd_to_json(c) == osd_to_json(cfg.osd)) return;` 删掉之后，
        # 改配置那条路照样工作 —— `SetOSD` 照样回成功、`GetOSDs` 回读照样是新值、
        # 画面照样当场变，只是**每一次都白写一次盘、白打一行日志**。
        # 而 NVR 周期性地重复下发同一份 OSD 是常态（`SetOSD` 的语义就是
        # "设成这样"，它对"已经这样了"和"刚改成这样"不作区分），所以这不是
        # 洁癖：少了它，日志会被这一种请求淹没，「这次到底改了什么」再也看不出来。
        #
        # 判据只能落在"它有没有做事"上，而防抖省掉的事有**两样**：一次存盘和
        # 一行日志。**SOAP 响应、`GetOSDs` 回读、`/api/status` 在防抖开与关时
        # 一模一样** —— 连 `osd_hot_applied` 都不该动（采集侧比的是配置的
        # **文本**，同一份文本本来就不该涨；它在防抖关掉时涨了才是 bug）。
        # 所以这里读**配置文件**（`--config` 指到被测进程那一份）。
        #
        # 为什么不判日志（这一段原先写的正是日志）：**裸形态根本没有日志文件。**
        # `Log::set_file` 只在 `cmd_console` 里调（`main.cpp:2778`），而
        # `--rtsp --http` 走的是 `cmd_run(..., RunForm::Bare)`，那条路自己的
        # 注释写着"没有单实例锁、没有日志文件"（`main.cpp:2935-2938`）——
        # 也就是 `tests\rtsp_sub_e2e.ps1` 用的那个形态。参数推导看着完全正确
        # （`--config` 所在目录下的 `desk2onvif.log`），**但它从来没被跑过一次**：
        # 第一次真跑就是"文件不存在"。（"我推出来的路径"和"产品真的写在哪儿"
        # 是两件事 —— 陷阱 85 的纪律在这一处的用法是"判据依赖的每一个事实都要
        # 当场量一遍"。）
        #
        # 配置文件则是**无条件**写的：`on_osd_patch` → `apply_config` →
        # `save_config`（`main.cpp:1859`），而那一步在**任何 diff 之前**；而
        # `save_config` 是"写 .tmp 再改名覆盖"（`config.cpp:141-173`），目标
        # 文件每次都换一个新文件 ⇒ 时间和 inode 都会变。这比日志更强：它量的
        # 就是那句"白写一次盘"本身，不是它的一个副产物。
        #
        # ⚠️ 路径必须由调用方显式给出（`--config`）：探针猜不出被测进程用的是
        # 哪一份配置，**猜错**的话 `os.stat` 抛异常（而不是返回一个像"没动过"
        # 的值）—— 这是有意的，见 `cfg_stamp` 的注释。缺省时明说跳过，不判。
        #
        # ⚠️ 而且**否定式断言必须配一个肯定式的伴**（陷阱 91）：上面那条
        # "没动"在"路径指了个根本不在这儿被写的文件"时同样为真。伴就是先发一份
        # **不一样**的 OSD，那一下时间戳**必须**往前走 —— 它同时校准了三件事：
        # 路径是对的、这个文件确实是这条路的产物、比对是活的。
        if items0 and cfg_path:
            # 换一个位置、其余一字不动。**从 11e 那份报文改出来**，不另抄一份：
            # 抄的话两份会漂移，而漂移的症状是"11e2 拿一份旧水印去比，比不等，
            # 于是防抖不触发"—— 报出来是"防抖没做事"，与真正的原因无关。
            other_inner = setosd_inner.replace('x="0.25" y="0.75"',
                                               'x="0.50" y="0.50"')
            R.check(other_inner != setosd_inner,
                    "11e2 用来做校准的那份报文与 11e 那份**确实不同**"
                    "（相同的话下面那条肯定式断言恒假）")

            s0 = cfg_stamp()
            st, data = media_authed(envelope(op_body(
                "SetOSD", NS["trt"], other_inner)))
            R.check(st == 200, f"11e2 校准：改一个不一样的位置 → HTTP {st}",
                    data[:300])
            time.sleep(0.5)
            s1 = cfg_stamp()
            R.check(s1[0] > s0[0],
                    f"校准：真改了的时候配置文件**确实被重写了一遍**"
                    f"（mtime_ns {s0[0]} → {s1[0]}）—— 这一条证明那把尺子是活的",
                    f"{cfg_path!r} 的修改时间没有往前走。要么 --config 指错了"
                    f"文件（那个文件不是这条路写的），要么 `save_config` 被挪到"
                    f"了 diff 之后，要么防抖把**真的改动**也吞了")

            st, data = media_authed(envelope(op_body(
                "SetOSD", NS["trt"], other_inner)))     # 与上一步**完全同一份**
            R.check(st == 200, f"11e2 重复下发同一份 → HTTP {st}", data[:300])
            time.sleep(0.5)
            s2 = cfg_stamp()
            R.check(s2 == s1,
                    f"重复下发同一份 OSD：配置文件一动没动（防抖）"
                    f"（{s1} → {s2}）",
                    "配置文件的 mtime/size/inode 变了，说明那句 `osd_to_json` "
                    "的比较没起作用 —— NVR 每次周期性重发都会白写一次盘、"
                    "白打一行日志")

            # 复原成 11e 那个位置。（这一次**故意不判**：它就是一次真改动，
            # 上面那条校准已经证明尺子是活的，再来一条只是同一件事的复读。）
            st, _ = media_authed(envelope(op_body(
                "SetOSD", NS["trt"], setosd_inner)))
            R.check(st == 200, f"11e2 复原到 11e 的位置 → HTTP {st}")
        elif items0:
            print("  [note] 没给 --config，11e2（防抖）跳过 —— 这条判据要读"
                  "被测进程那份配置文件的时间戳，路径猜不得（猜错了它当场抛，"
                  "而一个像「没动过」的返回值会让它恒绿）")

        # -- 11f. 名称项的回声：NVR 读一遍写回来不能把名称冻住 --------------
        # NVR 的 OSD 编辑页面常常把整项读回来再整项写回。它读到的 `PlainText`
        # 就是我们报出去的那个**当前设备名** —— 当真改名的话，水印上的名字会
        # 被冻死在 NVR 读到的那个瞬间，而"改了一次名之后水印再也不跟着变"
        # 没有人会往这里想。
        #
        # 下标**现读一次**，不用段首那份 `items0`：11e 刚把第 0 项改成了时间项，
        # 而"哪一项是名称项"正是这一段的自变量 —— 拿旧快照在出厂值恰好是
        # [time, name] 时碰巧对，换成别的顺序就成了指着产品的假失败。
        items_now = ((get_cfg() or {}).get("osd") or {}).get("items") or []
        name_idx = next((i for i, it in enumerate(items_now)
                         if it.get("type") == "name"), None)

        def set_osd_plain(idx, text):
            return envelope(op_body(
                "SetOSD", NS["trt"],
                f'<OSD token="OSD_{idx}">'
                "<tt:VideoSourceConfigurationToken>VideoSourceConfig"
                "</tt:VideoSourceConfigurationToken>"
                "<tt:Type>Text</tt:Type>"
                "<tt:Position><tt:Type>LowerRight</tt:Type></tt:Position>"
                "<tt:TextString><tt:Type>Plain</tt:Type>"
                f"<tt:PlainText>{text}</tt:PlainText>"
                "</tt:TextString></OSD>"))

        if name_idx is not None:
            st, data = media_authed(set_osd_plain(name_idx, dev0))
            R.check(st == 200, f"把名称项原样写回（PlainText={dev0!r}）→ HTTP {st}",
                    data[:300])
            it_n = ((get_cfg() or {}).get("osd") or {}).get("items") or []
            it_n = it_n[name_idx] if name_idx < len(it_n) else {}
            R.check(it_n.get("type") == "name",
                    f"回声之后它仍然是 name 项：{it_n.get('type')}"
                    "（冻成 text 的话水印上的名字再也不会跟着改）")
            # 反向：给一个**真的不一样**的文字，那就该变成静态 text 项 ——
            # 只做"永远是 name"这一半的话，用户就再也加不了固定文字了。
            st, data = media_authed(set_osd_plain(name_idx, "D2O-STATIC"))
            R.check(st == 200, f"给名称项写一段固定文字 → HTTP {st}", data[:300])
            it_t = ((get_cfg() or {}).get("osd") or {}).get("items") or []
            it_t = it_t[name_idx] if name_idx < len(it_t) else {}
            R.check(it_t.get("type") == "text" and it_t.get("text") == "D2O-STATIC",
                    f"变成了静态文字项：type={it_t.get('type')} "
                    f"text={it_t.get('text')!r}")
        else:
            print("  [note] 配置里没有 name 项，11f 跳过")

        # -- 11g. 认不出的 token → 可见的 Fault -----------------------------
        # token 是**位置派生**的（OSD_<下标>），所以删掉一项之后，一个缓存着
        # 旧 token 的 NVR 会拿到越界。这里要的正是"可见地失败"，而不是被
        # 悄悄夹到某一项上改掉**另一项**水印 —— 后者在用户那边是"我改的是
        # 右下角，怎么左上角的字变了"。
        st, data = media_authed(envelope(op_body(
            "GetOSD", NS["trt"], "<OSDToken>OSD_99</OSDToken>")))
        expect_fault(st, data, "GetOSD 用一个越界的 token",
                     "ter:InvalidArgVal", "ter:NoConfig")

        st, data = media_authed(envelope(op_body(
            "DeleteOSD", NS["trt"], "<OSDToken>OSD_99</OSDToken>")))
        expect_fault(st, data, "DeleteOSD 用一个越界的 token",
                     "ter:InvalidArgVal", "ter:NoConfig")

        st, data = media_authed(envelope(op_body(
            "SetOSD", NS["trt"],
            '<OSD token="1"><tt:Type>Text</tt:Type></OSD>')))
        expect_fault(st, data, "SetOSD 用一个不认识的 token（数字 1）",
                     "ter:InvalidArgVal", "ter:NoConfig")
        R.check(len(((get_cfg() or {}).get("osd") or {}).get("items") or [])
                == len(items0),
                "三次失败的请求一项水印都没动")

        # -- 11h. CreateOSD：到上限为止，再建必须失败 ------------------------
        def osd_new_body():
            return envelope(op_body(
                "CreateOSD", NS["trt"],
                "<OSD><tt:VideoSourceConfigurationToken>VideoSourceConfig"
                "</tt:VideoSourceConfigurationToken>"
                "<tt:Type>Text</tt:Type>"
                "<tt:Position><tt:Type>LowerLeft</tt:Type></tt:Position>"
                "<tt:TextString><tt:Type>Plain</tt:Type>"
                "<tt:PlainText>D2O-ITEM</tt:PlainText>"
                "</tt:TextString></OSD>"))

        limit = max_total if max_total > 0 else 4
        n0 = len(items0)
        created = []
        for k in range(max(0, limit - n0)):
            st, data = media_authed(osd_new_body())
            R.check(st == 200, f"CreateOSD 第 {k + 1} 个 → HTTP {st}", data[:300])
            if st != 200:
                break
            root = R.strict_xml(data, "CreateOSD")
            tok = (root.findtext(".//trt:OSDToken", None, NS)
                   if root is not None else None)
            # **`OSDToken` 是 `trt:` 的**，不是 `tt:` 的 —— 它的*类型*是
            # `tt:ReferenceToken`，但元素声明在 media.wsdl 里。写成 `tt:` 的话
            # 这一条会返回 None，而客户端那边只是"新建了但不知道 token"。
            R.check(tok == f"OSD_{n0 + k}",
                    f"返回的 OSDToken = {tok}（新项挂在列表末尾，token 是 OSD_{n0 + k}）")
            if tok:
                created.append(tok)

        cfg_h = get_cfg() or {}
        items_h = (cfg_h.get("osd") or {}).get("items") or []
        R.check(len(items_h) == max(n0, limit),
                f"水印项数到了上限：{len(items_h)}（上限 {limit}）")
        if n0 < len(items_h):
            R.check(items_h[n0].get("type") == "text"
                    and items_h[n0].get("text") == "D2O-ITEM",
                    f"新建项落成了静态文字：type={items_h[n0].get('type')} "
                    f"text={items_h[n0].get('text')!r}")
            R.check(items_h[n0].get("position") == "lower_left",
                    f"新建项的位置：{items_h[n0].get('position')}")

        st, data = media_authed(osd_new_body())
        expect_fault(st, data, "超过上限还建", "ter:InvalidArgVal", "ter:NoConfig")
        R.check(len(((get_cfg() or {}).get("osd") or {}).get("items") or [])
                == max(n0, limit),
                "失败的那一次没有在水印列表里留下东西"
                "（先 push 后校验的实现会留下一个画不出来的空项）")

        # -- 11i. DeleteOSD -------------------------------------------------
        # **倒着删。** token 是下标派生的，删掉中间那一项会让它后面所有项
        # 整体前移一格 —— 顺着删的话第二次就删错人了（拿到的是"删完之后
        # 那个下标上的另一项"）。这里如实按倒序走，顺带把这个性质写下来。
        for tok in reversed(created):
            st, data = media_authed(envelope(op_body(
                "DeleteOSD", NS["trt"], f"<OSDToken>{tok}</OSDToken>")))
            R.check(st == 200, f"DeleteOSD {tok} → HTTP {st}", data[:300])
        R.check(len(((get_cfg() or {}).get("osd") or {}).get("items") or []) == n0,
                f"删完之后回到 {n0} 项")
        if created:
            osds_i, st_i, _ = get_osds()
            R.check(osds_i is not None and len(osds_i) == n0,
                    f"删完之后 GetOSDs 也回到 {n0} 项："
                    f"{len(osds_i) if osds_i is not None else None}")
            # 位置派生 token 的代价，**如实断言**：刚删掉的那个 token 现在
            # 越界了。一个缓存着它的 NVR 会拿到一条可见的 Fault，而不是被
            # 静默改掉另一项 —— 这正是这个取舍要买的东西。
            last_tok = created[0]
            st, data = media_authed(envelope(op_body(
                "GetOSD", NS["trt"], f"<OSDToken>{last_tok}</OSDToken>")))
            expect_fault(st, data, f"刚删掉的 {last_tok} 现在越界",
                         "ter:InvalidArgVal", "ter:NoConfig")

        # -- 11j. 复原 -------------------------------------------------------
        # 这一段把水印改得面目全非（位置、格式、字号，还建删过几项）。配置
        # 文件可能是别处传进来的（--config），留着会让下一轮从一个奇怪的
        # 状态起步 —— 和 10g 同一个理由。
        st, _, b = c.post_json("/api/config", {"osd": osd0})
        R.check(st == 200, f"还原水印配置 → HTTP {st}", b[:200])
        items_r = ((get_cfg() or {}).get("osd") or {}).get("items") or []
        R.check(len(items_r) == len(items0),
                f"还原之后项数一致：{len(items_r)}（原本 {len(items0)}）")
        same = all(items_r[i].get("position") == items0[i].get("position")
                   and items_r[i].get("type") == items0[i].get("type")
                   for i in range(min(len(items_r), len(items0))))
        R.check(same, "还原之后每一项的位置与类型一致")
        osds_r, st_r, _ = get_osds()
        if osds_r is not None:
            R.check(len(osds_r) == len(items0),
                    f"还原之后 GetOSDs 报的也是 {len(items0)} 项"
                    f"（实际 {len(osds_r)}）")

    # ---- 12. 时间设置：NTP 真对一次，以及 NVR 那条路 ---------------------
    #
    # 这一段**自己起** `tests/ntp_probe.py`（一个纯标准库的假 SNTP 应答器），
    # 理由写在 `NtpProbe` 的类注释里：一条命令就能跑完，不需要夹具先起好 ——
    # 挂在夹具上的话，夹具哪天忘了或者改了参数，这一段会变成一条**恒绿的空转**
    # （探针不在 → 客户端超时 → "没发起查询"那一条照样成立），而它在报告上和
    # 「验过了」完全同形。
    #
    # 盖住四条边：NTP 正路（偏移**精确**等于 90）、手动模式下 NVR 那条路**不发**
    # 查询、NTP 模式下 NVR 那条路**真的发**查询、以及**对时失败**（黑洞探针 →
    # 确定性 timeout）。审计那半边留给 Phase V 的 `lifecycle_check.ps1` ——
    # 这一段不验它，也不假装验了。
    print("\n[12] 时间设置：NTP 对时（假 SNTP 服务器）+ NVR 触发那条路")

    app_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    def get_time():
        """取一次 `/api/time`。**每次现取，不缓存** —— 要读的正是会动的计数器
        （`sync_ok_count` / `last_sync_state`），缓一份等于把尺子冻住。"""
        st, _, b = c.get("/api/time")
        if st != 200:
            return None
        try:
            return json.loads(b)
        except ValueError:
            return None

    def wait_time(pred, timeout=20.0, step=0.25):
        """等到 `/api/time` 满足 `pred`。返回 (命中那一份 或 None, 最后读到的一份)。

        第二个返回值是给失败信息用的：只有 `None` 的话，报告上就少一句
        "它当时到底停在哪个状态"。
        """
        deadline = time.time() + timeout
        last = None
        while True:
            last = get_time()
            if last is not None and pred(last):
                return last, last
            if time.time() >= deadline:
                return None, last
            time.sleep(step)

    t_seen = get_time()
    if t_seen is None:
        R.check(False, "GET /api/time 拿不到，时间这一段没法验")
    else:
        R.check(isinstance(t_seen.get("offset_s"), int)
                and isinstance(t_seen.get("configured_offset_s"), int),
                "GET /api/time 报得出两个偏移（在用的 / 配置里的）："
                f"{t_seen.get('offset_s')!r} / "
                f"{t_seen.get('configured_offset_s')!r}",
                "这两个数**刻意是两个字段**：NTP 模式下它们本来就该不一样")
        mode0 = t_seen.get("mode")
        cfg_off0 = t_seen.get("configured_offset_s") or 0
        # 复原用的那一份配置。取自 `/api/time` 而不是 `GET /api/config`：
        # 前者报的就是这四个数（而且**永远**有），不必再去猜配置里缺没缺键。
        restore = {
            "mode": mode0,
            "ntp_server": t_seen.get("ntp_server"),
            "ntp_interval_s": t_seen.get("ntp_interval_s"),
            "offset_s": cfg_off0,
        }

        probe = NtpProbe(app_root)
        started, why = probe.start(90)
        if not started:
            # 绑不上 127.0.0.1:123 是**环境**，不是产品（陷阱 87）：这台机器上
            # 多数时候是空的，但真被别的 NTP 服务占着时红的是环境。
            R.skip("[12] NTP 的正路 / 手动不发查询 / NTP 触发 / 超时",
                   "假 SNTP 服务器起不来（127.0.0.1:123 被占？）：\n" + why)
        else:
            try:
                # -- 12a. 先把服务器指向探针，并切到手动 --------------------
                #
                # **这一步的顺序是有意义的**，不只是一个前置设置。要验的是
                # 「手动模式下不发查询」，如果 `ntp_server` 还是一个解析不出来的
                # 名字，那这条断言就是**对的结论、错的原因** —— 客户端连包都发不
                # 出去（`unreachable`），探针当然一个也收不到，于是"手动模式的
                # 守卫生效了"和"名字没解析出来"报的是同一个读数（陷阱 91 那一族
                # 的另一种形状）。先把服务器指向一个**确实能收**的地址，这个
                # 否定式断言才有内容。
                st, _, b = c.post_json("/api/config", {"time": {
                    "mode": "manual", "ntp_server": "127.0.0.1",
                    "ntp_interval_s": 3600}})
                R.check(st == 200, f"把对时服务器指向探针、切到手动 → HTTP {st}",
                        b[:200])
                try:
                    _r = json.loads(b) or {}
                except ValueError:
                    _r = {}
                applied = _r.get("applied") or []
                pending = _r.get("pending") or []
                # ⚠️ **不要判「`time.mode` 在 applied 里」** —— 第一版就是这么写
                # 的，而它当场红了：`applied` 是**差分**（`diff_config(cfg, next)`），
                # 而这一次请求里 `mode` 的值本来就等于现值（全新进程的默认就是
                # `manual`），于是它压根不在差分里。那条断言验的其实是"这一次请求
                # 恰好改了 mode"，与"时间设置是不是热生效"毫无关系 —— 又是一条
                # 「怎么都绿/怎么都红，取决于一件与它名义无关的事」的判据。
                #
                # 真正的不变量是**否定式的**：`time.*` **一个都不许进 `pending`**。
                # 进了 `pending` 就意味着"要重启才生效"，而时间设置是热生效的
                # （`apply_config` 里 `running_cfg.time = next.time` 是无条件的）。
                # 同时用**这一个请求里真的变了的那个字段**（`ntp_server`：默认
                # 的 `ntp.aliyun.com` → `127.0.0.1`）当正面同伴 —— 否则
                # `applied` 恒空的话，"没有 time.* 进 pending"照样成立。
                R.check("time.ntp_server" in applied and not any(
                    k.startswith("time.") for k in pending),
                        f"时间设置是**当场生效**的：变了的字段进了 applied，"
                        f"且没有任何 time.* 进 pending"
                        f"（applied={applied}，pending={pending}）")
                t_a, _ = wait_time(lambda t: t.get("mode") == "manual"
                                   and t.get("ntp_server") == "127.0.0.1",
                                   timeout=5.0)
                R.check(t_a is not None,
                        "服务端报回来的模式与服务器跟着变了",
                        "读到的是 "
                        f"{(get_time() or {}).get('mode')!r} / "
                        f"{(get_time() or {}).get('ntp_server')!r}")

                # 进来的配置万一本来就是 NTP 模式（换模式那一下可能还有一次
                # 对时在飞），等它落下来再取基准 —— 否则基准自己会动，12b 那条
                # "计数器没变"就会随机假红。这属于陷阱 87：判据依赖了一个
                # 没写进前提的条件（"取基准时没有在飞的对时"）。
                time.sleep(1.5)
                tm0 = get_time() or {}
                pk0 = probe.requests
                ok0 = tm0.get("sync_ok_count")
                state0 = tm0.get("last_sync_state")

                # -- 12b. 手动模式下，NVR 的 DateTimeType=NTP 不发查询 -------
                st, data = set_time("<tds:DateTimeType>NTP</tds:DateTimeType>")
                R.check(st == 200, f"手动模式下 DateTimeType=NTP → HTTP {st}",
                        data[:200])
                time.sleep(2.0)
                pk1 = probe.requests
                tm1 = get_time() or {}
                R.check(pk1 == pk0,
                        f"探针一个包都没多收到（{pk0} → {pk1}）—— 手动模式下"
                        f"确实**没发起查询**",
                        f"探针最后几行：\n{probe.log(3)}")
                R.check(tm1.get("sync_ok_count") == ok0
                        and tm1.get("last_sync_state") == state0,
                        f"对时的计数器也没动（ok {ok0} → "
                        f"{tm1.get('sync_ok_count')}；state {state0!r} → "
                        f"{tm1.get('last_sync_state')!r}）")
                # 「并把原因写进日志」那半条**在这里验不到**，如实写下来：
                # 本文件打的是 `--rtsp --http` 的**裸形态**，而 `Log::set_file`
                # 只在 `cmd_console` 里调 —— 那条路的注释白纸黑字写着"没有日志
                # 文件"。去读一个根本不存在的文件，最坏的结果是抛异常被咽掉、
                # 再报成一条看起来像"没写"的绿（Phase S 的 [11e2] 就是这么红
                # 过一次）。所以这里只验"没发查询"那件能验的事。

                # -- 12c. NTP 模式下真的对一次，偏移**精确等于** 90 ----------
                #
                # 只改 `mode`，**不调** `/api/time/sync`：`NtpSyncer::set_config`
                # 在手动→NTP 时就会立刻排一次对时（理由在那边写着：用户刚把开关
                # 打开，等一小时才有动静只会让他认为功能坏了）。少一个发起者，
                # 就少一处"这次查询到底是谁发的"的歧义。
                st, _, b = c.post_json("/api/config", {"time": {"mode": "ntp"}})
                R.check(st == 200, f"切到 NTP → HTTP {st}", b[:200])
                t_ok, t_last = wait_time(
                    lambda t: t.get("last_sync_state") == "ok", timeout=20.0)
                R.check(t_ok is not None, "对时状态变成 ok",
                        "最后一次读到："
                        + (json.dumps(t_last, ensure_ascii=False)
                           if t_last else "一次都没读到"))
                if t_ok is not None:
                    R.check(t_ok.get("last_sync_offset_s") == 90,
                            f"`last_sync_offset_s` 恰好是 90"
                            f"（实际 {t_ok.get('last_sync_offset_s')!r}）",
                            "判**精确相等**：纪元常数写错是 2208988800 秒"
                            "（整 25567 天）的差，而「偏移是个正数」这种断言"
                            "两个方向都放过")
                    R.check(t_ok.get("last_sync_stratum") == 2,
                            f"stratum 读回来了："
                            f"{t_ok.get('last_sync_stratum')!r}")
                    R.check(t_ok.get("offset_s") == 90,
                            f"设备时间真的被挪了 90 秒（offset_s = "
                            f"{t_ok.get('offset_s')!r}）",
                            "这一条是那个**向零截断**缺陷在链路末端的探测器："
                            "老写法下 `set_device_time(now()+90s)` 有 25% 的概率"
                            "落成 89。单次采样只有四分之一的机会撞上，所以确定性"
                            "的那个判据在 `tests/device_time_check.cpp`（同一档跑 "
                            "500 轮），这里是端到端复核一次")
                    R.check(t_ok.get("configured_offset_s") == cfg_off0,
                            f"配置里那个偏移**没被 NTP 改动**"
                            f"（configured_offset_s 仍是 {cfg_off0}）",
                            "NTP 得来的偏移不进配置是刻意的（见 ntp_syncer.h 的"
                            "文件头）：重启后要恢复的是「手动校准出来的那个」")
                    dn, hn = t_ok.get("device_now"), t_ok.get("host_now")
                    R.check(isinstance(dn, int) and isinstance(hn, int)
                            and abs((dn - hn) - 90) <= 1,
                            f"device_now − host_now = {dn - hn} 秒"
                            f"（与 offset_s 自洽）",
                            "这两个数由两段不同的代码算出来（一个是「主机现在」、"
                            "一个是「设备现在」），对不上就说明有一段没接上")
                    R.check(probe.requests > pk1,
                            f"探针确实收到了那次查询（{pk1} → "
                            f"{probe.requests}）—— 这是「ok 是从哪来的」的"
                            f"正面证据")

                # -- 12d. NTP 模式下，NVR 那条路真的发起一次查询 -------------
                #
                # 判"真的发了"用 `sync_ok_count` **递增**，而不是 `last_sync_at`
                # 变了：那个字段是**整秒**的，两次对时落在同一秒里它就纹丝不动，
                # 于是这条断言会随机假红（又是陷阱 87 的形状 —— 判据依赖了一个
                # 没写进前提的条件：两次对时至少隔一秒）。
                ok_before = (get_time() or {}).get("sync_ok_count")
                pk_before = probe.requests
                st, data = set_time("<tds:DateTimeType>NTP</tds:DateTimeType>")
                R.check(st == 200, f"NTP 模式下 DateTimeType=NTP → HTTP {st}",
                        data[:200])
                rt = R.strict_xml(data, "NTP 模式下的 SetSystemDateAndTime 响应")
                if rt is not None:
                    R.check(R.find(rt, ".//soap:Fault") is None,
                            "它不是 Fault（`Set*` 回 Fault 会让海康的添加向导"
                            "停在时间那一步）")
                    names = [el.tag.split("}")[-1] for el in rt.iter()]
                    R.check("SetSystemDateAndTimeResponse" in names,
                            f"响应体是 SetSystemDateAndTimeResponse：{names}")
                # ⚠️ **刻意不判「响应先于查询完成」**。`on_ntp_request` 只置一个位
                # + notify，SOAP 响应在任何实现下都是立刻回的 —— 量出来的两个数
                # 都会是几毫秒，那条断言怎么都绿（陷阱 85）。它想防的那件事
                # （在 handler 里同步等 UDP）由结构保证，不由这条测试保证。
                t_ok2, t_last2 = wait_time(
                    lambda t: (t.get("sync_ok_count") or 0) > (ok_before or 0),
                    timeout=20.0)
                R.check(t_ok2 is not None,
                        f"NVR 那一句真的触发了一次对时（ok 计数 {ok_before} → "
                        f"{(t_last2 or {}).get('sync_ok_count')}）",
                        "最后一次读到："
                        + (json.dumps(t_last2, ensure_ascii=False)
                           if t_last2 else "一次都没读到"))
                R.check(probe.requests > pk_before,
                        f"探针也收到了那一次查询（{pk_before} → "
                        f"{probe.requests}）—— 两条路各说一遍同一件事：计数的"
                        f"那个可能被别的路径推上去，包数是不会的")

                # -- 12e. DateTimeType 是模式驱动的 ---------------------------
                now_t = read_device_time()
                if R.check(now_t is not None and now_t[3] is not None,
                           "再读一次 GetSystemDateAndTime"):
                    R.check(now_t[3] == "NTP",
                            f"DateTimeType 报 {now_t[3]}（mode=ntp 时这才是诚实的）",
                            "第 [1] 段判的是 `Manual`，两处合起来才说明这个字段是"
                            "**跟着模式走**的，而不是写死了一个值")

                # -- 12f. 对时失败：黑洞探针 → 确定性 timeout -----------------
                #
                # **"根本不起服务器"造不出 timeout。** 环回上没人听的 UDP 端口会
                # 把 ICMP port unreachable 打回来（`WSAECONNRESET`），客户端把它
                # 归成 `unreachable` —— 也就是安静地量到了**另一条分支**，而报告
                # 上看起来像"我把失败验过了"。要造 timeout 就得让包**收得到、
                # 回不了**，那正是 `--silent`。
                probe.stop()
                started2, why2 = probe.start(90, silent=True)
                if not started2:
                    R.skip("[12] 对时失败那一条（黑洞探针）",
                           "黑洞探针起不来：\n" + why2)
                else:
                    pk_b2 = probe.requests
                    st, _, b = c.post_json("/api/time/sync", {})
                    R.check(st == 202,
                            f"POST /api/time/sync → HTTP {st}（回 202：真正的 UDP "
                            f"查询在后台线程上，不能占住 handler）", b[:200])
                    t_to, t_last3 = wait_time(
                        lambda t: t.get("last_sync_state") == "timeout",
                        timeout=20.0)
                    R.check(t_to is not None,
                            f"对时状态变成 timeout（实际 "
                            f"{(t_last3 or {}).get('last_sync_state')!r}）",
                            "最后一次读到："
                            + (json.dumps(t_last3, ensure_ascii=False)
                               if t_last3 else "一次都没读到"))
                    # **正面同伴**：探针确实收到了包。只判 timeout 的话，"一个包
                    # 都没发出去"和"发了、没人理"报的是同一个读数 —— 而这两件事
                    # 的含义正好相反（前者是本机的路由/DNS 问题，后者才是对时失败）。
                    R.check(probe.requests > pk_b2,
                            f"黑洞探针确实收到了查询（{pk_b2} → "
                            f"{probe.requests}）—— 所以这个 timeout 是"
                            f"「发出去了、没回音」，不是「根本没发」")
                    R.check(bool((t_last3 or {}).get("last_sync_error")),
                            f"带上了一句人话的失败原因："
                            f"{(t_last3 or {}).get('last_sync_error')!r}",
                            "UDP 到公网 123 在没有网络的环境里是**静默丢弃**的，"
                            "所以失败原因必须自己说出来，不能指望「没反应」本身")
            finally:
                probe.stop()

        # -- 12g. 网页手动校准，然后复原 -------------------------------------
        #
        # 复原分两步，**顺序和绕的那一下都不能省**：
        #
        #  1. 先把整个 `time` 段还原。模式必须在这一步回来 —— `POST /api/time`
        #     在 NTP 模式下会被**有意**拒绝（NTP 模式下钟归对时线程管，手动设的
        #     值下一个周期就被覆盖，用户看到的是"设成功了、一小时后自己变回去"）。
        #  2. 再把**设备时钟**推回配置里那个值。
        #
        # 第 2 步为什么要绕一个中间值：`POST /api/time` 走的是 `apply_config`，
        # 而它**只在 `next.time.offset_s != cfg.time.offset_s` 时才写
        # `DeviceState`** —— 那个"没变就不写"是刻意的（否则随便存一次配置就会把
        # 状态页上的"最后同步时刻"往前顶一下）。而上一步刚把配置还原成
        # `want_off`，所以直接发 `want_off` 是一次"配置没变"的调用，设备那边被
        # NTP 挪走的 +90 会原样留着。先推到一个不同的值、再推回来，两次都是真的
        # 变更，两次都落到了 `DeviceState` 上。
        #
        # 顺带：这一段本身就是「网页手动校准」这条需求的端到端验收 ——
        # `POST /api/time {"offset_s": N}` 之后 `/api/time` 的 `offset_s` 必须
        # **精确**是 N，而不是 N−1（向零截断那个缺陷的又一处出口）。
        want_off = cfg_off0
        st, _, b = c.post_json("/api/config", {"time": restore})
        R.check(st == 200, f"还原 time 配置段 → HTTP {st}", b[:200])
        step = want_off + 7
        st, _, b = c.post_json("/api/time", {"offset_s": step})
        R.check(st == 200,
                f"手动校准：把设备时钟推到 {step:+d} 秒 → HTTP {st}", b[:200])
        t_s, _ = wait_time(lambda t: t.get("offset_s") == step, timeout=5.0)
        R.check(t_s is not None,
                f"设备时钟**精确**变成 {step:+d} 秒"
                f"（实际 {(get_time() or {}).get('offset_s')!r}）",
                "「差一秒」不是小事：水印上就是差一秒，而它会表现成"
                "「NVR 对完时比录像机慢一秒」")
        st, _, b = c.post_json("/api/time", {"offset_s": want_off})
        R.check(st == 200, f"再推回 {want_off:+d} 秒 → HTTP {st}", b[:200])
        t_end = get_time() or {}
        R.check(t_end.get("offset_s") == want_off,
                f"设备时钟回到配置里那个值：{t_end.get('offset_s')!r}"
                f"（配置说 {want_off}）",
                "只验配置的话，配置对了而设备还停在 +90 —— 那正是这一段开头"
                "说的那种不一致")
        R.check(t_end.get("configured_offset_s") == want_off
                and t_end.get("mode") == mode0,
                f"配置也回到了原样：mode={t_end.get('mode')!r} "
                f"（原本 {mode0!r}），configured_offset_s="
                f"{t_end.get('configured_offset_s')!r}")
        end_t = read_device_time()
        if R.check(end_t is not None and end_t[3] is not None,
                   "复原之后再读一次 GetSystemDateAndTime"):
            want_type = "NTP" if mode0 == "ntp" else "Manual"
            R.check(end_t[3] == want_type,
                    f"DateTimeType 回到 {end_t[3]}（原本是 {mode0!r} 模式，"
                    f"所以该报 {want_type}）")

    # ==================================================================
    # [13] 审计：NVR 那条路
    # ==================================================================
    #
    # `[12]` 的文件头写着「审计那半边留给 Phase V 的 lifecycle_check.ps1 ——
    # 这一段不验它，也不假装验了」。这一段就是那句话欠下的另一半，对应计划里
    # 那行验收：「审计 · NVR | onvif_soap.py：SetOSD / 改名 / 对时各断言审计里
    # 有对应一行 + NVR 的 IP」。`lifecycle_check.ps1` 盖的是**网页**那条路，
    # 这一份盖的是 **NVR** 那条路 —— 两条路写着两份代码（`web_server.cpp` 的
    # `AuditWho{req.peer_ip(), \"web:\" + sessions.user()}` 与 `onvif_service.cpp`
    # 的 `AuditWho{peer, \"onvif:\" + opt.user}`），所以两条都得验。
    #
    # ## 判据为什么是「涨了一行」，而不是「找得到一行」
    #
    # `note_audit`（`onvif_service.cpp:685-697`）有一道 **60 秒去重**闸门：
    # 同一个 (来源, 动作, 细节) 三元组一分钟内只写一次。而 `[10]` / `[11]` /
    # `[12]` **已经在同一次运行里**写过这三个动作各至少一次 —— 也就是说
    # 「文件里找得到一条 onvif.SetOSD」在这一段开始之前**就已经为真**了。
    # 那条断言于是怎么都会绿（陷阱 85），而它看起来和真断言一模一样。
    #
    # 所以三件事都做齐：
    #   (a) 用**全新的细节串**（去重闸门按细节串判，新串拦不住）；
    #   (b) 断言该细节的行数**从 N 涨到 N+1**（涨不动就是没写）；
    #   (c) 先跑一组正控制，把「这段日志根本没在写」和「这一段没写」分开 ——
    #       不然三条「没找到」会被报成三条产品缺陷，而真相是审计压根没开。
    #
    # ## 这些细节串不是猜的，是逐字读出来的
    #
    # `osd_item_detail`（`onvif_service.cpp:580-592`）、`patch_detail`
    # （`:603-618`）、`SetSystemDateAndTime` 那两行（`:1477-1479`）都在下面
    # 各写了一遍出处。**理由**：细节串写错的话症状是「审计没记」，而归因会
    # 指向产品 —— 实际坏的是尺子（陷阱 46 那一族）。
    #
    # ## 时间戳**不与设备时钟对账**
    #
    # `AuditLog::local_now`（`audit_log.cpp:64-73`）用的是 `std::time`，也就是
    # **主机时间**；而 `[13c]` 正把设备时钟推到 +4321 秒。拿审计行上的时刻去比
    # `GetSystemDateAndTime` 会红成一个完全莫名其妙的形状，所以这一段里一次
    # 都没那么比。
    print("\n[13] 审计：NVR 改了什么，日志里得有对应一行 + NVR 的 IP")

    def audit_path_today():
        """今天那个审计文件的路径。

        ## 文件名**自己拼**，不列目录

        `AuditLog::read()`（`audit_log.cpp:425-469`）是**算术地**拼出文件名再
        逐个试的，从不 `directory_iterator`（:422-424 那行注释写着这件事）。
        这里跟着它拼，两边认的必然是同一个文件；列目录再挑的话，哪天前缀规则
        变了，两边会各自挑到不同的文件而**都不报错**。
        名字规格（`audit_log.cpp:114-122`）：`audit-` + 本地日期 + `.log`，
        **恰好 20 个字符** —— 所以日期必须是定长的 `%Y-%m-%d` 写法。

        ## 目录的规则

        `main.cpp:1919-1922`：`audit_dir = (配置文件所在目录 为空 ? "." : 它)
        / "logs"`。所以 `--config out/x/cfg.json` 时日志在 `out/x/logs/`，
        与 `desk2onvif.log` 的父目录是同一个 —— 用户找得到的那一份。
        """
        day = _dt.date.today().strftime("%Y-%m-%d")
        return os.path.join(os.path.dirname(os.path.abspath(cfg_path)),
                            "logs", "audit-" + day + ".log")

    def audit_rows_now():
        """读今天的审计文件，切成五元组 `(时刻, 来源, 身份, 动作, 细节)`。

        **读不出来就抛** —— 由 `audit_read` 接住，见那边为什么不让它往外抛。
        """
        # **严格 UTF-8，不给 `errors=`**：解码失败就该红。静默替换成 U+FFFD
        # 之后每一条内容断言都变成「找不到」，而归因会指向产品。
        with open(audit_path_today(), encoding="utf-8") as f:
            text = f.read()
        rows = []
        bad = []
        for line in text.splitlines():
            line = line.rstrip("\r")
            if not line:
                continue
            # 解析规则**照抄 `split_line`**（`audit_log.cpp:78-99`）：按
            # 「空格 竖线 空格」切四刀、必须恰好五段、时间戳至少 19 个字符。
            # 用裸竖线切的话，细节里恰好含一个竖线时会多切一刀 —— 而
            # `sanitize()`（:126-141）正是为那件事把竖线换成斜杠的。
            parts = line.split(" | ", 4)
            if len(parts) != 5 or len(parts[0]) < 19:
                bad.append(line)
                continue
            rows.append(tuple(parts))
        return rows, bad

    def audit_read():
        """读一次，**不抛**：返回 `(rows, bad, err)`，`err` 非空时前两个是 None。

        为什么不让它抛出去：读失败是**一条红断言**，不是「这份探针坏了」。
        往外抛会把 `[13]` 剩下的东西一起带走，而在夹具层
        （`$ErrorActionPreference = 'Stop'`）那个 traceback 会变成一句
        `NativeCommandError` 把整个 e2e 也杀掉 —— `[1b]` 的变异检验正是这么
        崩过一次（那一段的注释与陷阱 91 记着这个形状）。**一条断言失败和一份
        探针崩掉，在报告上是两回事。**
        """
        try:
            rows, bad = audit_rows_now()
            return rows, bad, None
        except Exception as e:
            return None, None, e

    def n_of(rows, action, pred):
        return sum(1 for r in rows if r[3] == action and pred(r[4]))

    def wait_growth(action, pred, before, timeout=4.0, step=0.15):
        """等到符合条件的那一行**多于** `before`。返回 `(命中的数 或 None, 最后读数)`。

        每轮**重新读整个文件**，不缓存 —— 要看的正是它有没有长大，缓存一份
        等于把尺子冻住（与 `get_status` 上面那句注释同一个理由）。
        """
        deadline = time.time() + timeout
        last = None
        while True:
            rows, _, err = audit_read()
            if err is None:
                last = n_of(rows, action, pred)
                if last > before:
                    return last, last
            if time.time() >= deadline:
                return None, last
            time.sleep(step)

    # 审计目录的规则写在 `audit_path_today` 的 docstring 里。路径猜不出来，
    # 所以不给 `--config` 时**明说跳过**（陷阱 60 的纪律）：猜一个不存在的会
    # 当场抛，猜一个存在的是读到**别的进程**的日志然后拿它当判据（假绿）。
    # 两种都比跳过坏，而跳过在报告上看得见。
    audit_ready = False
    if not cfg_path:
        R.skip(
            "审计（NVR 那条路）整段",
            "没给 --config：审计目录的规则是「配置文件所在目录\\logs」"
            "（`main.cpp:1919-1922`），路径猜不出来。"
            "`tests\\rtsp_sub_e2e.ps1` 是显式传 --config 的，所以发布用的那条 "
            "e2e 覆盖得到；单跑本文件时不传就是跳过。"
            "**这不是「验过了」**，是这一段没跑。")
    else:
        # ---- 13a. 正控制：这份日志得先是「活的」 -------------------------
        #
        # 关没关走 `/api/audit`。**只认它明确说 `enabled: false` 的那一种**：
        # 路由没注册时（`if (opt.audit_read)` 那条注册纪律）请求会落到静态
        # 兜底上，拿回来的是 200 加一份 index.html，`json.loads` 抛
        # `ValueError` —— 那是「这台服务器没有这个路由」，不是「审计关了」。
        # 两者必须分开：一个该跳过，一个该照常往下判文件。把它一概当成
        # 「关了」的话，一个把路由注册写坏了的实现会被这段测试放过。
        audit_off = False
        st13, _, b13 = c.get("/api/audit?days=1&limit=1")
        if st13 == 200:
            try:
                aj = json.loads(b13)
            except ValueError:
                aj = None
            if isinstance(aj, dict) and aj.get("enabled") is False:
                audit_off = True

        if audit_off:
            R.skip(
                "审计（NVR 那条路）整段",
                "这台服务器关着审计（GET /api/audit 报 enabled=false）。"
                "三条 NVR 的记录**没验** —— 而这不是「没发生过」，是压根没在"
                "记：`AuditLog::note` 的第一句就是 `if (!enabled_) return;`"
                "（`audit_log.cpp:233`）。要验就得去掉 --no-audit 重启一次。")
        else:
            rows13, bad13, err13 = audit_read()
            if err13 is not None:
                R.check(
                    False, "审计文件读得出来（NVR 那一层要建在它上面）",
                    f"读 {audit_path_today()} 失败："
                    f"{type(err13).__name__}: {err13}\n"
                    "这一条红要先当成**尺子**的问题查，不是产品的问题 —— "
                    "路径、目录权限、编码，三个都可能是它。")
            else:
                # 正控制用的那一条是 `config.set`：它在
                # `web_server.cpp:1048-1069` 里是**每一次成功的 `POST
                # /api/config` 都写**（连「没有字段发生变化」也写），而本文件
                # 在 [9] / [11] / [12] 里已经 POST 过好几次。
                # **不拿 `login.ok` 当正控制**：`[8b]` 只在 `/api/session` 报
                # `auth_required` 时才真的登录，没有鉴权的服务器上那条路整段
                # 不跑（它打一行 note 然后跳过），拿它当守卫会得到一条假红。
                n_cfgset = sum(1 for r in rows13 if r[3] == "config.set")
                R.check(len(rows13) > 0,
                        f"今天的审计文件里有 {len(rows13)} 行（正控制的前提）",
                        "零行的话下面三条「涨了一行」全都无从谈起 —— 那说明"
                        "「在写」这件事本身就不成立，而不是「这三件没记」")
                R.check(n_cfgset > 0,
                        f"里面有 {n_cfgset} 条 config.set（正控制：这一段之前"
                        f"改过好几次配置）",
                        "一条都没有的话，要么审计没开，要么 `config.set` 那条"
                        "写入点坏了 —— 两种都得先修，再谈 NVR 那三条")
                R.check(len(bad13) == 0,
                        f"每一行都解析得成五段（{len(rows13)} 行全部解析成功）",
                        "下面这些行不合产品 `split_line` 的规格，"
                        "而**一行读不出来本身就是一条发现**：\n  "
                        + "\n  ".join(bad13[:5]))
                audit_ready = (len(rows13) > 0 and n_cfgset > 0)

    if audit_ready:
        cfg13 = get_cfg() or {}

        # ---- 13b. 改名（SetVideoEncoderConfiguration）--------------------
        #
        # 目标名要**和现在不同**：`patch_from_body` 没读到变化时 `patch` 是空
        # 的，而 `onvif_service.cpp:1806` 那句是 `if (!patch.empty())` 才记 ——
        # 名字一样就**一条都不写**，症状看起来像「审计漏记」。
        # 名字里带一个**当次运行的时间戳**：`[10]` 用的是 `NVR-CH01`，那是写死
        # 的，两次跑之间会被 60 秒去重闸门挡住。带上秒数之后，同一天连跑两次
        # 也不会撞。
        stamp = _dt.datetime.now().strftime("%H%M%S")
        name0 = ((cfg13.get("onvif") or {}).get("device_name") or "").strip()
        name1 = "AUDIT-" + stamp
        if name1 == name0:
            name1 = "AUDIT-" + stamp + "-B"
        act_enc = "onvif.SetVideoEncoderConfiguration"
        det_enc = "名称=" + name1

        def pred_enc(d):
            return d == det_enc

        rows, _, err = audit_read()
        before = n_of(rows, act_enc, pred_enc) if err is None else 0
        st, data = media_authed(envelope(op_body(
            "SetVideoEncoderConfiguration", NS["trt"],
            '<Configuration token="VideoEncoderConfig_main"><Name>' + name1 +
            "</Name></Configuration>")))
        R.check(st == 200, f"改名成 {name1}（NVR 那条路）→ HTTP {st}", data[:200])
        hit, last = wait_growth(act_enc, pred_enc, before)
        R.check(hit is not None,
                f"审计里多了一行 {act_enc}，细节是「{det_enc}」"
                f"（动手前 {before} 条，动完手 {last} 条）",
                "细节串是**逐字**比对的：`patch_detail`（onvif_service.cpp:603）"
                "在一份只改了 Name 的报文上产出的就是这一句。对不上的话先回去"
                "核对产品那一行，再回来怀疑这条断言。\n"
                "**判据是「涨了」不是「找得到」** —— 60 秒去重闸门让后者怎么"
                "都会绿（陷阱 85）。")
        if hit is not None:
            rows, _, err = audit_read()
            mine = [r for r in rows if r[3] == act_enc and pred_enc(r[4])]
            row = mine[-1]
            # 来源**是裸 IP、不带端口**：`onvif_service.cpp:2241` 那句是
            # `std::string const peer = req.peer_ip();`，没有拼端口。
            R.check(row[1] == a.host,
                    f"那一行的来源是 NVR 的 IP「{row[1]}」（应为 {a.host}，"
                    f"裸 IP、不带端口）",
                    "来源要是空的、或者是别的地址，说明「谁改的」这一列"
                    "没有真的接上源头 —— 而这一列正是用户问这个功能时"
                    "唯一想要的那一列")
            # 身份串 = `onvif:` 加配置里的用户名（`onvif_service.cpp` 的
            # `AuditWho{peer, \"onvif:\" + opt.user}`）。用户名本身读得回来：
            # `/api/config` 报的就是它的明文（`config.cpp:430`，只有口令被
            # 哨兵挡掉），所以这里判**相等**而不是只判前缀 —— 只判
            # `startswith(\"onvif:\")` 的话，串成 `onvif:admin` 而配置里是
            # `admin2` 也照样绿。
            ident = "onvif:" + str((cfg13.get("onvif") or {}).get("user") or "")
            R.check(row[2] == ident,
                    f"那一行的身份是「{row[2]}」（应为 {ident}）",
                    "用户名取自 GET /api/config 的 onvif.user 明文")
            # 时间戳只判**形状**，不与设备时钟对账（见本段文件头）。
            R.check(bool(re.match(r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$",
                                  row[0])),
                    f"那一行的时刻是「{row[0]}」（YYYY-MM-DD HH:MM:SS）",
                    "格式不对的话 `AuditLog::read` 自己也会漏掉它 —— "
                    "页面上那一列会空着")
        else:
            R.skip("13b 的来源 / 身份 / 时刻",
                   "上一条（审计里涨了一行）红了，那一行根本不在，这三条没有"
                   "可判的对象。**明说跳过，而不是让它们静默消失** —— "
                   "静默消失时报告上只是总数少了三条，看起来和「这一段本来"
                   "就没这几条」一模一样（陷阱 46 的同一族）。这一条是"
                   "变异检验（M2：把产品那句「名称=」改成「名字=」）逼出来的："
                   "当时三条一起不见了，而汇总里 `跳过 0 项`。")

        # 复原设备名。
        #
        # 这一段必须**原地**把 `onvif.device_name` 改回去，理由不是好看：这一份
        # 配置文件在整轮 e2e 里是共享的，`[13]` 后面还有别的段，而设备名会进
        # `GetDeviceInformation` / `GetProfiles` 的每一份响应。留着 `AUDIT-<秒>`
        # 的话，**下一次跑别的断言时看到的是一个上一轮留下的名字**，那种失败
        # 指不到任何一行产品代码。
        # `[10g]`（本文件 :1328-1339）就是这么做的，这里照同一个纪律走。
        #
        # `name0` 为空时**明说跳过**，不假装设过：拿空名字去 `patch_from_body`
        # 得不到任何变化，`if (!patch.empty())` 那一句于是什么都不记 —— 也就是说
        # 这一条在那种情况下**根本验不到东西**（陷阱 60），报告里要说清楚。
        if name0:
            st, data = media_authed(envelope(op_body(
                "SetVideoEncoderConfiguration", NS["trt"],
                '<Configuration token="VideoEncoderConfig_main"><Name>' + name0 +
                "</Name></Configuration>")))
            R.check(st == 200, f"把设备名复原成「{name0}」→ HTTP {st}", data[:200])
            back = ((get_cfg() or {}).get("onvif") or {}).get("device_name")
            R.check(back == name0,
                    f"复原之后再读，设备名是「{back}」",
                    f"`[13b]` 改名走的是真 NVR 那条路，这里要确认它**真的被改"
                    f"回来了**（期望「{name0}」）—— 只判 HTTP 200 的话，一个"
                    f"「收了但没落盘」的实现也会绿")
        else:
            R.skip("13b 复原设备名",
                   "改名前读到的 device_name 是空的，拿空名字发过去不会产生"
                   "任何 patch，这一条验不到东西；跳过而不是判绿")

        # ---- 13c. 改水印（SetOSD）----------------------------------------
        #
        # 细节串是 `osd_token(idx) + "：" + osd_item_detail(改完那一项)`。
        # 两处都逐字读过：
        #   * `osd_token(0)` = `OSD_0`（`:847-849`）；
        #   * `osd_item_detail`（`:580-592`）拼的是
        #     `种类 + ，位置=X + ，字号=Npx + ，已启用/已关闭`，
        #     其中 `种类` 是 `时间水印` / `名称水印` / `文字水印`
        #     （文字那一种后面还跟着 `「正文」`）。
        #     分隔符是**全角逗号**，不是 ASCII 逗号。
        #
        # 报文里**故意不带 `<tt:TextString><tt:Type>`** —— `osd_item_from_body`
        # 的 `ttype` 为空时（`:1172` 那行注释）这一项的**种类保持原样**，
        # 于是上面那个前缀可以从**动手之前**读到的配置算出来。这一条正是
        # 「报文只送一半也是合法的」那个设计在这里的用处。
        #
        # ⚠️ 外层那个 `<tt:Type>Text</tt:Type>` **必须放在 `<tt:Position>`
        # 前面**：`osd_item_from_body` 取它用的是**扁平**的
        # `xml_text(body, \"Type\")`，而 `Position` 里也有一个 `Type`。
        # 顺序反了就拿到 `LowerLeft`，于是 `:1111` 判它「不是 Text」直接回
        # Fault —— 报的还是「only Text OSD is supported」，指不到顺序上。
        osd0 = cfg13.get("osd")
        items13 = (osd0 or {}).get("items") or []
        if not items13:
            R.skip("SetOSD 在审计里留下一行（token + 位置 + 字号）",
                   "配置里一个 OSD 项都没有，`OSD_0` 这个 token 不存在 —— "
                   "这一条**没验**。默认配置有两项，所以这条跳过意味着"
                   "上游有人把 OSD 项删光了。")
        else:
            it0 = items13[0]
            tp0 = it0.get("type")
            pfx = {"time": "时间水印", "name": "名称水印"}.get(tp0, "文字水印")
            if tp0 == "text" and it0.get("text"):
                pfx += "「" + str(it0.get("text")) + "」"
            # 字号从**当次运行的时间**推：`px_from_pt(pt) = (pt*4+1)/3`
            # （`onvif_service.cpp:938`）。这一项既是「改成了什么」的证据，
            # 也是让细节串**每次运行都不同**的手段 —— 写死的字号在第二次跑
            # 的时候会被 60 秒去重闸门挡住，而症状和「审计漏记」一模一样。
            pt13 = 20 + (time.time_ns() // 100000) % 80        # 20..99 pt
            px13 = (pt13 * 4 + 1) // 3
            en13 = "已启用" if it0.get("enabled") else "已关闭"
            det_osd = (f"OSD_0：{pfx}，位置=左下，字号={px13}px，{en13}")
            act_osd = "onvif.SetOSD"

            def pred_osd(d):
                return d == det_osd

            payload = (
                '<OSD token="OSD_0">'
                "<tt:VideoSourceConfigurationToken>VideoSourceConfig"
                "</tt:VideoSourceConfigurationToken>"
                "<tt:Type>Text</tt:Type>"
                "<tt:Position><tt:Type>LowerLeft</tt:Type></tt:Position>"
                f"<tt:TextString><tt:FontSize>{pt13}</tt:FontSize></tt:TextString>"
                "</OSD>")
            rows, _, err = audit_read()
            before = n_of(rows, act_osd, pred_osd) if err is None else 0
            st, data = media_authed(
                envelope(op_body("SetOSD", NS["trt"], payload)))
            R.check(st == 200,
                    f"SetOSD 把 OSD_0 挪到左下、字号设为 {pt13}pt"
                    f"（={px13}px）→ HTTP {st}", data[:200])
            hit, last = wait_growth(act_osd, pred_osd, before)
            R.check(hit is not None,
                    f"审计里多了一行 {act_osd}，细节是「{det_osd}」"
                    f"（动手前 {before} 条，动完手 {last} 条）",
                    "细节串逐字来自 `osd_item_detail`（onvif_service.cpp:580），"
                    "分隔符是全角逗号，前面挂着 token。对不上时先核对产品那句。")
            if hit is not None:
                rows, _, err = audit_read()
                mine = [r for r in rows if r[3] == act_osd and pred_osd(r[4])]
                row = mine[-1]
                R.check(row[1] == a.host and
                        row[2] == "onvif:" + str(
                            (cfg13.get("onvif") or {}).get("user") or ""),
                        f"那一行的来源/身份是「{row[1]}」「{row[2]}」",
                        f"应为 {a.host} / onvif:<用户名>")
            else:
                R.skip("13c 的来源 / 身份",
                       "上一条（审计里涨了一行）红了 —— 那一行不在，来源与身份"
                       "没有可判的对象。明说跳过，理由同 13b 那一条：静默消失"
                       "时报告上只是总数少了两条")
            # 复原：把整段 osd 原样 POST 回去（与 [11j] 同一个手法）。
            if isinstance(osd0, dict):
                st, _, b = c.post_json("/api/config", {"osd": osd0})
                R.check(st == 200,
                        f"把 osd 段还原（位置与字号都回去）→ HTTP {st}",
                        b[:200])

        # ---- 13d. 对时（SetSystemDateAndTime）-----------------------------
        #
        # 细节串是 `onvif_service.cpp:1477-1479` 那一行：
        #     「设了设备时间，偏移 <旧> → <新> 秒」
        # 两个数都是 `long long` 打出来的**秒**，箭头两侧各一个空格。
        # 新的那个由我们控制，取一个**别处没用过的** +4321（`[1b]` 用 ±180 /
        # −120，`[12]` 用 +90 / −120）。
        #
        # **不判精确相等**：目标时刻要取整到秒、再加上报文往返，实测会差一两秒
        # （`[1b]` 里设 +180 量到 179）—— 判相等会把一个完全正确的实现报成失败
        # （陷阱 84 的同一族：先问被测的那个东西保证什么）。判「离 4321 在
        # ±5 秒内」，而 4321 这个量级远在任何别的取值之外，所以它照样能唯一定位
        # 到这一行。
        off_target = 4321
        act_time = "onvif.SetSystemDateAndTime"
        head_time = "设了设备时间，偏移 "

        def pred_time(d):
            if not (d.startswith(head_time) and d.endswith(" 秒")):
                return False
            tail = d[len(head_time):-len(" 秒")]
            if " → " not in tail:
                return False
            try:
                return abs(int(tail.split(" → ")[-1]) - off_target) <= 5
            except ValueError:
                return False

        rows, _, err = audit_read()
        before = n_of(rows, act_time, pred_time) if err is None else 0
        # 改时钟**之前**设备时钟的偏移，现取一次。
        #
        # 它是下面那条「那一行带着新旧两个偏移」的**前提**：旧值那半必须来自
        # 一个独立于审计行本身的读法，否则判它就等于判「这一行里有两个数」
        # （陷阱 85 的形状，那一条原本就是这么写的、也正因此一次都没验到东西）。
        # `/api/time` 的 `offset_s` 报的就是 `DeviceState::time_offset()` ——
        # 与产品在 `onvif_service.cpp:1452` 读的是同一个字段。
        #
        # 读不到时**明说**，不悄悄退化成弱判据：那会让「今天读不到」和
        # 「产品没记旧值」在报告上长得一模一样。
        off_before = (get_time() or {}).get("offset_s")
        R.check(isinstance(off_before, int),
                f"动手之前设备时钟的偏移读得到：{off_before!r}",
                "这一条是下一条判据的前提，不是装饰：旧值只有来自一个"
                "**独立**的读法（/api/time），那条断言才真的在验东西")
        tgt13 = (_dt.datetime.now(_dt.timezone.utc)
                 + _dt.timedelta(seconds=off_target)).replace(microsecond=0)
        st, data = set_time("<tds:DateTimeType>Manual</tds:DateTimeType>"
                            "<tds:UTCDateTime>" + dt_fields(tgt13)
                            + "</tds:UTCDateTime>")
        R.check(st == 200,
                f"NVR 把设备时间设到 +{off_target} 秒 → HTTP {st}", data[:200])
        hit, last = wait_growth(act_time, pred_time, before)
        R.check(hit is not None,
                f"审计里多了一行 {act_time}，细节形如「{head_time}"
                f"<旧> → <新≈{off_target}> 秒」（动手前 {before} 条，"
                f"动完手 {last} 条）",
                "细节里记的是**结果**（新旧两个偏移），不是报文里那个绝对"
                "时刻 —— 所以判据只能落在新偏移上。")
        if hit is not None:
            rows, _, err = audit_read()
            mine = [r for r in rows if r[3] == act_time and pred_time(r[4])]
            row = mine[-1]
            R.check(row[1] == a.host and
                    row[2] == "onvif:" + str(
                        (cfg13.get("onvif") or {}).get("user") or ""),
                    f"那一行的来源/身份是「{row[1]}」「{row[2]}」",
                    f"应为 {a.host} / onvif:<用户名>")
            # 「谁把设备时钟改了」是这一段唯一能回答的地方：`DeviceState`
            # 的 `synced_at()` 只记「什么时候被改的」，记不了「被谁改的」
            # （`device_state.h:46`，这正是审计存在的理由之一）。
            #
            # ## 这一条**改写过一次**，原因是变异检验（陷阱 85）
            #
            # 原来的判据是 `"→" in row[4] and row[4].endswith(" 秒")`。
            # 它一次都没在验东西：上面那个 `pred_time` 已经要求
            # `" → " in tail` 且 `d.endswith(" 秒")`，而 `row` 就是按
            # `pred_time` 从 rows 里筛出来的 —— **能走到这一行的每一行都
            # 必然含箭头、也必然以「 秒」结尾**。实测（M3a：把产品那句里的
            # `" → "` 换成 `"->"`）它**根本没被跑到**：`pred_time` 先不认，
            # `hit` 就是 None，整块被 `if hit is not None` 跳过去了。
            #
            # 改成判**旧值也在，而且对得上**。旧值有一个独立于这一行的读法：
            # `/api/time` 的 `offset_s`（下面 `off_before`，就是发这一炮之前
            # 现取的），而产品那个数是它在改时钟之前从 `DeviceState` 读的
            # 同一个字段（`onvif_service.cpp:1452` 的 `time_offset()`）。
            # 两条路各自读一次、对得上，才叫「记了新旧两个偏移的实现」。
            #
            # 判据写成「两个数都解析得出来 + 旧≈off_before + 新≈off_target」，
            # 三个条件缺一不可：**只有一个数**时 `parts` 长度不足，
            # **把新值写两遍**时旧值那半就对不上（这正是 M3b 那个变异）。
            # 容差 ±2 是给旧值的（它是个整秒量，改时钟前后不受往返影响，
            # 所以这里不是「差不多」，是「允许主机时钟在两次读之间走一拍」）。
            tail = (row[4][len(head_time):-len(" 秒")]
                    if row[4].startswith(head_time)
                    and row[4].endswith(" 秒") else "")
            parts = tail.split(" → ") if tail else []
            got_old = got_new = None
            if len(parts) == 2:
                try:
                    got_old, got_new = int(parts[0]), int(parts[1])
                except ValueError:
                    got_old = got_new = None
            # 每一项都先问「拿得到吗」再算差：`off_before` 读不到时它是 None，
            # 而 `abs(int - None)` 会抛 TypeError —— **一条断言抛出去会把整份
            # 探针带走**（Phase U 收尾时实测过一次，一次 `FileNotFoundError`
            # 连夹具一起杀掉、后面两百多条一条没跑）。所以这里用短路写法，
            # 不靠「前面那条已经报红了」来保证这里安全。
            old_ok = (isinstance(off_before, int) and got_old is not None
                      and abs(got_old - off_before) <= 2)
            new_ok = got_new is not None and abs(got_new - off_target) <= 5
            R.check(old_ok and new_ok,
                    f"那一行带着新旧两个偏移：「{row[4]}」"
                    f"（旧应≈{off_before}，新应≈{off_target}）",
                    "只记一个数的话，现场就没法回答「原来偏多少」——"
                    "而排查「录像时间对不上」恰恰从它开始。"
                    "旧值那半来自 `/api/time` 的独立一读，不是从这一行自己"
                    "身上抠出来的：判它才不会退化成「这一行里有两个数」"
                    "那种怎么都绿的东西（陷阱 85）。")
        else:
            R.skip("13d 的来源 / 身份 / 新旧两个偏移",
                   "上一条（审计里涨了一行）红了 —— 那一行不在，这三条没有"
                   "可判的对象。明说跳过，理由同 13b 那一条")

        # 复原设备时钟。**走 ONVIF 这条路而不是 `POST /api/time`**：后者在
        # NTP 模式下是被拒的（`[12]` 早就记过这一条），而 `SetSystemDateAndTime`
        # 两种模式下都收 —— 用同一个入口改回去，才不会因为模式不同而留下
        # 一段「测试改过而没改回来」的偏移。
        off_now = (get_time() or {}).get("offset_s")
        if off_now is None:
            R.check(False, "对时复原前读一次 /api/time",
                    "读不到就没法知道该把它推回哪里 —— 这一段的收尾"
                    "**没做成**，设备时钟停在 +4321 附近")
        else:
            back = (_dt.datetime.now(_dt.timezone.utc)
                    + _dt.timedelta(seconds=off_now)).replace(microsecond=0)
            st, data = set_time("<tds:DateTimeType>Manual</tds:DateTimeType>"
                                "<tds:UTCDateTime>" + dt_fields(back)
                                + "</tds:UTCDateTime>")
            R.check(st == 200, f"把设备时钟推回 {off_now:+d} 秒 → HTTP {st}",
                    data[:200])
            t_after = get_time() or {}
            got_off = t_after.get("offset_s")
            R.check(isinstance(got_off, int) and abs(got_off - off_now) <= 2,
                    f"设备时钟回到原处：{got_off!r}（原本 {off_now}）",
                    "只判「发送成功」的话，一个把偏移算错的实现照样绿；"
                    "差 2 秒以内是目标时刻取整到秒带来的，不是错")

    print(f"\n{'=' * 60}\n通过 {R.passed} 项，失败 {R.failed} 项，"
          f"跳过 {R.skipped} 项。")
    return 1 if R.failed else 0


if __name__ == "__main__":
    sys.exit(main())
