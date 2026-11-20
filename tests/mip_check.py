#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna stage 8: check tests/scripts/mip.txt in build/host/volna.wav: a sawtooth at C7 and C8, no filter. The
energy off the note's harmonics against theirs (a fit at the exact frequency of the phase step) under -30 dB: the
oscillator plays a band-limited level (through level 0 alone: about -14 and -10 dB). Exit 1 if not."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
INC_C9 = [815363807, 863847862, 915214929, 969636441, 1027294024, 1088380105,
          1153098554, 1221665363, 1294309365, 1371273005, 1452813141, 1539201906]   # (volna.c)


def alias(note, t0, t1):
    f = (INC_C9[note % 12] >> (10 - note // 12)) * fs / 2 ** 32
    m = int(round(int((t1 - t0) * f) * fs / f))  # (whole cycles: the fit's own error far under what it measures)
    y = d[int(t0 * fs):int(t0 * fs) + m]
    tot = sum(v * v for v in y)
    har = 0.0
    h = 1
    while h * f < fs / 2:
        c = sum(v * math.cos(2 * math.pi * h * f * i / fs) for i, v in enumerate(y))
        s = sum(v * math.sin(2 * math.pi * h * f * i / fs) for i, v in enumerate(y))
        har += 2 * (c * c + s * s) / len(y)
        h += 1
    return f, 10 * math.log10(max(tot - har, 1e-9) / har)


bad = 0
for note, t0 in ((96, 1.8), (108, 3.3)):
    f, a = alias(note, t0, t0 + 0.5)
    ok = a < -30
    bad += not ok
    print(f"saw at {f:.0f} Hz: aliasing {a:.1f} dB {'ok' if ok else 'FAIL'}")
sys.exit(1 if bad else 0)
