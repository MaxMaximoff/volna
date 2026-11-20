#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: check tests/scripts/lofi.txt in build/host/volna.wav (a 523 Hz sine, the left channel).
  dry: everything but the fundamental under -50 dB of it;
  LO-FI, the defaults: as loud as dry within 3 dB (rms); the pitch wobbles (the period, from interpolated zero
  crossings over 10 cycles, swings past 0.3 %; dry: under 0.05 %);
  WOBBLE 0: the fundamental within 3 dB of dry, the rest (steps, hiss) over -45 dB of it (and under -10);
  RATE 2.4 kHz: the hold's image at R - 523 Hz over -20 dB of the fundamental;
  BITS 2.8: the note holds (no 20 ms window under 0.3 of the median), within 6 dB of dry;
  after the last note (from 13.6 s): every sample 0, both sides.
Exit 1 if one is off. Run by tests/run_tests.sh after the script."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "build/host/volna.wav")
fs, n = w.getframerate(), w.getnframes()
d = struct.unpack("<%dh" % (2 * n), w.readframes(n))
L = d[0::2]
F = 523.25


def amp(f, t0, t1):
    a, b = int(t0 * fs), int(t1 * fs)
    re = sum(L[i] * math.cos(2 * math.pi * f * i / fs) for i in range(a, b))
    im = sum(L[i] * math.sin(2 * math.pi * f * i / fs) for i in range(a, b))
    return math.hypot(re, im) * 2 / (b - a)


def dirt(t0, t1):                               # the fundamental, and the rest against it (dB)
    a, b = int(t0 * fs), int(t1 * fs)
    re = sum(L[i] * math.cos(2 * math.pi * F * i / fs) for i in range(a, b))
    im = sum(L[i] * math.sin(2 * math.pi * F * i / fs) for i in range(a, b))
    a1, ph = math.hypot(re, im) * 2 / (b - a), math.atan2(im, re)
    e = sum((L[i] - a1 * math.cos(2 * math.pi * F * i / fs - ph)) ** 2 for i in range(a, b)) / (b - a)
    return a1, 10 * math.log10(e / (a1 * a1 / 2) + 1e-30)


def rms(t0, t1):
    s = L[int(t0 * fs):int(t1 * fs)]
    return math.sqrt(sum(x * x for x in s) / len(s))


def swing(t0, t1):                              # the pitch's swing: rising zero crossings (interpolated), the period
    a, b = int(t0 * fs), int(t1 * fs)           # over each 10 of them, (max - min) / mean
    zc = [i - 1 + L[i - 1] / (L[i - 1] - L[i]) for i in range(a + 1, b) if L[i - 1] < 0 <= L[i]]
    per = [(zc[k + 10] - zc[k]) / 10 for k in range(0, len(zc) - 10, 10)]
    return (max(per) - min(per)) / (sum(per) / len(per))


bad = 0


def check(what, ok, msg):
    global bad
    bad += not ok
    print(f"{what}: {msg} {'ok' if ok else 'FAIL'}")


a0, d0 = dirt(1.9, 2.4)
check("dry", d0 < -50, f"the rest at {d0:.1f} dB")
lv = 20 * math.log10(rms(3.5, 4.9) / rms(1.9, 2.4))
sw0, sw1 = swing(1.6, 2.4), swing(3.5, 4.9)
check("LO-FI, the defaults", abs(lv) < 3, f"{lv:+.1f} dB (rms) against dry")
check("LO-FI, the defaults", sw1 > 0.003 and sw0 < 0.0005, f"the pitch swings {100 * sw1:.2f} % (dry {100 * sw0:.3f} %)")
a1, d1 = dirt(6.0, 7.4)
check("WOBBLE 0", abs(20 * math.log10(a1 / a0)) < 3, f"the fundamental {20 * math.log10(a1 / a0):+.1f} dB against dry")
check("WOBBLE 0", -45 < d1 < -10, f"the rest at {d1:.1f} dB")
R = 44100 * 40 ** (-100 / 127)
im = 20 * math.log10(amp(R - F, 8.7, 9.4) / amp(F, 8.7, 9.4))
check("RATE 2.4 kHz", im > -20, f"the image at {R - F:.0f} Hz {im:.1f} dB")
win = int(0.02 * fs)
lv = sorted(rms(a / fs, (a + win) / fs) for a in range(int(11.3 * fs), int(12.4 * fs) - win, win))
med = lv[len(lv) // 2]
check("BITS 2.8", lv[0] > 0.3 * med, f"the quietest 20 ms window x{lv[0] / med:.2f} of the median (no drop-out)")
dl = 20 * math.log10(med / rms(1.9, 2.4))
check("BITS 2.8", abs(dl) < 6, f"{dl:+.1f} dB against dry")
tail = max(abs(x) for x in d[2 * int(13.6 * fs):])
check("after the last note", tail == 0, f"the loudest sample {tail}")
sys.exit(1 if bad else 0)
