/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: the editor's SysEx frames (docs/editor-protocol.md). Framing only, no patch in sight, so the host tests
 * include it alone (tests/editor_test.c); volna.c answers the commands (ed_service).
 *
 *   F0 7D 56 4C pack7(cmd seq payload sum) F7      sum = ~(cmd + seq + payload) & FF; a reply: cmd | 80, seq, status
 *
 * pack7 is the update protocol's: one LSB-first bit stream, 8 bits a byte in, 7 out. No libc. */
#ifndef VOLNA_EDITOR_H
#define VOLNA_EDITOR_H
#include <stdint.h>

#define ED_PROTO 5u                             /* 5: track a patch pattern's source template.
                                                   4: independent patterns and an explicit pattern library.
                                                   1: INFO NAMES GET PUT; 2 (2026-11-17): the parameters, WATCH;
                                                   3 (2026-11-17): the user wavetables (TABLE ..) */
#define ED_TAG0 0x7Du                           /* the non-commercial ID, then "VL" */
#define ED_TAG1 0x56u
#define ED_TAG2 0x4Cu
#define ED_RAW_MAX 640u                         /* a received frame between F0 and F7 (the platform's buffer) */
#define ED_DEC_MAX ((ED_RAW_MAX - 3u) * 7u / 8u) /* its decoded bytes: 557 */
#define ED_OUT_MAX 600u                         /* a reply's decoded bytes (PATCH GET: 3 + 1 + 16 + 494) */
#define ED_WIRE_MAX (5u + (ED_OUT_MAX * 8u + 6u) / 7u + 1u)

enum { ED_INFO = 0x01, ED_NAMES = 0x02, ED_GET = 0x10, ED_PUT = 0x11,
       ED_PDESC = 0x20, ED_PVAL = 0x21, ED_PSET = 0x22, ED_PCHOICES = 0x23, ED_LAYOUT = 0x24, ED_WATCH = 0x25,
       ED_TINFO = 0x30, ED_TBEGIN = 0x31, ED_TDATA = 0x32, ED_TEND = 0x33, ED_TNAME = 0x34, ED_TCLEAR = 0x35,
       ED_PATINFO = 0x40, ED_PATGET = 0x41, ED_PATPUT = 0x42, ED_SAVE = 0x43,
       ED_CHANGED = 0xE0 };                     /* (CHANGED: the device's own, unasked, seq 0) */
#define ED_TCHUNK 512u                          /* TABLE DATA: the table's bytes a frame carries at most */
enum { ED_OK = 0, ED_BADREQ = 1, ED_BADPATCH = 2, ED_BUSY = 3, ED_FLASH = 4, ED_UNKNOWN = 5 };
#define ED_SOUND 0x7Fu                          /* GET / PUT: the sound being played, not a stored patch */

typedef struct {
    uint8_t cmd, seq;
    uint32_t n;                                 /* the payload's bytes, in p */
    uint8_t p[ED_DEC_MAX];
} ed_req_t;

static uint32_t ed_unpack7(const uint8_t *s, uint32_t n, uint8_t *d, uint32_t max)
{
    uint32_t acc = 0, bits = 0, k = 0, i;
    for (i = 0; i < n; i++) {
        acc |= (uint32_t)(s[i] & 0x7Fu) << bits;
        bits += 7u;
        if (bits >= 8u) {
            if (k >= max) return 0;
            d[k++] = (uint8_t)acc;
            acc >>= 8;
            bits -= 8u;
        }
    }
    return k;
}

static uint32_t ed_pack7(const uint8_t *s, uint32_t n, uint8_t *d)
{
    uint32_t acc = 0, bits = 0, k = 0, i;
    for (i = 0; i < n; i++) {
        acc |= (uint32_t)s[i] << bits;
        bits += 8u;
        while (bits >= 7u) {
            d[k++] = (uint8_t)(acc & 0x7Fu);
            acc >>= 7;
            bits -= 7u;
        }
    }
    if (bits) d[k++] = (uint8_t)(acc & 0x7Fu);
    return k;
}

/* is this received frame (the bytes between F0 and F7) the editor's? */
static int ed_mine(const uint8_t *raw, uint32_t n)
{
    return n >= 3u && raw[0] == ED_TAG0 && raw[1] == ED_TAG1 && raw[2] == ED_TAG2;
}

/* a received frame into q: 1 a well-formed request, 0 not (its sum, its length) */
static int ed_parse(const uint8_t *raw, uint32_t n, ed_req_t *q)
{
    static uint8_t d[ED_DEC_MAX];
    uint32_t m, i, s = 0;
    if (!ed_mine(raw, n) || n - 3u > ED_RAW_MAX) return 0;
    m = ed_unpack7(raw + 3, n - 3u, d, sizeof d);
    if (m < 3u || (m * 8u + 6u) / 7u != n - 3u) return 0;   /* (n - 3 bytes carry exactly m) */
    for (i = 0; i + 1u < m; i++) s += d[i];
    if ((uint8_t)~s != d[m - 1u]) return 0;
    q->cmd = d[0];
    q->seq = d[1];
    q->n = m - 3u;
    for (i = 0; i < q->n; i++) q->p[i] = d[2u + i];
    return 1;
}

/* a reply into w (F0 .. F7): cmd | 80, seq, status, then n bytes of p. Its length (0: too long) */
static uint32_t ed_build(uint8_t cmd, uint8_t seq, uint8_t status, const uint8_t *p, uint32_t n, uint8_t *w)
{
    static uint8_t d[ED_OUT_MAX];
    uint32_t i, s, k;
    if (n + 4u > sizeof d) return 0;
    d[0] = (uint8_t)(cmd | 0x80u);
    d[1] = seq;
    d[2] = status;
    for (i = 0; i < n; i++) d[3u + i] = p[i];
    for (i = 0, s = 0; i < n + 3u; i++) s += d[i];
    d[n + 3u] = (uint8_t)~s;
    w[0] = 0xF0;
    w[1] = ED_TAG0;
    w[2] = ED_TAG1;
    w[3] = ED_TAG2;
    k = 4u + ed_pack7(d, n + 4u, w + 4);
    w[k++] = 0xF7;
    return k;
}

static uint16_t ed_rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void ed_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
#endif
