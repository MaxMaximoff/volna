/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna engine: the state-variable filter (stage 3), LP / BP / HP / notch; and LP / BP / HP at 24 dB an octave.
 *
 * The zero-delay-feedback ("TPT") SVF of Andrew Simper (Cytomic, "Solving the continuous SVF equations using
 * trapezoidal integration", 2013): stable at any cutoff and resonance, and while they move, unlike the classic
 * Chamberlin SVF. Per sample (v0 the input, ic1 ic2 the two integrators' states):
 *   v1 = a1 ic1 + a2 (v0 - ic2)    the band pass (its peak 1 / k)
 *   v2 = ic2 + g v1                the low pass
 *   ic1 = 2 v1 - ic1, ic2 = 2 v2 - ic2
 *   high = v0 - k v1 - v2, notch = v0 - k v1, the band normalised to 0 dB at its peak = k v1
 * with g = tan(pi fc / fs), k = 1 / Q, a1 = 1 / (1 + g (g + k)), a2 = g a1.
 *
 * Integers: the coefficients are Q30 (g Q28), the products int64 (multiplies only). svf_coef() runs once a block
 * of ENV_SUB samples a voice: one 32-bit division for 1 / (1 + g (g + k)) to 16 bits, then a Newton step to 30.
 * The signal inside has SVF_SH bits more than the samples (the low cutoffs' integrators keep their precision);
 * the input is up to +-SVF_CLIP, the output is clamped there (a resonance peak is at most about +14 dB, see
 * tools/gen_svf.py: SVF_COMP).
 * The cutoff is a note in Q8 (256 = a semitone, MIDI numbers: 69 << 8 = 440 Hz), 0 .. SVF_NMAX (16.7 kHz).
 *
 * The 24 dB types (LP24, BP24, HP24; 2026-11-14): two such filters in a row at the same cutoff, as on the Blofeld.
 * The first (the "pre" stage) at a fixed Q 0.541, the lower Q of a 4th-order Butterworth's two, no peak of its own;
 * the second at the resonance, as the 12 dB type. Together: the slope 24 dB an octave (BP24: 12 each side), the
 * resonance peak 5.3 dB lower than the 12 dB type's (the first stage is at Q there). Twice the work a sample.
 *
 * The resonance driven (2026-11-14): with the filter's DRIVE on (nl 1, set by the caller), the band pass v1 (the
 * resonance's own state: Q times the input at the cutoff) goes through a soft knee before it feeds the integrators:
 * as it is up to SVF_NLK (full scale), then a cubic bend to SVF_NLK + 2/3 SVF_NLW (2.3 x full scale), flat beyond.
 * As in an analog filter, a loud input no longer rings Q times louder: the peak squashes and growls; a quiet one
 * and a low resonance are as before. DRIVE 0: the filter is linear, as it was, to the bit.
 *
 * More of the Blofeld's types (2026-11-16): NOTCH24 (two notch stages, as the other 24 dB types); PPG LP (a 24 dB low
 * pass in the spirit of the PPG Wave's: its first stage resonant too, at half the resonance, no level kept up: a
 * sharper, louder peak than LP24's; an approximation, not the PPG's circuit); COMB+ and COMB- (a delay line of the
 * cutoff's period fed back: y = g x +- fb y[n - D], D = fs / f, fb up to 0.98 by the resonance, g = sqrt(1 - fb):
 * the peaks (COMB+: at f and its multiples; COMB-: at the odd multiples of f / 2, an octave down, hollow) +17 dB at
 * the top). The comb's sample is not svf_tick's: volna.c runs it (comb_tick) in its own loop, with a line of COMB_N
 * samples a voice, a filter and a side; here only its coefficients and its picture. */
#ifndef VOLNA_SVF_H
#define VOLNA_SVF_H
#include <stdint.h>
#include "svf_tables.h"

#define SVF_SH 4
#define SVF_NMAX (132 << 8)                     /* 16.7 kHz: 1 + g (g + k) < 16 keeps the Q27 sum in 32 bits */
#define SVF_CLIP 131071                         /* 4x a full-scale sample: the output's ceiling */

enum { SVF_OFF, SVF_LP, SVF_BP, SVF_HP, SVF_NOTCH, SVF_LP24, SVF_BP24, SVF_HP24, SVF_NOTCH24, SVF_PPG, SVF_COMBP,
       SVF_COMBN, SVF_NTYPE };                  /* (new types at the end: the patches keep their meaning) */
#define SVF_K24 30274                           /* the 24 dB types' first stage: k = 1 / 0.5412, Q14 */
#define SVF_IS24(t) (((t) >= SVF_LP24 && (t) <= SVF_HP24) || (t) == SVF_NOTCH24 || (t) == SVF_PPG)
#define SVF_BASE(t) ((t) >= SVF_LP24 && (t) <= SVF_HP24 ? (t) - SVF_LP24 + SVF_LP : (t) == SVF_NOTCH24 ? SVF_NOTCH \
                     : (t) == SVF_PPG ? SVF_LP : (t))   /* LP24 -> LP and so on */
#define SVF_ISCOMB(t) ((t) == SVF_COMBP || (t) == SVF_COMBN)
#define COMB_N 1024u                            /* a comb's line: down to fs / 1022, 43 Hz */

typedef struct { int32_t ic1, ic2, ic3, ic4; } svf_state_t;     /* ic3 ic4: the 24 dB types' first stage */
typedef struct {
    int32_t g, a1, a2, cx, cl, cb;              /* g Q28, a1 a2 Q30; the output's mix, Q14 */
    int32_t p4, pa1, pa2, px, pl, pb;           /* a 24 dB type: its first stage (p4 1), the same, k SVF_K24 */
    int32_t nl;                                 /* 1: the resonance driven (svf_coef: 0; the caller sets it) */
    int32_t k, kp;                              /* the damping, its first stage's (svf_retune: Filter FM) */
} svf_coef_t;
#define SVF_NLK (1 << 19)                       /* the knee: a full-scale sample, inside (<< SVF_SH) */
#define SVF_NLW (1 << 20)                       /* the bend's width: flat from NLK + NLW at NLK + 2/3 NLW */

/* 1 / (1 + g (g + k)), Q30: one 32-bit division to 16 bits, a Newton step to 30 */
static inline int32_t svf_a1(int32_t g, int32_t k)
{
    int32_t d = (1 << 27) + (int32_t)(((int64_t)g * (g + (k << 14))) >> 29);   /* 1 + g (g + k), Q27, < 16 */
    int32_t a = (int32_t)(0xFFFFFFFFu / ((uint32_t)d >> 11)) << 14;          /* 1 / d, Q30, good to 16 bits */
    int32_t e = (1 << 30) - (int32_t)(((int64_t)d * a) >> 27);               /* Newton: 1 - d a */
    return a + (int32_t)(((int64_t)a * e) >> 30);                           /* a (1 + (1 - d a)): 30 bits */
}

/* g = tan(pi f / fs), Q28, for the note n (Q8, clamped to 0 .. SVF_NMAX) */
static inline int32_t svf_g(int32_t n)
{
    int32_t i, f;
    if (n < 0) n = 0;
    if (n > SVF_NMAX) n = SVF_NMAX;
    i = n >> 7;                                 /* the table: every half semitone */
    f = n & 127;
    return (int32_t)SVF_G[i] + (int32_t)(((int64_t)((int32_t)SVF_G[i + 1] - (int32_t)SVF_G[i]) * f) >> 7);
}
/* a comb's coefficients (out of line: only a comb pays the call), in the fields svf_tick does not need for it: cx its
 * delay D (Q16 samples, fs / the cutoff's frequency, 2 .. COMB_N - 2), cl fb (Q15), cb g (Q15), a1 its sign (1 COMB+,
 * -1 COMB-). D = 5393.97 x 2^(-n / 12): the semitones' table, linear within one (within 0.7 cent) */
static uint32_t svf_comb_delay(int32_t n)       /* a comb's delay for the note n (Q8), Q16 samples, 2 .. COMB_N - 2 */
{
    static const uint16_t SEMI_INV[13] = {65535, 61858, 58386, 55109, 52016, 49097, 46341, 43740, 41285, 38968, 36781,
                                          34716, 32768};   /* 2^(-i / 12), Q16 (the first 65535: 16 bits) */
    uint32_t o, r, m, d;
    if (n < 0) n = 0;
    if (n > SVF_NMAX) n = SVF_NMAX;
    o = (uint32_t)n / 3072u;
    r = (uint32_t)n % 3072u;
    m = SEMI_INV[r >> 8] - (((uint32_t)(SEMI_INV[r >> 8] - SEMI_INV[(r >> 8) + 1u]) * (r & 255u)) >> 8);
    d = (uint32_t)(((uint64_t)353499105u * m) >> 16) >> o;   /* (5393.97 Q16: the delay of note 0, 8.18 Hz) */
    if (d < 2u << 16) d = 2u << 16;
    if (d > (COMB_N - 2u) << 16) d = (COMB_N - 2u) << 16;
    return d;
}
static __attribute__((noinline)) void svf_comb_coef(svf_coef_t *c, int32_t n, uint32_t res, uint32_t type)
{
    uint32_t m, d = svf_comb_delay(n), fb, x, b;
    fb = (res & 127u) * 32112u / 127u;          /* 0 .. 0.98 */
    x = (32768u - fb) << 15;                    /* g = sqrt(1 - fb), Q15: bit by bit */
    for (m = 0, b = 1u << 15; b; b >>= 1) if ((m + b) * (m + b) <= x) m += b;
    c->cx = (int32_t)d; c->cl = (int32_t)fb; c->cb = (int32_t)m; c->a1 = type == SVF_COMBN ? -1 : 1;
    c->p4 = 0; c->nl = 0;
}
/* the coefficients for the cutoff n (a note, Q8), the resonance res (0..127) and the type */
static inline void svf_coef(svf_coef_t *c, int32_t n, uint32_t res, uint32_t type)
{
    int32_t k = SVF_K[res & 127u], cp = SVF_COMP[res & 127u] >> 1, kp;
    if (SVF_ISCOMB(type)) { svf_comb_coef(c, n, res, type); return; }
    c->g = svf_g(n);
    c->a1 = svf_a1(c->g, k);
    c->a2 = (int32_t)(((int64_t)c->g * c->a1) >> 28);
    c->p4 = SVF_IS24(type);
    c->nl = 0;
    c->k = k;
    if (c->p4) {                                /* the first stage: no resonance (PPG: half of it), no level
                                                   compensation */
        kp = type == SVF_PPG ? SVF_K[(res & 127u) >> 1] : SVF_K24;
        c->kp = kp;
        type = SVF_BASE(type);
        c->pa1 = svf_a1(c->g, kp);
        c->pa2 = (int32_t)(((int64_t)c->g * c->pa1) >> 28);
        c->px = type == SVF_HP || type == SVF_NOTCH ? 1 << 14 : 0;
        c->pl = type == SVF_LP ? 1 << 14 : type == SVF_HP ? -(1 << 14) : 0;
        c->pb = type == SVF_BP ? kp : type == SVF_HP || type == SVF_NOTCH ? -kp : 0;
    }
    switch (type) {
    case SVF_LP: c->cx = 0; c->cl = cp; c->cb = 0; break;
    case SVF_BP: c->cx = 0; c->cl = 0; c->cb = k; break;
    case SVF_HP: c->cx = cp; c->cl = -cp; c->cb = -((k * cp) >> 14); break;
    case SVF_NOTCH: c->cx = 1 << 14; c->cl = 0; c->cb = -k; break;
    default: c->cx = 1 << 14; c->cl = 0; c->cb = 0; break;   /* (off: the input as it is) */
    }
}

/* a filter retuned to the cutoff n (Filter FM: a sample at a time; its type and resonance as svf_coef set them): the
 * integrators' g and a1 a2 (a 24 dB type's first stage's too); a comb its delay. Out of line (svf_a1's division) */
static __attribute__((noinline)) void svf_retune(svf_coef_t *c, int32_t n, uint32_t type)
{
    if (SVF_ISCOMB(type)) { c->cx = (int32_t)svf_comb_delay(n); return; }
    c->g = svf_g(n);
    c->a1 = svf_a1(c->g, c->k);
    c->a2 = (int32_t)(((int64_t)c->g * c->a1) >> 28);
    if (c->p4) {
        c->pa1 = svf_a1(c->g, c->kp);
        c->pa2 = (int32_t)(((int64_t)c->g * c->pa1) >> 28);
    }
}
/* a 24 dB type's first stage, on v0 (inside: SVF_SH bits up). Out of line: the 12 dB types' sample stays inline and
 * as it was (one test more); the 24 dB types pay a call. No peak: within the input's range, bar a transient's
 * overshoot, clamped */
static __attribute__((noinline)) int32_t svf_pre(svf_state_t *s, const svf_coef_t *c, int32_t v0)
{
    int32_t v1 = (int32_t)(((int64_t)c->pa1 * s->ic3 + (int64_t)c->pa2 * (v0 - s->ic4)) >> 30), v2;
    v2 = s->ic4 + (int32_t)(((int64_t)c->g * v1) >> 28);
    s->ic3 = 2 * v1 - s->ic3;
    s->ic4 = 2 * v2 - s->ic4;
    v0 = (int32_t)(((int64_t)c->px * v0 + (int64_t)c->pl * v2 + (int64_t)c->pb * v1) >> 14);
    return v0 > SVF_CLIP << SVF_SH ? SVF_CLIP << SVF_SH : v0 < -(SVF_CLIP << SVF_SH) ? -(SVF_CLIP << SVF_SH) : v0;
}
/* the driven resonance's knee, past +-SVF_NLK (out of line: only a loud resonance pays the call). u past the knee,
 * u - u^3 / (3 NLW^2), its slope 1 - (u / NLW)^2: 1 at the knee, 0 at NLW, flat after. Multiplies only (int64):
 * u^2 >> 20, times u, times 2^16 / 3, >> 36: u^3 / (3 2^40); monotonic (a step of u moves it by 1 at most) */
static __attribute__((noinline)) int32_t svf_sat(int32_t v)
{
    int32_t u = (v < 0 ? -v : v) - SVF_NLK;
    if (u >= SVF_NLW) u = SVF_NLW * 2 / 3;
    else u -= (int32_t)(((((int64_t)u * u) >> 20) * u * 21845) >> 36);   /* (NLW = 2^20) */
    if (u > SVF_NLW * 2 / 3) u = SVF_NLW * 2 / 3;   /* (the truncations leave a few over, by the top) */
    return v < 0 ? -(SVF_NLK + u) : SVF_NLK + u;
}
/* one sample: x within +-SVF_CLIP, the result too */
static inline __attribute__((always_inline)) int32_t svf_tick(svf_state_t *s, const svf_coef_t *c, int32_t x)
{
    int32_t v0 = x * (1 << SVF_SH), v1, v2, y;
    if (c->p4) v0 = svf_pre(s, c, v0);
    v1 = (int32_t)(((int64_t)c->a1 * s->ic1 + (int64_t)c->a2 * (v0 - s->ic2)) >> 30);
    if (c->nl && (uint32_t)(v1 + SVF_NLK) > 2u * SVF_NLK) v1 = svf_sat(v1);   /* (|v1| > NLK) */
    v2 = s->ic2 + (int32_t)(((int64_t)c->g * v1) >> 28);
    s->ic1 = 2 * v1 - s->ic1;
    s->ic2 = 2 * v2 - s->ic2;
    y = (int32_t)(((int64_t)c->cx * v0 + (int64_t)c->cl * v2 + (int64_t)c->cb * v1) >> (14 + SVF_SH));
    return y > SVF_CLIP ? SVF_CLIP : y < -SVF_CLIP ? -SVF_CLIP : y;
}

/* the drive before a filter: a gain g8 / 256 (256 .. 2048), then a cubic soft clip t - t^3 / (3 L^2) up to
 * L = 1.5 x full scale, flat beyond: the result is within +-32769 and never goes down as x goes up (the cube's
 * factor is rounded down: its slope stays under 1). */
static inline int32_t svf_drive(int32_t x, int32_t g8)
{
    int32_t t = (x * g8) >> 8;                  /* (|x| <= SVF_CLIP, g8 <= 2048: < 2^28) */
    if (t > 49152) t = 49152;
    if (t < -49152) t = -49152;
    return t - (int32_t)(((int64_t)t * t * t * 38836) >> 48);   /* 38836 / 2^48 ~ 1 / (3 L^2), from below */
}

/* ---- for the screen (main loop): the analog prototype's response, to draw */
static int32_t svf_log2_q8(uint32_t v)          /* log2(v) x 256 for v >= 1: log2(1 + m) ~ m + .3466 m (1 - m),
                                                   within 0.01 (a smooth curve on the screen, no ripple) */
{
    int32_t p = 31, m;
    while (p && !(v >> p)) p--;
    m = (int32_t)(p >= 12 ? (v >> (p - 12)) & 4095u : (v << (12 - p)) & 4095u);   /* the mantissa, Q12 */
    return (p << 8) + ((m + (((m * (4096 - m)) >> 12) * 1420 >> 12)) >> 4);
}
static int32_t svf_exp2_q12(int32_t ou)         /* 2^(ou / 264), Q12, for ou within +-6 octaves (1584) */
{
    int32_t ip = ou >= 0 ? ou / 264 : -((263 - ou) / 264), f = (ou - ip * 264) * 4096 / 264;
    uint32_t m = (uint32_t)(4096 + ((f * (2689 + ((1407 * f) >> 12))) >> 12));   /* 2^f ~ 1 + .6565 f + .3435 f^2 */
    return (int32_t)(ip >= 0 ? m << ip : m >> -ip);
}
/* the level in dB, Q8, at ou / 264 octaves from the cutoff (within +-6 octaves; beyond, as at 6), damping k (Q14):
 * with u = x - 1/x, |H|^2 = 1 / (x^2 (u^2 + k^2)) low, x^2 / (u^2 + k^2) high, k^2 / (u^2 + k^2) band,
 * u^2 / (u^2 + k^2) notch; the low and high pass at their level comp (Q15). A 24 dB type: both stages' */
static int32_t svf_db_k(uint32_t type, int32_t k, uint32_t comp, int32_t ou)
{
    int32_t x, ix, u, l2x, num, den;
    uint32_t k2 = ((uint32_t)k * (uint32_t)k) >> 12;   /* Q16 */
    if (ou > 1584) ou = 1584;
    if (ou < -1584) ou = -1584;
    x = svf_exp2_q12(ou);
    ix = svf_exp2_q12(-ou);
    u = (x - ix) >> 4;                          /* Q8, |u| < 2^14 */
    den = svf_log2_q8((uint32_t)(u * u) + k2) - (16 << 8);
    l2x = ou * 512 / 264;                       /* log2(x^2), Q8 */
    switch (type) {
    case SVF_LP: num = -l2x; break;
    case SVF_HP: num = l2x; break;
    case SVF_BP: num = svf_log2_q8(k2) - (16 << 8); break;
    case SVF_NOTCH: num = u ? svf_log2_q8((uint32_t)(u * u)) - (16 << 8) : -(30 << 8); break;
    default: return 0;
    }
    if (type == SVF_LP || type == SVF_HP) num += 2 * (svf_log2_q8(comp) - (15 << 8));
    return ((num - den) * 771) >> 8;            /* 10 log10 = 3.0103 log2 */
}
/* a comb's level in dB, Q8, at ou / 264 octaves from its frequency f: theta = 2 pi f' / f, |H|^2 = g^2 / (1 +- 2 fb
 * cos theta + fb^2) (- COMB+, + COMB-); cos from the fraction of a turn by a parabola pair (within 0.06: the picture) */
static int32_t svf_comb_db(uint32_t type, uint32_t res, int32_t ou)
{
    int32_t fb = (int32_t)((res & 127u) * 32112u / 127u), x, c, den;
    if (ou > 1584) ou = 1584;
    if (ou < -1584) ou = -1584;
    x = svf_exp2_q12(ou) & 4095;                /* the fraction of a turn, Q12 */
    x = x < 2048 ? x : 4096 - x;                /* 0 .. half a turn: cos falls from 1 to -1 */
    c = x < 1024 ? 32768 - ((x * x) >> 5) : -32768 + (((2048 - x) * (2048 - x)) >> 5);   /* Q15: 1 at 0, 0 at a
                                                   quarter turn, -1 at half */
    if (type == SVF_COMBP) c = -c;
    den = 32768 + (int32_t)(((int64_t)fb * fb) >> 15) + (int32_t)(((int64_t)2 * fb * c) >> 15);   /* Q15, > 0 */
    if (den < 16) den = 16;
    return ((svf_log2_q8((uint32_t)(32768 - fb)) - svf_log2_q8((uint32_t)den)) * 771) >> 8;   /* g^2 = 1 - fb */
}
static int32_t svf_db_q8(uint32_t type, uint32_t res, int32_t ou)
{
    int32_t db;
    if (SVF_ISCOMB(type)) return svf_comb_db(type, res, ou);
    db = svf_db_k(SVF_BASE(type), SVF_K[res & 127u], SVF_COMP[res & 127u], ou);
    return SVF_IS24(type) ? db + svf_db_k(SVF_BASE(type), type == SVF_PPG ? SVF_K[(res & 127u) >> 1] : SVF_K24, 32768u, ou)
                          : db;
}
#endif
