/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna engine: modulation (stage 4a): three LFOs and an 8-slot matrix (16 slots since 2026-11-14), in the spirit of the Blofeld's;
 * the Blofeld's four modifiers and the rest of its sources and destinations since 2026-11-15.
 *
 * An LFO is a 32-bit phase stepping every ENV_SUB samples (LFO_INC, tools/gen_lfo.py), its value Q15 bipolar:
 * SINE (a cubic from the triangle, within 2 %), TRI, SQR, SAW (rising), RAND (a new random level every cycle,
 * glided to across it), S&H (a new random level every cycle, held). With KEY on it restarts at PHASE on each note,
 * a voice its own; with KEY off one LFO runs for all the voices. DELAY fades it in after a note (the envelope's
 * attack times, tools/gen_env.py).
 *
 * A slot: a source (Q15: LFOs and KEY bipolar, the rest 0..1), a destination, an amount -63..63. The main loop turns
 * the amount into a scale in the destination's units (mod_scale); the audio ISR adds source x scale >> 15 into the
 * destination, a voice at a time, every ENV_SUB samples. Integers only, products under 2^31.
 *
 * A modifier (as the Blofeld's): source A, an operation, source B or (B OFF) a constant -64..63; its value is a source
 * (MOD1..MOD4) the slots and the other modifiers read. They run in order, 1 to 4: a modifier reading a later one gets
 * its value of the last step (16 samples before). The bit operations work on the values as 16 bits, 0..65535 (-1 .. 1
 * shifted up): digital, stepped results, as on the Blofeld. */
#ifndef VOLNA_MOD_H
#define VOLNA_MOD_H
#include <stdint.h>
#include "lfo_tables.h"

#define NLFO 3u
static inline uint32_t mod_pitch_q20(int32_t n);
#define NMSLOT 16u                              /* the matrix's slots */
#define NMSLOT_A 8u                             /* the first 8 (patch_t's mod[]; the rest: mod2[]) */

enum { LS_SINE, LS_TRI, LS_SQR, LS_SAW, LS_RAND, LS_SH, LS_N };
enum { MS_OFF, MS_LFO1, MS_LFO2, MS_LFO3, MS_ENVAMP, MS_ENVFLT, MS_VELO, MS_KEY, MS_MACRO, MS_WHEEL, MS_RAND,
       MS_ENV3, MS_ENV4,
       MS_PRESS, MS_PPRESS, MS_BEND, MS_SUST,   /* 2026-11-15: aftertouch, poly pressure (the voice's note), the */
       MS_BREATH, MS_FOOT,                      /* pitch bend, the sustain pedal; CC 2, CC 4; */
       MS_CTLW, MS_CTLX, MS_CTLY, MS_CTLZ,      /* four CCs SETUP picks; */
       MS_L1MW, MS_L2PR,                        /* LFO 1 x the wheel, LFO 2 x aftertouch; */
       MS_MIN, MS_MAX,                          /* the constants -1, +1; */
       MS_MOD1, MS_MOD2, MS_MOD3, MS_MOD4,      /* the modifiers */
       MS_N };                                  /* (new ones at the end: the patches' slots keep their meaning) */
enum { MD_OFF, MD_PITCH, MD_P1, MD_P2, MD_P3, MD_WAVE, MD_W1, MD_W2, MD_W3, MD_L1, MD_L2, MD_L3, MD_NOISE, MD_RING,
       MD_CUT, MD_CUT1, MD_CUT2, MD_RES1, MD_RES2, MD_VOL,
       MD_AATK, MD_ADEC, MD_ASUS, MD_AREL, MD_FATK, MD_FDEC, MD_FSUS, MD_FREL,   /* the envelopes' A D S R: AMP, FLT, */
       MD_3ATK, MD_3DEC, MD_3SUS, MD_3REL, MD_4ATK, MD_4DEC, MD_4SUS, MD_4REL,   /* ENV 3, ENV 4 (+-127 at 63) */
       MD_DET1, MD_DET2, MD_DET3,               /* unison DETUNE (x 0 .. x 3 of the knob's) */
       MD_COLOR, MD_BAL1, MD_BAL2, MD_BAL3, MD_BALN, MD_BALR,   /* the noise's colour; each source's F1 <-> F2 */
       MD_DRV1, MD_DRV2, MD_FENV1, MD_FENV2,    /* the filters' drive, their envelope's reach */
       MD_PAN,                                  /* the voice across the stereo field */
       MD_LSPD1, MD_LSPD2, MD_LSPD3,            /* 2026-11-15: the LFOs' speed (+-127 RATE steps: their whole range) */
       MD_AMT1, MD_AMT2, MD_AMT3, MD_AMT4,      /* slots 1..4's amount (+-127: +-63 of it), as the Blofeld's M1..M4 */
       MD_FM1, MD_FM2, MD_FM3,                  /* the oscillators' FM AMT (+-127: the knob's whole 0..127) */
       MD_FFM1, MD_FFM2, MD_FPAN1, MD_FPAN2,    /* 2026-11-16: the filters' FM AMT (+-127), their PAN (+-127: all the
                                                   way, either side) */
       MD_N };                                  /* (new ones at the end: the patches' slots keep their meaning) */

typedef struct {                                /* an LFO's settings (they live in the patch) */
    uint8_t shape, rate, sync, delay, key, phase;   /* LS_*; RATE 0..127 (or, synced, an ARP_RATE_TICKS index);
                                                   0/1; 0..127 (env attack times); 0/1; 0..127 (start, KEY on) */
} lfo_set_t;
typedef struct { uint8_t src, dst; int8_t amt; } mod_slot_t;
#define NMODF 4u                                /* the modifiers */
enum { MO_ADD, MO_SUB, MO_MUL, MO_AND, MO_OR, MO_XOR, MO_MAX, MO_MIN, MO_N };
typedef struct { uint8_t a, b, op; int8_t c; } modf_set_t;   /* MS_* A and B (B OFF: the constant c, -64..63), MO_* */

typedef struct { uint32_t ph, rnd; int32_t from, to; } lfo_t;   /* an LFO running: phase; the random levels */

static uint32_t lfo_rand(lfo_t *l)              /* xorshift32, a bipolar Q15 level */
{
    uint32_t x = l->rnd ? l->rnd : 0x6C8E9CF5u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    l->rnd = x;
    return x;
}
static void lfo_start(lfo_t *l, uint32_t phase) /* KEY: from PHASE (0..127 of a cycle) */
{
    l->ph = phase << 25;
    l->from = l->to;
}
/* a step: the phase on by inc; returns the value, Q15 (-32767 .. 32767) */
static inline int32_t lfo_step(lfo_t *l, uint32_t shape, uint32_t inc)
{
    uint32_t ph = l->ph + inc;
    int32_t t, u;
    if (ph < l->ph && (shape == LS_RAND || shape == LS_SH)) {   /* a cycle over: a new random level */
        l->from = shape == LS_RAND ? l->to : 0;
        l->to = (int32_t)(lfo_rand(l) >> 17) - 16384;
        l->to *= 2;
    }
    l->ph = ph;
    switch (shape) {
    case LS_SQR: return ph < 0x80000000u ? 32767 : -32767;
    case LS_SAW: return (int32_t)(ph >> 16) - 32768 + 1;
    case LS_SH: return l->to;
    case LS_RAND: return l->from + (int32_t)(((int64_t)(l->to - l->from) * (int32_t)(ph >> 17)) >> 15);
    default: break;
    }
    u = (int32_t)((ph + 0x40000000u) >> 15);    /* 0 .. 131071: at phase 0 the middle, rising */
    t = u < 65536 ? u - 32768 : 98303 - u;      /* the triangle, -32768 .. 32767 */
    if (shape == LS_TRI) return t < -32767 ? -32767 : t;
    u = (t * t) >> 15;                          /* sine ~ 1.5 t - 0.5 t^3 */
    t = (t * (49152 - (u >> 1))) >> 15;
    return t > 32767 ? 32767 : t < -32767 ? -32767 : t;
}

/* a modifier: a op b, each Q15 bipolar (-32767..32767), the result too */
static inline int32_t modf_run(int32_t a, int32_t b, uint32_t op)
{
    int32_t r;
    switch (op) {
    case MO_SUB: r = a - b; break;
    case MO_MUL: r = (a * b) >> 15; break;
    case MO_AND: r = (int32_t)(((uint32_t)(a + 32768) & (uint32_t)(b + 32768)) & 0xFFFFu) - 32768; break;
    case MO_OR: r = (int32_t)(((uint32_t)(a + 32768) | (uint32_t)(b + 32768)) & 0xFFFFu) - 32768; break;
    case MO_XOR: r = (int32_t)(((uint32_t)(a + 32768) ^ (uint32_t)(b + 32768)) & 0xFFFFu) - 32768; break;
    case MO_MAX: r = a > b ? a : b; break;
    case MO_MIN: r = a < b ? a : b; break;
    default: r = a + b; break;
    }
    return r > 32767 ? 32767 : r < -32767 ? -32767 : r;
}
static inline int32_t modf_const(int32_t c)     /* a modifier's constant -64..63, Q15 (63: 32760, -64: -32767) */
{
    c *= 520;
    return c < -32767 ? -32767 : c;
}

/* an LFO's phase step moved by m RATE steps (the matrix's L.SPD, +-127): RATE is exponential, 2500^(1/127) a step
 * (1.067 semitones), so the step is scaled by 2^(m x 1.067 / 12), at most to RATE 127's (50 Hz), at least 1 */
static __attribute__((noinline)) uint32_t lfo_speed(uint32_t inc, int32_t m)   /* (out of app_audio) */
{
    int32_t s = m * 273;                        /* Q8 semitones */
    uint64_t x = inc;
    if (!m) return inc;
    while (s > 48 * 256) { x <<= 4; s -= 48 * 256; }   /* (mod_pitch_q20: +-48 semitones a go) */
    while (s < -48 * 256) { x >>= 4; s += 48 * 256; }
    x = (x * mod_pitch_q20(s)) >> 20;           /* (64-bit multiply: fine; x < 2^36 x 2^24) */
    return x > LFO_INC[127] ? LFO_INC[127] : x < 1u ? 1u : (uint32_t)x;
}

/* the phase step of an LFO: free (RATE, LFO_INC) or synced to bpm (a cycle of ticks MIDI clocks) */
static uint32_t lfo_inc(const lfo_set_t *s, uint32_t bpm, const uint8_t *ticks, uint32_t nticks)
{
    if (!s->sync) return LFO_INC[s->rate & 127u];
    return LFO_SYNC_K * (bpm ? bpm : 120u) / (5u * ticks[s->rate < nticks ? s->rate : nticks - 1u]);
}

/* a free LFO's speed for the screen: Hz x 100, rounded (1 Hz = a step of 1558259; truncated, RATE 0's 0.02 Hz read
 * 0.01, RATE 26's 0.099 Hz 0.09) */
static uint32_t lfo_hz100(uint32_t inc)
{
    return (inc + 7791u) / 15583u;
}

/* an amount (-63..63) in its destination's units, for source x scale >> 15 (the newer ones: +-127 at 63, read as
 * the knob's own steps: DETUNE, COLOR, BAL (0..128 of filter 2's share), DRIVE, ENV, PAN (all left .. all right)):
 * pitch Q8 semitones, +-12 at 63, the amount squared (fine for a vibrato near 0); a wave position Q8 waves, +-32;
 * a level +-127; a cutoff Q8 semitones, +-64; a resonance, the volume +-127 */
static int32_t mod_scale(uint32_t dst, int32_t amt)
{
    if (amt > 63) amt = 63;
    if (amt < -63) amt = -63;
    switch (dst) {
    case MD_PITCH: case MD_P1: case MD_P2: case MD_P3: return amt * (amt < 0 ? -amt : amt) * (12 * 256) / 3969;
    case MD_WAVE: case MD_W1: case MD_W2: case MD_W3: return amt * (32 * 256) / 63;
    case MD_CUT: case MD_CUT1: case MD_CUT2: return amt * (64 * 256) / 63;
    case MD_OFF: return 0;
    default: return amt * 127 / 63;             /* levels, resonance, volume */
    }
}

/* 2^(n / 12) for n Q8 semitones (-48..+48 semitones), Q20: the semitone table, linear within a semitone
 * (within 0.7 cent) */
static inline uint32_t mod_pitch_q20(int32_t n)
{
    static const uint16_t SEMI[13] = {32768, 34716, 36781, 38968, 41285, 43740, 46341, 49097, 52016, 55109, 58386,
                                      61858, 65535};   /* 2^(i/12) Q15 (the last 2.0, capped) */
    int32_t o, r, f;
    uint32_t m;
    if (n > 48 * 256) n = 48 * 256;
    if (n < -48 * 256) n = -48 * 256;
    n += 48 * 256;                              /* 0 .. 96 semitones, Q8 */
    o = n / (12 * 256);
    r = (n >> 8) - 12 * o;
    f = n & 255;
    m = SEMI[r] + (uint32_t)(((int32_t)(SEMI[r + 1] - SEMI[r]) * f) >> 8);   /* Q15, 1 .. 2 */
    if (r == 11) m = SEMI[11] + (uint32_t)(((65536 - SEMI[11]) * f) >> 8);   /* (to 2.0 exactly) */
    return o >= 4 ? m << (1 + o) : (m << 5) >> (4 - o);   /* Q15 x 2^(o - 4) -> Q20 */
}
#endif
