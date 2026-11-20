/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna engine: pitch offsets as a Q20 multiplier of the phase step (main loop: no divisions in the ISR).
 * semi: -48..+48 semitones, cents: -50..+50. 1 << 20 = unison. Q20 rather than Q16 so that octaves down keep
 * their precision (Q16 shifted right three octaves is 0.24 cent coarse); 4 octaves up still fit in 32 bits. */
#ifndef VOLNA_PITCH_H
#define VOLNA_PITCH_H
#include <stdint.h>

#define PITCH_ONE (1u << 20)

static const uint32_t SEMI_Q16[12] = {          /* 2^(i/12) * 65536 */
    65536u, 69433u, 73562u, 77936u, 82570u, 87480u, 92682u, 98193u, 104032u, 110218u, 116772u, 123715u};

static uint32_t pitch_mul_q20(int32_t semi, int32_t cents)
{
    int32_t o, r, sh;
    uint32_t m, f;
    if (semi < -48) semi = -48;
    if (semi > 48) semi = 48;
    if (cents < -50) cents = -50;
    if (cents > 50) cents = 50;
    o = (semi + 48) / 12 - 4;                   /* octaves, floor (semi + 48 >= 0: no negative division) */
    r = semi - 12 * o;                          /* 0..11 */
    /* 2^(c/1200) ~ 1 + c ln2/1200 + (c ln2/1200)^2 / 2: within 0.01 cent for |c| <= 50 */
    f = (uint32_t)(65536 + ((cents * 38763) >> 10) + ((cents * cents * 716) >> 16));
    m = (uint32_t)(((uint64_t)SEMI_Q16[r] * f) >> 16);   /* Q16 */
    sh = o + 4;                                 /* Q16 -> Q20 and the octaves: 0 .. 8 */
    return m << sh;
}
#endif
