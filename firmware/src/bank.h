/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: the patch bank's layout (stage 6; the record grew for 16 matrix slots). A blob holds PPB records of PREC
 * bytes. Banks saved before the records grew hold them PREC_OLD bytes apart: bank_spread() moves them to PREC apart
 * in place and zeroes each record's new tail, so an older bank loads as it was (a shorter record loads over the
 * defaults: patch_t only grows at its end). The last record moves first, each a byte at a time from its end: a
 * record's new place is above its old one, so nothing is overwritten before it has moved. No libc. */
#ifndef VOLNA_BANK_H
#define VOLNA_BANK_H
#include <stdint.h>

#define PPB 20u                                 /* patches a blob */
#define PREC 204u                               /* a record's bytes now: 20 x 204 = 4080, a blob holds 4084 */
#define PREC_OLD 192u                           /* before 16 matrix slots (2026-11-14) */

/* r: PPB x PREC bytes, the first PPB x PREC_OLD of them a bank in the old layout */
static void bank_spread(uint8_t *r)
{
    uint32_t i, k;
    for (i = PPB; i-- > 0u;) {
        for (k = PREC_OLD; k-- > 0u;) r[i * PREC + k] = r[i * PREC_OLD + k];
        for (k = PREC_OLD; k < PREC; k++) r[i * PREC + k] = 0;
    }
}
#endif
