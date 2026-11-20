/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna stage 1: the wavetable oscillator on the host.
 *   1. wave 0 is a sine: within 0.5 % of full scale;
 *   2. the table ends in a sawtooth-like wave (much richer than the sine: more sample-to-sample movement);
 *   3. a sweep 0 -> 63 -> 0 with the glide and the per-block ramp of app_audio never steps: the biggest jump between
 *      two samples stays within what the waves themselves do at that pitch, plus a small margin;
 *   4. the position never leaves 0 .. WT_POS_MAX.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/wt_osc_test tests/wt_osc_test.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "engine/wt_osc.h"
#include "engine/pitch.h"
#include "wt_public.h"
#define WT_BASIC ((const wt_row_t *)WT_PUB[0])   /* (level 0 of BASIC: the stage 1 table) */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    uint32_t i, w, ph, inc = 25599394u;         /* C4 at 44.1 kHz: 261.63 * 2^32 / 44100 */
    int32_t worst = 0, max_static = 0, prev;
    double err = 0;

    for (i = 0; i < 4096u; i++) {               /* 1 */
        uint32_t p = i * (1u << 20);
        double ref = 30000.0 * sin(2.0 * M_PI * (double)p / 4294967296.0);
        double d = fabs(wt_sample(WT_BASIC, p, 0) - ref);
        if (d > err) err = d;
    }
    CHECK(err < 32767 * 0.005, "wave 0 is not a sine: max error %.0f", err);

    for (w = 0; w < WT_WAVES; w++) {            /* the biggest step any wave makes at C4 */
        prev = wt_sample(WT_BASIC, 0, w << 16);
        for (ph = inc, i = 0; i < 2000u; i++, ph += inc) {
            int32_t s = wt_sample(WT_BASIC, ph, w << 16), d = abs(s - prev);
            if (d > max_static) max_static = d;
            prev = s;
        }
    }
    {                                           /* 2: the saw's edge: a step far steeper than the sine ever makes */
        int32_t d0 = 0, d63 = 0, s0p = wt_sample(WT_BASIC, 0, 0), s63p = wt_sample(WT_BASIC, 0, WT_POS_MAX);
        for (ph = inc, i = 0; i < 2000u; i++, ph += inc) {
            int32_t a = wt_sample(WT_BASIC, ph, 0), b = wt_sample(WT_BASIC, ph, WT_POS_MAX);
            if (abs(a - s0p) > d0) d0 = abs(a - s0p);
            if (abs(b - s63p) > d63) d63 = abs(b - s63p);
            s0p = a; s63p = b;
        }
        CHECK(d63 > 8 * d0, "wave 63 has no sawtooth edge (biggest step %d vs the sine's %d)", d63, d0);
    }

    {                                           /* 3, 4: app_audio's loop, blocks of 256, the knob jumping 8 waves */
        uint32_t now = 0, target = 0, pos, blk, n = 256u;
        ph = 0;
        prev = wt_sample(WT_BASIC, 0, 0);
        for (blk = 0; blk < 2000u; blk++) {
            uint32_t p1;
            int32_t dp;
            if (blk % 50u == 0u) target = blk < 900u ? (target + (8u << 16) > WT_POS_MAX ? WT_POS_MAX : target + (8u << 16))
                                                     : (target < (8u << 16) ? 0u : target - (8u << 16));
            p1 = wt_pos_glide(now, target);
            dp = ((int32_t)p1 - (int32_t)now) / (int32_t)n;
            pos = now;
            now = p1;
            for (i = 0; i < n; i++) {
                int32_t s, d;
                ph += inc;
                pos = (uint32_t)((int32_t)pos + dp);
                CHECK(pos <= WT_POS_MAX, "position out of range: %u", pos);
                if (pos > WT_POS_MAX) return 1;
                s = wt_sample(WT_BASIC, ph, pos);
                d = abs(s - prev);
                if (d > worst) worst = d;
                prev = s;
            }
        }
        CHECK(now == 0, "the sweep did not come back to wave 0 (%u)", now);
    }
    CHECK(worst <= max_static + max_static / 20, "the sweep steps: %d against %d in the waves", worst, max_static);

    {                                           /* 5: the pitch offsets, -36..+36 semitones and -50..+50 cents */
        int32_t sm, c;
        double worst_c = 0;
        for (sm = -36; sm <= 36; sm++)
            for (c = -50; c <= 50; c++) {
                double got = pitch_mul_q20(sm, c) / 1048576.0, want = pow(2.0, (sm * 100 + c) / 1200.0);
                double cents_off = fabs(1200.0 * log2(got / want));
                if (cents_off > worst_c) worst_c = cents_off;
            }
        CHECK(worst_c < 0.1, "pitch offsets off by %.3f cents", worst_c);
        CHECK(pitch_mul_q20(0, 0) == PITCH_ONE, "unison is not 1 << 20");
        CHECK(pitch_mul_q20(12, 0) == 2u * PITCH_ONE && pitch_mul_q20(-12, 0) == PITCH_ONE / 2u, "octaves are not exact");
        printf("pitch offsets: worst %.4f cents\n", worst_c);
    }

    printf("wave 0 vs sine: max error %.0f; biggest step: waves %d, sweep %d\n", err, max_static, worst);
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("wt_osc: OK\n");
    return 0;
}
