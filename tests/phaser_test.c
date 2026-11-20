/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: PHASER (engine/fx.h) on the host.
 *   the line alone (no dry, no sweep): every frequency through at 0 dB (allpass), 2 .. 12 stages, 30 Hz .. 12 kHz;
 *   dry and line half each, no feedback: the level a sine comes out at is |cos(N phi / 2)|, phi = 2 atan(t / tc)
 *   with t = tan(pi f / fs) (the bilinear transform's), within 0.2 dB where it is above -20 dB; at the first notch
 *   under -40 dB; for 2, 4, 8 and 12 stages, centres 100 Hz .. 5 kHz;
 *   feedback +-0.6 (a quiet sine: tanh about straight): the level against d + w A / (1 - fb A e^-jw) (the feedback
 *   comes a sample late), within 0.3 dB;
 *   swept (a 1.3 Hz LFO, +-2 octaves): the level of a sine swings past 20 dB (the notches go by), and no sample
 *   jumps more than a sine's own step allows (no steps in the sweep: no zipper);
 *   WIDTH 0: the sides the same to the bit; half a cycle: they differ;
 *   bounded: full-scale noise, 12 stages, feedback +-0.95, a fast sweep: the output within +-3 x full scale; then
 *   silence: every sample 0 (no feedback: within a second; +-0.95: it rings a while, as a real one, then 0); and
 *   16 settings at +-0.95 (centres 41 Hz .. 13 kHz, 2 and 12 stages): all silent 4 s after the noise.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/phaser_test tests/phaser_test.c -lm */
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/fx.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
#define FS 44100.0
#define BLK 256

static fx_state_t S;
static int32_t P_cn, P_dep, P_fb, P_dry, P_wet;
static uint32_t P_inc, P_woff, P_nst;
static void setp(int note, int dep_semi, double fb, uint32_t inc, uint32_t woff, uint32_t nst, int32_t dry, int32_t wet)
{
    memset(&S, 0, sizeof S);
    P_cn = note << 8; P_dep = dep_semi << 8; P_fb = (int32_t)lround(fb * 16384); P_inc = inc; P_woff = woff;
    P_nst = nst; P_dry = dry; P_wet = wet;
}
static void run(int32_t *buf, uint32_t n) { fx_phaser(&S.pha, buf, n, P_cn, P_dep, P_fb, P_inc, P_woff, P_nst, P_dry, P_wet); }

static double sine_db(double f, double a)       /* a sine through, its level against in (dB), over whole cycles */
{
    enum { SKIP = 8820 };
    static int32_t buf[2 * BLK];
    double re = 0, im = 0;
    int t, i, len = (int)lround(floor(8820.0 * f / FS + 1) * FS / f), n = SKIP + len;
    for (t = 0; t < n; t += BLK) {
        for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = (int32_t)lround(a * sin(2 * M_PI * f * (t + i) / FS));
        run(buf, BLK);
        for (i = 0; i < BLK; i++)
            if (t + i >= SKIP && t + i < n) {
                re += buf[2 * i] * cos(2 * M_PI * f * (t + i) / FS);
                im += buf[2 * i] * sin(2 * M_PI * f * (t + i) / FS);
            }
    }
    return 20 * log10(hypot(re, im) * 2 / len / a);
}
static double hz(int n) { return 440.0 * pow(2.0, (n - 69) / 12.0); }
static double complex line(double f, int note, int nst)   /* the stages' response, bilinear */
{
    double t = tan(M_PI * f / FS), tc = tan(M_PI * hz(note) / FS), phi = 2 * atan(t / tc);
    return cexp(-I * nst * phi);
}

int main(void)
{
    static const int NS[4] = {2, 4, 8, 12}, CN[4] = {43, 57, 81, 108};
    int a, b, k;
    double dmax = 0;

    /* the line alone: 0 dB everywhere */
    for (a = 0; a < 4; a++) {
        static const double F[6] = {30, 200, 1000, 3000, 8000, 12000};
        setp(69, 0, 0, 0, 0, (uint32_t)NS[a], 0, 4096);
        for (k = 0; k < 6; k++) {
            double got = sine_db(F[k], 8000);
            if (fabs(got) > dmax) dmax = fabs(got);
            CHECK(fabs(got) < 0.05, "allpass, %d stages, %.0f Hz: %.3f dB", NS[a], F[k], got);
        }
    }
    printf("phaser: the line alone within %.3f dB of 0 (2 .. 12 stages, 30 Hz .. 12 kHz)\n", dmax);

    /* dry and line half each: the notches where theory has them */
    dmax = 0;
    for (a = 0; a < 4; a++)
        for (b = 0; b < 4; b++) {
            double fc = hz(CN[b]), tc = tan(M_PI * fc / FS), fn = atan(tc * tan(M_PI / (2 * NS[a]))) * FS / M_PI, got;
            static const double M[5] = {0.25, 0.6, 1, 1.7, 4};
            setp(CN[b], 0, 0, 0, 0, (uint32_t)NS[a], 2048, 2048);
            for (k = 0; k < 5; k++) {
                double f = fc * M[k], want;
                if (f > 15000 || f < 20) continue;
                want = 20 * log10(cabs(0.5 + 0.5 * line(f, CN[b], NS[a])));
                if (want < -20) continue;
                got = sine_db(f, 8000);
                if (fabs(got - want) > dmax) dmax = fabs(got - want);
                CHECK(fabs(got - want) < 0.2, "%d stages at %.0f Hz, %.0f Hz: %.2f dB, want %.2f", NS[a], fc, f, got, want);
            }
            if (fn > 20 && fn < 15000) {
                got = sine_db(fn, 8000);
                CHECK(got < -40, "%d stages at %.0f Hz: the notch at %.1f Hz only %.1f dB", NS[a], fc, fn, got);
            }
        }
    printf("phaser: half and half, the level within %.3f dB of |cos(N phi / 2)|; the first notches under -40 dB\n", dmax);

    /* feedback (a quiet sine: tanh about straight) */
    dmax = 0;
    for (a = 0; a < 2; a++) {
        double fb = a ? -0.6 : 0.6;
        static const double F[5] = {150, 400, 900, 2000, 5000};
        setp(69, 0, fb, 0, 0, 4, 2048, 2048);
        for (k = 0; k < 5; k++) {
            double complex A = line(F[k], 69, 4), z1 = cexp(-I * 2 * M_PI * F[k] / FS);
            double complex fbp = z1 * (1 + z1) / 2 * (1 - z1) / (1 - PH_DCR / 32768.0 * z1);   /* (the mean, the DC
                                                   blocker) */
            double want = 20 * log10(cabs(0.5 + 0.5 * A / (1 - fb * A * fbp))), got = sine_db(F[k], 400);
            if (want < -20) continue;
            if (fabs(got - want) > dmax) dmax = fabs(got - want);
            CHECK(fabs(got - want) < 0.3, "feedback %+.1f, %.0f Hz: %.2f dB, want %.2f", fb, F[k], got, want);
        }
    }
    printf("phaser: feedback +-0.6 within %.3f dB of the theory\n", dmax);

    /* swept: the notches go by, and no steps */
    {
        static int32_t buf[2 * BLK];
        double f = 700, a8 = 8000, lo = 1e9, hi = 0, jump = 0, sj = 2 * M_PI * f / FS * a8;
        int t, i, prev = 0;
        setp(69, 24, 0, (uint32_t)(1.3 / FS * 4294967296.0), 0, 6, 2048, 2048);
        for (t = 0; t < 3 * 44100; t += BLK) {
            double e = 0;
            for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = (int32_t)lround(a8 * sin(2 * M_PI * f * (t + i) / FS));
            run(buf, BLK);
            for (i = 0; i < BLK; i++) {
                e += (double)buf[2 * i] * buf[2 * i];
                if (t + i > 4410 && fabs((double)(buf[2 * i] - prev)) > jump) jump = fabs((double)(buf[2 * i] - prev));
                prev = buf[2 * i];
            }
            e = sqrt(e / BLK);
            if (t > 4410) { if (e < lo) lo = e; if (e > hi) hi = e; }
        }
        CHECK(20 * log10(hi / lo) > 20, "swept: the level swings only %.1f dB", 20 * log10(hi / lo));
        CHECK(jump < 1.25 * sj, "swept: a step of %.0f (a sine's own: %.0f)", jump, sj);
        printf("phaser: swept, the level swings %.1f dB; the largest step %.0f (the sine's own %.0f)\n", 20 * log10(hi / lo), jump, sj);
    }

    /* WIDTH */
    {
        static int32_t buf[2 * BLK];
        int t, i, same = 1, diff = 0;
        for (a = 0; a < 2; a++) {
            setp(69, 24, 0.3, (uint32_t)(0.7 / FS * 4294967296.0), a ? 0x80000000u : 0u, 6, 2048, 2048);
            for (t = 0; t < 44100; t += BLK) {
                for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = (int32_t)lround(8000 * sin(2 * M_PI * 500 * (t + i) / FS));
                run(buf, BLK);
                for (i = 0; i < BLK; i++) {
                    if (!a && buf[2 * i] != buf[2 * i + 1]) same = 0;
                    if (a && abs(buf[2 * i] - buf[2 * i + 1]) > 1000) diff++;
                }
            }
        }
        CHECK(same, "WIDTH 0: the sides differ");
        CHECK(diff > 4410, "WIDTH half a cycle: the sides apart in only %d samples", diff);
        printf("phaser: WIDTH 0 the sides the same; half a cycle apart in %d samples of 44100\n", diff);
    }

    /* bounded, and the tail ends: no feedback, at once (a second); at +-0.95 the line rings as a real one would
       (the lowest centre, 12 stages: a long delay a loop), but it dies out to 0 */
    for (a = 0; a < 3; a++) {
        static int32_t buf[2 * BLK];
        static const double FB[3] = {0, 0.95, -0.95};
        uint32_t rnd = 777u;
        int t, i, pk = 0, zero_at = -1, pk1 = 0;
        setp(40, 36, FB[a], (uint32_t)(9.0 / FS * 4294967296.0), 0x40000000u, 12, 2048, 2048);
        for (t = 0; t < 5 * 44100; t += BLK) {
            for (i = 0; i < 2 * BLK; i++) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; buf[i] = (int32_t)(rnd >> 16) - 32768; }
            run(buf, BLK);
            for (i = 0; i < 2 * BLK; i++) if (abs(buf[i]) > pk) pk = abs(buf[i]);
        }
        P_cn = 16 << 8; P_dep = 0;              /* (the lowest centre: where a stage would stick) */
        for (t = 0; t < 60 * 44100 && zero_at < 0; t += BLK) {
            int any = 0;
            memset(buf, 0, sizeof buf);
            run(buf, BLK);
            for (i = 0; i < 2 * BLK; i++) { if (buf[i]) any = 1; if (t >= 44100 && t < 44100 + BLK && abs(buf[i]) > pk1) pk1 = abs(buf[i]); }
            if (!any && t > 0) {                /* (a block of zeros: is it for good? the next second too) */
                int u, still = 0;
                fx_pha_t keep = S.pha;
                for (u = 0; u < 44100 && !still; u += BLK) {
                    memset(buf, 0, sizeof buf);
                    run(buf, BLK);
                    for (i = 0; i < 2 * BLK; i++) if (buf[i]) still = 1;
                }
                S.pha = keep;
                if (!still) zero_at = t;
            }
        }
        CHECK(pk < 3 * 32768, "noise, feedback %+.2f: a peak of %d", FB[a], pk);
        CHECK(zero_at >= 0 && (FB[a] != 0 || zero_at < 44100), "silence after the noise (feedback %+.2f): not 0 after %.1f s",
              FB[a], zero_at / FS);
        printf("phaser: noise, 12 stages, feedback %+.2f: peak %d; silence after it: %d a second on, all 0 from %.2f s\n",
               FB[a], pk, pk1, zero_at / FS);
    }

    /* no tail that never dies: feedback +-0.95, the centre low to high, 2 and 12 stages; a second of noise, then
       5 s of silence: its last second all 0 (a quiet loop near a resonance used to keep ticking at +-1) */
    {
        static const int NT[4] = {28, 69, 110, 128}, NS2[2] = {2, 12};
        static int32_t buf[2 * BLK];
        int bad = 0, c, t, i;
        for (a = 0; a < 2; a++) for (b = 0; b < 4; b++) for (c = 0; c < 2; c++) {
            uint32_t rnd = 99u + (uint32_t)(a * 31 + b * 7 + c);
            int pk = 0;
            setp(NT[b], 0, a ? -0.95 : 0.95, 0, 0, (uint32_t)NS2[c], 2048, 2048);
            for (t = 0; t < 44100; t += BLK) {
                for (i = 0; i < 2 * BLK; i++) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; buf[i] = (int32_t)(rnd >> 16) - 32768; }
                run(buf, BLK);
            }
            for (t = 0; t < 5 * 44100; t += BLK) {
                memset(buf, 0, sizeof buf);
                run(buf, BLK);
                if (t >= 4 * 44100) for (i = 0; i < 2 * BLK; i++) if (abs(buf[i]) > pk) pk = abs(buf[i]);
            }
            if (pk) { bad++; printf("FAIL: feedback %+.2f, note %d, %d stages: %d after 4 s\n", a ? -0.95 : 0.95, NT[b], NS2[c], pk); }
        }
        fails += bad;
        printf("phaser: 16 settings at feedback +-0.95, every one silent 4 s after the noise\n");
    }

    printf(fails ? "phaser: %d FAILED\n" : "phaser: all passed\n", fails);
    return fails != 0;
}
