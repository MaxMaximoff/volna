#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: the ANALOG table (engine/va_osc.h) heard in the firmware. OSC1 alone, no filter (TYPE OFF), A1 (55 Hz):
  1. BASIC's sawtooth (wave 63) against ANALOG's (wave 0): about as loud (within 2 dB); the 100th harmonic (5.5 kHz):
     BASIC has none of its own (63 at most; what is there, under -48 dB, is the 128-sample table's images),
     ANALOG has it as a sawtooth should (1/100: -40 dB, within 3);
  2. ANALOG's sawtooth at C8 (4186 Hz): the aliasing (the energy off the note's harmonics) under -18 dB (polyBLEP:
     a naive edge would be about -10; the table's mip-maps are cleaner up there, tests/mip_check.py);
  3. wave 32, the square: no 2nd harmonic (under -30 dB), the 3rd at 1/3 (-9.5 dB, within 1.5);
  4. wave 63, the narrow pulse: its peak at most 1.3 x the sawtooth's (va_osc.h: 1.125 and the polyBLEP's), its
     level a quarter of the sawtooth's or more.
Runs the host build of firmware/src/volna.c (tests/run_host.sh builds it first). Exit 1 if one fails.
    python3 tests/analog_check.py [build/host/volna]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

HOST = sys.argv[1] if len(sys.argv) > 1 else "build/host/volna"
OUT = "build/host/analog"
SCRIPT = [
    "850 enc 3 63",                                         # (OSC row A at the start) BASIC wave 63: its sawtooth
    "1000 btn 3", "1010 enc 3 -40", "1020 enc 4 -40",       # MIX: OSC2 OSC3 off
    "1200 btn 4", "1250 btn 4", "1300 enc 2 -10",           # FILT row B: TYPE OFF
    "3200 btn 2", "3300 enc 2 6",                          # OSC: TABLE ANALOG (KNOB1)
    "3400 enc 3 -63",                                      # WAVE 0 (KNOB2), the sawtooth
    "6200 enc 3 32",                                        # the square
    "7600 enc 3 31",                                        # the narrow pulse
    "9500 end"]
NOTES = [(2000, 33), (3600, 33), (5000, 108), (6400, 33), (7800, 33)]
SCRIPT += [f"{t} midi 0x90 {n} 100" for t, n in NOTES] + [f"{t + 1000} midi 0x80 {n} 0" for t, n in NOTES]


def amp(y, fs, f):                              # the amplitude at f (a Hann window, Goertzel)
    c, s1, s2, m = 2 * math.cos(2 * math.pi * f / fs), 0.0, 0.0, len(y)
    for i, v in enumerate(y):
        s1, s2 = v * (0.5 - 0.5 * math.cos(2 * math.pi * i / m)) + c * s1 - s2, s1
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 1e-12)) * 4 / m


def alias(y, fs, f):                            # the energy off the harmonics of f, dB (as tests/mip_check.py)
    m = int(round(int(len(y) * f / fs) * fs / f))
    y = y[:m]
    tot, har, h = sum(v * v for v in y), 0.0, 1
    while h * f < fs / 2:
        c = sum(v * math.cos(2 * math.pi * h * f * i / fs) for i, v in enumerate(y))
        s = sum(v * math.sin(2 * math.pi * h * f * i / fs) for i, v in enumerate(y))
        har += 2 * (c * c + s * s) / len(y)
        h += 1
    return 10 * math.log10(max(tot - har, 1e-9) / har)


def db(x):
    return 20 * math.log10(max(x, 1e-9))


sp = Path(OUT + ".txt")
sp.write_text("\n".join(sorted(SCRIPT, key=lambda l: int(l.split()[0]))) + "\n")
subprocess.run([HOST, OUT, str(sp)], check=True, stdout=subprocess.DEVNULL)
w = wave.open(OUT + ".wav")
fs, n = w.getframerate(), w.getnframes()
L = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
win = [L[int((t / 1000 + 0.3) * fs):int((t / 1000 + 0.3) * fs) + 16384] for t, _ in NOTES]
rms = [math.sqrt(sum(v * v for v in x) / len(x)) for x in win]
F1, F8 = 55.0, 440.0 * 2 ** ((108 - 69) / 12)
h100 = [db(amp(win[i], fs, 100 * F1) / amp(win[i], fs, F1)) for i in (0, 1)]
al = alias(win[2][:int(0.4 * fs)], fs, F8)
sq2, sq3 = (db(amp(win[3], fs, k * F1) / amp(win[3], fs, F1)) for k in (2, 3))
peak = max(abs(v) for v in win[4]) / max(abs(v) for v in win[1])
checks = [
    ("as loud as BASIC's saw", abs(db(rms[1] / rms[0])) < 2, f"rms {rms[0]:.0f} / {rms[1]:.0f}"),
    ("BASIC: no 100th harmonic", h100[0] < -48, f"{h100[0]:.1f} dB"),
    ("ANALOG: the 100th at 1/100", abs(h100[1] + 40) < 3, f"{h100[1]:.1f} dB"),
    ("ANALOG at C8: aliasing low", al < -18, f"{al:.1f} dB"),
    ("square: no 2nd harmonic", sq2 < -30, f"{sq2:.1f} dB"),
    ("square: the 3rd at 1/3", abs(sq3 + 9.54) < 1.5, f"{sq3:.1f} dB"),
    ("narrow pulse: in bounds", rms[4] > rms[1] / 4 and peak < 1.3, f"rms {rms[4]:.0f}, peak x{peak:.2f}"),
]
bad = 0
for name, ok, what in checks:
    bad += not ok
    print(f"{name:28s} {what:20s} {'ok' if ok else 'FAIL'}")
sys.exit(1 if bad else 0)
