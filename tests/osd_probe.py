#!/usr/bin/env python3
"""Verify that the OSD watermark is really burned into the recorded video.

Why a pixel probe rather than "look at the frame": every way the OSD can be
wrong still produces a video that plays, has the right resolution and frame
rate, and shows text on it. The four that matter:

  * the adaptive colour picks the wrong direction (invisible on one background)
  * the box is anchored to the wrong corner (text half off-screen)
  * the clock freezes (the mask cache never invalidates)
  * nothing composites at all, and what looks like an OSD is the desktop's own
    clock in the taskbar

Telling those apart needs *which pixels* differ and *what colour* they are.

This expects a recording made by tests/osd_harness.ps1: a flat two-tone canvas,
dark on top and light on the bottom. Flat is the same trick cursor_probe uses:
on a real desktop, compression ringing around text swings pixels across the
full range, so *magnitude* cannot separate OSD ink from noise. On a flat field
there is nothing to ring on, and the deviation from the canvas *is* the
watermark.

Two measuring modes
-------------------

**默认（角上那一对）**：按 `tests/osd_harness.ps1` 的默认配置量两个角，
断言

  1. the upper-left ink is much BRIGHTER than its (dark) background
  2. the lower-right ink is much DARKER than its (light) background
     -> adaptive colour chose opposite directions on the two backgrounds
  3. the upper-left text differs between two samples 3 s apart
     -> the clock is running *inside the encoded video*
  4. the lower-right box is in the same place in both samples (within 2 px)
     -> fixed text does not drift, so (3) is the clock and not layout jitter

**`--ink-pct X,Y` / `--blank-corner tl`**：水印被**搬到别处**时用。给出水印
锚点的百分比（就是配置里 `osd.items[n].x_pct/y_pct` 那两个数），探针在画面
的 `w*X/100, h*Y/100` 处开一个窗口，断言那里**有墨**、配色方向对、而且墨迹的
左上角就在锚点附近（一个文字高以内）；同时断言 `--blank-corner` 指的那个角
**一个墨迹像素都没有** —— 后半句才是"搬走了"这件事的判据：只验新位置有字
的话，一个"在两个位置都画"的实现照样绿。

Measuring "the watermark" means measuring a *tight window at each corner*, not
the whole quadrant. This probe runs against a live desktop, where the quadrant
also contains whatever else is topmost at that moment -- a floating IME pill, a
toast, and the mouse pointer itself (which a hand can move anywhere mid-run).
Those are not the OSD's doing and must not be attributed to it; a probe that
lumps them in reports them as "the watermark moved", which is the failure it
would then never catch for real (see the trap table: a false failure is worse
than no test). Ink outside the windows is *reported* but is not an assertion.

(4) is also the only end-to-end proof of the "static desktop" path: the canvas
never changes, so every frame after the first one exists *only* because the OSD
text changed. If that path were broken the later samples would be missing or
frozen -- which is exactly the failure it is here to catch. (The pointer moves
independently of the canvas and produces frames of its own, so the canvas being
static is necessary but not sufficient; the direct evidence is the
`水印刷新` counter in the harness log.)

--selftest 是**量具的自校**（陷阱 82：一个用来量东西的工具，本身要先被量
一遍）。它不碰 ffmpeg、不碰桌面，只喂合成帧给 `analyse()` / `judge()` ——
**同一个**函数，不是重写的一份 —— 然后断言每一种"该红的"情形真的红、每一个
"该绿的"情形真的绿。少掉它的话，下面这些断言在搬位置那次改动里一起失效了也
没人知道（陷阱 85：怎么都绿的断言等于没验）。

Usage:
  python -I -X utf8 tests/osd_probe.py out/osd-harness.mp4 \
      --dark 20,20,24 --light 240,240,236 --split 0.5 \
      --cursor 2520,480 --at 1.0 --at 4.0

  python -I -X utf8 tests/osd_probe.py out/osd-harness.mp4 \
      --dark 20,20,24 --light 240,240,236 --split 0.5 \
      --ink-pct 55,60 --blank-corner tl --at 1.0 --at 4.0

  python -I -X utf8 tests/osd_probe.py --selftest
"""

import argparse
import json
import os
import subprocess
import sys

import numpy as np

# Flat field: x264's noise floor here is essentially zero. Same value the
# cursor probe uses.
TOLERANCE = 24

# A deviating blob bigger than this fraction of the frame is a smear, not text.
MAX_INK_FRACTION = 0.02

# How much brighter/darker the ink must be than the canvas under it. Adaptive
# colour has to be *decisive* -- a 30-level difference would be a wash.
#
# 这条判据的边界要知道（它是**判"墨迹够不够分明"**，不是"自适应选的方向对不对"）：
# `outline: true` 时前景选反了仍会留一圈**反色描边**，黑块白边或白块黑边的平均
# 亮度都在 128 上下，两边都过得了这一关。真正把"选反了"照出来的是**墨迹本身
# 看不见** —— 那时报的是"这里一个像素都没变"（见 osd_harness 的 outline 开关）。
# 所以这条留着，但它管的是"墨够不够清楚"，别把它当成配色方向的判据。
MIN_CONTRAST = 60

DARK_CEILING = 90     # "this background counts as dark"
LIGHT_FLOOR = 170     # "this background counts as light"

# Sample times are 3 s apart, so the seconds digit must have moved. Compared as
# a fraction of the box so the assertion does not depend on how wide the glyphs
# are.
MIN_CLOCK_CHANGE = 0.005

# The fixed-text box may not move at all between samples.
BOX_SLACK = 2

# Upper bounds on the blob that may legitimately sit outside the windows:
# the mouse pointer. Comfortably above a 64x64 large cursor scheme, far below
# any run of OSD text (auto font = frame height / 32 = 60 px on this canvas,
# and a clock is many glyphs wide).
POINTER_MAX = 80
POINTER_MAX_PIXELS = 4096

