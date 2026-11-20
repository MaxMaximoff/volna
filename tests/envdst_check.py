#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: every envelope destination of the matrix (A. F. 3. 4. ATK DEC SUS REL), heard.

For each: slot 1 WHEEL (MIDI CC 1) -> the destination, +-63; for ENV 3 and ENV 4 also slot 2 ENV 3 / ENV 4 -> CUT1
+24 so that they can be heard (+63 would push the cutoff to its top either way) (and for 4.REL slot 3 VELO -> 4.SUS +63: ENV 4 sustains, so its release matters).
OSC1 a saw (WAVE 63), OSC2 and OSC3 off. A3 held 1 s then let go, once with CC 1 at 0, once at 127; what the destination should change is
measured on both and compared:
  ATK  the level (A.) or the brightness (F. 3. 4.) in the first 0.1 s: lower at 127 (a slower attack);
  DEC  the same 0.5 .. 0.9 s into the note: higher at 127 (a slower decay keeps it up);
  SUS  the same 0.5 .. 0.9 s into the note: changed as the amount says;
  REL  the same 0.05 .. 0.35 s after the key: higher at 127 (a slower release).
Runs the host build of firmware/src/volna.c (tests/run_host.sh builds it first). Exit 1 if one is not heard.
    python3 tests/envdst_check.py [build/host/volna]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "build/host/volna"
OUT = "build/host/envdst"
DESTS = ["A.ATK", "A.DEC", "A.SUS", "A.REL", "F.ATK", "F.DEC", "F.SUS", "F.REL",
         "3.ATK", "3.DEC", "3.SUS", "3.REL", "4.ATK", "4.DEC", "4.SUS", "4.REL"]
MS_VELO, MS_WHEEL, MS_ENV3 = 6, 9, 11   # (MATRIX: KNOB2 the group, KNOB3 the destination in it)


def script(k, amt):
    """the script for destination k (0..15) at amount amt"""
    env, par = k // 4, k % 4
    s = ["810 enc 2 40", "830 enc 2 -4",                              # OSC1: TABLE +40, the last table, then back past the 4 user slots (2026-11-17: empty, BASIC) (as before the 2026-11-15 layout; a saw, ANALOG WAVE 0, leaves 4.ATK at x 0.93)
         "900 btn 3", "910 enc 3 -40", "920 enc 4 -40",              # OSC2 OSC3 off
         "1400 btn 6", "1450 btn 6", "1500 btn 6",                     # MOD, row C: MATRIX, slot 1
         f"1550 enc 2 {MS_WHEEL}", "1700 enc 3 5", f"1800 enc 4 {k}", f"2000 enc 5 {amt}"]   # (GROUP 5: ENV)
    t = 2800
    if env >= 2:                                                      # slot 2: ENV 3 / 4 -> CUT1 (to hear it)
        s += [f"{t} enc 0 1", f"{t + 50} enc 2 {MS_ENV3 + env - 2}", f"{t + 250} enc 3 3", f"{t + 350} enc 4 1",
              f"{t + 500} enc 5 24"]                                  # (GROUP 3 FILT, its 2nd: CUT1)
        t += 1200
        if env == 3 and par == 3:                                     # slot 3: VELO -> 4.SUS (ENV 4 sustains)
            s += [f"{t} enc 0 1", f"{t + 50} enc 2 {MS_VELO}", f"{t + 150} enc 3 5", f"{t + 250} enc 4 14",
                  f"{t + 450} enc 5 63"]                                # (ENV, its 15th: 4.SUS)
            t += 1200
    a, b = t + 200, t + 3200
    s += [f"{a - 100} midi 0xB0 1 0", f"{a} midi 0x90 57 100", f"{a + 1000} midi 0x80 57 0",
          f"{b - 100} midi 0xB0 1 127", f"{b} midi 0x90 57 100", f"{b + 1000} midi 0x80 57 0", f"{b + 2600} end"]
    return "\n".join(s) + "\n", a / 1000.0, b / 1000.0


def measure(path, t0, t1, bright):
    w = wave.open(path)
    fs, n = w.getframerate(), w.getnframes()
    d = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
    seg = d[int(t0 * fs):int(t1 * fs)]
    e = sum(x * x for x in seg) / len(seg)
    if not bright:
        return math.sqrt(e)
    dd = sum((seg[i] - seg[i - 1]) ** 2 for i in range(1, len(seg))) / len(seg)
    return math.sqrt(dd / max(e, 1.0))


bad = 0
for k, name in enumerate(DESTS):
    if "--only" in sys.argv and name != sys.argv[-1]:   # (one destination: --only 4.ATK)
        continue
    par = k % 4
    amt = -63 if name in ("A.SUS", "3.SUS") else 63
    text, a, b = script(k, amt)
    sp = Path(OUT + ".txt")
    sp.write_text(text)
    if "--shots" in sys.argv and name == sys.argv[-1]:            # (a destination's screens, to look at)
        lines = text.strip().split("\n") + ["2700 shot", f"{int(a * 1000) + 600} shot", f"{int(b * 1000) + 600} shot"]
        sp.write_text("\n".join(sorted(lines, key=lambda l: int(l.split()[0]))) + "\n")
    subprocess.run([HOST, OUT, str(sp)], check=True, stdout=subprocess.DEVNULL)
    win = [(0.0, 0.1), (0.5, 0.9), (0.5, 0.9), (1.05, 1.35)][par]
    bright = k >= 4
    x0 = measure(OUT + ".wav", a + win[0], a + win[1], bright)
    x1 = measure(OUT + ".wav", b + win[0], b + win[1], bright)
    r = x1 / max(x0, 1e-9)
    want_up = not (par == 0 or amt < 0)
    ok = r > 1.1 if want_up else r < 0.9
    bad += not ok
    print(f"{name:6s} {amt:+3d}: {'brightness' if bright else 'level':10s} x{r:5.2f} at CC 1 127 "
          f"(want {'up' if want_up else 'down'}) {'ok' if ok else 'NOT HEARD'}")
sys.exit(1 if bad else 0)
