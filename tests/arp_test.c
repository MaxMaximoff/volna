/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna stage 5a: the arpeggiator (engine/arp.h) on the host.
 *   1. the order and the direction: C E G played as G C E gives what each order x direction should;
 *   2. octaves: 2 octaves of C E G go C E G C' E' G', notes above 127 left out;
 *   3. the timing: a step every rate ticks for every rate, no drift over 1000 steps; the gate ends each note at
 *      gate % of its step; legato (100) holds it to the next; swing delays every second step;
 *   4. ON stops when the keys are let go; HOLD keeps playing, and a new chord after letting go replaces it;
 *      ONE SHOT plays one pass and stops (with a pattern: its LENGTH steps once, part 7);
 *   5. the velocity: each note's own, the first's, the last's, fixed;
 *   6. every note on gets its note off (nothing hangs), arp_clear included;
 *   7. patterns (stage 5b): a pause leaves the sequence where it is; previous, first, last, first + last, chord
 *      (4 at most), random; accent (and mute, which moves the sequence on); length and legato; timing x TIMING;
 *      the pattern's length; the text form; ties (one, a chain, on a muted step);
 *   8. glides (stage 6b): a glide step after a plain one glides; after a legato one it slides (no note off for the
 *      note before, the voice moves on); a chord or a muted step does not slide.
 *   cc -O1 -Wall -Ifirmware/src -o build/host/arp_test tests/arp_test.c */
#include <stdio.h>
#include <string.h>
#include "engine/arp.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static arp_par_t P0 = {ARP_ON, AD_UP, AO_PLAYED, 1, ARP_RATE_16TH, 50, 0, AV_EACH, ARP_ON, 0, 16, 64, 120};
static const uint16_t *PAT;                     /* the pattern the helpers play (0: none) */
static int on_count[128];                       /* notes sounding (on - off) */
static uint8_t outv[64], outg[64], outf[64];    /* the velocities of the notes play() saw; their glides, from */

/* run for n steps of the rate, one tick at a time; the notes played into out (up to max) */
static uint32_t play(arp_t *a, const arp_par_t *p, uint32_t ticks, uint8_t *out, uint32_t max, uint32_t *on_t,
                     uint32_t *off_t)
{
    uint32_t t, k, n = 0, noff = 0;
    arp_ev_t ev[ARP_EVMAX];
    for (t = 0; t < ticks * ARP_TICK; t += 32) {   /* now (as the firmware does after a key), then an eighth of a
                                                   tick at a time, up to just before the window's end */
        uint32_t ne = arp_advance(a, p, PAT, t ? 32u : 0u, ev, ARP_EVMAX);
        for (k = 0; k < ne; k++) {
            if (ev[k].on) {
                if (n < max) { out[n] = ev[k].note; outv[n] = ev[k].vel; outg[n] = ev[k].glide; outf[n] = ev[k].from;
                               if (on_t) on_t[n] = a->t; }
                n++; on_count[ev[k].note]++;
                if (ev[k].glide == ARP_GL_SLIDE) on_count[ev[k].from]--;   /* (a slide: the note before goes on as
                                                   this one, it gets no note off of its own) */
            }
            else { if (off_t && noff < max) off_t[noff] = a->t; noff++; on_count[ev[k].note]--; }
        }
    }
    return n;
}
static void chord(arp_t *a, const arp_par_t *p, const uint8_t *n, const uint8_t *v, uint32_t c)
{
    uint32_t i;
    for (i = 0; i < c; i++) arp_key_on(a, p, n[i], v ? v[i] : 100u);
}
static int same(const uint8_t *a, const uint8_t *b, uint32_t n) { return !memcmp(a, b, n); }
static void reset(arp_t *a)                     /* the end of a test: as the firmware does, arp_clear (its note's end) */
{
    arp_ev_t ev[ARP_CHORD];
    uint32_t i, n = arp_clear(a, ev);
    for (i = 0; i < n; i++) on_count[ev[i].note]--;
    memset(a, 0, sizeof *a);
}

