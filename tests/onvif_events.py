#!/usr/bin/env python3
"""ONVIF Events（PullPoint）端点测试（纯标准库 + 严格 XML 解析）。

这一段验的是**引用生命周期**，不是"有个事件服务"：

  * `CreatePullPointSubscription` 返回的 `SubscriptionReference` 必须能用
    （Address 里带订阅标识，PullMessages 认得它）；
  * 没有事件时 `PullMessages` 回**结构良好的空列表**（有 CurrentTime /
    TerminationTime，但零条 NotificationMessage）；
  * `Timeout` 被尊重（会等），但不会把请求永远挂着；
  * `Renew` 延长、`Unsubscribe` 删除，重复 `Unsubscribe` 幂等；
  * 删完之后再拉必须失败 —— 只验"回 200"的话，一个根本没删的实现也能全绿。

事件本身只有**心跳**（topic `d2o:Device/Heartbeat`），不是运动报警。
`onvif_events.py` 明确断言响应里**没有** `MotionAlarm` 字样：这个项目没有
运动检测，伪造一个报警源比不支持更坏。

用法：
    python -I -X utf8 tests/onvif_events.py --host 127.0.0.1 --port 8080
"""

import argparse
import datetime as _dt
import importlib.util as _ilu
import os
import sys
import time
import xml.etree.ElementTree as ET
from urllib.parse import parse_qs, urlsplit


def _load_common():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "onvif_common.py")
    spec = _ilu.spec_from_file_location("onvif_common", path)
    mod = _ilu.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_common = _load_common()
NS = dict(_common.NS)
NS.update({
    "tev": "http://www.onvif.org/ver10/events/wsdl",
    "wsnt": "http://docs.oasis-open.org/wsn/b-2",
    "wstop": "http://docs.oasis-open.org/wsn/t-1",
    "wsa": "http://www.w3.org/2005/08/addressing",
    "d2o": "urn:desk2onvif:events",
})
envelope = _common.envelope
op_body = _common.op_body
Client = _common.Client
Runner = _common.Runner
parse_challenge = _common.parse_challenge

DEV = "/onvif/device_service"
EVT = "/onvif/events_service"
NS_DEV = NS["tds"]
NS_TEV = NS["tev"]
NS_WSNT = NS["wsnt"]


