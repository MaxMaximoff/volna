#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: the voices' limit (engine/poly.h) heard in tests/scripts/poly.txt (build/host/volna.wav, today's INIT:
a sine; the limit drops to 3 at 2300 ms, VOLNA_POLY_DROP_MS).

Each note's level in a window: a Goertzel at its pitch over the left channel, against B4 held throughout.
  2.00-2.25 s: all four sound (C4 too);
  2.32-2.48 s: C4 gone (taken, out in ~6 ms), E4 G4 B4 still on;
  2.80-3.20 s: D5 sounds, E4 gone (D5 stole the oldest held: C4's voice is free, but 3 sound already), C4 gone.
Exit 1 if one is off."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
LAG = 0.0   # (the WAV starts with the script's time)
NOTE = {"C4": 60, "E4": 64, "G4": 67, "B4": 71, "D5": 74}


def level(name, t0, t1):
    f = 440.0 * 2 ** ((NOTE[name] - 69) / 12)
    a, b = int((t0 + LAG) * fs), int((t1 + LAG) * fs)
    c = 2 * math.cos(2 * math.pi * f / fs)
    s1 = s2 = 0.0
    for i in range(a, b):
        s1, s2 = d[i] + c * s1 - s2, s1
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0.0)) / (b - a)


bad = 0


def check(what, t0, t1, on, off):
    global bad
    ref = level("B4", t0, t1)
    for k in on:
        r = level(k, t0, t1) / ref if ref else 0
        ok = r > 0.3
        print("%-44s %s %-3s %.2f of B4  %s" % (what, "on ", k, r, "ok" if ok else "BAD"))
        bad |= not ok
    for k in off:
        r = level(k, t0, t1) / ref if ref else 1
        ok = r < 0.05
        print("%-44s %s %-3s %.3f of B4  %s" % (what, "off", k, r, "ok" if ok else "BAD"))
        bad |= not ok


check("4 voices: all four", 2.00, 2.25, ["C4", "E4", "G4"], [])
check("limit 3: C4 taken at once", 2.32, 2.48, ["E4", "G4"], ["C4"])
check("limit 3: D5 steals E4, no free voice", 2.80, 3.20, ["G4", "D5"], ["C4", "E4"])
sys.exit(1 if bad else 0)
