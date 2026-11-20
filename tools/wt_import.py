#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A wavetable from a WAV file (Serum's format and its kin) made into Volna's shape: 64 waves of 128 samples.

The WAV: mono (or the first channel), PCM 16/24/32 bits or float 32, single cycles laid end to end. The cycle length
is the one in Serum's 'clm ' chunk ("<!>2048 ..."), or 2048 without one (Serum's, Vital's, Bitwig's exports).

What is done to it:
  * each cycle is band-limited to Volna's 63 harmonics by its own DFT (a cycle of 2048 samples has up to 1023;
    taking every 16th sample would fold them back as noise), DC dropped;
  * 256 frames (or any count) become 64 waves. Volna blends neighbour waves linearly (engine/wt_osc.h), so the
    waves are the frames that let that blend come closest to all the frames: a dynamic programme over the frames,
    each wave within +-SLACK frames of the even spacing, so the table's timing on the WAVE knob stays as it was and
    a sudden jump between two frames stays sudden (two waves on either side of it) instead of smeared over four;
  * the table is scaled to a common peak (gen_uwt.py: 30000) after the band limit.

Pure Python, no numpy (none on the build machines): an FFT for power-of-two cycles, a plain DFT otherwise.

    python3 tools/wt_import.py FILE.wav     (says what it would make: frames, the error of even vs chosen spacing)
"""
import cmath
import math
import struct
import sys
from pathlib import Path

WAVES, LEN, HMAX = 64, 128, 63
SLACK = 4               # frames a wave may move off the even spacing (256 frames: +-1 wave of the 64)
GAP = 12                # the most frames between two chosen ones


def read_wav(path):
    """(the samples as floats in -1..1, the cycle length)"""
    b = Path(path).read_bytes()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a WAV file")
    fmt = data = None
    cyc = 2048
    i = 12
    while i + 8 <= len(b):
        cid, n = b[i:i + 4], struct.unpack("<I", b[i + 4:i + 8])[0]
        body = b[i + 8:i + 8 + n]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            data = body
        elif cid == b"clm " and body.startswith(b"<!>"):
            cyc = int(body[3:].split()[0])
        i += 8 + n + (n & 1)
    if not fmt or data is None:
        raise SystemExit(f"{path}: no fmt or data chunk")
    tag, ch, _, _, align, bits = fmt
    if tag == 0xFFFE:                               # WAVE_FORMAT_EXTENSIBLE: the subformat's first two bytes
        tag = struct.unpack("<H", b[b.find(b"fmt ") + 8 + 24:b.find(b"fmt ") + 8 + 26])[0]
    width = bits // 8
    out = []
    for k in range(0, len(data) - align + 1, align):
        s = data[k:k + width]
        if tag == 3 and bits == 32:
            out.append(struct.unpack("<f", s)[0])
        elif tag == 1 and bits == 16:
            out.append(struct.unpack("<h", s)[0] / 32768.0)
        elif tag == 1 and bits == 24:
            out.append(int.from_bytes(s, "little", signed=True) / 8388608.0)
        elif tag == 1 and bits == 32:
            out.append(struct.unpack("<i", s)[0] / 2147483648.0)
        else:
            raise SystemExit(f"{path}: format {tag}, {bits} bits: not read (PCM 16/24/32 or float 32)")
    if len(out) < cyc or len(out) % cyc:
        raise SystemExit(f"{path}: {len(out)} samples are not whole cycles of {cyc}")
    return out, cyc


def _fft(x):
    n = len(x)
    if n == 1:
        return [complex(x[0])]
    e, o = _fft(x[0::2]), _fft(x[1::2])
    out = [0j] * n
    for k in range(n // 2):
        t = cmath.exp(-2j * math.pi * k / n) * o[k]
        out[k], out[k + n // 2] = e[k] + t, e[k] - t
    return out


def harmonics(x):
    """the complex harmonics 1..HMAX of one cycle (amplitude 1 for a full-scale sine)"""
    n = len(x)
    if n & (n - 1) == 0 and n >= 2 * HMAX + 2:
        X = _fft(x)
        return [X[k] * 2 / n for k in range(1, HMAX + 1)]
    tw = [turn(m, n) for m in range(n)]           # exp(-2 pi i m / n), the same numbers as the site's
    return [sum(x[i] * tw[k * i % n] for i in range(n)) * 2 / n for k in range(1, HMAX + 1)]


def turn(m, n):
    """exp(-2j pi m / n) by + - * / alone: the site's converter (web/src/fw/volna/wavetable.ts) gets the very same
    bits in the browser, which Math.sin and C's sin do not promise (they part in the last bit now and then, 2026-11-17).
    The quarter turn from the integers, then Taylor's series on [0, pi/2)"""
    m %= n
    q = 4 * m // n
    x = math.pi / 2 * (4 * m - q * n) / n
    x2 = x * x
    s, c, ts, tc = 0.0, 0.0, x, 1.0
    for k in range(1, 13):                        # x^25 / 25! < 1e-19 at pi/2
        s += ts
        c += tc
        ts = -ts * x2 / ((2 * k) * (2 * k + 1))
        tc = -tc * x2 / ((2 * k - 1) * (2 * k))
    cs, sn = ((c, s), (-s, c), (-c, -s), (s, -c))[q]   # (cos, sin) of q quarter turns + x
    return complex(cs, -sn)


