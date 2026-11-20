/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: the editor's frames (firmware/src/editor.h) against the examples of docs/editor-protocol.md
 * (tools/ed_vectors.py), round trips, and frames that must be refused.
 *   cc -Ifirmware/src -o editor_test tests/editor_test.c && ./editor_test */
#include <stdio.h>
#include <string.h>
#include "editor.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint32_t unhex(const char *s, uint8_t *d)
{
    uint32_t n = 0;
    unsigned v;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s || sscanf(s, "%2x", &v) != 1) break;
        d[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

/* a request as the page sends it: the reply builder's frame with the status byte taken out */
static uint32_t request(uint8_t cmd, uint8_t seq, const uint8_t *p, uint32_t n, uint8_t *w)
{
    uint8_t d[ED_OUT_MAX];
    uint32_t i, s = 0, k;
    d[0] = cmd; d[1] = seq;
    for (i = 0; i < n; i++) d[2u + i] = p[i];
    for (i = 0; i < n + 2u; i++) s += d[i];
    d[n + 2u] = (uint8_t)~s;
    w[0] = 0xF0; w[1] = ED_TAG0; w[2] = ED_TAG1; w[3] = ED_TAG2;
    k = 4u + ed_pack7(d, n + 3u, w + 4);
    w[k++] = 0xF7;
    return k;
}

static ed_req_t q;

int main(void)
{
    uint8_t w[ED_WIRE_MAX], x[ED_WIRE_MAX], big[600];
    uint32_t n, m, i;

    /* the doc's examples */
    n = unhex("F0 7D 56 4C 01 02 74 07 F7", x);
    CHECK(ed_parse(x + 1, n - 2u, &q) && q.cmd == ED_INFO && q.seq == 1 && q.n == 0, "INFO example");
    n = unhex("F0 7D 56 4C 02 04 00 20 71 1C F7", x);
    CHECK(ed_parse(x + 1, n - 2u, &q) && q.cmd == ED_NAMES && q.seq == 2 && q.n == 2 && q.p[0] == 0 && q.p[1] == 20,
          "NAMES example");
    n = unhex("F0 7D 56 4C 10 06 7C 6B 06 F7", x);
    CHECK(ed_parse(x + 1, n - 2u, &q) && q.cmd == ED_GET && q.seq == 3 && q.n == 1 && q.p[0] == ED_SOUND, "GET example");
    m = unhex("F0 7D 56 4C 11 09 00 28 50 0C F7", x);
    {
        uint8_t slot = 5;
        n = ed_build(ED_PUT, 4, ED_OK, &slot, 1, w);
        CHECK(n == m && !memcmp(w, x, n), "PUT reply example");
    }

    /* round trips of every length a request can have */
    for (m = 0; m <= ED_DEC_MAX - 3u; m += 7u) {
        for (i = 0; i < m; i++) big[i] = (uint8_t)(i * 37u + m);
        n = request(ED_PUT, (uint8_t)m, big, m, w);
        CHECK(n - 2u <= ED_RAW_MAX, "a %u-byte payload: %u bytes between F0 and F7", m, n - 2u);
        CHECK(ed_parse(w + 1, n - 2u, &q) && q.n == m && q.seq == (uint8_t)m && !memcmp(q.p, big, m),
              "round trip %u", m);
    }

    /* refused: a flipped bit, a byte short, another tag, the longest payload plus one */
    n = request(ED_GET, 9, (const uint8_t *)"\x05", 1, w);
    w[5] ^= 1;
    CHECK(!ed_parse(w + 1, n - 2u, &q), "a flipped bit");
    n = request(ED_GET, 9, (const uint8_t *)"\x05", 1, w);
    CHECK(!ed_parse(w + 1, n - 3u, &q), "a byte short");
    n = request(ED_GET, 9, (const uint8_t *)"\x05", 1, w);
    w[2] = 0x46;
    CHECK(!ed_parse(w + 1, n - 2u, &q), "Felucca's tag");
    n = request(ED_PUT, 1, big, ED_DEC_MAX - 2u, w);
    CHECK(!ed_parse(w + 1, n - 2u, &q), "one byte too long (%u between F0 and F7)", n - 2u);

    /* a reply too long for the builder */
    CHECK(ed_build(ED_GET, 1, ED_OK, big, ED_OUT_MAX - 3u, w) == 0, "an oversized reply");
    n = ed_build(ED_GET, 1, ED_OK, big, ED_OUT_MAX - 4u, w);
    CHECK(n > 0 && n <= ED_WIRE_MAX, "the longest reply fits ED_WIRE_MAX (%u)", n);

    if (fails) { printf("editor_test: %d failed\n", fails); return 1; }
    printf("editor_test: ok\n");
    return 0;
}
