/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna engine: the EQ (2026-11-15), the effects' third slot, fixed (no type to choose): after slots 1 and 2, before
 * the output's limiter. Three bands, each a zero-delay-feedback SVF as in engine/svf.h (A. Simper, Cytomic,
 * "Solving the continuous SVF equations using trapezoidal integration", 2013), its output a mix of the input v0,
 * the band v1 and the low pass v2:  out = m0 v0 + m1 v1 + m2 v2.
 *   LOW (a shelf):   g = tan / sqrt(A), k = sqrt 2,    m0 = 1,   m1 = k (A - 1),    m2 = A^2 - 1
 *   MID (a bell):    g = tan,           k = 1 / (Q A), m0 = 1,   m1 = k (A^2 - 1),  m2 = 0
 *   HIGH (a shelf):  g = tan x sqrt(A), k = sqrt 2,    m0 = A^2, m1 = k (1 - A) A,  m2 = 1 - A^2
 * with A = 10^(dB / 40), tan = tan(pi f / fs): the analog shelves and peak of R. Bristow-Johnson's "Audio EQ
 * Cookbook", bilinear (a shelf is at half its gain at its frequency; the bell at its gain, its width Q). The shelves'
 * Q is fixed at 0.707 (no bump). A band at 0 dB is off: the ISR skips it and clears its state; all three at 0 dB, the
 * EQ costs nothing.
 *
 * Integers, as svf.h: g Q28, a1 a2 Q30, m0 m1 m2 Q14, the signal SVF_SH bits up inside, products int64 (multiplies
 * only). eq_band_coef() is the main loop's (one 32-bit division, in svf_a1); the ISR takes a whole set at a block's
 * start (volna.c: a sequence count, odd while the main loop writes). The gains are -12 .. +12 dB, a dB a step
 * (tools/gen_eq.py); the frequencies are notes (svf.h's table: up to SVF_NMAX). */
#ifndef VOLNA_EQ_H
#define VOLNA_EQ_H
#include <stdint.h>
#include "svf.h"
#include "eq_tables.h"

enum { EQ_LOW, EQ_MID, EQ_HIGH, EQ_NB };
#define EQ_KSH 23170                            /* the shelves' k: sqrt 2 (Q 0.707), Q14 */
#define EQ_IN_MAX 262143                        /* the input's clamp (8 x full scale: the slots' sum can be loud) */
#define EQ_IN_CLIP (1 << 26)                    /* between the bands, inside (+12 dB three times over stays under) */

typedef struct { int32_t g, a1, a2, m0, m1, m2; } eq_band_t;   /* g Q28, a1 a2 Q30, m0 m1 m2 Q14 */
typedef struct { eq_band_t b[EQ_NB]; int32_t on; } eq_coef_t;  /* on: a bit a band (not at 0 dB) */
typedef struct { int32_t ic[EQ_NB][2][4]; } eq_state_t;        /* a band, a side: its two integrators, the two
                                                   shifts' remainders (eq_tick) */
#define EQ_WORDS ((uint32_t)(sizeof(eq_coef_t) / 4u))

/* band b's coefficients: at the note n (Q8), db (-12 .. 12), the bell's res (svf.h's: Q = 0.5 x 50^(res / 127)) */
static void eq_band_coef(eq_band_t *c, uint32_t b, int32_t n, int32_t db, uint32_t res)
{
    int32_t d = db < -EQ_DBMAX ? 0 : db > EQ_DBMAX ? 2 * EQ_DBMAX : db + EQ_DBMAX, nd = 2 * EQ_DBMAX - d;   /* (nd: -dB) */
    int32_t a2 = EQ_A2[d], a = EQ_A[d], g = svf_g(n), k;
    if (b == EQ_LOW) {
        g = (int32_t)(((int64_t)g * EQ_SA[nd]) >> 14);   /* tan / sqrt(A) */
        k = EQ_KSH;
        c->m0 = 1 << 14;
        c->m1 = (k * (a - (1 << 14))) >> 14;
        c->m2 = a2 - (1 << 14);
    } else if (b == EQ_HIGH) {
        g = (int32_t)(((int64_t)g * EQ_SA[d]) >> 14);    /* tan x sqrt(A) */
        k = EQ_KSH;
        c->m0 = a2;
        c->m1 = (int32_t)(((int64_t)((k * ((1 << 14) - a)) >> 14) * a) >> 14);
        c->m2 = (1 << 14) - a2;
    } else {
        k = (int32_t)(((int64_t)SVF_K[res & 127u] * EQ_A[nd]) >> 14);   /* 1 / (Q A): 1 / A is A at -dB */
        c->m0 = 1 << 14;
        c->m1 = (int32_t)(((int64_t)k * (a2 - (1 << 14))) >> 14);
        c->m2 = 0;
    }
    c->g = g;
    c->a1 = svf_a1(g, k);                       /* (1 + g (g + k) < 16 over the bands' ranges: volna.c EQ_N*) */
    c->a2 = (int32_t)(((int64_t)g * c->a1) >> 28);
}

/* one band, one sample (inside: SVF_SH bits up). The two shifts keep what they cut off and add it to the next
 * sample's (error feedback): at a low frequency g is tiny (32 Hz: 0.0017), g v1 under one step would be cut to
 * nothing, and the low pass would stick at an offset after the sound stops (up to ~180 at the output, past the
 * silence test's 8). Kept, the integrators are right on average, and they settle at 0 */
static inline __attribute__((always_inline)) int32_t eq_tick(int32_t *ic, const eq_band_t *c, int32_t v0)
{
    int64_t p = (int64_t)c->a1 * ic[0] + (int64_t)c->a2 * (v0 - ic[1]) + ic[2];
    int32_t v1 = (int32_t)(p >> 30), v2, y;
    ic[2] = (int32_t)(p & 0x3FFFFFFF);
    p = (int64_t)c->g * v1 + ic[3];
    v2 = ic[1] + (int32_t)(p >> 28);
    ic[3] = (int32_t)(p & 0x0FFFFFFF);
    ic[0] = 2 * v1 - ic[0];
    ic[1] = 2 * v2 - ic[1];
    y = (int32_t)(((int64_t)c->m0 * v0 + (int64_t)c->m1 * v1 + (int64_t)c->m2 * v2) >> 14);
    return y > EQ_IN_CLIP ? EQ_IN_CLIP : y < -EQ_IN_CLIP ? -EQ_IN_CLIP : y;
}

/* the EQ over n frames of out (L R interleaved), in place. Out of line: the audio ISR's own loops stay as they were
 * (tools/isr_loops.py), and without a band on it returns at once */
static __attribute__((noinline)) void eq_run(eq_state_t *s, const eq_coef_t *c, int32_t *out, uint32_t n)
{
    uint32_t i, b, on = (uint32_t)c->on;
    for (b = 0; b < EQ_NB; b++)
        if (!((on >> b) & 1u)) {
            int32_t *z = &s->ic[b][0][0];
            uint32_t j;
            for (j = 0; j < 8u; j++) z[j] = 0;
        }
    if (!on) return;
    for (i = 0; i < 2u * n; i++) {
        uint32_t ch = i & 1u;
        int32_t x = out[i];
        x = x > EQ_IN_MAX ? EQ_IN_MAX : x < -EQ_IN_MAX ? -EQ_IN_MAX : x;
        x *= 1 << SVF_SH;
        if (on & 1u) x = eq_tick(s->ic[EQ_LOW][ch], &c->b[EQ_LOW], x);
        if (on & 2u) x = eq_tick(s->ic[EQ_MID][ch], &c->b[EQ_MID], x);
        if (on & 4u) x = eq_tick(s->ic[EQ_HIGH][ch], &c->b[EQ_HIGH], x);
        out[i] = (x + (1 << (SVF_SH - 1))) >> SVF_SH;   /* (rounded: no -1 left over from the floor) */
    }
}

/* ---- for the screen (main loop): a band's analog response in dB, Q8, at ou / 264 octaves from its frequency
 * (within +-5 octaves; beyond, as at 5). With x^2 = (f / f0)^2 (xx, Q16):
 *   bell:        |H|^2 = ((1 - xx)^2 + (A / Q)^2 xx) / ((1 - xx)^2 + (1 / (A Q))^2 xx)
 *   low shelf:   |H|^2 = A^2 ((A - xx)^2 + 2 A xx) / ((1 - A xx)^2 + 2 A xx)     (Q 0.707: 1 / Q^2 = 2)
 *   high shelf:  |H|^2 = A^2 ((1 - A xx)^2 + 2 A xx) / ((A - xx)^2 + 2 A xx) */
static int32_t eq_log2_q8(uint64_t v)           /* log2(v) x 256, v >= 1 (svf.h's, past 32 bits) */
{
    int32_t sh = 0;
    if (!v) return -(32 << 8);
    while (v >> 32) { v >>= 1; sh++; }
    return svf_log2_q8((uint32_t)v) + (sh << 8);
}
static int32_t eq_db_q8(uint32_t b, int32_t db, uint32_t res, int32_t ou)
{
    int32_t d, x;
    int64_t xx, A, num, den, t;
    if (!db) return 0;
    d = db < -EQ_DBMAX ? 0 : db > EQ_DBMAX ? 2 * EQ_DBMAX : db + EQ_DBMAX;
    if (ou > 1320) ou = 1320;
    if (ou < -1320) ou = -1320;
    x = svf_exp2_q12(ou);                       /* Q12, 1/32 .. 32 */
    xx = ((int64_t)x * x) >> 8;                 /* Q16 */
    A = (int64_t)EQ_A[d] << 2;                  /* Q16 */
    if (b == EQ_MID) {
        int64_t kq = (int64_t)SVF_K[res & 127u] << 2, up = (A * kq) >> 16, dn = (((int64_t)EQ_A[2 * EQ_DBMAX - d] << 2) * kq) >> 16;
        t = (65536 - xx) * (65536 - xx);        /* Q32 */
        num = t + ((up * up) >> 16) * xx;
        den = t + ((dn * dn) >> 16) * xx;
        return ((eq_log2_q8((uint64_t)num) - eq_log2_q8((uint64_t)den)) * 771) >> 8;   /* 10 log10 = 3.0103 log2 */
    }
    t = 2 * A * xx;                             /* Q32 */
    num = (A - xx) * (A - xx) + t;
    den = (65536 - ((A * xx) >> 16)) * (65536 - ((A * xx) >> 16)) + t;
    if (b == EQ_HIGH) { int64_t s = num; num = den; den = s; }
    return db * 128 + (((eq_log2_q8((uint64_t)num) - eq_log2_q8((uint64_t)den)) * 771) >> 8);   /* (20 log10 A: dB / 2) */
}
#endif
