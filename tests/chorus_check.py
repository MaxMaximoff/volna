#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna stage 7b: check tests/scripts/chorus.txt in build/host/volna.wav (a 220 Hz sine, held).
  no effect: the level steady (rms in 20 ms windows within 5 %: they are not whole cycles), the sides the same;
  CHORUS, MIX 64: the dry sine and its swinging copy beat: the level swings (> 15 %); the sides differ;
  FLANGER, FEEDBK +40: the sides differ, the level within reason (no runaway: under 4 x dry's, not silent).
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
raw = struct.unpack("<%dh" % (2 * n), w.readframes(n))
L, R = raw[0::2], raw[1::2]


def rms(d, t0, t1):
    s = d[int(t0 * fs):int(t1 * fs)]
    return math.sqrt(sum(x * x for x in s) / len(s))


def swing(t0, t1):
    v = [rms(L, t, t + 0.02) for t in [t0 + 0.02 * k for k in range(int((t1 - t0) / 0.02))]]
    return (max(v) - min(v)) / (sum(v) / len(v))


def corr(t0, t1):
    a, b = L[int(t0 * fs):int(t1 * fs)], R[int(t0 * fs):int(t1 * fs)]
    return sum(x * y for x, y in zip(a, b)) / math.sqrt(sum(x * x for x in a) * sum(y * y for y in b))


bad = 0


def check(what, ok, msg):
    global bad
    bad += not ok
    print(f"{what}: {msg} {'ok' if ok else 'FAIL'}")


s0, c0, r0 = swing(1.8, 2.3), corr(1.8, 2.3), rms(L, 1.8, 2.3)
check("no effect", s0 < 0.05 and c0 > 0.999, f"the level swings {s0:.3f}, the sides' correlation {c0:.3f}")
s1, c1 = swing(3.3, 5.3), corr(3.3, 5.3)
check("CHORUS", s1 > 0.15 and c1 < 0.95, f"the level swings {s1:.2f}, the sides' correlation {c1:.2f}")
c2, r2 = corr(6.3, 7.9), rms(L, 6.3, 7.9)
check("FLANGER +40", c2 < 0.95 and 0.2 * r0 < r2 < 4 * r0, f"the sides' correlation {c2:.2f}, level x{r2 / r0:.2f} of dry")
sys.exit(1 if bad else 0)
