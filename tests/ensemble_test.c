/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: ENSEMBLE (engine/fx.h) on the host.
 *   still taps: an impulse comes back at 6.5 ms, to the sample;
 *   the read between samples (Catmull-Rom): a sine in the line read at every 1/64 sample, its level at the worst
 *   fraction within 0.05 dB of Catmull-Rom's at half a sample: 0.007 dB down at 3 kHz, 0.75 at 10 kHz, 3.5 at
 *   15 kHz (a linear read: 0.2, 2.4, 6.4 dB; a moving tap sweeps through them hundreds of times a second);
 *   the bass stays dry: 60 Hz with dry and wet full, within -1 .. +2 dB of dry (without the high pass the delay
 *   would take ~3 dB off it);
 *   it moves: a 1 kHz sine, dry and wet full, the sum beats by more than 6 dB; vibrato alone (no chorus) beats too
 *   (still taps: not at all);
 *   no steps: the largest step between samples no more than x 1.3 of the sine's own (the taps ramped);
 *   WIDTH 0: the sides the same to the bit; a sixth of a cycle: they differ;
 *   bounded: full-scale noise at the top depths stays within x 2 of full scale; then silence: all 0 within 0.1 s.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/ensemble_test tests/ensemble_test.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/fx.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
#define FS 44100.0
#define BLK 256
static int16_t mem[FX_MEM];
static fx_state_t S;
static int32_t P_ds, P_dv, P_gt, P_dry, P_wet;
static uint32_t P_is, P_iv, P_woff;

static uint32_t inc(double hz) { return (uint32_t)(hz / FS * 4294967296.0); }
static void setp(double ms_s, double hz_s, double ms_v, double hz_v, uint32_t woff, int tone_note, int32_t dry, int32_t wet)
{
    memset(&S, 0, sizeof S);
    memset(mem, 0, sizeof mem);
    P_ds = (int32_t)lround(ms_s * 44.1 * 65536); P_dv = (int32_t)lround(ms_v * 44.1 * 65536);
    P_is = inc(hz_s); P_iv = inc(hz_v); P_woff = woff; P_gt = (int32_t)PH_G[2 * tone_note]; P_dry = dry; P_wet = wet;
}
static void run(int32_t *buf, uint32_t n) { fx_ensemble(&S.ens, mem, buf, n, P_ds, P_dv, P_is, P_iv, P_woff, P_gt, P_dry, P_wet); }

/* a sine of f through, 20 ms windows' levels (left) from skip on: their spread in dB; the largest step against
   the sine's own; the mean level against the input's */
static void sine(double f, double secs, double *spread, double *step, double *mean_db)
{
    static int32_t buf[2 * BLK];
    double a = 8000, lo = 1e30, hi = 0, acc = 0, worst = 0, tot = 0;
    int t, i, n = (int)(secs * FS), win = 882, cnt = 0, nw = 0, prev = 0;
    for (t = 0; t < n; t += BLK) {
        for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = (int32_t)lround(a * sin(2 * M_PI * f * (t + i) / FS));
        run(buf, BLK);
        for (i = 0; i < BLK; i++) {
            if (t + i < 4410) { prev = buf[2 * i]; continue; }
            acc += (double)buf[2 * i] * buf[2 * i];
            tot += (double)buf[2 * i] * buf[2 * i]; nw++;
            if (fabs((double)(buf[2 * i] - prev)) > worst) worst = fabs((double)(buf[2 * i] - prev));
            prev = buf[2 * i];
            if (++cnt == win) {
                double r = sqrt(acc / win);
                if (r < lo) lo = r;
                if (r > hi) hi = r;
                acc = 0; cnt = 0;
            }
        }
    }
    *spread = 20 * log10(hi / lo);
    *step = worst / (2 * M_PI * f / FS * a * (hi / (a / sqrt(2))));   /* (against the loudest window's sine) */
    *mean_db = 20 * log10(sqrt(tot / nw) / (a / sqrt(2)));
}

