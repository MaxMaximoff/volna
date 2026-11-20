#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna (2026-11-15): the matrix's newer sources and destinations and the modifiers, heard.

Each case: OSC2 and OSC3 off; slot 1 (and slot 2) set on MATRIX (MOD's row C: KNOB1 the source, KNOB2 the group,
KNOB3 the destination in it, KNOB4 the amount; SELECT the slot), modifier 1 on MODIF (row D); A3 held 1 s with the
controller at its rest, then again with it moved:
  PRESS   aftertouch -> CUTOFF +63 (a brighter wave, the filter as it is): brighter at 127;
  PPRESS  poly pressure on A3 -> PAN +63: to the right at 127;
  BEND    the pitch bend -> PAN +63: to the right at the top;
  CTLW    CC 16 (SETUP's CTL W) -> PAN +63: to the right at 127;
  MOD1    modifier 1 = WHEEL * CONST 63 -> PAN +63: to the right with the wheel up;
  MODSAVE as MOD1 with modifier 4, then SAVE to 05, PRESETS away and back (modifier 4 lies in the patch's second
          tail, blob 7): still heard;
  MOD1MIN modifier 1 = WHEEL MIN CTL X (CC 17 at 0): the wheel up changes nothing (the smaller one, 0);
  L1SPD   LFO 1 a square on PAN +63, the wheel on L1.SPD +63: the left and right swap far more often;
  AMT1    slot 1 MAX -> PAN at amount 0, slot 2 WHEEL -> AMT1 +63: the wheel makes slot 1 work, to the right.
Runs the host build of firmware/src/volna.c (tests/run_host.sh builds it first). Exit 1 if one is not heard.
    python3 tests/modmatrix_check.py [build/host/volna] [--only NAME]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "build/host/volna"
OUT = "build/host/modmatrix"
S = {"WHEEL": 9, "PRESS": 13, "PPRESS": 14, "BEND": 15, "CTLW": 19, "CTLX": 20, "MAX": 26, "MOD1": 27, "LFO1": 1}
G = {"FILT": 3, "AMP": 4, "MOD": 6}            # MATRIX's groups; in them: CUT 0 (FILT), PAN 1 (AMP), L1.SPD 0, AMT1 3
OFF23 = ["1000 btn 3", "1010 enc 3 -40", "1020 enc 4 -40"]   # MIX: OSC2 OSC3 off
MATRIX = ["2000 btn 6", "2050 btn 6", "2100 btn 6"]          # MOD, its row C


def slot(t, src, grp, dst, amt):               # the slot SELECT picks: source, group, destination, amount
    return [f"{t} enc 2 {src}", f"{t + 300} enc 3 {grp}", f"{t + 600} enc 4 {dst}", f"{t + 900} enc 5 {amt}"]


def modf(t, a, op, b, c):                       # row D (one more MOD press), modifier 1: A, OP, B, CONST (2 a detent)
    return [f"{t} btn 6", f"{t + 100} enc 2 {a}", f"{t + 400} enc 3 {op}", f"{t + 700} enc 4 {b}",
            f"{t + 1000} enc 5 {c}"]


def note(t, rest, moved):                       # A3 1 s: the rest's messages before it, the moved ones after on
    return ([f"{t - 100} {m}" for m in rest] + [f"{t} midi 0x90 57 100", f"{t + 1000} midi 0x80 57 0"],
            [f"{t - 100 + 50 * i} {m}" for i, m in enumerate(moved["before"])] + [f"{t} midi 0x90 57 100"]
            + [f"{t + 30} {m}" for m in moved["after"]] + [f"{t + 1000} midi 0x80 57 0"])


PAN = (G["AMP"], 1, 63)
CASES = {
    "PRESS": (["800 enc 3 40"] + OFF23 + MATRIX + slot(2200, S["PRESS"], G["FILT"], 0, 63),   # (a brighter wave)
              ["midi 0xD0 0 0"], {"before": ["midi 0xD0 127 0"], "after": []}, "bright", "up"),
    "PPRESS": (OFF23 + MATRIX + slot(2200, S["PPRESS"], *PAN), [], {"before": [], "after": ["midi 0xA0 57 127"]},
               "pan", "up"),
    "BEND": (OFF23 + MATRIX + slot(2200, S["BEND"], *PAN), ["midi 0xE0 0 64"], {"before": ["midi 0xE0 127 127"],
             "after": []}, "pan", "up"),
    "CTLW": (OFF23 + MATRIX + slot(2200, S["CTLW"], *PAN), ["midi 0xB0 16 0"], {"before": ["midi 0xB0 16 127"],
             "after": []}, "pan", "up"),
    "MOD1": (OFF23 + MATRIX + slot(2200, S["MOD1"], *PAN) + modf(3400, S["WHEEL"], 2, 0, 40),
             ["midi 0xB0 1 0"], {"before": ["midi 0xB0 1 127"], "after": []}, "pan", "up"),
    "MODSAVE": (OFF23 + MATRIX + slot(2200, S["MOD1"] + 3, *PAN) + ["3350 btn 6", "3380 enc 0 3"]
                + modf(3400, S["WHEEL"], 2, 0, 40)[1:]
                + ["4600 btn 9", "4700 enc 2 5", "4900 btn 9", "5300 enc 6 -1", "5600 enc 6 1"],   # SAVE to 05, away, back
                ["midi 0xB0 1 0"], {"before": ["midi 0xB0 1 127"], "after": []}, "pan", "up"),
    "MOD1MIN": (OFF23 + MATRIX + slot(2200, S["MOD1"], *PAN) + modf(3400, S["WHEEL"], 7, S["CTLX"], 0),
                ["midi 0xB0 1 0", "midi 0xB0 17 0"], {"before": ["midi 0xB0 1 127"], "after": []}, "pan", "same"),
    "L1SPD": (OFF23 + ["1500 btn 6", "1600 enc 2 2"] + ["2050 btn 6", "2100 btn 6"]   # LFO 1 a square; row C
              + slot(2200, S["WHEEL"], G["MOD"], 0, 63) + ["3400 enc 0 1"] + slot(3600, S["LFO1"], *PAN),
              ["midi 0xB0 1 0"], {"before": ["midi 0xB0 1 127"], "after": []}, "flips", "up"),
    "AMT1": (OFF23 + MATRIX + slot(2200, S["MAX"], G["AMP"], 1, 0) + ["3400 enc 0 1"]
             + slot(3600, S["WHEEL"], G["MOD"], 3, 63),
             ["midi 0xB0 1 0"], {"before": ["midi 0xB0 1 127"], "after": []}, "pan", "up"),
}


def measure(path, t0, t1, what):
    w = wave.open(path)
    fs, n = w.getframerate(), w.getnframes()
    raw = struct.unpack("<%dh" % (2 * n), w.readframes(n))
    L, R = raw[0::2][int(t0 * fs):int(t1 * fs)], raw[1::2][int(t0 * fs):int(t1 * fs)]
    rms = lambda x: math.sqrt(sum(v * v for v in x) / max(1, len(x)))
    if what == "pan":
        return rms(R) / max(rms(L), 1.0)
    if what == "flips":                         # how often the louder side changes, 2 ms windows
        win = int(fs * 0.002)
        side = [rms(R[i:i + win]) > rms(L[i:i + win]) for i in range(0, len(L) - win, win)]
        return 1 + sum(1 for i in range(1, len(side)) if side[i] != side[i - 1])
    e = sum(x * x for x in L) / len(L)          # brightness: the difference's rms over the rms
    dd = sum((L[i] - L[i - 1]) ** 2 for i in range(1, len(L))) / len(L)
    return math.sqrt(dd / max(e, 1.0))


bad = 0
for name, (setup, rest, moved, what, way) in CASES.items():
    if "--only" in sys.argv and name != sys.argv[-1]:
        continue
    a, b = 6000, 8600
    n0, n1 = note(a, rest, moved)[0], note(b, rest, moved)[1]
    lines = setup + n0 + n1 + [f"{b + 2200} end"]
    sp = Path(OUT + ".txt")
    sp.write_text("\n".join(sorted(lines, key=lambda l: int(l.split()[0]))) + "\n")
    subprocess.run([HOST, OUT, str(sp)], check=True, stdout=subprocess.DEVNULL)
    x0 = measure(OUT + ".wav", a / 1000 + 0.15, a / 1000 + 0.95, what)
    x1 = measure(OUT + ".wav", b / 1000 + 0.15, b / 1000 + 0.95, what)
    r = x1 / max(x0, 1e-9)
    ok = r > 1.25 if way == "up" else abs(r - 1) < 0.05
    bad += not ok
    print(f"{name:7s}: {what:6s} {x0:8.3f} -> {x1:8.3f} (x{r:5.2f}, want {way}) {'ok' if ok else 'NOT HEARD'}")
sys.exit(1 if bad else 0)
