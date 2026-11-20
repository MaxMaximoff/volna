/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna (2026-11-15): the modifiers' arithmetic and the LFOs' speed modulation (engine/mod.h) on the host.
 *   1. modf_run: + - * MAX MIN on a grid of values, against plain arithmetic clamped to +-32767; AND OR XOR on
 *      the values as 16 bits (0..65535); every result within +-32767;
 *   2. modf_const: -64 .. 63 rising, 0 at 0, 63 near 1, -64 at -1;
 *   3. lfo_speed: 0 moves nothing; +-k RATE steps from RATE x land within 1.5 % of RATE x +- k (both ways, all x and
 *      k within the table); far past the ends: RATE 127's step at most, 1 at least; rising with m;
 *   4. mod_scale of the newer destinations (L.SPD, AMT): +-127 at +-63.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/modf_test tests/modf_test.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "engine/mod.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int32_t clampq(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }

int main(void)
{
    static const int32_t V[] = {-32767, -20000, -16384, -1, 0, 1, 1000, 16384, 25000, 32767};
    uint32_t i, j, op, x;
    int32_t k, m, prev;

    for (i = 0; i < sizeof V / sizeof V[0]; i++)    /* 1 */
        for (j = 0; j < sizeof V / sizeof V[0]; j++) {
            int32_t a = V[i], b = V[j];
            CHECK(modf_run(a, b, MO_ADD) == clampq(a + b), "%d + %d -> %d", a, b, modf_run(a, b, MO_ADD));
            CHECK(modf_run(a, b, MO_SUB) == clampq(a - b), "%d - %d -> %d", a, b, modf_run(a, b, MO_SUB));
            CHECK(modf_run(a, b, MO_MUL) == clampq((a * b) >> 15), "%d * %d -> %d", a, b, modf_run(a, b, MO_MUL));
            CHECK(modf_run(a, b, MO_MAX) == (a > b ? a : b), "MAX %d %d", a, b);
            CHECK(modf_run(a, b, MO_MIN) == (a < b ? a : b), "MIN %d %d", a, b);
            CHECK(modf_run(a, b, MO_AND) == clampq((int32_t)(((uint32_t)(a + 32768) & (uint32_t)(b + 32768)) & 0xFFFFu) - 32768),
                  "%d AND %d", a, b);
            CHECK(modf_run(a, b, MO_XOR) == clampq((int32_t)(((uint32_t)(a + 32768) ^ (uint32_t)(b + 32768)) & 0xFFFFu) - 32768),
                  "%d XOR %d", a, b);
            for (op = 0; op < MO_N; op++) {
                int32_t r = modf_run(a, b, op);
                CHECK(r >= -32767 && r <= 32767, "op %u: %d, %d -> %d out of range", op, a, b, r);
            }
        }
    CHECK(modf_run(32767, 32767, MO_MUL) > 32760 && modf_run(-32767, 32767, MO_MUL) < -32760, "1 x 1, -1 x 1");
    CHECK(modf_run(0, 0, MO_XOR) == -32767, "0 XOR 0: %d (the bits equal: -1)", modf_run(0, 0, MO_XOR));
    CHECK(modf_run(5000, 5000, MO_OR) == 5000 && modf_run(5000, 5000, MO_AND) == 5000, "a OR a, a AND a: a");

    for (k = -64, prev = -40000; k <= 63; k++) {  /* 2 */
        int32_t c = modf_const(k);
        CHECK(c > prev && c >= -32767 && c <= 32767, "const %d -> %d (after %d)", k, c, prev);
        prev = c;
    }
    CHECK(modf_const(0) == 0 && modf_const(63) > 32700 && modf_const(-64) == -32767, "const ends: %d %d %d",
          modf_const(0), modf_const(63), modf_const(-64));

    for (x = 0; x < 128u; x++) {                  /* 3 */
        CHECK(lfo_speed(LFO_INC[x], 0) == LFO_INC[x], "RATE %u moved by 0", x);
        for (m = -127; m <= 127; m += 7) {
            int32_t t = (int32_t)x + m;
            uint32_t got = lfo_speed(LFO_INC[x], m);
            if (t >= 0 && t <= 127) {
                double r = (double)got / LFO_INC[t];
                CHECK(fabs(r - 1) < 0.015, "RATE %u %+d: %u, want %u (x%.4f)", x, m, got, LFO_INC[t], r);
            }
            CHECK(got >= 1u && got <= LFO_INC[127], "RATE %u %+d: %u past the ends", x, m, got);
        }
        for (m = -127, prev = 0; m <= 127; m++) {
            uint32_t got = lfo_speed(LFO_INC[x], m);
            CHECK((int64_t)got >= prev, "RATE %u: %+d slower than %+d", x, m, m - 1);
            prev = (int32_t)got;
        }
    }

    CHECK(mod_scale(MD_LSPD1, 63) == 127 && mod_scale(MD_AMT4, -63) == -127, "L.SPD, AMT: +-127 at +-63");

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("modf_test: ok\n");
    return 0;
}
