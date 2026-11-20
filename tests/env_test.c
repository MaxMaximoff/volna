/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna stage 2: the ADSR envelope (engine/env.h) on the host.
 *   1. the attack reaches full in its time (within a step), for every setting;
 *   2. the decay settles on the sustain level;
 *   3. the release reaches the floor and goes idle, in about its time (to -60 dB) plus the tail to -78 dB;
 *   4. a retrigger during the release starts the attack from where the level is: no jump;
 *   5. the level never leaves 0 .. ENV_FULL;
 *   6. the modes (stage 4b): ADS1DS2R holds at S2, ONE SHOT ends with the key held and ignores the key let go,
 *      LOOP S1S2 turns between S and S2, LOOP ALL attacks again; a new note above the attack level: no jump.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/env_test tests/env_test.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "engine/env.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static double ms_of(uint32_t x) { return pow(10000.0, x / 127.0); }
static double steps_of(double ms) { return ms * 44100.0 / 1000.0 / ENV_SUB; }

int main(void)
{
    env_rates_t r;
    env_t e;
    uint32_t x, n;

    for (x = 0; x < 128u; x++) {                /* 1 */
        double want = steps_of(ms_of(x));
        env_rates(&r, x, 64, 127, 64);
        e.lvl = 0; e.st = ENV_IDLE;
        env_on(&e);
        for (n = 0; e.st == ENV_ATTACK && n < 100000u; n++) {
            env_step(&e, &r);
            CHECK(e.lvl >= 0 && e.lvl <= ENV_FULL, "attack %u: level %d out of range", x, e.lvl);
        }
        CHECK(fabs(n - (want < 1 ? 1 : want)) <= 1.0, "attack %u: %u steps, want %.1f", x, n, want);
    }

    for (x = 0; x < 128u; x += 9u) {            /* 2 */
        env_rates(&r, 0, 40, x, 64);
        e.lvl = 0; e.st = ENV_IDLE;
        env_on(&e);
        for (n = 0; n < 20000u; n++) env_step(&e, &r);
        CHECK(abs(e.lvl - r.sus) <= ENV_FULL / 2000, "sustain %u: level %d, want %d", x, e.lvl, r.sus);
    }

    for (x = 0; x < 128u; x += 7u) {            /* 3 */
        double want = steps_of(ms_of(x)) * log(ENV_FULL / (double)ENV_FLOOR) / log(1000.0);
        env_rates(&r, 0, 0, 127, x);
        e.lvl = ENV_FULL; e.st = ENV_DECAY;
        env_off(&e, &r);
        for (n = 0; e.st != ENV_IDLE && n < 2000000u; n++) {
            int32_t before = e.lvl;
            env_step(&e, &r);
            CHECK(e.lvl <= before && e.lvl >= 0, "release %u: level rose or went negative", x);
        }
        CHECK(e.st == ENV_IDLE && e.lvl == 0, "release %u: never idle", x);
        CHECK(fabs(n - want) <= 0.02 * want + 2.0, "release %u: %u steps, want ~%.0f", x, n, want);
    }

    {                                           /* 4 */
        int32_t before;
        env_rates(&r, 64, 64, 100, 90);
        e.lvl = ENV_FULL / 3; e.st = ENV_RELEASE;
        before = e.lvl;
        env_on(&e);
        env_step(&e, &r);
        CHECK(e.st == ENV_ATTACK && e.lvl > before && e.lvl - before <= (int32_t)r.atk,
              "retrigger jumped: %d -> %d", before, e.lvl);
    }

    {                                           /* 6: the modes (stage 4b) */
        uint32_t st, k, seen2 = 0, flips = 0, attacks = 0;
        env_rates(&r, 10, 30, 90, 40);          /* ADS1DS2R: attack to 100, decay to 90, decay 2 to 30, held */
        env_mode(&r, EM_ADS1DS2R, 100, 30, 30);
        e.lvl = 0; e.st = ENV_IDLE; env_on(&e);
        for (n = 0; n < 20000u; n++) { env_step(&e, &r); if (e.lvl > env_level(100)) break; }
        CHECK(n == 20000u, "ADS1DS2R: above the attack level");
        CHECK(e.st == ENV_DECAY2 && abs(e.lvl - env_level(30)) < ENV_NEAR, "ADS1DS2R: not held at S2 (stage %u)", e.st);
        env_mode(&r, EM_ONESHOT, 100, 30, 30);  /* ONE SHOT: the key held, it ends anyway; the key let go: no matter */
        e.lvl = 0; e.st = ENV_IDLE; env_on(&e);
        for (n = 0; n < 200000u && e.st != ENV_IDLE; n++) env_step(&e, &r);
        CHECK(e.st == ENV_IDLE, "ONE SHOT: never over with the key held");
        e.lvl = 0; e.st = ENV_IDLE; env_on(&e);
        env_step(&e, &r);
        env_off(&e, &r);
        CHECK(e.st == ENV_ATTACK, "ONE SHOT: the key let go stopped the attack");
        env_mode(&r, EM_LOOP12, 100, 30, 30);   /* LOOP S1S2: between 90 and 30, on and on */
        e.lvl = 0; e.st = ENV_IDLE; env_on(&e);
        for (n = 0, st = e.st; n < 200000u; n++) {
            env_step(&e, &r);
            if (e.st != st) { if (e.st == ENV_DECAY2) seen2++; if (e.st == ENV_DECAY && st == ENV_DECAY2) flips++; st = e.st; }
            if (e.st == ENV_ATTACK && n > 2000u) { CHECK(0, "LOOP S1S2: back in the attack"); break; }
        }
        CHECK(flips >= 3 && seen2 >= 3, "LOOP S1S2: %u turns", flips);
        env_off(&e, &r);
        for (n = 0; n < 200000u && e.st != ENV_IDLE; n++) env_step(&e, &r);
        CHECK(e.st == ENV_IDLE, "LOOP S1S2: the key let go, never over");
        env_mode(&r, EM_LOOPALL, 100, 30, 30);  /* LOOP ALL: back to the attack */
        e.lvl = 0; e.st = ENV_IDLE; env_on(&e);
        for (n = 0, st = e.st; n < 200000u; n++) {
            env_step(&e, &r);
            if (e.st != st) { if (e.st == ENV_ATTACK) attacks++; st = e.st; }
        }
        CHECK(attacks >= 3, "LOOP ALL: %u attacks again", attacks);
        env_mode(&r, EM_LOOPALL, 50, 30, 90);   /* LOOP ALL from 0 each turn: even with S2 above the attack level */
        e.lvl = 0; e.st = ENV_IDLE; env_on(&e);
        for (n = 0, st = e.st, attacks = 0, k = 0; n < 200000u; n++) {
            int32_t was = e.lvl;
            env_step(&e, &r);
            if (e.st != st) {
                if (e.st == ENV_ATTACK) { attacks++; if (was > ENV_NEAR && e.lvl != 0) k++; }
                st = e.st;
            }
        }
        CHECK(attacks >= 3 && k == 0, "LOOP ALL, S2 above A.LVL: %u attacks again, %u not from 0", attacks, k);
        env_mode(&r, EM_ADS1DS2R, 50, 30, 30);  /* a new note above the attack level: no jump down */
        e.lvl = env_level(80); e.st = ENV_RELEASE;
        env_on(&e);
        k = (uint32_t)e.lvl;
        env_step(&e, &r);
        CHECK(e.st == ENV_DECAY && e.lvl == (int32_t)k, "retrigger above the attack level: %d -> %d (stage %u)",
              (int32_t)k, e.lvl, e.st);
        env_step(&e, &r);
        CHECK(e.lvl != (int32_t)k && abs(e.lvl - r.sus) < abs((int32_t)k - r.sus),
              "retrigger above the attack level: the decay does not go on towards S");
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("env: OK\n");
    return 0;
}
