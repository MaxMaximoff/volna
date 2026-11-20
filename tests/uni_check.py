#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna stage 7a: check tests/scripts/unison.txt in build/host/volna.wav (a sine, A3 220 Hz), both channels.
  UNISON 1: left = right, 220 Hz;
  UNISON 2, DETUNE 127, SPREAD 127: each copy alone on its side: 220 x 2^(-1/12) left, x 2^(1/12) right;
  UNISON 7, SPREAD 0: left = right, the level near one copy's (1/sqrt(7) each), no sample at full scale;
  SPREAD 127: left and right differ (correlation well under 1), about as loud.
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
raw = struct.unpack("<%dh" % (2 * n), w.readframes(n))
L, R = raw[0::2], raw[1::2]


def seg(d, t0, t1):
    return d[int(t0 * fs):int(t1 * fs)]


def freq(d, t0, t1):
    s = seg(d, t0, t1)
    xs = [i - 1 + s[i - 1] / (s[i - 1] - s[i]) for i in range(1, len(s)) if s[i - 1] < 0 <= s[i]]
    return (len(xs) - 1) * fs / (xs[-1] - xs[0]) if len(xs) > 2 else 0.0


def rms(d, t0, t1):
    s = seg(d, t0, t1)
    return math.sqrt(sum(x * x for x in s) / len(s))


def corr(t0, t1):
    a, b = seg(L, t0, t1), seg(R, t0, t1)
    return sum(x * y for x, y in zip(a, b)) / math.sqrt(sum(x * x for x in a) * sum(y * y for y in b))


bad = 0


def check(what, ok, msg):
    global bad
    bad += not ok
    print(f"{what}: {msg} {'ok' if ok else 'FAIL'}")


same1 = seg(L, 1.6, 1.9) == seg(R, 1.6, 1.9)
f1 = freq(L, 1.6, 1.9)
check("UNISON 1", same1 and 218 < f1 < 222, f"left = right {same1}, {f1:.1f} Hz")
fl, fr = freq(L, 3.2, 3.7), freq(R, 3.2, 3.7)
check("UNISON 2, DETUNE 127, SPREAD 127", abs(fl - 207.65) < 1.0 and abs(fr - 233.08) < 1.0,
      f"left {fl:.2f} Hz (want 207.65), right {fr:.2f} (want 233.08)")
same7 = seg(L, 4.6, 5.2) == seg(R, 4.6, 5.2)
r1, r7 = rms(L, 1.6, 1.9), rms(L, 4.6, 5.2)
pk = max(abs(x) for x in seg(L, 4.5, 5.3))
check("UNISON 7, SPREAD 0", same7 and 0.6 < r7 / r1 < 1.6 and pk < 32000,
      f"left = right {same7}, level x{r7 / r1:.2f} of one copy, peak {pk}")
c = corr(6.1, 6.7)
rl, rr = rms(L, 6.1, 6.7), rms(R, 6.1, 6.7)
check("UNISON 7, SPREAD 127", c < 0.8 and 0.7 < rl / rr < 1.4, f"correlation {c:.2f}, left / right x{rl / rr:.2f}")
sys.exit(1 if bad else 0)
