#!/usr/bin/env python3
"""量一段从 RTSP 拉下来、解成 WAV 的音频：是不是那条真正推出去的 1 kHz 正弦。

为什么非要"量波形"这一层，而不是看 SDP 或者看 ffprobe 报的编码参数：
这一路上三种错都**不影响 SDP**（见 tests/rtsp_audio_check.cpp 的文件头）——

  1. 裸 AU 外面又套了一层 ADTS 头（陷阱 76）：收端每个包多一个假头，
     解出来是噪音，而 `a=rtpmap:97 MPEG4-GENERIC/48000/2` 一字不差。
  2. 呈现时间的锚点用负 pts 当哨兵：每个 AU 的时间戳都等于投递时刻，
     收端要么没声音、要么一顿一顿。
  3. 音频挂成了第二个 ServerMediaSession（陷阱 77）：NVR 只看到视频 ——
     这一条由 ps1 那边的"同一个会话里两个流"断言负责，不在这里。

所以判据是波形本身，一共五条：

  * 声道数 / 采样率 与 SDP 一致（2 / 48000）
  * **时长够**（不是"拿到了一小段"）：RTP 的节奏错掉时最常见的结果是
    所有 AU 挤在同一瞬间被推出去，解出来只有几十毫秒
  * 整段的有效值**非静音**（-30 dBFS 以上）
  * 过零率数出来的频率 ≈ 1 kHz（±5%）
  * **切成一格一格看**，每一格都得有声音、量级还得和整体一致 ——
    这一条才是"一顿一顿"那个症状的判据：只看整段的 RMS，把声音全部
    挤到前 100 ms 里也能通过

以及一条与"是不是音调"有关的：波峰因数（peak/rms）只判**上界**。
有损编码器保的是能量不是峰值，低码率下峰值会被抬高（陷阱 84：64 kbps 上
实测 +3.9 dB），拿"峰值应当等于输入幅值"当判据会把正确的实现报成失败。

用法（由 tests/rtsp_audio_check.ps1 调用）：
  python -I -X utf8 tests/rtsp_audio_probe.py out\\rtsp-audio\\pulled.wav
"""

import argparse
import math
import struct
import sys
import wave

SAMPLE_RATE = 48000
CHANNELS = 2
TONE_HZ = 1000.0

# 至少要有这么长。ffmpeg 那边拉 6 秒，RTP 正常时到手就是 6 秒左右；
# 时间戳全挤成一堆的话会短得离谱（几十毫秒）。
MIN_SECONDS = 4.0
# 有效值下限，满量程 32768 的 -30 dBFS 左右。
MIN_RMS = 1000.0
# 频率容差。过零率在低码率 AAC 上有毛刺，1 kHz 上实测误差 0.08%，
# 给 5% 是为了留够余量而仍然能分辨"1000 Hz"和"500 Hz / 2000 Hz"。
FREQ_TOL_PCT = 5.0
# 分格的一致性：每一格的 RMS 要落在整体的这个倍数区间里。
BUCKET_LO = 0.5
BUCKET_HI = 2.0
BUCKET_SECONDS = 0.1
# 波峰因数上界。正弦是 1.414，AAC 64 kbps 实测 1.44；白噪音是 3~4。
MAX_CREST = 2.2


def read_wav(path):
    with wave.open(path, "rb") as w:
        ch = w.getnchannels()
        rate = w.getframerate()
        width = w.getsampwidth()
        n = w.getnframes()
        raw = w.readframes(n)
    if width != 2:
        raise SystemExit("[FAIL] 只认 16 位 PCM，这份是 %d 字节/样本" % width)
    total = n * ch
    samples = struct.unpack("<%dh" % total, raw[: total * 2])
    return ch, rate, n, samples


def rms(values):
    if not values:
        return 0.0
    return math.sqrt(sum(float(v) * v for v in values) / len(values))


def zero_cross_freq(values, rate):
    """过零率数出来的频率。正弦每周期过零两次。"""
    if len(values) < 2:
        return 0.0
    crossings = 0
    for i in range(1, len(values)):
        if (values[i - 1] < 0) != (values[i] < 0):
            crossings += 1
    seconds = len(values) / float(rate)
    if seconds <= 0:
        return 0.0
    return crossings / 2.0 / seconds


