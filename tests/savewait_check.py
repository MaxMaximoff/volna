#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: run tests/scripts/savewait.txt on the host and check that a SAVE writes the sound as it was when SAVE was
pressed, even when another patch is loaded while it waits for silence (until 2026-11-15 the loaded one was written):
  the first write comes after PRESETS is turned (3300), before SUPERSAW's note (6000); the second right after
  PRESETS loads its very number (9400), before the next note: the cases are met;
  the note from 06 is the edit (white noise: its zero crossings over half the edit's), not SUPERSAW (under a
  fifth: tones);
  07 loaded before its write, the note after it: the edit too, not 07's old sound (INIT, a tone).
Exit 1 if not.   python3 tests/savewait_check.py"""
import re
import struct
import subprocess
import sys
import wave

r = subprocess.run(["build/host/volna", "build/host/savewait", "tests/scripts/savewait.txt"], check=True,
                   capture_output=True, text=True)
saved = [int(m.group(1)) for m in re.finditer(r"(\d+) ms  patches 0 saved", r.stdout)]
w = wave.open("build/host/savewait.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
zc = lambda t0, t1: sum(1 for i in range(int(t0 * fs) + 1, int(t1 * fs)) if (d[i - 1] < 0) != (d[i] < 0))
bad = 0


def check(what, ok, msg):
    global bad
    bad += not ok
    print(f"{what}: {msg} {'ok' if ok else 'FAIL'}")


check("the writes after PRESETS", len(saved) == 2 and 3300 < saved[0] < 6000 and 9400 <= saved[1] < 9600,
      f"patches 0 (06, 07: the sector) saved at {saved} ms")
ze, zs, z6, z7 = zc(2.0, 2.6), zc(6.5, 7.1), zc(8.3, 8.9), zc(10.5, 11.1)
check("SUPERSAW is another sound", zs < ze / 5, f"zero crossings: the edit {ze}, SUPERSAW {zs}")
check("06 is the edit as saved", z6 > ze / 2, f"zero crossings: the edit {ze}, 06 {z6}")
check("07, loaded before its write, is the edit", z7 > ze / 2, f"zero crossings: the edit {ze}, 07 {z7}")
sys.exit(1 if bad else 0)
