/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna stage 3: the state-variable filter (engine/svf.h) on the host.
 *   1. the coefficients: g within 0.25 % (4 cents) of tan(pi f / fs) between the table's half semitones,
 *      0.015 % (0.25 cent) up to 4 kHz; a1 a2 to 1e-6 of
 *      1 / (1 + g (g + k)) and g a1 (the division and the Newton step);
 *   2. the response: sines through LP, BP, HP and notch at Q 0.7 match the filter's own transfer function (the
 *      bilinear transform of the analog one, prewarped at the cutoff) within 0.2 dB, at several cutoffs;
 *   3. the resonance: at the top (Q 25) the low pass peaks +14 dB (Q x SVF_COMP) at the cutoff;
 *   4. no blow-ups: full-scale noise, the top resonance, the cutoff jumping anywhere every block, 20 s of each
 *      type: the integrators stay far inside 32 bits; then silence: the ringing dies away;
 *   5. the drive: unity for small signals, bounded, monotonic;
 *   6. the screen's response curve (svf_db_q8) within 1 dB of the analog one;
 *   7. the 24 dB types (LP24 BP24 HP24; NOTCH24 and PPG LP since 2026-11-16): 2, 4 and 6 hold for them too (their response: both stages'); an octave
 *      past the cutoff the LP24 is 11..16 dB further down than the LP (its first stage, Q 0.54: about -13.5), the
 *      HP24 likewise below; the resonance peak is the 12 dB type's less 5.3 dB; the passband flat;
 *   8. the driven resonance (nl): the knee continuous, monotonic, its slope at most 1, flat at 2.33 x full scale;
 *      a quiet sine through it as through the linear filter, to the bit; a full-scale one at the top resonance
 *      squashed (its peak 6 dB or more under the quiet one's gain) and bounded; 4 holds with it on, every type.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/svf_test tests/svf_test.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "engine/svf.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double FS = 44100.0;
static double freq(double n) { return 440.0 * pow(2.0, (n - 69.0) / 12.0); }
static double q_of(int r) { return 0.5 * pow(50.0, r / 127.0); }

/* the analog prototype's |H|^2 at x (the frequency over the cutoff), damping k, the LP/HP level c */
static double h2(int type, double k, double c, double x)
{
    double den = (1 - x * x) * (1 - x * x) + k * k * x * x, num;
    switch (type) {
    case SVF_LP: num = c * c; break;
    case SVF_HP: num = c * c * x * x * x * x; break;
    case SVF_BP: num = k * k * x * x; break;
    default: num = (1 - x * x) * (1 - x * x); break;
    }
    return num / den;
}
/* the type's |H|^2 at x for the resonance r: a 24 dB type, its two stages' */
static double h2_type(int type, int r, double x)
{
    double k = SVF_K[r] / 16384.0, c = SVF_COMP[r] / 32768.0;
    if (SVF_IS24(type))                         /* (PPG LP: its first stage at half the resonance) */
        return h2(SVF_BASE(type), k, c, x) * h2(SVF_BASE(type), (type == SVF_PPG ? SVF_K[r >> 1] : SVF_K24) / 16384.0, 1.0, x);
    return h2(type, k, c, x);
}
/* the filter's own |H| at f, dB: the analog prototype at x = tan(pi f / fs) / tan(pi fc / fs) */
static double h_db(int type, int r, double fc, double f)
{
    return 10.0 * log10(h2_type(type, r, tan(M_PI * f / FS) / tan(M_PI * fc / FS)) + 1e-30);
}

/* a sine of amplitude a at f through the filter: the output's level at f in dB (a correlation, not a peak) */
static int g_nl;                                /* run_sine: the resonance driven (8) */
static double run_sine(int type, int r, int32_t n, double f, double a)
{
    svf_state_t s = {0, 0};
    svf_coef_t c;
    double si = 0, co = 0;
    int i, N = 1 << 15, settle = 1 << 14;
    svf_coef(&c, n, (uint32_t)r, (uint32_t)type);
    c.nl = g_nl;
    for (i = 0; i < settle + N; i++) {
        double ph = 2 * M_PI * f * i / FS;
        int32_t y = svf_tick(&s, &c, (int32_t)lrint(a * sin(ph)));
        if (i >= settle) { si += y * sin(ph); co += y * cos(ph); }
    }
    return 20.0 * log10(2.0 * sqrt(si * si + co * co) / N / a + 1e-30);
}

int main(void)
{
    static const char *const TN[SVF_NTYPE] = {"OFF", "LP", "BP", "HP", "NOTCH", "LP24", "BP24", "HP24", "NOTCH24",
                                              "PPG", "COMB+", "COMB-"};   /* (the combs: volna.c's comb_tick, not here) */
    svf_coef_t c;
    int32_t n, x, prev;
    int r, t, i;
    double worst_g = 0, worst_g4k = 0, worst_a = 0;

    for (n = 0; n <= SVF_NMAX; n += 13) {       /* 1 */
        for (r = 0; r < 128; r += 9) {
            double g, k = SVF_K[r] / 16384.0, gi, a1;
            svf_coef(&c, n, (uint32_t)r, SVF_LP);
            g = tan(M_PI * freq(n / 256.0) / FS);
            gi = c.g / 268435456.0;
            a1 = 1.0 / (1.0 + gi * (gi + k));
            if (fabs(gi / g - 1) > worst_g) worst_g = fabs(gi / g - 1);
            if (n <= 107 << 8 && fabs(gi / g - 1) > worst_g4k) worst_g4k = fabs(gi / g - 1);
            if (fabs(c.a1 / 1073741824.0 - a1) > worst_a) worst_a = fabs(c.a1 / 1073741824.0 - a1);
            if (fabs(c.a2 / 1073741824.0 - gi * a1) > worst_a) worst_a = fabs(c.a2 / 1073741824.0 - gi * a1);
        }
    }
    printf("coefficients: g within %.4f %% (%.4f %% up to 4 kHz), a1 a2 within %.2g\n", worst_g * 100,
           worst_g4k * 100, worst_a);
    CHECK(worst_g < 2.5e-3, "g off by %.4f %%", worst_g * 100);
    CHECK(worst_g4k < 1.5e-4, "g off by %.4f %% up to 4 kHz", worst_g4k * 100);
    CHECK(worst_a < 1e-6, "a1 / a2 off by %.2g", worst_a);

    {                                           /* 2 */
        static const int32_t CUT[4] = {48, 81, 105, 120};
        static const double REL[5] = {0.25, 0.7071, 1.0, 1.4142, 4.0};
        double worst = 0;
        for (t = SVF_LP; t < SVF_COMBP; t++)
            for (i = 0; i < 4; i++) {
                int j;
                double fc = freq(CUT[i]);
                for (j = 0; j < 5; j++) {
                    double f = fc * REL[j], want, got;
                    if (f > 19000) continue;
                    want = h_db(t, 11, fc, f);
                    if (want < -50) continue;   /* (the notch's floor, the 24 dB types' far skirts: no level to
                                                   compare) */
                    got = run_sine(t, 11, CUT[i] << 8, f, 20000);
                    if (fabs(got - want) > worst) worst = fabs(got - want);
                    CHECK(fabs(got - want) < 0.2, "%s at %.0f Hz, cutoff %.0f Hz: %.2f dB, want %.2f", TN[t], f, fc,
                          got, want);
                }
            }
        printf("response at Q 0.7: within %.3f dB of the transfer function\n", worst);
    }

    {                                           /* 3 */
        double got = run_sine(SVF_LP, 127, 81 << 8, freq(81), 2000), want = 20 * log10(q_of(127) * SVF_COMP[127] / 32768.0);
        printf("top resonance: the low pass at its cutoff %+.2f dB (want %+.2f)\n", got, want);
        CHECK(fabs(got - want) < 0.5, "top resonance peak %.2f dB, want %.2f", got, want);
    }

    for (t = SVF_LP; t < SVF_COMBP; t++) {      /* 4 */
        svf_state_t s = {0, 0};
        uint32_t rnd = 12345u, peak_after = 0;
        int32_t big = 0, y;
        for (i = 0; i < 44100 * 20; i++) {
            if (i % 16 == 0) {
                rnd = rnd * 1664525u + 1013904223u;
                svf_coef(&c, (int32_t)(rnd >> 8) % (SVF_NMAX + 1), 127, (uint32_t)t);
            }
            rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
            y = svf_tick(&s, &c, (int32_t)(rnd % 131071u) - 65535);
            (void)y;
            if (abs(s.ic1) > big) big = abs(s.ic1);
            if (abs(s.ic2) > big) big = abs(s.ic2);
            if (abs(s.ic3) > big) big = abs(s.ic3);
            if (abs(s.ic4) > big) big = abs(s.ic4);
        }
        svf_coef(&c, 60 << 8, 127, (uint32_t)t);   /* then silence at 262 Hz, the top resonance: it rings, then dies */
        for (i = 0; i < 44100 * 2; i++) {
            y = svf_tick(&s, &c, 0);
            if (i >= 44100 * 2 - 4410 && (uint32_t)abs(y) > peak_after) peak_after = (uint32_t)abs(y);
        }
        printf("%-5s modulated 20 s: integrators at most %d (%.1f bits); 2 s of silence later: %u\n", TN[t], big,
               log2(big + 1.0), peak_after);
        CHECK(big < (1 << 29), "%s: an integrator reached %d", TN[t], big);
        CHECK(peak_after <= (t == SVF_NOTCH24 ? 8u : 2u), "%s: still %u after 2 s of silence", TN[t], peak_after);   /* (NOTCH24
                                                   passes its first stage's own rounding through: a few LSB, -78 dB) */
    }

    {                                           /* 5 */
        static const int32_t G[3] = {256, 1024, 2048};
        for (i = 0; i < 3; i++) {
            prev = -100000;
            for (x = -SVF_CLIP; x <= SVF_CLIP; x += 7) {
                int32_t y = svf_drive(x, G[i]);
                CHECK(y >= prev, "drive %d: not monotonic at %d", G[i], x);
                CHECK(abs(y) <= 32769, "drive %d: %d at %d", G[i], y, x);
                if (G[i] == 256 && abs(x) <= 8000) CHECK(abs(y - x) <= abs(x) / 100 + 1, "drive: %d -> %d", x, y);
                prev = y;
            }
        }
    }

    {                                           /* 6 */
        double worst = 0;
        for (t = SVF_LP; t < SVF_COMBP; t++)
            for (r = 0; r < 128; r += 21)
                for (i = -1584; i <= 1584; i += 11) {
                    double x = pow(2.0, i / 264.0), want = 10 * log10(h2_type(t, r, x) + 1e-30);
                    double got = svf_db_q8((uint32_t)t, (uint32_t)r, i) / 256.0;
                    if (want < -60) continue;
                    if (fabs(got - want) > worst) worst = fabs(got - want);
                    CHECK(fabs(got - want) < 1.0, "curve %s r %d at %+.2f oct: %.2f dB, want %.2f", TN[t], r,
                          i / 264.0, got, want);
                }
        printf("screen curve: within %.2f dB\n", worst);
    }

    {                                           /* 7 */
        double lp = run_sine(SVF_LP, 0, 69 << 8, 880, 20000), lp4 = run_sine(SVF_LP24, 0, 69 << 8, 880, 20000);
        double hp = run_sine(SVF_HP, 0, 69 << 8, 220, 20000), hp4 = run_sine(SVF_HP24, 0, 69 << 8, 220, 20000);
        double pk = run_sine(SVF_LP, 127, 81 << 8, freq(81), 2000);
        double pk4 = run_sine(SVF_LP24, 127, 81 << 8, freq(81), 2000);
        double pass = run_sine(SVF_LP24, 0, 93 << 8, 110, 20000);
        printf("24 dB: an octave past 440 Hz LP %.1f dB, LP24 %.1f; an octave below, HP %.1f, HP24 %.1f; "
               "the top peak %+.1f (LP %+.1f); the passband %+.2f dB\n", lp, lp4, hp, hp4, pk4, pk, pass);
        CHECK(lp4 < lp - 11 && lp4 > lp - 16, "LP24 an octave up: %.1f dB (LP %.1f)", lp4, lp);
        CHECK(hp4 < hp - 11 && hp4 > hp - 16, "HP24 an octave down: %.1f dB (HP %.1f)", hp4, hp);
        CHECK(fabs(pk4 - (pk + 20 * log10(16384.0 / SVF_K24))) < 0.5, "LP24 top peak %.1f dB, LP %.1f", pk4, pk);
        CHECK(fabs(pass) < 0.1, "LP24 passband %.2f dB", pass);
    }

    {                                           /* 8 */
        int32_t v, y, py = svf_sat(SVF_NLK + 1) - 1, worst_step = 0;
        double quiet_lin, quiet, loud;
        for (v = SVF_NLK + 1; v < SVF_NLK + SVF_NLW + 4096; v += 37) {
            y = svf_sat(v);
            CHECK(y >= py && y - py <= 37, "knee at %d: %d after %d", v, y, py);
            CHECK(svf_sat(-v) == -y, "knee not odd at %d", v);
            if (y - py > worst_step) worst_step = y - py;
            py = y;
        }
        CHECK(abs(svf_sat(SVF_NLK + 1) - (SVF_NLK + 1)) <= 1, "the knee jumps: %d", svf_sat(SVF_NLK + 1));
        CHECK(py == SVF_NLK + SVF_NLW * 2 / 3, "the knee's top %d", py);
        quiet_lin = run_sine(SVF_LP, 127, 81 << 8, freq(81), 1000);
        g_nl = 1;
        quiet = run_sine(SVF_LP, 127, 81 << 8, freq(81), 1000);
        loud = run_sine(SVF_LP, 127, 81 << 8, freq(81), 32000);
        g_nl = 0;
        printf("driven resonance: the knee's top %.2f x full scale; the top resonance at its cutoff: quiet %+.2f dB "
               "(linear %+.2f), full scale %+.2f dB\n", py / (32768.0 * 16), quiet, quiet_lin, loud);
        CHECK(fabs(quiet - quiet_lin) < 0.01, "driven, quiet: %.2f dB, linear %.2f", quiet, quiet_lin);
        CHECK(loud < quiet - 6, "driven, full scale: %.2f dB, quiet %.2f: not squashed", loud, quiet);
        {                                       /* a quiet sine, sample for sample: linear or driven, the same */
            svf_state_t a = {0, 0, 0, 0}, b = {0, 0, 0, 0};
            svf_coef_t ca, cb;
            int same = 1;
            svf_coef(&ca, 70 << 8, 100, SVF_LP);
            cb = ca;
            cb.nl = 1;
            for (i = 0; i < 44100; i++) {
                int32_t xx = (int32_t)lrint(1500 * sin(2 * M_PI * 440 * i / FS));
                if (svf_tick(&a, &ca, xx) != svf_tick(&b, &cb, xx)) same = 0;
            }
            CHECK(same, "a quiet sine, driven: not the linear filter's");
        }
        for (t = SVF_LP; t < SVF_COMBP; t++) {  /* 4 with the resonance driven */
            svf_state_t s = {0, 0, 0, 0};
            uint32_t rnd = 777u, peak_after = 0;
            int32_t big = 0;
            for (i = 0; i < 44100 * 10; i++) {
                if (i % 16 == 0) {
                    rnd = rnd * 1664525u + 1013904223u;
                    svf_coef(&c, (int32_t)(rnd >> 8) % (SVF_NMAX + 1), 127, (uint32_t)t);
                    c.nl = 1;
                }
                rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
                (void)svf_tick(&s, &c, (int32_t)(rnd % 65535u) - 32767);
                if (abs(s.ic1) > big) big = abs(s.ic1);
                if (abs(s.ic2) > big) big = abs(s.ic2);
            }
            svf_coef(&c, 60 << 8, 127, (uint32_t)t);
            c.nl = 1;
            for (i = 0; i < 44100 * 2; i++) {
                y = svf_tick(&s, &c, 0);
                if (i >= 44100 * 2 - 4410 && (uint32_t)abs(y) > peak_after) peak_after = (uint32_t)abs(y);
            }
            CHECK(big < (1 << 29) && peak_after <= (t == SVF_NOTCH24 ? 8u : 2u), "%s driven: integrators %d, %u after silence",
                  TN[t], big, peak_after);
        }
    }

    printf(fails ? "svf: %d FAILED\n" : "svf: all passed\n", fails);
    return fails != 0;
}
