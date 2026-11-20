/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: LO-FI (engine/fx.h) on the host.
 *   all at rest (44.1 kHz, 16 bits, no wobble, no noise, TONE open): a 1 kHz sine through within 0.05 dB, an impulse
 *   back 2 samples later;
 *   BITS (against the sound's envelope): a sine's signal to noise (all but the fundamental) within 2 dB of
 *   6.02 N + 1.76 for 4, 6, 8, 10 bits (fractional BITS: N from the table), and the same for a sine 24 dB quieter at
 *   3 and 4 bits (against full scale it went to nothing there: the sound broke up); 3 bits: no DC (mean under 1);
 *   RATE ~4 kHz: a 1 kHz sine held: the fundamental down as a hold's sinc says (0.9 dB), its image at R - 1 kHz at
 *   the sinc's level too (-10.5 dB), within 2 dB;
 *   WOBBLE at the top: a 1 kHz sine's pitch (zero crossings, 50 ms windows) swings 2 .. 5 % from its lowest to its
 *   highest (the wow +-1.37 %, the flutter +-0.64 %); at 0: under 0.1 %;
 *   WOBBLE turned while it plays: no step past x 1.3 of the sine's own (slewed);
 *   NOISE: hiss on a playing sine (residual over 300); silence from the start: every sample 0, every setting at its
 *   top; after a loud sine: all 0 within 2 s.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/lofi_test tests/lofi_test.c -lm */
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
static int32_t P_wa, P_fa, P_nk, P_gt, P_dry, P_wet;
static uint32_t P_hinc, P_b;

/* the knobs as the firmware maps them (volna.c fx_apply) */
static void setp(int rate, int bits, int tone, int wob, int noise)
{
    memset(&S, 0, sizeof S);
    memset(mem, 0, sizeof mem);
    P_hinc = LF_INC[rate]; P_b = (uint32_t)bits; P_gt = (int32_t)PH_G[2 * (80 + tone * 52 / 127)];
    P_wa = wob * (160 << 16) / 127; P_fa = wob * (5 << 16) / 127; P_nk = noise * 9830 / 127; P_dry = 0; P_wet = 4096;
}
static void run(int32_t *buf, uint32_t n) { fx_lofi(&S.lof, mem, buf, n, P_wa, P_fa, P_hinc, P_b, P_nk, P_gt, P_dry, P_wet); }

static int32_t out_[20 * 44100];
static void play(double f, double a, int n)     /* a sine of f into out_ (left), n samples */
{
    static int32_t buf[2 * BLK];
    int t, i;
    for (t = 0; t < n; t += BLK) {
        for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = (int32_t)lround(a * sin(2 * M_PI * f * (t + i) / FS));
        run(buf, BLK);
        for (i = 0; i < BLK && t + i < n; i++) out_[t + i] = buf[2 * i];
    }
}
static double tone_amp(double f, int a, int b)  /* the amplitude of f in out_[a .. b) (whole cycles, near enough) */
{
    double re = 0, im = 0;
    int i;
    for (i = a; i < b; i++) { re += out_[i] * cos(2 * M_PI * f * i / FS); im += out_[i] * sin(2 * M_PI * f * i / FS); }
    return hypot(re, im) * 2 / (b - a);
}
static double pitch_swing(int a, int b)         /* zero crossings in 50 ms windows: (max - min) / mean */
{
    int win = 2205, s, i, lo = 1 << 30, hi = 0;
    double tot = 0, nw = 0;
    for (s = a; s + win <= b; s += win) {
        int zc = 0;
        for (i = s + 1; i < s + win; i++) zc += (out_[i - 1] < 0) != (out_[i] < 0);
        if (zc < lo) lo = zc;
        if (zc > hi) hi = zc;
        tot += zc; nw++;
    }
    return (hi - lo) / (tot / nw);
}

int main(void)
{
    int i;

    /* at rest */
    {
        static int32_t buf[2 * 64];
        double g;
        int at = -1, pk = 0;
        setp(0, 0, 127, 0, 0);
        memset(buf, 0, sizeof buf);
        buf[0] = buf[1] = 20000;
        run(buf, 64);
        for (i = 0; i < 64; i++) if (abs(buf[2 * i]) > pk) { pk = abs(buf[2 * i]); at = i; }
        setp(0, 0, 127, 0, 0);
        play(1000, 8000, 44100);
        g = 20 * log10(tone_amp(1000, 4410, 44100) / 8000);
        CHECK(at == 2 && fabs(g) < 0.05, "at rest: the impulse at %d, 1 kHz at %.3f dB", at, g);
        printf("lofi: at rest, the impulse back at %d samples; 1 kHz at %+.3f dB\n", at, g);
    }

    /* BITS: signal to noise, against the sound's own level: a loud sine and a quiet one (24 dB down) the same */
    {
        static const int NB[5] = {3, 4, 6, 8, 10};
        static const double AMP[2] = {30000, 1900};
        int k, ai;
        for (ai = 0; ai < 2; ai++) for (k = 0; k < 5; k++) {
            int p = (int)lround((16 - NB[k]) * 127.0 / 14), j;
            double bits = 16 - 14.0 * p / 127, f = 997, a1, ph, re = 0, im = 0, sig, noise = 0, want, got;
            if (ai == 0 && NB[k] == 3) continue;
            if (ai == 1 && NB[k] > 4) continue;
            setp(0, p, 127, 0, 0);
            play(f, AMP[ai], 44100);
            for (j = 4410; j < 44100; j++) { re += out_[j] * cos(2 * M_PI * f * j / FS); im += out_[j] * sin(2 * M_PI * f * j / FS); }
            a1 = hypot(re, im) * 2 / (44100 - 4410);
            ph = atan2(im, re);
            for (j = 4410; j < 44100; j++) {
                double e = out_[j] - a1 * cos(2 * M_PI * f * j / FS - ph);
                noise += e * e;
            }
            sig = a1 * a1 / 2;
            got = 10 * log10(sig / (noise / (44100 - 4410)));
            want = 6.02 * bits + 1.76;          /* (full scale: the sine's own peak, the envelope) */
            CHECK(got > want - 2 && got < want + (bits < 3.5 ? 4 : 2), "BITS %.2f, a sine of %.0f: SNR %.1f dB, want %.1f",
                  bits, AMP[ai], got, want);   /* (under 4 bits the error is no longer noise-like: better than the
                                                   formula, not worse) */
            printf("lofi: %5.2f bits, a sine of %5.0f: SNR %.1f dB (theory %.1f)\n", bits, AMP[ai], got, want);
        }
        {                                       /* no DC: the mean within what the error's own spread allows (the step
                                                   follows the envelope, so the error is noise-like now: its mean
                                                   wanders as sigma / sqrt(N), 4 sigma) */
            double mean = 0, re = 0, im = 0, a1, ph, e = 0, lim;
            int nn = 44100 - 4410;
            setp(0, (int)lround(13 * 127.0 / 14), 127, 0, 0);
            play(1000, 20000, 44100);           /* (1 kHz: the window is 900 whole cycles, no mean of its own) */
            for (i = 4410; i < 44100; i++) { mean += out_[i]; re += out_[i] * cos(2 * M_PI * 1000 * i / FS); im += out_[i] * sin(2 * M_PI * 1000 * i / FS); }
            mean /= nn;
            a1 = hypot(re, im) * 2 / nn; ph = atan2(im, re);
            for (i = 4410; i < 44100; i++) { double d = out_[i] - a1 * cos(2 * M_PI * 1000 * i / FS - ph); e += d * d; }
            lim = 4 * sqrt(e / nn) / sqrt(nn);
            CHECK(fabs(mean) < lim, "3 bits: a DC of %.2f (4 sigma: %.2f)", mean, lim);
            printf("lofi: 3 bits, the mean %.2f (4 sigma of the error's: %.2f; no DC)\n", mean, lim);
        }
    }

    /* RATE: a hold's droop and image */
    {
        int p = 82;
        double R = 44100 * pow(40, -p / 127.0), f = 1000, a = 8000, f1, im1, w1, wi;
        setp(p, 0, 127, 0, 0);
        play(f, a, 3 * 44100);
        f1 = 20 * log10(tone_amp(f, 4410, 3 * 44100) / a);
        im1 = 20 * log10(tone_amp(R - f, 4410, 3 * 44100) / a);
        w1 = 20 * log10(fabs(sin(M_PI * f / R) / (M_PI * f / R)));
        wi = 20 * log10(fabs(sin(M_PI * (R - f) / R) / (M_PI * (R - f) / R)));
        CHECK(fabs(f1 - w1) < 2 && fabs(im1 - wi) < 2, "RATE %.0f Hz: 1 kHz at %.1f (want %.1f), image at %.0f Hz %.1f (want %.1f)",
              R, f1, w1, R - f, im1, wi);
        printf("lofi: RATE %.0f Hz: 1 kHz %.1f dB (sinc %.1f), its image at %.0f Hz %.1f dB (sinc %.1f)\n", R, f1, w1, R - f, im1, wi);
    }

    /* WOBBLE */
    {
        double sw0, sw1;
        setp(0, 0, 127, 0, 0);
        play(1000, 8000, 4 * 44100);
        sw0 = pitch_swing(8820, 4 * 44100);
        setp(0, 0, 127, 127, 0);
        play(1000, 8000, 4 * 44100);
        sw1 = pitch_swing(8820, 4 * 44100);
        CHECK(sw0 < 0.001 && sw1 > 0.02 && sw1 < 0.05, "WOBBLE: the pitch swings %.2f %% (at 0: %.3f %%)", 100 * sw1, 100 * sw0);
        printf("lofi: WOBBLE at the top: the pitch swings %.2f %% (at 0: %.3f %%)\n", 100 * sw1, 100 * sw0);
    }
    {                                           /* turned while it plays */
        static int32_t buf[2 * BLK];
        double a = 8000, f = 1000, worst = 0, own = 2 * M_PI * f / FS * a;
        int t, prev = 0;
        setp(0, 0, 127, 0, 0);
        for (t = 0; t < 3 * 44100; t += BLK) {
            if (t == 44100) P_wa = 127 * (160 << 16) / 127, P_fa = 5 << 16;
            if (t == 2 * 44100) P_wa = P_fa = 0;
            for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = (int32_t)lround(a * sin(2 * M_PI * f * (t + i) / FS));
            run(buf, BLK);
            for (i = 0; i < BLK; i++) {
                if (t + i > 100 && fabs((double)(buf[2 * i] - prev)) > worst) worst = fabs((double)(buf[2 * i] - prev));
                prev = buf[2 * i];
            }
        }
        CHECK(worst < 1.3 * own, "WOBBLE turned: a step of %.0f (the sine's own %.0f)", worst, own);
        printf("lofi: WOBBLE turned on and off while it plays: the largest step %.0f (the sine's own %.0f)\n", worst, own);
    }

    /* NOISE, silence */
    {
        static int32_t buf[2 * BLK];
        double e = 0;
        int t, nz = 0, last = -1;
        setp(0, 0, 127, 0, 127);
        play(997, 8000, 44100);
        {
            double re = 0, im = 0, a1, ph;
            for (i = 4410; i < 44100; i++) { re += out_[i] * cos(2 * M_PI * 997 * i / FS); im += out_[i] * sin(2 * M_PI * 997 * i / FS); }
            a1 = hypot(re, im) * 2 / (44100 - 4410); ph = atan2(im, re);
            for (i = 4410; i < 44100; i++) { double d = out_[i] - a1 * cos(2 * M_PI * 997 * i / FS - ph); e += d * d; }
            e = sqrt(e / (44100 - 4410));
        }
        CHECK(e > 300, "NOISE at the top: the hiss only %.0f rms", e);
        printf("lofi: NOISE at the top on a sine of 8000: hiss %.0f rms\n", e);
        setp(100, 127, 0, 127, 127);
        for (t = 0; t < 44100; t += BLK) {
            memset(buf, 0, sizeof buf);
            run(buf, BLK);
            for (i = 0; i < 2 * BLK; i++) nz += buf[i] != 0;
        }
        CHECK(nz == 0, "silence from the start, everything at the top: %d samples not 0", nz);
        setp(100, 100, 60, 127, 127);
        play(300, 30000, 44100);
        for (t = 0; t < 3 * 44100; t += BLK) {
            memset(buf, 0, sizeof buf);
            run(buf, BLK);
            for (i = 0; i < 2 * BLK; i++) if (buf[i]) last = t + i / 2;
        }
        CHECK(last < 2 * 44100, "after a loud sine: still sounding at %.2f s", last / FS);
        printf("lofi: silence from the start: all 0; after a loud sine the last non-zero at %.2f s\n", last / FS);
    }

    printf(fails ? "lofi: %d FAILED\n" : "lofi: all passed\n", fails);
    return fails != 0;
}