# The measuring window at each corner. Big enough for the largest thing the
# default config can draw there -- auto font is frame height / 32 (60 px here)
# and the two default items are a date+time clock (603x48 measured at
# 2880x1920) and a short device name -- with room to spare for a longer name.
# Small enough that a pointer parked elsewhere in the quadrant, an IME
# floating pill, or a toast cannot intrude: 1200x200 is 4% of this frame.
REGION_W = 1200
REGION_H = 200

# `--ink-pct` 那个窗口的默认尺寸。比角上那个大：搬到别处的那一项可能是**手动
# 放大过字号**的（Phase P 的判据之一就是字号真的要能改），96 px 的日期时间在
# 2880 宽的画面上量到 ~965 px。
INK_W = 1400
INK_H = 320

# 锚点左边和上边留的余量。掩码的左上角**就是**锚点，墨迹从掩码内的
# (kHaloPad + 侧边距) 处才开始，所以真墨迹只会落在锚点的右下方 —— 留一点余量
# 是为了容下抗锯齿和编码的模糊，不是为了让判据变松。
ANCHOR_HEAD = 40

# 窗口内画布颜色允许的波动。超过就说明这个窗口横跨了暗/亮分界线，那么"这里
# 该是白字还是黑字"这件事本身就没有答案，判据不成立（不是产品的问题，是
# 调用方把窗口开在了分界线上）。
CANVAS_SLACK = 4


def find_ffmpeg():
    here = os.path.dirname(os.path.abspath(__file__))
    cand = os.path.join(here, "..", "third_party", "ffmpeg", "bin", "ffmpeg.exe")
    if os.path.exists(cand):
        return cand
    return "ffmpeg"


def find_ffprobe(ffmpeg):
    cand = os.path.join(os.path.dirname(ffmpeg), "ffprobe.exe")
    return cand if os.path.exists(cand) else "ffprobe"


def probe_size(ffmpeg, path):
    cmd = [ffmpeg, "-hide_banner", "-i", path]
    err = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stderr
    text = err.decode("utf-8", "replace")
    for line in text.splitlines():
        if "Video:" in line and "x" in line:
            for token in line.replace(",", " ").split():
                if "x" in token and token.split("x")[0].isdigit():
                    w, h = token.split("x")[:2]
                    if h.isdigit():
                        return int(w), int(h)
    raise SystemExit("[FAIL] read no video size:\n" + text[:600])


def decode_frames(ffmpeg, path, w, h):
    """Yield (pts_seconds, (h, w, 4) BGRA array) for every frame, in order.

    PTS is taken from ffprobe rather than assumed: on a static canvas the OSD
    path emits roughly one frame per second while PTS keeps advancing at the
    configured rate, so frame index and time are *not* the same thing -- and
    that separation is exactly what makes the clock assertion meaningful.
    """
    ffprobe = find_ffprobe(ffmpeg)
    cmd = [ffprobe, "-v", "error", "-select_streams", "v:0",
           "-show_entries", "frame=pts_time", "-of", "csv=p=0", path]
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    times = []
    for line in out.stdout.decode("utf-8", "replace").splitlines():
        line = line.strip().rstrip(",")
        if line:
            times.append(float(line))

    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", path,
           "-f", "rawvideo", "-pix_fmt", "bgra", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    want = w * h * 4
    i = 0
    while True:
        buf = proc.stdout.read(want)
        if len(buf) < want:
            break
        t = times[i] if i < len(times) else float(i) / 25.0
        yield t, np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 4)
        i += 1
    proc.stdout.close()
    err = proc.stderr.read().decode("utf-8", "replace")
    proc.wait()
    if proc.returncode != 0:
        raise SystemExit("[FAIL] ffmpeg decode failed:\n" + err[:600])


def ideal_canvas(w, h, dark, light, split):
    """The flat two-tone field the harness painted, as BGR (h, w, 3)."""
    cut = int(round(h * split))
    canvas = np.zeros((h, w, 3), dtype=np.int16)
    canvas[:cut] = dark[::-1]        # the harness gives RGB; frames are BGR
    canvas[cut:] = light[::-1]
    return canvas


def luma(bgr):
    """BT.601 integer weights, the same ones osd_overlay.cpp uses.

    Takes BGRA/BGR arrays; a single colour works too, since the weights are
    applied to the last axis.
    """
    a = np.asarray(bgr).astype(np.int32)
    return (299 * a[..., 2] + 587 * a[..., 1] + 114 * a[..., 0]) // 1000


def bbox(ys, xs):
    return int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())


def rect_mask(w, h, x0, y0, x1, y1):
    m = np.zeros((h, w), dtype=bool)
    m[max(0, y0):max(0, y1), max(0, x0):max(0, x1)] = True
    return m


def corner_window(w, h, corner, rw, rh):
    if corner == "tl":
        x0, y0 = 0, 0
    elif corner == "tr":
        x0, y0 = w - rw, 0
    elif corner == "bl":
        x0, y0 = 0, h - rh
    else:
        x0, y0 = w - rw, h - rh
    x0, y0 = max(0, x0), max(0, y0)
    return x0, y0, min(w, x0 + rw), min(h, y0 + rh)


class Ctx:
    """一次判定需要的全部上下文。

    main() 从命令行拼它，`--selftest` 用合成值拼它 —— **下游是同一份代码**。
    量具自校如果自己走另一条路，它证明的只是"另一条路是对的"。
    """

    def __init__(self, w, h, dark, light, split, cursor, mode, regions, wanted):
        self.w = w
        self.h = h
        self.dark = dark
        self.light = light
        self.split = split
        self.cursor = cursor
        self.mode = mode
        self.regions = regions
        self.wanted = wanted


