#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: check tests/scripts/eq.txt in build/host/volna.wav (a 494 Hz sine, both channels).
  MID -12 dB at its frequency (M.FREQ moved to 494 Hz): the level x 0.251 of dry (within 15 %), both sides;
  after SAVE to 05 and PRESETS away and back: the same (M.FREQ is in the patch's tail, blob 6: lost, the bell would
  be back at 988 Hz and take ~4 dB here, not 12).
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))


def rms(t0, t1, ch):
    s = d[2 * int(t0 * fs) + ch:2 * int(t1 * fs):2]
    return math.sqrt(sum(x * x for x in s) / len(s))


bad = 0
WANT = 10 ** (-12 / 20)
for ch, side in ((0, "left"), (1, "right")):
    dry = rms(1.8, 2.2, ch)
    for what, t0 in (("MID -12 dB", 3.3), ("MID -12 dB after SAVE and PRESETS", 6.8)):
        r = rms(t0, t0 + 0.4, ch) / max(dry, 1e-9)
        ok = abs(r / WANT - 1) < 0.15 and dry > 300
        bad += not ok
        print(f"{what}, {side}: x{r:.3f} of dry (rms {dry:.0f}; want x{WANT:.3f}) {'ok' if ok else 'FAIL'}")
sys.exit(1 if bad else 0)
