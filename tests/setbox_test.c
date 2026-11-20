/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: the settings' box (firmware/src/setbox.h): a box packed by an older layout (a shorter head, a shorter patch)
 * unpacks into a newer one with the rest left at the defaults; a newer box into an older layout keeps what that one
 * knows; something else is not a box.
 *   cc -O1 -Wall -Ifirmware/src -o build/host/setbox_test tests/setbox_test.c */
#include <stdio.h>
#include "setbox.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    static uint8_t box[SETBOX];
    uint8_t h0[10], p0[160], h1[14], p1[184], i;
    for (i = 0; i < 10; i++) h0[i] = (uint8_t)(i + 1);
    for (i = 0; i < 160; i++) p0[i] = (uint8_t)(200 - i);
    setbox_pack(box, h0, sizeof h0, p0, sizeof p0);             /* the old layout */
    for (i = 0; i < 14; i++) h1[i] = 0xAA;                      /* (the new layout's defaults) */
    for (i = 0; i < 184; i++) p1[i] = 0xBB;
    CHECK(!setbox_unpack(box, h1, sizeof h1, p1, sizeof p1), "an old box is not a box");
    for (i = 0; i < 10; i++) CHECK(h1[i] == i + 1, "head %u: %u", i, h1[i]);
    for (i = 10; i < 14; i++) CHECK(h1[i] == 0xAA, "head %u lost its default", i);
    for (i = 0; i < 160; i++) CHECK(p1[i] == (uint8_t)(200 - i), "patch %u: %u", i, p1[i]);
    for (i = 160; i < 184; i++) CHECK(p1[i] == 0xBB, "patch %u lost its default", i);
    setbox_pack(box, h1, sizeof h1, p1, sizeof p1);             /* a newer box into the older layout */
    for (i = 0; i < 10; i++) h0[i] = 0;
    for (i = 0; i < 160; i++) p0[i] = 0;
    CHECK(!setbox_unpack(box, h0, sizeof h0, p0, sizeof p0), "a newer box is not a box");
    CHECK(h0[9] == 10 && p0[0] == 200 && p0[159] == (uint8_t)(200 - 159), "a newer box into the older layout");
    box[0] ^= 0xFF;
    CHECK(setbox_unpack(box, h0, sizeof h0, p0, sizeof p0) == -1, "a box with a wrong magic unpacks");
    printf(fails ? "setbox: %d FAILED\n" : "setbox: older and newer layouts load, the rest at the defaults\n", fails);
    return fails != 0;
}
