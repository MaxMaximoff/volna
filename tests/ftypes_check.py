#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna (2026-11-16): the filter types after the Blofeld's, heard on white noise. MIX: the oscillators off, NOISE up;
filter 1 at CUTOFF 52 (note 56, 207.65 Hz), its ENV and KEYTRK 0; a note held 1.5 s; the spectrum averaged (Hann,
4096-sample frames, tests/offharm.py):
  COMB+   RESO 127: the energy near f and its multiples (within 4 bins) over 10 x as dense as between them;
  COMB-   RESO 127: so by the odd multiples of f / 2, and under 0.2 x as dense by f's;
  NOTCH   NOTCH24 against NOTCH12: a quarter octave either side of f lower (the notch wider);
  PPG     PPG LP against LP24 at RESO 100: the peak at f over the pass band (an octave below) higher.
Runs the host build of firmware/src/volna.c (tests/run_host.sh builds it first). Exit 1 if one is off.
    python3 tests/ftypes_check.py [build/host/volna]"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from offharm import psd  # noqa: E402

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "build/host/volna"
OUT = "build/host/ftypes"
F = 440.0 * 2 ** ((56 - 69) / 12)
N = 4096
BASE = ["900 btn 3", "1000 enc 2 -40", "1100 enc 3 -40", "1200 enc 4 -40", "1300 enc 5 40",   # MIX: noise alone
        "1500 btn 4", "1600 enc 2 -12", "1800 enc 4 -24", "2000 enc 5 -10"]                   # FILT: 52, ENV 0, KEY 0
TYPE = {"LP24": -1, "NOTCH24": 5, "NOTCH12": 6, "COMB+": 7, "COMB-": 8, "PPG": 9}           # (from LP12, row B)


def spectrum(name, ftype, res):
    lines = BASE + [f"2200 enc 3 {res}", "2500 btn 4", f"2600 enc 2 {TYPE[ftype]}",
                    "3000 midi 0x90 57 100", "4500 midi 0x80 57 0", "5000 end"]
    sp = Path(f"{OUT}-{name}.txt")
    sp.write_text("\n".join(lines) + "\n")
    subprocess.run([HOST, f"{OUT}-{name}", str(sp)], check=True, stdout=subprocess.DEVNULL)
    w = wave.open(f"{OUT}-{name}.wav")
    fs, n = w.getframerate(), w.getnframes()
    x = struct.unpack("<%dh" % (2 * n), w.readframes(n))[0::2]
    return fs, psd(list(x[int(3.3 * fs):int(4.4 * fs)]), N)


def share(p, fs, lines):                        # the energy within 4 bins of the lines, over all (from 50 Hz)
    bw = fs / N
    near = [k for k in range(len(p)) if k * bw > 50 and min(abs(k * bw - f) for f in lines) <= 4 * bw]
    tot = sum(p[k] for k in range(len(p)) if k * bw > 50)
    return sum(p[k] for k in near) / max(tot, 1e-30), len(near) / sum(1 for k in range(len(p)) if k * bw > 50)


def dens(s, w):                                 # how much denser the energy is by the lines than between them
    return (s / max(w, 1e-9)) / max((1 - s) / max(1 - w, 1e-9), 1e-9)


def band(p, fs, f0, f1):
    bw = fs / N
    return sum(p[k] for k in range(len(p)) if f0 <= k * bw <= f1)


bad = 0


def report(name, ok, text):
    global bad
    bad += not ok
    print(f"{name:6s}: {text} {'ok' if ok else 'FAIL'}")


fs, p = spectrum("combp", "COMB+", 52)
s, white = share(p, fs, [F * k for k in range(1, int(20000 / F))])
d = dens(s, white)
report("COMB+", d > 10, f"by {F:.1f} Hz and its multiples {100 * s:.0f} % of the energy on {100 * white:.0f} % of the bins: x{d:.0f} as dense (want > 10)")
fs, p = spectrum("combn", "COMB-", 52)
s, white = share(p, fs, [F / 2 * k for k in range(1, int(20000 / F * 2), 2)])
s2, w2 = share(p, fs, [F * k for k in range(1, int(20000 / F))])
d, d2 = dens(s, white), dens(s2, w2)
report("COMB-", d > 10 and d2 < 0.2, f"by the odd multiples of {F / 2:.1f} Hz x{d:.0f} as dense (want > 10), by {F:.1f} Hz's x{d2:.2f} (want < 0.2)")
fs, p24 = spectrum("n24", "NOTCH24", 0)
fs, p12 = spectrum("n12", "NOTCH12", 0)
q = 2 ** 0.25
r = band(p24, fs, F / q, F * q) / max(band(p12, fs, F / q, F * q), 1e-30)
report("NOTCH", r < 0.7, f"a quarter octave around {F:.0f} Hz: NOTCH24 x{r:.2f} of NOTCH12's (want < 0.7)")
fs, pp = spectrum("ppg", "PPG", 38)
fs, pl = spectrum("lp24", "LP24", 38)
pk = lambda p: band(p, fs, F / 2 ** (1 / 6), F * 2 ** (1 / 6)) / max(band(p, fs, F / 2 ** 1.5, F / 2 ** 0.5), 1e-30)
report("PPG", pk(pp) > 1.5 * pk(pl), f"RESO 100: the peak over the pass band, PPG LP x{pk(pp):.1f}, LP24 x{pk(pl):.1f} (want PPG > 1.5 x LP24)")
sys.exit(1 if bad else 0)
