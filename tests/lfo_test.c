/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna stage 4a: the LFOs and the matrix's arithmetic (engine/mod.h) on the host.
 *   1. the rates: RATE 0, 64, 127 within 0.5 % of 0.02 Hz x 2500^(x/127); synced: 120 BPM a quarter = 2 Hz,
 *      a sixteenth = 8 Hz;
 *   2. the shapes over a cycle: SINE within 2.5 % of sin (from 0, rising), TRI, SQR, SAW rising, all within
 *      +-32767; S&H holds a cycle, then changes; RAND glides (no jump bigger than a step's share);
 *   3. KEY: lfo_start puts the phase at PHASE;
 *   4. mod_pitch_q20: 2^(n/12) within 1 cent from -48 to +48 semitones; mod_scale at the ends.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/lfo_test tests/lfo_test.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "engine/mod.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint8_t TICKS[14] = {96, 48, 36, 32, 24, 18, 16, 12, 9, 8, 6, 4, 3, 2};
static double hz_of(uint32_t inc) { return inc * 44100.0 / 16.0 / 4294967296.0; }

int main(void)
{
    lfo_set_t s = {LS_SINE, 0, 0, 0, 0, 0};
    lfo_t l = {0, 0, 0, 0};
    uint32_t x, i, inc;
    double worst = 0;
    int32_t v, prev, lo, hi;

    for (x = 0; x < 128u; x += 64u) {           /* 1 */
        double want = 0.02 * pow(2500.0, (x > 127u ? 127u : x) / 127.0), got;
        s.rate = (uint8_t)(x > 127u ? 127u : x);
        got = hz_of(lfo_inc(&s, 120, TICKS, 14));
        CHECK(fabs(got / want - 1) < 0.005, "RATE %u: %.4f Hz, want %.4f", s.rate, got, want);
    }
    s.rate = 127;
    CHECK(fabs(hz_of(lfo_inc(&s, 120, TICKS, 14)) / 50.0 - 1) < 0.005, "RATE 127: %.3f Hz", hz_of(lfo_inc(&s, 120, TICKS, 14)));
    for (x = 0; x < 128u; x++) {                /* the screen's Hz x 100: rounded (RATE 0: 0.02, RATE 26: 0.10) */
        double want = hz_of(LFO_INC[x]) * 100;
        CHECK(fabs(lfo_hz100(LFO_INC[x]) - want) <= 0.6, "RATE %u shown as %u / 100 Hz, is %.3f", x,
              lfo_hz100(LFO_INC[x]), want / 100);
    }
    CHECK(lfo_hz100(LFO_INC[0]) == 2 && lfo_hz100(LFO_INC[26]) == 10, "RATE 0, 26: %u, %u / 100 Hz",
          lfo_hz100(LFO_INC[0]), lfo_hz100(LFO_INC[26]));
    s.sync = 1; s.rate = 4;                     /* 1/4 at 120 BPM */
    CHECK(fabs(hz_of(lfo_inc(&s, 120, TICKS, 14)) - 2.0) < 0.01, "sync 1/4: %.4f Hz", hz_of(lfo_inc(&s, 120, TICKS, 14)));
    s.rate = 10;                                /* 1/16 */
    CHECK(fabs(hz_of(lfo_inc(&s, 120, TICKS, 14)) - 8.0) < 0.04, "sync 1/16: %.4f Hz", hz_of(lfo_inc(&s, 120, TICKS, 14)));

    inc = 1u << 22;                             /* 2 a cycle: 1024 steps */
    l.ph = 0;                                   /* 2: SINE */
    for (i = 0; i < 1024u; i++) {
        double ref = 32767.0 * sin(2 * M_PI * (i + 1) / 1024.0);
        v = lfo_step(&l, LS_SINE, inc);
        if (fabs(v - ref) / 32767.0 > worst) worst = fabs(v - ref) / 32767.0;
    }
    printf("sine: within %.2f %%\n", worst * 100);
    CHECK(worst < 0.025, "sine off by %.2f %%", worst * 100);
    {
        static const uint32_t SH[3] = {LS_TRI, LS_SQR, LS_SAW};
        for (x = 0; x < 3u; x++) {
            int ok = 1;
            l.ph = 0; lo = 99999; hi = -99999; prev = -99999;
            for (i = 0; i < 1023u; i++) {
                v = lfo_step(&l, SH[x], inc);
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                if (SH[x] == LS_SAW && prev != -99999 && v < prev) ok = 0;
                prev = v;
            }
            CHECK(lo >= -32767 && hi <= 32767 && hi - lo > 64000 && ok, "shape %u: %d .. %d%s", SH[x], lo, hi,
                  ok ? "" : ", the saw falls");
        }
    }
    {                                           /* S&H, RAND */
        int32_t first, changes = 0, jump = 0;
        l.ph = 0;
        first = lfo_step(&l, LS_SH, inc);
        for (i = 1; i < 1023u; i++) if (lfo_step(&l, LS_SH, inc) != first) changes++;
        CHECK(!changes, "S&H changed within a cycle");
        lfo_step(&l, LS_SH, inc); lfo_step(&l, LS_SH, inc);
        CHECK(lfo_step(&l, LS_SH, inc) != first, "S&H: the same level a cycle on");
        l.ph = 0; prev = lfo_step(&l, LS_RAND, inc);
        for (i = 0; i < 5000u; i++) {
            v = lfo_step(&l, LS_RAND, inc);
            if (abs(v - prev) > jump) jump = abs(v - prev);
            prev = v;
        }
        CHECK(jump <= 130, "RAND jumps by %d in a step (a glide moves 65534 / 1024 at most)", jump);
    }
    lfo_start(&l, 64);                          /* 3 */
    CHECK(l.ph == 0x80000000u, "PHASE 64: %08X", l.ph);

    worst = 0;                                  /* 4 */
    for (v = -48 * 256; v <= 48 * 256; v += 7) {
        double got = mod_pitch_q20(v) / 1048576.0, want = pow(2.0, v / 256.0 / 12.0), c = 1200 * log2(got / want);
        if (fabs(c) > worst) worst = fabs(c);
    }
    printf("pitch: within %.2f cents\n", worst);
    CHECK(worst < 1.0, "mod_pitch_q20 off by %.2f cents", worst);
    CHECK(mod_scale(MD_PITCH, 63) == 12 * 256 && mod_scale(MD_PITCH, -63) == -12 * 256 && mod_scale(MD_CUT, 63) == 64 * 256
          && mod_scale(MD_W1, -63) == -32 * 256 && mod_scale(MD_VOL, 63) == 127 && mod_scale(MD_OFF, 63) == 0,
          "mod_scale ends");
    printf(fails ? "lfo: %d FAILED\n" : "lfo: all passed\n", fails);
    return fails != 0;
}
