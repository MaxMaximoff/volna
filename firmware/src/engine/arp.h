/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna engine: the arpeggiator, in the spirit of the Blofeld's. Stage 5a: modes OFF / ON / ONE SHOT / HOLD,
 * directions UP / DOWN / UP-DOWN / DOWN-UP, the order of the notes (as played, reversed, by key, by velocity),
 * 1..10 octaves, a rate in MIDI clock ticks (24 a quarter note), the gate, swing, where the velocity comes from.
 * ONE SHOT plays once: with a pattern, its LENGTH steps (pauses, chords and ties as written; a tie on the last step
 * ends with it); without one, one pass of the notes.
 * Stage 5b: a pattern of up to 16 steps, each a type (normal, pause, previous, first, last, first + last, chord,
 * random), an accent, a length and a timing, a tie; and a glide flag (stage 6b): the step's notes glide in from the
 * note before; after a step held legato (a note each) it is a slide, as on a 303: the note held moves to the new
 * one, no new attack.
 *
 * The main loop owns it: keys go in (arp_key_on / arp_key_off), time goes in (arp_advance: Q8 clock ticks, from the
 * internal tempo or from MIDI clock), note events come out for the voices. Times are kept as a schedule (the grid
 * of steps in ticks), so a late call plays late but never drifts. Nothing here touches the hardware.
 *
 * A pattern step is 16 bits: type 0..2, glide 3, accent 4..6 (0 mute, 1..7 = -96 .. +96 velocity, 4 none),
 * length 7..9 (0 legato, 1..7 = -3 .. +3 x 15 % of the step against GATE, 4 none), timing 10..12 (0 random,
 * 1..7 = -3 .. +3: the step early or late, by TIMING (the factor) up to half a step at +-3, 4 none), tie 13.
 * A tie (as on the SQ-64 or a 303, the way mutagen-arp does it): the step's notes hold through the next step, which
 * plays nothing of its own (whatever its type); that step's length says when they end, its own tie holds them on
 * further. (Unlike a legato length, which lets the next step play its next note with no gap.)
 * The sequence moves on only with NORMAL steps (and PREVIOUS while there is no previous note yet): a pause, a
 * repeat, the first or the last note, a chord, a random note leave it where it is. A muted step (accent 0) moves it
 * on and does not sound. */
#ifndef VOLNA_ARP_H
#define VOLNA_ARP_H
#include <stdint.h>

#define ARP_MAXN 16u                            /* notes held at once */
#define ARP_TICK 256u                           /* a MIDI clock tick in arp time units (Q8) */
#define ARP_CHORD 4u                            /* a chord step plays this many at most (Volna has 4 voices) */
#define ARP_NPAT 16u                            /* patterns in the bank (all of them the user's) */
#define ARP_PLEN 16u                            /* steps in a pattern */
#define ARP_EVMAX (3u * ARP_CHORD + 4u)         /* events arp_advance may give at once: a gate's ends, a step's */

enum { ARP_OFF, ARP_ON, ARP_ONESHOT, ARP_HOLD, ARP_NMODE };
enum { AD_UP, AD_DOWN, AD_UPDN, AD_DNUP, AD_N };
enum { AO_PLAYED, AO_REVERSED, AO_KEYUP, AO_KEYDN, AO_VELUP, AO_VELDN, AO_N };
enum { AV_EACH, AV_FIRST, AV_LAST, AV_32, AV_64, AV_100, AV_127, AV_N };
enum { AS_NORMAL, AS_PAUSE, AS_PREV, AS_FIRST, AS_LAST, AS_FIRSTLAST, AS_CHORD, AS_RANDOM, AS_N };

#define ARP_NRATE 14u                           /* a step in clock ticks: 1/1 .. 1/32 triplets, longest first */
static const uint8_t ARP_RATE_TICKS[ARP_NRATE] = {96, 48, 36, 32, 24, 18, 16, 12, 9, 8, 6, 4, 3, 2};
static const char *const ARP_RATE_NAME[ARP_NRATE] = {
    "1/1", "1/2", "1/4.", "1/2T", "1/4", "1/8.", "1/4T", "1/8", "16.", "1/8T", "1/16", "16T", "1/32", "32T"};