int main(void)
{
    double sp, st, mdb;

    /* still taps: the impulse at 6.5 ms */
    {
        static int32_t buf[2 * 512];
        int i, at = -1, pk = 0;
        setp(0, 0, 0, 0, 0, 132, 0, 4096);
        memset(buf, 0, sizeof buf);
        buf[0] = buf[1] = 30000;
        run(buf, 512);
        for (i = 0; i < 512; i++) if (abs(buf[2 * i]) > pk) { pk = abs(buf[2 * i]); at = i; }
        CHECK(at == 287, "still: the impulse back at %d, not 287", at);
        printf("ensemble: still taps, the impulse back at %d samples (6.5 ms)\n", at);
    }

    /* the read between samples: a sine in the line, read at every 1/64 of a sample; its level at the worst fraction
       against the sine's (Catmull-Rom at half a sample: -0.75 dB at 10 kHz; a linear read: -2.4) and the error */
    {
        static const double F[3] = {3000, 10000, 15000};
        int fi;
        for (fi = 0; fi < 3; fi++) {
            int j, q, i;
            double worst = 0;
            for (i = 0; i < (int)FX_ELEN; i++)
                mem[2 * i] = (int16_t)lround(8000 * sin(2 * M_PI * F[fi] * i / FS));
            for (q = 0; q < 64; q++) {
                double re = 0, im = 0, lvl;
                for (j = 0; j < 441; j++) {      /* (441 positions: 100 cycles at 10 kHz, whole ones at all three) */
                    uint32_t w = 600u + (uint32_t)j;
                    int32_t dq = (300 << 16) + q * 1024;
                    double y = ens_read(mem, w, dq, 0) / 2.0, tpos = w - 300 - q / 64.0;
                    re += y * cos(2 * M_PI * F[fi] * tpos / FS);
                    im += y * sin(2 * M_PI * F[fi] * tpos / FS);
                }
                lvl = -20 * log10(hypot(re, im) * 2 / 441 / 8000);
                if (lvl > worst) worst = lvl;
            }
            {                                   /* Catmull-Rom at half a sample: (9 cos(w/2) - cos(3w/2)) / 8 */
                double wv = 2 * M_PI * F[fi] / FS, cr = -20 * log10((9 * cos(wv / 2) - cos(3 * wv / 2)) / 8);
                CHECK(fabs(worst - cr) < 0.05, "the read at %.0f Hz: down %.3f dB, Catmull-Rom's %.3f", F[fi], worst, cr);
                printf("ensemble: the read between samples at %5.0f Hz: down %.3f dB at the worst fraction (Catmull-Rom's"
                       " %.3f; linear: %.2f)\n", F[fi], worst, cr, -20 * log10(cos(wv / 2)));
            }
        }
        memset(mem, 0, sizeof mem);
    }

    /* the bass stays dry */
    setp(3, 0.6, 0.6, 6, 0, 120, 4096, 4096);
    sine(60, 3, &sp, &st, &mdb);
    CHECK(mdb > -1 && mdb < 2, "60 Hz, dry and wet full: %.2f dB against dry", mdb);
    printf("ensemble: 60 Hz with dry and wet full: %+.2f dB against dry (the bass not hollowed)\n", mdb);

    /* it moves, without steps */
    setp(3, 0.6, 0.6, 6, 0, 132, 4096, 4096);
    sine(1000, 4, &sp, &st, &mdb);
    CHECK(sp > 6, "1 kHz, chorus and vibrato: the sum beats only %.1f dB", sp);
    CHECK(st < 1.3, "1 kHz, chorus and vibrato: a step x%.2f of the sine's own", st);
    printf("ensemble: 1 kHz, dry and wet: the sum beats %.1f dB; the largest step x%.2f of the sine's\n", sp, st);
    {                                           /* vibrato alone: the three taps' swings cancel in their mean, mostly
                                                   (what is left shimmers at 3 x its rate), so less than the chorus;
                                                   against still taps, though, it moves */
        double sp0;
        setp(0, 0.6, 0, 6, 0, 132, 4096, 4096);
        sine(1000, 2, &sp0, &st, &mdb);
        setp(0, 0.6, 0.6, 6, 0, 132, 4096, 4096);
        sine(1000, 2, &sp, &st, &mdb);
        CHECK(sp0 < 0.05 && sp > 0.5, "1 kHz, vibrato alone: beats %.2f dB (still: %.3f)", sp, sp0);
        CHECK(st < 1.3, "1 kHz, vibrato alone: a step x%.2f", st);
        printf("ensemble: vibrato alone: beats %.2f dB (still taps: %.3f), step x%.2f\n", sp, sp0, st);
    }

    /* WIDTH */
    {
        static int32_t buf[2 * BLK];
        int t, i, a, same = 1, diff = 0;
        for (a = 0; a < 2; a++) {
            setp(3, 0.6, 0.6, 6, a ? 715827883u : 0u, 132, 0, 4096);
            for (t = 0; t < 44100; t += BLK) {
                for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = (int32_t)lround(8000 * sin(2 * M_PI * 700 * (t + i) / FS));
                run(buf, BLK);
                for (i = 0; i < BLK; i++) {
                    if (!a && buf[2 * i] != buf[2 * i + 1]) same = 0;
                    if (a && abs(buf[2 * i] - buf[2 * i + 1]) > 2000) diff++;
                }
            }
        }
        CHECK(same, "WIDTH 0: the sides differ");
        CHECK(diff > 4410, "WIDTH a sixth: the sides apart in only %d samples", diff);
        printf("ensemble: WIDTH 0 the sides the same; a sixth of a cycle: apart in %d samples of 44100\n", diff);
    }

    /* bounded, and the tail ends */
    {
        static int32_t buf[2 * BLK];
        uint32_t rnd = 4242u;
        int t, i, pk = 0, last = -1;
        setp(3, 2, 0.6, 10, 715827883u, 132, 4096, 4096);
        for (t = 0; t < 3 * 44100; t += BLK) {
            for (i = 0; i < 2 * BLK; i++) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; buf[i] = (int32_t)(rnd >> 16) - 32768; }
            run(buf, BLK);
            for (i = 0; i < 2 * BLK; i++) if (abs(buf[i]) > pk) pk = abs(buf[i]);
        }
        for (t = 0; t < 44100; t += BLK) {
            memset(buf, 0, sizeof buf);
            run(buf, BLK);
            for (i = 0; i < 2 * BLK; i++) if (buf[i]) last = t + i / 2;
        }
        CHECK(pk < 2 * 32768, "noise: a peak of %d", pk);
        CHECK(last < 4410, "silence after the noise: still sounding at %.3f s", last / FS);
        printf("ensemble: noise at the top depths peaks at %d; silence after it: the last non-zero at %.3f s\n", pk, last / FS);
    }

    printf(fails ? "ensemble: %d FAILED\n" : "ensemble: all passed\n", fails);
    return fails != 0;
}
