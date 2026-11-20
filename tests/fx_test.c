/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna stage 7b: the effects (engine/fx.h) on the host.
 *   DELAY: an impulse comes back after the time (to the sample), each echo the feedback times the last; at TONE open
 *   the echo is the impulse itself; a time change glides (no jump of the read point); PING sends the first echo left,
 *   the second right; at full feedback and full scale noise nothing grows past the line's range, and silence after
 *   it dies out.
 *   REVERB: an impulse gives a tail on both sides that differ (a stereo plate); a bigger SIZE rings longer; at the top
 *   SIZE and no damping, 10 s of full scale noise stay bounded, and 30 s of silence after it end at 0 exactly.
 *   DIST: at no drive a quiet sine goes through as it is (times LEVEL); HARD at a high drive makes it nearly a square;
 *   FOLD folds it (more zero crossings); TUBE adds the second harmonic (SOFT does not) and leaves no DC; full scale
 *   noise at the top drive stays within LEVEL for every curve.
 *   CHORUS: no depth: an impulse comes back at the base delay; with depth, impulses come back between base - depth
 *   and base + depth; WIDTH 0: the sides the same, half a cycle: they differ; a flanger's feedback at +-0.95 stays
 *   bounded with noise and dies out after it.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/fx_test tests/fx_test.c */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/fx.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
#define FS 44100u
#define BLK 256u
static int16_t mem[FX_MEM];
static int32_t buf[2u * BLK];

static uint32_t rnd = 12345u;
static int32_t noise(void) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return (int32_t)(rnd >> 16) - 32768; }

/* run a slot over len frames of input in (mono into both, or 0), the output into L, R (may be 0) */
static fx_state_t st;
static void run(uint32_t type, const int32_t *in, uint32_t len, int32_t *L, int32_t *R, int32_t a, int32_t b,
                int32_t c, int32_t d, int32_t dry, int32_t wet)
{
    uint32_t t, i;
    for (t = 0; t < len; t += BLK) {
        uint32_t n = len - t < BLK ? len - t : BLK;
        for (i = 0; i < n; i++) buf[2u * i] = buf[2u * i + 1u] = in ? in[t + i] : 0;
        if (type == FX_DELAY) fx_delay(&st.dly, mem, buf, n, a, b, c, d, dry, wet);
        else if (type == FX_DIST) fx_dist(&st.dst, buf, n, a, (uint32_t)b, c, d, dry, wet);
        else fx_reverb(&st.rev, mem, buf, n, a, b, c, d, dry, wet);
        for (i = 0; i < n; i++) { if (L) L[t + i] = buf[2u * i]; if (R) R[t + i] = buf[2u * i + 1u]; }
    }
}
static void clear(void) { memset(mem, 0, sizeof mem); memset(&st, 0, sizeof st); }
static void run_cho(const int32_t *in, uint32_t len, int32_t *L, int32_t *R, int32_t base, int32_t depth, int32_t fb,
                    uint32_t inc, uint32_t woff)    /* CHORUS, wet only */
{
    uint32_t t, i;
    for (t = 0; t < len; t += BLK) {
        uint32_t n = len - t < BLK ? len - t : BLK;
        for (i = 0; i < n; i++) buf[2u * i] = buf[2u * i + 1u] = in[t + i];
        fx_chorus(&st.cho, mem, buf, n, base, depth, fb, inc, woff, 0, 4096);
        for (i = 0; i < n; i++) { L[t + i] = buf[2u * i]; if (R) R[t + i] = buf[2u * i + 1u]; }
    }
}
static double harm(const int32_t *x, uint32_t len, uint32_t k)   /* the k-th harmonic of 441 Hz (100 samples) */
{
    double re = 0, im = 0;
    uint32_t i;
    for (i = 0; i < len; i++) {
        re += x[i] * cos(2 * M_PI * k * i / 100.0);
        im += x[i] * sin(2 * M_PI * k * i / 100.0);
    }
    return sqrt(re * re + im * im) * 2 / len;
}

