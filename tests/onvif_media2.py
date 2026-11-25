#!/usr/bin/env python3
"""ONVIF Media2 端点测试（纯标准库 + 严格 XML 解析）。

为什么单独一个探针，而不是往 `onvif_soap.py` 里再塞十段：Media2 的**结构**
和 Media1 不一样（`tr2:Profiles` / `tr2:Configurations` / `tr2:Uri`），而
最容易犯的错恰恰是"把 Media1 的响应换个前缀发出去"—— 那种响应看起来完全
正常，`strict_xml` 也过得去（前缀都声明了），只有逐字断言元素名才抓得住。

用法：
    python -I -X utf8 tests/onvif_media2.py --host 127.0.0.1 --port 8080
    python -I -X utf8 tests/onvif_media2.py --user admin --password admin

`--profile-token` 不传时用 GetProfiles 的第一条；聚合端点可以指定 `ch2_main`
之类，把单路断言对准那一条。
"""

import argparse
import importlib.util as _ilu
import os
import sys
import xml.etree.ElementTree as ET
from urllib.parse import urlsplit


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
    "tr2": "http://www.onvif.org/ver20/media/wsdl",
})
envelope = _common.envelope
op_body = _common.op_body
Client = _common.Client
Runner = _common.Runner
parse_challenge = _common.parse_challenge

