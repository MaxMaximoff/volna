/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the real firmware's migration and redundant library store without hardware. */
#define DINGHY_HOST 1
#define main volna_host_main
#include "../firmware/src/volna.c"
#undef main
#include <assert.h>

int main(void)
{
    uint32_t i, j;
    patch_t p, q;
    char name[PATCH_NAME];
    uint8_t record[16 + sizeof(patch_t)];
    settings_default(); pat_load(); patlib_init();
    assert(__builtin_offsetof(patch_t, patver) >= 266u);
    for (i = 0; i <= PAT_OWN; i++) {
        p = PATCH_DEFAULT; p.arp.pattern = (uint8_t)i;
        for (j = 0; j < ARP_PLEN; j++) p.ost[j] = AST_MAKE(AS_CHORD, 1u, 6u, 3u, 7u) | AST_TIEBIT;
        q = p; pattern_own(&p);
        assert(p.patver == 1 && p.arp.pattern == PAT_OWN && patch_ok(&p));
        for (j = 0; j < ARP_PLEN; j++) assert(p.ost[j] == (i == 0 ? AST_DEFAULT : i == PAT_OWN ? q.ost[j] : pats.st[i-1][j]));
        q = p; pattern_own(&p); assert(!memcmp(&q, &p, sizeof p));
    }
    /* An old exported patch resolves against the frozen bank, not the editable library. */
    p = PATCH_DEFAULT; p.arp.pattern = 2;
    ed_record(record, &p, "LEGACY      ");
    ed_wr16(record + 2, 266u);
    assert(ed_unrecord(record, 16 + 266u, &q, name));
    assert(q.patver == 1 && !memcmp(q.ost, pats.st[1], sizeof q.ost));
    p = q;
    assert(!patlib_store(1, PATCH_DEFAULT.ost, 7, 99));
    assert(patlib_at == 0 && patlib.generation == 1);
    assert(ed_unrecord(record, 16 + 266u, &q, name));
    assert(!memcmp(&p, &q, sizeof p));
    assert(!patlib_store(2, p.ost, 8, 101));
    assert(patlib_at == 1 && patlib.generation == 2);
    /* Reboot selects the latest valid copy; a torn target preserves the previous one. */
    host_patlib[1][0] = 0; patlib_at = -1; patlib_init();
    assert(patlib_at == 0 && patlib.generation == 1 && patlib.bank.len[1] == 7);
    host_patlib_fail = 1;
    assert(patlib_store(1, p.ost, 3, 44) == -1);
    assert(patlib_at == 0 && patlib.generation == 1 && patlib.bank.len[1] == 7);
    host_patlib_fail = 0;
    assert(!patlib_store(1, p.ost, 3, 44));
    assert(patlib_at == 1 && patlib.generation == 2 && patlib.bank.len[1] == 3);
    /* Two patches using the same legacy reference become independent copies. */
    p = PATCH_DEFAULT; p.arp.pattern = 2; pattern_own(&p); q = p;
    p.ost[0] ^= 1; assert(p.ost[0] != q.ost[0] && q.ost[0] == pats.st[1][0]);
    assert(!patch_put(42, &p, "INDEPENDENT "));
    patch_get(42, &q, name); assert(!memcmp(&p, &q, sizeof p));
    host_psec_fail = 1; q.ost[0] ^= 1;
    assert(patch_put(42, &q, "FAILED      ") == -1);
    patch_get(42, &q, name); assert(!memcmp(&p, &q, sizeof p));
    host_psec_fail = 0;
    assert(!host_blob[PAT_BLOB][0]); /* no migration or library write touched legacy storage */
    puts("pattern migration and library recovery: OK");
    return 0;
}