#define ARP_RATE_16TH 10u

/* a step's fields */
#define AST_TYPE(s) ((uint32_t)(s) & 7u)
#define AST_GLIDE(s) (((uint32_t)(s) >> 3) & 1u)
#define AST_ACC(s) (((uint32_t)(s) >> 4) & 7u)
#define AST_LEN(s) (((uint32_t)(s) >> 7) & 7u)
#define AST_TIM(s) (((uint32_t)(s) >> 10) & 7u)
#define AST_TIE(s) (((uint32_t)(s) >> 13) & 1u)
#define AST_TIEBIT 0x2000u
#define AST_MAKE(type, glide, acc, len, tim) \
    ((uint16_t)((type) | (glide) << 3 | (acc) << 4 | (len) << 7 | (tim) << 10))
#define AST_DEFAULT AST_MAKE(AS_NORMAL, 0u, 4u, 4u, 4u)   /* a plain step: the next note, as it is */

typedef struct {                                /* the settings (they live in the patch) */
    uint8_t mode, dir, order, oct;              /* ARP_*, AD_*, AO_*, 1..10 */
    uint8_t rate, gate, swing, vel;             /* ARP_RATE_TICKS index; 5..100 % of the step (100: legato, the
                                                   note holds into the next); 0..75 % of a step later every second
                                                   step; AV_* */
    uint8_t last;                               /* the mode RUN brings back (ON, ONE SHOT or HOLD) */
    uint8_t pattern;                            /* 0 none (every step plain), 1..16 the bank's */
    uint8_t plen;                               /* the pattern's steps, 1..16 */
    uint8_t tfac;                               /* TIMING: how far the steps' timing moves them, 0..127 */
    uint16_t tempo;                             /* 40..300 BPM (when no MIDI clock comes) */
} arp_par_t;

/* a note event. A note on with a glide: ARP_GL_GLIDE glides in from the note before; ARP_GL_SLIDE is a slide (the
 * step's GLIDE after a step held legato, a note each: the voice playing from moves to note, no new attack, no note
 * off for from) */
enum { ARP_GL_NONE, ARP_GL_GLIDE, ARP_GL_SLIDE };
typedef struct { uint8_t on, note, vel, glide, from; } arp_ev_t;

typedef struct {
    uint8_t note[ARP_MAXN], vel[ARP_MAXN];      /* the notes it plays, in the order they came */
    uint8_t n, phys;                            /* how many; how many keys are down (HOLD keeps notes let go) */
    uint8_t run, sounding, leg, tied;           /* it steps; how many of its notes sound (snote); they hold to
                                                   the next step (legato, or a tie); the next step only holds them
                                                   on (a tie) */
    uint8_t snote[ARP_CHORD];
    uint8_t pnote, pvel;                        /* the last step's first note (PREVIOUS plays it again) */
    uint32_t k, s;                              /* the sequence's next entry; the step about to play (the
                                                   pattern's step s % plen, swing on the odd ones) */
    uint32_t t, grid, next, off;                /* arp time (Q8 ticks): now; step s on the grid; when it plays
                                                   (swing, timing); when the sounding notes end */
    uint32_t rnd;                               /* RANDOM's and the random timing's */
} arp_t;

/* a pattern written as text, a step a character (the bank's first fill, tests): x a note, X with an accent (+64),
 * - a note held into the next (legato), . a pause, p the previous note, f the first, l the last, b first + last,
 * c a chord, C an accented chord, r a random note, = a note tied into the next step, g a note gliding in (after a
 * legato one: a slide); anything else plain. Returns the steps written (<= 16). */
