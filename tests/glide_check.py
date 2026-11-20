#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna stage 6b: check tests/scripts/glide.txt in build/host/volna.wav (a sine; A2 110 Hz, A3 220 Hz).

The pitch in a window: the rising zero crossings, each placed between its two samples, over the time they span.
  POLY, GLIDE OFF: A3 right after its note-on is A3 already;
  MONO, GLIDE 100: A3 over A2 starts on its way (between the two) and gets there;
  LEGATO: A3 on its own does not glide; A2 over it glides down; A2 let go: back up towards A3;
  BEND 12: up an octave, down an octave, back to the centre.
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]


def freq(t0, t1):
    a, b = int(t0 * fs), int(t1 * fs)
    xs = [i - 1 + d[i - 1] / (d[i - 1] - d[i]) for i in range(a + 1, b) if d[i - 1] < 0 <= d[i]]
    return (len(xs) - 1) * fs / (xs[-1] - xs[0]) if len(xs) > 2 else 0.0


bad = 0


def check(what, f, lo, hi):
    global bad
    ok = lo <= f <= hi
    bad += not ok
    print(f"{what}: {f:.1f} Hz (want {lo}..{hi}) {'ok' if ok else 'FAIL'}")


check("POLY, no glide: A3 at once", freq(2.03, 2.08), 216, 224)
check("MONO: A2 before", freq(3.85, 3.95), 108, 112)
check("MONO, GLIDE 100: on its way up", freq(4.06, 4.10), 120, 200)
check("MONO: there", freq(4.80, 4.90), 216, 224)
check("LEGATO: A3 alone, no glide", freq(6.03, 6.08), 216, 224)
check("LEGATO: A2 over it, on its way down", freq(6.56, 6.60), 120, 205)
check("LEGATO: A2 nearly there (0.35 s in: ~2 semitones left)", freq(6.82, 6.88), 112, 132)
check("LEGATO: A2 let go, back up", freq(7.40, 7.48), 187, 224)
check("BEND 12: the centre", freq(8.20, 8.28), 216, 224)
check("BEND 12: up (A4)", freq(8.50, 8.60), 432, 446)
check("BEND 12: down (A2)", freq(8.90, 9.00), 108, 112)
check("BEND 12: the centre again", freq(9.20, 9.30), 216, 224)
sys.exit(1 if bad else 0)
