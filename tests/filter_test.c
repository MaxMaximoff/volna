/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: FILTER (engine/fx.h) on the host.
 *   still (DEPTH 0): noise through every type at three cutoffs and resonances, against the same filter (svf.h's
 *   coefficients) in doubles: under -50 dB off, or (a narrow band at the top resonance, its output quiet) the error
 *   under 3 rms, -80 dBFS: the integer filter's own floor, which the resonance lifts (svf.h's own svf_tick, without
 *   remainders, printed alongside: its low pass sticks at an offset);
 *   swept fast (8 Hz, +-5 octaves, resonance 100): against the same filter with its coefficients computed every
 *   sample, within -40 dB (rms of the difference); coefficients stepped every 16 samples without the ramp: printed
 *   for comparison;
 *   ENV: a 2 kHz sine through a low pass at 200 Hz, DEPTH +5 octaves: louder opens it more, and a quiet one (24 dB
 *   down) still opens it (dB, not linear);
 *   LFO: the sides the same to the bit; STEREO: they differ;
 *   bounded: full-scale noise at the top resonance (LP24, a fast sweep) within the clip; then silence: all 0 within
 *   a second at 554 Hz, and at the lowest cutoff (13 Hz, resonance 120: it rings ~3 s) within 10 s — 0, no offset
 *   stuck, for every type.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/filter_test tests/filter_test.c -lm */
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
static int32_t P_cn, P_dep;
static uint32_t P_rate, P_woff, P_res, P_ts;
static void setp(int note, int dep_semi, uint32_t rate, uint32_t woff, uint32_t res, uint32_t type, uint32_t src)
{
    memset(&S, 0, sizeof S);
    P_cn = note << 8; P_dep = dep_semi << 8; P_rate = rate; P_woff = woff; P_res = res; P_ts = type | src << 8;
}
static void run(int32_t *buf, uint32_t n) { fx_filter(&S.flt, buf, n, P_cn, P_dep, P_rate, P_woff, P_res, P_ts, 0, 4096); }
static uint32_t hz_inc(double hz) { return (uint32_t)(hz / FS * 4294967296.0); }
static uint32_t rnd = 2024u;
static int32_t noise(int32_t a) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return (int32_t)((int64_t)((int32_t)(rnd >> 16) - 32768) * a >> 15); }