static uint32_t arp_pattern_text(uint16_t *st, const char *s)
{
    uint32_t i, n;
    for (i = 0; i < ARP_PLEN && s[i]; i++) {
        uint32_t c = (uint8_t)s[i];
        st[i] = c == 'X' ? AST_MAKE(AS_NORMAL, 0u, 6u, 4u, 4u) : c == '-' ? AST_MAKE(AS_NORMAL, 0u, 4u, 0u, 4u)
              : c == '.' ? AST_MAKE(AS_PAUSE, 0u, 4u, 4u, 4u) : c == 'p' ? AST_MAKE(AS_PREV, 0u, 4u, 4u, 4u)
              : c == 'f' ? AST_MAKE(AS_FIRST, 0u, 4u, 4u, 4u) : c == 'l' ? AST_MAKE(AS_LAST, 0u, 4u, 4u, 4u)
              : c == 'q' ? AST_MAKE(AS_PREV, 0u, 3u, 4u, 4u) : c == 'b' ? AST_MAKE(AS_FIRSTLAST, 0u, 4u, 4u, 4u) : c == 'c' ? AST_MAKE(AS_CHORD, 0u, 4u, 4u, 4u)
              : c == 'C' ? AST_MAKE(AS_CHORD, 0u, 6u, 4u, 4u) : c == 'r' ? AST_MAKE(AS_RANDOM, 0u, 4u, 4u, 4u)
              : c == '=' ? (uint16_t)(AST_DEFAULT | AST_TIEBIT) : c == 'g' ? AST_MAKE(AS_NORMAL, 1u, 4u, 4u, 4u)
              : AST_DEFAULT;
    }
    n = i;
    for (; i < ARP_PLEN; i++) st[i] = AST_DEFAULT;
    return n;
}

/* the sequence: the notes sorted by the order, then again an octave up, ... (oct times); returns its length */
static uint32_t arp_build(const arp_t *a, const arp_par_t *p, uint8_t *notes, uint8_t *vels)
{
    uint8_t idx[ARP_MAXN];
    uint32_t i, j, o, m = 0, n = a->n;
    for (i = 0; i < n; i++) idx[i] = (uint8_t)(p->order == AO_REVERSED ? n - 1u - i : i);
    if (p->order >= AO_KEYUP)                   /* an insertion sort, stable: equal keys keep the played order */
        for (i = 1; i < n; i++) {
            uint8_t x = idx[i];
            for (j = i; j > 0; j--) {
                uint8_t y = idx[j - 1u];
                int32_t c = p->order == AO_KEYUP ? (int32_t)a->note[y] - a->note[x]
                          : p->order == AO_KEYDN ? (int32_t)a->note[x] - a->note[y]
                          : p->order == AO_VELUP ? (int32_t)a->vel[y] - a->vel[x] : (int32_t)a->vel[x] - a->vel[y];
                if (c <= 0) break;
                idx[j] = y;
            }
            idx[j] = x;
        }
    for (o = 0; o < (p->oct ? p->oct : 1u) && o < 10u; o++)
        for (i = 0; i < n; i++) {
            uint32_t nt = a->note[idx[i]] + 12u * o;
            if (nt > 127u) continue;
            notes[m] = (uint8_t)nt;
            vels[m++] = a->vel[idx[i]];
        }
    return m;
}

/* the steps in one pass of the direction over a sequence of len, and which entry step k plays */
static uint32_t arp_period(uint32_t dir, uint32_t len)
{
    return (dir == AD_UPDN || dir == AD_DNUP) && len > 1u ? 2u * len - 2u : len;
}
static uint32_t arp_index(uint32_t dir, uint32_t len, uint32_t k)
{
    uint32_t per = arp_period(dir, len), m = k % per, i = m < len ? m : per - m;   /* (UP-DOWN: 0 1 2 3 2 1) */
    return dir == AD_DOWN || dir == AD_DNUP ? len - 1u - i : i;
}

static void arp_start(arp_t *a)                 /* step 0, now */
{
    a->run = 1;
    a->k = a->s = 0;
    a->grid = a->next = a->t;
}

/* a key down: HOLD starts a new chord when no key was down; a note already in keeps its place */
static void arp_key_on(arp_t *a, const arp_par_t *p, uint32_t note, uint32_t vel)
{
    uint32_t i;
    if (p->mode == ARP_HOLD && !a->phys) a->n = 0;
    if (a->phys < 255u) a->phys++;
    for (i = 0; i < a->n && a->note[i] != note; i++) {}
    if (i < a->n) a->vel[i] = (uint8_t)vel;
    else if (a->n < ARP_MAXN) { a->note[a->n] = (uint8_t)note; a->vel[a->n++] = (uint8_t)vel; }
    if (!a->run) arp_start(a);
}
/* a key up: gone from the sequence, except in HOLD */
static void arp_key_off(arp_t *a, const arp_par_t *p, uint32_t note)
{
    uint32_t i, j;
    if (a->phys) a->phys--;
    if (p->mode == ARP_HOLD) return;
    for (i = 0; i < a->n; i++)
        if (a->note[i] == note) {
            for (j = i; j + 1u < a->n; j++) { a->note[j] = a->note[j + 1u]; a->vel[j] = a->vel[j + 1u]; }
            a->n--;
            break;
        }
}

