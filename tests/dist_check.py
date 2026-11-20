#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna stage 7b: check tests/scripts/dist.txt in build/host/volna.wav (a 220 Hz sine, the left channel).
  no effect: the 2nd and the 3rd harmonics under 1 % of the 1st;
  DIST SOFT x 16: the 3rd over 5 %, the 2nd still under 1 % (a symmetric curve), the level within x 0.5 .. x 2 of dry;
  DIST TUBE: the 2nd over 3 %.
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
F = 220.0


def harm(t0, t1, k):
    a, b = int(t0 * fs), int(t1 * fs)
    re = sum(d[i] * math.cos(2 * math.pi * k * F * i / fs) for i in range(a, b))
    im = sum(d[i] * math.sin(2 * math.pi * k * F * i / fs) for i in range(a, b))
    return math.hypot(re, im) * 2 / (b - a)


def rms(t0, t1):
    s = d[int(t0 * fs):int(t1 * fs)]
    return math.sqrt(sum(x * x for x in s) / len(s))


bad = 0


def check(what, ok, msg):
    global bad
    bad += not ok
    print(f"{what}: {msg} {'ok' if ok else 'FAIL'}")


r = {}
for name, t0 in (("dry", 1.8), ("soft", 3.3), ("tube", 4.8)):
    h = [harm(t0, t0 + 0.4, k) for k in (1, 2, 3)]
    r[name] = (h[1] / h[0], h[2] / h[0], rms(t0, t0 + 0.4))
    print(f"  {name}: 2nd {r[name][0]:.3f}, 3rd {r[name][1]:.3f} of the 1st, rms {r[name][2]:.0f}")
check("no effect", r["dry"][0] < 0.01 and r["dry"][1] < 0.01, "a clean sine")
check("SOFT", r["soft"][1] > 0.05 and r["soft"][0] < 0.01 and 0.5 < r["soft"][2] / r["dry"][2] < 2,
      f"3rd {r['soft'][1]:.3f}, 2nd {r['soft'][0]:.3f}, level x{r['soft'][2] / r['dry'][2]:.2f}")
check("TUBE", r["tube"][0] > 0.03, f"2nd {r['tube'][0]:.3f}")
sys.exit(1 if bad else 0)
