/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: the local wavetables (firmware/gen/wt_local.h from local/wavetables, tools/gen_uwt.py) on the host.
 * Without local/ the header is a stub and there is nothing to check. With it, every table:
 *   1. 64 waves, each with its guard sample (the last = the first), a name;
 *   2. the peak within 30000 (wt_sample's products stay under 2^31) and not silent: some wave reaches 30000;
 *   3. wt_sample over every wave and phase, blending into the next wave, stays within the peak.
 *   cc -O1 -Wall -Ifirmware/src -Ifirmware/gen -o build/host/wt_local_test tests/wt_local_test.c */
#include <stdio.h>
#include <stdlib.h>
#include "engine/wt_osc.h"
#include "wt_local.h"

int main(void)
{
#if WT_LOCAL_N
    int fails = 0;
    uint32_t t, w, i;
    for (t = 0; t < WT_LOCAL_N; t++) {
        int32_t peak = 0, worst = 0;
        const wt_row_t *tab = (const wt_row_t *)WT_LOCAL[t];   /* (level 0; the mip-maps: tests/mip_test.c) */
        if (!WT_LOCAL_NAME[t][0]) { printf("FAIL: table %u has no name\n", t); fails++; }
        for (w = 0; w < WT_WAVES; w++) {
            if (tab[w][WT_LEN] != tab[w][0]) { printf("FAIL: %s wave %u: guard %d, first %d\n", WT_LOCAL_NAME[t], w, tab[w][WT_LEN], tab[w][0]); fails++; }
            for (i = 0; i <= WT_LEN; i++) if (abs(tab[w][i]) > peak) peak = abs(tab[w][i]);
        }
        for (w = 0; w < WT_WAVES; w++)
            for (i = 0; i < 512u; i++) {
                int32_t s = wt_sample(tab, i << 23, (w << 16) | 0x8000u);
                if (abs(s) > worst) worst = abs(s);
            }
        if (peak != 30000) { printf("FAIL: %s: peak %d, want 30000\n", WT_LOCAL_NAME[t], peak); fails++; }
        if (worst > peak) { printf("FAIL: %s: wt_sample reached %d over the peak %d\n", WT_LOCAL_NAME[t], worst, peak); fails++; }
    }
    printf(fails ? "wt_local: %d FAILED\n" : "wt_local: %u local tables fine\n", fails ? fails : (int)WT_LOCAL_N);
    return fails != 0;
#else
    printf("wt_local: no local tables (local/wavetables absent): nothing to check\n");
    return 0;
#endif
}
