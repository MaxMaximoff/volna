#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna (2026-11-16): check tests/scripts/init.txt in build/host/volna.wav: INIT's note (A3, 220 Hz) is one sine.
  - it sounds (not silence) and left = right (a mono voice, no effects);
  - its pitch is 220 Hz (OSC1 at the note; the old INIT's OSC3 an octave down would pull the zero crossings);
  - all but a sliver of its energy is on 220 Hz: under 1 % on the harmonics 2..10 and on 110 Hz (OSC3's octave
    down) and 220 x 2^(7/1200) (OSC2's 7 cents up: as a beat, over the window its energy moves the 220 Hz bin).
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
raw = struct.unpack("<%dh" % (2 * n), w.readframes(n))
L, R = raw[0::2], raw[1::2]
t0, t1 = 1.25, 1.85                             # the held note, past the attack and the filter envelope's drop
s = L[int(t0 * fs):int(t1 * fs)]


def power(x, f):
    """the power at f (Goertzel, a Hann window)"""
    k = 2.0 * math.cos(2.0 * math.pi * f / fs)
    a = b = 0.0
    m = len(x)
    for i, v in enumerate(x):
        a, b = v * (0.5 - 0.5 * math.cos(2.0 * math.pi * i / m)) + k * a - b, a
    return a * a + b * b - k * a * b


def freq(x):
    xs = [i - 1 + x[i - 1] / (x[i - 1] - x[i]) for i in range(1, len(x)) if x[i - 1] < 0 <= x[i]]
    return (len(xs) - 1) * fs / (xs[-1] - xs[0]) if len(xs) > 2 else 0.0


bad = 0
peak = max(abs(v) for v in s)
same = max(abs(a - b) for a, b in zip(L, R))
f = freq(s)
p1 = power(s, 220.0)
other = sum(power(s, 220.0 * h) for h in range(2, 11)) + power(s, 110.0)
share = 100.0 * other / (p1 + other) if p1 + other else 100.0
for ok, msg in ((peak > 2000, "INIT sounds: peak %d (want > 2000)" % peak),
                (same == 0, "left = right: the largest difference %d (want 0)" % same),
                (abs(f - 220.0) < 0.5, "the pitch %.2f Hz (want 220 +- 0.5)" % f),
                (share < 1.0, "%.3f %% of the energy off 220 Hz (110 Hz, harmonics 2..10; want < 1)" % share)):
    print("%-5s %s" % ("" if ok else "FAIL", msg))
    bad += not ok
sys.exit(1 if bad else 0)
