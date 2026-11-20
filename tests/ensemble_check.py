#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: check tests/scripts/ensemble.txt in build/host/volna.wav (sines, both channels).
  dry C6: steady (20 ms windows within 1.5 dB);
  ENSEMBLE on C6: the level swings past 6 dB (the taps beat against the dry), the sides more than 3 dB apart
  somewhere, no step past x 1.3 of the sine's own (no clicks);
  ENSEMBLE on C2 (65 Hz): within -1.5 .. +2 dB of dry C2 (the bass not hollowed).
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))
WIN = int(0.02 * fs)


def rms(t0, t1, ch=0):
    s = d[2 * int(t0 * fs) + ch:2 * int(t1 * fs):2]
    return math.sqrt(sum(x * x for x in s) / len(s)) + 1e-9


def levels(t0, t1, ch):
    return [rms(a / fs, (a + WIN) / fs, ch) for a in range(int(t0 * fs), int(t1 * fs) - WIN, WIN)]


def steps(t0, t1, ch, f):
    s = d[2 * int(t0 * fs) + ch:2 * int(t1 * fs):2]
    worst = 0
    for a in range(0, len(s) - WIN, WIN):
        seg = s[a:a + WIN + 1]
        pk = max(abs(x) for x in seg)
        st = max(abs(seg[i] - seg[i - 1]) for i in range(1, len(seg)))
        worst = max(worst, st / (2 * math.pi * f / fs * pk + 1e-9))
    return worst


bad = 0


def check(what, ok, msg):
    global bad
    bad += not ok
    print(f"{what}: {msg} {'ok' if ok else 'FAIL'}")


dry = levels(1.8, 2.4, 0)
check("dry C6", 20 * math.log10(max(dry) / min(dry)) < 1.5, f"within {20 * math.log10(max(dry) / min(dry)):.2f} dB")
L, R = levels(4.5, 6.9, 0), levels(4.5, 6.9, 1)
sw = 20 * math.log10(max(L) / min(L))
apart = max(abs(20 * math.log10(a / b)) for a, b in zip(L, R))
st = max(steps(4.5, 6.9, 0, 1046.5), steps(4.5, 6.9, 1, 1046.5))
check("ENSEMBLE on C6", sw > 6, f"the level swings {sw:.1f} dB")
check("ENSEMBLE on C6", apart > 3, f"the sides up to {apart:.1f} dB apart")
check("ENSEMBLE on C6", st < 1.3, f"the largest step x{st:.2f} of the sine's own")
b = 20 * math.log10(rms(7.4, 8.0) / rms(2.9, 3.5))
check("ENSEMBLE on C2", -1.5 < b < 2, f"{b:+.2f} dB against dry")
sys.exit(1 if bad else 0)
