#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: the 24 dB filter types, heard in the firmware. A sawtooth (OSC1 WAVE 63, OSC2 and OSC3 off) at A2 through
filter 1 at its 830 Hz, its envelope off: once as LP12, then (row B, TYPE one step back) as LP24; then likewise HP12
and HP24 at a high cutoff. The 24 dB type takes away more of what is past its cutoff: LP24 darker than LP12 (the 16th
harmonic, 1760 Hz, an octave past the cutoff, against the fundamental: 8 dB down or more), HP24 thinner than HP12 (the
level, down); what passes stays (the LP24's level within 6 dB of the LP12's: its knee is softer, Q 0.54 first).
Runs the host build of firmware/src/volna.c (tests/run_host.sh builds it first). Exit 1 if one is not heard.
    python3 tests/filter24_check.py [build/host/volna]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

HOST = sys.argv[1] if len(sys.argv) > 1 else "build/host/volna"
OUT = "build/host/filter24"
SAW = ["850 enc 3 63", "1000 btn 3", "1010 enc 3 -40", "1020 enc 4 -40",   # (OSC row A at the start) OSC1 a saw
       "1200 btn 4", "1210 enc 4 -24"]                                                  # FILT: its envelope off
NOTES = [2000, 3600, 5200, 6800]                # A2 for a second each: LP12, LP24, HP12, HP24
SCRIPT = SAW + [
    "3300 btn 4", "3400 enc 2 -1",              # row B: TYPE LP12 -> LP24
    "4900 enc 2 5", "4950 btn 4", "4975 btn 4", "5000 enc 2 30",   # HP12; row A (B, C, A since row C came,
                                                # 2026-11-16): the cutoff up (to ~4.7 kHz)
    "6500 btn 4", "6600 enc 2 -1",              # row B: HP12 -> HP24
] + [f"{t} midi 0x90 45 100" for t in NOTES] + [f"{t + 1000} midi 0x80 45 0" for t in NOTES] + ["8800 end"]


def frames(path):
    w = wave.open(path)
    n = w.getnframes()
    return w.getframerate(), struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]


def level(L):
    return math.sqrt(sum(v * v for v in L) / max(1, len(L)))


def harm(L, fs, f):                             # the amplitude at the frequency f (Goertzel)
    w = 2 * math.pi * f / fs
    c, s1, s2 = 2 * math.cos(w), 0.0, 0.0
    for x in L:
        s1, s2 = x + c * s1 - s2, s1
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0.0)) * 2 / len(L)


sp = Path(OUT + ".txt")
sp.write_text("\n".join(sorted(SCRIPT, key=lambda l: int(l.split()[0]))) + "\n")
subprocess.run([HOST, OUT, str(sp)], check=True, stdout=subprocess.DEVNULL)
fs, L = frames(OUT + ".wav")
win = [L[int((t / 1000 + 0.3) * fs):int((t / 1000 + 0.95) * fs)] for t in NOTES]
o12, o24 = (harm(w, fs, 110.0 * 16) / max(harm(w, fs, 110.0), 1e-9) for w in win[:2])
l12, l24, h12, h24 = level(win[0]), level(win[1]), level(win[2]), level(win[3])
checks = [
    ("LP24 darker than LP12", o24 < 0.4 * o12, f"16th harmonic {20 * math.log10(o12):.1f} -> {20 * math.log10(o24):.1f} dB"),
    ("LP24 keeps what passes", abs(20 * math.log10(l24 / max(l12, 1))) < 6, f"level {l12:.0f} -> {l24:.0f}"),
    ("HP24 thinner than HP12", h24 < 0.8 * h12, f"level {h12:.0f} -> {h24:.0f}"),
]
bad = 0
for name, ok, what in checks:
    bad += not ok
    print(f"{name:24s} {what:32s} {'ok' if ok else 'NOT HEARD'}")
sys.exit(1 if bad else 0)
