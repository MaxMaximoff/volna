/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna engine: the envelopes (stage 2; the modes, stage 4b), in the spirit of the Blofeld's.
 *
 * The level is Q30 (ENV_FULL = 1 << 30) and moves a step every ENV_SUB samples (the caller ramps the samples in
 * between). Attack: linear, up to full (ADSR) or to the attack level. Decay, decay 2: exponential towards sustain,
 * sustain 2. Release: exponential towards 0; below -78 dB the envelope is idle (the voice free, for AMP).
 * A new note starts the attack from the level where the envelope is (no click); if it is above the attack's top
 * already, the decay goes on from there (no jump down).
 * The modes:
 *   ADSR       attack to full, decay to S, held, release;
 *   ADS1DS2R   attack to the attack level, decay to S, decay 2 to S2, held, release;
 *   ONE SHOT   the same without holding: after S2 the release, whatever the key does;
 *   LOOP S1S2  as ADS1DS2R, but from S2 back to the decay towards S, and so on, while the key is down;
 *   LOOP ALL   as ADS1DS2R, but from S2 back to 0 and the attack, and so on, while the key is down (the Blofeld's
 *              way: a step down to 0 each turn, an LFO of its own shape; on AMP it may click, the patch's choice).
 * A decay is over (on to the next stage) within ENV_NEAR of its target. The rates come from env_rates() and
 * env_mode() in the main loop (tables in firmware/gen/env_tables.h); a step only multiplies and adds. */
#ifndef VOLNA_ENV_H
#define VOLNA_ENV_H
#include <stdint.h>
#include "env_tables.h"

#define ENV_FULL (1 << 30)
#define ENV_SUB 16u
#define ENV_FLOOR (ENV_FULL >> 13)              /* -78 dB: the release ends here */
#define ENV_NEAR (ENV_FULL >> 10)               /* -60 dB of full: a decay has got there */

enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_RELEASE, ENV_DECAY2 };   /* (the order the screen knows) */
enum { EM_ADSR, EM_ADS1DS2R, EM_ONESHOT, EM_LOOP12, EM_LOOPALL, EM_N };

typedef struct { int32_t lvl; uint8_t st; } env_t;
typedef struct {
    volatile uint32_t atk, dec, dec2, rel;
    volatile int32_t sus, sus2, top;            /* S, S2, where the attack ends (ADSR: full) */
    volatile uint8_t mode;
} env_rates_t;

static int32_t env_level(uint32_t x) { return (int32_t)((x > 127u ? 127u : x) * (uint32_t)(ENV_FULL / 127)); }

/* the rates of the parameters a, d, s, r (0..127): an ADSR (env_mode after it for the others) */
static void env_rates(env_rates_t *r, uint32_t a, uint32_t d, uint32_t s, uint32_t rl)
{
    r->atk = ENV_ATK[a & 127u];
    r->dec = ENV_COEF[d & 127u];
    r->rel = ENV_COEF[rl & 127u];
    r->sus = env_level(s);
    r->dec2 = r->dec;
    r->sus2 = r->sus;
    r->top = ENV_FULL;
    r->mode = EM_ADSR;
}
/* the mode and its parameters: the attack level al, decay 2 d2, sustain 2 s2 (0..127) */
static void env_mode(env_rates_t *r, uint32_t mode, uint32_t al, uint32_t d2, uint32_t s2)
{
    r->dec2 = ENV_COEF[d2 & 127u];
    r->sus2 = env_level(s2);
    r->top = mode == EM_ADSR ? ENV_FULL : env_level(al);
    r->mode = (uint8_t)(mode < EM_N ? mode : EM_ADSR);
}

static inline void env_on(env_t *e) { e->st = ENV_ATTACK; }
static inline void env_off(env_t *e, const env_rates_t *r)   /* the key let go (ONE SHOT: it does not care) */
{
    if (r->mode != EM_ONESHOT && (e->st == ENV_ATTACK || e->st == ENV_DECAY || e->st == ENV_DECAY2))
        e->st = ENV_RELEASE;
}

/* a voice taken (Volna's voices' limit, engine/poly.h): out in ~6 ms whatever its envelope (x 0.6 a step of 16
 * samples: -78 dB in 18 steps); returns the level, Q30 */
static inline int32_t env_kill(env_t *e)
{
    if (e->st != ENV_IDLE) {
        e->lvl = (int32_t)(((int64_t)e->lvl * 644245094) >> 30);
        if (e->lvl < ENV_FLOOR) { e->lvl = 0; e->st = ENV_IDLE; }
        else e->st = ENV_RELEASE;
    }
    return e->lvl;
}

/* one step; returns the level, Q30 */
static inline int32_t env_step(env_t *e, const env_rates_t *r)
{
    int32_t d;
    switch (e->st) {
    case ENV_ATTACK:
        if (e->lvl >= r->top) { e->st = ENV_DECAY; break; }   /* (above the top already: on from here) */
        e->lvl += (int32_t)r->atk;              /* < 2^30 + 2^30: no overflow */
        if (e->lvl >= r->top) { e->lvl = r->top; e->st = ENV_DECAY; }
        break;
    case ENV_DECAY:
        e->lvl = r->sus + (int32_t)(((int64_t)(e->lvl - r->sus) * r->dec) >> 30);   /* (int64 multiply: fine) */
        d = e->lvl - r->sus;
        if (r->mode != EM_ADSR && d < ENV_NEAR && d > -ENV_NEAR) e->st = ENV_DECAY2;
        break;
    case ENV_DECAY2:
        e->lvl = r->sus2 + (int32_t)(((int64_t)(e->lvl - r->sus2) * r->dec2) >> 30);
        d = e->lvl - r->sus2;
        if (d < ENV_NEAR && d > -ENV_NEAR) {
            if (r->mode == EM_LOOPALL) { e->lvl = 0; e->st = ENV_ATTACK; }   /* (from 0: a step down, on purpose) */
            else e->st = r->mode == EM_ONESHOT ? ENV_RELEASE : r->mode == EM_LOOP12 ? ENV_DECAY : ENV_DECAY2;
        }
        break;
    case ENV_RELEASE:
        e->lvl = (int32_t)(((int64_t)e->lvl * r->rel) >> 30);
        if (e->lvl < ENV_FLOOR) { e->lvl = 0; e->st = ENV_IDLE; }
        break;
    default:
        e->lvl = 0;
        break;
    }
    return e->lvl;
}
#endif
