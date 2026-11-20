#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: check tests/scripts/envmod.txt in build/host/volna.wav: VELO -> A.ATK +40, so the soft note (30, at 2.5 s)
reaches 80 % of its peak within ~0.03 s, the hard one (127, at 3.5 s) only after ~0.15 s or more (attack 2 + 80:
~0.4 s to full). Exit 1 if not."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
win = int(fs * 0.005)


def rise(t0, t1):                               # from the note to 80 % of its peak, s
    lv = [math.sqrt(sum(x * x for x in d[a:a + win]) / win) for a in range(int(t0 * fs), int(t1 * fs) - win, win)]
    peak = max(lv)
    return next(i for i, v in enumerate(lv) if v >= 0.8 * peak) * win / fs


soft, hard = rise(2.5, 3.3), rise(3.5, 4.5)
ok = soft < 0.03 and hard > 0.15
print(f"VELO -> A.ATK: the soft note's attack {soft * 1000:.0f} ms, the hard one's {hard * 1000:.0f} ms {'ok' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
