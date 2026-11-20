#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: check tests/scripts/wavemod.txt's screens: with LFO 1 on WAV1 and a note held, the OSC picture (the screen's
middle zone) changes between two shots a quarter of the LFO apart; with nothing sounding it stands still.
Exit 1 if not."""
import sys
from PIL import Image, ImageChops


def diff(a, b):
    ia, ib = Image.open(f"build/host/volna-{a}.png").convert("RGB"), Image.open(f"build/host/volna-{b}.png").convert("RGB")
    box = (0, 120, 480, 366)                    # the picture: y 60 .. 183 of 240, the PNGs at 2x
    d = ImageChops.difference(ia.crop(box), ib.crop(box)).convert("L").tobytes()
    return sum(1 for v in d if v)


held, still = diff(2400, 2650), diff(3600, 3900)
ok = held > 500 and still == 0
print(f"the picture: {held} pixels changed while the LFO moved the wave, {still} with nothing sounding "
      f"{'ok' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
