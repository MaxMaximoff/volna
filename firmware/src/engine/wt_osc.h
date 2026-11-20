/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna engine: the wavetable oscillator (stage 1).
 *
 * A table is WT_WAVES waves of WT_LEN samples, int16, each row with one guard sample (row[WT_LEN] = row[0]).
 * The phase is 32 bits over one cycle (the top 7 bits pick the sample, the next 15 the fraction).
 * The position in the table is Q16: the wave index in the high bits, the blend towards the next wave in the low
 * 16 bits, 0 .. WT_POS_MAX. A sample reads 2 x 2 neighbours and blends them: linear between samples, then
 * linear between the two waves. Integers only; the products stay under 2^31.
 *
 * Mip-maps (stage 8, tools/wt_mip.py): a table holds each wave six times, band-limited for higher and higher notes
 * (WT_LV_*): level 0 the wave itself (128 samples, up to 63 harmonics), then 31, 15, 7, 3 and 1 harmonics, each at
 * 4 samples or more a cycle of its top harmonic (128, 64, 32, 16, 16 samples). The oscillator takes the first level
 * whose top harmonic stays under 24 kHz at its phase step (wt_level): what folds back from above 22.05 kHz lands
 * above 20 kHz. A table is WT_WORDS int16: the 64 rows of level 0, then of level 1, ... */
#ifndef VOLNA_WT_OSC_H
#define VOLNA_WT_OSC_H
#include <stdint.h>

#define WT_WAVES 64u
#define WT_LEN 128u
#define WT_POS_MAX ((WT_WAVES - 1u) << 16)

typedef const int16_t wt_row_t[WT_LEN + 1u];

#define WT_NLV 6u
#define WT_WORDS 24960u
static const uint16_t WT_LV_OFS[WT_NLV] = {0, 8256, 16512, 20672, 22784, 23872};   /* where each level starts */
static const uint8_t WT_LV_SH[WT_NLV] = {25, 25, 26, 27, 28, 28};   /* 32 - log2(its samples): the phase's shift */
static const uint32_t WT_LV_THR[WT_NLV] = {     /* a step under this: the level's top harmonic under 24 kHz */
    37101542u, 75399908u, 155826477u, 333913881u, 779132389u, 2337397167u};

static inline uint32_t wt_level(uint32_t step)  /* the level for a phase step (a few compares) */
{
    uint32_t k = 0;
    while (k + 1u < WT_NLV && step >= WT_LV_THR[k]) k++;
    return k;
}
/* one sample of level data d (a table + WT_LV_OFS[k]), sh its WT_LV_SH, st its row (samples + 1) */
static inline int32_t wt_sample_lv(const int16_t *d, uint32_t sh, uint32_t st, uint32_t ph, uint32_t pos)
{
    uint32_t i = ph >> sh, w = pos >> 16, w1 = w + 1u < WT_WAVES ? w + 1u : w;
    int32_t f = (int32_t)((ph >> (sh - 15u)) & 0x7FFFu);  /* between samples, Q15 */
    int32_t wf = (int32_t)((pos >> 1) & 0x7FFFu);         /* between waves, Q15 */
    const int16_t *ra = d + w * st, *rb = d + w1 * st;
    int32_t a = ra[i] + (((ra[i + 1u] - ra[i]) * f) >> 15);
    int32_t b = rb[i] + (((rb[i + 1u] - rb[i]) * f) >> 15);
    return a + (((b - a) * wf) >> 15);
}

/* one sample, -32767..32767 at most (the table's own peak) */
static inline int32_t wt_sample(const wt_row_t *tab, uint32_t ph, uint32_t pos)
{
    uint32_t i = ph >> 25, w = pos >> 16, w1 = w + 1u < WT_WAVES ? w + 1u : w;
    int32_t f = (int32_t)((ph >> 10) & 0x7FFFu);           /* between samples, Q15 */
    int32_t wf = (int32_t)((pos >> 1) & 0x7FFFu);          /* between waves, Q15 */
    int32_t a0 = tab[w][i], a1 = tab[w][i + 1u], b0 = tab[w1][i], b1 = tab[w1][i + 1u];
    int32_t a = a0 + (((a1 - a0) * f) >> 15);
    int32_t b = b0 + (((b1 - b0) * f) >> 15);
    return a + (((b - a) * wf) >> 15);
}

/* the position glides towards its target, a block at a time: no steps you can hear when a knob turns a detent.
 * Each block covers a quarter of the way left (at least 1/64 of a wave); the caller ramps across the block. */
static inline uint32_t wt_pos_glide(uint32_t now, uint32_t target)
{
    int32_t d = (int32_t)target - (int32_t)now, step = d / 4;
    if (d > 0 && step < 1024) step = d < 1024 ? d : 1024;
    if (d < 0 && step > -1024) step = d > -1024 ? d : -1024;
    return (uint32_t)((int32_t)now + step);
}
#endif
