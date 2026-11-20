#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna (2026-11-16): the filters' DRIVE curves, heard. OSC1 a sine (BASIC wave 0) alone, filter 1 open (a low pass at
the top), its DRIVE 64; CURVE (row C) each in turn; A3 (220 Hz) held 0.8 s each. A static curve on a sine gives its
harmonics: the symmetric ones (CLIP HARD MEDIUM SOFT SQUARE SINE) odd ones only (the 2nd under 1 % of the 1st), the
lopsided ones (TUBE PICKUP1 PICKUP2 RECTIFY OSC1 MOD: OSC1 times itself) a 2nd over 2 %; each curve other than SOFT
another tone than SOFT's (the overtones 2..10 against the 1st, in dB: on average over 1.5 dB apart; BINARY and OVERFLOW
only that); no DC left (under 5 % of the rms: the lopsided ones' blocker).
Runs the host build of firmware/src/volna.c (tests/run_host.sh builds it first). Exit 1 if one is off.
    python3 tests/dcurve_check.py [build/host/volna]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "build/host/volna"
OUT = "build/host/dcurve"
NAMES = ["CLIP", "TUBE", "HARD", "MEDIUM", "SOFT", "PICKUP1", "PICKUP2", "RECTIFY", "SQUARE", "BINARY", "OVERFLOW",
         "SINE", "OSC1 MOD"]                    # (the knob's order; SOFT at 4, a patch's default)
ODD = {"CLIP", "HARD", "MEDIUM", "SOFT", "SQUARE", "SINE"}
EVEN = {"TUBE", "PICKUP1", "PICKUP2", "RECTIFY", "OSC1 MOD"}
lines = ["800 enc 3 -64",                                        # OSC1: WAVE 0
         "900 btn 3", "1000 enc 3 -40", "1100 enc 4 -40",       # MIX: OSC2 OSC3 off
         "1300 btn 4", "1400 enc 2 60", "1500 enc 4 -24",       # FILT: open, ENV 0
         "1700 btn 4", "1800 enc 4 16",                          # row B: DRIVE 64
         "2000 btn 4", "2100 enc 2 -4"]                          # row C: CURVE from SOFT to CLIP
T0 = 3000
for i in range(len(NAMES)):
    t = T0 + i * 1200
    if i:
        lines.append(f"{t - 200} enc 2 1")
    lines += [f"{t} midi 0x90 57 100", f"{t + 800} midi 0x80 57 0"]
lines.append(f"{T0 + len(NAMES) * 1200} end")
sp = Path(OUT + ".txt")
sp.write_text("\n".join(lines) + "\n")
subprocess.run([HOST, OUT, str(sp)], check=True, stdout=subprocess.DEVNULL)
w = wave.open(OUT + ".wav")
fs, n = w.getframerate(), w.getnframes()
x = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]


def amp(a, f):                                  # Goertzel, a Hann window
    m = len(a)
    c = 2 * math.cos(2 * math.pi * f / fs)
    s1 = s2 = 0.0
    for i, v in enumerate(a):
        s0 = v * (0.5 - 0.5 * math.cos(2 * math.pi * i / m)) + c * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0.0)) / m * 4


H, A = {}, {}
for i, name in enumerate(NAMES):
    t = (T0 + i * 1200) / 1000
    a = x[int((t + 0.2) * fs):int((t + 0.7) * fs)]
    H[name] = [amp(a, 220.0 * k) for k in range(1, 11)]
    A[name] = a
bad = 0
soft = H["SOFT"]
for name in NAMES:
    h, a = H[name], A[name]
    h2 = h[1] / max(h[0], 1.0)
    db = lambda v: [20 * math.log10(max(u, 1e-2) / max(v[0], 1e-2)) for u in v[1:]]   # (the overtones against the 1st)
    dd = sum(abs(p - q) for p, q in zip(db(h), db(soft))) / 9
    ok = True
    if name in ODD:
        ok &= h2 < 0.01
    if name in EVEN:
        ok &= h2 > 0.02
    if name != "SOFT":
        ok &= dd > 1.5
    dc = abs(sum(a) / len(a)) / max(math.sqrt(sum(v * v for v in a) / len(a)), 1.0)   # (the blocker: no DC left)
    ok &= dc < 0.05
    bad += not ok
    print(f"{name:8s}: 2nd/1st {h2:8.3f}, against SOFT {dd:5.1f} dB apart, DC {100 * dc:4.1f} % of the rms, harmonics "
          + " ".join(f"{20 * math.log10(max(v, 1e-3) / max(h[0], 1e-3)):5.0f}" for v in h[1:6]) + f" {'ok' if ok else 'FAIL'}")
sys.exit(1 if bad else 0)
