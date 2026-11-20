/* SPDX-License-Identifier: GPL-3.0-only */
/* Where Dinghy keeps your data in flash, per board (platform.c: settings_*, blob_*, slot_*). Macros and compile-time
 * checks only, so the host tests include it (tests/smk_flash_test.c).
 *
 *              FM-1                          SMK-37 (Pro, Elite, MKE-P37)
 *   settings   0xFC000, 1 sector             0x9D000, 1 sector
 *   blobs      0xF4000.. NBLOB sectors       0x9E000.. NBLOB sectors
 *   slots      0x97000.. NSLOT x SLOT_SECT   0xC2000.. NSLOT x SLOT_SECT (USRFLASH, SLOOP's sample slots)
 *   owner tag  -                             0xAF000: "SMK37DATA" + DATA_FAMILY (smk_owner_check)
 *
 * On the FM-1 the other firmwares of the family keep their data in 0x97000..0xFBFFF too: a new firmware finds the
 * magic or the length wrong and starts fresh (give your fork its own STORE_MAGIC). The SMK-37's store is shared by
 * the ports under the fm1-to-smk37 kit's convention: whose data it is says the owner tag, anything else is cleared
 * once at boot (the stock settings, another port's), never parsed. Calibration (0xC1000), BT pairing, the stock
 * resources and key_mac are never in reach: FL_RANGE_OK below is the app's allow-list, and the RAM flash driver
 * refuses every protected range on its own as well (FL_GUARD, hal/fm1_flash_map.h). */
#pragma once
#include <stdint.h>

#ifndef STORE_MAGIC
#define STORE_MAGIC 0x4E4C4F56u                 /* "VOLN": settings and blobs (slots: "SLOT") */
#endif
#define NBLOB 8u                                /* blobs: one 4 KiB sector each */
#define NSLOT 8u                                /* slots: SLOT_SECT sectors each, for something bigger (samples, songs) */
#define SLOT_SECT 4u
#define PSEC_N 13u                              /* Volna's patches (2026-11-15): sectors of their own at the slots' start
                                                   (Volna keeps no slots): psec_load / psec_save */
_Static_assert(PSEC_N <= NSLOT * SLOT_SECT, "the patches' sectors outgrow the slots' area");

#if BOARD_SMK37
#define SETTINGS_AT 0x0009D000u
#define BLOBS_AT 0x0009E000u
#define OWNER_AT 0x000AF000u                    /* (the kit's place for it) */
#define SLOTS_AT 0x000C2000u
#ifndef DATA_FAMILY
#define DATA_FAMILY "VOLNA"                    /* the owner tag's family: 1..7 of A-Z 0-9 (tools/build.py DATA_FAMILY) */
#endif
#define FL_RANGE_OK(off, n) (FL_IN(off, n, FL_OTA_LO, FL_OTA_HI) || FL_IN(off, n, SETTINGS_AT, SETTINGS_AT + 0x1000u) || \
                             FL_IN(off, n, BLOBS_AT, BLOBS_AT + NBLOB * 0x1000u) || FL_IN(off, n, OWNER_AT, OWNER_AT + 0x1000u) || \
                             FL_IN(off, n, SLOTS_AT, SLOTS_AT + NSLOT * SLOT_SECT * 0x1000u))
#else
#define SETTINGS_AT 0x000FC000u                 /* (Felucca's globals area) */
#define BLOBS_AT 0x000F4000u
#define SLOTS_AT 0x00097000u                    /* (Felucca's main store, FL_DATA) */
/* Volna's user wavetables (2026-11-17): UWT_N slots of UWT_SECT sectors right after the patches' sectors, each a header
 * sector, then the table (WT_WORDS int16, 49,920 B: 13 sectors). The sound reads them in place through the plain XIP
 * window (platform.c uwt_xip). They overlap the slots' area: Volna keeps no slots (slot_load / slot_save unused) */
