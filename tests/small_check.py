#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna (2026-11-16): the smaller things after the Blofeld's, heard. OSC1 a sine (BASIC wave 0) alone, filter 1
open, its ENV 0; the pitch from interpolated zero crossings (40 ms windows):
  GLISS    MONO, GLIDE 100, G.MODE GLISS: A3 held, A4 on it: during the glide (20 ms windows) the pitch sits on
           semitones (off by under 0.6 x PORTA's, on average), PORTA's glides through them (over 0.2);
  FINGER   MONO, GLIDE 100, G.MODE FING P: A4 after A3 let go starts at A4 (no glide); A4 on A3 held glides;
  KEYTRK   OSC1 KEYTRK 0 %: A3 and A4 at the same pitch (C4's, 261.6 Hz);
  BEND     OSC1's BEND +12: the wheel at the top bends it an octave (A3 at 440 Hz);
  FADE     LFO 1 (a sine, ~4.5 Hz) on PITCH, DELAY 84 (~0.4 s), FADE OUT: the vibrato wide at first, gone later;
  LIMIT    LFO 1 (a slow saw) on WAVE across the table, LIMIT ON (OSC row D): the tone moves in steps (20 ms
           windows as the one before, the 2nd and 3rd harmonics: 20 points more than off; part of the cycle stays at the
           table's end either way).
Runs the host build of firmware/src/volna.c (tests/run_host.sh builds it first). Exit 1 if one is off.
    python3 tests/small_check.py [build/host/volna]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "build/host/volna"
OUT = "build/host/small"
BASE = ["700 enc 3 -64",                                            # OSC1 a sine
        "750 btn 3", "800 enc 3 -40", "850 enc 4 -40",              # MIX: OSC2 OSC3 off
        "900 btn 4", "950 enc 2 60", "1000 enc 4 -24"]              # FILT: open, ENV 0


def run(lines, name):
    sp = Path(f"{OUT}-{name}.txt")
    sp.write_text("\n".join(sorted(lines, key=lambda l: int(l.split()[0]))) + "\n")
    subprocess.run([HOST, f"{OUT}-{name}", str(sp)], check=True, stdout=subprocess.DEVNULL)
    w = wave.open(f"{OUT}-{name}.wav")
    fs, n = w.getframerate(), w.getnframes()
    return fs, struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]


def pitch(x, fs, t0, t1):                       # from the rising zero crossings, interpolated
    a = x[int(t0 * fs):int(t1 * fs)]
    z = [i - 1 + a[i - 1] / (a[i - 1] - a[i]) for i in range(1, len(a)) if a[i - 1] < 0 <= a[i]]
    return (len(z) - 1) * fs / (z[-1] - z[0]) if len(z) > 2 else 0.0


def track(x, fs, t0, t1, w=0.04):
    out, t = [], t0
    while t + w <= t1:
        out.append(pitch(x, fs, t, t + w))
        t += w / 2
    return out


bad = 0


def report(name, ok, text):
    global bad
    bad += not ok
    print(f"{name:6s}: {text} {'ok' if ok else 'FAIL'}")


def semis_off(fr):                              # how far, on average, from the nearest semitone over A3
    v = [12 * math.log2(f / 220.0) for f in fr if f > 100]
    v = [s for s in v if 0.5 < s < 11.5]       # (the glide's middle)
    return sum(abs(s - round(s)) for s in v) / max(len(v), 1), len(v)


GL = ["1100 btn 8", "1200 enc 3 50", "1300 enc 4 1"]                # PATCH: GLIDE 100, MODE MONO
fs, x = run(BASE + GL + ["1400 btn 8", "1500 enc 3 2",              # row B: G.MODE GLISS
                         "2000 midi 0x90 57 100", "2500 midi 0x90 69 100", "5000 midi 0x80 69 0",
                         "5050 midi 0x80 57 0", "5500 end"], "gliss")
og, ng = semis_off(track(x, fs, 2.5, 5.0, 0.02))
fs, x = run(BASE + GL + ["2000 midi 0x90 57 100", "2500 midi 0x90 69 100", "5000 midi 0x80 69 0",
                         "5050 midi 0x80 57 0", "5500 end"], "porta")
op, npo = semis_off(track(x, fs, 2.5, 5.0, 0.02))
report("GLISS", og < 0.6 * op and op > 0.2 and ng > 5 and npo > 5,
       f"off the semitones during the glide: GLISS {og:.2f} ({ng} windows), PORTA {op:.2f} ({npo})")

FG = ["1100 btn 8", "1200 enc 3 50", "1300 enc 4 1", "1400 btn 8", "1500 enc 3 1"]   # GLIDE 100, MONO, G.MODE FING P
fs, x = run(BASE + FG + ["2000 midi 0x90 57 100", "2500 midi 0x80 57 0", "3000 midi 0x90 69 100", "3600 midi 0x80 69 0",
                         "4000 midi 0x90 57 100", "4500 midi 0x90 69 100", "5100 midi 0x80 69 0",
                         "5150 midi 0x80 57 0", "5600 end"], "finger")
a_sep, a_leg = pitch(x, fs, 3.0, 3.04), pitch(x, fs, 4.5, 4.54)
report("FINGER", abs(a_sep - 440) < 10 and a_leg < 400,
       f"A4 alone starts at {a_sep:.0f} Hz (want 440), on A3 held at {a_leg:.0f} Hz (gliding, want under 400)")

fs, x = run(BASE + ["1100 btn 2", "1150 btn 2", "1200 enc 5 -10",   # (OSC: row A, then) row B: KEYTRK 0 %
                    "2000 midi 0x90 57 100", "2800 midi 0x80 57 0", "3000 midi 0x90 69 100", "3800 midi 0x80 69 0",
                    "4200 end"], "keytrk")
p3, p4 = pitch(x, fs, 2.2, 2.7), pitch(x, fs, 3.2, 3.7)
report("KEYTRK", abs(p3 - 261.6) < 2 and abs(p4 - 261.6) < 2, f"KEYTRK 0 %: A3 at {p3:.1f} Hz, A4 at {p4:.1f} Hz (want 261.6)")

fs, x = run(BASE + ["1100 btn 2", "1150 btn 2", "1175 btn 2", "1200 enc 5 37",   # OSC row C: BEND +12
                    "1900 midi 0xE0 127 127", "2000 midi 0x90 57 100", "2800 midi 0x80 57 0", "3200 end"], "bend")
pb = pitch(x, fs, 2.2, 2.7)
report("BEND", abs(pb - 440) < 4, f"OSC1 BEND +12, the wheel at the top: A3 at {pb:.1f} Hz (want ~440)")

fs, x = run(BASE + ["1100 btn 6", "1200 enc 3 12", "1300 enc 5 21",   # LFO 1: RATE ~4.5 Hz, DELAY 84 (~0.4 s)
                    "1400 btn 6", "1500 enc 4 1",                   # row B: FADE OUT
                    "1600 btn 6", "1700 enc 2 1", "1750 enc 3 1", "1800 enc 5 30",   # MATRIX slot 1: LFO1 -> PITCH +30
                    "2500 midi 0x90 57 100", "5500 midi 0x80 57 0", "6000 end"], "fade")
sd = lambda fr: math.sqrt(sum((f - sum(fr) / len(fr)) ** 2 for f in fr) / len(fr))
e, l = sd(track(x, fs, 2.55, 3.2, 0.02)), sd(track(x, fs, 4.6, 5.4, 0.02))
report("FADE", e > 4 * max(l, 0.3), f"the vibrato's spread: {e:.1f} Hz at first, {l:.2f} Hz later (FADE OUT)")


def amp(a, fs, f):                              # Goertzel, a Hann window
    m = len(a)
    c = 2 * math.cos(2 * math.pi * f / fs)
    s1 = s2 = 0.0
    for i, v in enumerate(a):
        s0 = v * (0.5 - 0.5 * math.cos(2 * math.pi * i / m)) + c * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0.0)) / m * 4


