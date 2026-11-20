# SPDX-License-Identifier: GPL-3.0-only
"""Volna: how much of a periodic tone lies off its harmonics (what folds back from past fs / 2 lands there), in dB.
A Hann window over n samples (a power of 2), the spectrum by an FFT (pure Python); the energy in the bins more than
4 away from every harmonic of f0 (and from DC), over all the energy but DC. Hann's leakage 4 bins out is under -90 dB,
so the floor is far below what matters. Used by tests/sync_check.py."""
import cmath
import math


def fft(a):                                     # iterative radix 2, len(a) a power of 2
    n = len(a)
    a = list(a)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    m = 2
    while m <= n:
        w = cmath.exp(-2j * math.pi / m)
        for k in range(0, n, m):
            wk = 1
            for t in range(m // 2):
                u, v = a[k + t], a[k + t + m // 2] * wk
                a[k + t], a[k + t + m // 2] = u + v, u - v
                wk *= w
        m *= 2
    return a


def off_db(x, fs, f0, n=8192):
    x = x[:n]
    m = sum(x) / len(x)
    w = [(v - m) * (0.5 - 0.5 * math.cos(2 * math.pi * i / n)) for i, v in enumerate(x)]
    p = [abs(c) ** 2 for c in fft(w)[:n // 2]]
    bw = fs / n
    tot = sum(p[5:])
    off = sum(v for k, v in enumerate(p) if k > 4 and min(abs(k * bw - f0 * h) for h in range(1, int(fs / 2 / f0) + 2)) > 4 * bw)
    return 10 * math.log10(max(off, 1e-30) / max(tot, 1e-30))


def psd(x, n=4096):                             # the power a bin, averaged over frames of n (half overlapped), Hann
    w = [0.5 - 0.5 * math.cos(2 * math.pi * i / n) for i in range(n)]
    acc = [0.0] * (n // 2)
    frames = 0
    for s in range(0, len(x) - n + 1, n // 2):
        f = fft([x[s + i] * w[i] for i in range(n)])
        for k in range(n // 2):
            acc[k] += abs(f[k]) ** 2
        frames += 1
    return [v / max(frames, 1) for v in acc]
