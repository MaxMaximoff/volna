/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: an older patch bank (records PREC_OLD bytes apart) spread to the new layout (firmware/src/bank.h): every
 * record's bytes where the new layout reads them, each record's new tail zero, nothing else changed.
 *   cc -O1 -Wall -Ifirmware/src -o build/host/bank_test tests/bank_test.c */
#include <stdio.h>
#include "bank.h"

int main(void)
{
    static uint8_t r[PPB * PREC];
    uint32_t i, k, bad = 0;
    for (i = 0; i < PPB * PREC; i++) r[i] = 0xEE;   /* (what lies past the old bank: must not show) */
    for (i = 0; i < PPB; i++)
        for (k = 0; k < PREC_OLD; k++) r[i * PREC_OLD + k] = (uint8_t)(i * 7u + k * 13u + 1u);
    bank_spread(r);
    for (i = 0; i < PPB; i++) {
        for (k = 0; k < PREC_OLD; k++) bad += r[i * PREC + k] != (uint8_t)(i * 7u + k * 13u + 1u);
        for (k = PREC_OLD; k < PREC; k++) bad += r[i * PREC + k] != 0;
    }
    printf(bad ? "bank: %u bytes wrong\n" : "bank: an old bank spread to %u-byte records, all in place\n", bad ? bad : PREC);
    return bad != 0;
}