static uint32_t arp_rand(arp_t *a)              /* xorshift32 */
{
    uint32_t x = a->rnd ? a->rnd : 0x2545F491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return a->rnd = x;
}
static uint32_t arp_ends(arp_t *a, arp_ev_t *ev)   /* the sounding notes end */
{
    uint32_t i, n = a->sounding;
    for (i = 0; i < n; i++) { ev[i].on = 0; ev[i].note = a->snote[i]; ev[i].vel = 0; ev[i].glide = 0; ev[i].from = 0; }
    a->sounding = 0;
    return n;
}
/* step s's offset from the grid (Q8 ticks, may be < 0): swing on the odd steps, the pattern step's timing */
static int32_t arp_shift(arp_t *a, const arp_par_t *p, const uint16_t *pat, uint32_t s, uint32_t step)
{
    int32_t sh = (s & 1u) ? (int32_t)(step * (p->swing > 75u ? 75u : p->swing) / 100u) : 0;
    if (pat) {
        uint32_t plen = p->plen && p->plen <= ARP_PLEN ? p->plen : ARP_PLEN, tm = AST_TIM(pat[s % plen]);
        int32_t u = tm ? (int32_t)tm - 4 : (int32_t)(arp_rand(a) % 7u) - 3;   /* -3 .. +3 */
        sh += u * (int32_t)(step / 2u) / 3 * (int32_t)(p->tfac > 127u ? 127u : p->tfac) / 127;
    }
    return sh;
}

/* time goes on by dt (Q8 ticks); the note events due meanwhile go into ev (up to max: ARP_EVMAX or more); returns
 * how many. pat: the pattern's 16 steps, or 0 for none (every step plain) */