def steps(x, fs, t0, t1, w=0.02):               # how many windows (20 ms, every 10) sound as the one before: the
    b = []                                      # 2nd and 3rd harmonics of 440 Hz against the 1st, within 0.5 %
    t = t0
    while t + w <= t1:
        a = x[int(t * fs):int((t + w) * fs)]
        h1 = max(amp(a, fs, 440.0), 1.0)
        b.append((amp(a, fs, 880.0) / h1, amp(a, fs, 1320.0) / h1))
        t += w / 2
    same = sum(1 for i in range(1, len(b)) if all(abs(b[i][k] - b[i - 1][k]) <= 0.005 * max(b[i - 1][k], 0.01) for k in (0, 1)))
    return same / (len(b) - 1)


LW = ["1100 btn 6", "1200 enc 2 3", "1300 enc 3 -10",               # LFO 1: a saw, slow
      "1400 btn 6", "1500 btn 6", "1600 enc 2 1", "1700 enc 3 1", "1800 enc 4 4", "1900 enc 5 63"]   # LFO1 -> WAVE +63
fs, x = run(BASE + LW + ["2000 btn 2", "2050 btn 2", "2100 btn 2", "2125 btn 2", "2150 enc 2 1",   # OSC row D: LIMIT
                         "3000 midi 0x90 69 100", "5000 midi 0x80 69 0", "5500 end"], "limit")
s_on = steps(x, fs, 3.2, 4.8)
fs, x = run(BASE + LW + ["3000 midi 0x90 69 100", "5000 midi 0x80 69 0", "5500 end"], "nolimit")
s_off = steps(x, fs, 3.2, 4.8)
report("LIMIT", s_on > s_off + 0.2, f"windows as the one before: LIMIT ON {100 * s_on:.0f} %, OFF {100 * s_off:.0f} %")
sys.exit(1 if bad else 0)
