/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: the settings in a box of a fixed size (2026-11-14), so that they and the sound being edited survive a
 * firmware update. The store loads a sector only at the length it was saved with; a box is always SETBOX bytes:
 *   [0..1] SETBOX_MAGIC, [2..3] the head's length, [4..5] the patch's length, [6..7] 0, the head, the patch.
 * Unpacking copies each part over what is there already (the defaults) as far as both lengths go: the head (the
 * panel's state) and the patch (the sound) may each grow at their end, an older box loads into a newer layout, a
 * newer one into an older (what it does not know is left out). No libc. */
#ifndef VOLNA_SETBOX_H
#define VOLNA_SETBOX_H
#include <stdint.h>

#define SETBOX 512u
#define SETBOX_MAGIC 0x5642u                    /* "BV" */

static void setbox_pack(uint8_t *b, const void *head, uint32_t hn, const void *patch, uint32_t pn)
{
    const uint8_t *h = (const uint8_t *)head, *p = (const uint8_t *)patch;
    uint32_t i;
    if (hn > SETBOX - 8u) hn = SETBOX - 8u;
    if (pn > SETBOX - 8u - hn) pn = SETBOX - 8u - hn;
    for (i = 0; i < SETBOX; i++) b[i] = 0;
    b[0] = (uint8_t)SETBOX_MAGIC; b[1] = (uint8_t)(SETBOX_MAGIC >> 8);
    b[2] = (uint8_t)hn; b[3] = (uint8_t)(hn >> 8);
    b[4] = (uint8_t)pn; b[5] = (uint8_t)(pn >> 8);
    for (i = 0; i < hn; i++) b[8u + i] = h[i];
    for (i = 0; i < pn; i++) b[8u + hn + i] = p[i];
}
/* 0: unpacked (the caller checks the values); -1: not a box */
static int setbox_unpack(const uint8_t *b, void *head, uint32_t hn, void *patch, uint32_t pn)
{
    uint8_t *h = (uint8_t *)head, *p = (uint8_t *)patch;
    uint32_t sh = b[2] | (uint32_t)b[3] << 8, sp = b[4] | (uint32_t)b[5] << 8, i;
    if ((b[0] | (uint32_t)b[1] << 8) != SETBOX_MAGIC || sh + sp > SETBOX - 8u) return -1;
    for (i = 0; i < hn && i < sh; i++) h[i] = b[8u + i];
    for (i = 0; i < pn && i < sp; i++) p[i] = b[8u + sh + i];
    return 0;
}
#endif
