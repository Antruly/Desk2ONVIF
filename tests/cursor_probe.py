#!/usr/bin/env python3
"""Verify that the mouse pointer really ends up in the recorded video.

Why a pixel probe rather than "look at the frame": every failure mode of cursor
compositing is quiet. A wrong AND/XOR order paints a black-and-white blob, a
wrong bit order mirrors the shape, and forgetting to erase the old position
leaves a trail of arrows. All three still produce a video that plays, has the
right resolution and framerate, and shows a cursor in it. The only thing that
tells them apart is *which pixels* differ.

This expects a recording made by tests/cursor_harness.ps1: a solid-colour
fullscreen canvas with the pointer walking a known path. That is what makes the
"which pixels" question answerable at all. On a real desktop it is not:

    tolerance  8: 288235 pixels differ between two frames, 1.3% near the pointer
    tolerance 48:   1146 pixels differ, 41.7% near the pointer
    tolerance 96:    812 pixels differ, 55.5% near the pointer, bbox x[47..2840]

Compression ringing around text swings isolated pixels from black to white at
ultrafast, so *magnitude* cannot separate the pointer from the noise -- both
jump the full range. A flat canvas removes the high frequencies there is
anything to ring on, and then the deviation from the background colour *is* the
pointer.

With the deviation being exactly the pointer, the assertions get strong:

  1. every frame deviates from the canvas somewhere   -> the pointer is drawn
  2. every frame deviates in exactly ONE place        -> the old one was erased
     (a missing erase leaves a second blob, and they accumulate step by step)
  3. that one blob is pointer-sized, not a smear
  4. the first frame holds the start point, the last
     frame holds the end point                        -> position maps correctly
  5. the blob only ever advances along the path       -> no stale/frozen frames

Usage:
  python -I tests/cursor_probe.py out/cursor-flat.mp4 \
      --bg 0,102,204 --from 300,300 --to 1200,900
"""

import argparse
import os
import subprocess
import sys

import numpy as np

# A pointer viewed against a flat field. Generous on both ends: shapes vary with
# DPI and pointer scheme, and an anti-aliased fringe partly falls under TOL.
MIN_PIXELS = 60
MAX_PIXELS = 2500
MAX_BBOX = 128           # a blob bigger than this is a smear, not a pointer
CHANNEL_TOLERANCE = 24   # flat field: x264's noise floor here is essentially 0

# Anti-aliased cursor edges can dip under the threshold and cut a thin diagonal
# shape into pieces. Dilating before labelling closes those hairline gaps; a
# genuine un-erased trail is ~100 px away and stays a separate blob.
DILATE = 2

# The centroid of an arrow is not its hotspot, so the expected position is
# compared against the blob's centroid with slack.
POSITION_SLACK = 40


def find_ffmpeg():
    here = os.path.dirname(os.path.abspath(__file__))
    cand = os.path.join(here, "..", "third_party", "ffmpeg", "bin", "ffmpeg.exe")
    if os.path.exists(cand):
        return cand
    return "ffmpeg"


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
    raise SystemExit("[FAIL] 读不出视频尺寸：\n" + text[:600])


def decode_frames(ffmpeg, path, w, h):
    """Yield every frame as an (h, w, 4) BGRA array, in order."""
    cmd = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-i", path,
        "-f", "rawvideo", "-pix_fmt", "bgra", "-",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    want = w * h * 4
    while True:
        buf = proc.stdout.read(want)
        if len(buf) < want:
            break
        yield np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 4)
    proc.stdout.close()
    err = proc.stderr.read().decode("utf-8", "replace")
    proc.wait()
    if proc.returncode != 0:
        raise SystemExit("[FAIL] ffmpeg 解码失败：\n" + err[:600])


def dilate(mask, radius):
    """Binary dilation, without numpy's wrap-around (which would join the two
    screen edges into one blob)."""
    h, w = mask.shape
    out = mask.copy()
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            if dy == 0 and dx == 0:
                continue
            src = mask[max(0, dy):h + min(0, dy), max(0, dx):w + min(0, dx)]
            dst = out[max(0, -dy):h + min(0, -dy), max(0, -dx):w + min(0, -dx)]
            dst |= src
    return out


def blobs(mask, radius):
    """Connected components of the deviating pixels, as lists of (x, y).

    Labelling runs on the dilated mask so hairline gaps don't split one pointer
    into two; the returned pixel lists are the *undilated* pixels belonging to
    each blob, so the size and bbox describe what was really drawn.
    """
    joined = dilate(mask, radius)
    ys, xs = np.nonzero(joined)
    pending = set(zip(xs.tolist(), ys.tolist()))
    out = []
    while pending:
        seed = pending.pop()
        group = [seed]
        stack = [seed]
        while stack:
            x, y = stack.pop()
            for ny in range(y - 1, y + 2):
                for nx in range(x - 1, x + 2):
                    if (nx, ny) in pending:
                        pending.discard((nx, ny))
                        stack.append((nx, ny))
                        group.append((nx, ny))
        gx = np.fromiter((p[0] for p in group), dtype=np.int32, count=len(group))
        gy = np.fromiter((p[1] for p in group), dtype=np.int32, count=len(group))
        real = mask[gy, gx]
        if real.any():          # dilated-only pixels mean this was all fringe
            out.append((gx[real], gy[real]))
    return out