def build_regions(w, h, dark, light, split, ink_pcts, blank_corners, ink_w, ink_h):
    """把命令行给出的窗口变成掩码 + 每个窗口的判据参数。

    返回 (mode, regions)。没给任何窗口参数就是 legacy：两个角，判据照旧。
    """
    if not ink_pcts and not blank_corners:
        regions = []
        for name, label in (("tl", "左上"), ("br", "右下")):
            x0, y0, x1, y1 = corner_window(w, h, name, REGION_W, REGION_H)
            regions.append({"name": name, "kind": "legacy-" + name, "label": label,
                            "mask": rect_mask(w, h, x0, y0, x1, y1),
                            "rect": (x0, y0, x1, y1),
                            "ax": None, "ay": None, "bg": None, "uniform": True})
        return "legacy", regions

    canvas = ideal_canvas(w, h, dark, light, split)
    regions = []
    for i, (xp, yp) in enumerate(ink_pcts):
        # **和 osd_overlay.cpp 的 custom 分支逐字对应**：锚点是掩码左上角，
        # 位置是 `dst_w * x_pct / 100`（整数截断，没有 margin）。两边写不一样的
        # 话，症状是"预览在左上、录出来偏右下"。
        ax, ay = w * xp // 100, h * yp // 100
        x0, y0 = max(0, ax - ANCHOR_HEAD), max(0, ay - ANCHOR_HEAD)
        x1, y1 = min(w, ax + ink_w), min(h, ay + ink_h)
        sub = luma(canvas[y0:y1, x0:x1])
        regions.append({
            "name": "ink%d" % i, "kind": "ink", "label": "水印锚点 %d%%,%d%%" % (xp, yp),
            "mask": rect_mask(w, h, x0, y0, x1, y1),
            "rect": (x0, y0, x1, y1),
            "ax": ax, "ay": ay, "xp": xp, "yp": yp,
            "uniform": int(sub.max() - sub.min()) <= CANVAS_SLACK,
            "bg": int(sub.mean()),
        })
    for i, corner in enumerate(blank_corners):
        x0, y0, x1, y1 = corner_window(w, h, corner, REGION_W, REGION_H)
        regions.append({
            "name": "blank%d" % i, "kind": "blank",
            "label": "%s 角（宽 %d 高 %d）" % (corner, x1 - x0, y1 - y0),
            "mask": rect_mask(w, h, x0, y0, x1, y1),
            "rect": (x0, y0, x1, y1),
            "ax": None, "ay": None, "bg": None, "uniform": True,
        })
    return "explicit", regions


def analyse(frame, w, h, dark, light, split, cursor, regions):
    """Split one frame's deviation-from-canvas into the named windows."""
    canvas = ideal_canvas(w, h, dark, light, split)
    dev = np.abs(frame[:, :, :3].astype(np.int16) - canvas).max(axis=2) > TOLERANCE
    luma_img = luma(frame[:, :, :3])

    # 区域掩码是**全屏二维布尔**，不是"非零像素的下标列表"。用下标列表的话，
    # 两次采样的掩码长度不同（秒数一变，墨迹像素数就跟着变），
    # `mask_a ^ mask_b` 直接广播失败 —— 而"钟在走"恰恰只能靠这个异或判。
    #
    # 窗口贴着角、按 REGION_W/REGION_H 定尺寸，**不是整个象限**：象限里同时
    # 住着这一刻最上层的任何东西（输入法的悬浮标、气泡通知），以及鼠标指针
    # 本身 —— 而指针是有人碰了鼠标就会自己跑掉的。把那些算进"水印"里，报出来
    # 的失败是"水印位置在飘"，而真正的位置错误反而再也测不出来。
    covered = np.zeros((h, w), dtype=bool)
    for r in regions:
        covered |= r["mask"]

    info = {
        "total": 0,
        "pointer": None,
        "pointer_drift": None,
        "outside": 0,
        "outside_bbox": None,
        "outside_is_pointer": False,
        "foreign_bbox": None,
        "covered_regions": [],
        "regions": {},
    }

    # 窗口之外的东西一律**只报告、不断言**。它可能是指针（捕获层把 DXGI
    # 单独给的指针形状自己合成上去），也可能是别的窗口 —— 两者都属于"当时
    # 屏幕上的现状"，不是 OSD 的作为。按**尺寸**把指针认出来：水印的自动字号
    # 是帧高/32（本画布 = 60 像素高、几十个字符宽），而指针是 22x35 上下的
    # 一小块，差别是结构性的。
    outer = dev & ~covered
    if outer.any():
        oy, ox = np.nonzero(outer)
        x0, y0 = int(ox.min()), int(oy.min())
        x1, y1 = int(ox.max()), int(oy.max())
        bw, bh = x1 - x0 + 1, y1 - y0 + 1
        info["outside"] = int(len(ox))
        info["outside_bbox"] = (x0, y0, x1, y1)
        info["outside_is_pointer"] = (bw <= POINTER_MAX and bh <= POINTER_MAX
                                      and len(ox) <= POINTER_MAX_PIXELS)
        if info["outside_is_pointer"]:
            info["pointer"] = {"bbox": (x0, y0, x1, y1), "count": int(len(ox))}
            if cursor is not None:
                info["pointer_drift"] = max(abs(x0 - cursor[0]),
                                            abs(y0 - cursor[1]))
        else:
            # 比指针大的东西 ⇒ 当时屏幕上有别的窗口。**它有没有压住测量窗口，
            # 决定了这一帧还能不能拿来判水印。** 压住的那一半像素落进了
            # `covered` 里（所以不在 `outer` 里），会被上面那段当成"墨迹" ——
            # 实测过一次：录制中途 Windows 的开始菜单弹了出来，写在
            # 55%,60% 的水印整个被盖住，探针报的是"窗口里的像素有 183237 个，
            # 那不是水印，是一整片涂抹"。**那句话的宾语错了**：它不是水印画的，
            # 是开始菜单画的；而这条断言本来要抓的是"水印糊成一团"。
            info["foreign_bbox"] = (x0, y0, x1, y1)
        # 窗口之外的一律从 `dev` 里摘掉，免得它混进下面 "整片涂抹" 那条判据。
        # 注意**不做**外扩：外扩会把紧挨着窗口的指针像素连带挖进窗口里一点点，
        # 而窗口里本来就该只有水印 —— 真落进窗口的东西不该被悄悄抹掉。
        dev &= ~outer

    # 用**别的窗口的包围盒**去量哪些测量窗口被压住了。这个判据只能抓住
    # "那个东西伸到了窗口外面"的情况（伸出来才会进 `outer`、才有包围盒）；
    # 一个恰好只覆盖窗口、一点不外溢的东西看起来和水印糊成一团完全一样，
    # 那种情况这里故意不管 —— 管它就得放宽"整片涂抹"那条判据，而那条是
    # 用来抓真故障的。**宁可漏报成失败，不可把失败放宽成通过。**
    if info["foreign_bbox"] is not None:
        fx0, fy0, fx1, fy1 = info["foreign_bbox"]
        for r in regions:
            rx0, ry0, rx1, ry1 = r["rect"]
            if fx0 <= rx1 and rx0 <= fx1 and fy0 <= ry1 and ry0 <= fy1:
                info["covered_regions"].append(r["name"])

    info["total"] = int(dev.sum())
    if not info["total"]:
        return info

    for r in regions:
        mask = dev & r["mask"]
        if not mask.any():
            continue
        gy, gx = np.nonzero(mask)
        info["regions"][r["name"]] = {
            "bbox": bbox(gy, gx),
            "count": int(mask.sum()),
            "luma_min": int(luma_img[gy, gx].min()),
            "luma_max": int(luma_img[gy, gx].max()),
            "luma_mean": float(luma_img[gy, gx].mean()),
            "mask": mask,
        }
    return info


