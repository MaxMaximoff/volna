#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna's wavetable mip-maps (stage 8), shared by tools/gen_tables.py (Volna's own tables) and tools/gen_uwt.py
(the local ones). The layout must match engine/wt_osc.h (WT_LV_LEN, WT_LV_OFS, WT_WORDS):

  level 0: 128 samples, the wave as it is (up to 63 harmonics)
  level 1: 128 samples, up to 31 harmonics      (4 samples a cycle of the top one, at least: the linear
  level 2:  64 samples, up to 15                 interpolation's images stay low)
  level 3:  32 samples, up to 7
  level 4:  16 samples, up to 3
  level 5:  16 samples, the fundamental alone

Every wave of a level is a row of its samples plus one guard (= sample 0); a table is the 64 waves of level 0, then
of level 1, ... (24960 int16, ~49 KB). A level above 0 is the wave's own harmonics (a DFT of level 0), cut off and
drawn again at the level's length; one that would peak over PEAK is scaled down to it (wt_sample's products stay
under 2^31). Pure Python (no numpy on the build machines)."""
import math

PEAK = 30000
WAVES = 64
LEVELS = [(128, 63), (128, 31), (64, 15), (32, 7), (16, 3), (16, 1)]   # (samples, the top harmonic)
WORDS = sum(WAVES * (n + 1) for n, _ in LEVELS)                        # 24960

_TW = {}


def _twiddles(n, hmax):
    key = (n, hmax)
    if key not in _TW:
        _TW[key] = [([math.cos(2 * math.pi * k * i / n) for i in range(n)],
                     [math.sin(2 * math.pi * k * i / n) for i in range(n)]) for k in range(hmax + 1)]
    return _TW[key]


def spectrum(x, hmax):
    """the harmonics 1..hmax of one cycle x (any length): a list of (cos, sin) amplitudes"""
    n = len(x)
    tw = _twiddles(n, hmax)
    return [(2.0 / n * sum(a * b for a, b in zip(x, tw[k][0])), 2.0 / n * sum(a * b for a, b in zip(x, tw[k][1])))
            for k in range(1, hmax + 1)]


def synth(spec, n, hmax):
    """one cycle of n samples from the harmonics 1..hmax of spec"""
    tw = _twiddles(n, hmax)
    out = [0.0] * n
    for k in range(1, min(hmax, len(spec)) + 1):
        c, s = spec[k - 1]
        if c == 0.0 and s == 0.0:
            continue
        cs, sn = tw[k]
        for i in range(n):
            out[i] += c * cs[i] + s * sn[i]
    return out


def rows(l0):
    """one wave's rows for every level: l0 its 128 samples (floats or ints, within PEAK); a list of int lists, each
    with its guard"""
    spec = spectrum(l0, 63)
    out = []
    for lv, (n, h) in enumerate(LEVELS):
        x = [float(v) for v in l0] if lv == 0 else synth(spec, n, h)
        pk = max(abs(v) for v in x)
        g = PEAK / pk if pk > PEAK else 1.0
        r = [int(round(v * g)) for v in x]
        r.append(r[0])
        out.append(r)
    return out


def table_c(waves):
    """a table (64 waves of 128 samples) as the body of a C initialiser: level by level, a row a line"""
    per = [rows(w) for w in waves]
    lines = []
    for lv in range(len(LEVELS)):
        lines.append(f"    /* level {lv}: {LEVELS[lv][0]} samples, up to {LEVELS[lv][1]} harmonics */")
        for w in range(WAVES):
            lines.append("    " + ",".join(str(v) for v in per[w][lv]) + ",")
    return "\n".join(lines)