def runs(records):
    """Collapse consecutive frames holding the same pointer position."""
    out = []
    for i, (cx, cy, n) in enumerate(records):
        if out and out[-1][2] == (cx, cy):
            out[-1][1] = i
            continue
        out.append([i, i, (cx, cy), n])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--bg", required=True,
                    help="画布底色，格式 R,G,B（与 cursor_harness.ps1 的 -Color 一致）")
    ap.add_argument("--from", dest="from_", required=True,
                    help="第一帧里指针应在的位置，格式 x,y")
    ap.add_argument("--to", required=True,
                    help="最后一帧里指针应在的位置，格式 x,y")
    args = ap.parse_args()

    bg = np.array([int(v) for v in args.bg.split(",")], dtype=np.int16)
    if bg.shape != (3,):
        raise SystemExit("--bg 要写成 R,G,B")

    def point(s):
        p = s.split(",")
        if len(p) != 2:
            raise SystemExit("坐标要写成 x,y，收到的是 '%s'" % s)
        return int(p[0]), int(p[1])

    start, end = point(args.from_), point(args.to)

    ffmpeg = find_ffmpeg()
    w, h = probe_size(ffmpeg, args.video)
    print("视频 %s  %dx%d  画布 RGB(%s)" % (args.video, w, h, args.bg))
    print("指针应当从 %s 走到 %s" % (start, end))

    failures = []
    records = []
    for i, frame in enumerate(decode_frames(ffmpeg, args.video, w, h)):
        # BGRA in the file; the canvas colour is written RGB.
        bgr = frame[:, :, :3].astype(np.int16)
        dev = np.abs(bgr - bg[::-1]).max(axis=2) > CHANNEL_TOLERANCE
        found = blobs(dev, DILATE)
        if len(found) != 1:
            failures.append(
                "第 %d 帧里有 %d 团非背景像素，应该有且只有 1 团 —— "
                "多出来的那团是上一次没擦掉的指针（残留会一步一步累积）"
                % (i, len(found)))
            records.append((0, 0, 0))
            continue
        gx, gy = found[0]
        n = len(gx)
        cx, cy = int(gx.mean()), int(gy.mean())
        records.append((cx, cy, n))

        if n < MIN_PIXELS:
            failures.append("第 %d 帧的非背景像素只有 %d 个，比一个指针该有的少"
                            % (i, n))
        elif n > MAX_PIXELS:
            failures.append("第 %d 帧有 %d 个非背景像素 —— 那不是指针，是一片涂抹"
                            % (i, n))
        bw = int(gx.max() - gx.min()) + 1
        bh = int(gy.max() - gy.min()) + 1
        if bw > MAX_BBOX or bh > MAX_BBOX:
            failures.append("第 %d 帧的色块有 %dx%d 那么大，指针装不下"
                            % (i, bw, bh))

    if not records:
        raise SystemExit("[FAIL] 一帧都没解出来 —— 文件是坏的或者空的")

    print("\n共 %d 帧。指针停留过的位置：" % len(records))
    for first, last, (cx, cy), n in runs(records):
        span = ("帧 %d" % first + ("" if first == last else "~%d" % last))
        print("  %-12s 质心 (%4d,%4d)  %4d 像素" % (span, cx, cy, n))

    if any(r[2] == 0 for r in records):
        failures.append("有整帧一个非背景像素都没有 —— 指针没画上去")
    else:
        def near(a, b):
            return abs(a[0] - b[0]) <= POSITION_SLACK and abs(a[1] - b[1]) <= POSITION_SLACK

        if not near(records[0][:2], start):
            failures.append("第一帧的指针在 %s，应该在 %s 附近" % (records[0][:2], start))
        if not near(records[-1][:2], end):
            failures.append("最后一帧的指针在 %s，应该在 %s 附近" % (records[-1][:2], end))

        # Only ever advancing: a frozen frame (the pointer stuck while the
        # mouse moves) or a replayed stale buffer shows up as going backwards.
        def along(p):
            return abs(p[0] - start[0]) + abs(p[1] - start[1])

        progress = [along(r[:2]) for r in records]
        for i in range(1, len(progress)):
            if progress[i] < progress[i - 1] - 4:
                failures.append("第 %d 帧的指针往回退了（%d -> %d）—— 拿到了旧画面"
                                % (i, progress[i - 1], progress[i]))
                break

        distinct = len({(r[0] // 4, r[1] // 4) for r in records})
        if distinct < 4:
            failures.append("整段视频里指针只出现在 %d 个不同的位置上 —— "
                            "它没有跟着鼠标走" % distinct)

    print("-" * 64)
    for f in failures:
        print("[FAIL] " + f)
    if failures:
        return 1
    print("[ OK ] 指针画在正确的位置上、跟着鼠标走，且旧位置每次都擦干净了")
    return 0


if __name__ == "__main__":
    sys.exit(main())