def parse_xsd_datetime(s):
    """`2026-11-26T12:00:00Z` → aware datetime。只认我们发出去的那种形状。"""
    return _dt.datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(
        tzinfo=_dt.timezone.utc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin")
    a = ap.parse_args()

    c = Client(a.host, a.port)
    R = Runner()

    def soap(path, body):
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

    def pull(path, timeout, limit=5):
        inner = (f"<tev:Timeout>{timeout}</tev:Timeout>"
                 f"<tev:MessageLimit>{limit}</tev:MessageLimit>")
        return soap(path, envelope(op_body("PullMessages", NS_TEV, inner)))

    def unsubscribe(path):
        return soap(path, envelope(op_body("Unsubscribe", NS_WSNT)))

    # ---- [1] GetServices 宣告 Events ------------------------------------
    print("\n[1] GetServices 宣告 Events，XAddr 指向 /onvif/events_service")
    st, data = soap(DEV, envelope(op_body("GetServices", NS_DEV)))
    R.check(st == 200, f"GetServices → HTTP {st}", data[:200])
    root = R.strict_xml(data, "GetServices")
    evt_addr = None
    if root is not None:
        for svc in R.findall(root, ".//tds:Service"):
            ns = R.find(svc, "tds:Namespace")
            xa = R.find(svc, "tds:XAddr")
            if ns is not None and ns.text == NS_TEV:
                evt_addr = xa.text if xa is not None else None
        R.check(evt_addr is not None,
                f"服务列表里有 Events（XAddr={evt_addr}）",
                "没有 Events 时 CreatePullPointSubscription 会 404/405，"
                "部分 NVR 会在添加通道的日志里留一条 Fault。")
        if evt_addr:
            R.check(urlsplit(evt_addr).path == EVT,
                    f"XAddr 路径 = {urlsplit(evt_addr).path}（期望 {EVT}）")

    # ---- [2] GetServiceCapabilities -------------------------------------
    print("\n[2] Events GetServiceCapabilities")
    st, data = soap(EVT, envelope(op_body("GetServiceCapabilities", NS_TEV)))
    R.check(st == 200, f"HTTP {st}", data[:200])
    root = R.strict_xml(data, "GetServiceCapabilities[Events]")
    if root is not None:
        caps = R.find(root, ".//tev:Capabilities")
        R.check(caps is not None, "找到 tev:Capabilities")
        if caps is not None:
            mp = caps.get("MaxPullPoints") or ""
            R.check(mp.isdigit() and int(mp) > 0,
                    f"MaxPullPoints = {mp!r}（要 > 0）")
            R.check((caps.get("WSSubscriptionPolicySupport") or "").lower()
                    == "false",
                    "WSSubscriptionPolicySupport = false（我们只做 PullPoint）")

    # ---- [3] GetEventProperties -----------------------------------------
    print("\n[3] GetEventProperties：topic 树与方言")
    st, data = soap(EVT, envelope(op_body("GetEventProperties", NS_TEV)))
    R.check(st == 200, f"HTTP {st}", data[:300])
    root = R.strict_xml(data, "GetEventProperties")
    if root is not None:
        R.check(R.find(root, ".//tev:TopicNamespaceLocation") is not None,
                "有 TopicNamespaceLocation")
        fixed = R.find(root, ".//wsnt:FixedTopicSet")
        R.check(fixed is not None and (fixed.text or "").strip().lower()
                == "true", f"FixedTopicSet = {fixed.text if fixed is not None else None}")
        ts = R.find(root, ".//wstop:TopicSet")
        R.check(ts is not None, "有 wstop:TopicSet")
        if ts is not None:
            # `TopicSetType` 的 any 是 ##other，所以树节点用 tt:Topic；
            # 叶子必须叫 Heartbeat —— 这是我们真正会发的那一种。
            leaf = ts.find(".//tt:Topic[@name='Heartbeat']", NS)
            R.check(leaf is not None, "topic 树里有 Heartbeat 叶子")
            R.check(ts.find(".//tt:Topic[@name='MotionAlarm']", NS) is None,
                    "topic 树里**没有** MotionAlarm（没有运动源，不伪造）")
        dialects = [d.text for d in R.findall(root, ".//wsnt:TopicExpressionDialect")]
        R.check(any("ConcreteSet" in (d or "") for d in dialects),
                f"TopicExpressionDialect 含 ConcreteSet：{dialects}")
        R.check(R.find(root, ".//tev:MessageContentFilterDialect") is not None,
                "有 MessageContentFilterDialect")

    # ---- [4] CreatePullPointSubscription --------------------------------
    print("\n[4] CreatePullPointSubscription：SubscriptionReference 可用")
    st, data = soap(EVT, envelope(op_body(
        "CreatePullPointSubscription", NS_TEV,
        "<tev:InitialTerminationTime>PT1H</tev:InitialTerminationTime>")))
    R.check(st == 200, f"HTTP {st}", data[:300])
    root = R.strict_xml(data, "CreatePullPointSubscription")
    sub_path = None
    sub_token = None
    old_term = None
    if root is not None:
        addr = R.find(root, ".//tev:SubscriptionReference/wsa:Address")
        R.check(addr is not None and bool(addr.text),
                f"SubscriptionReference/Address = "
                f"{addr.text if addr is not None else None}")
        cur = R.find(root, ".//wsnt:CurrentTime")
        term = R.find(root, ".//wsnt:TerminationTime")
        R.check(cur is not None and bool(cur.text), "有 wsnt:CurrentTime")
        R.check(term is not None and bool(term.text), "有 wsnt:TerminationTime")
        if term is not None and term.text:
            try:
                old_term = parse_xsd_datetime(term.text)
                now = _dt.datetime.now(_dt.timezone.utc)
                R.check(old_term > now + _dt.timedelta(minutes=50),
                        f"TerminationTime 约一小时后（{term.text}）")
            except ValueError as e:
                R.check(False, f"TerminationTime 是 xsd:dateTime（{term.text}）", e)
        if addr is not None and addr.text:
            sp = urlsplit(addr.text)
            R.check(sp.path == EVT, f"Address 路径 = {sp.path}")
            q = parse_qs(sp.query)
            vals = q.get("SubscriptionID") or []
            R.check(len(vals) == 1 and bool(vals[0]),
                    f"Address 查询串带订阅标识：{sp.query}")
            if vals:
                sub_token = vals[0]
                sub_path = sp.path + "?" + sp.query
            else:
                sub_path = sp.path
        rp = R.find(root, ".//wsa:ReferenceParameters/d2o:SubscriptionID")
        R.check(rp is not None and bool(rp.text),
                f"ReferenceParameters 里也有标识 = "
                f"{rp.text if rp is not None else None}")

    if sub_path is None:
        # 后面每一条都依赖这个引用。没有它时明说跳过后面的生命周期段，
        # 而不是拿空路径继续打，把"没建成"变成一串看不出根因的失败。
        R.skip("PullPoint 生命周期", "CreatePullPointSubscription 没给出可用的 Address")
        print(f"\n结果：通过 {R.passed}，失败 {R.failed}，跳过 {R.skipped}")
        return 0 if R.failed == 0 else 1

    # ---- [5] PullMessages：第一条是心跳，第二条是空 ---------------------
    print("\n[5] PullMessages：第一条心跳、之后空列表")
    st, data = pull(sub_path, "PT0S")
    R.check(st == 200, f"第一次 PullMessages → HTTP {st}", data[:300])
    root = R.strict_xml(data, "PullMessages[heartbeat]")
    if root is not None:
        cur = R.find(root, ".//tev:CurrentTime")
        term = R.find(root, ".//tev:TerminationTime")
        R.check(cur is not None and bool(cur.text), "有 tev:CurrentTime")
        R.check(term is not None and bool(term.text), "有 tev:TerminationTime")
        notes = R.findall(root, ".//wsnt:NotificationMessage")
        R.check(len(notes) >= 1,
                f"第一次就有心跳（通知数 = {len(notes)}）",
                "订阅刚建时 last_heartbeat 被往回拨了一个周期，第一条应当"
                "立刻能拿到；拿不到说明这个回拨没了。")
        for n in notes:
            topic = R.find(n, "wsnt:Topic")
            R.check(topic is not None and (topic.text or "").strip()
                    == "d2o:Device/Heartbeat",
                    f"topic = {topic.text if topic is not None else None}")
            R.check("MotionAlarm" not in ET.tostring(n, encoding="unicode"),
                    "通知里没有 MotionAlarm")
            msg = R.find(n, "wsnt:Message/tt:Message")
            R.check(msg is not None and bool(msg.get("UtcTime")),
                    f"tt:Message@UtcTime = "
                    f"{None if msg is None else msg.get('UtcTime')}")
            if msg is not None:
                state = msg.find("tt:Data/tt:SimpleItem[@Name='State']", NS)
                R.check(state is not None and state.get("Value") == "Online",
                        f"Data/State = {None if state is None else state.get('Value')}")

    st, data = pull(sub_path, "PT0S")
    R.check(st == 200, f"第二次 PullMessages → HTTP {st}", data[:200])
    root = R.strict_xml(data, "PullMessages[empty]")
    if root is not None:
        notes = R.findall(root, ".//wsnt:NotificationMessage")
        R.check(len(notes) == 0, f"空列表的消息数 = {len(notes)}")
        R.check(R.find(root, ".//tev:CurrentTime") is not None,
                "空列表也有 CurrentTime（结构良好，不是空 body）")
        R.check(R.find(root, ".//tev:TerminationTime") is not None,
                "空列表也有 TerminationTime")

    # ---- [6] Timeout 会被尊重（等，但有限） -----------------------------
    print("\n[6] PullMessages[Timeout=PT1S]：等约 1 秒后回空")
    started = time.monotonic()
    st, data = pull(sub_path, "PT1S")
    elapsed = time.monotonic() - started
    R.check(st == 200, f"HTTP {st}（{elapsed:.2f}s）", data[:200])
    R.check(0.7 <= elapsed < 4.0,
            f"耗时 {elapsed:.2f}s（期望 0.7~4s：既不立即返回、也不挂死）")
    if st == 200:
        root = ET.fromstring(data)
        R.check(len(root.findall(".//wsnt:NotificationMessage", NS)) == 0,
                "超时后仍然是空列表")

    # ---- [7] Renew -------------------------------------------------------
    print("\n[7] Renew：TerminationTime 往后走")
    st, data = soap(sub_path, envelope(op_body(
        "Renew", NS_WSNT,
        "<wsnt:TerminationTime>PT2H</wsnt:TerminationTime>")))
    R.check(st == 200, f"HTTP {st}", data[:200])
    root = R.strict_xml(data, "Renew")
    new_term = None
    if root is not None:
        t = R.find(root, ".//wsnt:TerminationTime")
        R.check(t is not None and bool(t.text),
                f"RenewResponse/TerminationTime = "
                f"{t.text if t is not None else None}")
        if t is not None and t.text:
            try:
                new_term = parse_xsd_datetime(t.text)
            except ValueError as e:
                R.check(False, f"TerminationTime 是 xsd:dateTime（{t.text}）", e)
        R.check(R.find(root, ".//wsnt:CurrentTime") is not None,
                "RenewResponse 有 CurrentTime")
    if old_term is not None and new_term is not None:
        R.check(new_term > old_term,
                f"续期后 {new_term.isoformat()} > 原 {old_term.isoformat()}")

    # 不带查询串、只有一个活跃订阅：PullMessages 仍然认得它。
    # 这是真实 NVR 的常见行为（它只把 Address 存下来，可能丢掉查询串）。
    print("\n[7b] 不带订阅标识、只有一个订阅时仍然可用")
    st, data = pull(EVT, "PT0S")
    R.check(st == 200, f"HTTP {st}", data[:200])

    # ---- [8] Unsubscribe：删除 + 幂等 -----------------------------------
    print("\n[8] Unsubscribe：删除、重复删除幂等、删后拉取失败")
    st, data = unsubscribe(sub_path)
    R.check(st == 200, f"第一次 Unsubscribe → HTTP {st}", data[:200])
    root = R.strict_xml(data, "Unsubscribe")
    if root is not None:
        R.check(root.tag == f"{{{NS_WSNT}}}UnsubscribeResponse",
                f"响应根元素 = {root.tag}")

    st, data = unsubscribe(sub_path)
    R.check(st == 200, f"第二次 Unsubscribe（幂等）→ HTTP {st}", data[:200])

    st, data = pull(sub_path, "PT0S")
    R.check(st == 500, f"删除后 PullMessages → HTTP {st}（期望 500 Fault）",
            data[:200])
    root = R.strict_xml(data, "PullMessages after Unsubscribe")
    if root is not None:
        sub = R.find(root, ".//soap:Subcode/soap:Value")
        R.check(sub is not None and sub.text == "ter:InvalidArgVal",
                f"Subcode = {sub.text if sub is not None else None}")

    # ---- [9] 畸形 Timeout 是 Fault，不是挂住 -----------------------------
    print("\n[9] PullMessages[Timeout=abc] → 结构良好的 Fault")
    # 需要一个活订阅才能走到 Timeout 解析那一步。
    st, data = soap(EVT, envelope(op_body("CreatePullPointSubscription", NS_TEV)))
    R.check(st == 200, f"新建一个订阅 → HTTP {st}", data[:200])
    if st == 200:
        root = ET.fromstring(data)
        addr = root.find(".//tev:SubscriptionReference/wsa:Address", NS)
        p2 = None
        if addr is not None and addr.text:
            sp = urlsplit(addr.text)
            p2 = sp.path + "?" + sp.query
        if p2:
            st, data = pull(p2, "not-a-duration")
            R.check(st == 500, f"畸形 Timeout → HTTP {st}（期望 500）", data[:200])
            if st == 500:
                R.check(b"InvalidArgVal" in data,
                        "Fault 里说明了参数非法")
            unsubscribe(p2)

    print(f"\n结果：通过 {R.passed}，失败 {R.failed}，跳过 {R.skipped}")
    return 0 if R.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
