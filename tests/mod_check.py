#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna stage 4a: check the modulation in build/host/volna.wav after tests/scripts/mod.txt.

LFO 1 is a ~1 Hz square into one slot, its destination changed along the script:
  PITCH +63 (3.4 .. 5.4 s): zero crossings in 50 ms windows swing by about x4 (+-12 semitones = two octaves);
  CUTOFF (6.0 .. 8.0 s): the brightness (rms of the difference over rms) swings, the pitch (crossings) does not;
  WAVE (8.6 .. 10.6 s): the brightness swings, and no sample jumps further than the loudest step of the plain
  sound (the position stays inside the table: no garbage).
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
win = int(fs * 0.05)


def windows(t0, t1):
    out = []
    for a in range(int(t0 * fs), int(t1 * fs) - win, win):
        seg = d[a:a + win]
        zc = sum(1 for i in range(1, win) if (seg[i - 1] < 0) != (seg[i] < 0))
        e = sum(x * x for x in seg) / win
        dd = sum((seg[i] - seg[i - 1]) ** 2 for i in range(1, win)) / win
        out.append((zc, math.sqrt(dd / max(e, 1.0)), max(abs(seg[i] - seg[i - 1]) for i in range(1, win))))
    return out


def spread(vals):
    vals = sorted(vals)
    k = max(1, len(vals) // 5)
    lo, hi = sum(vals[:k]) / k, sum(vals[-k:]) / k     # the quietest and the busiest fifth
    return hi / max(lo, 1e-9)


bad = 0
p = windows(3.4, 5.4)
zr = spread([x[0] for x in p])
ok = 3.0 < zr < 5.5
bad += not ok
print(f"PITCH: zero crossings swing x{zr:.2f} (want ~4: two octaves) {'ok' if ok else 'FAIL'}")
c = windows(6.0, 8.0)
br, zc = spread([x[1] for x in c]), spread([x[0] for x in c])
ok = br > 1.3 and zc < 2.0
bad += not ok
print(f"CUTOFF: brightness swings x{br:.2f}, crossings x{zc:.2f} {'ok' if ok else 'FAIL'}")
wv = windows(8.6, 10.6)
br = spread([x[1] for x in wv])
step = max(x[2] for x in wv)
ok = br > 1.2 and step < 20000
bad += not ok
print(f"WAVE: brightness swings x{br:.2f}, the biggest sample step {step} {'ok' if ok else 'FAIL'}")
sys.exit(1 if bad else 0)
