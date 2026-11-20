#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: check tests/scripts/slot16.txt: the matrix's 16th slot (LFO 1 -> PITCH +63) swings the pitch by about two
octaves (zero crossings in 50 ms windows, the busiest fifth against the quietest: ~x4), before the patch is saved and
after it is loaded back. Exit 1 if not."""
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
win = int(fs * 0.05)


def swing(t0, t1):
    zc = []
    for a in range(int(t0 * fs), int(t1 * fs) - win, win):
        s = d[a:a + win]
        zc.append(sum(1 for i in range(1, win) if (s[i - 1] < 0) != (s[i] < 0)))
    zc.sort()
    k = max(1, len(zc) // 5)
    return (sum(zc[-k:]) / k) / max(sum(zc[:k]) / k, 1e-9)


bad = 0
for what, t0 in (("slot 16", 2.1), ("slot 16 after SAVE and PRESETS", 6.6)):
    r = swing(t0, t0 + 1.8)
    ok = 3.0 < r < 5.5
    bad += not ok
    print(f"{what}: the pitch swings x{r:.2f} (want ~4: two octaves) {'ok' if ok else 'FAIL'}")
sys.exit(1 if bad else 0)
