#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna stages 5a 5b: count the arpeggiator's steps in build/host/volna.wav after tests/scripts/arp.txt (or
another script's, its phases given as NAME:T0:T1:RATE after the file).

A step is an attack: the level over 4 ms windows jumps (the default sound: a 2 ms attack, the gate at 50 %, so the
level falls between steps). Each phase of the script should have its rate of steps; exit 1 if one is off by more
than a step. Run by tests/run_tests.sh after the script."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")   # then phases: NAME:T0:T1:RATE ...
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
win = int(fs * 0.004)
lv = [math.sqrt(sum(x * x for x in d[i:i + win]) / win) for i in range(0, n - win, win)]
onsets = [i * win / fs for i in range(2, len(lv)) if lv[i] > 1.6 * lv[i - 2] + 150 and lv[i - 1] <= 1.6 * lv[i - 3] + 150]


def count(t0, t1):
    return sum(1 for t in onsets if t0 <= t < t1)


PHASES = [("ON, 120 BPM 1/16, 8 a second", 1.05, 2.95, 8.0),
          ("ON, keys let go: silent", 3.05, 3.95, 0.0),
          ("ON, 180 BPM 1/8, 6 a second", 4.05, 5.95, 6.0),
          ("HOLD after letting go, 6 a second", 6.55, 7.95, 6.0),
          ("MIDI clock 90 BPM, 1/8: 3 a second", 8.2, 9.95, 3.0),
          ("RUN off: silent", 10.3, 10.95, 0.0)]
if len(sys.argv) > 2:                           # the phases of another script
    PHASES = [(f.split(":")[0], float(f.split(":")[1]), float(f.split(":")[2]), float(f.split(":")[3]))
              for f in sys.argv[2:]]
bad = 0
for name, t0, t1, rate in PHASES:
    got, want = count(t0, t1), rate * (t1 - t0)
    ok = abs(got - want) <= 1.0 + 0.05 * want
    bad += not ok
    print(f"{name:40s} {got:3d} steps, want {want:5.1f} {'ok' if ok else 'FAIL'}")
sys.exit(1 if bad else 0)
