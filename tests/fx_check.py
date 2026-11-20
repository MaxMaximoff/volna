#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna stage 7b: check tests/scripts/fx.txt in build/host/volna.wav (a short pluck, both channels).
  no effects: silence after the tap;
  DELAY (TIME 70 = 248 ms, FEEDBK 50 = x 0.388, MIX 64): the echo at 248 ms (autocorrelation), each echo ~0.39 of
  the one before;
  DELAY synced 1/8 at 120 BPM with PING 127: the first echo 250 ms after on the left only, the second on the right;
  REVERB (SIZE 80): a tail on both sides, not the same (a stereo plate), gone within ~2.5 s;
  DELAY into REVERB: still a tail a second after.
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
raw = struct.unpack("<%dh" % (2 * n), w.readframes(n))
L, R = raw[0::2], raw[1::2]


def seg(d, t0, t1):
    return d[int(t0 * fs):int(t1 * fs)]


def rms(d, t0, t1):
    s = seg(d, t0, t1)
    return math.sqrt(sum(x * x for x in s) / max(1, len(s)))


def corr(a, b):
    return sum(x * y for x, y in zip(a, b)) / math.sqrt(sum(x * x for x in a) * sum(y * y for y in b) or 1)


bad = 0


def check(what, ok, msg):
    global bad
    bad += not ok
    print(f"{what}: {msg} {'ok' if ok else 'FAIL'}")


q = rms(L, 2.0, 2.45)
check("no effects", q < 2, f"after the tap {q:.1f}")
s = seg(L, 3.0, 4.2)                            # the echo's lag: the autocorrelation's peak in 150 .. 400 ms
lag = max(range(int(0.15 * fs), int(0.4 * fs), 4), key=lambda k: sum(s[i] * s[i + k] for i in range(0, len(s) - k, 3)))
lms = lag * 1000 / fs
e = [rms(L, 3.0 + 0.248 * k, 3.0 + 0.248 * k + 0.1) for k in range(1, 4)]
check("DELAY 248 ms", abs(lms - 248) < 3 and 0.3 < e[1] / e[0] < 0.48 and 0.3 < e[2] / e[1] < 0.48,
      f"the echo at {lms:.0f} ms, each x{e[1] / e[0]:.2f} x{e[2] / e[1]:.2f} of the last (want 0.39)")
l1, r1, l2, r2 = rms(L, 5.75, 5.85), rms(R, 5.75, 5.85), rms(L, 6.0, 6.1), rms(R, 6.0, 6.1)
check("DELAY 1/8 PING", l1 > 20 * (r1 + 1) and r2 > 20 * (l2 + 1),
      f"250 ms: left {l1:.0f} right {r1:.0f}; 500 ms: left {l2:.0f} right {r2:.0f}")
tl, tr = rms(L, 8.3, 9.0), rms(R, 8.3, 9.0)
c = corr(seg(L, 8.3, 9.0), seg(R, 8.3, 9.0))
end = max(rms(L, 10.6, 11.6), rms(R, 10.6, 11.6))
check("REVERB", tl > 15 and tr > 15 and c < 0.8 and end < 2,
      f"the tail left {tl:.0f} right {tr:.0f}, correlation {c:.2f}, 2.6 s after {end:.1f}")
ch = rms(L, 13.5, 14.0)
check("DELAY into REVERB", ch > 10, f"a second after the tap {ch:.0f}")
sys.exit(1 if bad else 0)
