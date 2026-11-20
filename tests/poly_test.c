/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: the voices' limit's decision (engine/poly.h) on the FM-1's figures of 2026-11-18 (VOX, FX + OUT, in % of a
 * block; 1 % = 1393.2 ticks). */
#include <stdio.h>
#include "engine/poly.h"

static int bad;
static void check(const char *what, uint32_t got, uint32_t want)
{
    printf("%-64s %u %s\n", what, got, got == want ? "ok" : "BAD");
    if (got != want) bad = 1;
}
#define PC(x) ((uint32_t)((x) * 1393.2))        /* % -> ticks */

int main(void)
{
    /* SUPERSAW (OSC1 UNI 7): VOX 50 FX 9 on 4 notes -> 4 x 12.5 + 9 = 59 % */
    check("SUPERSAW, 4 notes (59 %): 4", poly_next(4, 4, PC(59), PC(50), 4), 4);
    check("SUPERSAW, 1 note (VOX 13): 4", poly_next(4, 4, PC(22), PC(13), 1), 4);
    /* three oscillators x UNI 7: VOX 68 FX 9 on 3 notes -> 4 x 22.7 + 9 = 100 % */
    check("3 x UNI 7, 3 notes (VOX 68 FX 9): 3", poly_next(4, 4, PC(77), PC(68), 3), 3);
    check("3 x UNI 7, 1 note (VOX 23 FX 9): 3, before the 4th note", poly_next(4, 4, PC(32), PC(23), 1), 3);
    check("limited, still heavy: 3", poly_next(3, 4, PC(77), PC(68), 3), 3);
    check("limited, between 82 and 90 % (VOX 19.5 x 4 + 9 = 87): stays 3", poly_next(3, 4, PC(28.5), PC(19.5), 1), 3);
    check("not limited, between 82 and 90 %: stays 4", poly_next(4, 4, PC(28.5), PC(19.5), 1), 4);
    check("limited, a light sound again (VOX 12.5 FX 9): 4", poly_next(3, 4, PC(46.5), PC(37.5), 3), 4);
    check("nothing playing: no change (3)", poly_next(3, 4, PC(9), PC(1), 0), 3);
    check("nothing playing: no change (4)", poly_next(4, 4, PC(9), PC(1), 0), 4);
    check("the voices' share past the block's (a torn read): no wrap", poly_next(4, 4, PC(50), PC(60), 4), 4);
    check("MONO's single voice (nmax 1): unchanged", poly_next(1, 1, PC(99), PC(90), 1), 1);
    puts(bad ? "poly test FAILED" : "poly test passed");
    return bad;
}
