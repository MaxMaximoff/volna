#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna (2026-11-16): Filter FM and the filters' PAN, heard. FILTER row C: CURVE, FM SRC, FM AMT, PAN.
  FFM   OSC1 a sawtooth through filter 1 (a low pass at CUTOFF 56, RESO 40, its ENV and KEYTRK 0), its cutoff moved
        by OSC2 (a sine at the note, FINE 0, silent in the mix): A3 with FM AMT 0, then 100: another tone (the
        overtones 2..10 against the 1st over 1.5 dB apart on average), still on 220 Hz's harmonics (the source at the
        note: the tone stays periodic), over 90 %;
  PAR   ROUTE PAR, filter 1's PAN all left: the right side silent (under 2 % of the left's rms);
  SER   ROUTE SER, filter 2's PAN all right (filter 2 off: what comes out of 1 passes it): the left side silent.
Runs the host build of firmware/src/volna.c (tests/run_host.sh builds it first). Exit 1 if one is off.
    python3 tests/ffm_check.py [build/host/volna]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "build/host/volna"
OUT = "build/host/ffm"


def run(lines, name):
    sp = Path(f"{OUT}-{name}.txt")
    sp.write_text("\n".join(sorted(lines, key=lambda l: int(l.split()[0]))) + "\n")
    subprocess.run([HOST, f"{OUT}-{name}", str(sp)], check=True, stdout=subprocess.DEVNULL)
    w = wave.open(f"{OUT}-{name}.wav")
    fs, n = w.getframerate(), w.getnframes()
    raw = struct.unpack("<%dh" % (2 * n), w.readframes(n))
    return fs, raw[0::2], raw[1::2]


def amp(a, fs, f):                              # Goertzel, a Hann window
    m = len(a)
    c = 2 * math.cos(2 * math.pi * f / fs)
    s1 = s2 = 0.0
    for i, v in enumerate(a):
        s0 = v * (0.5 - 0.5 * math.cos(2 * math.pi * i / m)) + c * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0.0)) / m * 4


def rms(a):
    return math.sqrt(sum(v * v for v in a) / max(len(a), 1))


bad = 0


def report(name, ok, text):
    global bad
    bad += not ok
    print(f"{name:4s}: {text} {'ok' if ok else 'FAIL'}")


SAW = ["800 enc 3 40",                                             # OSC1: WAVE 63 (a saw)
       "850 enc 0 1", "900 enc 3 -64", "950 enc 5 -7", "1000 enc 0 -1",   # OSC2: a sine, FINE 0
       "1100 btn 3", "1200 enc 3 -40", "1300 enc 4 -40",           # MIX: OSC2 OSC3 silent
       "1500 btn 4", "1600 enc 2 -10", "1700 enc 3 8", "1800 enc 4 -24", "1900 enc 5 -10",   # FILT: 56, RESO 40
       "2000 btn 4", "2100 btn 4", "2200 enc 3 2"]                 # row C: FM SRC OSC2
fs, L, R = run(SAW + ["3000 midi 0x90 57 100", "4000 midi 0x80 57 0", "4200 enc 4 50",
                      "5000 midi 0x90 57 100", "6000 midi 0x80 57 0", "6500 end"], "fm")
h0 = [amp(L[int(3.3 * fs):int(3.9 * fs)], fs, 220.0 * k) for k in range(1, 11)]
a1 = L[int(5.3 * fs):int(5.9 * fs)]
h1 = [amp(a1, fs, 220.0 * k) for k in range(1, 11)]
db = lambda v: [20 * math.log10(max(u, 1e-2) / max(v[0], 1e-2)) for u in v[1:]]
dd = sum(abs(p - q) for p, q in zip(db(h0), db(h1))) / 9
m = sum(a1) / len(a1)
hs = sum(amp(a1, fs, 220.0 * k) ** 2 / 2 for k in range(1, 100)) / max(rms([v - m for v in a1]) ** 2, 1.0)
report("FFM", dd > 1.5 and hs > 0.9, f"FM AMT 0 against 100: {dd:.1f} dB apart (want > 1.5), {100 * hs:.0f} % on 220 Hz's harmonics (want > 90)")

fs, L, R = run(["900 btn 4", "1000 btn 4", "1100 enc 3 1", "1200 btn 4", "1300 enc 5 -40",
                "2000 midi 0x90 57 100", "3000 midi 0x80 57 0", "3500 end"], "par")
l, r = rms(L[int(2.3 * fs):int(2.9 * fs)]), rms(R[int(2.3 * fs):int(2.9 * fs)])
report("PAR", r < 0.02 * l, f"PAR, filter 1 all left: left {l:.0f}, right {r:.0f}")

fs, L, R = run(["900 btn 4", "1000 btn 4", "1100 btn 4", "1200 enc 0 1", "1300 enc 5 40",
                "2000 midi 0x90 57 100", "3000 midi 0x80 57 0", "3500 end"], "ser")
l, r = rms(L[int(2.3 * fs):int(2.9 * fs)]), rms(R[int(2.3 * fs):int(2.9 * fs)])
report("SER", l < 0.02 * r, f"SER, filter 2 all right: left {l:.0f}, right {r:.0f}")
sys.exit(1 if bad else 0)
