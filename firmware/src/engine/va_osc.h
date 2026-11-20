/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna engine: the ANALOG "table" (2026-11-14): a sawtooth and a pulse computed a sample at a time, not read from a
 * table, so a low note keeps all its harmonics (a table's wave has 128 samples: 63 harmonics, 3.5 kHz at 55 Hz).
 *
 * Its 64 "waves", as a table's: 0 a sawtooth, crossfading to a square at 32, then the pulse narrowing to 6 % at 63
 * (an LFO on WAVE: PWM). The edges are band-limited with polyBLEP (V. Valimaki, A. Huovilainen, "Antialiasing
 * oscillators in subtractive synthesis", IEEE Signal Processing Magazine 24(2), 2007): within a step of a jump, a
 * polynomial residue -(1 - x)^2 before and (1 - x)^2 after it, x the distance from the jump in steps. What still
 * aliases is far quieter than a naive edge; above ~5 kHz more of it is heard (a table's mip-map is cleaner there).
 *
 * The pulse has no DC (its mean taken off: (2w - 1)) and is scaled by 1 - |2w - 1| / 2, so its peak stays within
 * 1.125 of the sawtooth's. Out: about +-VA_PEAK, as a table's wave (30000).
 * Integers: the phase 32 bits a cycle, the step dt; inv = 2^46 / dt (va_inv: one 32-bit division a sub-block), the
 * distance x = d inv >> 32 (Q14) by a 64-bit multiply. */
#ifndef VOLNA_VA_OSC_H
#define VOLNA_VA_OSC_H
#include <stdint.h>

#define VA_PEAK 30000

static inline uint32_t va_inv(uint32_t dt)      /* ~2^46 / dt */
{
    return 0xFFFFFFFFu / ((dt >> 14) + 1u);
}
/* the polyBLEP residue at phase t (Q14): -(1 - t/dt)^2 just after the jump (t < dt), (1 - (1 - t)/dt)^2 just
 * before it, 0 elsewhere */
static inline int32_t va_blep(uint32_t t, uint32_t dt, uint32_t inv)
{
    uint32_t d = t < dt ? t : 0u - t;           /* (0 - t: the distance to the next jump) */
    int32_t x, y;
    if (d >= dt) return 0;
    x = (int32_t)(((uint64_t)d * inv) >> 32);
    y = 16384 - (x > 16384 ? 16384 : x);
    y = (y * y) >> 14;
    return t < dt ? -y : y;
}
/* a sample at phase t, step dt (va_inv: inv), position pos (Q16, 0 .. 63 waves) */
static inline int32_t va_core(uint32_t t, uint32_t dt, uint32_t inv, uint32_t pos)
{
    int32_t b0 = va_blep(t, dt, inv), saw = (int32_t)(t >> 17) - 16384 - b0, y;   /* Q14 */
    if (pos == 0) y = saw;
    else {
        uint32_t w = 0x80000000u;               /* the pulse's width: 1/2 up to wave 32, then narrower */
        int32_t m = pos >= (32u << 16) ? 32768 : (int32_t)(pos >> 6), pul, a;   /* the crossfade, Q15 */
        if (pos > (32u << 16)) w -= (pos - (32u << 16)) * 930u;   /* (to 0.06 at 63: 930 = 0.44 x 2^32 / 31 / 2^16) */
        a = 16384 - (int32_t)(w >> 17);         /* |2w - 1|, Q14 (w <= 1/2) */
        pul = (t < w ? 16384 : -16384) + b0 - va_blep(t - w, dt, inv) + a;   /* (a: the DC off) */
        pul = (pul * (16384 - (a >> 1))) >> 14;
        y = saw + (((pul - saw) * m) >> 15);
    }
    return (y * (VA_PEAK >> 1)) >> 13;          /* Q14 -> +-30000 */
}
/* the naive shape (no polyBLEP), for the screen: phase t, position pos */
static inline int32_t va_shape(uint32_t t, uint32_t pos)
{
    return va_core(t, 0u, 0u, pos);
}
#endif
