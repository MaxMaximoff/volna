#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna (2026-11-15): hard sync, heard. OSC2 a sawtooth 5 semitones up (FINE 0; not 7: at 1.5 x OSC2 is cut where
a saw or a sine crosses 0, the jump all but none), OSC1 and OSC3 silent in the mix
(OSC3 the master all the same, at the note: INIT's is an octave down), the filter open; A3 (220 Hz) held 1 s with SYNC off, then on (the lines: Goertzel):
  OFF     the energy on the harmonics of OSC2's own 293.7 Hz;
  ON      the energy on the harmonics of OSC3's 220 Hz (the note's pitch is the master's), little left at 293.7 Hz;
  ALIAS   A4 and A6 (440, 1760 Hz masters): the energy off the harmonics (what folds back lands there; the polyBLEP
          and OSC2's darker level keep it down), FFT (tests/offharm.py): under -35 and -30 dB (without the polyBLEP
          -33.1 and -27.8);
  LEAD    factory 06 SYNC LEAD, a note with the wheel (CC 1) at 0, then at 127 (WHEEL -> PIT2): another tone (the first
          20 harmonics of 220 Hz in another proportion: 1 - their cosine over 0.15), the pitch 220 Hz both times.
Runs the host build of firmware/src/volna.c (tests/run_host.sh builds it first). Exit 1 if one is off.
    python3 tests/sync_check.py [build/host/volna]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from offharm import off_db  # noqa: E402

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "build/host/volna"
OUT = "build/host/sync"
SETUP = ["900 btn 3", "1000 enc 2 -40", "1100 enc 4 -40",            # MIX: OSC1, OSC3 silent
         "1300 btn 4", "1400 enc 2 60",                                # FILT: open
         "1600 btn 2", "1700 enc 0 1", "1800 enc 3 40", "2200 enc 4 5", "2400 enc 5 -7",   # OSC2: a saw, +5, FINE 0
         "2450 enc 0 1", "2500 enc 4 12", "2550 enc 0 -1",            # OSC3 (INIT's sub) up to the note
         "2600 btn 2", "2700 btn 2"]                                   # OSC2's row C (KNOB3: SYNC)


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


def harm(x, fs, f0):                            # the share of the energy (DC aside) on f0's harmonics (all under
    m = sum(x) / len(x)                         # fs / 2: what folds back lands between them)
    x = [v - m for v in x]
    return sum(amp(x, fs, f0 * k) ** 2 / 2 for k in range(1, int((fs / 2 - 50) / f0) + 1)) / max(rms(x) ** 2, 1.0)


def bright(x):
    return math.sqrt(sum((x[i] - x[i - 1]) ** 2 for i in range(1, len(x))) / len(x)) / max(rms(x), 1.0)


bad = 0


def report(name, ok, text):
    global bad
    bad += not ok
    print(f"{name:5s}: {text} {'ok' if ok else 'FAIL'}")


F2 = 220.0 * 2 ** (5 / 12)
fs, x = run(SETUP + ["3500 midi 0x90 57 100", "4500 midi 0x80 57 0", "4700 enc 4 1",
                     "5500 midi 0x90 57 100", "6500 midi 0x80 57 0", "7200 end"], "pitch")
a, b = x[int(3.8 * fs):int(4.4 * fs)], x[int(5.8 * fs):int(6.4 * fs)]
h = harm(a, fs, F2)
report("OFF", h > 0.95, f"SYNC off: {100 * h:.1f} % on OSC2's 293.7 Hz harmonics (want > 95)")
h, r = harm(b, fs, 220.0), amp(b, fs, F2) / max(amp(b, fs, 220.0), 1.0)
report("ON", h > 0.95 and r < 0.1, f"SYNC on: {100 * h:.1f} % on 220 Hz's harmonics (want > 95), 293.7 Hz x{r:.3f} of 220 Hz")

for note, f0, lim in ((69, 440.0, -35.0), (93, 1760.0, -30.0)):   # (without the polyBLEP: -33.1, -27.8)
    fs, x = run(SETUP + ["2900 enc 4 1", f"3500 midi 0x90 {note} 100", f"4500 midi 0x80 {note} 0", "5200 end"], "alias")
    d = off_db(list(x[int(3.8 * fs):]), fs, f0)
    report("ALIAS", d < lim, f"{f0:.0f} Hz synced: {d:.1f} dB off the harmonics (want under {lim:.0f})")

fs, x = run(["900 btn 8", "1000 enc 6 6", "1400 midi 0xB0 1 0", "1500 midi 0x90 57 100", "2500 midi 0x80 57 0",
             "3400 midi 0xB0 1 127", "3500 midi 0x90 57 100", "4500 midi 0x80 57 0", "5200 end"], "lead")
e0, e1 = x[int(1.8 * fs):int(1.87 * fs)], x[int(3.8 * fs):int(3.87 * fs)]   # (past the strike, before the first echo)
v0, v1 = [amp(e0, fs, 220.0 * k) for k in range(1, 21)], [amp(e1, fs, 220.0 * k) for k in range(1, 21)]
cs = sum(a * b for a, b in zip(v0, v1)) / max(math.sqrt(sum(a * a for a in v0) * sum(b * b for b in v1)), 1e-9)
h0, h1 = harm(e0, fs, 220.0), harm(e1, fs, 220.0)
report("LEAD", 1 - cs > 0.15 and h0 > 0.9 and h1 > 0.9,
       f"SYNC LEAD, the wheel 0 against 127 (OSC2 +7.6 semitones): 1 - cos {1 - cs:.2f} of the first 20 harmonics "
       f"(want > 0.15); on 220 Hz's harmonics {100 * h0:.0f} % and {100 * h1:.0f} % (want > 90)")
sys.exit(1 if bad else 0)