def judge(samples, ctx):
    """判定取到的几帧。返回 (failures, lines)。

    `--selftest` 直接拿 failures 去对账，所以**判据只有这一份** —— 自校和实跑
    走的是同一段代码。
    """
    w, h = ctx.w, ctx.h
    failures, lines = [], []

    for t in sorted(samples):
        info = samples[t]
        lines.append("\n[t=%.1fs] 水印墨迹 %d 个像素（窗口内）" % (t, info["total"]))
        p = info["pointer"]
        if p is not None:
            x0, y0, x1, y1 = p["bbox"]
            line = "  指针 %dx%d @(%d,%d)，%d 个像素（窗口外，已摘掉）" % (
                x1 - x0 + 1, y1 - y0 + 1, x0, y0, p["count"])
            if info["pointer_drift"] is not None:
                line += "，离 --cursor 差了 %d 像素" % info["pointer_drift"]
            lines.append(line)
        elif info["outside"] > 0:
            x0, y0, x1, y1 = info["outside_bbox"]
            lines.append("  窗口外还有 %d 个像素（%dx%d @(%d,%d)）—— 比指针大，"
                         "当作当时屏幕上的其它窗口，不计入断言"
                         % (info["outside"], x1 - x0 + 1, y1 - y0 + 1, x0, y0))
        elif ctx.cursor is not None:
            lines.append("  指针没找到（大概是移出画面了）")

        # 屏幕上有别的东西压在测量窗口上时，这一帧**对那几个窗口不作证**。
        # 实测两次（开始菜单、Alt-Tab 切换器），两次都被下面"整片涂抹"那条判据
        # 报成"水印糊成一团"—— 那句话的宾语错了，动手的不是水印。**判据一条都
        # 不放宽**：只是把被压住的窗口从"这一帧要判的东西"里摘出去，并且如实
        # 报出"这一帧没判成"，而不是判它通过。
        cov = list(info["covered_regions"])
        if cov:
            fx0, fy0, fx1, fy1 = info["foreign_bbox"]
            lines.append("  [无法判定] 有别的东西（%dx%d @(%d,%d)）压住了 %s —— "
                         "这一帧对这几个窗口不作证"
                         % (fx1 - fx0 + 1, fy1 - fy0 + 1, fx0, fy0,
                            "、".join(cov)))

        if info["total"] > int(w * h * MAX_INK_FRACTION):
            if cov:
                lines.append("  （窗口里那 %d 个像素里混着上面那个东西，"
                             "“整片涂抹”这条不判 —— 它多半是别人的窗口画的）"
                             % info["total"])
                continue
            failures.append("t=%.1fs：窗口里的像素有 %d 个，超过全屏的 %.0f%% "
                            "—— 那不是水印，是一整片涂抹"
                            % (t, info["total"], MAX_INK_FRACTION * 100))
            continue

        if ctx.mode == "legacy":
            tl = None if "tl" in cov else info["regions"].get("tl")
            br = None if "br" in cov else info["regions"].get("br")
            if "tl" in cov:
                pass                      # 上面已经报过"无法判定"
            elif tl is None:
                failures.append("t=%.1fs：左上角一个像素都没变 —— 时间水印没画上去" % t)
            else:
                x0, y0, x1, y1 = tl["bbox"]
                lines.append("  左上  色块 %dx%d @(%d,%d)  墨迹亮度 %.0f..%.0f（均值 %.0f）"
                             % (x1 - x0 + 1, y1 - y0 + 1, x0, y0,
                                tl["luma_min"], tl["luma_max"], tl["luma_mean"]))
                bg = int(luma(ctx.dark))
                if tl["luma_mean"] - bg < MIN_CONTRAST:
                    failures.append("t=%.1fs：左上角墨迹的平均亮度 %.0f 只比暗底 %d "
                                    "亮 %.0f —— 自适应配色没有选白字"
                                    % (t, tl["luma_mean"], bg,
                                       tl["luma_mean"] - bg))
                if x0 > w // 4 or y0 > h // 8:
                    failures.append("t=%.1fs：时间水印贴着 (%d,%d)，不在左上角"
                                    % (t, x0, y0))
            if "br" in cov:
                pass                      # 同上
            elif br is None:
                failures.append("t=%.1fs：右下角一个像素都没变 —— 名称那一项没画"
                                "上去（`DeviceState` 没接到 `SessionOptions` 的"
                                "话，就是这个症状）" % t)
            else:
                x0, y0, x1, y1 = br["bbox"]
                lines.append("  右下  色块 %dx%d @(%d,%d)  墨迹亮度 %.0f..%.0f（均值 %.0f）"
                             % (x1 - x0 + 1, y1 - y0 + 1, x0, y0,
                                br["luma_min"], br["luma_max"], br["luma_mean"]))
                bg = int(luma(ctx.light))
                if bg - br["luma_mean"] < MIN_CONTRAST:
                    failures.append("t=%.1fs：右下角墨迹的平均亮度 %.0f 只比亮底 %d "
                                    "暗 %.0f —— 自适应配色没有选黑字"
                                    % (t, br["luma_mean"], bg,
                                       bg - br["luma_mean"]))
                if x1 < w * 3 // 4 or y1 < h * 7 // 8:
                    failures.append("t=%.1fs：名称那一项收在 (%d,%d)，不贴右下角"
                                    % (t, x1, y1))
            continue

        for r in ctx.regions:
            if r["name"] in cov:
                continue                  # 这一帧对它不作证（上面已报）
            got = info["regions"].get(r["name"])
            if r["kind"] == "blank":
                if got is None:
                    lines.append("  %s  空的（0 个像素），符合预期" % r["label"])
                else:
                    x0, y0, x1, y1 = got["bbox"]
                    failures.append("t=%.1fs：%s 本该一个像素都没有，却有 %d 个"
                                    "（%dx%d @(%d,%d)）—— 水印还留在老位置上"
                                    % (t, r["label"], got["count"],
                                       x1 - x0 + 1, y1 - y0 + 1, x0, y0))
                continue
            if got is None:
                failures.append("t=%.1fs：%s 里一个像素都没变 —— 水印没画到那个"
                                "位置（锚点在画面的 %d%%,%d%% 处）"
                                % (t, r["label"], r["xp"], r["yp"]))
                continue
            x0, y0, x1, y1 = got["bbox"]
            ink_h = y1 - y0 + 1
            lines.append("  %s  色块 %dx%d @(%d,%d)  墨迹亮度 %.0f..%.0f（均值 %.0f）"
                         % (r["label"], x1 - x0 + 1, ink_h, x0, y0,
                            got["luma_min"], got["luma_max"], got["luma_mean"]))
            lines.append("    锚点 (%d,%d)，墨迹左上角偏 (%d,%d) 像素（墨迹高 %d）"
                         % (r["ax"], r["ay"], abs(x0 - r["ax"]), abs(y0 - r["ay"]),
                            ink_h))
            if not r["uniform"]:
                failures.append("t=%.1fs：%s 这个窗口横跨了两种底色 —— 该是白字"
                                "还是黑字在这里没有答案，把 --ink-pct 挪开分界"
                                "线再量（这条指探针的用法，不是产品）"
                                % (t, r["label"]))
            elif r["bg"] < 128:
                if got["luma_mean"] - r["bg"] < MIN_CONTRAST:
                    failures.append("t=%.1fs：%s 墨迹平均亮度 %.0f 只比暗底 %d "
                                    "亮 %.0f —— 自适应配色没有选白字"
                                    % (t, r["label"], got["luma_mean"], r["bg"],
                                       got["luma_mean"] - r["bg"]))
            else:
                if r["bg"] - got["luma_mean"] < MIN_CONTRAST:
                    failures.append("t=%.1fs：%s 墨迹平均亮度 %.0f 只比亮底 %d "
                                    "暗 %.0f —— 自适应配色没有选黑字"
                                    % (t, r["label"], got["luma_mean"], r["bg"],
                                       r["bg"] - got["luma_mean"]))
            # 锚点是掩码左上角，墨迹从掩码内的留边处才开始，所以真墨迹只会落在
            # 锚点的右下方、偏一个"留边 + 侧边距"。**判据用墨迹自己的高当尺子**：
            # 字号是可配的（0 到 512），写死像素数会在放大字号的那一跑上假红，
            # 而"位置公式整个算错"的偏差是几百像素，拿一个文字高去卡绰绰有余。
            if abs(x0 - r["ax"]) > ink_h or abs(y0 - r["ay"]) > ink_h:
                failures.append("t=%.1fs：%s 的墨迹左上角 (%d,%d) 离锚点 (%d,%d) "
                                "偏了 (%d,%d) 像素，超过一个文字高（%d）—— "
                                "位置公式和实际画出来的对不上"
                                % (t, r["label"], x0, y0, r["ax"], r["ay"],
                                   abs(x0 - r["ax"]), abs(y0 - r["ay"]), ink_h))

    a, b = samples[ctx.wanted[0]], samples[ctx.wanted[1]]

    # "钟在走"要挑一个**随时间变**的窗口比：legacy 是左上那个时间项；显式
    # 模式下是第一个 `--ink-pct` 窗口（调用方保证那一项是 time 类型）。
    clock_names = [r["name"] for r in ctx.regions
                   if r["kind"] in ("legacy-tl", "ink")]
    if clock_names:
        name = clock_names[0]
        ga, gb = a["regions"].get(name), b["regions"].get(name)
        if name in a["covered_regions"] or name in b["covered_regions"]:
            # 钟走没走这条判据要**两帧都不被压住**才成立：任何一帧被压住，
            # 掩码的异或里就混进了别人的像素，报出来的会是"秒数冻住了"。
            lines.append("\n%s：两次采样里有一次被别的东西压住了 —— "
                         "“钟在走”这条不判" % name)
        elif ga and gb:
            # 按**位置**比，不按颜色比：秒数变了就是那几个数字像素从"有墨"变成
            # "没墨"（或反过来），所以取两个位置掩码的异或。
            changed = int((ga["mask"] ^ gb["mask"]).sum())
            denom = max(1, int(ga["count"] + gb["count"]))
            lines.append("\n%s：取自 PTS %.2fs 与 %.2fs，墨迹像素 %d 与 %d，"
                         "位置不同的 %d 个"
                         % (name, a["pts"], b["pts"], ga["count"], gb["count"],
                            changed))
            if changed / float(denom) < MIN_CLOCK_CHANGE:
                failures.append("相隔 %.1f 秒的两帧里，%s 一模一样 —— 静止桌面上"
                                "秒数冻住了（掩码缓存没失效）"
                                % (ctx.wanted[1] - ctx.wanted[0], name))

    if ctx.mode == "legacy":
        ga, gb = a["regions"].get("br"), b["regions"].get("br")
        if "br" in a["covered_regions"] or "br" in b["covered_regions"]:
            lines.append("右下角：“位置飘没飘”这条也不判（有一次被压住了）")
        elif ga and gb:
            box_a, box_b = ga["bbox"], gb["bbox"]
            drift = max(abs(p - q) for p, q in zip(box_a, box_b))
            lines.append("右下角：名称那一项的包围盒两次采样相差最多 %d 像素" % drift)
            if drift > BOX_SLACK:
                failures.append("名称那一项的包围盒在两次采样之间移动了 %d 像素 "
                                "—— 水印的位置在飘，上面那条“秒数变了”就不是钟在走"
                                % drift)

    return failures, lines


