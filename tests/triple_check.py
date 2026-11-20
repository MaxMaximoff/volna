#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna (2026-11-16): TRIPLE (the Blofeld's Triple FX: S&H, overdrive, a chorus), heard. OSC1 a sine (BASIC wave 0)
alone, filter 1 open; FX slot 1 TRIPLE (FX [GLO] = button 7, KNOB1 TYPE +9; row A RATE DEPTH, row B CHO MIX, S&H,
DRIVE); A3 held 1 s each:
  CLEAN   CHO MIX 0, S&H 0, DRIVE 0: the sine as it is: left and right alike, 99 % on 220 Hz;
  DRIVE   DRIVE 127: the 3rd harmonic up (over 10 % of the 1st), no 2nd (a symmetric curve);
  S&H     S&H 127 (a sample held 32): images off 220 Hz's harmonics (over 5 % of the energy off them);
  CHORUS  CHO MIX 127: the two sides apart (the LFO half a cycle apart): their difference over 10 % of either.
Runs the host build (tests/run_host.sh builds it first). Exit 1 if one is off.   python3 tests/triple_check.py"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from offharm import off_db  # noqa: E402

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "build/host/volna"
OUT = "build/host/triple"
BASE = ["700 enc 3 -64", "750 btn 3", "800 enc 3 -40", "850 enc 4 -40", "900 btn 4", "950 enc 2 60", "1000 enc 4 -24",
        "1100 btn 7", "1200 enc 2 9",                               # FX slot 1: TRIPLE (CHO 50, S&H 0, DRIVE 30)
        "1300 btn 7"]                                               # row B: CHO MIX, S&H, DRIVE


def run(extra, name):
    lines = BASE + extra + ["2000 midi 0x90 57 100", "3000 midi 0x80 57 0", "3500 end"]
    sp = Path(f"{OUT}-{name}.txt")
    sp.write_text("\n".join(sorted(lines, key=lambda l: int(l.split()[0]))) + "\n")
    subprocess.run([HOST, f"{OUT}-{name}", str(sp)], check=True, stdout=subprocess.DEVNULL)
    w = wave.open(f"{OUT}-{name}.wav")
    fs, n = w.getframerate(), w.getnframes()
    raw = struct.unpack("<%dh" % (2 * n), w.readframes(n))
    return fs, raw[0::2][int(2.3 * fs):int(2.9 * fs)], raw[1::2][int(2.3 * fs):int(2.9 * fs)]


def amp(a, fs, f):
    m = len(a)
    c = 2 * math.cos(2 * math.pi * f / fs)
    s1 = s2 = 0.0
    for i, v in enumerate(a):
        s0 = v * (0.5 - 0.5 * math.cos(2 * math.pi * i / m)) + c * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0.0)) / m * 4


rms = lambda a: math.sqrt(sum(v * v for v in a) / len(a))
bad = 0


def report(name, ok, text):
    global bad
    bad += not ok
    print(f"{name:6s}: {text} {'ok' if ok else 'FAIL'}")


ZERO = ["1400 enc 2 -40", "1500 enc 4 -40"]                         # CHO MIX 0, DRIVE 0
fs, L, R = run(ZERO, "clean")
d = rms([a - b for a, b in zip(L, R)]) / max(rms(L), 1)
od = off_db(list(L), fs, 220.0)
report("CLEAN", d < 0.01 and od < -40, f"left and right {100 * d:.1f} % apart, {od:.0f} dB off 220 Hz's harmonics")
fs, L, R = run(ZERO + ["1600 enc 4 64"], "drive")
h1, h2, h3 = amp(L, fs, 220), amp(L, fs, 440), amp(L, fs, 660)
report("DRIVE", h3 > 0.1 * h1 and h2 < 0.01 * h1, f"DRIVE 127: 3rd {h3 / h1:.2f} of the 1st, 2nd {h2 / h1:.3f}")
fs, L, R = run(ZERO + ["1600 enc 3 64"], "sh")
od = off_db(list(L), fs, 220.0)
report("S&H", od > -13, f"S&H 127: {od:.1f} dB off 220 Hz's harmonics (want over -13: images)")
fs, L, R = run(["1400 enc 2 40", "1500 enc 4 -40"], "chorus")
d = rms([a - b for a, b in zip(L, R)]) / max(rms(L), 1)
report("CHORUS", d > 0.1, f"CHO MIX 127: left and right {100 * d:.0f} % apart")
sys.exit(1 if bad else 0)
