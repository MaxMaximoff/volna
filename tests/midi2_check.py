#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: run tests/scripts/midi2.txt on the host and check, from the sound and the MIDI it sends out:
  the sustain pedal holds a note let go while it is down (still sounding 0.2 .. 0.7 s after the key) and lets it go
  with it (silent 0.8 s after); without the pedal the note is gone by then;
  program change 2: SUPERSAW (wide: the sides' difference against their sum over 0.3; INIT's A3 is mono, under 0.05);
  the arp on: its steps go out over MIDI (several note ons a second, each with its off), the key itself does not;
  the arp off: the key goes out once, on and off.
Exit 1 if not.   python3 tests/midi2_check.py"""
import math
import re
import struct
import subprocess
import sys
import wave

r = subprocess.run(["build/host/volna", "build/host/midi2", "tests/scripts/midi2.txt"], check=True, capture_output=True,
                   text=True)
outs = [(int(m.group(1)), int(m.group(2), 16), int(m.group(3))) for m in
        re.finditer(r"(\d+) ms  MIDI out ([0-9A-F]{2}) (\d+)", r.stdout)]
w = wave.open("build/host/midi2.wav")
fs, n = w.getframerate(), w.getnframes()
lr = struct.unpack("<%dh" % (2 * n), w.readframes(n))
d, dr = lr[0::2], lr[1::2]
rms = lambda t0, t1: math.sqrt(sum(x * x for x in d[int(t0 * fs):int(t1 * fs)]) / max(1, int((t1 - t0) * fs)))
width = lambda t0, t1: math.sqrt(sum((d[i] - dr[i]) ** 2 for i in range(int(t0 * fs), int(t1 * fs)))
                                / max(1, sum((d[i] + dr[i]) ** 2 for i in range(int(t0 * fs), int(t1 * fs)))))
bad = 0


def check(what, ok, msg):
    global bad
    bad += not ok
    print(f"{what}: {msg} {'ok' if ok else 'FAIL'}")


held, gone, plain = rms(1.5, 2.0), rms(3.0, 3.5), rms(4.8, 5.3)
check("sustain pedal", held > 500 and gone < 5 and plain < 5,
      f"0.2 .. 0.7 s after the key {held:.0f}, after the pedal {gone:.1f}, without the pedal {plain:.1f}")
wi, wn = width(1.0, 1.25), width(6.6, 6.85)
check("program change 2", wi < 0.05 and wn > 0.3, f"the width: INIT's A3 {wi:.2f}, after it {wn:.2f}")
on = [o for o in outs if 8600 <= o[0] < 9700 and o[1] == 0x90]
off = [o for o in outs if 8600 <= o[0] < 9900 and o[1] == 0x80]
check("the arp's MIDI out", 6 <= len(on) <= 10 and len(off) == len(on),
      f"{len(on)} note ons, {len(off)} offs in a second at 120 BPM 1/16 (want ~8, each with its off)")
k = [o for o in outs if 9950 <= o[0] < 10600]
check("the key's MIDI out, the arp off", [o[1] for o in k] == [0x90, 0x80] and k[0][2] == 57, f"{k}")
sys.exit(1 if bad else 0)
