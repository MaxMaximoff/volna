#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: check tests/scripts/phaser.txt in build/host/volna.wav (a 523 Hz sine, both channels).
  no effect: the level steady (its 20 ms windows within 1.5 dB);
  PHASER (the defaults): the level swings past 15 dB (the notches go by), the sides apart (their levels more than
  6 dB apart somewhere), no step between samples past what the sine itself makes (x 1.3: no clicks, no zipper);
  FEEDBK 0.95, 8 stages: the same, and nothing at full scale.
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))
F = 523.25
WIN = int(0.02 * fs)


def levels(t0, t1, ch):
    out = []
    for a in range(int(t0 * fs), int(t1 * fs) - WIN, WIN):
        s = d[2 * a + ch:2 * (a + WIN):2]
        out.append(math.sqrt(sum(x * x for x in s) / len(s)) + 1e-9)
    return out


def steps(t0, t1, ch):                          # the largest step against what the sine's own peak allows
    s = d[2 * int(t0 * fs) + ch:2 * int(t1 * fs):2]
    worst = 0
    for a in range(0, len(s) - WIN, WIN):
        seg = s[a:a + WIN + 1]
        pk = max(abs(x) for x in seg)
        st = max(abs(seg[i] - seg[i - 1]) for i in range(1, len(seg)))
        worst = max(worst, st / (2 * math.pi * F / fs * pk + 1e-9))
    return worst


bad = 0


def check(what, ok, msg):
    global bad
    bad += not ok
    print(f"{what}: {msg} {'ok' if ok else 'FAIL'}")


dry = levels(2.0, 2.9, 0)
check("no effect", 20 * math.log10(max(dry) / min(dry)) < 1.5, f"the level within {20 * math.log10(max(dry) / min(dry)):.2f} dB")
for what, t0, t1 in (("PHASER", 4.0, 7.4), ("FEEDBK 0.95, 8 stages", 8.5, 10.9)):
    L, R = levels(t0, t1, 0), levels(t0, t1, 1)
    sw = 20 * math.log10(max(L) / min(L))
    apart = max(abs(20 * math.log10(a / b)) for a, b in zip(L, R))
    st = max(steps(t0, t1, 0), steps(t0, t1, 1))
    pk = max(abs(x) for x in d[2 * int(t0 * fs):2 * int(t1 * fs)])
    check(what, sw > 15, f"the level swings {sw:.1f} dB")
    check(what, apart > 6, f"the sides up to {apart:.1f} dB apart")
    check(what, st < 1.3, f"the largest step x{st:.2f} of the sine's own")
    check(what, pk < 32000, f"peak {pk}")
sys.exit(1 if bad else 0)