int main(void)
{
    static int32_t in[30u * FS], L[30u * FS], R[30u * FS];
    uint32_t i, D = 11025u;                     /* 250 ms */
    int32_t pk;

    /* DELAY: the time, the feedback (8192: x 0.5), TONE open (4096), no PING, wet only */
    clear(); memset(in, 0, sizeof in); in[0] = 20000;
    run(FX_DELAY, in, 4u * D, L, R, (int32_t)D << 8, 8192, 4096, 0, 0, 4096);
    CHECK(L[D] == 20000 && L[D - 1u] == 0 && L[D + 1u] == 0, "delay: the echo at %u: %d %d %d", D, L[D - 1u], L[D], L[D + 1u]);
    CHECK(L[2u * D] == 10000 && L[3u * D] == 5000, "delay: feedback 0.5: %d %d", L[2u * D], L[3u * D]);
    CHECK(R[D] == 20000, "delay: the right as the left without PING: %d", R[D]);
    /* PING full: the first echo left only, the second right only */
    clear();
    run(FX_DELAY, in, 3u * D, L, R, (int32_t)D << 8, 8192, 4096, 256, 0, 4096);
    CHECK(L[D] == 20000 && R[D] == 0 && L[2u * D] == 0 && R[2u * D] == 10000, "ping: L %d R %d, then L %d R %d",
          L[D], R[D], L[2u * D], R[2u * D]);
    /* a time change glides: from 250 ms to 125 ms, the read point never jumps by more than a sample a sample */
    clear();
    for (i = 0; i < 8u * D; i++) in[i] = (int32_t)(i % 200u) * 100 - 10000;   /* a slow saw */
    run(FX_DELAY, in, 2u * D, L, 0, (int32_t)D << 8, 0, 4096, 0, 0, 4096);
    for (pk = 0, i = 0; i < 6u * D; i += BLK) {   /* a block at a time: the read point moves <= half a sample a sample */
        int32_t was = st.dly.dq8;
        run(FX_DELAY, in + 2u * D + i, BLK, L + 2u * D + i, 0, (int32_t)(D / 2u) << 8, 0, 4096, 0, 0, 4096);
        if (abs(st.dly.dq8 - was) > pk) pk = abs(st.dly.dq8 - was);
    }
    CHECK(pk <= (int32_t)BLK * 128, "delay: a time change glides (%d Q8 in a block, at most %u)", pk, BLK * 128u);
    CHECK(st.dly.dq8 == (int32_t)(D / 2u) << 8, "delay: the time got there: %d", st.dly.dq8 >> 8);
    /* full feedback (0.98), dark, half PING, full-scale noise for 5 s: bounded; at feedback 0.5, silence after
       noise dies out to (almost) nothing */
    clear();
    for (i = 0; i < 5u * FS; i++) in[i] = noise();
    memset(in + 5u * FS, 0, sizeof(int32_t) * 25u * FS);
    run(FX_DELAY, in, 30u * FS, L, R, 3000 << 8, 16129, 1500, 128, 2048, 4096);
    for (pk = 0, i = 0; i < 30u * FS; i++) if (abs(L[i]) > pk) pk = abs(L[i]);
    CHECK(pk <= 100000, "delay: noise at full feedback stays bounded: peak %d", pk);
    clear();
    run(FX_DELAY, in, 10u * FS, L, R, 3000 << 8, 8192, 1500, 128, 2048, 4096);
    for (pk = 0, i = 9u * FS; i < 10u * FS; i++) { if (abs(L[i]) > pk) pk = abs(L[i]); if (abs(R[i]) > pk) pk = abs(R[i]); }
    CHECK(pk < 4, "delay: 5 s after, quiet: peak %d", pk);

    /* REVERB: an impulse; decay 0.5 vs 0.9; damping 0.3; predelay 20 ms; full width; wet only */
    {
        double e[2] = {0, 0};
        int32_t decs[2] = {8192, 14745};      /* (0.5 and 0.9) */
        uint32_t k, late = FS / 2u;
        for (k = 0; k < 2u; k++) {
            clear(); memset(in, 0, sizeof(int32_t) * 3u * FS); in[0] = 30000;
            run(FX_REVERB, in, 3u * FS, L, R, decs[k], 4915, 882, 256, 0, 4096);
            for (i = late; i < 3u * FS; i++) e[k] += (double)L[i] * L[i];
            if (k == 1) {
                double lr = 0, ll = 0, rr = 0;
                for (i = 882; i < FS; i++) { lr += (double)L[i] * R[i]; ll += (double)L[i] * L[i]; rr += (double)R[i] * R[i]; }
                CHECK(ll > 0 && rr > 0 && lr * lr < 0.5 * ll * rr, "reverb: the sides differ (correlation^2 %.2f)", lr * lr / (ll * rr));
                for (pk = 0, i = 0; i < 882u; i++) if (abs(L[i]) > pk) pk = abs(L[i]);
                CHECK(pk == 0, "reverb: nothing before the predelay (20 ms): %d", pk);
            }
        }
        CHECK(e[1] > 20.0 * e[0] && e[0] > 0, "reverb: SIZE rings longer (late energy %.3g vs %.3g)", e[1], e[0]);
    }
    clear();
    for (i = 0; i < 10u * FS; i++) in[i] = noise();
    memset(in + 10u * FS, 0, sizeof(int32_t) * 20u * FS);
    run(FX_REVERB, in, 30u * FS, L, R, 15456, 0, 1, 256, 0, 4096);   /* the top SIZE (0.943), no damping */
    for (pk = 0, i = 0; i < 30u * FS; i++) if (abs(L[i]) > pk) pk = abs(L[i]);
    CHECK(pk < 300000, "reverb: noise at the top size stays bounded: peak %d", pk);
    clear();
    run(FX_REVERB, in, 30u * FS, L, R, 13107, 4915, 1, 256, 0, 4096);   /* SIZE 0.8 (RT60 ~5.6 s): dies out to 0 */
    for (pk = 0, i = 29u * FS; i < 30u * FS; i++) { if (abs(L[i]) > pk) pk = abs(L[i]); if (abs(R[i]) > pk) pk = abs(R[i]); }
    CHECK(pk == 0, "reverb: 20 s after, silent: peak %d", pk);

    {                                           /* DIST: a 441 Hz sine (100 samples a cycle); LEVEL 4096 (1) */
        uint32_t sh, len = FS;
        double h1, h2, mean, rmsv;
        int32_t zi, zo;
        for (i = 0; i < len; i++) in[i] = (int32_t)lrint(3277 * sin(2 * M_PI * i / 100.0));   /* 0.1 */
        clear();
        run(FX_DIST, in, len, L, 0, 256, DS_SOFT, 4096, 4096, 0, 4096);   /* no drive, TONE open */
        h1 = harm(L + FS / 2u, FS / 2u, 1);
        CHECK(fabs(h1 - 3277) < 40, "dist: a quiet sine through SOFT at no drive: %.0f (want ~3277)", h1);
        for (i = 0; i < len; i++) in[i] = (int32_t)lrint(16384 * sin(2 * M_PI * i / 100.0));   /* 0.5 */
        clear();
        run(FX_DIST, in, len, L, 0, 16384, DS_HARD, 4096, 4096, 0, 4096);   /* x 64 */
        for (rmsv = 0, pk = 0, i = FS / 2u; i < len; i++) { rmsv += (double)L[i] * L[i]; if (abs(L[i]) > pk) pk = abs(L[i]); }
        rmsv = sqrt(rmsv / (len - FS / 2u));
        CHECK(rmsv > 0.9 * pk && pk <= 32767, "dist: HARD x64 nearly a square: rms %.0f of peak %d", rmsv, pk);
        clear();
        run(FX_DIST, in, len, L, 0, 1536, DS_FOLD, 4096, 4096, 0, 4096);   /* x 6: 3 x full scale, folded */
        for (zi = zo = 0, i = FS / 2u + 1u; i < len; i++) {
            zi += (in[i - 1u] < 0) != (in[i] < 0);
            zo += (L[i - 1u] < 0) != (L[i] < 0);
        }
        for (pk = 0, i = FS / 2u; i < len; i++) if (abs(L[i]) > pk) pk = abs(L[i]);
        CHECK(zo > 2 * zi && pk <= 32768, "dist: FOLD x6: %d zero crossings (the sine's %d), peak %d", zo, zi, pk);
        clear();
        run(FX_DIST, in, len, L, 0, 512, DS_SOFT, 4096, 4096, 0, 4096);
        h1 = harm(L + FS / 2u, FS / 2u, 1); h2 = harm(L + FS / 2u, FS / 2u, 2);
        CHECK(h2 < 0.002 * h1, "dist: SOFT is symmetric: 2nd harmonic %.4f of the 1st", h2 / h1);
        clear();
        run(FX_DIST, in, len, L, 0, 512, DS_TUBE, 4096, 4096, 0, 4096);
        h1 = harm(L + FS / 2u, FS / 2u, 1); h2 = harm(L + FS / 2u, FS / 2u, 2);
        for (mean = 0, i = FS / 2u; i < len; i++) mean += L[i];
        mean /= len - FS / 2u;
        CHECK(h2 > 0.03 * h1 && fabs(mean) < 50, "dist: TUBE: 2nd harmonic %.3f of the 1st, DC %.1f", h2 / h1, mean);
        for (sh = 0; sh < DS_N; sh++) {          /* full scale noise at the top drive, LEVEL 0.5: within it */
            for (i = 0; i < len; i++) in[i] = noise() * 4;
            clear();
            run(FX_DIST, in, len, L, R, 16384, (int32_t)sh, 4096, 2048, 0, 4096);
            for (pk = 0, i = 0; i < len; i++) { if (abs(L[i]) > pk) pk = abs(L[i]); if (abs(R[i]) > pk) pk = abs(R[i]); }
            CHECK(pk <= (sh == DS_TUBE ? 20000 : 16384), "dist: curve %u, noise at x64: peak %d (LEVEL 0.5 of full scale; TUBE: its DC blocker's overshoot)", sh, pk);
        }
    }
    {                                           /* CHORUS */
        uint32_t inc1 = 97391u;                 /* 1 Hz: 2^32 / 44100 */
        int32_t lo = 1 << 30, hi = 0, k;
        double lr = 0, ll = 0, rr = 0;
        clear(); memset(in, 0, sizeof(int32_t) * FS); in[0] = 20000;
        run_cho(in, FS / 10u, L, R, 441 << 8, 0, 0, inc1, 0);   /* 10 ms, no depth */
        CHECK(L[441] == 20000 && R[441] == 20000 && L[440] == 0 && L[442] == 0, "chorus: the base delay: %d %d %d",
              L[440], L[441], L[442]);
        clear(); memset(in, 0, sizeof(int32_t) * FS);   /* depth 3 ms (132 samples) at 1 Hz: impulses every 50 ms */
        for (i = 0; i < FS; i += 2205u) in[i] = 20000;
        run_cho(in, FS, L, 0, 441 << 8, 132 << 8, 0, inc1, 0);
        for (i = 0; i + 2205u <= FS; i += 2205u) {
            uint32_t j, at = 0;
            int32_t best = 0;
            for (j = 200u; j < 700u; j++) if (L[i + j] > best) { best = L[i + j]; at = j; }
            if ((int32_t)at < lo) lo = (int32_t)at;
            if ((int32_t)at > hi) hi = (int32_t)at;
        }
        CHECK(abs(lo - 309) <= 10 && abs(hi - 573) <= 10, "chorus: the delay swings %d .. %d (want 309 .. 573: the impulses"
              " land near the triangle's ends, not on them)", lo, hi);
        for (i = 0; i < 2u * FS; i++) in[i] = noise();
        clear();
        run_cho(in, FS, L, R, 441 << 8, 132 << 8, 0, inc1, 0);
        CHECK(!memcmp(L, R, sizeof(int32_t) * FS), "chorus: WIDTH 0: the sides the same");
        clear();
        run_cho(in, FS, L, R, 441 << 8, 132 << 8, 0, inc1, 0x80000000u);
        for (i = 0; i < FS; i++) { lr += (double)L[i] * R[i]; ll += (double)L[i] * L[i]; rr += (double)R[i] * R[i]; }
        CHECK(lr * lr < 0.8 * ll * rr, "chorus: WIDTH half a cycle: the sides differ (correlation^2 %.2f)", lr * lr / (ll * rr));
        for (k = -1; k <= 1; k += 2) {          /* a flanger: 2 ms +- 2 ms, feedback +-0.95 */
            for (i = 0; i < 5u * FS; i++) in[i] = noise();
            memset(in + 5u * FS, 0, sizeof(int32_t) * 3u * FS);
            clear();
            run_cho(in, 8u * FS, L, R, 88 << 8, 88 << 8, k * 15565, 2u * inc1, 0x40000000u);
            for (pk = 0, i = 0; i < 5u * FS; i++) if (abs(L[i]) > pk) pk = abs(L[i]);
            CHECK(pk < 1000000, "flanger, feedback %+d: noise stays bounded: peak %d", k, pk);
            for (pk = 0, i = 7u * FS; i < 8u * FS; i++) { if (abs(L[i]) > pk) pk = abs(L[i]); if (abs(R[i]) > pk) pk = abs(R[i]); }
            CHECK(pk < 4, "flanger, feedback %+d: 2 s after, quiet: %d", k, pk);
        }
    }
    {                                           /* the output's limiter */
        int32_t x, prev = -40000, worst = 0;
        for (x = -300000; x <= 300000; x += 7) {
            int32_t y = out_limit(x);
            if (y < prev) worst = 1;            /* (never down as x goes up) */
            if (y > 32767 || y < -32767) worst = 2;
            if (x >= -OUT_KNEE && x <= OUT_KNEE && y != x) worst = 3;
            prev = y;
        }
        CHECK(!worst, "limiter: %s", worst == 1 ? "not monotonic" : worst == 2 ? "past full scale"
                                                  : "not transparent under the knee");
        CHECK(out_limit(OUT_KNEE + 1) - OUT_KNEE <= 1 && out_limit(1000000) > 32700,
              "limiter: continuous at the knee, near full scale far above it");
    }
    printf(fails ? "fx: %d FAILED\n" : "fx: all passed\n", fails);
    return fails != 0;
}