int main(void)
{
    static const uint32_t TY[7] = {SVF_LP24, SVF_LP, SVF_BP24, SVF_BP, SVF_HP24, SVF_HP, SVF_NOTCH};
    static int32_t buf[2 * BLK];
    int a, b, r, t, i;

    /* still: the same filter as svf.h's (its coefficients), against it in doubles (quiet noise: the driven knee not
       reached, a linear filter); svf.h's own svf_tick (no remainders) alongside, for comparison */
    {
        static const int CN[3] = {40, 76, 120};
        static const uint32_t RS[3] = {0, 60, 120};
        double worst = -200, worst_t = -200, worst_abs = 0;
        for (a = 0; a < 7; a++) for (b = 0; b < 3; b++) for (r = 0; r < 3; r++) {
            svf_state_t ref;
            svf_coef_t c;
            double s1 = 0, s2 = 0, s3 = 0, s4 = 0, e = 0, et = 0, p = 0, ea;
            int cnt = 0;
            memset(&ref, 0, sizeof ref);
            svf_coef(&c, CN[b] << 8, RS[r], TY[a]);
            c.nl = 1;
            setp(CN[b], 0, 0, 0, RS[r], TY[a], FS_LFO);
            for (t = 0; t < 22050; t += BLK) {
                int32_t in[BLK];
                for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = in[i] = noise(r == 2 ? 500 : 8000);   /* (loud
                                                   enough that a narrow band's output is not just its rounding; at
                                                   the top resonance under the driven knee) */
                run(buf, BLK);
                for (i = 0; i < BLK; i++) {
                    double v0 = in[i], v1, v2, y, g = c.g / 268435456.0;
                    int32_t yt = svf_tick(&ref, &c, in[i]);
                    if (c.p4) {
                        double pa1 = c.pa1 / 1073741824.0, pa2 = c.pa2 / 1073741824.0;
                        v1 = pa1 * s3 + pa2 * (v0 - s4); v2 = s4 + g * v1; s3 = 2 * v1 - s3; s4 = 2 * v2 - s4;
                        v0 = (c.px * v0 + c.pl * v2 + c.pb * v1) / 16384.0;
                    }
                    v1 = c.a1 / 1073741824.0 * s1 + c.a2 / 1073741824.0 * (v0 - s2); v2 = s2 + g * v1;
                    s1 = 2 * v1 - s1; s2 = 2 * v2 - s2;
                    y = (c.cx * v0 + c.cl * v2 + c.cb * v1) / 16384.0;
                    if (t > 4410) { e += (buf[2 * i] - y) * (buf[2 * i] - y); et += (yt - y) * (yt - y); p += y * y; cnt++; }
                }
            }
            ea = sqrt(e / cnt);                 /* (the error in samples: the integer filter's own floor) */
            e = 10 * log10(e / p + 1e-30); et = 10 * log10(et / p + 1e-30);
            if (e > worst) worst = e;
            if (et > worst_t) worst_t = et;
            if (ea > worst_abs) worst_abs = ea;
            CHECK(e < -50 || ea < 3, "still, type %u note %d reso %u: %.1f dB off the filter in doubles (%.2f rms)",
                  TY[a], CN[b], RS[r], e, ea);
        }
        printf("filter: still, every type, 3 cutoffs, 3 resonances: off the filter in doubles by %.1f dB at worst, or"
               " %.2f rms (-%.0f dBFS) where the output is quiet (svf.h's svf_tick: %.1f dB)\n", worst, worst_abs,
               -20 * log10(worst_abs / 32768), worst_t);
    }

    /* swept fast: the ramp against coefficients every sample */
    {
        double e = 0, e16 = 0, ref = 0;
        uint32_t inc = hz_inc(8), ph;
        int32_t st[8] = {0}, st16[8] = {0};
        svf_coef_t c16;
        memset(&c16, 0, sizeof c16);
        setp(76, 60, inc, 0, 100, SVF_LP24, FS_LFO);
        ph = 0;
        for (t = 0; t < 44100; t += BLK) {
            int32_t in[BLK];
            for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = in[i] = noise(8000);
            run(buf, BLK);
            for (i = 0; i < BLK; i++) {
                svf_coef_t c;
                int32_t y, y16;
                ph += inc;                      /* (the LFO where fx_filter has it this sample) */
                svf_coef(&c, (76 << 8) + (int32_t)(((int64_t)fx_cos14(ph - 0x40000000u) * 2 * (60 << 8)) >> 15), 100, SVF_LP24);
                c.nl = 1;
                y = ff_tick(st, &c, in[i]);
                if (!((t + i) & 15)) { svf_coef(&c16, (76 << 8) + (int32_t)(((int64_t)fx_cos14(ph + 15u * inc - 0x40000000u) * 2 * (60 << 8)) >> 15), 100, SVF_LP24); c16.nl = 1; }
                y16 = ff_tick(st16, &c16, in[i]);
                if (t > 4410) { e += (double)(buf[2 * i] - y) * (buf[2 * i] - y); e16 += (double)(y16 - y) * (y16 - y); ref += (double)y * y; }
            }
        }
        CHECK(10 * log10(e / ref) < -40, "swept: %.1f dB off coefficients every sample", 10 * log10(e / ref));
        printf("filter: swept 8 Hz +-5 octaves at resonance 100: %.1f dB off coefficients every sample (stepped every 16"
               " without the ramp: %.1f dB)\n", 10 * log10(e / ref), 10 * log10(e16 / ref));
    }

    /* ENV */
    {
        double g[3];
        static const double A[3] = {0, 1000, 16000};
        uint32_t kr = (uint32_t)((1 << 24) / (0.2 * FS));   /* (200 ms) */
        for (a = 0; a < 3; a++) {
            double re = 0, im = 0, f = 2000, amp = a ? A[a] : 1000;
            setp(56, a ? 60 : 0, kr, 0, 20, SVF_LP24, FS_ENV);
            for (t = 0; t < 44100; t += BLK) {
                for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = (int32_t)lround(amp * sin(2 * M_PI * f * (t + i) / FS));
                run(buf, BLK);
                for (i = 0; i < BLK; i++) if (t + i >= 22050) { re += buf[2 * i] * cos(2 * M_PI * f * (t + i) / FS); im += buf[2 * i] * sin(2 * M_PI * f * (t + i) / FS); }
            }
            g[a] = 20 * log10(hypot(re, im) * 2 / 22050 / amp);
        }
        CHECK(g[0] + 6 < g[1] && g[1] + 6 < g[2], "ENV: 2 kHz at %.1f (still), %.1f (quiet), %.1f dB (loud)", g[0], g[1], g[2]);
        printf("filter: ENV, 2 kHz through LP24 at 208 Hz: closed %.1f dB, a quiet note %.1f dB, a loud one %.1f dB\n", g[0], g[1], g[2]);
    }

    /* the sides */
    for (a = 0; a < 2; a++) {
        int same = 1, diff = 0;
        setp(76, 36, hz_inc(1), 0x40000000u, 60, SVF_BP, a ? FS_STEREO : FS_LFO);
        for (t = 0; t < 44100; t += BLK) {
            for (i = 0; i < BLK; i++) buf[2 * i] = buf[2 * i + 1] = noise(8000);
            run(buf, BLK);
            for (i = 0; i < BLK; i++) { if (buf[2 * i] != buf[2 * i + 1]) same = 0; if (abs(buf[2 * i] - buf[2 * i + 1]) > 1000) diff++; }
        }
        if (!a) CHECK(same, "LFO: the sides differ");
        else CHECK(diff > 4410, "STEREO: the sides apart in only %d samples", diff);
        if (a) printf("filter: LFO the sides the same; STEREO apart in %d samples of 44100\n", diff);
    }

    /* bounded, and every tail ends */
    {
        int pk = 0, worst_t = 0;
        setp(76, 60, hz_inc(9), 0, 127, SVF_LP24, FS_LFO);
        for (t = 0; t < 3 * 44100; t += BLK) {
            for (i = 0; i < 2 * BLK; i++) buf[i] = noise(32767);
            run(buf, BLK);
            for (i = 0; i < 2 * BLK; i++) if (abs(buf[i]) > pk) pk = abs(buf[i]);
        }
        CHECK(pk <= SVF_CLIP, "noise at the top resonance: a peak of %d", pk);
        for (a = 0; a < 7; a++) for (b = 0; b < 2; b++) {
            int last = -1;
            setp(b ? 4 : 76, 0, 0, 0, 120, TY[a], FS_LFO);
            for (t = 0; t < 44100; t += BLK) {
                for (i = 0; i < 2 * BLK; i++) buf[i] = noise(20000);
                run(buf, BLK);
            }
            for (t = 0; t < 20 * 44100; t += BLK) {
                memset(buf, 0, sizeof buf);
                run(buf, BLK);
                for (i = 0; i < 2 * BLK; i++) if (buf[i]) last = t + i / 2;
            }
            if (last > worst_t) worst_t = last;
            /* (at 13 Hz, Q ~ 16 rings: tau = Q / (pi f) = 0.4 s, 60 dB in ~3 s; then 0 for good) */
            CHECK(last < (b ? 10 : 1) * 44100, "type %u at note %d: still sounding %.2f s into the silence", TY[a], b ? 4 : 76, last / FS);
        }
        printf("filter: noise at the top resonance peaks at %d (the clip %d); every type's tail 0 by %.2f s (13 Hz, Q 16"
               " rings that long)\n", pk, SVF_CLIP, worst_t / FS);
    }

    printf(fails ? "filter: %d FAILED\n" : "filter: all passed\n", fails);
    return fails != 0;
}
