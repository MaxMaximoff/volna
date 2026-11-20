#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: check tests/scripts/fxfilter.txt in build/host/volna.wav (noise through FILTER, both channels).
The brightness: zero crossings a second (filtered noise crosses about twice its cutoff a second).
  dry noise: over 10000 a second;
  FILTER, the defaults (an LFO): in 50 ms windows the brightness swings past x 3 (the cutoff sweeps), the sides the
  same (SOURCE LFO);
  SOURCE ENV: the hard note brighter than the soft one by x 1.3 at least;
  after the last note (from 10.3 s): every sample 0.
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))
L, R = d[0::2], d[1::2]


def zcr(t0, t1, s=L):
    a, b = int(t0 * fs), int(t1 * fs)
    return sum(1 for i in range(a + 1, b) if (s[i - 1] < 0) != (s[i] < 0)) / (t1 - t0)


bad = 0


def check(what, ok, msg):
    global bad
    bad += not ok
    print(f"{what}: {msg} {'ok' if ok else 'FAIL'}")


dry = zcr(1.7, 2.4)
check("dry noise", dry > 10000, f"{dry:.0f} crossings a second")
win = [zcr(t / 20, t / 20 + 0.05) for t in range(66, 138)]
check("FILTER, an LFO", max(win) > 3 * min(win), f"the brightness from {min(win):.0f} to {max(win):.0f} a second")
same = L[int(3.3 * fs):int(6.9 * fs)] == R[int(3.3 * fs):int(6.9 * fs)]
check("FILTER, an LFO", same, "the sides the same" if same else "the sides differ")
hard, soft = zcr(7.7, 8.4), zcr(8.9, 9.6)
check("SOURCE ENV", hard > 1.3 * soft, f"a hard note {hard:.0f}, a soft one {soft:.0f} crossings a second")
tail = max(abs(x) for x in d[2 * int(10.3 * fs):])
check("after the last note", tail == 0, f"the loudest sample {tail}")
sys.exit(1 if bad else 0)