def dump(samples, failures):
    out = {"failures": failures, "samples": {}}
    for t, info in samples.items():
        one = {"pts": info["pts"], "total": info["total"], "regions": {},
               # `covered` 是驱动脚本用来**重录一次**的依据；`foreign_bbox` 是重录
               # 之后仍然失败时，能指着说"当时屏幕上是这个东西"的唯一线索。
               "covered": list(info["covered_regions"]),
               "foreign_bbox": (list(info["foreign_bbox"])
                                if info["foreign_bbox"] is not None else None)}
        for name, got in info["regions"].items():
            one["regions"][name] = {"bbox": list(got["bbox"]), "count": got["count"],
                                    "luma_mean": got["luma_mean"]}
        out["samples"][str(t)] = one
    return out


# --------------------------------------------------------------------------
# --selftest：量具自校
# --------------------------------------------------------------------------

def synth(w, h, dark, light, split, blobs):
    """合成一帧：平坦画布 + 若干实心方块（代替烧进去的文字）。

    方块是实心的，真实字形不是 —— 这一层差别**不影响**要校的那些判据：探针量
    的是"偏离底色的像素在哪、什么颜色"，与像素怎么来的无关。反过来说，这里校
    不了"字形的抗锯齿会不会被编码糊掉"，那一条只能靠真实录制那几跑。
    """
    frame = np.zeros((h, w, 4), dtype=np.uint8)
    frame[:, :, 3] = 255
    frame[:, :, :3] = ideal_canvas(w, h, dark, light, split).astype(np.uint8)
    for b in blobs:
        x0, y0 = b["x"], b["y"]
        frame[y0:y0 + b["h"], x0:x0 + b["w"], :3] = np.array(b["rgb"][::-1],
                                                            dtype=np.uint8)
    return frame