static uint32_t arp_advance(arp_t *a, const arp_par_t *p, const uint16_t *pat, uint32_t dt, arp_ev_t *ev,
                            uint32_t max)
{
    uint8_t notes[ARP_MAXN * 10u], vels[ARP_MAXN * 10u];
    uint32_t ne = 0;
    a->t += dt;
    if (a->sounding && !a->leg && (int32_t)(a->t - a->off) >= 0 && ne + ARP_CHORD <= max)   /* the gate's end */
        ne += arp_ends(a, ev + ne);
    if (a->run && (int32_t)(a->t - a->next) > (int32_t)(4u * 96u * ARP_TICK))   /* far behind (a stalled loop): */
        a->grid = a->next = a->t;               /* go on from now rather than play a burst of steps */
    while (a->run && (int32_t)(a->t - a->next) >= 0 && ne + 2u * ARP_CHORD <= max) {
        uint32_t len = arp_build(a, p, notes, vels), plen = p->plen && p->plen <= ARP_PLEN ? p->plen : ARP_PLEN;
        uint32_t st = pat ? pat[a->s % plen] : AST_DEFAULT, ty = AST_TYPE(st), acc = AST_ACC(st), sl = AST_LEN(st);
        uint32_t step, start, i, c = 0, nt[ARP_CHORD], vb[ARP_CHORD], gl, from = 0;
        int32_t g;
        step = ARP_RATE_TICKS[p->rate < ARP_NRATE ? p->rate : ARP_RATE_16TH] * ARP_TICK;
        g = sl ? (int32_t)(p->gate < 5u ? 5u : p->gate) + ((int32_t)sl - 4) * 15 : 100;   /* (LENGTH: +-15 % a unit) */
        g = g < 5 ? 5 : g > 100 ? 100 : g;
        if (!len || (p->mode == ARP_ONESHOT && (pat ? a->s >= plen : a->k >= arp_period(p->dir, len)))) {
            ne += arp_ends(a, ev + ne);
            a->run = a->tied = 0;               /* (ONE SHOT: with a pattern its LENGTH steps once, a tie on the last
                                                   one ending with it; without one, one pass of the notes) */
            break;
        }
        if (a->tied && a->sounding) {           /* the step before tied its notes over this one: they hold on */
            a->tied = (uint8_t)AST_TIE(st);
            goto next_step;
        }
        a->tied = 0;
        switch (ty) {                           /* the notes this step plays (the sequence moves on with NORMAL) */
        case AS_NORMAL: i = arp_index(p->dir, len, a->k++); nt[c] = notes[i]; vb[c++] = vels[i]; break;
        case AS_PREV:
            if (a->pnote) { nt[c] = a->pnote; vb[c++] = a->pvel; }
            else { i = arp_index(p->dir, len, a->k++); nt[c] = notes[i]; vb[c++] = vels[i]; }   /* (none yet) */
            break;
        case AS_FIRST: nt[c] = notes[0]; vb[c++] = vels[0]; break;
        case AS_LAST: nt[c] = notes[len - 1u]; vb[c++] = vels[len - 1u]; break;
        case AS_FIRSTLAST:
            nt[c] = notes[0]; vb[c++] = vels[0];
            if (len > 1u) { nt[c] = notes[len - 1u]; vb[c++] = vels[len - 1u]; }
            break;
        case AS_CHORD: for (i = 0; i < len && i < a->n && c < ARP_CHORD; i++) { nt[c] = notes[i]; vb[c++] = vels[i]; } break;
        case AS_RANDOM: i = arp_rand(a) % len; nt[c] = notes[i]; vb[c++] = vels[i]; break;
        default: break;                         /* (a pause) */
        }
        gl = AST_GLIDE(st) && acc && c ? ARP_GL_GLIDE : ARP_GL_NONE;
        if (gl && c == 1u && a->leg && a->sounding == 1u) {   /* a slide: the held note moves to this one */
            gl = ARP_GL_SLIDE;
            from = a->snote[0];
            a->sounding = 0;                    /* (no note off for it: the voice goes on) */
        } else ne += arp_ends(a, ev + ne);      /* (legato, or a gate that ends with the step) */
        if (c) { a->pnote = (uint8_t)nt[0]; a->pvel = (uint8_t)vb[0]; }
        for (i = 0; acc && i < c; i++) {        /* (accent 0: a muted step: it counts, it does not sound) */
            int32_t v = (int32_t)(p->vel == AV_FIRST && a->n ? a->vel[0] : p->vel == AV_LAST && a->n ? a->vel[a->n - 1u]
                      : p->vel == AV_32 ? 32u : p->vel == AV_64 ? 64u : p->vel == AV_100 ? 100u
                      : p->vel == AV_127 ? 127u : vb[i]) + ((int32_t)acc - 4) * 32;
            ev[ne].on = 1; ev[ne].note = (uint8_t)nt[i]; ev[ne].glide = (uint8_t)gl; ev[ne].from = (uint8_t)from;
            ev[ne++].vel = (uint8_t)(v < 1 ? 1 : v > 127 ? 127 : v);
            a->snote[a->sounding++] = (uint8_t)nt[i];
        }
        a->tied = (uint8_t)(AST_TIE(st) && a->sounding);
    next_step:
        start = a->next;
        a->s++;
        a->grid += step;
        a->next = a->grid + (uint32_t)arp_shift(a, p, pat, a->s, step);
        if ((int32_t)(a->next - start) < 1) a->next = start + 1u;   /* (an early step never before this one) */
        a->leg = a->tied || g >= 100;           /* (a tie holds them through the next step) */
        a->off = start + step * (uint32_t)g / 100u;
        if ((int32_t)(a->off - a->next) > 0) a->off = a->next;   /* (a swung or shifted step is shorter) */
    }
    return ne;
}

/* the mode changed, or all over: nothing held, nothing stepping; the sounding notes' ends into ev (<= ARP_CHORD) */
static uint32_t arp_clear(arp_t *a, arp_ev_t *ev)
{
    uint32_t ne = arp_ends(a, ev);
    a->run = a->tied = 0;
    a->n = a->phys = 0;
    a->pnote = 0;
    return ne;
}
#endif
