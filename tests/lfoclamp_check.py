#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: check tests/scripts/lfoclamp.txt's screens. LFO 1 (a sine, ~10 s a cycle) on WAV1 +33 (+-16.8 waves) at
wave 9 swings past the table's start: the position stops at wave 0 for ~3 s a cycle (by design: the matrix's sum is
kept inside the table). The "LFO stuck at 0" report (2026-11-14) was this stop unexplained, so the screen says it:
  - while the picture stands still (shots 500 ms apart alike), "W9 > [0 LFO1" and a wall at the ruler's start are in
    C_WARN; while it moves (away from the end), no C_WARN;
  - MATRIX: slot 1's reach "-8..26" in C_WARN.
Exit 1 if not."""
import sys
from PIL import Image, ImageChops

TS = list(range(2500, 26001, 500))


def img(t):
    return Image.open(f"build/host/volna-{t}.png").convert("RGB")


def warn(im, box):                              # C_WARN (255, 90, 70), alpha-blended onto black: red, little else
    d = im.crop(box).tobytes()
    return sum(1 for i in range(0, len(d), 3) if d[i] > 120 and d[i + 1] < d[i] * 0.45 and d[i + 2] < d[i] * 0.4)


def moved(a, b):                                # the picture's pixels that differ (left of the ruler's wall)
    box = (60, 120, 480, 366)
    return sum(1 for v in ImageChops.difference(a.crop(box), b.crop(box)).convert("L").tobytes() if v)


TEXT, WALL = (240, 150, 480, 200), (14, 320, 30, 372)   # the VOICE line (y 80..100), the ruler's start (2x PNGs)
bad, still_n = 0, 0
shots = [img(t) for t in TS]
for i in range(1, len(TS) - 1):
    t, cur = TS[i], shots[i]
    m0, m1 = moved(shots[i - 1], cur), moved(cur, shots[i + 1])
    wt, ww = warn(cur, TEXT), warn(cur, WALL)
    if m0 == 0:                                 # standing (at the end)
        still_n += 1
        if wt < 40 or ww < 20:
            bad += 1
            print(f"{t} ms: the wave stands at the end, the screen does not say so (text {wt}, wall {ww}) FAIL")
    elif m0 > 1500 and m1 > 1500 and (wt or ww):   # moving on both sides: away from the end
        bad += 1
        print(f"{t} ms: the wave moves ({m0}, {m1}), yet C_WARN shows (text {wt}, wall {ww}) FAIL")
print(f"the wave stood at the table's start in {still_n} shots of {len(TS) - 2}, each marked"
      f" {'ok' if still_n >= 6 and not bad else 'FAIL'}")
bad += still_n < 6
mx = warn(img(26400), (0, 120, 480, 160))       # MATRIX: slot 1's line (canvas y 0..15 at y 60)
ok = mx > 40
bad += not ok
print(f"MATRIX: slot 1's reach past the table's start in C_WARN ({mx} px) {'ok' if ok else 'FAIL'}")
sys.exit(1 if bad else 0)