def selftest():
    w, h = 2880, 1920
    dark = np.array([20, 20, 24], dtype=np.int16)
    light = np.array([240, 240, 236], dtype=np.int16)
    split = 0.5
    white = (255, 255, 255)
    black = (0, 0, 0)

    xp, yp = 55, 60
    ax, ay = w * xp // 100, h * yp // 100
    ink_blob = {"x": ax + 8, "y": ay + 10, "w": 500, "h": 72, "rgb": black}

    legacy_mode, legacy_regions = build_regions(w, h, dark, light, split, [], [],
                                                INK_W, INK_H)
    ink_mode, ink_regions = build_regions(w, h, dark, light, split, [(xp, yp)],
                                          ["tl"], INK_W, INK_H)
    assert legacy_mode == "legacy" and ink_mode == "explicit"

    def run(name, frames, mode, regions, expect_fail, needle=None,
            expect_covered=None):
        ctx = Ctx(w, h, dark, light, split, None, mode, regions, [])
        samples = {}
        times = []
        for i, f in enumerate(frames):
            t = 1.0 + 3.0 * i
            info = analyse(f, w, h, dark, light, split, None, regions)
            info["pts"] = t
            samples[t] = info
            times.append(t)
        ctx.wanted = sorted(times)
        failures, _ = judge(samples, ctx)
        ok = (len(failures) > 0) == expect_fail
        if ok and needle is not None:
            ok = any(needle in f for f in failures)
        if ok and expect_covered is not None:
            # "被别的东西压住了"这条路**也要有一节自校**：不可判定的判据要是
            # 从来没被走到过，它和"永远绿"没有区别（陷阱 85）。
            got = any(s["covered_regions"] for s in samples.values())
            ok = (got == expect_covered)
            if not ok:
                print("[selftest]   （covered 期望 %s，实际 %s，窗口 %s）"
                      % (expect_covered, got,
                         [s["covered_regions"] for s in samples.values()]))
        want = "报错" if expect_fail else "通过"
        print("[selftest] %-38s 期望%s → %s  %s"
              % (name, want, "OK" if ok else "FAIL",
                 ("；".join(failures)[:150]) if failures else "无失败"))
        return ok

    plain = synth(w, h, dark, light, split, [])
    # 默认那一对：左上白字（暗底）、右下黑字（亮底）
    left1 = {"x": 100, "y": 50, "w": 600, "h": 48, "rgb": white}
    # 第二帧把左边那一块挪 30 px —— 等价于"秒数变了"：掩码位置变了
    left2 = {"x": 130, "y": 50, "w": 600, "h": 48, "rgb": white}
    right = {"x": w - 700, "y": h - 150, "w": 500, "h": 50, "rgb": black}
    painted1 = synth(w, h, dark, light, split, [left1, right])
    painted2 = synth(w, h, dark, light, split, [left2, right])

    # 搬走那一跑：只在锚点处画，**左上角什么都不画**
    moved1 = synth(w, h, dark, light, split, [ink_blob])
    # 第二帧在方块中间挖个洞 —— 包围盒不动、掩码变了，正是"钟在走而位置不飘"
    hole = synth(w, h, dark, light, split, [])
    moved2 = moved1.copy()
    moved2[ay + 40:ay + 55, ax + 200:ax + 260, :3] = \
        hole[ay + 40:ay + 55, ax + 200:ax + 260, :3]
    # 窗口外一小块 —— 指针的形状。只报告，不该影响任何断言
    pointer = {"x": 2520, "y": 400, "w": 22, "h": 35, "rgb": white}
    moved1p = synth(w, h, dark, light, split, [ink_blob, pointer])
    moved2p = moved2.copy()
    moved2p[400:435, 2520:2542, :3] = np.array(white[::-1], dtype=np.uint8)

    print("量具自校（%dx%d，暗底 RGB%s / 亮底 RGB%s）" % (w, h, dark, light))
    print("锚点 %d%%,%d%% = (%d,%d)\n" % (xp, yp, ax, ay))

    results = [
        # 1. 什么都没画：两个角都该报"没画上去"
        run("空画面 · 默认两个角", [plain, plain], legacy_mode, legacy_regions, True,
            "左上角一个像素都没变"),
        # 2. 该绿的：两个角都有墨、配色方向相反、钟在走、名称不飘
        run("画好了 · 默认两个角", [painted1, painted2], legacy_mode,
            legacy_regions, False),
        # 3. 变异：两帧一模一样 ⇒ "钟在走"必须红（否则那条断言是摆设）
        run("钟冻住 · 默认两个角", [painted1, painted1], legacy_mode,
            legacy_regions, True, "秒数冻住了"),
        # 4. 该绿的：搬到锚点处、左上角空着、窗口外有指针
        run("搬到 55%,60% · 老位置空着", [moved1p, moved2p], ink_mode, ink_regions,
            False),
        # 5. 变异：老位置还留着字 ⇒ "一个像素都没有"必须红
        run("老位置还留着水印", [synth(w, h, dark, light, split, [ink_blob, left1]),
                                 synth(w, h, dark, light, split, [ink_blob, left2])],
            ink_mode, ink_regions, True, "本该一个像素都没有"),
        # 6. 变异：墨迹对底色不够分明（灰字压在亮底上）⇒ 对比度判据必须红。
        #    注意这里**不能**用"亮底上画纯白字"来构造：255 与 240 只差 15 级，
        #    在 TOLERANCE=24 以下，量具根本看不见它 —— 那时报的是"这里没有墨"，
        #    而不是"配色选错了"。灰字（206）与亮底差 34 级，看得见、但不够分明，
        #    正好落在这一条判据该管的地方。
        run("墨迹对底色不够分明",
            [synth(w, h, dark, light, split,
                   [dict(ink_blob, rgb=(206, 206, 206))]),
             synth(w, h, dark, light, split,
                   [dict(ink_blob, rgb=(206, 206, 206))])],
            ink_mode, ink_regions, True, "没有选黑字"),
        # 7. 变异：整个窗口涂满 ⇒ "整片涂抹"必须红。（黑块压在亮底上：差 240 级）
        run("窗口里一整片涂抹",
            [synth(w, h, dark, light, split,
                   [{"x": ax - ANCHOR_HEAD, "y": ay - ANCHOR_HEAD, "w": INK_W,
                     "h": INK_H, "rgb": black}]),
             synth(w, h, dark, light, split,
                   [{"x": ax - ANCHOR_HEAD, "y": ay - ANCHOR_HEAD, "w": INK_W,
                     "h": INK_H, "rgb": black}])],
            ink_mode, ink_regions, True, "一整片涂抹"),
        # 8. 变异：锚点整个挪到别处（位置公式和实际画的对不上）⇒ 必须红
        run("墨迹不在锚点上",
            [synth(w, h, dark, light, split,
                   [dict(ink_blob, x=ax + 600, y=ay + 200)]),
             synth(w, h, dark, light, split,
                   [dict(ink_blob, x=ax + 600, y=ay + 200)])],
            ink_mode, ink_regions, True, "位置公式和实际画出来的对不上"),
        # 9. 屏幕上别的窗口压在测量窗口上（实测过的真实情形：开始菜单、Alt-Tab
        #    切换器）⇒ **一条失败都不许报**，但必须报出"这一帧不作证"。
        #    这个方块特意做得比窗口大：只有**伸出窗口**的那部分才会进 `outer`、
        #    才会有包围盒，才判得出"谁压住了谁"。压在窗口里、一点不外溢的东西
        #    与"水印糊成一团"长得一模一样，那种情况故意不认（见 analyse 里那段）。
        #    把 expect_covered 那一半拿掉的话，这个方块会在"整片涂抹"上报红 ——
        #    而这正是这次要修掉的那个误报。
        run("别的窗口压住了测量窗口",
            [synth(w, h, dark, light, split,
                   [{"x": ax - 200, "y": ay - 100, "w": 1600, "h": 600,
                     "rgb": (0, 255, 0)}]),
             synth(w, h, dark, light, split,
                   [{"x": ax - 200, "y": ay - 100, "w": 1600, "h": 600,
                     "rgb": (0, 255, 0)}])],
            ink_mode, ink_regions, False, None, True),
    ]

    print("-" * 64)
    if all(results):
        print("[ OK ] 量具有刻度：%d 种情形该红的红了、该绿的绿了" % len(results))
        return 0
    print("[FAIL] 有 %d 种情形对不上 —— 先用合成图把刻度量准，"
          "再拿它去量真实录制" % (results.count(False)))
    return 1


# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video", nargs="?")
    ap.add_argument("--dark", help="upper canvas colour, R,G,B")
    ap.add_argument("--light", help="lower canvas colour, R,G,B")
    ap.add_argument("--split", type=float, default=0.5,
                    help="fraction of the height that is dark (0..1)")
    ap.add_argument("--at", type=float, action="append", default=[],
                    help="sample time in seconds; give twice, 3 s apart")
    ap.add_argument("--cursor", default=None, metavar="X,Y",
                    help="where osd_harness.ps1 parked the pointer; reported "
                         "against, not enforced (the pointer can be moved by "
                         "hand mid-recording)")
    ap.add_argument("--ink-pct", action="append", default=[], metavar="X,Y",
                    help="水印锚点在画面上的百分比（= 配置里那一项的 "
                         "x_pct/y_pct）；断言这里**有墨**且位置对得上。给了它"
                         "就不再走默认的两个角")
    ap.add_argument("--ink-w", type=int, default=INK_W)
    ap.add_argument("--ink-h", type=int, default=INK_H)
    ap.add_argument("--blank-corner", action="append", default=[],
                    choices=["tl", "tr", "bl", "br"],
                    help="这个角上的窗口必须**一个墨迹像素都没有** —— "
                         "水印搬走之后的老位置")
    ap.add_argument("--dump-json", metavar="PATH",
                    help="把每帧的包围盒与失败清单写成 JSON，供驱动脚本对账"
                         "（跨两次录制比字号这类事，探针自己看不见）")
    ap.add_argument("--selftest", action="store_true",
                    help="不碰 ffmpeg：用合成帧自校量具，然后退出")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not args.video:
        ap.error("要么给一个视频，要么 --selftest")
    if not args.dark or not args.light:
        ap.error("--dark 和 --light 必给（判据要拿它们当基准）")
    if len(args.at) != 2:
        raise SystemExit("--at 要给两次（相隔 3 秒的两个时刻）")

    def rgb(s):
        p = [int(v) for v in s.split(",")]
        if len(p) != 3:
            raise SystemExit("颜色要写成 R,G,B，收到的是 '%s'" % s)
        return np.array(p, dtype=np.int16)

    def pair(s, what):
        p = [int(v) for v in s.split(",")]
        if len(p) != 2:
            raise SystemExit("%s 要写成 X,Y，收到的是 '%s'" % (what, s))
        return p[0], p[1]

    dark, light = rgb(args.dark), rgb(args.light)
    cursor = pair(args.cursor, "--cursor") if args.cursor else None
    ink_pcts = [pair(s, "--ink-pct") for s in args.ink_pct]

    # 先量尺子再量东西。两个底色要是取得一样（或者都偏中灰），下面"墨迹比
    # 底色亮/暗"那两条断言就永远成立，而这条测试会安静地什么都不验。
    d_luma, l_luma = int(luma(dark)), int(luma(light))
    if d_luma > DARK_CEILING:
        raise SystemExit("[FAIL] --dark 的亮度是 %d，不够暗（要 <= %d）—— "
                         "自适应配色测不出来" % (d_luma, DARK_CEILING))
    if l_luma < LIGHT_FLOOR:
        raise SystemExit("[FAIL] --light 的亮度是 %d，不够亮（要 >= %d）—— "
                         "自适应配色测不出来" % (l_luma, LIGHT_FLOOR))

    ffmpeg = find_ffmpeg()
    w, h = probe_size(ffmpeg, args.video)
    mode, regions = build_regions(w, h, dark, light, args.split, ink_pcts,
                                  args.blank_corner, args.ink_w, args.ink_h)

    print("视频 %s  %dx%d" % (args.video, w, h))
    print("画布 上半 RGB(%s) / 下半 RGB(%s)，分界在 %.0f%% 处"
          % (args.dark, args.light, args.split * 100))
    if mode == "legacy":
        print("判据 默认两个角（左上白字 / 右下黑字）")
    else:
        print("判据 " + "；".join(
            [r["label"] + "（必须有墨）" if r["kind"] == "ink"
             else r["label"] + "（必须空）" for r in regions]))

    wanted = sorted(args.at)
    samples = {}
    total_frames = 0
    first_t = last_t = 0.0
    for t, frame in decode_frames(ffmpeg, args.video, w, h):
        if total_frames == 0:
            first_t = t
        last_t = t
        total_frames += 1
        for want in wanted:
            if want not in samples and t >= want:
                samples[want] = analyse(frame, w, h, dark, light, args.split,
                                        cursor, regions)
                samples[want]["pts"] = t   # 记录实际取自哪一帧，便于对账
    if total_frames == 0:
        raise SystemExit("[FAIL] 一帧都没解出来 —— 文件是坏的或者空的")
    print("共 %d 帧，PTS %.2f..%.2f 秒" % (total_frames, first_t, last_t))

    missing = [t for t in wanted if t not in samples]
    if missing:
        raise SystemExit("[FAIL] 视频里没有 %.1f 秒之后的帧 —— 静止桌面上"
                         "水印时钟没有产出新帧（录像比 %.1f 秒短）"
                         % (missing[0], missing[0]))

    ctx = Ctx(w, h, dark, light, args.split, cursor, mode, regions, wanted)
    failures, lines = judge(samples, ctx)
    for line in lines:
        print(line)

    if args.dump_json:
        with open(args.dump_json, "w", encoding="utf-8") as f:
            json.dump(dump(samples, failures), f, ensure_ascii=False, indent=1)

    print("-" * 64)
    for f in failures:
        print("[FAIL] " + f)
    if failures:
        # **硬失败优先**：有真断言红了就报红，不让"无法判定"把它盖过去 ——
        # 一帧里同时有真故障和被压住的窗口时，报"无法判定"等于把真故障藏起来。
        return 1

    # 一个窗口都没判成、也没有真失败 ⇒ **这一跑什么也没证明**。退出码 2 是给
    # 驱动脚本看的："重录一次，别把我当成通过"。这不是"跳过"的礼貌说法 ——
    # 探针**确实判定不了**，把它算成绿会让整条测试变成一个永远不会红的摆设
    # （陷阱 85 的同一个形状）。
    blocked = sorted({n for t in samples for n in samples[t]["covered_regions"]})
    if blocked:
        print("[ ?? ] 这一跑没判成：%s 被屏幕上别的东西压住了（%s）。"
              "水印到底画没画、画在哪，这一跑回答不了 —— 重录一次，"
              "或者自己去看那几帧"
              % ("、".join(blocked),
                 "；".join("%.1fs %s" % (t, samples[t]["foreign_bbox"])
                           for t in sorted(samples)
                           if samples[t]["foreign_bbox"] is not None)))
        return 2

    if mode == "legacy":
        print("[ OK ] 水印烧进了画面：左上白字/右下黑字，且钟在走、名称不飘")
    else:
        ink = [r for r in regions if r["kind"] == "ink"]
        blank = [r for r in regions if r["kind"] == "blank"]
        print("[ OK ] 水印烧进了画面：%s 有字、%s 空着，且钟在走"
              % ("/".join(r["label"] for r in ink),
                 "/".join(r["label"] for r in blank) if blank else "（没指定空窗口）"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
