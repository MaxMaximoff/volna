#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: the matrix's newer destinations, heard: one of each kind (DET1, COLOR, BAL1, F1.DRV, F1.ENV, PAN).

For each: slot 1 WHEEL (MIDI CC 1) -> the destination (MATRIX: KNOB2 the group, KNOB3 the destination in it,
KNOB4 the amount); a sound set up so that the destination shows; A3 held 1 s with CC 1 at 0, then at 127:
  DET1   -63, UNISON 3 (SPREAD 0): at 127 the copies' detune x 0: the level stops beating (its swing down);
  COLOR  -63, noise alone, the filter open: at 127 the colour 0: darker;
  BAL1   +63, filter 1 shut, filter 2 off (SER): at 127 OSC1 goes to filter 2, past filter 1: louder;
  F1.DRV +63, a sine, the filter open: at 127 the drive up: brighter;
  F1.ENV +63, a saw: at 127 the filter envelope's reach up: brighter at the start;
  PAN    +63: at 127 the voice to the right: the right far louder than the left.
Runs the host build of firmware/src/volna.c (tests/run_host.sh builds it first). Exit 1 if one is not heard.
    python3 tests/newdst_check.py [build/host/volna] [--only NAME]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "build/host/volna"
OUT = "build/host/newdst"
MS_WHEEL = 9
OFF23 = ["1000 btn 3", "1010 enc 3 -40", "1020 enc 4 -40"]            # MIX: OSC2 OSC3 off
OPEN = ["1200 btn 4", "1210 enc 2 60"]                               # FILT: filter 1 open
# name: (the setup, GROUP, DEST in it, AMOUNT, what to measure, the window into the note, which way at 127)
CASES = {
    "DET1": (["800 btn 2", "850 enc 2 2", "900 enc 4 -16"] + OFF23, 1, 8, -63, "swing", (0.3, 0.95), "down"),
    "COLOR": (["1000 btn 3", "1010 enc 2 -40", "1020 enc 3 -40", "1030 enc 4 -40", "1040 enc 5 40"] + OPEN,
              2, 5, -63, "bright", (0.3, 0.95), "down"),
    "BAL1": (OFF23 + ["1200 btn 4", "1210 enc 2 -40"], 2, 6, 63, "level", (0.3, 0.95), "up"),
    "F1.DRV": (["800 enc 3 -30"] + OFF23 + OPEN, 3, 5, 63, "bright", (0.3, 0.95), "up"),
    "F1.ENV": (["800 enc 3 40"] + OFF23, 3, 7, 63, "bright", (0.0, 0.15), "up"),
    "PAN": (OFF23, 4, 1, 63, "pan", (0.3, 0.95), "up"),
}


def script(setup, grp, dst, amt):
    s = setup + ["2000 btn 6", "2050 btn 6", "2100 btn 6",              # MOD, row C: MATRIX, slot 1
                 f"2200 enc 2 {MS_WHEEL}", f"2400 enc 3 {grp}", f"2500 enc 4 {dst}", f"2700 enc 5 {amt}"]
    a, b = 3600, 6200
    s += [f"{a - 100} midi 0xB0 1 0", f"{a} midi 0x90 57 100", f"{a + 1000} midi 0x80 57 0",
          f"{b - 100} midi 0xB0 1 127", f"{b} midi 0x90 57 100", f"{b + 1000} midi 0x80 57 0", f"{b + 2200} end"]
    return "\n".join(s) + "\n", a / 1000.0, b / 1000.0


def measure(path, t0, t1, what):
    w = wave.open(path)
    fs, n = w.getframerate(), w.getnframes()
    raw = struct.unpack("<%dh" % (2 * n), w.readframes(n))
    L, R = raw[0::2][int(t0 * fs):int(t1 * fs)], raw[1::2][int(t0 * fs):int(t1 * fs)]
    rms = lambda x: math.sqrt(sum(v * v for v in x) / max(1, len(x)))
    if what == "level":
        return rms(L)
    if what == "pan":
        return rms(R) / max(rms(L), 1.0)
    if what == "swing":                         # the level's swing over 20 ms windows: max / min
        win = int(fs * 0.02)
        v = [rms(L[i:i + win]) for i in range(0, len(L) - win, win)]
        return max(v) / max(min(v), 1.0)
    e = sum(x * x for x in L) / len(L)          # brightness: the difference's rms over the rms
    dd = sum((L[i] - L[i - 1]) ** 2 for i in range(1, len(L))) / len(L)
    return math.sqrt(dd / max(e, 1.0))


bad = 0
for name, (setup, grp, dst, amt, what, win, way) in CASES.items():
    if "--only" in sys.argv and name != sys.argv[-1]:
        continue
    text, a, b = script(setup, grp, dst, amt)
    sp = Path(OUT + ".txt")
    sp.write_text("\n".join(sorted(text.strip().split("\n"), key=lambda l: int(l.split()[0]))) + "\n")
    subprocess.run([HOST, OUT, str(sp)], check=True, stdout=subprocess.DEVNULL)
    x0 = measure(OUT + ".wav", a + win[0], a + win[1], what)
    x1 = measure(OUT + ".wav", b + win[0], b + win[1], what)
    r = x1 / max(x0, 1e-9)
    ok = r > 1.25 if way == "up" else r < 0.8
    bad += not ok
    print(f"{name:6s} {amt:+3d}: {what:6s} {x0:8.3f} -> {x1:8.3f} (x{r:5.2f}, want {way}) {'ok' if ok else 'NOT HEARD'}")
sys.exit(1 if bad else 0)
