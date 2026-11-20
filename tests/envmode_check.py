#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna stage 4b: check the envelope modes in build/host/volna.wav after tests/scripts/envmode.txt.
  LOOP S1S2 (1.6 .. 3.4 s, A3 held): the level in 25 ms windows swings (S1 100 against S2 20: ~x5 in amplitude);
  ONE SHOT: a 50 ms tap at 4.0 sounds on clearly longer than the same tap in ADSR at 5.0 (the decays, then the
  release, whatever the key did). Exit 1 if one is off."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
win = int(fs * 0.025)


def rms(a):
    seg = d[a:a + win]
    return math.sqrt(sum(x * x for x in seg) / len(seg))


def lasts(t0, t1, frac=0.02):                   # how long after t0 the level stays above frac of its peak
    lv = [rms(a) for a in range(int(t0 * fs), int(t1 * fs) - win, win)]
    peak = max(lv)
    last = max(i for i, v in enumerate(lv) if v > frac * peak)
    return (last + 1) * win / fs


loop = [rms(a) for a in range(int(1.6 * fs), int(3.4 * fs) - win, win)]
sw = max(loop) / max(min(loop), 1.0)
bad = 0
ok = sw > 3.0
bad += not ok
print(f"LOOP S1S2: the level swings x{sw:.2f} (S1 100 against S2 20) {'ok' if ok else 'FAIL'}")
one, adsr = lasts(4.0, 4.95), lasts(5.0, 5.95)
ok = one > adsr + 0.15
bad += not ok
print(f"ONE SHOT: a 50 ms tap lasts {one:.2f} s, in ADSR {adsr:.2f} s {'ok' if ok else 'FAIL'}")
sys.exit(1 if bad else 0)