int main(void)
{
    static const uint8_t GCE[3] = {67, 60, 64}, VEL[3] = {20, 120, 70};
    uint8_t out[64], want[16];
    uint32_t on_t[64], off_t[64], i, n, r;
    arp_t a = {0};
    arp_par_t p;

    {                                           /* 1 */
        static const struct { uint8_t order, dir; uint8_t want[8]; } C[] = {
            {AO_PLAYED, AD_UP, {67, 60, 64, 67, 60, 64, 67, 60}},
            {AO_REVERSED, AD_UP, {64, 60, 67, 64, 60, 67, 64, 60}},
            {AO_KEYUP, AD_UP, {60, 64, 67, 60, 64, 67, 60, 64}},
            {AO_KEYDN, AD_UP, {67, 64, 60, 67, 64, 60, 67, 64}},
            {AO_KEYUP, AD_DOWN, {67, 64, 60, 67, 64, 60, 67, 64}},
            {AO_KEYUP, AD_UPDN, {60, 64, 67, 64, 60, 64, 67, 64}},
            {AO_KEYUP, AD_DNUP, {67, 64, 60, 64, 67, 64, 60, 64}},
            {AO_VELUP, AD_UP, {67, 64, 60, 67, 64, 60, 67, 64}},   /* velocities 20 (G) 120 (C) 70 (E) */
            {AO_VELDN, AD_UP, {60, 64, 67, 60, 64, 67, 60, 64}},
        };
        for (i = 0; i < sizeof C / sizeof C[0]; i++) {
            reset(&a); p = P0; p.order = C[i].order; p.dir = C[i].dir;
            chord(&a, &p, GCE, VEL, 3);
            n = play(&a, &p, 8 * 6, out, 64, 0, 0);
            CHECK(n == 8 && same(out, C[i].want, 8), "order %u dir %u: %u notes %u %u %u %u %u %u %u %u", C[i].order,
                  C[i].dir, n, out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
        }
    }
    {                                           /* 2 */
        static const uint8_t W2[6] = {60, 64, 67, 72, 76, 79}, HI[2] = {120, 126};
        reset(&a); p = P0; p.order = AO_KEYUP; p.oct = 2;
        chord(&a, &p, GCE, 0, 3);
        n = play(&a, &p, 6 * 6, out, 64, 0, 0);
        CHECK(n == 6 && same(out, W2, 6), "2 octaves: %u notes, %u %u %u %u %u %u", n, out[0], out[1], out[2], out[3], out[4], out[5]);
        reset(&a); p.oct = 3;
        chord(&a, &p, HI, 0, 2);
        n = play(&a, &p, 4 * 6, out, 64, 0, 0);
        want[0] = 120; want[1] = 126; want[2] = 120; want[3] = 126;
        CHECK(n == 4 && same(out, want, 4), "above 127 left out: %u %u %u %u", out[0], out[1], out[2], out[3]);
    }
    for (r = 0; r < ARP_NRATE; r++) {           /* 3: timing */
        uint32_t st = ARP_RATE_TICKS[r] * ARP_TICK, bad = 0;
        reset(&a); p = P0; p.rate = (uint8_t)r; p.gate = 50;
        chord(&a, &p, GCE, 0, 3);
        n = play(&a, &p, 40u * ARP_RATE_TICKS[r], out, 64, on_t, off_t);
        for (i = 1; i < 40u && i < n; i++) if (on_t[i] - on_t[i - 1u] != st) bad++;
        for (i = 0; i < 39u && i < n; i++) if (off_t[i] - on_t[i] != st / 2u) bad++;
        CHECK(n >= 40u && !bad, "rate %s: %u notes, %u off the grid or the gate", ARP_RATE_NAME[r], n, bad);
    }
    {
        reset(&a); p = P0; p.gate = 100;        /* legato: the note ends as the next starts */
        chord(&a, &p, GCE, 0, 3);
        n = play(&a, &p, 10 * 6, out, 64, on_t, off_t);
        for (i = 0; i + 1u < n; i++) CHECK(off_t[i] == on_t[i + 1u], "legato: note %u ends %u, the next starts %u", i, off_t[i], on_t[i + 1u]);
        reset(&a); p = P0; p.swing = 50;        /* swing: every second step half a step late */
        chord(&a, &p, GCE, 0, 3);
        n = play(&a, &p, 8 * 6, out, 64, on_t, off_t);
        for (i = 1; i < n; i++) {
            uint32_t d = on_t[i] - on_t[0], g = i * 6u * ARP_TICK + ((i & 1u) ? 3u * ARP_TICK : 0u);
            CHECK(d == g, "swing: step %u at %u, want %u", i, d, g);
        }
        reset(&a); p = P0;                      /* no drift: step 1000 right on the grid */
        chord(&a, &p, GCE, 0, 3);
        n = play(&a, &p, 1001u * 6u, out, 64, 0, 0);
        CHECK(n == 1001u, "1000 steps: %u notes", n);
    }
    {                                           /* 4 */
        reset(&a); p = P0;
        chord(&a, &p, GCE, 0, 3);
        play(&a, &p, 12, out, 64, 0, 0);
        for (i = 0; i < 3; i++) arp_key_off(&a, &p, GCE[i]);
        n = play(&a, &p, 60, out, 64, 0, 0);
        CHECK(n == 0 && !a.run, "ON: %u notes after the keys went up", n);
        reset(&a); p = P0; p.mode = ARP_HOLD;
        chord(&a, &p, GCE, 0, 3);
        play(&a, &p, 12, out, 64, 0, 0);
        for (i = 0; i < 3; i++) arp_key_off(&a, &p, GCE[i]);
        n = play(&a, &p, 36, out, 64, 0, 0);
        CHECK(n == 6, "HOLD: %u notes after the keys went up, want 6", n);
        arp_key_on(&a, &p, 50, 100);            /* a new chord: replaces */
        arp_key_on(&a, &p, 53, 100);
        CHECK(a.n == 2 && a.note[0] == 50 && a.note[1] == 53, "HOLD: a new chord has %u notes", a.n);
        reset(&a); p = P0; p.mode = ARP_ONESHOT; p.dir = AD_UPDN; p.order = AO_KEYUP;
        chord(&a, &p, GCE, 0, 3);
        n = play(&a, &p, 120, out, 64, 0, 0);
        want[0] = 60; want[1] = 64; want[2] = 67; want[3] = 64;
        CHECK(n == 4 && same(out, want, 4) && !a.run, "ONE SHOT up-down: %u notes (want 4: C E G E)", n);
    }
    {                                           /* 5 */
        static const struct { uint8_t mode, want; } V[] = {{AV_EACH, 0}, {AV_FIRST, 20}, {AV_LAST, 70}, {AV_32, 32},
                                                           {AV_64, 64}, {AV_100, 100}, {AV_127, 127}};
        arp_ev_t ev[ARP_EVMAX];
        for (i = 0; i < 7u; i++) {
            uint32_t ne, k, ok = 1;
            reset(&a); p = P0; p.vel = V[i].mode;
            chord(&a, &p, GCE, VEL, 3);
            for (n = 0; n < 3u * 6u * ARP_TICK; n += ARP_TICK) {
                ne = arp_advance(&a, &p, 0, ARP_TICK, ev, ARP_EVMAX);
                for (k = 0; k < ne; k++)
                    if (!ev[k].on) on_count[ev[k].note]--;
                    else {
                        on_count[ev[k].note]++;
                        uint32_t w = V[i].want ? V[i].want : ev[k].note == 67 ? 20u : ev[k].note == 60 ? 120u : 70u;
                        if (ev[k].vel != w) ok = 0;
                    }
            }
            CHECK(ok, "velocity mode %u", V[i].mode);
        }
    }
    {                                           /* 7 */
        static const uint8_t CEG[3] = {60, 64, 67}, FIVE[5] = {60, 62, 64, 65, 67};
        uint16_t pt[16];
        uint32_t st = 6u * ARP_TICK;
        PAT = pt;
        reset(&a); p = P0; arp_pattern_text(pt, "x.x.");    /* pauses: C E G on steps 0 2 4, not C . G . */
        p.plen = 4; chord(&a, &p, CEG, 0, 3);
        n = play(&a, &p, 6 * 6, out, 64, on_t, 0);
        CHECK(n == 3 && out[0] == 60 && out[1] == 64 && out[2] == 67 && on_t[1] - on_t[0] == 2u * st,
              "pause: %u notes %u %u %u", n, out[0], out[1], out[2]);
        reset(&a); p = P0; arp_pattern_text(pt, "xp"); p.plen = 2; chord(&a, &p, CEG, 0, 3);   /* C C E E G G */
        n = play(&a, &p, 6 * 6, out, 64, 0, 0);
        want[0] = 60; want[1] = 60; want[2] = 64; want[3] = 64; want[4] = 67; want[5] = 67;
        CHECK(n == 6 && same(out, want, 6), "previous: %u %u %u %u %u %u", out[0], out[1], out[2], out[3], out[4], out[5]);
        reset(&a); p = P0; arp_pattern_text(pt, "flb"); p.plen = 3; chord(&a, &p, CEG, 0, 3);  /* C, G, C+G */
        n = play(&a, &p, 3 * 6, out, 64, 0, 0);
        CHECK(n == 4 && out[0] == 60 && out[1] == 67 && out[2] == 60 && out[3] == 67, "first last first+last: %u notes", n);
        reset(&a); p = P0; arp_pattern_text(pt, "c"); p.plen = 1; chord(&a, &p, FIVE, 0, 5);   /* 4 of 5 at once */
        n = play(&a, &p, 1 * 6, out, 64, on_t, 0);
        CHECK(n == 4 && on_t[3] == on_t[0], "chord: %u notes", n);
        reset(&a); p = P0; p.oct = 2; arp_pattern_text(pt, "r"); p.plen = 1; chord(&a, &p, CEG, 0, 3);
        n = play(&a, &p, 40 * 6, out, 64, 0, 0);
        for (r = 0, i = 0; i < 40u && i < n; i++)
            if (out[i] != 60 && out[i] != 64 && out[i] != 67 && out[i] != 72 && out[i] != 76 && out[i] != 79) r++;
        CHECK(n == 40 && !r && a.k == 0, "random: %u notes, %u outside, the sequence moved %u", n, r, a.k);
        reset(&a); p = P0; arp_pattern_text(pt, "Xx"); p.plen = 2;   /* accent +64: 100 -> 127 (capped) */
        pt[1] = AST_MAKE(AS_NORMAL, 0u, 2u, 4u, 4u);            /* -64: 100 -> 36 */
        chord(&a, &p, CEG, 0, 3);
        n = play(&a, &p, 2 * 6, out, 64, 0, 0);
        CHECK(n == 2 && outv[0] == 127 && outv[1] == 36, "accents: %u %u", outv[0], outv[1]);
        reset(&a); p = P0; arp_pattern_text(pt, "xx"); p.plen = 2;   /* mute: step 0 counts (C), step 1 plays E */
        pt[0] = AST_MAKE(AS_NORMAL, 0u, 0u, 4u, 4u);
        chord(&a, &p, CEG, 0, 3);
        n = play(&a, &p, 2 * 6, out, 64, 0, 0);
        CHECK(n == 1 && out[0] == 64, "mute: %u notes, the first %u", n, out[0]);
        reset(&a); p = P0; arp_pattern_text(pt, "-x"); p.plen = 2; chord(&a, &p, CEG, 0, 3);   /* legato, then 50 % */
        pt[1] = AST_MAKE(AS_NORMAL, 0u, 4u, 7u, 4u);            /* +3: 50 + 45 = 95 % */
        n = play(&a, &p, 2 * 6 + 1, out, 64, on_t, off_t);
        CHECK(n >= 2 && off_t[0] == on_t[1] && off_t[1] - on_t[1] - st * 95u / 100u < 32u,   /* (seen to 1/8 tick) */
              "length: legato ends %u (next %u), +3 lasts %u", off_t[0], on_t[1], off_t[1] - on_t[1]);
        reset(&a); p = P0; p.tfac = 127; arp_pattern_text(pt, "xx"); p.plen = 2; chord(&a, &p, CEG, 0, 3);
        pt[1] = AST_MAKE(AS_NORMAL, 0u, 4u, 4u, 7u);            /* +3 at TIMING 127: half a step late */
        n = play(&a, &p, 3 * 6, out, 64, on_t, 0);
        CHECK(n == 3 && on_t[1] - on_t[0] == st + st / 2u && on_t[2] - on_t[0] == 2u * st, "timing +3: at %u, %u",
              on_t[1] - on_t[0], on_t[2] - on_t[0]);
        reset(&a); p.tfac = 0; chord(&a, &p, CEG, 0, 3);       /* TIMING 0: on the grid */
        n = play(&a, &p, 3 * 6, out, 64, on_t, 0);
        CHECK(n == 3 && on_t[1] - on_t[0] == st, "timing at factor 0: %u", on_t[1] - on_t[0]);
        reset(&a); p = P0; arp_pattern_text(pt, "x.x.x.x."); p.plen = 3; chord(&a, &p, CEG, 0, 3);   /* x . x | x . x */
        n = play(&a, &p, 6 * 6, out, 64, on_t, 0);
        CHECK(n == 4 && on_t[1] - on_t[0] == 2u * st && on_t[2] - on_t[0] == 3u * st && on_t[3] - on_t[0] == 5u * st,
              "pattern length 3: %u notes", n);
        reset(&a); p = P0; arp_pattern_text(pt, "=x"); p.plen = 2; chord(&a, &p, CEG, 0, 3);   /* a tie: C holds */
        n = play(&a, &p, 4 * 6, out, 64, on_t, off_t);           /* through step 1, ends at its gate (50 %) */
        CHECK(n == 2 && out[0] == 60 && out[1] == 64 && on_t[1] - on_t[0] == 2u * st
              && off_t[0] - on_t[0] - (st + st / 2u) < 32u, "tie: %u notes, %u %u, the first lasts %u", n, out[0],
              out[1], off_t[0] - on_t[0]);
        reset(&a); p = P0; arp_pattern_text(pt, "==x"); p.plen = 3; chord(&a, &p, CEG, 0, 3);   /* two ties: 2.5 */
        n = play(&a, &p, 4 * 6, out, 64, on_t, off_t);
        CHECK(n == 2 && out[1] == 64 && on_t[1] - on_t[0] == 3u * st && off_t[0] - on_t[0] - (2u * st + st / 2u) < 32u,
              "tie chain: %u notes, the first lasts %u", n, off_t[0] - on_t[0]);
        reset(&a); p = P0; arp_pattern_text(pt, "=x..."); p.plen = 2; chord(&a, &p, CEG, 0, 3);
        pt[0] = (uint16_t)(AST_MAKE(AS_NORMAL, 0u, 0u, 4u, 4u) | AST_TIEBIT);   /* a muted step's tie: nothing to hold */
        n = play(&a, &p, 2 * 6, out, 64, on_t, 0);
        CHECK(n == 1 && out[0] == 64, "tie on a muted step: %u notes, %u", n, out[0]);
        reset(&a); p = P0; arp_pattern_text(pt, "xg-gcg"); p.plen = 6; chord(&a, &p, CEG, 0, 3);   /* 8 */
        pt[4] = AST_MAKE(AS_CHORD, 0u, 4u, 0u, 4u);             /* (a chord held legato: then a glide, not a slide) */
        n = play(&a, &p, 6 * 6, out, 64, on_t, off_t);
        CHECK(n == 8 && outg[0] == ARP_GL_NONE && outg[1] == ARP_GL_GLIDE && outg[2] == ARP_GL_NONE
              && outg[3] == ARP_GL_SLIDE && outf[3] == out[2] && outg[7] == ARP_GL_GLIDE,
              "glide: %u notes, glides %u %u %u %u .. %u, slide from %u (want %u)", n, outg[0], outg[1], outg[2],
              outg[3], outg[7], outf[3], out[2]);
        reset(&a); p = P0; arp_pattern_text(pt, "-g"); p.plen = 2; chord(&a, &p, CEG, 0, 3);   /* C slides to E */
        n = play(&a, &p, 2 * 6, out, 64, on_t, off_t);
        CHECK(n == 2 && out[0] == 60 && out[1] == 64 && outg[1] == ARP_GL_SLIDE && outf[1] == 60 && on_count[60] == 0
              && on_count[64] == 0 && off_t[0] - on_t[1] - st / 2u < 32u,
              "slide: %u notes %u -> %u (%u), E ends %u after it starts", n, out[0], out[1], outg[1], off_t[0] - on_t[1]);
        reset(&a); p = P0; arp_pattern_text(pt, "-g"); p.plen = 2; chord(&a, &p, CEG, 0, 3);   /* a muted glide */
        pt[1] = AST_MAKE(AS_NORMAL, 1u, 0u, 4u, 4u);
        n = play(&a, &p, 2 * 6, out, 64, 0, 0);
        CHECK(n == 1 && outg[0] == ARP_GL_NONE, "muted glide: %u notes", n);
        reset(&a); p = P0; p.mode = ARP_ONESHOT; arp_pattern_text(pt, "xxxxxxxxxxxxxxxx"); p.plen = 16;   /* ONE SHOT */
        chord(&a, &p, CEG, 0, 3);               /* with a pattern: all its 16 steps once, not one pass of 3 notes */
        n = play(&a, &p, 20 * 6, out, 64, 0, 0);
        CHECK(n == 16 && !a.run, "ONE SHOT, a pattern of 16: %u notes (want 16), still running %u", n, a.run);
        reset(&a); p = P0; p.mode = ARP_ONESHOT; arp_pattern_text(pt, "x.x."); p.plen = 4; chord(&a, &p, CEG, 0, 3);
        n = play(&a, &p, 8 * 6, out, 64, 0, 0);
        CHECK(n == 2 && !a.run, "ONE SHOT, x.x. of 4: %u notes (want 2)", n);
        reset(&a); p = P0; p.mode = ARP_ONESHOT; arp_pattern_text(pt, "xxx="); p.plen = 4; chord(&a, &p, CEG, 0, 3);
        n = play(&a, &p, 8 * 6, out, 64, on_t, off_t);   /* a tie on the last step ends with the pattern */
        CHECK(n == 4 && !a.run && off_t[3] - on_t[0] == 4u * st, "ONE SHOT, a tie at the end: %u notes, the last "
              "ends %u after the first starts (want %u)", n, off_t[3] - on_t[0], 4u * st);
        n = arp_pattern_text(pt, "xX-.pflbcCr?q");
        CHECK(n == 13 && AST_TYPE(pt[3]) == AS_PAUSE && AST_ACC(pt[1]) == 6u && AST_LEN(pt[2]) == 0u
              && AST_TYPE(pt[9]) == AS_CHORD && AST_TYPE(pt[10]) == AS_RANDOM && pt[11] == AST_DEFAULT
              && AST_TYPE(pt[12]) == AS_PREV && AST_ACC(pt[12]) == 3u && pt[15] == AST_DEFAULT, "pattern text");
        PAT = 0;
    }
    {                                           /* 6 */
        arp_ev_t ev[ARP_CHORD];
        reset(&a); p = P0; p.gate = 100;
        chord(&a, &p, GCE, 0, 3);
        play(&a, &p, 9, out, 64, 0, 0);
        n = arp_clear(&a, ev);
        for (i = 0; i < n; i++) on_count[ev[i].note]--;
        for (i = 0; i < 128u; i++) CHECK(on_count[i] == 0, "note %u: %d ons without an off", i, on_count[i]);
    }
    printf(fails ? "arp: %d FAILED\n" : "arp: all passed\n", fails);
    return fails != 0;
}
