#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: the factory sounds side by side. A host build with VOLNA_PROBE (the output a quarter, before the limiter)
plays each: A3 alone, then a C minor chord of 4 (velocity 110). The peaks, before the output's limiter:
  pads and the like (POLY): the chord 10 000 .. 19 000;
  mono, arp and short sounds (NOTE): the note 3 500 .. 12 000;
  every one: the chord under the limiter's knee (20 000): the factory never leans on it.
Exit 1 if one is off.   python3 tests/levels_check.py"""
import struct
import subprocess
import sys
import wave

SOUNDS = [(0, "INIT", "POLY"), (1, "SOFT PAD", "POLY"), (2, "SUPERSAW", "POLY"), (3, "BASS-1", "NOTE"), (4, "FILT", "POLY"), (5, "FM EP", "POLY"), (6, "SYNC LEAD", "NOTE")]   # (number, name, kind)
L, t, marks = ["900 btn 8"], 1000, []
prev = 0
for num, _, _ in SOUNDS:
    if num != prev:
        L.append(f"{t} enc 6 {num - prev}")
        t += 300
        prev = num
    marks.append(t)
    L += [f"{t} midi 0x90 57 110", f"{t + 1200} midi 0x80 57 0"]
    c = t + 1600
    L += [f"{c} midi 0x90 {k} 110" for k in (48, 51, 55, 60)] + [f"{c + 1500} midi 0x80 {k} 0" for k in (48, 51, 55, 60)]
    t = c + 2700
L.append(f"{t} end")
open("build/host/levels.txt", "w").write("\n".join(L) + "\n")
subprocess.run(["cc", "-O2", "-DDINGHY_HOST", "-DVOLNA_PROBE", '-DAPP_VER="probe"', "-Ifirmware/hal", "-Ifirmware/src",
                "-Ifirmware/gen", "-o", "build/host/probe", "firmware/src/volna.c"], check=True)
subprocess.run(["build/host/probe", "build/host/probe", "build/host/levels.txt"], check=True, stdout=subprocess.DEVNULL)
w = wave.open("build/host/probe.wav")
fs, n = w.getframerate(), w.getnframes()
raw = struct.unpack("<%dh" % (2 * n), w.readframes(n))
bad = 0
for (_, name, kind), m in zip(SOUNDS, marks):
    t0 = m / 1000.0
    pk = lambda a, b: 4 * max(abs(x) for x in raw[2 * int(a * fs):2 * int(b * fs)])
    note, chord = pk(t0, t0 + 1.5), pk(t0 + 1.6, t0 + 4.2)
    ok = chord < 20000 and (10000 <= chord <= 19000 if kind == "POLY" else 3500 <= note <= 12000)
    bad += not ok
    print(f"{name:13s} {kind}: the note {note:6d}, the chord {chord:6d} {'ok' if ok else 'OFF'}")
sys.exit(1 if bad else 0)
