#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna (2026-11-16): the envelopes' SINGLE trigger (ENV row C, TRIGGER), heard. MONO; AMP: DECAY 110 (~3 s), SUST 0
(a long decay to nothing); A3 held, A4 on it 0.6 s later: NORMAL starts AMP's attack again (A4 as loud as A3 was at first), SINGLE lets
it go on decaying (A4 no louder than A3 just before it). Runs the host build (tests/run_host.sh builds it first).
    python3 tests/trig_check.py [build/host/volna]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "build/host/volna"
OUT = "build/host/trig"
BASE = ["900 btn 8", "1000 enc 4 1",                                # PATCH: MODE MONO
        "1100 btn 5", "1150 enc 3 15", "1200 enc 4 -60"]            # ENV (AMP) row A: DECAY 110 (~3 s), SUST 0


def run(extra, name):
    lines = BASE + extra + ["2000 midi 0x90 57 100", "2600 midi 0x90 69 100", "3400 midi 0x80 69 0",
                            "3450 midi 0x80 57 0", "3900 end"]
    sp = Path(f"{OUT}-{name}.txt")
    sp.write_text("\n".join(sorted(lines, key=lambda l: int(l.split()[0]))) + "\n")
    subprocess.run([HOST, f"{OUT}-{name}", str(sp)], check=True, stdout=subprocess.DEVNULL)
    w = wave.open(f"{OUT}-{name}.wav")
    fs, n = w.getframerate(), w.getnframes()
    x = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
    r = lambda t0, t1: math.sqrt(sum(v * v for v in x[int(t0 * fs):int(t1 * fs)]) / (int(t1 * fs) - int(t0 * fs)))
    return r(2.5, 2.6), r(2.65, 2.75)            # A3 just before A4, A4 just after


b0, a0 = run([], "normal")
b1, a1 = run(["1300 btn 5", "1350 btn 5", "1400 enc 3 1"], "single")   # row C: TRIGGER SINGLE
bad = 0
ok = a0 > 1.5 * b0
bad += not ok
print(f"NORMAL: A3 before {b0:.0f}, A4 after {a0:.0f} (the attack again: x{a0 / max(b0, 1):.2f}, want > 1.5) {'ok' if ok else 'FAIL'}")
ok = a1 < 1.1 * b1
bad += not ok
print(f"SINGLE: A3 before {b1:.0f}, A4 after {a1:.0f} (goes on: x{a1 / max(b1, 1):.2f}, want < 1.1) {'ok' if ok else 'FAIL'}")
sys.exit(1 if bad else 0)