def dbfs(x):
    if x <= 0:
        return float("-inf")
    return 20.0 * math.log10(x / 32768.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav", help="ffmpeg 从 rtsp://… -map 0:a 拉出来的 WAV")
    ap.add_argument("--tone-hz", type=float, default=TONE_HZ,
                    help="推出去的那条正弦的频率（默认 1000）")
    args = ap.parse_args()

    ch, rate, n, samples = read_wav(args.wav)
    seconds = n / float(rate) if rate else 0.0
    left = samples[0::ch]
    right = samples[1::ch] if ch > 1 else left

    print("读到 %s：%d 声道 %d Hz %.3f 秒 (%d 样本/声道)"
          % (args.wav, ch, rate, seconds, n))

    # 一个样本都没有时**当场说清楚，不要往下走**：后面每一步都会在空序列上
    # 炸出 Traceback，而 Traceback 读起来像"这把尺子坏了"，不像"被测的东西
    # 没出声"。实测这个形状真的出现过 —— 给每个裸 AU 前面套一个假 ADTS 头
    # （陷阱 76 那个变异）时，ffmpeg 每个包都报 `Error decoding AAC frame
    # header`，写出来的就是 0 秒的 WAV。
    if n == 0:
        print("-" * 64)
        print("[FAIL] 一个样本都没解出来 —— 收端没拿到可解的数据"
              "（上游 ffmpeg 那边应当已经报了 `Error decoding AAC frame header`）")
        return 1

    failures = []

    if ch != CHANNELS:
        failures.append("声道数是 %d，SDP 的 config 说的是 %d" % (ch, CHANNELS))
    if rate != SAMPLE_RATE:
        failures.append("采样率是 %d，SDP 说的是 %d" % (rate, SAMPLE_RATE))
    if seconds < MIN_SECONDS:
        failures.append("只解出 %.3f 秒（要求 ≥ %.1f）—— 时间戳很可能是挤在一起的"
                        % (seconds, MIN_SECONDS))

    rms_l = rms(left)
    rms_r = rms(right)
    peak = max(max(abs(v) for v in left), max(abs(v) for v in right))
    print("有效值 L %.1f (%.1f dBFS) / R %.1f (%.1f dBFS)，峰值 %d，波峰因数 %.3f"
          % (rms_l, dbfs(rms_l), rms_r, dbfs(rms_r), peak,
             (peak / rms_l) if rms_l > 0 else 0.0))

    if rms_l < MIN_RMS:
        failures.append("左声道有效值 %.1f 低于 %.0f —— 解出来基本是静音"
                        % (rms_l, MIN_RMS))
    if rms_r < MIN_RMS:
        failures.append("右声道有效值 %.1f 低于 %.0f —— 解出来基本是静音"
                        % (rms_r, MIN_RMS))

    # 频率用**中间那一段**算，避开起播那几十毫秒的补积压（实测环回首秒
    # 只有 72% 实时，那一小段是引擎起转时的补数据，不是稳态）。
    if seconds >= 3.0:
        a = int(rate * 1.0)
        b = int(rate * min(seconds - 0.5, 4.0))
    else:
        a, b = 0, len(left)
    freq = zero_cross_freq(left[a:b], rate)
    err_pct = abs(freq - args.tone_hz) / args.tone_hz * 100.0 if args.tone_hz else 0.0
    print("过零率数出的频率 %.1f Hz（期望 %.0f，偏差 %.2f%%）"
          % (freq, args.tone_hz, err_pct))
    if err_pct > FREQ_TOL_PCT:
        failures.append("频率 %.1f Hz 偏离 %.0f Hz 达到 %.2f%% —— 要么不是那条正弦，"
                        "要么包被解错了" % (freq, args.tone_hz, err_pct))

    # 分格一致性：这是"一顿一顿 / 全挤在一瞬间"的判据。
    step = max(1, int(rate * BUCKET_SECONDS))
    buckets = []
    for i in range(0, len(left) - step + 1, step):
        buckets.append(rms(left[i:i + step]))
    silent = [i for i, v in enumerate(buckets) if v < rms_l * BUCKET_LO]
    loud = [i for i, v in enumerate(buckets) if v > rms_l * BUCKET_HI]
    print("分格（%.1f 秒一格）共 %d 格：最小 %.1f，最大 %.1f，整体 %.1f"
          % (BUCKET_SECONDS, len(buckets),
             min(buckets) if buckets else 0.0,
             max(buckets) if buckets else 0.0, rms_l))
    if silent:
        failures.append("有 %d 格（共 %d 格）明显轻于整体，第一格在第 %d 格 —— "
                        "音轨是断续的" % (len(silent), len(buckets), silent[0]))
    if loud:
        failures.append("有 %d 格（共 %d 格）明显重于整体，第一格在第 %d 格 —— "
                        "声音挤成一堆了" % (len(loud), len(buckets), loud[0]))

    crest = (peak / rms_l) if rms_l > 0 else 0.0
    if crest > MAX_CREST:
        failures.append("波峰因数 %.3f 超过 %.1f —— 这更像噪音而不是音调"
                        % (crest, MAX_CREST))

    print("-" * 64)
    for f in failures:
        print("[FAIL] " + f)
    if failures:
        return 1
    print("[ OK ] 解出来的是一条 %.0f Hz、%.2f 秒、逐格均匀的非静音音调"
          % (freq, seconds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