DEV = "/onvif/device_service"
MED = "/onvif/media_service"
MED2 = "/onvif/media2_service"
NS_DEV = NS["tds"]
NS_MED = NS["trt"]
NS_MED2 = NS["tr2"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin")
    ap.add_argument("--profile-token", default=None,
                    help="只对这条 profile 做单路断言（如 ch2_main）")
    a = ap.parse_args()

    c = Client(a.host, a.port)
    R = Runner()

    def soap(path, body):
        """带 Digest 发一次：先匿名拿挑战，再按 RFC 2617 算摘要。"""
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

    # ---- [1] GetServices：Media2 的 XAddr 必须真的指向这个端点 -------------
    print("\n[1] GetServices 宣告 Media2，且 XAddr 就是 /onvif/media2_service")
    st, data = soap(DEV, envelope(op_body("GetServices", NS_DEV)))
    R.check(st == 200, f"GetServices → HTTP {st}", data[:200])
    root = R.strict_xml(data, "GetServices")
    media2_addr = None
    if root is not None:
        for svc in R.findall(root, ".//tds:Service"):
            ns = R.find(svc, "tds:Namespace")
            xa = R.find(svc, "tds:XAddr")
            if ns is not None and ns.text == NS_MED2:
                media2_addr = xa.text if xa is not None else None
        R.check(media2_addr is not None,
                f"服务列表里有 Media2（XAddr={media2_addr}）",
                "没有 Media2 时新一些的 NVR 会退回 Media1；这条红说明"
                "GetServices 没宣告，或者 media2_enabled=false。")
        if media2_addr:
            sp = urlsplit(media2_addr)
            R.check(sp.path == MED2,
                    f"XAddr 路径 = {sp.path}（期望 {MED2}）")

    # ---- [2] GetServiceCapabilities --------------------------------------
    print("\n[2] Media2 GetServiceCapabilities：Capabilities2 的形状")
    st, data = soap(MED2, envelope(op_body("GetServiceCapabilities", NS_MED2)))
    R.check(st == 200, f"HTTP {st}", data[:300])
    root = R.strict_xml(data, "GetServiceCapabilities[Media2]")
    caps = None
    if root is not None:
        caps = R.find(root, ".//tr2:Capabilities")
        R.check(caps is not None, "找到 tr2:Capabilities")
        if caps is not None:
            R.check((caps.get("SnapshotUri") or "").lower() == "true",
                    f"SnapshotUri = {caps.get('SnapshotUri')}")
            pc = R.find(caps, "tr2:ProfileCapabilities")
            sc = R.find(caps, "tr2:StreamingCapabilities")
            R.check(pc is not None, "有 tr2:ProfileCapabilities")
            R.check(sc is not None, "有 tr2:StreamingCapabilities")
            if pc is not None:
                conf = pc.get("ConfigurationsSupported") or ""
                for want in ("VideoSource", "VideoEncoder"):
                    R.check(want in conf,
                            f"ConfigurationsSupported 含 {want}（{conf!r}）")
                R.check((pc.get("MaximumNumberOfProfiles") or "").isdigit(),
                        f"MaximumNumberOfProfiles = "
                        f"{pc.get('MaximumNumberOfProfiles')}")
            if sc is not None:
                R.check((sc.get("RTSPStreaming") or "").lower() == "true",
                        f"RTSPStreaming = {sc.get('RTSPStreaming')}")

    # ---- [3] GetProfiles：token 集合必须和 Media1 一致 --------------------
    print("\n[3] GetProfiles：Media2 的 token 集合 == Media1")
    st1, data1 = soap(MED, envelope(op_body("GetProfiles", NS_MED)))
    R.check(st1 == 200, f"Media1 GetProfiles → HTTP {st1}")
    m1_tokens = set()
    if st1 == 200:
        r1 = ET.fromstring(data1)
        m1_tokens = {p.get("token") for p in r1.findall(".//trt:Profiles", NS)}

    st, data = soap(MED2, envelope(op_body("GetProfiles", NS_MED2)))
    R.check(st == 200, f"Media2 GetProfiles → HTTP {st}", data[:300])
    root = R.strict_xml(data, "GetProfiles[Media2]")
    tokens = set()
    first = None
    if root is not None:
        profiles = R.findall(root, ".//tr2:Profiles")
        tokens = {p.get("token") for p in profiles}
        R.check(bool(profiles), f"profile 数 = {len(profiles)}")
        R.check(tokens == m1_tokens,
                f"Media2 token 集合 {sorted(tokens)} == Media1 {sorted(m1_tokens)}",
                "两边不一致意味着 NVR 从 Media2 看到的通道和从 Media1 看到"
                "的不是同一台设备。")
        if profiles:
            first = profiles[0]
            token = first.get("token")
            R.check(bool(token), f"首条 token = {token}")
            # Media2 的 Name 在 tr2 里；配置项是 tr2:VideoEncoder，里面的
            # tt:Name。**不能出现 Media1 的 tt:VideoEncoderConfiguration**。
            name = first.find("tr2:Name", NS)
            R.check(name is not None and name.text,
                    f"tr2:Name = {name.text if name is not None else None}")
            enc = first.find("tr2:Configurations/tr2:VideoEncoder", NS)
            R.check(enc is not None, "有 tr2:Configurations/tr2:VideoEncoder")
            R.check(first.find(".//tt:VideoEncoderConfiguration", NS) is None,
                    "没有 Media1 的 tt:VideoEncoderConfiguration（形状没串）")
            if enc is not None:
                R.check((enc.get("GovLength") or "").isdigit(),
                        f"VideoEncoder@GovLength = {enc.get('GovLength')}")
                R.check(bool(enc.get("Profile")),
                        f"VideoEncoder@Profile = {enc.get('Profile')}")
                e = enc.find("tt:Encoding", NS)
                R.check(e is not None and e.text == "H264",
                        f"tt:Encoding = {e.text if e is not None else None}")
            vs = first.find("tr2:Configurations/tr2:VideoSource", NS)
            R.check(vs is not None, "有 tr2:Configurations/tr2:VideoSource")
            if vs is not None:
                b = vs.find("tt:Bounds", NS)
                R.check(b is not None and (b.get("width") or "").isdigit(),
                        f"VideoSource Bounds = "
                        f"{None if b is None else b.attrib}")

    chosen = a.profile_token
    if chosen is None and first is not None:
        chosen = first.get("token")
    if chosen is not None:
        R.check(chosen in tokens, f"选的 profile token {chosen} 在表里")

    # ---- [4] GetProfiles 带 Token 只回一条 --------------------------------
    print("\n[4] GetProfiles[Token] 只回这一条")
    if chosen is None:
        R.skip("GetProfiles[Token]", "前面没拿到任何 profile token")
    else:
        inner = f"<tr2:Token>{chosen}</tr2:Token>"
        st, data = soap(MED2, envelope(op_body("GetProfiles", NS_MED2, inner)))
        R.check(st == 200, f"HTTP {st}", data[:200])
        root = R.strict_xml(data, "GetProfiles[Token]")
        if root is not None:
            ps = R.findall(root, ".//tr2:Profiles")
            R.check(len(ps) == 1,
                    f"条数 = {len(ps)}（期望 1）")
            if ps:
                R.check(ps[0].get("token") == chosen,
                        f"token = {ps[0].get('token')}")

    # ---- [5] GetStreamUri / GetSnapshotUri：Media2 的 Uri 是裸元素 --------
    print("\n[5] GetStreamUri / GetSnapshotUri 的 Media2 形状")
    rtsp_uri = None
    if chosen is None:
        R.skip("GetStreamUri[Media2]", "没有 profile token")
    else:
        inner = (f"<tr2:Protocol>RtspUnicast</tr2:Protocol>"
                 f"<tr2:ProfileToken>{chosen}</tr2:ProfileToken>")
        st, data = soap(MED2, envelope(op_body("GetStreamUri", NS_MED2, inner)))
        R.check(st == 200, f"GetStreamUri → HTTP {st}", data[:300])
        root = R.strict_xml(data, "GetStreamUri[Media2]")
        if root is not None:
            u = R.find(root, ".//tr2:Uri")
            rtsp_uri = u.text if u is not None else None
            R.check(bool(rtsp_uri and rtsp_uri.startswith("rtsp://")),
                    f"tr2:Uri = {rtsp_uri}")
            R.check(root.find(".//trt:MediaUri", NS) is None,
                    "没有 Media1 的 trt:MediaUri 包装")

        inner = f"<tr2:ProfileToken>{chosen}</tr2:ProfileToken>"
        st, data = soap(MED2, envelope(op_body("GetSnapshotUri", NS_MED2, inner)))
        R.check(st == 200, f"GetSnapshotUri → HTTP {st}", data[:300])
        root = R.strict_xml(data, "GetSnapshotUri[Media2]")
        if root is not None:
            u = R.find(root, ".//tr2:Uri")
            snap = u.text if u is not None else None
            R.check(bool(snap and snap.startswith("http://")),
                    f"tr2:Uri = {snap}")

    # ---- [6] GetVideoEncoderConfigurations -------------------------------
    print("\n[6] GetVideoEncoderConfigurations：VideoEncoder2Configuration")
    st, data = soap(MED2, envelope(
        op_body("GetVideoEncoderConfigurations", NS_MED2)))
    R.check(st == 200, f"HTTP {st}", data[:300])
    root = R.strict_xml(data, "GetVideoEncoderConfigurations")
    enc_tokens = set()
    if root is not None:
        cfgs = R.findall(root, ".//tr2:Configurations")
        R.check(bool(cfgs), f"配置数 = {len(cfgs)}")
        enc_tokens = {cfg.get("token") for cfg in cfgs}
        R.check(all(t and t.startswith("VideoEncoderConfig_") for t in enc_tokens),
                f"token 形状 = {sorted(enc_tokens)}",
                "Media2 的编码器 token 应当沿用 Media1 的 "
                "VideoEncoderConfig_<profile> 规则。")
        for cfg in cfgs:
            R.check((cfg.get("GovLength") or "").isdigit(),
                    f"[{cfg.get('token')}] GovLength = {cfg.get('GovLength')}")
            R.check(bool(cfg.get("Profile")),
                    f"[{cfg.get('token')}] Profile = {cfg.get('Profile')}")
            e = cfg.find("tt:Encoding", NS)
            res = cfg.find("tt:Resolution/tt:Width", NS)
            br = cfg.find("tt:RateControl/tt:BitrateLimit", NS)
            R.check(e is not None and e.text == "H264",
                    f"[{cfg.get('token')}] Encoding = "
                    f"{e.text if e is not None else None}")
            R.check(res is not None and res.text.isdigit(),
                    f"[{cfg.get('token')}] Width = "
                    f"{res.text if res is not None else None}")
            R.check(br is not None and br.text.isdigit(),
                    f"[{cfg.get('token')}] BitrateLimit = "
                    f"{br.text if br is not None else None}")
        if chosen is not None:
            R.check(f"VideoEncoderConfig_{chosen}" in enc_tokens,
                    f"选中的 profile 有对应的编码器配置 "
                    f"VideoEncoderConfig_{chosen}")

    # ---- [7] GetVideoEncoderConfigurationOptions -------------------------
    print("\n[7] GetVideoEncoderConfigurationOptions：Media2 的属性与元素")
    st, data = soap(MED2, envelope(
        op_body("GetVideoEncoderConfigurationOptions", NS_MED2)))
    R.check(st == 200, f"HTTP {st}", data[:300])
    root = R.strict_xml(data, "GetVideoEncoderConfigurationOptions")
    if root is not None:
        opts = R.findall(root, ".//tr2:Options")
        R.check(bool(opts), f"Options 数 = {len(opts)}")
        for o in opts:
            gov = (o.get("GovLengthRange") or "").split()
            R.check(len(gov) == 2 and all(x.isdigit() for x in gov),
                    f"GovLengthRange = {o.get('GovLengthRange')!r}（要两个整数）")
            R.check(bool((o.get("FrameRatesSupported") or "").strip()),
                    f"FrameRatesSupported = {o.get('FrameRatesSupported')!r}")
            R.check(bool((o.get("ProfilesSupported") or "").strip()),
                    f"ProfilesSupported = {o.get('ProfilesSupported')!r}")
            for path in ("tt:Encoding", "tt:QualityRange/tt:Min",
                         "tt:QualityRange/tt:Max", "tt:ResolutionsAvailable",
                         "tt:BitrateRange/tt:Min", "tt:BitrateRange/tt:Max"):
                R.check(o.find(path, NS) is not None,
                        f"Options 里有 {path}")

    # ---- [8] 未实现的操作仍然是结构良好的 Fault --------------------------
    print("\n[8] Media2 未实现的操作 → ONVIF Fault")
    st, data = soap(MED2, envelope(op_body("GetAnalyticsEngines", NS_MED2)))
    R.check(st == 500, f"HTTP {st}（期望 500 Fault）", data[:200])
    root = R.strict_xml(data, "Media2 GetAnalyticsEngines Fault")
    if root is not None:
        code = R.find(root, ".//soap:Code/soap:Value")
        R.check(code is not None and code.text == "soap:Receiver",
                f"Fault code = {code.text if code is not None else None}")
        sub = R.find(root, ".//soap:Subcode/soap:Value")
        R.check(sub is not None and sub.text == "ter:ActionNotSupported",
                f"Subcode = {sub.text if sub is not None else None}")

    print(f"\n结果：通过 {R.passed}，失败 {R.failed}，跳过 {R.skipped}")
    return 0 if R.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
