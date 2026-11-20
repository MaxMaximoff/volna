#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna (2026-11-15): FM, heard. OSC1 a sine (BASIC wave 0) modulated (FM SRC OSC2) by OSC2, a sine 19 semitones up
(silent in the mix), OSC3 off, the filter open; A3 (220 Hz) held 1 s each, the lines measured (Goertzel):
  SMALL   FM AMT 30 (I ~0.45): the carrier (220 Hz) well over the first sideband (220 + 659 = 879 Hz): J0 >> J1;
  NULL    FM AMT 70 (I ~2.4, J0's first zero): the carrier nearly gone against the sideband;
  LINES   at AMT 70, the energy on the lines 220 +- n x 659 (n <= 3, folded at 0): the pitch stays put (PM);
  EP      factory 05 FM EP: brighter at the strike (40..120 ms) than later (500..700 ms): ENV 3 -> FM1;
  ALIAS   OSC1 a sawtooth (wave 63) at A6 (1760 Hz), FM SRC OSC2 at the same pitch, AMT 127: the energy on the
          harmonics of 1760 Hz (what folds back from past 22 kHz lands between them): printed, at least 80 %.
Runs the host build of firmware/src/volna.c (tests/run_host.sh builds it first). Exit 1 if one is off.
    python3 tests/fm_check.py [build/host/volna]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "build/host/volna"
OUT = "build/host/fm"
FC, FM = 220.0, 220.0 * 2 ** (19 / 12)

SETUP = ["900 enc 3 -64",                                   # OSC1: WAVE 0 (a sine)
         "1000 enc 0 1", "1100 enc 3 -64", "1300 enc 4 19", "1500 enc 5 -7",   # OSC2: a sine, +19, FINE 0
         "1700 btn 3", "1800 enc 3 -40", "1900 enc 4 -40",  # MIX: OSC2, OSC3 silent
         "2100 btn 4", "2200 enc 2 60",                      # FILT: open
         "2500 btn 2", "2600 enc 0 -1", "2700 btn 2", "2800 btn 2",   # OSC 1, row C
         "2900 enc 2 2"]                                     # FM SRC OSC2


def run(lines, name):
    sp = Path(f"{OUT}-{name}.txt")
    sp.write_text("\n".join(sorted(lines, key=lambda l: int(l.split()[0]))) + "\n")
    subprocess.run([HOST, f"{OUT}-{name}", str(sp)], check=True, stdout=subprocess.DEVNULL)
    w = wave.open(f"{OUT}-{name}.wav")
    fs, n = w.getframerate(), w.getnframes()
    return fs, struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]


def amp(x, fs, f):                              # the line's amplitude (Goertzel, a Hann window)
    n = len(x)
    c = 2 * math.cos(2 * math.pi * f / fs)
    s1 = s2 = 0.0
    for i, v in enumerate(x):
        s0 = v * (0.5 - 0.5 * math.cos(2 * math.pi * i / n)) + c * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0.0)) / n * 4


def rms(x):
    return math.sqrt(sum(v * v for v in x) / max(1, len(x)))


def bright(x):
    return math.sqrt(sum((x[i] - x[i - 1]) ** 2 for i in range(1, len(x))) / len(x)) / max(rms(x), 1.0)


bad = 0


def report(name, ok, text):
    global bad
    bad += not ok
    print(f"{name:6s}: {text} {'ok' if ok else 'FAIL'}")


# SMALL, NULL, LINES: AMT 30, then 70
lines = SETUP + ["3000 enc 3 15", "3500 midi 0x90 57 100", "4500 midi 0x80 57 0",
                 "5000 enc 3 20", "5500 midi 0x90 57 100", "6500 midi 0x80 57 0", "7200 end"]
fs, x = run(lines, "pm")
a = x[int(3.8 * fs):int(4.4 * fs)]
c0, s0 = amp(a, fs, FC), amp(a, fs, FC + FM)
report("SMALL", c0 > 3 * s0, f"AMT 30: the carrier {c0:7.0f}, the sideband {s0:7.0f} (x{c0 / max(s0, 1):.1f}, want > 3)")
b = x[int(5.8 * fs):int(6.4 * fs)]
c1, s1 = amp(b, fs, FC), amp(b, fs, FC + FM)
report("NULL", c1 < 0.35 * s1, f"AMT 70: the carrier {c1:7.0f}, the sideband {s1:7.0f} (x{c1 / max(s1, 1):.2f}, want < 0.35)")
on = sum(amp(b, fs, abs(FC + k * FM)) ** 2 / 2 for k in range(-3, 4))
report("LINES", on > 0.9 * rms(b) ** 2, f"AMT 70: {100 * on / rms(b) ** 2:.1f} % of the energy on 220 +- n x 659 Hz (want > 90)")

# EP: the factory sound 05
fs, x = run(["900 btn 8", "1000 enc 6 5", "1500 midi 0x90 57 100", "2500 midi 0x80 57 0", "3200 end"], "ep")
e0, e1 = bright(x[int(1.54 * fs):int(1.62 * fs)]), bright(x[int(2.0 * fs):int(2.2 * fs)])
report("EP", e0 > 1.3 * e1, f"FM EP: brightness {e0:.3f} at the strike, {e1:.3f} later (x{e0 / max(e1, 1e-9):.2f}, want > 1.3)")

# ALIAS: a sawtooth at A6 under FM 1:1, AMT 127
alias = ["900 enc 3 63", "1000 enc 0 1", "1100 enc 3 -64", "1500 enc 5 -7",   # OSC1 a saw; OSC2 a sine, FINE 0
         "1700 btn 3", "1800 enc 3 -40", "1900 enc 4 -40", "2100 btn 4", "2200 enc 2 60",
         "2500 btn 2", "2600 enc 0 -1", "2700 btn 2", "2800 btn 2", "2900 enc 2 2", "3000 enc 3 64",
         "3500 midi 0x90 93 100", "4500 midi 0x80 93 0", "5200 end"]
fs, x = run(alias, "alias")
c = x[int(3.8 * fs):int(4.4 * fs)]
harm = sum(amp(c, fs, 1760.0 * k) ** 2 / 2 for k in range(1, 13))
frac = harm / max(rms(c) ** 2, 1.0)
report("ALIAS", frac > 0.8, f"A6 saw, FM 1:1 at AMT 127: {100 * frac:.1f} % of the energy on the harmonics (want > 80)")
sys.exit(1 if bad else 0)
