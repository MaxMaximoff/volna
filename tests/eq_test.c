/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: the EQ (engine/eq.h) on the host.
 *   coefficients: over every band's notes (volna.c EQ_NLO .. EQ_NHI), every gain and MID's every Q, a1 is
 *   1 / (1 + g (g + k)) as doubles make it (svf_a1's 32-bit sum never overflows);
 *   response: a sine through each band, its level against the analog shelf / bell at the prewarped frequency (the
 *   bilinear transform's: x = tan(pi f / fs) / tan(pi f0 / fs)), within 0.25 dB: below, at and above the frequency, at
 *   the ranges' ends;
 *   the screen's curve (eq_db_q8) against the same formulas in doubles, within 0.25 dB over +-5 octaves;
 *   all bands at 0 dB: the output is the input, to the bit; a band switched off clears its state;
 *   bounded: full-scale noise through +12 dB on all three (the top notes, the narrowest bell), then silence: the tail
 *   ends at |x| <= 1.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/eq_test tests/eq_test.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/eq.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
#define FS 44100.0
static const int NLO[EQ_NB] = {24, 36, 72}, NHI[EQ_NB] = {84, 120, 128};   /* (volna.c EQ_NLO, EQ_NHI) */
static const char *const BN[EQ_NB] = {"LOW", "MID", "HIGH"};

static double hz(int n) { return 440.0 * pow(2.0, (n - 69) / 12.0); }
static double qof(int r) { return 0.5 * pow(50.0, r / 127.0); }
static double ref_db(int b, int db, int r, double x)   /* the analog band at x = f / f0, dB */
{
    double A = pow(10.0, db / 40.0), xx = x * x, num, den;
    if (b == EQ_MID) {
        double Q = qof(r), t = (1 - xx) * (1 - xx);
        return 10 * log10((t + A * A / (Q * Q) * xx) / (t + xx / (A * A * Q * Q)));
    }
    num = (A - xx) * (A - xx) + 2 * A * xx;
    den = (1 - A * xx) * (1 - A * xx) + 2 * A * xx;
    if (b == EQ_HIGH) { double s = num; num = den; den = s; }
    return 20 * log10(A) + 10 * log10(num / den);
}

static eq_coef_t C;
static eq_state_t S;
static void set_band(int b, int n, int db, int r)
{
    memset(&C, 0, sizeof C);
    eq_band_coef(&C.b[b], (uint32_t)b, n << 8, db, (uint32_t)r);
    C.on = db ? 1 << b : 0;
    memset(&S, 0, sizeof S);
}
static double sine_db(double f)                 /* a sine of f through C: its level out against in, dB (measured over
                                                   whole cycles, after 0.25 s) */
{
    enum { SKIP = 11025, BLK = 256 };
    static int32_t buf[2 * BLK];
    double re = 0, im = 0, a = 8000;
    int t, i, len = (int)lround(floor(11025.0 * f / FS + 1) * FS / f), n = SKIP + len;
    for (t = 0; t < n; t += BLK) {
        for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = (int32_t)lround(a * sin(2 * M_PI * f * (t + i) / FS));
        eq_run(&S, &C, buf, BLK);
        for (i = 0; i < BLK; i++)
            if (t + i >= SKIP && t + i < n) {
                re += buf[2 * i] * cos(2 * M_PI * f * (t + i) / FS);
                im += buf[2 * i] * sin(2 * M_PI * f * (t + i) / FS);
            }
    }
    return 20 * log10(hypot(re, im) * 2 / len / a);
}

int main(void)
{
    int b, n, db, r, i, worst_db = 0;
    double worst = 0;

    /* the coefficients over the whole ranges */
    for (b = 0; b < EQ_NB; b++)
        for (n = NLO[b]; n <= NHI[b]; n++)
            for (db = -EQ_DBMAX; db <= EQ_DBMAX; db++)
                for (r = 0; r <= (b == EQ_MID ? 90 : 0); r++) {
                    eq_band_t c;
                    double A = pow(10.0, db / 40.0), g = tan(M_PI * hz(n) / FS), k = M_SQRT2, e;
                    if (b == EQ_LOW) g /= sqrt(A);
                    else if (b == EQ_HIGH) g *= sqrt(A);
                    else k = 1.0 / (qof(r) * A);
                    eq_band_coef(&c, (uint32_t)b, n << 8, db, (uint32_t)r);
                    e = fabs(c.a1 / 1073741824.0 * (1 + g * (g + k)) - 1);
                    if (e > worst) worst = e;
                }
    CHECK(worst < 2e-4, "coefficients: a1 off by %.2e of 1 / (1 + g (g + k))", worst);
    printf("eq: a1 within %.1e over every note, gain and Q\n", worst);

    /* the response, sines through each band */
    {
        static const int G[] = {-12, -5, 6, 12};
        double dmax = 0;
        for (b = 0; b < EQ_NB; b++) {
            int ns[3] = {NLO[b], (NLO[b] + NHI[b]) / 2, NHI[b]};
            for (i = 0; i < 3; i++)
                for (db = 0; db < 4; db++) {
                    int rs[3] = {0, 11, 90}, nr = b == EQ_MID ? 3 : 1, j, m;
                    for (j = 0; j < nr; j++) {
                        double f0 = hz(ns[i]), mult[5] = {0.125, 0.5, 1, 2, 8};
                        set_band(b, ns[i], G[db], rs[j]);
                        for (m = 0; m < 5; m++) {
                            double f = f0 * mult[m], got, want;
                            if (f > 18000 || f < 15) continue;
                            got = sine_db(f);
                            want = ref_db(b, G[db], rs[j], tan(M_PI * f / FS) / tan(M_PI * f0 / FS));
                            if (fabs(got - want) > dmax) dmax = fabs(got - want);
                            CHECK(fabs(got - want) < 0.25, "%s %+d dB at note %d (Q %.2f), %.0f Hz: %.2f dB, want %.2f",
                                  BN[b], G[db], ns[i], qof(rs[j]), f, got, want);
                        }
                    }
                }
        }
        printf("eq: sines through the bands within %.3f dB of the analog response, prewarped\n", dmax);
    }

    /* the screen's curve */
    {
        double dmax = 0;
        for (b = 0; b < EQ_NB; b++)
            for (db = -EQ_DBMAX; db <= EQ_DBMAX; db += 3)
                for (r = 0; r <= 90; r += 30)
                    for (i = -1320; i <= 1320; i += 33) {
                        double got = eq_db_q8((uint32_t)b, db, (uint32_t)r, i) / 256.0;
                        double want = db ? ref_db(b, db, r, pow(2.0, i / 264.0)) : 0;
                        if (fabs(got - want) > dmax) { dmax = fabs(got - want); worst_db = db; }
                        CHECK(fabs(got - want) < 0.25, "screen: %s %+d dB Q %.2f at %+.2f oct: %.2f, want %.2f",
                              BN[b], db, qof(r), i / 264.0, got, want);
                    }
        printf("eq: the screen's curve within %.3f dB (worst at %+d dB)\n", dmax, worst_db);
    }

    /* all off: the input as it is; a band off: its state cleared */
    {
        static int32_t x[512], y[512];
        uint32_t k;
        for (k = 0; k < 512; k++) x[k] = y[k] = (int32_t)(k * 7919u % 65535u) - 32767;
        set_band(EQ_MID, 83, 0, 11);
        eq_run(&S, &C, y, 256);
        CHECK(!memcmp(x, y, sizeof x), "all at 0 dB: the output is not the input");
        set_band(EQ_MID, 83, 12, 11);
        eq_run(&S, &C, y, 256);
        C.on = 0;
        eq_run(&S, &C, y, 1);
        for (k = 0; k < 8u; k++) CHECK(!(&S.ic[EQ_MID][0][0])[k], "a band switched off keeps its state (%u)", k);
        printf("eq: all at 0 dB the input to the bit; a band off cleared\n");
    }

    /* bounded, and the tail ends */
    {
        static int32_t buf[512];
        uint32_t rnd = 12345u, t, k;
        int32_t pk = 0, tail = 0;
        memset(&C, 0, sizeof C);
        memset(&S, 0, sizeof S);
        eq_band_coef(&C.b[EQ_LOW], EQ_LOW, 84 << 8, 12, 0);
        eq_band_coef(&C.b[EQ_MID], EQ_MID, 120 << 8, 12, 90);
        eq_band_coef(&C.b[EQ_HIGH], EQ_HIGH, 128 << 8, 12, 0);
        C.on = 7;
        for (t = 0; t < 10u * 44100u; t += 256u) {
            for (k = 0; k < 512u; k++) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; buf[k] = (int32_t)(rnd >> 16) - 32768; }
            eq_run(&S, &C, buf, 256);
            for (k = 0; k < 512u; k++) if (abs(buf[k]) > pk) pk = abs(buf[k]);
        }
        for (t = 0; t < 2u * 44100u; t += 256u) {
            memset(buf, 0, sizeof buf);
            eq_run(&S, &C, buf, 256);
            if (t >= 44100u) for (k = 0; k < 512u; k++) if (abs(buf[k]) > tail) tail = abs(buf[k]);
        }
        CHECK(pk <= EQ_IN_CLIP >> SVF_SH, "noise: a peak of %d past the clamp", pk);
        CHECK(tail <= 1, "silence after the noise: still %d after a second", tail);
        printf("eq: full-scale noise at +12 dB x 3 peaks at %d, the tail ends at %d\n", pk, tail);
    }

    printf(fails ? "eq: %d FAILED\n" : "eq: all passed\n", fails);
    return fails != 0;
}