def _sq(z):
    """|z|^2 without hypot (C's and the browser's differ in the last bit; the site must choose the same frames)"""
    return z.real * z.real + z.imag * z.imag


def choose(H, waves=WAVES, slack=SLACK, gap=GAP):
    """the frames to keep: 0 and the last among them, the linear blend between neighbours closest to every frame"""
    n = len(H)
    if n <= waves:
        return [round(i * (n - 1) / (waves - 1)) for i in range(waves)]

    def seg(a, b):
        e = 0.0
        for f in range(a + 1, b):
            t = (f - a) / (b - a)
            e += sum(_sq((1 - t) * u + t * v - w) for u, v, w in zip(H[a], H[b], H[f]))
        return e

    span = (n - 1) / (waves - 1)
    gap = max(gap, math.ceil(span) + 2 * slack)
    cost = {}
    INF = float("inf")
    best = {0: 0.0}
    back = []
    for k in range(1, waves):
        c = k * span
        nb, bk = {}, {}
        lo, hi = max(1, round(c) - slack), min(n - 1, round(c) + slack)
        if k == waves - 1:
            lo = hi = n - 1
        for b in range(lo, hi + 1):
            for a, v in best.items():
                if a >= b or b - a > gap:
                    continue
                if (a, b) not in cost:
                    cost[a, b] = seg(a, b)
                v += cost[a, b]
                if v < nb.get(b, INF):
                    nb[b], bk[b] = v, a
        if not nb:
            raise SystemExit("wt_import: no path through the frames (SLACK/GAP too tight)")
        best = nb
        back.append(bk)
    path = [n - 1]
    for bk in reversed(back):
        path.append(bk[path[-1]])
    return path[::-1]


def blend_error(H, path):
    """how far the linear blend between the chosen frames is from all the frames, in dB of the table's energy"""
    e = 0.0
    for a, b in zip(path, path[1:]):
        for f in range(a + 1, b):
            t = (f - a) / (b - a)
            e += sum(_sq((1 - t) * u + t * v - w) for u, v, w in zip(H[a], H[b], H[f]))
    tot = sum(abs(v) ** 2 for h in H for v in h)
    return 10 * math.log10(max(e, 1e-30) / tot)


def cycle(h, n=LEN):
    """n samples of one cycle from its harmonics 1..HMAX"""
    tw = [[cmath.exp(2j * math.pi * k * i / n) for i in range(n)] for k in range(1, HMAX + 1)]
    return [sum((c * w[i]).real for c, w in zip(h, tw)) for i in range(n)]


def load(path, peak):
    """64 waves of 128 ints within peak (scaled so the loudest sample is peak), and a report line"""
    x, cyc = read_wav(path)
    H = [harmonics(x[f * cyc:(f + 1) * cyc]) for f in range(len(x) // cyc)]
    path_ = choose(H)
    even = [round(i * (len(H) - 1) / (WAVES - 1)) for i in range(WAVES)]
    waves = [cycle(H[f]) for f in path_]
    pk = max(abs(v) for w in waves for v in w) or 1.0
    kept = sum(abs(v) ** 2 for h in H for v in h)
    full = sum(sum(v * v for v in x[f * cyc:(f + 1) * cyc]) * 2 / cyc for f in range(len(H)))
    note = (f"{len(H)} frames of {cyc} -> 64 x 128; blend error {blend_error(H, even):.1f} dB evenly spaced, "
            f"{blend_error(H, path_):.1f} dB chosen; {100 * kept / max(full, 1e-30):.1f}% of the energy below "
            f"harmonic 64")
    return [[int(round(v * peak / pk)) for v in w] for w in waves], note, path_


if __name__ == "__main__":
    for p in sys.argv[1:]:
        _, note, frames = load(p, 30000)
        print(f"{Path(p).name}: {note}\n  frames: {frames}")
