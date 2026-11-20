/* SPDX-License-Identifier: GPL-3.0-only */
/* The voices a sound may use (2026-11-18): a heavy sound plays 3 notes, not 4, so the audio block never runs past
 * its time (3 oscillators x UNISON 7 on 4 notes took 101 % on the FM-1 and crackled).
 *
 * Heavy is measured, not guessed: the ISR times every block (the SETUP figures) and counts the voices it played.
 * Half a second's longest block gives a voice's cost (its voices' time / their number) and the rest (effects, EQ,
 * limiter). Four voices of it past POLY_HI of the block: 3 voices. Back to 4 under POLY_LO (no flapping in
 * between). The ISR also flags a block past POLY_OVER with every voice playing: a chord taken at once on a fresh
 * sound drops a voice at once, not half a second later.
 * One voice at most is taken (the user's choice, 2026-11-18): a sound too heavy even for 3 still crackles.
 * Ticks: 139320 = a whole block (256 frames at 44.1 kHz, TIMER4 at 24 MHz). */
#ifndef VOLNA_POLY_H
#define VOLNA_POLY_H
#include <stdint.h>

#define POLY_BLOCK 139320u                      /* a block's ticks: 100 % */
#define POLY_HI (POLY_BLOCK / 100u * 90u)       /* 4 voices past 90 %: 3 */
#define POLY_LO (POLY_BLOCK / 100u * 82u)       /* 4 voices under 82 %: 4 again */
#define POLY_OVER (POLY_BLOCK / 100u * 95u)     /* a block past 95 % with every voice on: 3 at once */

/* the voices next: cap now (nmax or nmax - 1), the longest block's ticks (tot), its voices' share (vox, with the
 * block's setup), the voices it played (nv). No voice played: nothing learnt, cap stays. */
static uint32_t poly_next(uint32_t cap, uint32_t nmax, uint32_t tot, uint32_t vox, uint32_t nv)
{
    uint32_t rest, need;
    if (!nv || nmax < 2u) return cap;
    if (vox > tot) vox = tot;
    rest = tot - vox;
    need = vox / nv * nmax + rest;              /* (every voice playing: at most 4 x 139320, fits) */
    if (need > POLY_HI) return nmax - 1u;
    if (need < POLY_LO) return nmax;
    return cap;
}
#endif
