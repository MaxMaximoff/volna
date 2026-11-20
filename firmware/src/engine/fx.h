/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna engine: the effects (stage 7b), two slots in a row after the voices, one type each. The types come one at a
 * time: DELAY and REVERB, then DIST, then CHORUS.
 *
 * Each slot has FX_MEM 16-bit words of its own (in the pool: static, no malloc), its state, and its settings as the
 * main loop writes them (fx_par_t: read once a block). The main loop changes a slot's type with the slot off: off,
 * memory and state cleared, the settings, the new type last (the ISR never sees half of it).
 *
 * DELAY: a stereo line (FX_DLEN frames: 0.79 s at 44.1 kHz), its time in Q8 samples gliding towards the knob's
 * (no clicks; a tape's pitch bend on the way, at most +-half a sample a sample), read between two samples. The feedback goes through a one-pole
 * low-pass (TONE). PING moves the input to the left and crosses the feedback: at 256 the echoes go left, right,
 * left. The line keeps half the signal (int16 at x / 2, saturated).
 *
 * REVERB: a plate after J. Dattorro, "Effect Design, Part 1" (J. AES 45(9), 1997): a predelay, four input
 * diffusers, a tank of two halves crossing over (an allpass, a delay, the damping low-pass, the decay, an allpass, a
 * delay), the outputs from seven taps on each side. His lengths are for 29761 Hz, here x 1.4818 for 44.1 kHz. No
 * modulation in the tank (yet). Coefficients Q14, the lines int16; the decay multiplies round towards 0, so the tail
 * dies out to 0 rather than holding a -1.
 *
 * DIST: the drive (a gain x 1 .. x 64, tools/gen_dist.py) into a curve: SOFT (tanh, a table), HARD (a clip),
 * FOLD (a wavefolder: past full scale it turns back), TUBE (tanh moved off centre: even harmonics, like a tube's
 * asymmetry). Then, for TUBE, a DC blocker (~35 Hz: its offset), TONE (a one-pole low-pass), LEVEL (the curve's
 * output is near full scale, whatever goes in: LEVEL brings it down). No oversampling: a high drive aliases.
 *
 * CHORUS: a short stereo line (FX_CLEN frames, a mask for the ring) read at a delay that a triangle LFO swings:
 * base +- depth (Q8 samples, read between two samples). The right side reads at the LFO WIDTH later in its cycle
 * (0: the same as the left; 127: half a cycle, the classic stereo chorus). Feedback, signed: a flanger's (a short
 * base): + rings, - hollow. The mode is only the main loop's: CHORUS 3 .. 30 ms, a depth up to 8 ms; FLANGER
 * 0.2 .. 10 ms, a depth up to the base.
 *
 * PHASER (2026-11-15): 2 .. 12 first-order allpass stages in a row, all at one frequency, the line summed with the
 * dry: where the stages turn the phase by an odd half turn, the two cancel (N / 2 notches). Each stage is the
 * zero-delay-feedback one-pole of svf.h's family (TPT: lp = s + v, v = G (x - s), s = lp + v; the allpass 2 lp - x),
 * G = g / (1 + g) from a table (tools/gen_phaser.py), Q30, the shift's remainder carried to the next sample (as
 * engine/eq.h: no offset sticks at 20 Hz). The frequency sweeps in octaves (a note, Q8) around the centre: a
 * triangle LFO bent towards a sine (t (2 - |t|)), recomputed every 16 samples and ramped a sample at a time (no
 * steps heard however fast it moves). The right side's LFO WIDTH later in its cycle (stereo swirl). Feedback,
 * signed, from the line's end to its start through tanh (DIST's table): a soft knee, an analog phaser's resonance,
 * never a runaway. The line passes DC as it is, and (a sample late) turns the top by half a turn: a bare feedback
 * would ring at 0 Hz (+) or at 22 kHz (-), 20 x at 0.95. So, as the coupling capacitors of an analog one, it goes
 * through a DC blocker (10 Hz) and the mean of the last two outputs (nothing at 22 kHz, -3 dB at 11 kHz) first.
 * Its mix is the phaser's own: MIX 127 = dry and line half each (the deepest notches, the level
 * kept), 0 = dry.
 *
 * ENSEMBLE (2026-11-15): the string ensemble (Solina, the Juno's chorus): on each side three taps of one short line,
 * each tap's delay swung by two sine LFOs at once, a slow one (the chorus, 0.1 .. 2 Hz, up to +-3 ms) and a fast one
 * (the vibrato, 3 .. 10 Hz, up to +-0.6 ms), the three a third of a cycle apart, around 6.5 ms. The right side's
 * LFOs WIDTH later (up to a sixth of a cycle: its taps then swing opposite the left's). The taps are read with a
 * cubic (Catmull-Rom, 4 points): a linear read loses up to 2.4 dB at 10 kHz at half a sample, and a moving tap sweeps
 * through that hundreds of times a second (a rough top). Their mean goes through a high pass (156 Hz: delays of a few
 * ms against the dry would hollow out the bass; it stays dry) and TONE (a low pass, 1.5 .. 16.7 kHz, as a BBD line's),
 * both TPT one-poles (PH_G) with the shift's remainder carried. The delays every 16 samples, ramped a sample at a time.
 *
 * LO-FI (2026-11-15): an old sampler's and a tape's ways, in the order a sound goes through them. WOBBLE: the tape's
 * wow (0.6 Hz) and flutter (9 Hz) as a delay swung by two sines (ENSEMBLE's line and cubic read; at 0 a delay of 2
 * samples, its changes slewed: a tape's bend, not a click). RATE: a sample and hold at 44.1 .. 1.1 kHz, its step
 * fractional (it sweeps without steps; the aliasing is the point). NOISE: hiss added as a sample is taken, as loud as
 * the sound's envelope (150 ms) allows: no hiss between notes (the silence test, saving, stay as they were). BITS:
 * 16 .. 2 bits, fractional, mid-tread and symmetric (0 stays 0: no hum on silence, no DC), quantised once a hold,
 * and counted against that envelope, not full scale: a note is 12 .. 20 dB under full scale, and at 3 bits of full
 * scale (a step of 8192) all of it under 4096 went to 0 — its peaks came and went, the sound broke up, stuttered
 * on its decay. Now 3 bits are 3 bits of the note, loud or quiet, to its end (the step, from the envelope, every 16
 * samples: one 32-bit division a side; never under one sample).
 * TONE: the reconstruction filter after it (a TPT low pass, 830 Hz .. 16.7 kHz).
 *
 * FILTER (2026-11-15): the voices' filter (svf.h: Simper's SVF, LP BP HP at 12 and 24 dB, notch; the resonance
 * driven, so a loud mix squashes its peak rather than clipping) on the whole sound, its cutoff moved by SOURCE: an
 * LFO (a sine), the same with the right side a quarter cycle later (STEREO), an LFO at a note value of the tempo
 * (SYNC), or the sound's own envelope (ENV, an auto-wah: attack 3 ms, release RATE; in dB, 48 dB under full scale to
 * full scale, so a quiet note opens it as plainly as a loud one), DEPTH octaves either way (up to 5). The
 * coefficients every 16 samples, and between them each ramped a sample at a time (a fast sweep at a high resonance
 * without the zipper of steps); the integrators carry their shifts' remainders (the tail ends at 0).
 *
 * Mix (all): dry and wet crossfade, each at full up to the middle (Q12). Integers only, no division in the loops. */
#ifndef VOLNA_FX_H
#define VOLNA_FX_H
#include <stdint.h>
#include "dist_tables.h"
#include "phaser_tables.h"
#include "lofi_tables.h"
#include "svf.h"

#define FX_MEM 70000u                           /* int16 words a slot (2 x 140 000 B of the pool) */
#define FX_DLEN (FX_MEM / 2u)                   /* the delay's frames: 35000, 0.79 s */
enum { FX_OFF, FX_DELAY, FX_REVERB, FX_DIST, FX_CHORUS, FX_PHASER, FX_ENSEMBLE, FX_LOFI, FX_FILTER, FX_TRIPLE, FX_N };
enum { FS_LFO, FS_STEREO, FS_SYNC, FS_ENV, FS_N };   /* FILTER's SOURCE */   /* (new ones at the end: the patches keep
                                                   their meaning) */
enum { DS_SOFT, DS_HARD, DS_FOLD, DS_TUBE, DS_N };      /* DIST's curves */
enum { CM_CHORUS, CM_FLANGER, CM_N };                   /* CHORUS's modes */
#define FX_CLEN 2048u                           /* CHORUS's line: 46 ms (4096 words of the slot's memory) */

typedef struct {                                /* a slot's settings, in the ISR's units (the main loop writes) */
    volatile uint8_t type;                      /* FX_*: written last */
    volatile int32_t dry, wet;                  /* Q12 */
    volatile int32_t a, b, c, d, e, f;          /* DELAY: the time (Q8 samples), the feedback (Q14), TONE (Q12),
                                                   PING (Q8); REVERB: the decay (Q14), the damping (Q14), the
                                                   predelay (samples), the width (Q8); DIST: the gain (Q8), the
                                                   curve (DS_*), TONE (Q12), LEVEL (Q12); CHORUS: the base delay
                                                   (Q8 samples), the depth (Q8 samples), the feedback (Q14, signed),
                                                   the LFO's step a sample, WIDTH as a phase (32 bits a cycle);
                                                   PHASER: the centre (a note, Q8), the depth (notes, Q8), the
                                                   feedback (Q14, signed), the LFO's step a sample, WIDTH as a
                                                   phase, the stages; ENSEMBLE: the chorus's and the vibrato's
                                                   reach (samples, Q16), their LFOs' steps a sample, WIDTH as a
                                                   phase, TONE (G, Q30); LO-FI: the wow's and the flutter's reach
                                                   (samples, Q16), RATE (LF_INC), BITS (LF_ index), NOISE (Q15),
                                                   TONE (G, Q30); FILTER: the cutoff (a note, Q8), DEPTH (notes, Q8,
                                                   signed), the LFO's step a sample (or ENV's release, Q24), the
                                                   right side's phase, RESO, the filter's type | SOURCE << 8 */
} fx_par_t;

#define PH_NMAX 12u                             /* PHASER's stages, at most */
typedef struct { uint32_t w; int32_t dq8, lpl, lpr; } fx_dly_t;
typedef struct { uint32_t t; int32_t dl, dr, ll, lr; } fx_rev_t;   /* (t: the rings' common counter) */
typedef struct { int32_t xl, xr, hl, hr, lpl, lpr; } fx_dst_t;
typedef struct { uint32_t w, ph; } fx_cho_t;
typedef struct {                                /* PHASER: the LFO, a sample counter (the coefficients every 16); a
                                                   side each: G now and its step a sample, the line's last two
                                                   outputs and the DC blocker's input and output (the feedback's),
                                                   the stages' states and remainders */
    uint32_t ph, cnt;
    int32_t g[2], dg[2], y[2], y2[2], hx[2], hh[2];
    int32_t st[2][PH_NMAX], rm[2][PH_NMAX];
} fx_pha_t;
typedef struct {                                /* ENSEMBLE: the line's place, the two LFOs, a sample counter; a side
                                                   each: the three taps' delays now (Q16 samples) and their steps;
                                                   the high pass's and TONE's states and remainders */
    uint32_t w, phs, phf, cnt;
    int32_t dq[2][3], dd[2][3];
    int32_t hs[2], hr[2], ts[2], tr[2];
} fx_ens_t;
typedef struct {                                /* LO-FI: the line's place, the wow's and the flutter's phases, the
                                                   hold's, a sample counter, the noise's generator; the delay now
                                                   and its step; a side each: the held sample, the envelope, TONE's
                                                   state and remainder */
    uint32_t w, wph, fph, hph, cnt, rnd;
    int32_t dq, dd;
    int32_t hold[2], env[2], ts[2], tr[2];
    uint32_t qs[2], qi[2];                      /* BITS' step now (Q8) and its inverse (2^32 / step), a side */
} fx_lof_t;
typedef struct {                                /* FILTER: the LFO, a sample counter, the envelope (Q8 samples); a
                                                   side each: the coefficients now, their steps a sample (g a1 a2
                                                   pa1 pa2), the stages' states and remainders */
    uint32_t ph, cnt;
    int32_t env;
    svf_coef_t c[2];
    int32_t d[2][5];
    int32_t st[2][8];
} fx_flt_t;
typedef struct {                                /* TRIPLE: the chorus's line and LFO, the hold's count, the samples
                                                   held */
    fx_cho_t cho;
    uint32_t hc;
    int32_t hl, hr;
} fx_tri_t;
typedef union { fx_dly_t dly; fx_rev_t rev; fx_dst_t dst; fx_cho_t cho; fx_pha_t pha; fx_ens_t ens; fx_lof_t lof;
                fx_flt_t flt; fx_tri_t tri; } fx_state_t;

static inline int32_t fx_sat16(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }
static inline int32_t fx_half(int32_t x) { return (x - (x >> 31)) >> 1; }   /* x / 2 towards 0 (x >> 1 floors:
                                                   with the feedback a -10 would hold for ever) */
static inline int32_t fx_mul14z(int32_t x, int32_t g)   /* x g >> 14, rounded towards 0 (a tail that dies out) */
{
    int32_t p = x * g;
    return p < 0 ? -((-p) >> 14) : p >> 14;
}

/* ---- DELAY */
static void fx_delay(fx_dly_t *s, int16_t *m, int32_t *out, uint32_t n, int32_t tgt, int32_t fb, int32_t k,
                     int32_t pp, int32_t dry, int32_t wet)
{
    uint32_t i, w = s->w;
    int32_t dq = s->dq8, lpl = s->lpl, lpr = s->lpr;
    if (tgt < 256) tgt = 256;
    if (tgt > (int32_t)(FX_DLEN - 2u) << 8) tgt = (int32_t)(FX_DLEN - 2u) << 8;
    if (dq < 256 || dq > (int32_t)(FX_DLEN - 2u) << 8) dq = tgt;
    for (i = 0; i < n; i++) {
        int32_t L = out[2u * i], R = out[2u * i + 1u], df = dq - tgt, st = df >> 12, tl, tr, il, ir, fl, fr;
        uint32_t di, fq;
        int32_t r0, r1;
        if (!st && df) st = df > 0 ? 1 : -1;  /* (the time glides: ~93 ms a step of e, the read point at most */
        st = st > 128 ? 128 : st < -128 ? -128 : st;   /* half a sample a sample faster or slower: a tape's bend) */
        dq -= st;
        di = (uint32_t)dq >> 8; fq = (uint32_t)dq & 255u;
        r0 = (int32_t)w - (int32_t)di; if (r0 < 0) r0 += (int32_t)FX_DLEN;
        r1 = r0 - 1; if (r1 < 0) r1 += (int32_t)FX_DLEN;
        tl = (m[2 * r0] * (int32_t)(256u - fq) + m[2 * r1] * (int32_t)fq) >> 7;   /* (x 2: the line keeps half) */
        tr = (m[2 * r0 + 1] * (int32_t)(256u - fq) + m[2 * r1 + 1] * (int32_t)fq) >> 7;
        lpl += ((tl - lpl) * k) >> 12;          /* TONE */
        lpr += ((tr - lpr) * k) >> 12;
        il = (L * (256 - pp) + ((L + R) >> 1) * pp) >> 8;   /* PING: the input to the left, the feedback crossed */
        ir = (R * (256 - pp)) >> 8;
        fl = fx_mul14z((lpl * (256 - pp) + lpr * pp) >> 8, fb);
        fr = fx_mul14z((lpr * (256 - pp) + lpl * pp) >> 8, fb);
        m[2u * w] = (int16_t)fx_sat16(fx_half(il + fl));
        m[2u * w + 1u] = (int16_t)fx_sat16(fx_half(ir + fr));
        if (++w == FX_DLEN) w = 0;
        out[2u * i] = (L * dry + lpl * wet) >> 12;
        out[2u * i + 1u] = (R * dry + lpr * wet) >> 12;
    }
    s->w = w; s->dq8 = dq; s->lpl = lpl; s->lpr = lpr;
}

/* ---- REVERB: the lines (Dattorro's lengths x 1.4818): 0..3 the input diffusers, 4 the left tank's allpass, 5 its
 * delay, 6 its second allpass, 7 its second delay, 8..11 the right's, 12 the predelay (100 ms). Each line is a ring of
 * a power of two words (its length or more): one counter t for all of them, a line's place masked, no compares, no
 * index a line (the same delays and taps as before, so the same sound: tests/fx_test.c, tests/fx_check.py).
 * RVn: the ring's start, its mask, the line's length */
#define RV0 0u, 255u, 210u
#define RV1 256u, 255u, 159u
#define RV2 512u, 1023u, 562u
#define RV3 1536u, 511u, 410u
#define RV4 2048u, 1023u, 996u
#define RV5 3072u, 8191u, 6598u
#define RV6 11264u, 4095u, 2667u
#define RV7 15360u, 8191u, 5512u
#define RV8 23552u, 2047u, 1345u
#define RV9 25600u, 8191u, 6249u
#define RV10 33792u, 4095u, 3936u
#define RV11 37888u, 8191u, 4687u
#define RV12 46080u, 8191u, 4411u
#define REV_WORDS 54272u                        /* (the rings' words in all, of the slot's FX_MEM) */
typedef char fx_rev_fits[REV_WORDS <= FX_MEM ? 1 : -1];
#define REV_PRE_MAX 4410u                       /* the predelay's longest (its line's length - 1) */
static inline int32_t rv_tap(const int16_t *m, uint32_t t, uint32_t b, uint32_t msk, uint32_t len, uint32_t k)
{                                               /* written k ago (1: this sample, as the line was just written) */
    (void)len;
    return m[b + ((t + 1u - k) & msk)];
}
static inline int32_t rv_delay(int16_t *m, uint32_t t, uint32_t b, uint32_t msk, uint32_t len, int32_t x)
{                                               /* in x, out what went in len ago */
    int32_t y = m[b + ((t - len) & msk)];
    m[b + (t & msk)] = (int16_t)fx_sat16(x);
    return y;
}
static inline int32_t rv_allpass(int16_t *m, uint32_t t, uint32_t b, uint32_t msk, uint32_t len, int32_t x, int32_t g)
{                                               /* g Q14 */
    int32_t z = m[b + ((t - len) & msk)], v = fx_sat16(x - ((z * g) >> 14));
    m[b + (t & msk)] = (int16_t)v;
    return z + ((v * g) >> 14);
}
static void fx_reverb(fx_rev_t *s, int16_t *m, int32_t *out, uint32_t n, int32_t dec, int32_t damp, int32_t pre,
                      int32_t wd, int32_t dry, int32_t wet)
{
    uint32_t i, t = s->t;
    int32_t dd2 = dec + 2458;                   /* decay diffusion 2: the decay + 0.15, 0.25 .. 0.5 */
    dd2 = dd2 < 4096 ? 4096 : dd2 > 8192 ? 8192 : dd2;
    if (pre < 1) pre = 1;
    if (pre > (int32_t)REV_PRE_MAX) pre = (int32_t)REV_PRE_MAX;
    for (i = 0; i < n; i++, t++) {
        int32_t L = out[2u * i], R = out[2u * i + 1u], x, a, b, yl, yr, ll = s->ll, lr = s->lr;
        rv_delay(m, t, RV12, (L + R) >> 2);     /* the predelay (mono, half) */
        x = rv_tap(m, t, RV12, (uint32_t)pre);
        x = rv_allpass(m, t, RV0, x, 12288);    /* the input diffusers: 0.75 0.75 0.625 0.625 */
        x = rv_allpass(m, t, RV1, x, 12288);
        x = rv_allpass(m, t, RV2, x, 10240);
        x = rv_allpass(m, t, RV3, x, 10240);
        a = rv_allpass(m, t, RV4, x + fx_mul14z(lr, dec), -11469);   /* the left half, fed by the right's end */
        b = rv_delay(m, t, RV5, a);
        s->dl += ((b - s->dl) * (16384 - damp)) >> 14;
        b = rv_allpass(m, t, RV6, fx_mul14z(s->dl, dec), dd2);
        s->ll = rv_delay(m, t, RV7, b);
        a = rv_allpass(m, t, RV8, x + fx_mul14z(ll, dec), -11469);   /* the right half, fed by the left's end */
        b = rv_delay(m, t, RV9, a);
        s->dr += ((b - s->dr) * (16384 - damp)) >> 14;
        b = rv_allpass(m, t, RV10, fx_mul14z(s->dr, dec), dd2);
        s->lr = rv_delay(m, t, RV11, b);
        yl = rv_tap(m, t, RV9, 394) + rv_tap(m, t, RV9, 4407) - rv_tap(m, t, RV10, 2835) + rv_tap(m, t, RV11, 2958)
           - rv_tap(m, t, RV5, 2949) - rv_tap(m, t, RV6, 277) - rv_tap(m, t, RV7, 1580);
        yr = rv_tap(m, t, RV5, 523) + rv_tap(m, t, RV5, 5375) - rv_tap(m, t, RV6, 1820) + rv_tap(m, t, RV7, 3961)
           - rv_tap(m, t, RV9, 3128) - rv_tap(m, t, RV10, 496) - rv_tap(m, t, RV11, 179);
        yl = (yl * 5) >> 2;                     /* (x 1.25: about as loud as what goes in) */
        yr = (yr * 5) >> 2;
        x = (yl * (256 + wd) + yr * (256 - wd)) >> 9;   /* WIDTH: 0 mono, 256 the plate's two sides */
        yr = (yr * (256 + wd) + yl * (256 - wd)) >> 9;
        out[2u * i] = (L * dry + x * wet) >> 12;
        out[2u * i + 1u] = (R * dry + yr * wet) >> 12;
    }
    s->t = t;
}

/* ---- DIST */
#define DIST_TUBE0 9546                         /* tanh(0.3), Q15: TUBE's curve through 0 */
static inline int32_t dist_tanh(int32_t x)      /* tanh of x (Q15, 32768 = 1), Q15; past +-4 the table's ends */
{
    uint32_t p, i, f;
    if (x >= 131072) return DIST_TANH[256];
    if (x <= -131072) return DIST_TANH[0];
    p = (uint32_t)(x + 131072);
    i = p >> 10;
    f = p & 1023u;
    return DIST_TANH[i] + (((DIST_TANH[i + 1u] - DIST_TANH[i]) * (int32_t)f) >> 10);
}
static inline int32_t dist_shape(int32_t x, uint32_t sh)   /* the curve: x Q15 (the drive in it), out Q15 */
{
    uint32_t t;
    switch (sh) {
    case DS_HARD: return x > 32767 ? 32767 : x < -32767 ? -32767 : x;
    case DS_FOLD:                               /* a triangle of period 4: up to 1, back down to -1, ... */
        t = (uint32_t)(x + 32768) & 131071u;
        return t < 65536u ? (int32_t)t - 32768 : 98304 - (int32_t)t;
    case DS_TUBE: return ((dist_tanh(x + 9830) - DIST_TUBE0) * 25384) >> 15;   /* (tanh(x + 0.3) - tanh(0.3)) /
                                                   1.29: -1 .. 0.55, within full scale */
    default: return dist_tanh(x);
    }
}
static void fx_dist(fx_dst_t *s, int32_t *out, uint32_t n, int32_t g, uint32_t sh, int32_t k, int32_t lv,
                    int32_t dry, int32_t wet)
{
    uint32_t i;
    int32_t xl = s->xl, xr = s->xr, hl = s->hl, hr = s->hr, lpl = s->lpl, lpr = s->lpr;
    for (i = 0; i < n; i++) {
        int32_t L = out[2u * i], R = out[2u * i + 1u], a, b, yl, yr;
        a = L > 131071 ? 131071 : L < -131071 ? -131071 : L;   /* (x the gain, <= 16384: under 2^31) */
        b = R > 131071 ? 131071 : R < -131071 ? -131071 : R;
        yl = dist_shape((a * g) >> 8, sh);
        yr = dist_shape((b * g) >> 8, sh);
        if (sh == DS_TUBE) {                    /* the DC blocker (R = 0.995, rounded: no offset of its own) */
            hl = yl - xl + ((hl * 32604 + 16384) >> 15);
            hr = yr - xr + ((hr * 32604 + 16384) >> 15);
            xl = yl; xr = yr;
        } else { hl = yl; hr = yr; }           /* (the symmetric curves leave no DC) */
        lpl += ((hl - lpl) * k) >> 12;          /* TONE */
        lpr += ((hr - lpr) * k) >> 12;
        out[2u * i] = (L * dry + ((lpl * lv) >> 12) * wet) >> 12;
        out[2u * i + 1u] = (R * dry + ((lpr * lv) >> 12) * wet) >> 12;
    }
    s->xl = xl; s->xr = xr; s->hl = hl; s->hr = hr; s->lpl = lpl; s->lpr = lpr;
}

/* ---- the output's limiter (after the slots): as it is up to OUT_KNEE, then tanh towards full scale, 32767 at most,
 * so that MASTER (which only turns it down) never clips. A soft clip, no division */
#define OUT_KNEE 20000
static inline int32_t out_limit(int32_t x)
{
    int32_t a = x < 0 ? -x : x, u;
    if (a <= OUT_KNEE) return x;
    u = a - OUT_KNEE;
    if (u > 51000) u = 51000;                   /* (tanh is flat by then; x 84104 stays under 2^32) */
    u = (int32_t)(((uint32_t)u * 84104u) >> 15);   /* (32767 - OUT_KNEE) -> 1.0, Q15 */
    a = OUT_KNEE + (((32767 - OUT_KNEE) * dist_tanh(u)) >> 15);
    return x < 0 ? -a : a;
}

/* ---- CHORUS */
static inline int32_t cho_tri(uint32_t ph)      /* a triangle, Q15: -32768 at phase 0, up to 32767 at half */
{
    uint32_t u = ph >> 15;                      /* 0 .. 131071 */
    return u < 65536u ? (int32_t)u - 32768 : 98303 - (int32_t)u;
}
static inline int32_t cho_read(const int16_t *m, uint32_t w, int32_t dq, uint32_t ch)   /* dq Q8 samples ago */
{
    uint32_t r0 = (w - ((uint32_t)dq >> 8)) & (FX_CLEN - 1u), r1 = (r0 - 1u) & (FX_CLEN - 1u), f = (uint32_t)dq & 255u;
    return (m[2u * r0 + ch] * (int32_t)(256u - f) + m[2u * r1 + ch] * (int32_t)f) >> 7;   /* (x 2: half kept) */
}
static void fx_chorus(fx_cho_t *s, int16_t *m, int32_t *out, uint32_t n, int32_t base, int32_t depth, int32_t fb,
                      uint32_t inc, uint32_t woff, int32_t dry, int32_t wet)
{
    uint32_t i, w = s->w, ph = s->ph;
    for (i = 0; i < n; i++) {
        int32_t L = out[2u * i], R = out[2u * i + 1u], dl, dr, tl, tr;
        ph += inc;
        dl = base + (int32_t)(((int64_t)cho_tri(ph) * depth) >> 15);   /* (int64 multiply: fine) */
        dr = base + (int32_t)(((int64_t)cho_tri(ph + woff) * depth) >> 15);
        dl = dl < 512 ? 512 : dl > (int32_t)(FX_CLEN - 4u) << 8 ? (int32_t)(FX_CLEN - 4u) << 8 : dl;   /* (>= 2 samples) */
        dr = dr < 512 ? 512 : dr > (int32_t)(FX_CLEN - 4u) << 8 ? (int32_t)(FX_CLEN - 4u) << 8 : dr;
        tl = cho_read(m, w, dl, 0);
        tr = cho_read(m, w, dr, 1);
        m[2u * w] = (int16_t)fx_sat16(fx_half(L + fx_mul14z(tl, fb)));
        m[2u * w + 1u] = (int16_t)fx_sat16(fx_half(R + fx_mul14z(tr, fb)));
        w = (w + 1u) & (FX_CLEN - 1u);
        out[2u * i] = (L * dry + tl * wet) >> 12;
        out[2u * i + 1u] = (R * dry + tr * wet) >> 12;
    }
    s->w = w; s->ph = ph;
}

/* ---- TRIPLE (2026-11-16, the Blofeld's Triple FX): S&H, then overdrive, then a chorus, in a row. S&H holds a
 * sample for sh of them (1: as it is; the rate cut, no filter: the Blofeld's grit), the overdrive tanh(x g) (g Q8,
 * x 1 .. x 64 as DIST's, 0: none; symmetric, no DC), the chorus (the line, the LFO as CHORUS's, no feedback) mixed in by cm (Q12). x: one
 * sample through S&H and the drive, for the screen too */
static inline int32_t tri_drive(int32_t x, int32_t g)
{
    int32_t t;
    if (!g) return x;
    t = (int32_t)(((int64_t)x * g) >> 8);
    return dist_tanh(t > 131072 ? 131072 : t < -131072 ? -131072 : t);
}
static __attribute__((noinline)) void fx_triple(fx_tri_t *s, int16_t *m, int32_t *out, uint32_t n, int32_t base,
                                                int32_t depth, int32_t cm, uint32_t inc, uint32_t sh, int32_t g,
                                                int32_t dry, int32_t wet)
{
    uint32_t i, w = s->cho.w, ph = s->cho.ph, hc = s->hc, comp = sh >> 8;   /* (sh: the hold | the level's
                                                   compensation << 8, Q12) */
    int32_t hl = s->hl, hr = s->hr;
    sh &= 255u;
    for (i = 0; i < n; i++) {
        int32_t L = out[2u * i], R = out[2u * i + 1u], a, b, dl, dr, tl, tr;
        if (++hc >= sh) { hc = 0; hl = fx_sat16(L); hr = fx_sat16(R); }   /* S&H */
        a = tri_drive(hl, g);
        b = tri_drive(hr, g);
        if (g) { a = (a * (int32_t)comp) >> 12; b = (b * (int32_t)comp) >> 12; }   /* (the drive's gain half
                                                   taken back: 1 / sqrt(g)) */
        ph += inc;                              /* the chorus: the right side half a cycle on */
        dl = base + (int32_t)(((int64_t)cho_tri(ph) * depth) >> 15);
        dr = base + (int32_t)(((int64_t)cho_tri(ph + 0x80000000u) * depth) >> 15);
        tl = cho_read(m, w, dl < 512 ? 512 : dl, 0);
        tr = cho_read(m, w, dr < 512 ? 512 : dr, 1);
        m[2u * w] = (int16_t)fx_half(a);
        m[2u * w + 1u] = (int16_t)fx_half(b);
        w = (w + 1u) & (FX_CLEN - 1u);
        a = (a * (4096 - cm) + tl * cm) >> 12;
        b = (b * (4096 - cm) + tr * cm) >> 12;
        out[2u * i] = (L * dry + a * wet) >> 12;
        out[2u * i + 1u] = (R * dry + b * wet) >> 12;
    }
    s->cho.w = w; s->cho.ph = ph; s->hc = hc; s->hl = hl; s->hr = hr;
}

/* ---- PHASER */
#define PH_NLO (28 << 8)                        /* the sweep's ends: 41 Hz .. 13.3 kHz (notes, Q8; below 41 Hz the
                                                   notches would all be sub-bass, and the line rings for ages) */
#define PH_NHI (128 << 8)
#define PH_DCR 32721                            /* the feedback's DC blocker: R = 1 - 2 pi 10 / 44100, Q15 */
#define PH_SH 4                                 /* the line's extra bits (as svf.h's SVF_SH) */
static inline int32_t ph_g(int32_t n)           /* G for the note n (Q8), Q30 */
{
    int32_t i, f;
    n = n < PH_NLO ? PH_NLO : n > PH_NHI ? PH_NHI : n;
    i = n >> 7;
    f = n & 127;
    return (int32_t)PH_G[i] + (int32_t)(((int64_t)((int32_t)PH_G[i + 1] - (int32_t)PH_G[i]) * f) >> 7);
}
static inline int32_t ph_lfo(uint32_t ph)       /* the sweep's shape, Q15: the triangle bent towards a sine,
                                                   t (2 - |t|) (within 1.1 % of it, no corners) */
{
    int32_t t = cho_tri(ph), a = t < 0 ? -t : t;
    return (int32_t)(((int64_t)t * (65536 - a)) >> 15);
}
/* cn the centre (a note, Q8), dep the sweep's reach (notes, Q8, either way), fb Q14 signed, inc the LFO's step a
 * sample, woff the right side's phase, nst the stages. Out of line: app_audio's own loops stay as they were */
static __attribute__((noinline)) void fx_phaser(fx_pha_t *s, int32_t *out, uint32_t n, int32_t cn, int32_t dep,
                                                int32_t fb, uint32_t inc, uint32_t woff, uint32_t nst, int32_t dry,
                                                int32_t wet)
{
    uint32_t i, k, c;
    if (nst > PH_NMAX) nst = PH_NMAX;
    for (i = 0; i < n; i++) {
        if (!(s->cnt++ & 15u))                  /* the coefficients where the LFO is 16 samples on; a ramp there */
            for (c = 0; c < 2u; c++) {
                int32_t nt = cn + (int32_t)(((int64_t)ph_lfo(s->ph + 16u * inc + (c ? woff : 0u)) * dep) >> 15);
                int32_t gt = ph_g(nt);
                if (!s->g[c]) s->g[c] = gt;     /* (fresh: start there) */
                s->dg[c] = (gt - s->g[c]) / 16;
            }
        s->ph += inc;
        for (c = 0; c < 2u; c++) {
            int32_t x = out[2u * i + c], u, g = s->g[c] += s->dg[c], m;
            int32_t *st = s->st[c], *rm = s->rm[c];
            m = (s->y[c] + s->y2[c] + 1) >> 1;  /* the feedback: the mean of the last two, no DC, through tanh */
            {                                   /* (R h towards 0: rounded to nearest it would stick anywhere within
                                                   1 / (2 (1 - R)) = +-351, and the loop would hold that offset) */
                int64_t p = (int64_t)s->hh[c] * PH_DCR;
                s->hh[c] = m - s->hx[c] + (int32_t)(p < 0 ? -((-p) >> 15) : p >> 15);
            }
            s->hx[c] = m;
            u = x > 131071 ? 131071 : x < -131071 ? -131071 : x;
            {                                   /* the feedback, inside (PH_SH bits up): tanh of the sample, its bits
                                                   under one sample carried as they are (tanh is a straight line
                                                   there); of |h|, the sign after (odd to the bit: the table's chord
                                                   gives -1 for -1 but 0 for +1); times fb towards 0 (never more than
                                                   it should be: a quiet loop only loses). Each of the three: without
                                                   it a quiet tail near a resonance kept ticking for ever (a limit
                                                   cycle, +-1 .. 4 at the output; tests/phaser_test.c) */
                int64_t p;
                int32_t h = s->hh[c], a = h < 0 ? -h : h, t;
                t = dist_tanh(a >> PH_SH) * (1 << PH_SH) + (a & ((1 << PH_SH) - 1));
                p = (int64_t)(h < 0 ? -t : t) * fb;
                u = u * (1 << PH_SH) + (int32_t)(p < 0 ? -((-p) >> 14) : p >> 14);
            }
            for (k = 0; k < nst; k++) {         /* the stages: TPT one-pole, its allpass 2 lp - u */
                int64_t p = (int64_t)(u - st[k]) * g + rm[k];
                int32_t v = (int32_t)(p >> 30), lp = v + st[k];
                rm[k] = (int32_t)(p & 0x3FFFFFFF);
                st[k] = lp + v;
                u = 2 * lp - u;
            }
            s->y2[c] = s->y[c];
            s->y[c] = u;
            out[2u * i + c] = (int32_t)(((int64_t)x * dry * (1 << PH_SH) + (int64_t)u * wet + (1 << (11 + PH_SH)))
                                        >> (12 + PH_SH));   /* (rounded once, from inside: no -1 left by a floor) */
        }
    }
}

/* ---- ENSEMBLE */
#define FX_ELEN 1024u                           /* the line: 23 ms a side (2048 words of the slot's memory) */
#define ENS_BASE (287 << 16)                    /* the taps' centre: 6.5 ms (Q16 samples) */
#define ENS_HPN (51 << 8)                       /* the wet's high pass: 156 Hz (a note, Q8) */
#define ENS_THIRD 1431655765u                   /* a third of a cycle */
static inline int32_t fx_cos14(uint32_t th)     /* cos of th (32 bits a turn), Q14 (PH_COS, between its points) */
{
    uint32_t i = th >> 24, f = (th >> 16) & 255u;
    return PH_COS[i] + (((PH_COS[i + 1u] - PH_COS[i]) * (int32_t)f) >> 8);
}
static inline int32_t ens_tap(int32_t ds, int32_t dv, uint32_t ps, uint32_t pf)   /* a tap's delay (Q16 samples) */
{
    return ENS_BASE + (int32_t)(((int64_t)fx_cos14(ps - 0x40000000u) * ds) >> 14)
                    + (int32_t)(((int64_t)fx_cos14(pf - 0x40000000u) * dv) >> 14);   /* (sines) */
}
/* the line read dq (Q16 samples) ago, between its samples by Catmull-Rom (4 points; the line keeps half: 2x) */
static inline int32_t ens_read(const int16_t *m, uint32_t w, int32_t dq, uint32_t ch)
{
    const uint32_t M = FX_ELEN - 1u;
    uint32_t i1 = (w - ((uint32_t)dq >> 16)) & M;
    int32_t f = (dq & 0xFFFF) >> 5;             /* (Q11) */
    int32_t y0 = m[2u * ((i1 + 1u) & M) + ch], y1 = m[2u * i1 + ch], y2 = m[2u * ((i1 - 1u) & M) + ch];
    int32_t y3 = m[2u * ((i1 - 2u) & M) + ch], r;
    r = (((y3 - y0) + 3 * (y1 - y2)) * f) >> 11;
    r = ((r + 2 * y0 - 5 * y1 + 4 * y2 - y3) * f) >> 11;
    r = ((r + y2 - y0) * f) >> 11;
    return r + 2 * y1;
}
static inline int32_t ens_lp(int32_t x, int32_t g, int32_t *s, int32_t *r)   /* TPT one-pole low pass (G Q30) */
{
    int64_t p = (int64_t)(x - *s) * g + *r;
    int32_t v = (int32_t)(p >> 30), y = v + *s;
    *r = (int32_t)(p & 0x3FFFFFFF);
    *s = y + v;
    return y;
}
/* ds, dv: the chorus's and the vibrato's reach (Q16 samples), is, iv: their LFOs' steps a sample, woff the right
 * side's phase, gt TONE's G (Q30). Out of line, as PHASER */
static __attribute__((noinline)) void fx_ensemble(fx_ens_t *s, int16_t *m, int32_t *out, uint32_t n, int32_t ds,
                                                  int32_t dv, uint32_t is, uint32_t iv, uint32_t woff, int32_t gt,
                                                  int32_t dry, int32_t wet)
{
    uint32_t i, c, k, w = s->w;
    int32_t gh = ph_g(ENS_HPN);
    for (i = 0; i < n; i++) {
        if (!(s->cnt++ & 15u)) {                /* the taps where the LFOs are 16 samples on; a ramp there */
            uint32_t ps = s->phs + 16u * is, pf = s->phf + 16u * iv;
            for (c = 0; c < 2u; c++)
                for (k = 0; k < 3u; k++) {
                    uint32_t o = k * ENS_THIRD + (c ? woff : 0u);
                    int32_t t = ens_tap(ds, dv, ps + o, pf + o);
                    if (!s->dq[c][k]) s->dq[c][k] = t;   /* (fresh: start there) */
                    s->dd[c][k] = (t - s->dq[c][k]) / 16;
                }
        }
        s->phs += is;
        s->phf += iv;
        m[2u * w] = (int16_t)fx_sat16(fx_half(out[2u * i]));   /* (the line keeps half) */
        m[2u * w + 1u] = (int16_t)fx_sat16(fx_half(out[2u * i + 1u]));
        for (c = 0; c < 2u; c++) {
            int32_t x = out[2u * i + c], sum = 0, y;
            for (k = 0; k < 3u; k++) sum += ens_read(m, w, s->dq[c][k] += s->dd[c][k], c);
            y = (int32_t)(((int64_t)sum * 21845) >> 12);   /* the mean of the three, PH_SH (4) bits up */
            y -= ens_lp(y, gh, &s->hs[c], &s->hr[c]);      /* the bass stays dry */
            y = ens_lp(y, gt, &s->ts[c], &s->tr[c]);       /* TONE */
            out[2u * i + c] = (int32_t)(((int64_t)x * dry * 16 + (int64_t)y * wet + (1 << 15)) >> 16);
        }
        w = (w + 1u) & (FX_ELEN - 1u);
    }
    s->w = w;
}

/* ---- LO-FI */
#define LF_WOW 58435u                           /* the wow's step a sample: 0.6 Hz */
#define LF_FLUT 876523u                         /* the flutter's: 9 Hz */
#define LF_MIN (2 << 16)                        /* the line's shortest delay: 2 samples (Q16) */
#define LF_SLEW 16384                           /* the delay's change, at most a quarter sample a sample */
#define LF_REL 32763                            /* the envelope's release: 1 - 5 / 32768 a sample (150 ms) */
static inline void lf_step(uint32_t b, int32_t env, uint32_t *qs, uint32_t *qi)   /* BITS b against the envelope
                                                   env: the step (Q8 samples, at least one sample) and 2^32 / it */
{
    uint32_t st = (uint32_t)(((uint64_t)LF_STEP[b] * (uint32_t)(env > 0 ? env : 0)) >> 15);
    if (st < 256u) st = 256u;
    *qs = st;
    *qi = 0xFFFFFFFFu / st;
}
static inline int32_t lf_quant(int32_t x, uint32_t qs, uint32_t qi)   /* x to the step qs: mid-tread, symmetric */
{
    int32_t a = x < 0 ? -x : x, q;
    q = (int32_t)(((int64_t)a * qi + (1 << 23)) >> 24);   /* (the nearest step; half away from 0) */
    q = (int32_t)(((int64_t)q * qs) >> 8);
    return x < 0 ? -q : q;
}
/* wa, fa the wow's and the flutter's reach (Q16 samples), hinc the hold's step (Q31), b BITS (a table index), nk
 * NOISE (Q15 of the envelope), gt TONE's G (Q30). Out of line */
static __attribute__((noinline)) void fx_lofi(fx_lof_t *s, int16_t *m, int32_t *out, uint32_t n, int32_t wa, int32_t fa,
                                              uint32_t hinc, uint32_t b, int32_t nk, int32_t gt, int32_t dry, int32_t wet)
{
    uint32_t i, c, w = s->w;
    if (b > 127u) b = 127u;
    if (!s->rnd) s->rnd = 0x9E3779B9u;          /* (fresh: a seed; xorshift stays at 0 from 0) */
    for (i = 0; i < n; i++) {
        uint32_t take;
        if (!(s->cnt++ & 15u)) {                /* the delay where the wow and the flutter are 16 samples on */
            uint32_t pw = s->wph + 16u * LF_WOW - 0x40000000u, pf = s->fph + 16u * LF_FLUT - 0x40000000u;
            int32_t t = LF_MIN + wa + fa + (int32_t)(((int64_t)fx_cos14(pw) * wa) >> 14)
                      + (int32_t)(((int64_t)fx_cos14(pf) * fa) >> 14), d;
            if (!s->dq) s->dq = t;              /* (fresh: start there) */
            d = (t - s->dq) / 16;
            s->dd = d > LF_SLEW ? LF_SLEW : d < -LF_SLEW ? -LF_SLEW : d;   /* (WOBBLE turned: a bend, not a jump) */
            lf_step(b, s->env[0], &s->qs[0], &s->qi[0]);   /* BITS against each side's envelope */
            lf_step(b, s->env[1], &s->qs[1], &s->qi[1]);
        }
        s->wph += LF_WOW;
        s->fph += LF_FLUT;
        s->dq += s->dd;
        if (s->dq < LF_MIN) s->dq = LF_MIN;
        m[2u * w] = (int16_t)fx_sat16(fx_half(out[2u * i]));
        m[2u * w + 1u] = (int16_t)fx_sat16(fx_half(out[2u * i + 1u]));
        s->hph += hinc;                         /* the hold: a new sample when its phase passes a whole one */
        take = s->hph >> 31;
        s->hph &= 0x7FFFFFFFu;
        for (c = 0; c < 2u; c++) {
            int32_t x = out[2u * i + c], v = ens_read(m, w, s->dq, c), a = v < 0 ? -v : v, y;
            s->env[c] = a > s->env[c] ? a : (int32_t)(((int64_t)s->env[c] * LF_REL) >> 15);
            if (take) {
                int32_t nz;
                s->rnd ^= s->rnd << 13; s->rnd ^= s->rnd >> 17; s->rnd ^= s->rnd << 5;
                nz = (int32_t)(s->rnd >> 16) - 32768;
                v += (int32_t)(((int64_t)nz * s->env[c] * nk) >> 30);   /* (the hiss, as loud as the envelope lets) */
                s->hold[c] = lf_quant(v, s->qs[c], s->qi[c]);
            }
            y = ens_lp(s->hold[c] * (1 << 4), gt, &s->ts[c], &s->tr[c]);   /* TONE, 4 bits up */
            out[2u * i + c] = (int32_t)(((int64_t)x * dry * 16 + (int64_t)y * wet + (1 << 15)) >> 16);
        }
        w = (w + 1u) & (FX_ELEN - 1u);
    }
    s->w = w;
}

/* ---- FILTER */
#define FF_ATK 126800                           /* the envelope's attack: 3 ms (Q24 a sample) */
/* svf.h's sample, its two shifts carrying their remainders (s: ic1 ic2 r1 r2, the 24 dB pre stage's ic3 ic4 r3 r4);
 * x within +-SVF_CLIP, the result too, rounded */
static inline __attribute__((always_inline)) int32_t ff_tick(int32_t *s, const svf_coef_t *c, int32_t x)
{
    int32_t v0 = x * (1 << SVF_SH), v1, v2;
    int64_t p;
    if (c->p4) {
        p = (int64_t)c->pa1 * s[4] + (int64_t)c->pa2 * (v0 - s[5]) + s[6];
        v1 = (int32_t)(p >> 30); s[6] = (int32_t)(p & 0x3FFFFFFF);
        p = (int64_t)c->g * v1 + s[7];
        v2 = s[5] + (int32_t)(p >> 28); s[7] = (int32_t)(p & 0x0FFFFFFF);
        s[4] = 2 * v1 - s[4];
        s[5] = 2 * v2 - s[5];
        v0 = (int32_t)(((int64_t)c->px * v0 + (int64_t)c->pl * v2 + (int64_t)c->pb * v1 + 8192) >> 14);
        v0 = v0 > SVF_CLIP << SVF_SH ? SVF_CLIP << SVF_SH : v0 < -(SVF_CLIP << SVF_SH) ? -(SVF_CLIP << SVF_SH) : v0;
    }
    p = (int64_t)c->a1 * s[0] + (int64_t)c->a2 * (v0 - s[1]) + s[2];
    v1 = (int32_t)(p >> 30); s[2] = (int32_t)(p & 0x3FFFFFFF);
    if (c->nl && (uint32_t)(v1 + SVF_NLK) > 2u * SVF_NLK) v1 = svf_sat(v1);
    p = (int64_t)c->g * v1 + s[3];
    v2 = s[1] + (int32_t)(p >> 28); s[3] = (int32_t)(p & 0x0FFFFFFF);
    s[0] = 2 * v1 - s[0];
    s[1] = 2 * v2 - s[1];
    v0 = (int32_t)(((int64_t)c->cx * v0 + (int64_t)c->cl * v2 + (int64_t)c->cb * v1 + (1 << (13 + SVF_SH))) >> (14 + SVF_SH));
    return v0 > SVF_CLIP ? SVF_CLIP : v0 < -SVF_CLIP ? -SVF_CLIP : v0;
}
/* cn the cutoff (a note, Q8), dep DEPTH (notes, Q8, signed), rate the LFO's step a sample (ENV: the release, Q24),
 * woff the right side's phase, res RESO, tsrc the filter's type | SOURCE << 8. Out of line */
static __attribute__((noinline)) void fx_filter(fx_flt_t *s, int32_t *out, uint32_t n, int32_t cn, int32_t dep,
                                                uint32_t rate, uint32_t woff, uint32_t res, uint32_t tsrc, int32_t dry,
                                                int32_t wet)
{
    uint32_t i, c, k, type = tsrc & 255u, src = (tsrc >> 8) & 255u;
    if (type >= SVF_NTYPE || type == SVF_OFF) type = SVF_LP;
    for (i = 0; i < n; i++) {
        int32_t L = out[2u * i], R = out[2u * i + 1u];
        if (src == FS_ENV) {                    /* the envelope: the louder side, fast up, RATE down */
            int32_t a = (L < 0 ? -L : L) > (R < 0 ? -R : R) ? (L < 0 ? -L : L) : (R < 0 ? -R : R), e = s->env;
            if (a > SVF_CLIP) a = SVF_CLIP;
            a <<= 8;
            s->env = e + (int32_t)(((int64_t)(a - e) * (a > e ? FF_ATK : (int32_t)rate)) >> 24);
        }
        if (!(s->cnt++ & 15u)) {                /* the coefficients 16 samples on; a ramp to them */
            int32_t m[2];
            if (src == FS_ENV) {                /* 0 at -48 dBFS (and under) .. 1 at full scale, in dB */
                int32_t lv = svf_log2_q8((uint32_t)(s->env >> 8) + 1u) - (7 << 8);
                m[0] = m[1] = lv <= 0 ? 0 : lv >= (8 << 8) ? 32767 : lv * 32767 / (8 << 8);
            } else {
                uint32_t p = s->ph + 16u * rate - 0x40000000u;
                m[0] = fx_cos14(p) * 2;
                m[1] = src == FS_STEREO ? fx_cos14(p + woff) * 2 : m[0];
            }
            for (c = 0; c < 2u; c++) {
                svf_coef_t t;
                svf_coef(&t, cn + (int32_t)(((int64_t)m[c] * dep) >> 15), res, type);
                t.nl = 1;                       /* (the resonance driven: a loud mix squashes the peak) */
                if (!s->c[c].g || t.p4 != s->c[c].p4) {   /* (fresh, or the slope changed: start there) */
                    s->c[c] = t;
                    for (k = 0; k < 5u; k++) s->d[c][k] = 0;
                    for (k = 4; k < 8u; k++) s->st[c][k] = 0;
                } else {
                    svf_coef_t *u = &s->c[c];
                    s->d[c][0] = (t.g - u->g) / 16; s->d[c][1] = (t.a1 - u->a1) / 16; s->d[c][2] = (t.a2 - u->a2) / 16;
                    s->d[c][3] = (t.pa1 - u->pa1) / 16; s->d[c][4] = (t.pa2 - u->pa2) / 16;
                    u->cx = t.cx; u->cl = t.cl; u->cb = t.cb; u->px = t.px; u->pl = t.pl; u->pb = t.pb; u->nl = 1;
                }
            }
        }
        if (src != FS_ENV) s->ph += rate;
        for (c = 0; c < 2u; c++) {
            svf_coef_t *u = &s->c[c];
            int32_t x = c ? R : L, y;
            u->g += s->d[c][0]; u->a1 += s->d[c][1]; u->a2 += s->d[c][2]; u->pa1 += s->d[c][3]; u->pa2 += s->d[c][4];
            y = ff_tick(s->st[c], u, x > SVF_CLIP ? SVF_CLIP : x < -SVF_CLIP ? -SVF_CLIP : x);
            out[2u * i + c] = (x * dry + y * wet + 2048) >> 12;
        }
    }
}
#endif