#define UWT_N 4u
#define UWT_SECT 14u
#define UWT_AT (SLOTS_AT + PSEC_N * 0x1000u)    /* 0xA4000 .. 0xDC000 */
#define PATLIB_AT (UWT_AT + UWT_N * UWT_SECT * 0x1000u)
#define PATLIB_COPIES 2u                       /* 0xDC000..0xDE000; legacy blob 5 stays untouched */
#define FL_RANGE_OK(off, n) (FL_IN(off, n, FL_OTA_LO, FL_OTA_HI) || FL_IN(off, n, SETTINGS_AT, SETTINGS_AT + 0x1000u) || \
                             FL_IN(off, n, BLOBS_AT, BLOBS_AT + NBLOB * 0x1000u) || \
                             FL_IN(off, n, SLOTS_AT, SLOTS_AT + NSLOT * SLOT_SECT * 0x1000u) || \
                             FL_IN(off, n, UWT_AT, UWT_AT + UWT_N * UWT_SECT * 0x1000u) || \
                             FL_IN(off, n, PATLIB_AT, PATLIB_AT + PATLIB_COPIES * 0x1000u))
#endif
#ifndef UWT_N
#define UWT_N 0u                                /* (SMK-37: no user wavetables) */
#define UWT_SECT 0u
#endif
#include "fm1_flash_map.h"
#define SECTOR_MAX (4096u - 12u)                /* settings, a blob: the sector minus the header */
#define SLOT_MAX (SLOT_SECT * 0x1000u - 12u)

#if BOARD_SMK37
_Static_assert(SETTINGS_AT >= FL_DATA_LO && SETTINGS_AT + 0x1000u <= BLOBS_AT, "SMK store: settings outside FL_DATA");
_Static_assert(BLOBS_AT + NBLOB * 0x1000u <= OWNER_AT && OWNER_AT + 0x1000u <= FL_DATA_HI, "SMK store: blobs / owner tag");
_Static_assert(SLOTS_AT >= FL_SMP_LO && SLOTS_AT + NSLOT * SLOT_SECT * 0x1000u <= FL_SMP_HI, "SMK store: slots outside FL_SMP");
_Static_assert(FL_WIN_SAFE(SLOTS_AT, SLOTS_AT + NSLOT * SLOT_SECT * 0x1000u), "SMK store: the slots overlap a protected range");
_Static_assert(FL_WIN_SAFE(SETTINGS_AT, OWNER_AT + 0x1000u), "SMK store: overlaps a protected range");
_Static_assert(sizeof DATA_FAMILY - 1u >= 1u && sizeof DATA_FAMILY - 1u <= 7u, "SMK store: family 1..7 characters");

/* the 16-byte owner tag: "SMK37DATA" + the family, zero padded (a 7-letter family fills it, no NUL) */
static void smk_owner_tag(uint8_t tag[16])
{
    static const char FAM[] = DATA_FAMILY;
    uint32_t i;
    for (i = 0; i < 16u; i++)
        tag[i] = i < 9u ? (uint8_t)"SMK37DATA"[i] : i - 9u < sizeof FAM - 1u ? (uint8_t)FAM[i - 9u] : 0u;
}
#else
_Static_assert(SLOTS_AT >= FL_DATA_LO && SLOTS_AT + NSLOT * SLOT_SECT * 0x1000u <= FL_DATA_HI, "FM-1 store: slots outside FL_DATA");
_Static_assert(UWT_AT >= SLOTS_AT + PSEC_N * 0x1000u && UWT_AT % 0x1000u == 0u, "FM-1 store: the wavetables over the patches");
_Static_assert(UWT_AT + UWT_N * UWT_SECT * 0x1000u <= FL_DATA_HI, "FM-1 store: the wavetables outside FL_DATA");
_Static_assert(UWT_AT >= FL_PLAIN_LO, "FM-1 store: the wavetables outside the plain XIP window");
_Static_assert(PATLIB_AT + PATLIB_COPIES * 0x1000u <= BLOBS_AT, "pattern library overlaps blobs");
_Static_assert(PATLIB_AT >= UWT_AT + UWT_N * UWT_SECT * 0x1000u, "pattern library overlaps tables");
_Static_assert((UWT_SECT - 1u) * 0x1000u >= 24960u * 2u, "FM-1 store: a wavetable (WT_WORDS) outgrows its slot");
_Static_assert(BLOBS_AT >= 0xEA000u && BLOBS_AT + NBLOB * 0x1000u <= SETTINGS_AT, "FM-1 store: blobs (free, below the settings)");
#endif
