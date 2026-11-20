/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna stage 8: the wavetables' mip-maps (engine/wt_osc.h, tools/wt_mip.py) on the host.
 *   1. the layout: each level where WT_LV_OFS says, the table WT_WORDS long; the thresholds what they claim
 *      (the level's top harmonic at 24 kHz);
 *   2. every table (Volna's own and the local ones), every level, every wave: the guard sample, the peak within
 *      30000 (wt_sample's products), nothing above the level's top harmonic but int16 rounding (a DFT of the row);
 *   3. the point of it: a sawtooth (BASIC wave 63) at A4, C6, C7, C8 aliases far less through the level wt_level
 *      picks than through level 0 alone (the energy off the harmonics, against the harmonics').
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/mip_test tests/mip_test.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "engine/wt_osc.h"
#include "wt_public.h"
#include "wt_local.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static const uint32_t TOPH[WT_NLV] = {63, 31, 15, 7, 3, 1};

static void table(const char *name, const int16_t *t)
{
    uint32_t lv, w, i, bad = 0;
    int32_t peak = 0;
    double worst = 0;
    for (lv = 0; lv < WT_NLV; lv++) {
        uint32_t n = 1u << (32u - WT_LV_SH[lv]), st = n + 1u;
        for (w = 0; w < WT_WAVES; w++) {
            const int16_t *r = t + WT_LV_OFS[lv] + w * st;
            double tot = 0, hi = 0;
            uint32_t h;
            if (r[n] != r[0]) bad++;
            for (i = 0; i <= n; i++) if (abs(r[i]) > peak) peak = abs(r[i]);
            if (!lv) continue;
            for (i = 0; i < n; i++) tot += (double)r[i] * r[i];
            for (h = TOPH[lv] + 1u; h <= n / 2u; h++) {   /* the energy above the top harmonic */
                double c = 0, s = 0;
                for (i = 0; i < n; i++) { c += r[i] * cos(2 * M_PI * h * i / n); s += r[i] * sin(2 * M_PI * h * i / n); }
                hi += (c * c + s * s) * (h == n / 2u ? 1.0 : 2.0) / n;
            }
            if (sqrt(hi / n) > worst) worst = sqrt(hi / n);   /* (rms above the cut, in LSB: int16 rounding is ~0.3) */
        }
    }
    CHECK(!bad, "%s: %u rows without their guard sample", name, bad);
    CHECK(peak <= 30000 && peak >= 29000, "%s: the peak %d (want <= 30000, and near it)", name, peak);
    CHECK(worst < 1.0, "%s: above a level's top harmonic: %.2f LSB rms (want under 1: rounding only)", name, worst);
    printf("%-12s peak %d, above the cut at most %.2f LSB rms\n", name, peak, worst);
}

static double alias_db(uint32_t step, int mip)  /* BASIC wave 63, a second: off-harmonic energy / harmonic, dB */
{
    static double y[44100];
    double f = step * 44100.0 / 4294967296.0, tot = 0, har = 0;
    uint32_t lv = mip ? wt_level(step) : 0u, n, h, ph = 0, st = (1u << (32u - WT_LV_SH[lv])) + 1u;
    for (n = 0; n < 44100u; n++, ph += step) {
        y[n] = wt_sample_lv(WT_PUB[0] + WT_LV_OFS[lv], WT_LV_SH[lv], st, ph, WT_POS_MAX);
        tot += y[n] * y[n];
    }
    for (h = 1; h * f < 22050.0; h++) {
        double c = 0, s = 0;
        for (n = 0; n < 44100u; n++) { c += y[n] * cos(2 * M_PI * h * f * n / 44100.0); s += y[n] * sin(2 * M_PI * h * f * n / 44100.0); }
        har += 2 * (c * c + s * s) / 44100.0;
    }
    return 10 * log10((tot - har) / har);
}

int main(void)
{
    uint32_t lv, t;
    for (lv = 0; lv + 1u < WT_NLV; lv++) {      /* 1 */
        uint32_t n = 1u << (32u - WT_LV_SH[lv]);
        CHECK(WT_LV_OFS[lv + 1u] == WT_LV_OFS[lv] + WT_WAVES * (n + 1u), "level %u: the next one starts at %u", lv, WT_LV_OFS[lv + 1u]);
    }
    CHECK(WT_WORDS == WT_LV_OFS[WT_NLV - 1u] + WT_WAVES * ((1u << (32u - WT_LV_SH[WT_NLV - 1u])) + 1u), "WT_WORDS");
    for (lv = 0; lv < WT_NLV; lv++) {
        double want = 4294967296.0 * 24000.0 / 44100.0 / TOPH[lv];
        CHECK(fabs(WT_LV_THR[lv] - want) < 2.0, "level %u: threshold %u, want %.0f", lv, WT_LV_THR[lv], want);
        CHECK((1u << (32u - WT_LV_SH[lv])) >= (lv ? 4u * (TOPH[lv] + 1u) : 128u) || lv == 0, "level %u: under 4 samples a cycle", lv);
    }
    for (t = 0; t < WT_PUB_N; t++) table(WT_PUB_NAME[t], WT_PUB[t]);   /* 2 */
#if WT_LOCAL_N
    for (t = 0; t < WT_LOCAL_N; t++) table(WT_LOCAL_NAME[t], WT_LOCAL[t]);
#endif
    {                                           /* 3 */
        static const double F[4] = {440, 1047, 2093, 4186};
        for (t = 0; t < 4u; t++) {
            uint32_t step = (uint32_t)(F[t] * 4294967296.0 / 44100.0);
            double a0 = alias_db(step, 0), am = alias_db(step, 1);
            printf("saw at %4.0f Hz: aliasing %6.1f dB through level 0, %6.1f dB through level %u\n", F[t], a0, am, wt_level(step));
            CHECK(am < -33.0 && am < a0 - 8.0, "saw at %.0f Hz: %.1f dB with the mip-maps (level 0: %.1f)", F[t], am, a0);
        }
    }
    printf(fails ? "mip: %d FAILED\n" : "mip: all passed\n", fails);
    return fails != 0;
}
