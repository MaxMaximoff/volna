/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: a wavetable synth with an arpeggiator for the M-VAVE FM-1 (fm-firmwares, ideas/03-wavetable-synth.md).
 * Stage 3: 4 voices of three wavetable oscillators (engine/wt_osc.h), noise with a colour and ring modulation, two
 * state-variable filters in series or side by side (engine/svf.h) with their own envelope, an ADSR on the level
 * (engine/env.h); three LFOs and an 8-slot modulation matrix (engine/mod.h, stage 4a); an arpeggiator with 16
 * patterns of 16 steps (engine/arp.h, stages 5a 5b); a bank of 100 patches (stage 6a); glide, POLY / MONO / LEGATO
 * and the pitch bend (stage 6b); unison (stage 7a); two effect slots, DELAY, REVERB, DIST, CHORUS (engine/fx.h, stage 7b); the
 * screen and the panel as docs/ui/UI.md designs them (ui.h).
 * The synth grows stage by stage (NOTES.md).
 *
 * Built on Dinghy: the smallest FM-1 firmware to build your own on. Everything below this file (platform.c) is
 * done: updates, the USB rescue, MIDI (USB, the TRS jack), USB audio, keys, knobs, LEDs, the screen, flash
 * storage; the FM-1 and the SMK-37.
 *
 * The panel (docs/ui/UI.md; the faceplate is Volna's own, the stock labels in brackets):
 *   OSC [FX] MIX [SCL] FILT [ENV] ENV [LFO] MOD [EDIT] FX [GLO]   the sections; again: the page's row B
 *   PATCH [HOME]  SAVE  ARP  STEPS [SEQ]  RUN [PLAY]  SHIFT [REC]
 *   KNOB1..4   the four cells on top of the screen       SELECT     which one in the section (OSC 1/2/3,
 *                                                                   FILTER 1/2, ENV AMP/FLT)
 *   ALGORITHM  MACRO: MORPH, all three table positions   PRESETS    the patches (stage 6)
 *   OCT- OCT+  the octave (SHIFT: transpose)              MASTER     the volume (platform.c)
 *   SHIFT +    a section: its defaults; PATCH: SETUP; a knob: fine steps; held: the cheat sheet on screen
 *   MIDI in    notes from USB or the TRS jack, with their velocity; a MIDI clock blinks PLAY, START / STOP light it
 *   SMK-37     the keys with velocity, the outer keys, the pads, the pitch wheel (+-BEND semitones, as MIDI's)
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer: the screen as a PNG, the sound as a WAV.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
#define APP_NAME "VOLNA"                        /* the rescue and the crash screen say this */
static void app_idle(void);
#define LCD_IDLE app_idle                      /* while the screen's DMA runs: MIDI in (platform.c) */
#define CV_MAX (240u * 124u)                    /* the canvas: the screen's picture zone at once (59 KB of the pool) */
#include <stdint.h>
static void ed_take(const uint8_t *p, uint32_t n);
#define OTA_APP_FRAME ed_take                  /* the editor's SysEx (F0 7D ..): ota.c hands it over (ed_service) */
#include "platform.c"
#if BOARD_SMK37
#include "smk_wheel.h"
#endif
#include "engine/wt_osc.h"
#include "engine/va_osc.h"
#include "engine/pitch.h"
#include "engine/env.h"
#include "engine/svf.h"
#include "engine/arp.h"
#include "engine/mod.h"
#include "engine/fx.h"
#include "engine/eq.h"
#include "engine/poly.h"
#include "wt_public.h"                          /* generated: tools/gen_tables.py (Volna's own, stage 8) */
#include "wt_local.h"                           /* generated: tools/gen_uwt.py, from local/ (or a stub: none) */

/* the wavetables: Volna's own (0 BASIC, PULSE, SYNC, VOICE, FM, FOLD), then the local ones (local/wavetables: not in
 * git, not published). Each with its mip-maps (engine/wt_osc.h) */
#define NTAB (WT_PUB_N + WT_LOCAL_N)
#define TAB_VA 63u                              /* the ANALOG "table" (engine/va_osc.h): computed, not read; on the
                                                   knob after the own ones (ui.h: tab_pos) */
/* The user wavetables (2026-11-17): UWT_N slots in flash (store.h), loaded from the site (the editor protocol 3,
 * TABLE ..), played in place through XIP (platform.c uwt_xip). Fixed numbers TAB_U0.. (below ANALOG), so a patch
 * means the same slot whatever local tables a build carries. A slot: its first sector the header, the table (WT_WORDS
 * int16, as the own ones: wt_osc.h) from the second. An empty or broken slot plays BASIC. */
#define TAB_U0 59u
#define UWT_NAME 12u
#define UWT_MAGIC 0x31545756u                   /* "VWT1" */
#define UWT_BYTES (WT_WORDS * 2u)
#define UWT_DATA 0x1000u                        /* the table's offset in its slot */
typedef struct {                                /* the header, at the slot's start (written last: a cut upload stays
                                                   empty) */
    uint32_t magic, bytes, crc;                 /* crc: CRC-32 (zlib's) of the table's bytes */
    char name[UWT_NAME];                        /* the panel's characters, space padded */
} uwt_head_t;
_Static_assert(TAB_U0 + UWT_N <= TAB_VA && TAB_U0 >= NTAB, "the user wavetables' numbers");
_Static_assert(UWT_N == 0u || UWT_DATA + UWT_BYTES <= UWT_SECT * 0x1000u, "a user wavetable outgrows its slot");
#if UWT_N
static uint8_t uwt_ok[UWT_N];                   /* slot k checked: header and CRC (uwt_scan) */
static uint32_t uwt_crc_of[UWT_N];
static char uwt_nm[UWT_N][UWT_NAME + 1];
static uint32_t uwt_crc32(uint32_t c, const uint8_t *p, uint32_t n)   /* CRC-32 (zlib's), a nibble at a time */
{
    static const uint32_t T[16] = {0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
                                   0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
                                   0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    c = ~c;
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return ~c;
}
static void uwt_scan(uint32_t k)                /* (main loop) slot k: usable? header, length, the table's CRC */
{
    const uint8_t *s;
    uwt_head_t h;
    uint32_t i;
    if (k >= UWT_N) return;
    s = uwt_xip(k);
    uwt_ok[k] = 0;
    for (i = 0; i < UWT_NAME; i++) uwt_nm[k][i] = ' ';
    uwt_nm[k][UWT_NAME] = 0;
    if (!s) return;
    memcpy(&h, s, sizeof h);
    if (h.magic != UWT_MAGIC || h.bytes != UWT_BYTES || uwt_crc32(0, s + UWT_DATA, UWT_BYTES) != h.crc) return;
    for (i = 0; i < UWT_NAME; i++) uwt_nm[k][i] = h.name[i] >= ' ' && h.name[i] <= 'Z' ? h.name[i] : ' ';
    uwt_crc_of[k] = h.crc;
    uwt_ok[k] = 1;
}
#endif
static const int16_t *wt_table(uint32_t t)      /* (ANALOG: BASIC, never read) */
{
#if UWT_N
    if (t >= TAB_U0 && t < TAB_U0 + UWT_N) {
        const uint8_t *s = uwt_ok[t - TAB_U0] ? uwt_xip(t - TAB_U0) : 0;
        return s ? (const int16_t *)(const void *)(s + UWT_DATA) : WT_PUB[0];
    }
#endif
#if WT_LOCAL_N
    if (t >= WT_PUB_N && t < NTAB) return WT_LOCAL[t - WT_PUB_N];
#endif
    return WT_PUB[t < WT_PUB_N ? t : 0u];
}
static const char *wt_name(uint32_t t)
{
    if (t == TAB_VA) return "ANALOG";
#if UWT_N
    if (t >= TAB_U0 && t < TAB_U0 + UWT_N) {
        static char nms[UWT_N][UWT_NAME + 1];   /* a filled slot: its name, trailing spaces cut; an empty one (it plays
                                                   BASIC): "USER n EMPTY" (2026-11-17, the user's choice) */
        uint32_t k = t - TAB_U0, n = UWT_NAME;
        char *nm = nms[k];
        if (!uwt_ok[k]) { memcpy(nm, "USER 0 EMPTY", 13); nm[5] = (char)('1' + k); return nm; }
        memcpy(nm, uwt_nm[k], UWT_NAME + 1u);
        while (n && nm[n - 1u] == ' ') nm[--n] = 0;
        return n ? nm : "USER";
    }
#endif
#if WT_LOCAL_N
    if (t >= WT_PUB_N && t < NTAB) return WT_LOCAL_NAME[t - WT_PUB_N];
#endif
    return WT_PUB_NAME[t < WT_PUB_N ? t : 0u];
}
static int tab_known(uint32_t t)                /* a table this build plays: its own, the local ones, ANALOG, a user slot */
{
    return t < NTAB || t == TAB_VA || (t >= TAB_U0 && t < TAB_U0 + UWT_N);
}

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen) */
#define NV 4u                                   /* voices: 4 notes at most, by design: the CPU left goes into the
                                                   sound (unison, quality), not into more notes (ideas/03) */
#define NOSC 3u                                 /* wavetable oscillators a voice */
#define UMAX 7u                                 /* unison copies of an oscillator, at most (stage 7a) */
enum { FMS_OFF, FMS_O1, FMS_O2, FMS_O3, FMS_NOISE, FMS_N };   /* FM's sources (FM: below, by fm_src) */
static struct {
    volatile uint32_t inc;                      /* phase step per frame: f * 2^32 / 44100 (unbent) } main loop  */
    volatile uint8_t gate;                      /* the key is down                                } this order, */
    volatile uint8_t vel;                       /* the velocity, 1..127 (the filters' VELO)       } this order, */
    volatile uint32_t gk;                       /* the glide to it: a coefficient a step (ENV_COEF), 0 none   }        */
    volatile uint8_t gfrom;                     /* from this note; 255: from where the voice's pitch is now }       */
    volatile uint32_t km[NOSC];                 /* each oscillator's KEYTRK for this note: its step x km / 2^20 (1 at
                                                   100 %, and at C4) } before trig */
    volatile uint8_t leg;                       /* LEG_ALL legato: the envelopes go on (no attack); } bits 0..3: AMP
                                                   FLT 3 4 on SINGLE trigger: on while they sound   }             */
    volatile uint32_t trig;                     /* +1 a note-on: the ISR starts the attack        } trig last   */
    volatile uint8_t busy;                      /* the ISR's answer: the envelope is not idle (the voice sounds) */
    volatile uint8_t kill;                      /* (main loop writes) taken for the voices' limit (engine/poly.h): it
                                                   fades out in ~6 ms whatever its envelope; a note on it clears it */
    int32_t gofs;                               /* the ISR's: the glide's distance left, Q16 semitones (0: there) */
    uint32_t gkn;                               /* the ISR's: the glide's coefficient for this note */
    uint8_t gnote;                              /* the ISR's: the note it last started + 1 (0: none yet) */
    volatile int32_t fn[2];                     /* the ISR's: the filters' cutoffs now, Q8 notes (for the screen) */
    env_t xenv[2];                              /* ENV 3, ENV 4 (the matrix's; stepped while a slot reads them) */
    env_t env, fenv;                            /* the ISR's own from here (the screen only reads them): the level's
                                                   envelope, the filters' */
    svf_state_t sf[2];
    svf_state_t sr[2];                          /* the right side's filters (unison SPREAD: a stereo voice) */
    uint32_t uph[NOSC][UMAX];                   /* the unison copies' phases, free running */
    lfo_t lfo[NLFO];                            /* its LFOs (those with KEY on) */
    int32_t lfade[NLFO];                        /* the LFOs' fade-in since the note (DELAY), Q30 */
    int32_t nrnd;                               /* the note's random value (source RAND), Q15 */
    volatile int32_t ppress;                    /* its note's poly pressure, Q15 (main loop writes; a note: 0) */
    int32_t mv[NMODF];                          /* the modifiers' values as the last step left them (the ISR's) */
    int32_t lsm[NLFO], am[4];                   /* the matrix's L.SPD and AMT1..4 of the last step (the ISR's) */
    int32_t fmv[FMS_N];                         /* FM's sources as the last sample left them: 0, OSC 1..3, the noise */
    uint32_t fms[NOSC];                         /* each oscillator's FM source, as fm_prep last put it (the ISR's) */
    uint32_t mph;                               /* sync: its master's phase (OSC3's pitch) */
    int32_t s1d;                                /* sync: OSC2's sample held back a sample (the polyBLEP's) */
    uint32_t cw;                                /* the combs' write position (2026-11-16; their lines: comb_buf) */
    int32_t dcb[2][2][2];                       /* the lopsided DRIVE curves' DC blockers: [side][filter][x, y] */
    int32_t wofs[NOSC];                         /* its modulation of the table positions, Q16, as the last block ended */
    uint8_t wedge;                              /* the matrix pushed a position past the table's end, as the last
                                                   block ended (a bit an oscillator; the screen shows the stop) */
    uint32_t seen, ph[NOSC], rnd;
    int32_t amp;                                /* the gain now, Q15: the envelope times the velocity, ramped */
    int32_t nlp;                                /* the noise through its colour filter */
    uint32_t age;                               /* main loop's */
    uint8_t note;
} voice[NV];
static volatile uint8_t glide_gls;               /* (main loop writes) GLISS: the glide in semitone steps */
#define LEG_ALL 0x80u                           /* (voice.leg: legato, every envelope goes on) */
static volatile uint8_t lfo_fout;               /* (main loop writes) the LFOs fading out after the note (a bit each:
                                                   DELAY its time; 2026-11-16, the Blofeld's negative FADE) */
static volatile uint8_t osc_lim;                /* (main loop writes) LIMIT WT: the oscillators reading whole waves
                                                   only (a bit each; vfm_run) */
static int32_t bend_pos;                        /* the bend, -8192..8191 (MIDI's or the SMK's wheel, the last moved) */
static struct {                                 /* an oscillator's settings as the ISR reads them (main loop writes) */
    volatile uint32_t target;                   /* table position, Q16 */
    volatile uint32_t mul;                      /* pitch: the voice's step times this / 2^20 (engine/pitch.h) */
    volatile uint32_t mulb;                     /* mul with the pitch bend in it, its own range (2026-11-16: the main
                                                   loop's, every pass): what the ISR reads */
    const int16_t *volatile tab;                /* its wavetable (wt_table): level 0's rows, then the mip-maps */
    volatile uint8_t va;                        /* 1: the ANALOG table (va_osc.h), tab unread */
    uint32_t now;                               /* the position sounding now, Q16 (the ISR's own) */
} osc[NOSC];
enum { SRC_O1, SRC_O2, SRC_O3, SRC_NOISE, SRC_RING, NSRC };   /* what the mixer mixes */
/* FM (2026-11-15, as the Blofeld's FM Source and FM Amount, done as a DX's phase modulation): an oscillator reads its
 * table at its phase plus its modulator's last sample (an oscillator's raw output, itself too: feedback; or the
 * noise) times k(AMT), k = AMT^2 x 181 / 16: at 127 a full-scale sine moves the phase +-8 radians. The pitch stays
 * put at any depth. The modulator's sample is the one before (each oscillator one sample late: the order does not
 * matter). The bands FM adds reach up to the carrier + (I + 1) x the modulator (Carson): the carrier's mip-map level
 * follows that (fm_prep), so a deep FM reads a darker level instead of folding back */
static volatile uint8_t fm_src[NOSC], fm_amt[NOSC], fm_on;   /* (main loop writes): FMS_*, 0..127; some source set */
/* hard sync (2026-11-15, as the Blofeld's Osc 2 Sync): OSC2 starts its cycle again each time OSC3's does, at the share
 * of a sample past OSC3's wrap (a counter of OSC3's pitch of its own: OSC3's copies do not matter). The jump that makes
 * is smoothed by a polyBLEP over the samples either side: OSC2's output comes a sample late under sync. OSC2's unison
 * copies all start again too (no smoothing: they alias). Done in vfm_run with FM: nothing paid without it */
static volatile uint8_t osc_sync;               /* (main loop writes) OSC2 synced to OSC3 */
/* the filters' DRIVE curves (2026-11-16, the Blofeld's): SOFT the cubic of before (svf_drive, the default: the
 * patches as they were), the rest in vfm_run's loop (drive_curve) */
enum { DC_SOFT, DC_CLIP, DC_TUBE, DC_HARD, DC_MEDIUM, DC_PICK1, DC_PICK2, DC_RECT, DC_SQUARE, DC_BINARY, DC_OVER,
       DC_SINE, DC_OSC1, DC_N };
static volatile uint8_t flt_curve[2];           /* (main loop writes) DC_* */
static volatile uint8_t vx_on;                  /* (main loop writes) a voice needs vfm_run all the time: sync, a comb,
                                                   a DRIVE curve, Filter FM, a filter's PAN (FM: by its depth, a
                                                   sub-block at a time) */
/* Filter FM and the filters' PAN (2026-11-16, the Blofeld's): a filter's cutoff moved a sample at a time by an FM
 * source's last sample (FMS_*: OSC 1..3, the noise; as the oscillators' FM) times k(AMT), up to +-4 octaves at 127
 * (AMT squared), the coefficients worked out again each sample (svf_retune; a comb: its delay); a filter's output
 * across the stereo field (PAR: each its own; SER: filter 2's), the sum of the sides kept, as the voice's PAN */
static volatile uint8_t flt_fms[2], flt_fma[2]; /* (main loop writes) FMS_*, 0..127 */
static volatile int8_t flt_pan[2];              /* (main loop writes) -63 all left .. 63 all right */
static int16_t comb_buf[NV][2][2][COMB_N];      /* the combs' lines (COMB+, COMB-, engine/svf.h): a voice, a side (left,
                                                   right: unison SPREAD), a filter; the output / 4 (+-SVF_CLIP in 16
                                                   bits). 32 KB: only RAM, the pool is full */
static volatile int32_t mix_g[2][NSRC];         /* each source's gain into filter 1's input and filter 2's (BAL) */
static volatile uint8_t mix_split;              /* some source goes to filter 2: the ISR makes its own mix */
static volatile int32_t noise_k = 32768;        /* the noise colour: a one-pole low-pass, Q15 (32768 = white) */
static volatile int32_t vol_gain = 100;         /* the patch's volume, 0..127 (128 = unity) */
static struct {                                 /* a filter's settings as the ISR reads them (main loop writes) */
    volatile int32_t base;                      /* the cutoff, Q8 notes (69 << 8 = 440 Hz) */
    volatile int32_t env;                       /* the envelope's reach at full level, Q8 semitones */
    volatile int32_t ktrk;                      /* a note above C4 moves the cutoff this much, Q8 */
    volatile int32_t velo;                      /* x velocity >> 10: the cutoff's move, Q8 */
    volatile int32_t drive;                     /* the drive's gain x 256 (0: none) */
    volatile uint8_t type, res;                 /* SVF_OFF .. SVF_HP24; the resonance 0..127 */
} flt[2];
static volatile uint8_t flt_par;                /* 0: filter 1 into filter 2; 1: side by side, mixed */
static env_rates_t amp_env, flt_env, xenv_r[2];   /* the envelopes' rates (main loop writes): AMP, FLT, ENV 3, 4 */
static volatile int32_t vsens[4];               /* VELO of AMP FLT 3 4, Q15 (32768 = 63: all of it), < 0: inverted */
static volatile uint32_t mod_envs;              /* ENV 3, ENV 4 read by a slot (a bit each) */
static volatile uint8_t env_par[4][4];          /* each envelope's A D S R (main loop writes): what the matrix moves */
/* an envelope's rates with the matrix's move of its A D S R (m): r itself when there is none, else tmp */
static inline const env_rates_t *env_modded(env_rates_t *tmp, const env_rates_t *r, uint32_t e, const int32_t *m)
{
    int32_t x[4], i;
    if (!(m[0] | m[1] | m[2] | m[3])) return r;
    *tmp = *r;
    for (i = 0; i < 4; i++) {
        x[i] = (int32_t)env_par[e][i] + m[i];
        x[i] = x[i] < 0 ? 0 : x[i] > 127 ? 127 : x[i];
    }
    tmp->atk = ENV_ATK[x[0]];
    tmp->dec = ENV_COEF[x[1]];
    tmp->sus = env_level((uint32_t)x[2]);
    tmp->rel = ENV_COEF[x[3]];
    return tmp;
}
static volatile struct { uint8_t src, dst, idx; int8_t amt; int32_t scale; } mslot[NMSLOT];   /* the matrix's slots in use, in a row
                                                   (main loop writes; mod_scale) */
static volatile uint32_t mod_n, mod_lfos;       /* how many; which LFOs they read (a bit each) */
static volatile uint32_t mod_xs, mod_lspd, mod_amts;   /* 2026-11-15: some slot reads a newer source (from MS_PRESS:
                                                   mod_more); the LFOs whose speed a slot moves; slots 1..4 whose
                                                   amount one moves (a bit each) */
static volatile uint8_t modf_a[NMODF], modf_b[NMODF], modf_op[NMODF];   /* the modifiers (main loop writes) */
static uint8_t mod_fx;                          /* (main loop's) a slot moves a filter's PAN: the voices in vfm_run */
static volatile int32_t modf_c[NMODF];          /* their constants, Q15 */
static volatile int32_t mod_press, mod_bend, mod_sust, mod_breath, mod_foot, mod_ctl[4];   /* the MIDI sources, Q15
                                                   (main loop writes): aftertouch, the bend, CC 64, 2, 4, CTL W..Z */
static volatile uint32_t lfo_incs[NLFO], lfo_fades[NLFO];   /* the LFOs: phase steps; DELAY's fade-in steps (Q30) */
static volatile uint8_t lfo_shapes[NLFO], lfo_keys[NLFO], lfo_phases[NLFO];
static lfo_t glfo[NLFO];                        /* the shared LFOs (KEY off; the ISR's own) */
static volatile int32_t mod_macro, mod_wheel;   /* sources MACRO (ALGORITHM's MORPH), WHEEL (CC 1), Q15 */
static volatile int32_t mix_lv[NSRC], mix_w2[NSRC], noise_comp = 64;   /* for level modulation: the levels, BAL's share
                                                   to filter 2 (0..128), the noise's COLOR gain (Q6) */
static volatile uint8_t noise_col = 127, flt_drv[2];   /* COLOR, the filters' DRIVE as set (the matrix moves them) */
static uint16_t noise_comp_tab[128];            /* COLOR's loudness gain for each COLOR, as noise_comp (main loop) */
static int32_t noise_kc(int32_t c)              /* COLOR 0..127 -> the noise's one-pole coefficient, Q15 */
{
    return c >= 127 ? 32768 : 400 + (c + 1) * (c + 1) * 2;
}
/* unison, type B (stage 7a): an oscillator plays n copies of itself (1..UMAX), each its own phase, detuned (uni_mul)
 * and panned (uni_gl, uni_gr: their gains to the left and the right, the 1/sqrt(n) loudness in them); the filters and
 * the envelopes stay one a voice. Panned copies (SPREAD) make the voice stereo: its filters run twice, left and right */
static volatile uint32_t uni_mul[NOSC][UMAX];   /* a copy's step: the oscillator's times this / 2^20 */
static volatile int32_t uni_gl[NOSC][UMAX], uni_gr[NOSC][UMAX];   /* Q12 (4096 = 1) */
static volatile uint8_t uni_n[NOSC];            /* copies (1: plain); written after the arrays */
static volatile uint8_t uni_on, uni_st;         /* some oscillator has copies; some has them panned (stereo) */
/* the effects (stage 7b, engine/fx.h): two slots after the voices, each its own memory in the pool */
static int16_t fx_mem[2][FX_MEM] __attribute__((section(".pool")));
static fx_par_t fxp[2];                         /* the slots' settings (the main loop writes; fx_apply) */
static fx_state_t fxs[2];                       /* the ISR's (the main loop clears it while the slot is off) */
/* the EQ (engine/eq.h), the third slot after them: the main loop writes a set of coefficients into eq_box, eq_seq odd
 * while it does; at a block's start the ISR copies a set whose even count it has not seen yet. The ISR interrupts the
 * main loop, never the other way round: coming in the middle of a write, it finds the count odd and keeps its old set
 * for that block (it never takes half of one) */
static volatile int32_t eq_box[EQ_WORDS];
static volatile uint32_t eq_seq;
static eq_coef_t eqc;                           /* the ISR's own copy */
static uint32_t eq_seen;                        /* (the ISR's) */
static eq_state_t eqs;                          /* (the ISR's) */
static volatile int32_t out_peak;              /* the ISR's: the loudest sample of the last block (silence: < 8) */
static volatile uint32_t cpu_ticks;             /* the longest app_audio since the last reset (the ISR writes) */
static volatile uint32_t cpu_part[2];           /* that block's split: the voices (with the block's setup), the effects'
                                                   slots; the rest (EQ, limiter) is what is left (2026-11-16) */
static volatile uint32_t cpu_sum, cpu_cnt;      /* every block since the reset: the ticks added, how many (the mean) */
static volatile uint32_t cpu_reset;             /* +1: start over (the main loop writes) */
static volatile uint32_t cpu_nv;                /* the voices the longest block played (engine/poly.h) */
static volatile uint8_t cpu_over;               /* the ISR sets it: a block past POLY_OVER with every voice playing */
static uint8_t poly_n = NV;                     /* (main loop's) the voices the sound may use now: NV, or NV - 1 when
                                                   it is heavy (engine/poly.h) */

/* the velocity's gain, Q15: vp the velocity (Q15, 127 -> 32766), s its sensitivity (vsens). s = 0: always 1;
 * s = 1: vp itself (a soft note quiet); s < 0: the other way round (a soft note loud, a hard one quiet) */
static inline int32_t vel_gain(int32_t vp, int32_t s)
{
    if (s < 0) { vp = 32767 - vp; s = -s; }
    return 32768 - ((s * (32768 - vp)) >> 15);
}
#define MIX_CLIP(x) ((x) > SVF_CLIP ? SVF_CLIP : (x) < -SVF_CLIP ? -SVF_CLIP : (x))   /* (the dark noise's rare peaks) */
#define MAXSUB 64u                              /* sub-blocks of ENV_SUB in an audio block, at most */
/* the ANALOG table's sample (out of line: a table's oscillator pays one test). The sub-block marks it by a level
 * shift sh of 0 (a table's is 25 and more) and puts va_inv's inverse step where a table's stride goes */
static __attribute__((noinline)) int32_t va_sample(uint32_t t, uint32_t dt, uint32_t inv, uint32_t pos)
{
    return va_core(t, dt, inv, pos);
}
static inline int32_t osc_sample(const int16_t *d, uint32_t sh, uint32_t st, uint32_t ph, uint32_t dt, uint32_t pos)
{
    return sh ? wt_sample_lv(d, sh, st, ph, pos) : va_sample(ph, dt, st, pos);
}
/* the ANALOG table's unison copies, as uni_osc's (out of line); inv: the note's inverse step */
static __attribute__((noinline)) int32_t uni_va(uint32_t inv, uint32_t *ph, const uint32_t *us, const int32_t *gl,
                                                const int32_t *gr, uint32_t n, uint32_t pos, int32_t *r, uint32_t off)
{
    int32_t sl = 0, sr = 0, x;
    uint32_t c;
    for (c = 0; c < n; c++) {
        ph[c] += us[c];
        x = va_core(ph[c] + off, us[c], inv, pos);   /* (|x| <= 1.125 x 30000: the sums as below, under 2^31; off:
                                                   FM's phase offset) */
        sl += x * gl[c];
        sr += x * gr[c];
    }
    *r = sr >> 12;
    return sl >> 12;
}
/* an oscillator's n unison copies: each phase on by its step, the samples summed through their gains, left (the
 * return) and right (*r); Q12 gains <= 2 x 4096 / sqrt(n): |sum| < 7 x 30000 x 8192, under 2^31 */
static inline int32_t uni_osc(const int16_t *tb, uint32_t sh, uint32_t stw, uint32_t *ph, const uint32_t *us,
                              const int32_t *gl, const int32_t *gr, uint32_t n, uint32_t pos, int32_t *r, uint32_t off)
{
    int32_t sl = 0, sr = 0, x;
    uint32_t c;
    if (!sh) return uni_va(stw, ph, us, gl, gr, n, pos, r, off);   /* (ANALOG: marked as osc_sample says) */
    for (c = 0; c < n; c++) {
        ph[c] += us[c];
        x = wt_sample_lv(tb, sh, stw, ph[c] + off, pos);   /* (off: FM's phase offset, all copies alike) */
        sl += x * gl[c];
        sr += x * gr[c];
    }
    *r = sr >> 12;
    return sl >> 12;
}
/* ---- a voice's sub-block in passes (2026-11-16): the oscillators into buffers, then the mix, each filter, the gain.
 * The per-sample loop of before held some 30 values at once and kept most of them on the stack (tools/isr_loops.py:
 * 205 stack accesses in its 637 instructions), and called the filters out of line twice a sample; on the FM-1 the
 * voices of 11 SUPERSAW took 77 % of the block. Each pass below is a short loop over at most ENV_SUB samples with a
 * few values live, out of line (a call a pass a sub-block, not a sample). The arithmetic is the old loop's, sample
 * for sample, to the bit: the copies are summed in the same order (copy 0 first), a source with no gain adds 0. */

/* one table oscillator over n samples: its phase *ph on by st a sample, its table position pos on by dp (both
 * before the sample is read, as the old loop did). What becomes of a sample x (mode, a constant: each user its own
 * copy of the loops): OM_SET o[i] = x; OM_L o[i] += x g (a unison copy, summed in place: no buffer, no second pass);
 * OM_LR also o2[i] += x h (the right side). Not heard: not called, its phases moved on by the caller */
enum { OM_SET, OM_L, OM_LR };
static inline __attribute__((always_inline)) void osc_run(int32_t *o, int32_t *o2, int32_t g, int32_t h, uint32_t n,
                                                          const int16_t *d, uint32_t sh, uint32_t stw, uint32_t *ph,
                                                          uint32_t st, uint32_t pos, int32_t dp, const int mode)
{
    uint32_t i, p = *ph;
#define OSC_PUT(x) do {                                                     \
        int32_t x_ = (x);                                                   \
        if (mode == OM_SET) o[i] = x_;                                      \
        else { o[i] += x_ * g; if (mode == OM_LR) o2[i] += x_ * h; }        \
    } while (0)
    if (!sh) {                                  /* ANALOG (osc_sample's mark): stw is the inverse step */
        for (i = 0; i < n; i++) { p += st; pos = (uint32_t)((int32_t)pos + dp); OSC_PUT(va_core(p, st, stw, pos)); }
    } else if (!dp) {                           /* the position still: the two rows found once */
        uint32_t w = pos >> 16, w1 = w + 1u < WT_WAVES ? w + 1u : w;
        int32_t wf = (int32_t)((pos >> 1) & 0x7FFFu);
        const int16_t *ra = d + w * stw, *rb = d + w1 * stw;
        if (w1 == w || !wf)                     /* one row (the last wave, or right on one): b - a is 0 x wf */
            for (i = 0; i < n; i++) {
                uint32_t k;
                int32_t f;
                p += st;
                k = p >> sh;
                f = (int32_t)((p >> (sh - 15u)) & 0x7FFFu);
                OSC_PUT(ra[k] + (((ra[k + 1u] - ra[k]) * f) >> 15));
            }
        else
            for (i = 0; i < n; i++) {
                uint32_t k;
                int32_t f, a, b;
                p += st;
                k = p >> sh;
                f = (int32_t)((p >> (sh - 15u)) & 0x7FFFu);
                a = ra[k] + (((ra[k + 1u] - ra[k]) * f) >> 15);
                b = rb[k] + (((rb[k + 1u] - rb[k]) * f) >> 15);
                OSC_PUT(a + (((b - a) * wf) >> 15));
            }
    } else {
        for (i = 0; i < n; i++) { p += st; pos = (uint32_t)((int32_t)pos + dp); OSC_PUT(wt_sample_lv(d, sh, stw, p, pos)); }
    }
#undef OSC_PUT
    *ph = p;
}
static __attribute__((noinline)) void blk_osc(int32_t *o, uint32_t n, const int16_t *d, uint32_t sh, uint32_t stw,
                                              uint32_t *ph, uint32_t st, uint32_t pos, int32_t dp)
{
    osc_run(o, 0, 0, 0, n, d, sh, stw, ph, st, pos, dp, OM_SET);
}
static __attribute__((noinline)) void blk_copy_l(int32_t *ol, int32_t g, uint32_t n, const int16_t *d, uint32_t sh,
                                                 uint32_t stw, uint32_t *ph, uint32_t st, uint32_t pos, int32_t dp)
{
    osc_run(ol, 0, g, 0, n, d, sh, stw, ph, st, pos, dp, OM_L);
}
static __attribute__((noinline)) void blk_copy_lr(int32_t *ol, int32_t *or_, int32_t g, int32_t h, uint32_t n,
                                                  const int16_t *d, uint32_t sh, uint32_t stw, uint32_t *ph, uint32_t st,
                                                  uint32_t pos, int32_t dp)
{
    osc_run(ol, or_, g, h, n, d, sh, stw, ph, st, pos, dp, OM_LR);
}
/* an oscillator's nc unison copies over n samples (uni_osc's, the loops turned round: a copy at a time), summed
 * through their gains into ol[] (left) and, with or (not 0), or[] (right); the sums >> 12 as uni_osc's. (ANALOG:
 * va_core(ph + 0, us, inv, pos), as uni_va) */
static __attribute__((noinline)) void blk_uni(int32_t *ol, int32_t *or_, uint32_t n, const int16_t *d, uint32_t sh,
                                              uint32_t stw, uint32_t *ph, const uint32_t *us, const int32_t *gl,
                                              const int32_t *gr, uint32_t nc, uint32_t pos, int32_t dp)
{
    uint32_t c, i;
    for (i = 0; i < n; i++) ol[i] = 0;
    if (or_) {
        for (i = 0; i < n; i++) or_[i] = 0;
        for (c = 0; c < nc; c++) blk_copy_lr(ol, or_, gl[c], gr[c], n, d, sh, stw, &ph[c], us[c], pos, dp);
        for (i = 0; i < n; i++) or_[i] >>= 12;
    } else
        for (c = 0; c < nc; c++) blk_copy_l(ol, gl[c], n, d, sh, stw, &ph[c], us[c], pos, dp);
    for (i = 0; i < n; i++) ol[i] >>= 12;
}
/* the mix into a filter over n samples: s[] = MIX_CLIP((o0 g0 + o1 g1 + o2 g2 + noise g3 + ring g4) >> 8); the ring
 * rg[] made by the caller (only when g4 is not 0). A source at gain 0 adds 0: skipped */
static __attribute__((noinline)) void blk_mix(int32_t *s, uint32_t n, const int32_t *const *src, const int32_t *g)
{
    uint32_t i, k;
    for (i = 0; i < n; i++) s[i] = 0;
    for (k = 0; k < NSRC; k++) {
        const int32_t *x = src[k];
        int32_t gk = g[k];
        if (gk) for (i = 0; i < n; i++) s[i] += x[i] * gk;
    }
    for (i = 0; i < n; i++) s[i] = MIX_CLIP(s[i] >> 8);
}
/* the ring over n samples: half: the unison loop's ((a >> 1) (b >> 1)) >> 12; else the plain loop's (a b) >> 14 */
static __attribute__((noinline)) void blk_ring(int32_t *rg, uint32_t n, const int32_t *a, const int32_t *b, uint32_t half)
{
    uint32_t i;
    if (half) for (i = 0; i < n; i++) rg[i] = ((a[i] >> 1) * (b[i] >> 1)) >> 12;
    else for (i = 0; i < n; i++) rg[i] = (a[i] * b[i]) >> 14;
}
/* one filter over n samples, in place: x[] = svf_tick(s, c, drive ? svf_drive(x, drive) : x). A 12 dB type, not
 * driven (DRIVE on sets nl: never here): the state in registers; LP's output is its low pass alone (cx cb 0: their
 * products add 0), the others' all three terms */
static __attribute__((noinline)) void blk_svf(int32_t *x, uint32_t n, svf_state_t *s, const svf_coef_t *c, int32_t drv)
{
    uint32_t i;
    if (!c->p4 && !c->nl && !drv) {
        int32_t ic1 = s->ic1, ic2 = s->ic2, a1 = c->a1, a2 = c->a2, g = c->g, cx = c->cx, cl = c->cl, cb = c->cb;
        if (!cx && !cb)
            for (i = 0; i < n; i++) {
                int32_t v0 = x[i] * (1 << SVF_SH), v1, v2, y;
                v1 = (int32_t)(((int64_t)a1 * ic1 + (int64_t)a2 * (v0 - ic2)) >> 30);
                v2 = ic2 + (int32_t)(((int64_t)g * v1) >> 28);
                ic1 = 2 * v1 - ic1;
                ic2 = 2 * v2 - ic2;
                y = (int32_t)(((int64_t)cl * v2) >> (14 + SVF_SH));
                x[i] = y > SVF_CLIP ? SVF_CLIP : y < -SVF_CLIP ? -SVF_CLIP : y;
            }
        else
            for (i = 0; i < n; i++) {
                int32_t v0 = x[i] * (1 << SVF_SH), v1, v2, y;
                v1 = (int32_t)(((int64_t)a1 * ic1 + (int64_t)a2 * (v0 - ic2)) >> 30);
                v2 = ic2 + (int32_t)(((int64_t)g * v1) >> 28);
                ic1 = 2 * v1 - ic1;
                ic2 = 2 * v2 - ic2;
                y = (int32_t)(((int64_t)cx * v0 + (int64_t)cl * v2 + (int64_t)cb * v1) >> (14 + SVF_SH));
                x[i] = y > SVF_CLIP ? SVF_CLIP : y < -SVF_CLIP ? -SVF_CLIP : y;
            }
        s->ic1 = ic1; s->ic2 = ic2;
    } else
        for (i = 0; i < n; i++) x[i] = svf_tick(s, c, drv ? svf_drive(x[i], drv) : x[i]);
}
/* the voice's filters over n samples (the old per-sample voice_filt's, a pass at a time): y[] from s[] (into filter
 * 1) and s2[] (BAL's share for filter 2; split 0: none, as 0), SER or PAR; y[] clamped at the end. s2[] is used up */
static __attribute__((noinline)) void blk_filt(int32_t *y, const int32_t *s, int32_t *s2, uint32_t n, svf_state_t *a,
                                               svf_state_t *b, const svf_coef_t *cf, const uint32_t *ft,
                                               const int32_t *fdr, uint32_t par, uint32_t split)
{
    uint32_t i;
    for (i = 0; i < n; i++) y[i] = s[i];
    if (ft[0]) blk_svf(y, n, a, &cf[0], fdr[0]);
    if (par) {                                  /* PAR: filter 2 on its own share, the two added */
        if (split) {
            if (ft[1]) blk_svf(s2, n, b, &cf[1], fdr[1]);
            for (i = 0; i < n; i++) y[i] += s2[i];
        }
    } else {                                    /* SER: filter 1's output and the share that skips it, into 2 */
        if (split) for (i = 0; i < n; i++) y[i] = MIX_CLIP(y[i] + s2[i]);
        else for (i = 0; i < n; i++) y[i] = MIX_CLIP(y[i]);
        if (ft[1]) blk_svf(y, n, b, &cf[1], fdr[1]);
    }
    for (i = 0; i < n; i++) y[i] = MIX_CLIP(y[i]);
}
/* the gain (its ramp from *amp by da a sample, before the sample, as before), PAN, into the block's out[] */
static __attribute__((noinline)) void blk_out(int32_t *out, const int32_t *yl, const int32_t *yr, uint32_t n,
                                              int32_t amp, int32_t da, uint32_t panon, int32_t pl, int32_t pr)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t l, r;
        amp += da;
        l = (yl[i] * amp) >> 15; r = (yr[i] * amp) >> 15;
        if (panon) { l = (l * pl) >> 14; r = (r * pr) >> 14; }
        out[2u * i] += l;
        out[2u * i + 1u] += r;
    }
}
/* FM a sub-block (out of app_audio): each oscillator's k from AMT (+ the matrix's FM1..3, dm 0: none) and, where it
 * modulates, the step its mip-map level is picked by: the carrier's + (I + 1) x the modulator's (the noise: the
 * carrier's own), I = 8 x (AMT / 127)^2 */
static __attribute__((noinline)) void fm_prep(int32_t *fk, uint32_t *ftop, const uint32_t *sv, const int32_t *dm,
                                             uint32_t *fs)
{
    uint32_t k;
    for (k = 0; k < NOSC; k++) {
        uint32_t s = fm_src[k] < FMS_N ? fm_src[k] : FMS_OFF, sm;
        fs[k] = s;                              /* (the voice's: the loops read its sample through it) */
        int32_t a = (int32_t)fm_amt[k] + (dm ? dm[MD_FM1 + k] : 0);
        uint64_t t;
        a = a < 0 || !s || s >= FMS_N ? 0 : a > 127 ? 127 : a;
        fk[k] = (a * a * 181) >> 4;             /* (<= 182 459: x 32767 wraps in 32 bits, as a phase does) */
        sm = s >= FMS_O1 && s <= FMS_O3 ? sv[s - FMS_O1] : sv[k];
        t = (uint64_t)sv[k] + (((uint64_t)sm * (uint32_t)(256 + ((a * a * 130) >> 10))) >> 8);   /* ((I + 1) Q8) */
        ftop[k] = !a ? 0u : t > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)t;
    }
    if (osc_sync && sv[1] < 0x80000000u && ftop[1] < 2u * sv[1]) ftop[1] = 2u * sv[1];   /* (sync: cutting OSC2's
                                                   cycle spreads its top harmonics past fs / 2: half its bandwidth) */
}
/* a voice's sub-block under FM: app_audio's unison loop (it does one copy too, and stereo), each oscillator read at
 * its phase + its FM offset, the sources kept for the next sample; out of app_audio, so that its loops cost nothing
 * more without FM. What the loop moves comes back in c */
typedef struct {
    uint32_t i0, i1;                            /* the samples: i0 .. i1 - 1 */
    int32_t amp, da, nlp, nkv;
    uint32_t ph[NOSC], pos[NOSC], rnd;
    const int32_t *dpos, *gv1, *gv2, *fdv, *fk;
    const int16_t *const *ld;
    const uint32_t *lsh, *lst, *sv, *un, *ft, *fs;
    uint32_t (*us)[UMAX], (*uph)[UMAX];
    int32_t (*ugl)[UMAX], (*ugr)[UMAX];
    uint32_t vsplit, ust, par, panon;
    int32_t pl, pr;
    svf_state_t sf0, sf1, sr0, sr1;
    const svf_coef_t *cf;
    int32_t *fv, *out;
    uint32_t sync, mph;                         /* sync: on; the master's phase (in, out) */
    int16_t (*cbuf)[2][COMB_N];                 /* the combs' lines of the voice ([side][filter]) */
    uint32_t dcv[2];                            /* the filters' DRIVE curves (DC_*) */
    const int32_t *dm;                          /* the matrix's moves this sub-block (0: none): F1.FM .. F2.PAN */
    const volatile int32_t *fn;                 /* the filters' cutoffs this sub-block (the voice's fn), Q8 notes */
    int32_t (*dcb)[2][2];                       /* their DC blockers ([side][filter][x, y], the voice's) */
    uint32_t cw;                                /* their write position (in, out) */
    int32_t s1d;                                /* sync: OSC2's sample held back (in, out) */
} vfm_t;
static uint32_t sync_t(uint32_t ph, uint32_t st)   /* ph / st, Q15 (ph < st: the share of a sample past the wrap) */
{
    uint32_t s = 0, d;
    while ((st >> s) > 0xFFFFu) s++;            /* (both under 2^16: a 32-bit divide) */
    d = st >> s;
    s = d ? ((ph >> s) << 15) / d : 0u;
    return s > 32767u ? 32767u : s;
}
/* a comb's sample (its coefficients: svf_comb_coef): the line read D back (linear between two samples), y = g x +- fb
 * that, written at w */
static inline int32_t comb_tick(int16_t *buf, uint32_t w, const svf_coef_t *c, int32_t x)
{
    uint32_t d = (uint32_t)c->cx, i = d >> 16;
    int32_t d0 = buf[(w - i) & (COMB_N - 1u)], d1 = buf[(w - i - 1u) & (COMB_N - 1u)];
    int32_t dl = (d0 + (((d1 - d0) * (int32_t)((d & 0xFFFFu) >> 1)) >> 15)) * 4;
    int32_t y = ((x * c->cb) >> 15) + c->a1 * ((dl * c->cl) >> 15);   /* (|x|, |dl| <= 2^17, Q15: under 2^31) */
    y = y > SVF_CLIP ? SVF_CLIP : y < -SVF_CLIP ? -SVF_CLIP : y;
    buf[w & (COMB_N - 1u)] = (int16_t)(y >> 2);
    return y;
}
/* a DRIVE curve (not SOFT: svf_drive's): x within +-SVF_CLIP, g8 the gain (Q8, 256 .. 2048), om OSC1's sample (OSC1
 * MOD); the result within about +-32768, as svf_drive's. t = x g8 / 256, Q15 (32768 = full scale):
 *   CLIP     hard at full scale;               TUBE     tanh(t + 0.3) - tanh(0.3) (DIST's: even harmonics);
 *   HARD     tanh(2 t);                         MEDIUM   tanh(1.4 t);
 *   PICKUP1  tanh(t + t^2 / 4);                 PICKUP2  tanh(t - t^2 / 2 + t^3 / 4): lopsided both;
 *   RECTIFY  |t| (an octave up);                SQUARE   8 t clipped: near a square;
 *   BINARY   t to 3 bits and the sign;          OVERFLOW t wrapping round 16 bits (a digital fold);
 *   SINE     sin(pi / 2 t), on and on: a sine folder;   OSC1 MOD  t (clipped) times OSC1: a ring */
static int32_t drive_curve(int32_t x, int32_t g8, uint32_t cv, int32_t om)
{
    int32_t t = (x * g8) >> 8, u;               /* (|x| <= 2^17, g8 <= 2^11: under 2^28) */
    if (t > 4 * 32768) t = 4 * 32768;
    if (t < -4 * 32768) t = -4 * 32768;
    switch (cv) {
    case DC_CLIP: return t > 32767 ? 32767 : t < -32767 ? -32767 : t;
    case DC_TUBE: return dist_shape(t, DS_TUBE);
    case DC_HARD: return dist_tanh(t * 2 > 131072 ? 131072 : t * 2 < -131072 ? -131072 : t * 2);
    case DC_MEDIUM: u = t + ((t * 13) >> 5); return dist_tanh(u > 131072 ? 131072 : u < -131072 ? -131072 : u);   /* (x 1.4) */
    case DC_PICK1: u = t > 65536 ? 65536 : t < -65536 ? -65536 : t; return dist_tanh(u + ((u * u) >> 17));
    case DC_PICK2:
        u = t > 65536 ? 65536 : t < -65536 ? -65536 : t;
        return dist_tanh(u - ((u * u) >> 16) + (int32_t)((((int64_t)u * u) >> 15) * u >> 17));
    case DC_RECT: u = t < 0 ? -t : t; return u > 32767 ? 32767 : u;
    case DC_SQUARE: u = t * 8; return u > 32767 ? 32767 : u < -32767 ? -32767 : u;
    case DC_BINARY: u = t > 32767 ? 32767 : t < -32767 ? -32767 : t; return u >= 0 ? u & ~0x1FFF : -((-u) & ~0x1FFF);
    case DC_OVER: return (int32_t)(int16_t)(uint16_t)t;
    case DC_SINE:                               /* the triangle of period 4 (DIST's FOLD), bent to a sine */
        u = dist_shape(t, DS_FOLD);
        return (int32_t)(((int64_t)u * (49152 - ((((int64_t)u * u) >> 15) >> 1))) >> 15);
    case DC_OSC1: u = t > 32767 ? 32767 : t < -32767 ? -32767 : t; return (u * (om > 32767 ? 32767 : om < -32767 ? -32767 : om)) >> 15;
    default: return svf_drive(x, g8);
    }
}
#define DC_LOPSIDED(c) ((c) == DC_TUBE || (c) == DC_PICK1 || (c) == DC_PICK2 || (c) == DC_RECT || (c) == DC_OSC1)
static inline int32_t filt_one(int32_t x, svf_state_t *st, const svf_coef_t *c, uint32_t t, int32_t dr, int16_t *buf,
                               uint32_t w, uint32_t cv, int32_t om, int32_t *db)
{
    if (dr) {
        x = cv ? drive_curve(x, dr, cv, om) : svf_drive(x, dr);
        if (DC_LOPSIDED(cv)) {                  /* a lopsided curve's DC (RECTIFY: 0.64 of full scale) would thump
                                                   with the envelope: a blocker, ~7 Hz (R 0.999, rounded) */
            int32_t y = x - db[0] + ((db[1] * 32735 + 16384) >> 15);
            db[0] = x;
            db[1] = y;
            x = y;
        }
    }
    return SVF_ISCOMB(t) ? comb_tick(buf, w, c, x) : svf_tick(st, c, x);
}
/* blk_filt's filters a sample at a time, with the combs and the DRIVE curves (vfm_run's), the two filters' outputs
 * apart (their PAN): PAR, filter 1's in the return, filter 2's in *y2 (0 without its share); SER, 0 and what comes out
 * of filter 2. Their sum, clipped, is blk_filt's, bit for bit */
static inline int32_t voice_filt_2(int32_t s, int32_t s2, svf_state_t *a, svf_state_t *b, const svf_coef_t *cf,
                                   const uint32_t *ft, const int32_t *fdr, uint32_t par, uint32_t split,
                                   int16_t (*cb)[COMB_N], uint32_t w, const uint32_t *cv, int32_t om, int32_t (*db)[2],
                                   int32_t *y2)
{
    int32_t y = ft[0] ? filt_one(s, a, &cf[0], ft[0], fdr[0], cb[0], w, cv[0], om, db[0]) : s;
    if (par) {
        *y2 = split ? (ft[1] ? filt_one(s2, b, &cf[1], ft[1], fdr[1], cb[1], w, cv[1], om, db[1]) : s2) : 0;
        return y;
    }
    {
        int32_t x2 = MIX_CLIP(y + s2);
        *y2 = ft[1] ? filt_one(x2, b, &cf[1], ft[1], fdr[1], cb[1], w, cv[1], om, db[1]) : x2;
    }
    return 0;
}
static __attribute__((noinline)) void comb_clear(uint32_t v)   /* a voice free: its lines silent (a comb rings long;
                                                   no comb set: nothing to do) */
{
    if (SVF_ISCOMB(flt[0].type) || SVF_ISCOMB(flt[1].type)) memset(comb_buf[v], 0, sizeof comb_buf[v]);
}
static __attribute__((noinline)) void vfm_run(vfm_t *c)
{
    const int32_t *dpos = c->dpos, *gv1 = c->gv1, *gv2 = c->gv2, *fdv = c->fdv, *fk = c->fk;
    const int16_t *const *ld = c->ld;
    const uint32_t *lsh = c->lsh, *lst = c->lst, *sv = c->sv, *un = c->un, *ft = c->ft, *fs = c->fs;
    const svf_coef_t *cf = c->cf;
    uint32_t i, ph0 = c->ph[0], ph1 = c->ph[1], ph2 = c->ph[2], pos0 = c->pos[0], pos1 = c->pos[1], pos2 = c->pos[2];
    uint32_t rnd = c->rnd, vsplit = c->vsplit, ust = c->ust, par = c->par, panon = c->panon;
    int32_t amp = c->amp, da = c->da, nlp = c->nlp, nkv = c->nkv, pl = c->pl, pr = c->pr, *fv = c->fv, *out = c->out;
    svf_state_t sf0 = c->sf0, sf1 = c->sf1, sr0 = c->sr0, sr1 = c->sr1;
    uint32_t sync = c->sync, mph = c->mph, cw = c->cw, f, ffs[2], pan = 0, lim = osc_lim;
    uint32_t lm0 = lim & 1u ? 0xFFFF0000u : 0xFFFFFFFFu, lm1 = lim & 2u ? 0xFFFF0000u : 0xFFFFFFFFu;
    uint32_t lm2 = lim & 4u ? 0xFFFF0000u : 0xFFFFFFFFu;   /* (LIMIT WT: the position's blend between waves cut) */
    int32_t s1d = c->s1d, ffk[2], fg[2][2];     /* Filter FM: each filter's k (0: none); PAN: its gains, [filter][L, R] */
    svf_coef_t cfl[2];                          /* (Filter FM: the coefficients retuned a sample at a time) */
    for (f = 0; f < 2u; f++) {                  /* the filters' FM and PAN, this sub-block (AMT, PAN + the matrix's) */
        int32_t a = (int32_t)flt_fma[f] + (c->dm ? c->dm[MD_FFM1 + f] : 0), p = flt_pan[f] * 127 / 63 + (c->dm ? c->dm[MD_FPAN1 + f] : 0);
        ffs[f] = flt_fms[f] < FMS_N ? flt_fms[f] : FMS_OFF;
        a = a < 0 || !ffs[f] || !ft[f] ? 0 : a > 127 ? 127 : a;
        ffk[f] = a * a * 12288 / 16129;         /* (Q8 semitones at full scale: 48 at 127) */
        p = p < -127 ? -127 : p > 127 ? 127 : p;
        p = p * 129 > 16384 ? 16384 : p * 129 < -16384 ? -16384 : p * 129;
        fg[f][0] = 16384 - p;
        fg[f][1] = 16384 + p;
        pan |= p != 0;
        cfl[f] = cf[f];
    }
    if (ffk[0] | ffk[1]) cf = cfl;
    for (i = c->i0; i < c->i1; i++) {
        int32_t o0, o1, o2, r0, r1, r2, rg, s, s2 = 0, y, yr, y2, yr2;
        for (f = 0; f < 2u; f++)                /* Filter FM: the cutoff where the source's last sample puts it */
            if (ffk[f]) svf_retune(&cfl[f], c->fn[f] + ((fv[ffs[f]] * ffk[f]) >> 15), ft[f]);
        uint32_t mp = mph + sv[2], t = 0, wrap = sync && mp < mph;   /* (sync: the master wrapped, t of a sample ago) */
        mph = mp;
        if (wrap) t = sync_t(mp, sv[2]);
        uint32_t f0 = (uint32_t)fv[fs[0]] * (uint32_t)fk[0], f1 = (uint32_t)fv[fs[1]] * (uint32_t)fk[1];
        uint32_t f2 = (uint32_t)fv[fs[2]] * (uint32_t)fk[2];   /* (the phase offsets: the sources' last samples, wrapping) */
        amp += da;
        pos0 = (uint32_t)((int32_t)pos0 + dpos[0]);
        pos1 = (uint32_t)((int32_t)pos1 + dpos[1]);
        pos2 = (uint32_t)((int32_t)pos2 + dpos[2]);
        rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
        nlp += ((((int32_t)rnd >> 17) - nlp) * nkv) >> 15;
        if (un[0] > 1u) o0 = uni_osc(ld[0], lsh[0], lst[0], c->uph[0], c->us[0], c->ugl[0], c->ugr[0], un[0], pos0 & lm0, &r0, f0);
        else { ph0 += sv[0]; o0 = r0 = osc_sample(ld[0], lsh[0], lst[0], ph0 + f0, sv[0], pos0 & lm0); }
        if (un[1] > 1u) {
            if (wrap) {                         /* (sync: the copies start again, no smoothing) */
                uint32_t k;
                for (k = 0; k < un[1]; k++) c->uph[1][k] = (uint32_t)(((uint64_t)c->us[1][k] * t) >> 15) - c->us[1][k];
            }
            o1 = uni_osc(ld[1], lsh[1], lst[1], c->uph[1], c->us[1], c->ugl[1], c->ugr[1], un[1], pos1 & lm1, &r1, f1);
        } else if (sync) {                      /* sync: the cycle starts again t ago; the jump there (the wave at 0
                                                   less where it was at that moment) smoothed: the sample before
                                                   + d t^2 / 2, this one - d (1 - t)^2 / 2; out a sample late */
            uint32_t pc = ph1 + sv[1];
            int32_t yn;
            if (wrap) {
                uint32_t back = (uint32_t)(((uint64_t)sv[1] * t) >> 15);   /* (t of a sample, in OSC2's phase) */
                int32_t d = osc_sample(ld[1], lsh[1], lst[1], f1, sv[1], pos1 & lm1)
                          - osc_sample(ld[1], lsh[1], lst[1], pc - back + f1, sv[1], pos1 & lm1), u = 32768 - (int32_t)t;
                ph1 = back;
                yn = osc_sample(ld[1], lsh[1], lst[1], ph1 + f1, sv[1], pos1 & lm1);
                s1d += (int32_t)(((int64_t)d * (((int32_t)t * (int32_t)t) >> 15)) >> 16);
                yn -= (int32_t)(((int64_t)d * ((u * u) >> 15)) >> 16);
            } else { ph1 = pc; yn = osc_sample(ld[1], lsh[1], lst[1], ph1 + f1, sv[1], pos1 & lm1); }
            o1 = r1 = s1d;
            s1d = yn;
        } else { ph1 += sv[1]; o1 = r1 = osc_sample(ld[1], lsh[1], lst[1], ph1 + f1, sv[1], pos1 & lm1); }
        if (un[2] > 1u) o2 = uni_osc(ld[2], lsh[2], lst[2], c->uph[2], c->us[2], c->ugl[2], c->ugr[2], un[2], pos2 & lm2, &r2, f2);
        else { ph2 += sv[2]; o2 = r2 = osc_sample(ld[2], lsh[2], lst[2], ph2 + f2, sv[2], pos2 & lm2); }
        fv[FMS_O1] = o0; fv[FMS_O2] = o1; fv[FMS_O3] = o2; fv[FMS_NOISE] = nlp;
        rg = ((o0 >> 1) * (o1 >> 1)) >> 12;     /* (as app_audio's unison loop from here) */
        s = MIX_CLIP((o0 * gv1[0] + o1 * gv1[1] + o2 * gv1[2] + nlp * gv1[3] + rg * gv1[4]) >> 8);
        if (vsplit) s2 = MIX_CLIP((o0 * gv2[0] + o1 * gv2[1] + o2 * gv2[2] + nlp * gv2[3] + rg * gv2[4]) >> 8);
        y = voice_filt_2(s, s2, &sf0, &sf1, cf, ft, fdv, par, vsplit, c->cbuf[0], cw, c->dcv, o0, c->dcb[0], &y2);
        if (ust) {
            rg = ((r0 >> 1) * (r1 >> 1)) >> 12;
            s = MIX_CLIP((r0 * gv1[0] + r1 * gv1[1] + r2 * gv1[2] + nlp * gv1[3] + rg * gv1[4]) >> 8);
            if (vsplit) s2 = MIX_CLIP((r0 * gv2[0] + r1 * gv2[1] + r2 * gv2[2] + nlp * gv2[3] + rg * gv2[4]) >> 8);
            yr = voice_filt_2(s, s2, &sr0, &sr1, cf, ft, fdv, par, vsplit, c->cbuf[1], cw, c->dcv, r0, c->dcb[1], &yr2);
        } else { yr = y; yr2 = y2; }
        cw++;
        if (pan) {                              /* the filters' PAN: each output its own gains (Q14 a side) */
            y = MIX_CLIP((((y >> 2) * fg[0][0]) >> 12) + (((y2 >> 2) * fg[1][0]) >> 12));
            yr = MIX_CLIP((((yr >> 2) * fg[0][1]) >> 12) + (((yr2 >> 2) * fg[1][1]) >> 12));
        } else { y = MIX_CLIP(y + y2); yr = MIX_CLIP(yr + yr2); }
        y = (y * amp) >> 15; yr = (yr * amp) >> 15;
        if (panon) { y = (y * pl) >> 14; yr = (yr * pr) >> 14; }
        out[2u * i] += y;
        out[2u * i + 1u] += yr;
    }
    c->ph[0] = ph0; c->ph[1] = ph1; c->ph[2] = ph2; c->pos[0] = pos0; c->pos[1] = pos1; c->pos[2] = pos2;
    c->rnd = rnd; c->nlp = nlp; c->sf0 = sf0; c->sf1 = sf1; c->sr0 = sr0; c->sr1 = sr1;
    c->mph = mph; c->s1d = s1d; c->cw = cw;
}
/* a note's envelopes (out of app_audio): each starts its attack, but one on SINGLE trigger (a bit of m, 2026-11-16,
 * the Blofeld's) that still sounds (not idle, not in its release): it goes on, as legato */
static __attribute__((noinline)) void env_trig(uint32_t v, uint32_t m)
{
    env_t *e[4] = {&voice[v].env, &voice[v].fenv, &voice[v].xenv[0], &voice[v].xenv[1]};
    uint32_t i;
    for (i = 0; i < 4u; i++)
        if (!(m >> i & 1u) || e[i]->st == ENV_IDLE || e[i]->st == ENV_RELEASE) env_on(e[i]);
}
/* the effects' slots 1 and 2 on the block (out of app_audio since 2026-11-16: its loops' size stays put as types
 * come; one call a block) */
static __attribute__((noinline)) void fx_slots(int32_t *out, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < 2u; k++) {
        uint32_t ty = fxp[k].type;
        if (ty == FX_DELAY)
            fx_delay(&fxs[k].dly, fx_mem[k], out, n, fxp[k].a, fxp[k].b, fxp[k].c, fxp[k].d, fxp[k].dry, fxp[k].wet);
        else if (ty == FX_REVERB)
            fx_reverb(&fxs[k].rev, fx_mem[k], out, n, fxp[k].a, fxp[k].b, fxp[k].c, fxp[k].d, fxp[k].dry, fxp[k].wet);
        else if (ty == FX_DIST)
            fx_dist(&fxs[k].dst, out, n, fxp[k].a, (uint32_t)fxp[k].b, fxp[k].c, fxp[k].d, fxp[k].dry, fxp[k].wet);
        else if (ty == FX_CHORUS)
            fx_chorus(&fxs[k].cho, fx_mem[k], out, n, fxp[k].a, fxp[k].b, fxp[k].c, (uint32_t)fxp[k].d,
                      (uint32_t)fxp[k].e, fxp[k].dry, fxp[k].wet);
        else if (ty == FX_PHASER)
            fx_phaser(&fxs[k].pha, out, n, fxp[k].a, fxp[k].b, fxp[k].c, (uint32_t)fxp[k].d, (uint32_t)fxp[k].e,
                      (uint32_t)fxp[k].f, fxp[k].dry, fxp[k].wet);
        else if (ty == FX_ENSEMBLE)
            fx_ensemble(&fxs[k].ens, fx_mem[k], out, n, fxp[k].a, fxp[k].b, (uint32_t)fxp[k].c, (uint32_t)fxp[k].d,
                        (uint32_t)fxp[k].e, fxp[k].f, fxp[k].dry, fxp[k].wet);
        else if (ty == FX_LOFI)
            fx_lofi(&fxs[k].lof, fx_mem[k], out, n, fxp[k].a, fxp[k].b, (uint32_t)fxp[k].c, (uint32_t)fxp[k].d, fxp[k].e,
                    fxp[k].f, fxp[k].dry, fxp[k].wet);
        else if (ty == FX_FILTER)
            fx_filter(&fxs[k].flt, out, n, fxp[k].a, fxp[k].b, (uint32_t)fxp[k].c, (uint32_t)fxp[k].d, (uint32_t)fxp[k].e,
                      (uint32_t)fxp[k].f, fxp[k].dry, fxp[k].wet);
        else if (ty == FX_TRIPLE)
            fx_triple(&fxs[k].tri, fx_mem[k], out, n, fxp[k].a, fxp[k].b, fxp[k].c, (uint32_t)fxp[k].d,
                      (uint32_t)fxp[k].e, fxp[k].f, fxp[k].dry, fxp[k].wet);
    }
}
/* the matrix's newer sources (2026-11-15), out of app_audio (its size: tools/isr_loops.py): the MIDI ones, the
 * products, the constants, then the modifiers in order (a later one: its value of the last step) */
static __attribute__((noinline)) void mod_more(int32_t *src, uint32_t v)
{
    uint32_t i;
    src[MS_PRESS] = mod_press;
    src[MS_PPRESS] = voice[v].ppress;
    src[MS_BEND] = mod_bend;
    src[MS_SUST] = mod_sust;
    src[MS_BREATH] = mod_breath;
    src[MS_FOOT] = mod_foot;
    for (i = 0; i < 4u; i++) src[MS_CTLW + i] = mod_ctl[i];
    src[MS_L1MW] = (src[MS_LFO1] * mod_wheel) >> 15;
    src[MS_L2PR] = (src[MS_LFO2] * mod_press) >> 15;
    src[MS_MIN] = -32767;
    src[MS_MAX] = 32767;
    for (i = 0; i < NMODF; i++) src[MS_MOD1 + i] = voice[v].mv[i];
    for (i = 0; i < NMODF; i++) {
        uint32_t a = modf_a[i], b = modf_b[i];
        int32_t r = modf_run(a < MS_N ? src[a] : 0, b == MS_OFF ? modf_c[i] : b < MS_N ? src[b] : 0, modf_op[i]);
        src[MS_MOD1 + i] = voice[v].mv[i] = r;
    }
}
/* the slots' scales with the amounts of slots 1..4 moved (AMT1..4 of the last step: +-127 = +-63 of the amount);
 * returns sc filled, or the plain scales when none moves */
static __attribute__((noinline)) const int32_t *mod_amt(int32_t *sc, const int32_t *plain, uint32_t n, uint32_t v)
{
    uint32_t k, moved = 0;
    for (k = 0; k < n; k++) {
        uint32_t i = mslot[k].idx;
        int32_t m = i < 4u ? voice[v].am[i] : 0;
        sc[k] = plain[k];
        if (m) { sc[k] = mod_scale(mslot[k].dst, (int32_t)mslot[k].amt + m / 2); moved = 1; }
    }
    return moved ? sc : plain;
}
static __attribute__((noinline)) void mod_keep(uint32_t v, const int32_t *dm)   /* L.SPD, AMT1..4 for the next step */
{
    uint32_t i;
    for (i = 0; i < NLFO; i++) voice[v].lsm[i] = dm[MD_LSPD1 + i];
    for (i = 0; i < 4u; i++) voice[v].am[i] = dm[MD_AMT1 + i];
}
static void app_audio(int32_t *out, uint32_t n)
{
    static int32_t glv[NLFO][MAXSUB];           /* the shared LFOs (KEY off), a value a sub-block */
    uint32_t v, i, j, k, f, l, p0[NOSC], mul[NOSC], ft[2], fres[2], par = flt_par, split = mix_split;
    uint32_t nmod = mod_n, lused = mod_lfos, eused = mod_envs, linc[NLFO], lshape[NLFO], lkey[NLFO];
    uint32_t xs = mod_xs, lsp = mod_lspd, amu = mod_amts, fmon = fm_on, spx = vx_on;
    const int16_t *tb0 = osc[0].tab, *tb1 = osc[1].tab, *tb2 = osc[2].tab;
    int32_t dp[NOSC], g1[NSRC], g2[NSRC], nk = noise_k, vg = vol_gain, fdr[2], fenv[2], vs0 = vsens[0], vs1 = vsens[1];
    int32_t mscale[NMSLOT], lfade[NLFO];
    uint8_t msrc[NMSLOT], mdst[NMSLOT];
    uint32_t uon = uni_on, ust = uni_st, un[NOSC], umul[NOSC][UMAX], us[NOSC][UMAX], lsh[NOSC], lst[NOSC];
    const int16_t *ld[NOSC];                    /* the oscillators' mip-map levels (a sub-block's) */
    uint32_t vaon[NOSC] = {osc[0].va, osc[1].va, osc[2].va};   /* the ANALOG table (osc_sample) */
    uint32_t nbusy = 0;                         /* the voices played this block (engine/poly.h) */
    int32_t ugl[NOSC][UMAX], ugr[NOSC][UMAX];
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_ticks(), t1, t2;          /* (the split: after the voices, after the effects) */
#endif
    for (k = 0; k < NOSC; k++) {                /* the positions glide, ramped across the block (32-bit divide) */
        uint32_t p1 = wt_pos_glide(osc[k].now, osc[k].target);
        p0[k] = osc[k].now;
        dp[k] = n ? ((int32_t)p1 - (int32_t)p0[k]) / (int32_t)n : 0;
        osc[k].now = (uint32_t)((int32_t)p0[k] + dp[k] * (int32_t)n);   /* (where the ramp ends: exactly) */
        mul[k] = osc[k].mulb;                   /* (the bend in it, each oscillator its own range) */
        un[k] = uon ? uni_n[k] : 1u;            /* unison: the copies, their steps and gains, once a block */
        if (un[k] < 1u || un[k] > UMAX) un[k] = 1u;
        for (l = 0; l < un[k]; l++) { umul[k][l] = uni_mul[k][l]; ugl[k][l] = uni_gl[k][l]; ugr[k][l] = uni_gr[k][l]; }
    }
    for (k = 0; k < NSRC; k++) { g1[k] = mix_g[0][k]; g2[k] = mix_g[1][k]; }
    for (f = 0; f < 2u; f++) {                  /* the filters' settings, once a block */
        ft[f] = flt[f].type < SVF_NTYPE ? flt[f].type : SVF_OFF;
        fres[f] = flt[f].res;
        fdr[f] = flt[f].drive;
        fenv[f] = flt[f].env;
    }
    for (k = 0; k < nmod; k++) { msrc[k] = mslot[k].src; mdst[k] = mslot[k].dst; mscale[k] = mslot[k].scale; }
    for (l = 0; l < NLFO; l++) {                /* the LFOs: the shared ones a value a sub-block, now */
        linc[l] = lfo_incs[l]; lshape[l] = lfo_shapes[l]; lkey[l] = lfo_keys[l]; lfade[l] = (int32_t)lfo_fades[l];
        if ((lused >> l & 1u) && !lkey[l] && !(lsp >> l & 1u))   /* (its speed moved: each voice its own) */
            for (j = 0; j < n && j / ENV_SUB < MAXSUB; j += ENV_SUB) glv[l][j / ENV_SUB] = lfo_step(&glfo[l], lshape[l], linc[l]);
    }
    for (v = 0; v < NV; v++) {
        uint32_t base = voice[v].inc;           /* (the bend: in mul, an oscillator's own) */
        int32_t gofs;                           /* (the glide: read after a note may have set it) */
        uint32_t gkn;
        uint32_t ph0 = voice[v].ph[0], ph1 = voice[v].ph[1], ph2 = voice[v].ph[2], rnd = voice[v].rnd;
        uint32_t st[NOSC], sv[NOSC], trig = voice[v].trig, pos0, pos1, pos2;
        int32_t amp = voice[v].amp, nlp = voice[v].nlp, fb[2], fe[2], note = voice[v].note, vp = voice[v].vel * 258;
        int32_t velg = (vel_gain(vp, vs0) * 11430) >> 15;   /* the level: 11430 at the top keeps four voices in scale */
        int32_t gf = vel_gain(vp, vs1);         /* the filter envelope's reach */
        int32_t gv1[NSRC], gv2[NSRC], dpos[NOSC];
        int32_t fdv[2], fev[2], nkv, pl = 16384, pr = 16384;   /* drive, the filters' envelope, the noise's colour,
                                                   PAN (Q14 a side, 16384 = as it is): as the matrix moves them */
        uint32_t vsplit, panon, wedge = 0;
        svf_state_t sf0 = voice[v].sf[0], sf1 = voice[v].sf[1], sr0 = voice[v].sr[0], sr1 = voice[v].sr[1];
        svf_coef_t cf[2];
        int32_t lv0, lv1, lv2;                  /* (each oscillator heard, this sub-block) */
        int32_t fk[NOSC] = {0, 0, 0}, *fv = voice[v].fmv;   /* FM: each oscillator's k (0: none), its sources' */
        uint32_t ftop[NOSC] = {0, 0, 0}, *fs = voice[v].fms;   /* (its bands' top step, 0: none; its sources) */
        if (trig != voice[v].seen) {            /* a note: the glide from where it comes; then (unless legato) the
                                                   envelopes start, the LFOs with KEY from their PHASE, the LFOs'
                                                   fade-in from 0, the note's random value */
            uint32_t gk = voice[v].gk, gf = voice[v].gfrom, leg = voice[v].leg;
            int32_t from = gf <= 127u ? (int32_t)gf << 16
                         : voice[v].gnote ? ((int32_t)voice[v].gnote - 1) * 65536 + voice[v].gofs : note << 16;
            int32_t go = gk ? from - (note << 16) : 0;
            voice[v].seen = trig;
            voice[v].gofs = go > (48 << 16) ? 48 << 16 : go < -(48 << 16) ? -(48 << 16) : go;
            voice[v].gkn = gk;
            voice[v].gnote = (uint8_t)(note + 1);
            if ((leg & LEG_ALL) && voice[v].env.st != ENV_IDLE && voice[v].env.st != ENV_RELEASE) goto legato;   /* (a
                                                   voice let go or free meanwhile: it starts after all) */
            env_trig(v, leg);                   /* (the envelopes start; one on SINGLE goes on while it sounds) */
            for (l = 0; l < NLFO; l++) {
                if (lkey[l]) lfo_start(&voice[v].lfo[l], lfo_phases[l]);
                voice[v].lfade[l] = 0;
            }
            if (!rnd) rnd = 0x9E3779B9u * (v + 1u);
            voice[v].nrnd = (int32_t)(rnd >> 16) - 32768;
            voice[v].wofs[0] = voice[v].wofs[1] = voice[v].wofs[2] = 0;   /* (an old note's offset: gone) */
        }
    legato:
        gofs = voice[v].gofs;
        gkn = voice[v].gkn;
        if (!voice[v].gate) {
            env_off(&voice[v].env, &amp_env); env_off(&voice[v].fenv, &flt_env);
            env_off(&voice[v].xenv[0], &xenv_r[0]); env_off(&voice[v].xenv[1], &xenv_r[1]);
        }
        if (voice[v].env.st == ENV_IDLE && !amp) { voice[v].busy = 0; continue; }   /* silent: costs nothing */
        voice[v].busy = 1;
        nbusy++;
        if (!rnd) rnd = 0x9E3779B9u * (v + 1u);  /* the noise's seed, a different one a voice */
        pos0 = (uint32_t)((int32_t)p0[0] + voice[v].wofs[0]);   /* the table positions: the shared one + the voice's */
        pos1 = (uint32_t)((int32_t)p0[1] + voice[v].wofs[1]);
        pos2 = (uint32_t)((int32_t)p0[2] + voice[v].wofs[2]);
        for (k = 0; k < NOSC; k++) {
            uint64_t s64 = ((((uint64_t)base * mul[k]) >> 20) * voice[v].km[k]) >> 20;   /* (KEYTRK: km; int64
                                                   multiplies: fine) */
            st[k] = sv[k] = s64 > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)s64;   /* above Nyquist anyway: no wrap */
        }
        for (k = 0; k < NSRC; k++) { gv1[k] = g1[k]; gv2[k] = g2[k]; }
        for (f = 0; f < 2u; f++) {              /* the cutoff: its own, the key, the velocity (the envelope: below) */
            fb[f] = flt[f].base + (note - 60) * flt[f].ktrk + (((int32_t)voice[v].vel * flt[f].velo) >> 10);
            fe[f] = (fenv[f] * gf) >> 15;       /* (<= 24570 x 32768) */
        }
        for (j = 0; j < n; j += ENV_SUB) {      /* the envelopes and the modulation a step every 16 samples, the gain
                                                   and the table positions ramped in between, the filters held */
            static const int32_t NOMOD[16];     /* (no matrix: the envelopes as they are) */
            env_rates_t etmp[2];                /* (the envelopes' rates as the matrix moves them, this voice's) */
            const int32_t *em = NOMOD;          /* the matrix's move of the envelopes' A D S R, AMP FLT 3 4 */
            int32_t lvl, fl, dm[MD_N], vgm = vg, je = (int32_t)(j + ENV_SUB < n ? j + ENV_SUB : n), cnt = je - (int32_t)j;
            int32_t to, da;
            for (k = 0; k < MD_AATK; k++) dm[k] = 0;   /* (the envelopes' and the newer ones: below, only with the
                                                   matrix on) */
            fdv[0] = fdr[0]; fdv[1] = fdr[1]; fev[0] = fe[0]; fev[1] = fe[1]; nkv = nk; vsplit = split; panon = 0;
            if (nmod) {                         /* the matrix first: the sources now (the envelopes as the last step
                                                   left them), a slot at a time; then the envelopes step with it */
                int32_t src[MS_N], msv[NMSLOT];
                const int32_t *scp = mscale;
                for (k = MD_AATK; k < MD_N; k++) dm[k] = 0;
                src[MS_OFF] = 0;
                for (l = 0; l < NLFO; l++) {
                    int32_t lv = 0;
                    if (lused >> l & 1u) {
                        lv = lkey[l] || (lsp >> l & 1u)   /* (L.SPD: a voice's own, free running unless KEY) */
                           ? lfo_step(&voice[v].lfo[l], lshape[l], lsp >> l & 1u ? lfo_speed(linc[l], voice[v].lsm[l]) : linc[l])
                           : glv[l][j / ENV_SUB < MAXSUB ? j / ENV_SUB : MAXSUB - 1u];
                        if (voice[v].lfade[l] < ENV_FULL) {   /* DELAY: fading in after the note (FADE OUT: out) */
                            voice[v].lfade[l] += lfade[l];
                            if (voice[v].lfade[l] > ENV_FULL) voice[v].lfade[l] = ENV_FULL;
                            lv = (int32_t)(((int64_t)lv * (lfo_fout >> l & 1u ? ENV_FULL - voice[v].lfade[l]
                                                                             : voice[v].lfade[l])) >> 30);
                        } else if (lfo_fout >> l & 1u) lv = 0;
                    }
                    src[MS_LFO1 + l] = lv;
                }
                src[MS_ENVAMP] = voice[v].env.lvl >> 15;
                src[MS_ENVFLT] = voice[v].fenv.lvl >> 15;
                src[MS_VELO] = vp;
                src[MS_KEY] = (note - 60) * 512 > 32767 ? 32767 : (note - 60) * 512 < -32767 ? -32767 : (note - 60) * 512;
                src[MS_MACRO] = mod_macro;
                src[MS_WHEEL] = mod_wheel;
                src[MS_RAND] = voice[v].nrnd;
                for (l = 0; l < 2u; l++)        /* ENV 3, ENV 4 (stepped below, while read); their VELO scales them */
                    src[MS_ENV3 + l] = eused >> l & 1u ? ((voice[v].xenv[l].lvl >> 15) * vel_gain(vp, vsens[2u + l])) >> 15 : 0;
                if (xs) mod_more(src, v);       /* (the MIDI sources, the constants, the modifiers) */
                if (amu) scp = mod_amt(msv, mscale, nmod, v);
                for (k = 0; k < nmod; k++) dm[mdst[k]] += (src[msrc[k]] * scp[k]) >> 15;   /* (< 2^29) */
                if (lsp | amu) mod_keep(v, dm);
                em = &dm[MD_AATK];
                for (l = 0; l < 2u; l++)
                    if (eused >> l & 1u) env_step(&voice[v].xenv[l], env_modded(&etmp[0], &xenv_r[l], 2u + l, em + 8u + 4u * l));
                for (k = 0; k < NSRC; k++) { gv1[k] = g1[k]; gv2[k] = g2[k]; }   /* (as set: a move back to 0 too) */
                if (dm[MD_L1] | dm[MD_L2] | dm[MD_L3] | dm[MD_NOISE] | dm[MD_RING] | dm[MD_COLOR] | dm[MD_BAL1]
                    | dm[MD_BAL2] | dm[MD_BAL3] | dm[MD_BALN] | dm[MD_BALR]) {   /* the levels, COLOR, split by BAL */
                    int32_t ncomp = noise_comp;
                    if (dm[MD_COLOR]) {         /* the noise's colour, and its loudness kept (as sound_apply) */
                        int32_t c = (int32_t)noise_col + dm[MD_COLOR];
                        c = c < 0 ? 0 : c > 127 ? 127 : c;
                        nkv = noise_kc(c);
                        ncomp = noise_comp_tab[c];
                    }
                    for (k = 0; k < NSRC; k++) {
                        int32_t lv = mix_lv[k] + dm[MD_L1 + k], w2 = mix_w2[k] + dm[MD_BAL1 + k];   /* (MD_L1 .. RING,
                                                   MD_BAL1 .. BALR: in SRC_ order) */
                        lv = lv < 0 ? 0 : lv > 127 ? 127 : lv;
                        w2 = w2 < 0 ? 0 : w2 > 128 ? 128 : w2;
                        if (k == SRC_NOISE) lv = (lv * ncomp) >> 5;   /* (COLOR's loudness, as sound_apply) */
                        gv2[k] = (lv * w2) >> 7;
                        gv1[k] = lv - gv2[k];
                        if (gv2[k]) vsplit = 1; /* (BAL sends some to filter 2: its own mix) */
                    }
                }
                for (f = 0; f < 2u; f++) {      /* the drive, the envelope's reach */
                    if (dm[MD_DRV1 + f]) {
                        int32_t d = (int32_t)flt_drv[f] + dm[MD_DRV1 + f];
                        d = d < 0 ? 0 : d > 127 ? 127 : d;
                        fdv[f] = d ? 256 + d * 14 : 0;
                    }
                    if (dm[MD_FENV1 + f]) fev[f] = fe[f] + ((dm[MD_FENV1 + f] * 195 * gf) >> 15);   /* (+-127: the
                                                   knob's whole -63..63) */
                }
                if (dm[MD_PAN]) {               /* PAN: -127 all left .. 127 all right, the sum kept */
                    int32_t pn = dm[MD_PAN] * 129;
                    pn = pn < -16384 ? -16384 : pn > 16384 ? 16384 : pn;
                    pl = 16384 - pn; pr = 16384 + pn; panon = 1;
                }
                vgm = vg + dm[MD_VOL];
                vgm = vgm < 0 ? 0 : vgm > 127 ? 127 : vgm;
            }
            {                                   /* the pitch: the glide (an exponential approach, as an analog
                                                   portamento) and the matrix; none: the steps as they are */
                int32_t pc = dm[MD_PITCH];
                if (gofs) {
                    pc += glide_gls ? ((gofs >> 8) + 128) & ~255 : gofs >> 8;   /* (Q16 -> Q8 semitones; GLISS:
                                                   whole ones) */
                    gofs = (int32_t)(((int64_t)gofs * gkn) >> 30);
                    if (gofs < 64 && gofs > -64) gofs = 0;   /* (within 1/1000 of a semitone: there) */
                }
                if (pc | dm[MD_P1] | dm[MD_P2] | dm[MD_P3])
                    for (k = 0; k < NOSC; k++) {
                        uint64_t s64 = ((uint64_t)st[k] * mod_pitch_q20(pc + dm[MD_P1 + k])) >> 20;
                        sv[k] = s64 > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)s64;
                    }
                else for (k = 0; k < NOSC; k++) sv[k] = st[k];
                if (uon)                        /* the unison copies' steps (64-bit multiply: fine); DET moves
                                                   their detune: x 0 .. x 3 */
                    for (k = 0; k < NOSC; k++) {
                        int32_t dq = nmod && dm[MD_DET1 + k] ? 64 + dm[MD_DET1 + k] : 64;
                        dq = dq < 0 ? 0 : dq > 192 ? 192 : dq;
                        for (l = 0; l < un[k]; l++) {
                            uint32_t um = dq == 64 ? umul[k][l]
                                        : (uint32_t)((int32_t)(1 << 20) + (((int32_t)umul[k][l] - (1 << 20)) * dq >> 6));
                            uint64_t u64 = ((uint64_t)sv[k] * um) >> 20;
                            us[k][l] = u64 > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)u64;
                        }
                    }
                if (fmon | spx) fm_prep(fk, ftop, sv, nmod ? dm : 0, fs);   /* (FM: its k, its bands' top; sync:
                                                   OSC2's level a step darker) */
                for (k = 0; k < NOSC; k++) {    /* the mip-map level for this pitch (the highest copy's; FM's bands) */
                    uint32_t top = sv[k], lv;
                    for (l = 0; un[k] > 1u && l < un[k]; l++) if (us[k][l] > top) top = us[k][l];
                    if (ftop[k] > top) top = ftop[k];
                    lv = wt_level(top);
                    ld[k] = (k == 0 ? tb0 : k == 1 ? tb1 : tb2) + WT_LV_OFS[lv];
                    lsh[k] = WT_LV_SH[lv];
                    lst[k] = (1u << (32u - WT_LV_SH[lv])) + 1u;
                    if (vaon[k]) lsh[k] = 0, lst[k] = va_inv(sv[k]);   /* ANALOG: marked, its inverse step (the
                                                   copies use the note's; a 32-bit divide) */
                }
            }
            lvl = voice[v].kill ? env_kill(&voice[v].env)   /* (taken for the voices' limit: out fast) */
                : env_step(&voice[v].env, env_modded(&etmp[0], &amp_env, 0, em));   /* the envelopes, as moved now */
            fl = env_step(&voice[v].fenv, env_modded(&etmp[1], &flt_env, 1, em + 4)) >> 15;
            wedge = 0;
            for (k = 0; k < NOSC; k++) {        /* the table positions: the shared glide + this voice's modulation,
                                                   kept inside the table (a little margin: the ramp's rounding);
                                                   pushed past an end: it stops there, marked for the screen */
                int32_t bj = (int32_t)p0[k] + dp[k] * (int32_t)j, be = (int32_t)p0[k] + dp[k] * je;
                int32_t cur = (int32_t)(k == 0 ? pos0 : k == 1 ? pos1 : pos2) - bj, tgt = be + ((dm[MD_WAVE] + dm[MD_W1 + k]) << 8);
                if (tgt < 256 || tgt > (int32_t)WT_POS_MAX - 16) {
                    if (dm[MD_WAVE] + dm[MD_W1 + k]) wedge |= 1u << k;
                    tgt = tgt < 256 ? 256 : (int32_t)WT_POS_MAX - 16;
                }
                tgt -= be;
                if (!(dm[MD_WAVE] | dm[MD_W1 + k]) && !cur) tgt = 0;   /* (no modulation, none left: the glide itself) */
                dpos[k] = dp[k] + (tgt - cur) / cnt;   /* (toward 0: the ramp never passes its target) */
            }
            to = (int32_t)(((((lvl >> 15) * velg) >> 15) * vgm) >> 7);
            da = (to - amp) >> 4;               /* (>> 4: ENV_SUB) */
            for (f = 0; f < 2u; f++)
                if (ft[f]) {
                    int32_t cn = fb[f] + ((fev[f] * fl) >> 15) + dm[MD_CUT] + dm[MD_CUT1 + f];
                    int32_t rs = (int32_t)fres[f] + dm[MD_RES1 + f];
                    svf_coef(&cf[f], cn, (uint32_t)(rs < 0 ? 0 : rs > 127 ? 127 : rs), ft[f]);
                    cf[f].nl = fdv[f] != 0;     /* (DRIVE on: the resonance driven too) */
                    voice[v].fn[f] = cn;
                }
            if (fk[0] | fk[1] | fk[2] | spx) {  /* FM, sync, a comb or a DRIVE curve this sub-block: its own loop,
                                                   out of app_audio (vfm_run); the loops below stay as they were */
                vfm_t c;
                c.i0 = j; c.i1 = (uint32_t)je; c.amp = amp; c.da = da;
                c.ph[0] = ph0; c.ph[1] = ph1; c.ph[2] = ph2; c.pos[0] = pos0; c.pos[1] = pos1; c.pos[2] = pos2;
                c.rnd = rnd; c.nlp = nlp; c.nkv = nkv; c.dpos = dpos; c.ld = ld; c.lsh = lsh; c.lst = lst; c.sv = sv;
                c.un = un; c.us = us; c.ugl = ugl; c.ugr = ugr; c.uph = voice[v].uph; c.gv1 = gv1; c.gv2 = gv2;
                c.vsplit = vsplit; c.ust = ust; c.par = par; c.panon = panon; c.pl = pl; c.pr = pr;
                c.sf0 = sf0; c.sf1 = sf1; c.sr0 = sr0; c.sr1 = sr1; c.cf = cf; c.ft = ft; c.fdv = fdv;
                c.fk = fk; c.fs = fs; c.fv = fv; c.out = out;
                c.sync = osc_sync; c.dcv[0] = flt_curve[0]; c.dcv[1] = flt_curve[1]; c.dcb = voice[v].dcb;
                c.dm = nmod ? dm : 0; c.fn = voice[v].fn;
                c.mph = voice[v].mph; c.s1d = voice[v].s1d; c.cbuf = comb_buf[v]; c.cw = voice[v].cw;
                vfm_run(&c);
                voice[v].mph = c.mph; voice[v].s1d = c.s1d; voice[v].cw = c.cw;
                ph0 = c.ph[0]; ph1 = c.ph[1]; ph2 = c.ph[2]; pos0 = c.pos[0]; pos1 = c.pos[1]; pos2 = c.pos[2];
                rnd = c.rnd; nlp = c.nlp; sf0 = c.sf0; sf1 = c.sf1; sr0 = c.sr0; sr1 = c.sr1;
                amp = to;
                continue;
            }
            lv0 = gv1[0] | gv2[0] | gv1[4] | gv2[4];   /* an oscillator heard (its level, the ring's); one not: its
                                                   phase runs on, its sample is not read (0 x its gain: 0 anyway) */
            lv1 = gv1[1] | gv2[1] | gv1[4] | gv2[4];
            lv2 = gv1[2] | gv2[2];
            {                                   /* the sub-block in passes (blk_*, 2026-11-16): the noise, each
                                                   oscillator, the ring, the mix, the filters, the gain; unison's
                                                   right side (copies panned) its own mix and filters */
                int32_t bo[NOSC][ENV_SUB], bor[NOSC][ENV_SUB], bnz[ENV_SUB], brg[ENV_SUB], bs[ENV_SUB], bs2[ENV_SUB];
                int32_t byl[ENV_SUB], byr[ENV_SUB], *orr[NOSC];
                const int32_t *src[NSRC];
                uint32_t cn = (uint32_t)cnt, *php[NOSC], pst[NOSC], hd[NOSC], st2 = uon && ust;
                php[0] = &ph0; php[1] = &ph1; php[2] = &ph2;
                pst[0] = pos0; pst[1] = pos1; pst[2] = pos2;
                hd[0] = lv0 != 0; hd[1] = lv1 != 0; hd[2] = lv2 != 0;
                for (i = 0; i < cn; i++) {      /* the noise: white (xorshift32), then its colour (a one-pole low-pass) */
                    rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
                    nlp += ((((int32_t)rnd >> 17) - nlp) * nkv) >> 15;
                    bnz[i] = nlp;
                }
                for (k = 0; k < NOSC; k++) {    /* the oscillators; one not heard: its phases run on, nothing read */
                    orr[k] = bo[k];             /* (the right side: the left's, unless copies are panned) */
                    if (uon && un[k] > 1u) {
                        if (hd[k]) {
                            blk_uni(bo[k], st2 ? bor[k] : 0, cn, ld[k], lsh[k], lst[k], voice[v].uph[k], us[k], ugl[k],
                                    ugr[k], un[k], pst[k], dpos[k]);
                            if (st2) orr[k] = bor[k];
                        } else for (l = 0; l < un[k]; l++) voice[v].uph[k][l] += us[k][l] * cn;
                    } else if (hd[k]) blk_osc(bo[k], cn, ld[k], lsh[k], lst[k], php[k], sv[k], pst[k], dpos[k]);
                    else *php[k] += sv[k] * cn;
                }
                pos0 = (uint32_t)((int32_t)pos0 + dpos[0] * cnt);   /* (where the sub-block's ramps end) */
                pos1 = (uint32_t)((int32_t)pos1 + dpos[1] * cnt);
                pos2 = (uint32_t)((int32_t)pos2 + dpos[2] * cnt);
                src[SRC_O1] = bo[0]; src[SRC_O2] = bo[1]; src[SRC_O3] = bo[2]; src[SRC_NOISE] = bnz; src[SRC_RING] = brg;
                if (gv1[SRC_RING] | gv2[SRC_RING]) blk_ring(brg, cn, bo[0], bo[1], uon);   /* (unison's: halves first) */
                blk_mix(bs, cn, src, gv1);
                if (vsplit) blk_mix(bs2, cn, src, gv2);   /* (what BAL sends to filter 2) */
                blk_filt(byl, bs, bs2, cn, &sf0, &sf1, cf, ft, fdv, par, vsplit);
                if (st2) {                      /* the right side: its own mix, its own filters */
                    src[SRC_O1] = orr[0]; src[SRC_O2] = orr[1]; src[SRC_O3] = orr[2];
                    if (gv1[SRC_RING] | gv2[SRC_RING]) blk_ring(brg, cn, orr[0], orr[1], 1u);
                    blk_mix(bs, cn, src, gv1);
                    if (vsplit) blk_mix(bs2, cn, src, gv2);
                    blk_filt(byr, bs, bs2, cn, &sr0, &sr1, cf, ft, fdv, par, vsplit);
                }
                blk_out(&out[2u * j], byl, st2 ? byr : byl, cn, amp, da, panon, pl, pr);
            }
            amp = to;                           /* (the ramp's rounding does not pile up) */
        }
        if (voice[v].env.st == ENV_IDLE) {      /* the voice is free: the next note starts from rest */
            amp = 0;
            sf0.ic1 = sf0.ic2 = sf0.ic3 = sf0.ic4 = 0;
            sf1 = sf0;
            sr0 = sf0; sr1 = sf1;
            voice[v].fenv.lvl = 0;
            voice[v].fenv.st = ENV_IDLE;
            voice[v].xenv[0].lvl = voice[v].xenv[1].lvl = 0;
            voice[v].xenv[0].st = voice[v].xenv[1].st = ENV_IDLE;
            gofs = 0;                           /* (a glide cut short: the next one from the note itself) */
            if (spx) comb_clear(v);             /* (its combs ringing on: gone) */
        }
        voice[v].gofs = gofs;
        voice[v].ph[0] = ph0; voice[v].ph[1] = ph1; voice[v].ph[2] = ph2; voice[v].rnd = rnd;
        voice[v].wofs[0] = (int32_t)pos0 - (int32_t)osc[0].now;   /* (its modulation of the table, where it ends) */
        voice[v].wofs[1] = (int32_t)pos1 - (int32_t)osc[1].now;
        voice[v].wofs[2] = (int32_t)pos2 - (int32_t)osc[2].now;
        voice[v].wedge = (uint8_t)wedge;
        voice[v].amp = amp;
        voice[v].nlp = nlp;
        voice[v].sf[0] = sf0; voice[v].sf[1] = sf1;
        if (!ust) { sr0 = sf0; sr1 = sf1; }     /* (mono: the right side's filters follow, ready for SPREAD) */
        voice[v].sr[0] = sr0; voice[v].sr[1] = sr1;
    }
#ifndef DINGHY_HOST
    t1 = fm1_ticks();
#endif
    fx_slots(out, n);                           /* the effects, slot 1 then slot 2 (out of app_audio) */
#ifndef DINGHY_HOST
    t2 = fm1_ticks();
#endif
    {                                           /* the EQ: a new set of coefficients first, if one is whole */
        uint32_t q = eq_seq, w;
        if (!(q & 1u) && q != eq_seen) {
            int32_t *d = (int32_t *)&eqc;
            for (w = 0; w < EQ_WORDS; w++) d[w] = eq_box[w];
            eq_seen = q;
        }
        eq_run(&eqs, &eqc, out, n);
    }
    {                                           /* how loud the block ended: an effect's tail is not silence */
        int32_t pk = 0, x;
        for (i = 0; i < 2u * n; i += 16u) { x = out[i] < 0 ? -out[i] : out[i]; if (x > pk) pk = x; }
        out_peak = pk;
    }
    for (i = 0; i < 2u * n; i++) {              /* the output: soft above OUT_KNEE, never past full scale (MASTER
                                                   only turns it down: nothing is clipped hard after this) */
#ifdef VOLNA_PROBE
        out[i] >>= 2;                           /* (the host's level probe: what goes in, a quarter, unlimited) */
#endif
        out[i] = out_limit(out[i]);
    }
#ifndef DINGHY_HOST
    {
        static uint32_t seen;
        uint32_t dt = fm1_ticks() - t0, r = cpu_reset;
        if (r != seen) { seen = r; cpu_ticks = 0; cpu_sum = 0; cpu_cnt = 0; }
        cpu_sum += dt;                          /* (half a second: ~86 blocks of ~140 000 ticks at most, fits) */
        cpu_cnt++;
        if (dt > cpu_ticks) { cpu_ticks = dt; cpu_part[0] = t1 - t0; cpu_part[1] = t2 - t1; cpu_nv = nbusy; }
        if (dt > POLY_OVER && nbusy >= NV) cpu_over = 1;   /* (every voice on and out of time: one goes now) */
    }
#endif
}

/* ---- notes (main loop) */
static const uint32_t INC_C9[12] = {            /* MIDI 120..131 (C9..B9); lower octaves shift right */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
static uint32_t ages, last_note = 255;
static uint32_t isqrt32(uint32_t x)             /* floor(sqrt(x)), bit by bit (main loop) */
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* the velocity curve (SETUP: CURVE), on every note: the keys, MIDI in, the SMK's keys and pads */
enum { VC_LIN, VC_SOFT, VC_HARD, VC_FIX, NVC };
static uint8_t vel_curve, vel_fix = 100;        /* (set from the settings by sound_apply) */
static uint32_t vel_shape(uint32_t vel)         /* 1..127 -> 1..127 */
{
    switch (vel_curve) {
    case VC_SOFT: return isqrt32(vel * 127u);   /* a root: a soft touch goes further (30 -> 61) */
    case VC_HARD: vel = (vel * vel + 63u) / 127u; return vel ? vel : 1u;   /* a square: it takes a hard one (30 -> 7) */
    case VC_FIX: return vel_fix;                /* every note the same */
    default: return vel;
    }
}

/* the voice mode (PATCH: MODE) and the glide (GLIDE), set from the patch by sound_apply.
 * POLY: 4 voices; a voice glides from its own last pitch (GLIDE above 0).
 * MONO: one voice, every note starts the envelopes again; it glides from the note before (GLIDE above 0).
 * LEGATO: one voice; a note while another is held only moves the pitch (no attack) and glides; a note on its own
 * starts the envelopes and does not glide (fingered portamento).
 * Mono or legato, the keys held are a stack: letting go of the newest goes back to the one before. */
enum { VM_POLY, VM_MONO, VM_LEGATO, VM_N };
enum { GM_PORTA, GM_FINGP, GM_GLISS, GM_FINGG, GM_N };
enum { PC_INIT, PC_ARP, PC_ATMO, PC_BASS, PC_DRUM, PC_FX, PC_KEYS, PC_LEAD, PC_MONO, PC_PAD, PC_PERC, PC_POLY, PC_SEQ,
       PC_N };                                  /* a patch's categories (the Blofeld's) */   /* the glide's modes (the Blofeld's): bit 0 FINGERED (only from
                                                   a key held), bit 1 GLISSANDO (semitone steps) */
#define GLIDE_STEP 64u                          /* an arp step's glide when the patch's GLIDE is 0 (~0.1 s) */
enum { PG_NONE, PG_GLIDE, PG_SLIDE };           /* play's glide: none but the patch's; an arp step's glide; its
                                                   slide (legato from the note held, as a 303's) */
static uint8_t vmode, glide_t, glide_mode;      /* (set from the patch by sound_apply) */
static int8_t osc_okt[NOSC];                    /* the oscillators' KEYTRK (sound_apply's, for voice_go) */
static uint8_t env_sgl;                         /* the envelopes on SINGLE trigger (sound_apply's, for voice_go) */
static uint8_t mono_note[16], mono_vel[16], mono_n;   /* the keys held, oldest first (MONO, LEGATO) */
static uint32_t glide_k(uint32_t t) { return t ? ENV_COEF[t & 127u] : 0u; }

/* voice v plays note (vel shaped already): leg 1 legato (the envelopes go on), gk the glide (0 none), from the note
 * it glides from (255: the voice's own pitch) */
static void voice_go(uint32_t v, uint32_t note, uint32_t vel, uint32_t leg, uint32_t gk, uint32_t from)
{
    voice[v].kill = 0;                          /* (a voice fading out for the limit: a note again) */
    voice[v].note = (uint8_t)note;
    voice[v].ppress = 0;                        /* (a new note: no pressure yet) */
    voice[v].age = ++ages;
    voice[v].inc = INC_C9[note % 12u] >> (10u - note / 12u);
    {                                           /* KEYTRK: the oscillators' steps about C4 (100 %: 1) */
        uint32_t k;
        for (k = 0; k < NOSC; k++) {
            int32_t d = ((int32_t)note - 60) * 256 * osc_okt[k] / 50;   /* (Q8 semitones: (note - C4) (KEYTRK - 1)) */
            voice[v].km[k] = osc_okt[k] ? mod_pitch_q20(d) : 1u << 20;
        }
    }
    voice[v].vel = (uint8_t)vel;
    voice[v].gk = gk;
    voice[v].gfrom = (uint8_t)(from <= 127u ? from : 255u);
    voice[v].leg = (uint8_t)((leg ? LEG_ALL : 0u) | env_sgl);   /* (SINGLE: the envelopes it asks for) */
    voice[v].gate = 1;
    voice[v].trig++;                            /* last: the ISR starts the note on it */
    last_note = note;
}
static void mono_drop(uint32_t note)            /* the key out of the stack */
{
    uint32_t i, j;
    for (i = 0; i < mono_n; i++)
        if (mono_note[i] == note) {
            for (j = i; j + 1u < mono_n; j++) { mono_note[j] = mono_note[j + 1u]; mono_vel[j] = mono_vel[j + 1u]; }
            mono_n--;
            return;
        }
}
/* the voices' limit (engine/poly.h): a voice sounds while its key is down or it rings on, unless taken */
static uint32_t voice_sounds(uint32_t v) { return (voice[v].gate || voice[v].busy) && !voice[v].kill; }
static uint32_t poly_sounding(void)
{
    uint32_t v, n = 0;
    for (v = 0; v < NV; v++) n += voice_sounds(v);
    return n;
}
static void poly_set(uint32_t cap)              /* the limit now; voices past it go (the oldest released first, else
                                                   the oldest held: as a note would steal them) */
{
    if (cap < 1u || cap > NV) cap = NV;
    poly_n = (uint8_t)cap;
    while (vmode == VM_POLY && poly_sounding() > cap) {
        uint32_t v, pick = 0, best = 0xFFFFFFFFu;
        for (v = 0; v < NV; v++) {
            uint32_t score;
            if (!voice_sounds(v)) continue;
            score = !voice[v].gate ? voice[v].age & 0x3FFFFFFFu : 0x40000000u + (voice[v].age & 0x3FFFFFFFu);
            if (score < best) { best = score; pick = v; }
        }
        voice[pick].gate = 0;
        voice[pick].kill = 1;
    }
}
static void poly_service(void)                  /* (main loop, often) a block out of time with every voice on */
{
#ifndef DINGHY_HOST
    if (cpu_over) { cpu_over = 0; if (poly_n == NV) poly_set(NV - 1u); }
#elif defined VOLNA_POLY_DROP_MS
    static uint8_t done;                        /* (the host has no block times: a test drops a voice at a time) */
    if (!done && fm1_ms >= VOLNA_POLY_DROP_MS) { done = 1; poly_set(NV - 1u); }
#endif
}
static void poly_learn(uint32_t tot, uint32_t vox, uint32_t nv)   /* (every half second: SETUP's figures) */
{
    uint32_t c = poly_next(poly_n, NV, tot, vox, nv);
    if (c != poly_n) poly_set(c);
}

/* a note: pg PG_NONE (keys, MIDI: the patch's GLIDE), PG_GLIDE (an arp step's glide: from the note before), PG_SLIDE
 * (an arp step's slide: the voice playing from moves to note, legato) */
static void play_g(uint32_t note, uint32_t vel, uint32_t pg, uint32_t from)
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu, gk = glide_k(glide_t), room;
    if (note > 127u || !vel) return;
    vel = vel_shape(vel > 127u ? 127u : vel);
    if (pg == PG_NONE && (glide_mode & 1u)) {  /* FINGERED (P, G): a glide only from a key still held */
        uint32_t held = vmode != VM_POLY ? mono_n > 0u : 0u;
        for (v = 0; vmode == VM_POLY && v < NV; v++) held |= voice[v].gate;
        if (!held) gk = 0;
    }
    if (pg != PG_NONE && !gk) gk = glide_k(GLIDE_STEP);
    if (vmode != VM_POLY) {                     /* one voice: the stack */
        uint32_t others;
        if (pg == PG_SLIDE) mono_drop(from);    /* (the arp never ends the note it slides from) */
        mono_drop(note);
        if (mono_n == 16u) mono_drop(mono_note[0]);   /* (full: the oldest goes) */
        mono_note[mono_n] = (uint8_t)note; mono_vel[mono_n++] = (uint8_t)vel;
        others = mono_n > 1u || pg == PG_SLIDE; /* (a slide: the note before still sounds) */
        if (vmode == VM_LEGATO)
            voice_go(0, note, vel, others, others || pg != PG_NONE ? gk : 0u, pg != PG_NONE ? last_note : 255u);
        else voice_go(0, note, vel, pg == PG_SLIDE, gk, pg != PG_NONE ? last_note : 255u);
        return;
    }
    if (pg == PG_SLIDE)                         /* the voice that plays the note slid from: it moves, legato */
        for (v = 0; v < NV; v++)
            if (voice[v].gate && voice[v].note == from) { voice_go(v, note, vel, 1, gk, 255u); return; }
    room = poly_sounding() < poly_n;
    for (v = 0; v < NV; v++) {                  /* the same note; else a free voice (while fewer than poly_n sound);
                                                   else the oldest released; else the oldest held */
        uint32_t snd = voice_sounds(v);
        uint32_t score = snd && voice[v].note == note ? 0u
                       : !snd ? (room ? 1u : 0xFFFFFFFFu)
                       : !voice[v].gate ? 0x40000000u + (voice[v].age & 0x3FFFFFFFu)
                       : 0x80000000u + (voice[v].age & 0x3FFFFFFFu);
        if (score < best) { best = score; pick = v; }
    }
    voice_go(pick, note, vel, 0, gk, pg != PG_NONE ? last_note : 255u);   /* (an arp glide: from the note before,
                                                   whichever voice played it) */
}
static void play(uint32_t note, uint32_t vel) { play_g(note, vel, PG_NONE, 255u); }
static void stop(uint32_t note)
{
    uint32_t v;
    if (vmode != VM_POLY) {                     /* one voice: back to the key held before, or let go */
        uint32_t top = mono_n ? mono_note[mono_n - 1u] : 255u;
        mono_drop(note);
        if (!mono_n) { voice[0].gate = 0; return; }
        if (note == top)                        /* (MONO starts it again; LEGATO glides back to it) */
            voice_go(0, mono_note[mono_n - 1u], mono_vel[mono_n - 1u], vmode == VM_LEGATO, glide_k(glide_t), 255u);
        return;
    }
    for (v = 0; v < NV; v++)
        if (voice[v].note == note)
            voice[v].gate = 0;
}
static void voices_off(void)                    /* every voice let go, no key held */
{
    uint32_t v;
    for (v = 0; v < NV; v++) voice[v].gate = 0;
    mono_n = 0;
}
static int quiet(void)                          /* nothing sounds, not even a release or an effect's tail */
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].gate || voice[v].busy) return 0;
    return out_peak < 8;
}

/* ---- what survives a power-off (up to SECTOR_MAX bytes): the panel's state and the sound */
typedef struct { uint8_t wave; int8_t semi, cents; uint8_t level, table; } osc_set_t;   /* an oscillator (table:
                                                   wt_table; 0 BASIC .. 5 FOLD, then local) */
typedef struct {                                /* a filter */
    uint8_t type, cut, res;                     /* SVF_* (engine/svf.h); the cutoff 0..127 (13 Hz .. 15.8 kHz, a
                                                   semitone a step); the resonance 0..127 (Q 0.5 .. 25) */
    int8_t env;                                 /* the filter envelope's reach, -63..63 (63: +8 octaves at its top) */
    uint8_t ktrk;                               /* key tracking, 0..100 % (100: the cutoff follows the note) */
    uint8_t drive;                              /* 0..127: a gain up to 8x into a soft clip before the filter; not 0:
                                                   the resonance driven too (engine/svf.h, svf_sat) */
    int8_t velo;                                /* -63..63: the velocity moves the cutoff (63: +4 octaves at 127) */
} filt_set_t;
#define NENV 4u                                 /* AMP, FLT, ENV 3, ENV 4 */
typedef struct {                                /* an envelope (0..127; times: tools/gen_env.py) */
    uint8_t a, d, s, r;
    int8_t vel;                                 /* VELO -63..63: how much the velocity scales it (AMP: the level, FLT:
                                                   the reach, 3 and 4: their level); 0 none, 63 all of it, < 0 the
                                                   other way round */
    uint8_t mode, al, d2, s2;                   /* EM_* (engine/env.h); the attack level, decay 2, sustain 2 */
} env_set_t;
typedef struct {                                /* an effect's slot (0..127 each; what p[] means: by the type) */
    uint8_t type, mix;                          /* FX_*; dry .. wet (64: both full) */
    uint8_t p[6];                               /* DELAY: TIME, FEEDBK, SYNC, TONE, PING; REVERB: SIZE, DAMP, PRE,
                                                   WIDTH; DIST: DRIVE, TONE, SHAPE, LEVEL; CHORUS: RATE, DEPTH,
                                                   MODE, FEEDBK (64 = 0), WIDTH, DELAY */
} fx_set_t;
static const fx_set_t FX_DEF[FX_N] = {          /* a type's starting values (TYPE turned: these) */
    {FX_OFF, 0, {0}}, {FX_DELAY, 40, {70, 50, 0, 90, 0}}, {FX_REVERB, 36, {80, 50, 10, 127}},
    {FX_DIST, 127, {84, 100, DS_SOFT, 40}},     /* (DIST: all wet, x 16, LEVEL x 0.1: a note about as loud as dry) */
    {FX_CHORUS, 50, {50, 50, CM_CHORUS, 64, 127, 50}},   /* (CHORUS: ~0.6 Hz, +-3 ms around 14 ms, the sides apart) */
    {FX_PHASER, 127, {40, 80, 72, 89, 1, 64}},   /* (PHASER: ~0.4 Hz, +-2 octaves around 523 Hz, feedback +0.38,
                                                   4 stages, the sides a quarter apart; MIX 127: the classic half
                                                   and half) */
    {FX_ENSEMBLE, 56, {75, 80, 50, 76, 90, 127}},   /* (ENSEMBLE: 0.56 Hz +-1.9 ms, vibrato 5.8 Hz +-0.23 ms,
                                                   TONE 7.9 kHz, the sides opposite) */
    {FX_LOFI, 127, {40, 70, 100, 30, 20, 0}},   /* (LO-FI: 13.8 kHz, 8.3 bits, TONE 8.9 kHz, a little wobble and
                                                   hiss; all wet) */
    {FX_FILTER, 127, {70, 70, 0, FS_LFO, 104, 50}},
    {FX_TRIPLE, 127, {50, 60, 50, 0, 30, 0}}};   /* (TRIPLE: the chorus ~0.6 Hz +-2 ms at half, no S&H, a little
                                                   drive; all wet) */   /* (FILTER: LP24 at 587 Hz, resonance 70, an LFO ~0.56 Hz
                                                   +-3.2 octaves; all wet) */
#define FF_NTYPE 7u                             /* FILTER's types (p[2]), the main filter's order without OFF */
static const uint8_t FF_TYPE[FF_NTYPE] = {SVF_LP24, SVF_LP, SVF_BP24, SVF_BP, SVF_HP24, SVF_HP, SVF_NOTCH};
#define PH_FLO 36u                              /* PHASER's FREQ (p[2]): a note, 65 Hz .. 4.2 kHz */
#define PH_FHI 108u
#define PH_NSTG 6u                              /* STAGES (p[4]): 0..5 -> 2, 4, .. 12 */
typedef struct {                                /* the EQ (engine/eq.h): LOW MID HIGH */
    int8_t gain[EQ_NB];                         /* -12 .. 12 dB (0: the band off) */
    uint8_t note[EQ_NB];                        /* each band's frequency, a note (MIDI numbers): EQ_NLO .. EQ_NHI */
    uint8_t q;                                  /* MID's width: svf.h's resonance 0 .. EQ_QMAX (Q 0.5 .. 8) */
} eq_set_t;
static const uint8_t EQ_NLO[EQ_NB] = {24, 36, 72};    /* 32.7 Hz, 65 Hz, 523 Hz */
static const uint8_t EQ_NHI[EQ_NB] = {84, 120, 128};  /* 1.05 kHz, 8.4 kHz, 13.3 kHz (svf_a1's sum under 16 at +12 dB) */
#define EQ_QMAX 90u
typedef struct {                                /* the sound (the patch format grows from this, stage 6) */
    osc_set_t osc[NOSC];
    uint8_t noise, ring, color;                 /* the mixer: noise, ring mod osc 1 x osc 2, the noise's colour */
    int8_t bal[5];                              /* O1 O2 O3 NOISE RING: -63 all into filter 1 .. 63 all into filter 2
                                                   (SER: past filter 1, straight into 2) */
    filt_set_t flt[2];
    uint8_t route;                              /* 0: filter 1 into filter 2 (SER); 1: side by side (PAR) */
    env_set_t env[NENV];                        /* the envelopes: AMP (the level), FLT (the filters' cutoff), ENV 3 and
                                                   ENV 4 (sources of the matrix) */
    uint8_t volume;                             /* the patch's level */
    arp_par_t arp;                              /* the arpeggiator (engine/arp.h) */
    lfo_set_t lfo[NLFO];                        /* the LFOs (engine/mod.h) */
    mod_slot_t mod[NMSLOT_A];                   /* the matrix: source, destination, amount (slots 1..8) */
    uint8_t glide;                              /* stage 6b: GLIDE 0 off, 1..127 its time (as the envelopes' decay) */
    uint8_t vmode;                              /* VM_POLY, VM_MONO, VM_LEGATO */
    uint8_t bend;                               /* the pitch bend's range, 0..24 semitones */
    uint8_t uni[NOSC];                          /* stage 7a: each oscillator's unison copies, 1..UMAX (1: plain) */
    uint8_t udet[NOSC];                         /* DETUNE 0..127: the outer copies +-100 cents at 127, squared */
    uint8_t uspr[NOSC];                         /* SPREAD 0..127: the copies across the stereo field (0: mono) */
    fx_set_t fx[2];                             /* stage 7b: the effects' slots */
    mod_slot_t mod2[NMSLOT - NMSLOT_A];         /* the matrix's slots 9..16 (2026-11-14) */
    eq_set_t eq;                                /* the EQ, the effects' third slot (2026-11-15; past the bank's record:
                                                   its tail blob, see patch_get) */
    uint16_t ost[ARP_PLEN];                     /* the patch's own pattern (PATTERN OWN, 2026-11-15): its steps (arp.h);
                                                   its LENGTH and T.AMT are arp.plen and arp.tfac. In the tail too */
    modf_set_t modf[NMODF];                     /* the modifiers (2026-11-15; partly in the second tail, TAIL2_BLOB) */
    uint8_t fms[NOSC], fma[NOSC];               /* FM (2026-11-15): each oscillator's source (FMS_*), AMT 0..127 */
    uint8_t sync;                               /* hard sync (2026-11-15): OSC2 synced to OSC3, 0/1 */
    uint8_t dcurve[2];                          /* the filters' DRIVE curves (2026-11-16): DC_* */
    uint8_t ffms[2], ffma[2];                   /* Filter FM (2026-11-16): each filter's source (FMS_*), AMT 0..127 */
    int8_t fpan[2];                             /* each filter's PAN, -63 .. 63 */
    uint8_t gmode;                              /* the glide's mode (2026-11-16): GM_* */
    int8_t okt[NOSC];                           /* each oscillator's KEYTRK: 100 + 2 x this % (0: 100 %, as before;
                                                   -50: none, a drone; -128 .. 48: -156 .. 196 %) */
    uint8_t obend[NOSC];                        /* each oscillator's bend range: 0 the patch's BEND, else 25 + its own
                                                   (-24 .. 24 semitones) */
    uint8_t lfout;                              /* the LFOs fading out (a bit each; DELAY their time) */
    uint8_t wlim;                               /* LIMIT WT: the oscillators on whole waves (a bit each) */
    uint8_t etrig;                              /* the envelopes on SINGLE trigger (a bit each: AMP FLT 3 4) */
    uint8_t cat;                                /* the patch's category (2026-11-16, the Blofeld's): PC_* */
    uint32_t patver;                            /* 0: legacy reference; 1: independent patch pattern */
    uint32_t pat_source;                        /* last template copied into this patch (1..ARP_NPAT; 0: none) */
} patch_t;
_Static_assert(__builtin_offsetof(patch_t, patver) >= 266u, "pattern version overlaps legacy patch bytes");
_Static_assert(__builtin_offsetof(patch_t, pat_source) == 272u, "template source overlaps v4 patch bytes");
static mod_slot_t *pslot(patch_t *p, uint32_t i)   /* the matrix's slot i (0..15) of patch p */
{
    return i < NMSLOT_A ? &p->mod[i] : &p->mod2[(i - NMSLOT_A) % (NMSLOT - NMSLOT_A)];
}
static struct {
    uint8_t oct, ch;                            /* the octave (-2..2), the MIDI channel (0 = OMNI) */
    uint8_t vcurve, keyvel, setup_row;          /* SETUP: the velocity curve (VC_), the FM-1 keys' velocity (and
                                                   FIX's), the page's row */
    uint8_t sec, osc_sel, osc_row, mix_row;     /* the screen: the section, the oscillator and its row, the mixer's
                                                   row (A B C) */
    uint8_t filt_sel, filt_row, env_sel, env_row;   /* the filter (0, 1) and its row; the envelope (AMP, FLT), its row */
    uint8_t arp_row, steps_row, step_sel;       /* the ARP page's row; STEPS': its row, the step (0..15) */
    uint8_t mod_lfo, mod_slot, mod_row;         /* MOD: the LFO (0..2) rows A B pick, the slot (0..7) MATRIX picks;
                                                   the row (A, B: the LFO; C: MATRIX) */
    uint8_t prog, patch_row;                    /* the patch (0..99) the sound came from; the PATCH page's row */
    uint8_t fx_sel, fx_row;                     /* FX: the slot (0, 1; 2 the EQ), the row */
    char name[12];                              /* its name (the sound being edited keeps it) */
    int8_t morph, tune, trans;                  /* MACRO's morph (-63..63), the tuning (cents), the transpose */
    uint8_t ctl[4];                             /* SETUP: the CCs of sources CTL W..Z (0: none), 2026-11-15 */
    uint8_t mod_mf;                             /* MOD: the modifier (0..3) row D picks */
    patch_t p;
} set;
static const patch_t PATCH_DEFAULT = {          /* a first sound, as plain as it gets (2026-11-16): OSC1 alone on wave
                                                   0 (BASIC's sine), OSC2 and OSC3 silent, all three at the note (before:
                                                   a mid wave, a brighter one 7 cents up, a sub: FACT_OSC); */
#ifdef VOLNA_INIT1                             /* (the host's scenarios, written on the first INIT: tests/run_tests.sh
                                                   builds them with it; tests/init_check.py hears this one) */
    .osc = {{24, 0, 0, 127}, {40, 0, 7, 90}, {12, -12, 0, 70}},
#else
    .osc = {{0, 0, 0, 127}, {0, 0, 0, 0}, {0, 0, 0, 0}},
#endif
                                                /* no noise (white when it comes), no ring; */
    .noise = 0, .ring = 0, .color = 127,
    .bal = {-63, -63, -63, -63, -63},           /* everything into filter 1 */
    .flt = {{SVF_LP, 76, 24, 24, 50, 0, 0},     /* a low pass at 830 Hz, a little resonance, opened by its envelope */
            {SVF_OFF, 127, 0, 0, 0, 0, 0}},     /* (to ~6.8 kHz, settling at ~1.6 kHz), following the keys halfway */
    .route = 0,
    .env = {{2, 80, 100, 70, 63, EM_ADSR, 127, 64, 100},   /* the level: a quick attack, a long decay to a high
                                                   sustain, ~0.2 s;
                                                   all of the velocity (as before VELO came) */
            {0, 72, 40, 70, 0, EM_ADSR, 127, 64, 40},   /* the filters: at once, ~0.3 s down to 40; the same for
                                                   any velocity */
            {20, 60, 64, 60, 0, EM_ADSR, 127, 64, 32},  /* ENV 3, ENV 4: plain, for the matrix */
            {0, 50, 0, 50, 0, EM_ADSR, 127, 50, 0}},
    .volume = 100,
    .arp = {ARP_OFF, AD_UP, AO_PLAYED, 1, ARP_RATE_16TH, 50, 0, AV_EACH, ARP_ON,   /* off: RUN turns it on; */
            0, 16, 64, 120},                    /* no pattern; 120 BPM (16, 64: LENGTH and T.AMT of the patch's own
                                                   pattern, PATTERN OWN; a bank pattern has its own) */
    .lfo = {{LS_SINE, 64, 0, 0, 0, 0},          /* ~1 Hz, ~3 Hz, ~0.3 Hz: free, shared; nothing reads them until */
            {LS_TRI, 80, 0, 0, 0, 0},           /* a slot does */
            {LS_SH, 48, 0, 0, 0, 0}},
    .mod = {{0}},                               /* the matrix empty: the sound as before */
    .glide = 0, .vmode = VM_POLY, .bend = 2,    /* no glide, 4 voices, the bend +-2 semitones */
    .uni = {1, 1, 1}, .udet = {50, 50, 50}, .uspr = {64, 64, 64},   /* no unison (when it comes: ~15 cents, half wide) */
    .fx = {{FX_OFF, 0, {0}}, {FX_OFF, 0, {0}}},   /* no effects */
    .eq = {{0, 0, 0}, {45, 83, 111}, 11},       /* the EQ flat (110 Hz, 988 Hz, 4.98 kHz; Q 0.71) */
    .ost = {AST_DEFAULT, AST_DEFAULT, AST_DEFAULT, AST_DEFAULT, AST_DEFAULT, AST_DEFAULT, AST_DEFAULT, AST_DEFAULT,
            AST_DEFAULT, AST_DEFAULT, AST_DEFAULT, AST_DEFAULT, AST_DEFAULT, AST_DEFAULT, AST_DEFAULT, AST_DEFAULT}};
                                                /* (its own pattern: 16 plain steps) */
/* the patterns' first fill (Volna's own rhythms; a step a character: arp_pattern_text). SHIFT + STEPS brings a
 * pattern back to this */
static const char *const PAT_START[ARP_NPAT] = {
    "xxxxxxxxxxxxxxxx", "x.x.x.x.x.x.x.x.", "x..x..x.x..x..x.", "X.xxX.xxX.xxX.xx",
    "x.xx.x.xx.x.x.xx", "c...c...c...c...", "c..c..c.c..c..c.", "xpxpxpxpxpxpxpxp",
    "fxxxfxxxfxxxfxxx", "bxxxbxxxbxxxbxxx", "x.r.x.r.x.r.x.r.", "X-x.X-x.X-x.X-x.",
    "C.x.x.C.x.x.C.x.", "X.x-gx.X-gx.x-g.", "xxxxxxxxxxxxxxxx", "xxxxxxxxxxxxxxxx"};   /* (14: slides) */

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* unison: where the copies sit, Q8 of the outer ones' detune (-256 .. 256), for 1..7 copies; a little uneven, so
 * that their beats do not line up */
static const int16_t UNI_OFS[UMAX + 1u][UMAX] = {
    {0}, {0}, {-256, 256}, {-256, 0, 256}, {-256, -80, 90, 256}, {-256, -118, 0, 126, 256},
    {-256, -150, -50, 55, 145, 256}, {-256, -170, -84, 0, 80, 166, 256}};
static const uint16_t UNI_GAIN[UMAX + 1u] = {0, 4096, 2896, 2365, 2048, 1832, 1672, 1548};   /* 1 / sqrt(n), Q12 */
static uint32_t cents_mul_q20(int32_t c)        /* 2^(c / 1200), c Q8 cents (+-100 cents): 1 + x + x^2/2, Q20 */
{
    int32_t x = (c * 606) >> 8;                 /* x = c ln2 / 1200, Q20 (|x| <= 60600) */
    uint32_t ax = (uint32_t)(x < 0 ? -x : x);
    return (uint32_t)((int32_t)(1u << 20) + x + (int32_t)((ax * ax) >> 21));
}
static void uni_apply(void)                     /* the patch's unison to the ISR: steps, gains; the copies last */
{
    uint32_t k, c, on = 0, st = 0;
    for (k = 0; k < NOSC; k++) {
        uint32_t nc = set.p.uni[k] >= 1u && set.p.uni[k] <= UMAX ? set.p.uni[k] : 1u, d = set.p.udet[k];
        int32_t cmax = (int32_t)(25600u * d * d / 16129u), g = UNI_GAIN[nc], spr = set.p.uspr[k];   /* (Q8 cents) */
        for (c = 0; c < nc; c++) {
            int32_t pan = nc > 1u ? (((int32_t)(2u * c) - (int32_t)(nc - 1u)) * 256 / (int32_t)(nc - 1u)) * spr / 127 : 0;
            uni_mul[k][c] = cents_mul_q20(cmax * UNI_OFS[nc][c] / 256);
            uni_gl[k][c] = g * (256 - pan) >> 8;   /* (pan -256 .. 256: all left .. all right, the sum kept) */
            uni_gr[k][c] = g * (256 + pan) >> 8;
        }
        uni_n[k] = (uint8_t)nc;
        if (nc > 1u) { on = 1; if (spr) st = 1; }
    }
    uni_st = (uint8_t)st;
    uni_on = (uint8_t)on;
}
/* the effects to the ISR; bpm for a synced delay. A slot whose type changes goes off first, its memory and state
 * cleared, the new settings, the type last */
static uint32_t fx_bpm = 120;
static uint32_t cho_rate(uint32_t r) { return 20u + (r & 127u) * 90u / 127u; }   /* CHORUS's RATE: the LFOs' table
                                                   20 .. 110 (0.03 .. 22 Hz) */
static uint32_t fx_time(const fx_set_t *f)      /* DELAY's time in samples (free: 10 ms .. 0.79 s, squared; synced:
                                                   a note value at fx_bpm, capped at the line) */
{
    uint32_t t;
    if (f->p[2]) t = (uint32_t)ARP_RATE_TICKS[f->p[0] < ARP_NRATE ? f->p[0] : ARP_RATE_16TH] * 110250u / (fx_bpm ? fx_bpm : 120u);
    else t = 441u + (FX_DLEN - 2u - 441u) * f->p[0] * f->p[0] / 16129u;
    return t < FX_DLEN - 2u ? t : FX_DLEN - 2u;
}
static uint32_t fx_rt_ms(uint32_t size)         /* REVERB: about how long to -60 dB (a tank half is 0.358 s, with the
                                                   decay twice in it) */
{
    int32_t g = 16384, d = 4915 + (int32_t)size * 83;
    uint32_t n = 0;
    while (g > 16 && n < 200u) { g = (g * d) >> 14; g = (g * d) >> 14; n++; }
    return n * 358u;
}
static void fx_apply(void)
{
    uint32_t k;
    for (k = 0; k < 2u; k++) {
        const fx_set_t *f = &set.p.fx[k];
        uint32_t ty = f->type < FX_N ? f->type : FX_OFF, m = f->mix;
        if (fxp[k].type != ty) {
            fxp[k].type = FX_OFF;               /* (the ISR leaves it alone from here) */
            memset(fx_mem[k], 0, sizeof fx_mem[k]);
            memset(&fxs[k], 0, sizeof fxs[k]);
        }
        fxp[k].dry = m <= 64u ? 4096 : (int32_t)((127u - m) * 4096u / 63u);
        fxp[k].wet = m >= 64u ? 4096 : (int32_t)(m * 4096u / 64u);
        if (ty == FX_PHASER) {                  /* (its own mix: 127 = dry and line half each, the deepest notches
                                                   at the level of dry; 0 = dry) */
            fxp[k].wet = (int32_t)(m * 2048u / 127u);
            fxp[k].dry = 4096 - fxp[k].wet;
        }
        if (ty == FX_DELAY) {
            fxp[k].a = (int32_t)(fx_time(f) << 8);
            fxp[k].b = f->p[1] * 127;           /* FEEDBK: 127 -> 0.984 */
            fxp[k].c = 150 + (int32_t)(f->p[3] * f->p[3] * 3946u / 16129u);   /* TONE: 127 open */
            fxp[k].d = f->p[4] * 256 / 127;     /* PING */
        } else if (ty == FX_REVERB) {
            fxp[k].a = 4915 + f->p[0] * 83;     /* SIZE: the decay 0.3 .. 0.943 */
            fxp[k].b = f->p[1] * 100;           /* DAMP: 0 .. 0.775 */
            fxp[k].c = f->p[2] * 4410 / 127;    /* PRE: 0 .. 100 ms */
            fxp[k].d = f->p[3] * 256 / 127;     /* WIDTH */
        } else if (ty == FX_DIST) {
            fxp[k].a = DIST_GAIN[f->p[0] & 127u];   /* DRIVE: x 1 .. x 64 */
            fxp[k].b = f->p[2] < DS_N ? f->p[2] : DS_SOFT;
            fxp[k].c = 150 + (int32_t)(f->p[1] * f->p[1] * 3946u / 16129u);   /* TONE: 127 open */
            fxp[k].d = (int32_t)(f->p[3] * f->p[3] * 4096u / 16129u);   /* LEVEL: squared, 127 = the curve's full
                                                   scale, 40 ~ x 0.1 (a note goes in at ~0.08 of it) */
        } else if (ty == FX_CHORUS) {
            uint32_t base, depth;
            if (f->p[2] == CM_FLANGER) {        /* 0.2 .. 10 ms, the depth up to the base (it sweeps to ~0) */
                base = 9u + (441u - 9u) * f->p[5] / 127u;
                depth = base * f->p[1] / 127u;
            } else {                            /* 3 .. 30 ms, +- up to 8 ms */
                base = 132u + (1323u - 132u) * f->p[5] / 127u;
                depth = 353u * f->p[1] / 127u;
            }
            fxp[k].a = (int32_t)(base << 8);
            fxp[k].b = (int32_t)(depth << 8);
            fxp[k].c = ((int32_t)f->p[3] - 64) * 15565 / 63;   /* FEEDBK: +-0.95 */
            fxp[k].d = (int32_t)(LFO_INC[cho_rate(f->p[0])] >> 4);   /* (a step a sample: the LFOs' table / 16) */
            fxp[k].e = (int32_t)(f->p[4] * 16909320u);   /* WIDTH: 127 = half a cycle */
        } else if (ty == FX_PHASER) {
            uint32_t fq = f->p[2] < PH_FLO ? PH_FLO : f->p[2] > PH_FHI ? PH_FHI : f->p[2];
            fxp[k].a = (int32_t)(fq << 8);      /* FREQ: the sweep's centre */
            fxp[k].b = (int32_t)(f->p[1] * 9216u / 127u);   /* DEPTH: up to +-3 octaves (Q8 notes) */
            fxp[k].c = ((int32_t)f->p[3] - 64) * 15565 / 63;   /* FEEDBK: +-0.95 */
            fxp[k].d = (int32_t)(LFO_INC[cho_rate(f->p[0])] >> 4);   /* RATE: as CHORUS's (0.03 .. 22 Hz) */
            fxp[k].e = (int32_t)(f->p[5] * 16909320u);   /* WIDTH: 127 = half a cycle */
            fxp[k].f = 2 * ((int32_t)(f->p[4] % PH_NSTG) + 1);   /* STAGES */
        } else if (ty == FX_ENSEMBLE) {
            fxp[k].a = (int32_t)(f->p[1] * (132u << 16) / 127u);   /* DEPTH: the chorus up to +-3 ms (Q16 samples) */
            fxp[k].b = (int32_t)(f->p[2] * (26u << 16) / 127u);    /* VIB: up to +-0.6 ms */
            fxp[k].c = (int32_t)(LFO_INC[26u + f->p[0] * 49u / 127u] >> 4);   /* RATE: 0.1 .. 2 Hz (the LFOs' table) */
            fxp[k].d = (int32_t)(LFO_INC[81u + f->p[3] * 20u / 127u] >> 4);   /* V.RATE: 3 .. 10 Hz */
            fxp[k].e = (int32_t)(f->p[5] * 5636440u);   /* WIDTH: 127 = a sixth of a cycle (the sides opposite) */
            fxp[k].f = (int32_t)PH_G[2u * (90u + f->p[4] * 42u / 127u)];   /* TONE: 1.5 .. 16.7 kHz (notes 90 .. 132) */
        } else if (ty == FX_LOFI) {
            fxp[k].a = (int32_t)(f->p[3] * (160u << 16) / 127u);   /* WOBBLE: the wow up to +-160 samples (+-1.4 %) */
            fxp[k].b = (int32_t)(f->p[3] * (5u << 16) / 127u);     /* and the flutter up to +-5 (+-0.6 %) */
            fxp[k].c = (int32_t)LF_INC[f->p[0] & 127u];            /* RATE: 44.1 .. 1.1 kHz */
            fxp[k].d = (int32_t)(f->p[1] & 127u);                  /* BITS: 16 .. 2 */
            fxp[k].e = (int32_t)(f->p[4] * 9830u / 127u);          /* NOISE: up to 0.3 of the envelope */
            fxp[k].f = (int32_t)PH_G[2u * (80u + f->p[2] * 52u / 127u)];   /* TONE: 830 Hz .. 16.7 kHz (notes 80 .. 132) */
        } else if (ty == FX_TRIPLE) {
            fxp[k].a = 300 << 8;                /* the chorus: 6.8 ms, +- up to 4 ms (DEPTH) */
            fxp[k].b = (int32_t)((176u * f->p[1] / 127u) << 8);
            fxp[k].c = (int32_t)(f->p[2] * 4096u / 127u);   /* CHO: its share */
            fxp[k].d = (int32_t)(LFO_INC[cho_rate(f->p[0])] >> 4);   /* RATE: as CHORUS's */
            fxp[k].f = f->p[4] ? DIST_GAIN[f->p[4] & 127u] : 0;   /* DRIVE: x 1 .. x 64, as DIST's (a note comes in
                                                   at ~0.08 of full scale) */
            fxp[k].e = 1 + (int32_t)(f->p[3] * f->p[3] * 31u / 16129u)   /* S&H: hold 1 .. 32 samples (squared); */
                     + (int32_t)((65536u / isqrt32(fxp[k].f ? (uint32_t)fxp[k].f : 256u)) << 8);   /* the drive's level
                                                   1 / sqrt(its gain), Q12 (x 64: x 1/8) */
        } else if (ty == FX_FILTER) {
            uint32_t src = f->p[3] < FS_N ? f->p[3] : FS_LFO;
            fxp[k].a = ((int32_t)(f->p[0] & 127u) + 4) << 8;   /* CUTOFF: as the main filter's (13 Hz .. 15.8 kHz) */
            fxp[k].b = ((int32_t)f->p[4] - 64) * (60 << 8) / 63;   /* DEPTH: +-5 octaves */
            if (src == FS_ENV)                  /* RATE: ENV's release 20 ms .. 1 s (Q24 a sample) */
                fxp[k].c = (int32_t)(19022u * LFO_INC[0] / LFO_INC[(f->p[5] & 127u) >> 1]);
            else if (src == FS_SYNC) {          /* a note value at the tempo: a cycle */
                uint32_t per = (uint32_t)ARP_RATE_TICKS[f->p[5] < ARP_NRATE ? f->p[5] : ARP_RATE_16TH] * 110250u
                             / (fx_bpm ? fx_bpm : 120u);
                fxp[k].c = (int32_t)(0xFFFFFFFFu / (per ? per : 1u));
            } else fxp[k].c = (int32_t)(LFO_INC[cho_rate(f->p[5])] >> 4);   /* as CHORUS's (0.03 .. 22 Hz) */
            fxp[k].d = 0x40000000;              /* STEREO: the right side a quarter cycle later */
            fxp[k].e = f->p[1] & 127u;
            fxp[k].f = (int32_t)(FF_TYPE[f->p[2] % FF_NTYPE] | src << 8);
        }
        fxp[k].type = (uint8_t)ty;
    }
}
static void eq_apply(void)                      /* the patch's EQ to the ISR: a whole set, the count odd meanwhile */
{
    const eq_set_t *e = &set.p.eq;
    eq_coef_t c;
    const int32_t *s = (const int32_t *)&c;
    uint32_t b, w;
    c.on = 0;
    for (b = 0; b < EQ_NB; b++) {
        int32_t n = e->note[b] < EQ_NLO[b] ? EQ_NLO[b] : e->note[b] > EQ_NHI[b] ? EQ_NHI[b] : e->note[b];
        eq_band_coef(&c.b[b], b, n << 8, e->gain[b], e->q > EQ_QMAX ? EQ_QMAX : e->q);
        if (e->gain[b]) c.on |= 1 << b;
    }
    eq_seq++;
    for (w = 0; w < EQ_WORDS; w++) eq_box[w] = s[w];
    eq_seq++;
}
static void bend_apply(void)                    /* the bend into each oscillator's step, its own range (0: BEND's):
                                                   every pass, and with the patch (the splash's notes too) */
{
    uint32_t k;
    for (k = 0; k < NOSC; k++) {
        int32_t rg = set.p.obend[k] ? (int32_t)set.p.obend[k] - 25 : (int32_t)set.p.bend;
        uint32_t bq = mod_pitch_q20((bend_pos * rg) >> 5) >> 4;   /* Q8 semitones -> 2^(s/12), Q16 */
        uint64_t m = ((uint64_t)osc[k].mul * bq) >> 16;
        osc[k].mulb = m > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)m;
    }
}
static void sound_apply(void)                   /* the patch (and MORPH, the tuning) to the ISR */
{
    uint32_t k, tmul = pitch_mul_q20(0, set.tune);
    for (k = 0; k < NOSC; k++) {
        const osc_set_t *o = &set.p.osc[k];
        osc[k].target = (uint32_t)clampi((int32_t)o->wave + set.morph, 0, (int32_t)WT_WAVES - 1) << 16;
        osc[k].mul = (uint32_t)(((uint64_t)pitch_mul_q20(o->semi, o->cents) * tmul) >> 20);
        osc[k].tab = wt_table(o->table);
        osc[k].va = o->table == TAB_VA;
        fm_src[k] = set.p.fms[k];               /* FM (the ISR reads it while fm_on says so) */
        fm_amt[k] = set.p.fma[k];
    }
    bend_apply();
    fm_on = set.p.fms[0] || set.p.fms[1] || set.p.fms[2];
    osc_sync = set.p.sync;
    flt_curve[0] = set.p.dcurve[0];             /* (DRIVE 0 too: the matrix may drive it) */
    flt_curve[1] = set.p.dcurve[1];
    for (k = 0; k < 2u; k++) {
        flt_fms[k] = set.p.ffms[k];
        flt_fma[k] = set.p.ffma[k];
        flt_pan[k] = set.p.fpan[k];
    }
    vx_on = (uint8_t)(osc_sync || SVF_ISCOMB(set.p.flt[0].type) || SVF_ISCOMB(set.p.flt[1].type) || flt_curve[0]
                      || flt_curve[1] || set.p.ffms[0] || set.p.ffms[1] || set.p.fpan[0] || set.p.fpan[1]
                      || mod_fx || set.p.wlim);   /* (mod_fx: a slot on F1.PAN or F2.PAN) */
    noise_k = set.p.color >= 127u ? 32768       /* 127: white; 0: dark (a cutoff near 90 Hz), squared between */
            : (int32_t)(400u + (set.p.color + 1u) * (set.p.color + 1u) * 2u);
    {   /* the one-pole keeps k / (2 - k) of white noise's power: the gain sqrt((2 - k) / k) puts it back, so COLOR
           changes the tone, not the loudness (x 12.8 at 0); x 2 more: noise at 127 about as loud as an oscillator */
        uint32_t g_q6 = isqrt32((uint32_t)(65536 - noise_k) * 4096u / (uint32_t)noise_k), split = 0;   /* 64 = 1 */
        int32_t lv[NSRC];
        lv[SRC_O1] = set.p.osc[0].level; lv[SRC_O2] = set.p.osc[1].level; lv[SRC_O3] = set.p.osc[2].level;
        lv[SRC_NOISE] = (int32_t)((set.p.noise * g_q6) >> 5);   /* <= 127 x 816 / 32 = 3238 */
        lv[SRC_RING] = set.p.ring;
        for (k = 0; k < NSRC; k++) {            /* BAL: the level split between the two filters' inputs */
            int32_t w2 = (set.p.bal[k] + 63) * 128 / 126, to2 = (lv[k] * w2) >> 7;   /* w2 0..128 */
            mix_g[0][k] = lv[k] - to2;
            mix_g[1][k] = to2;
            mix_w2[k] = w2;
            if (to2) split = 1;
        }
        mix_lv[SRC_O1] = set.p.osc[0].level; mix_lv[SRC_O2] = set.p.osc[1].level; mix_lv[SRC_O3] = set.p.osc[2].level;
        mix_lv[SRC_NOISE] = set.p.noise; mix_lv[SRC_RING] = set.p.ring;   /* (for the level modulation) */
        noise_comp = (int32_t)g_q6;
        noise_col = set.p.color;
        mix_split = (uint8_t)split;
    }
    for (k = 0; k < 2u; k++) {
        const filt_set_t *q = &set.p.flt[k];
        flt[k].type = q->type;
        flt[k].res = q->res;
        flt[k].base = ((int32_t)q->cut + 4) << 8;   /* 0 -> note 4 (13 Hz), 127 -> note 131 (15.8 kHz) */
        flt[k].env = q->env * 390;              /* 63 -> 96 semitones */
        flt[k].ktrk = ((int32_t)q->ktrk * 256 + 50) / 100;
        flt[k].velo = q->velo * 1573;           /* 63 x 127 x 1573 >> 10: 48 semitones */
        flt[k].drive = q->drive ? 256 + q->drive * 14 : 0;   /* 1x .. 7.9x */
        flt_drv[k] = q->drive;
    }
    flt_par = set.p.route;
    vol_gain = set.p.volume;
    {   /* the matrix: the slots in use, in a row, their amounts in their destinations' units; the sources read (the
           modifiers': all four, when a slot reads one); the LFOs */
        uint32_t n = 0, used = 0, eu = 0, rd = 0, amu = 0, lsp = 0, lfx = 0;
        for (k = 0; k < NMSLOT; k++) {          /* (slots 1..4 a slot's AMT moves: in use, amount 0 too) */
            const mod_slot_t *m = pslot(&set.p, k);
            if (m->src != MS_OFF && m->amt && m->dst >= MD_AMT1 && m->dst <= MD_AMT4) amu |= 1u << (m->dst - MD_AMT1);
        }
        for (k = 0; k < NMSLOT; k++) {
            const mod_slot_t *m = pslot(&set.p, k);
            if (m->src == MS_OFF || m->dst == MD_OFF || (!m->amt && !(k < 4u && (amu >> k & 1u)))) continue;
            mslot[n].src = m->src;
            mslot[n].dst = m->dst;
            mslot[n].idx = (uint8_t)k;
            mslot[n].amt = m->amt;
            mslot[n].scale = mod_scale(m->dst, m->amt);
            rd |= 1u << m->src;
            if (m->dst >= MD_LSPD1 && m->dst <= MD_LSPD3) lsp |= 1u << (m->dst - MD_LSPD1);
            if (m->dst == MD_FPAN1 || m->dst == MD_FPAN2) lfx = 1;
            n++;
        }
        for (k = 0; k < NMODF; k++) {           /* the modifiers (the ISR reads them while mod_xs says so) */
            modf_a[k] = set.p.modf[k].a;
            modf_b[k] = set.p.modf[k].b;
            modf_op[k] = set.p.modf[k].op;
            modf_c[k] = modf_const(set.p.modf[k].c);
        }
        if (rd >> MS_MOD1)                      /* (a modifier read: all four run, their sources read) */
            for (k = 0; k < NMODF; k++) rd |= 1u << set.p.modf[k].a | 1u << set.p.modf[k].b;
        if (rd >> MS_L1MW & 1u) rd |= 1u << MS_LFO1;
        if (rd >> MS_L2PR & 1u) rd |= 1u << MS_LFO2;
        used = rd >> MS_LFO1 & 7u;
        eu = rd >> MS_ENV3 & 3u;
        mod_lfos = used;
        mod_envs = eu;
        mod_xs = rd >> MS_PRESS != 0u;
        mod_lspd = lsp;
        mod_amts = amu;
        mod_fx = lfx;
        mod_n = n;                              /* (last: the ISR reads up to it) */
        for (k = 0; k < NLFO; k++) {
            lfo_shapes[k] = set.p.lfo[k].shape;
            lfo_keys[k] = set.p.lfo[k].key;
            lfo_phases[k] = set.p.lfo[k].phase;
            lfo_fades[k] = ENV_ATK[set.p.lfo[k].delay & 127u];
        }
        mod_macro = set.morph * 520;            /* (+-63 -> +-32760) */
    }
    vel_curve = set.vcurve;
    vel_fix = set.keyvel;
    if (set.p.vmode != vmode) voices_off();     /* (POLY <-> MONO: no voice left hanging) */
    vmode = set.p.vmode < VM_N ? set.p.vmode : VM_POLY;
    glide_gls = set.p.gmode >= GM_GLISS;
    lfo_fout = set.p.lfout;
    env_sgl = set.p.etrig;
    osc_lim = set.p.wlim;
    glide_mode = set.p.gmode;
    for (k = 0; k < NOSC; k++) osc_okt[k] = set.p.okt[k];
    glide_t = set.p.glide;
    uni_apply();
    fx_apply();
    eq_apply();
    for (k = 0; k < NENV; k++) {                /* the envelopes: their rates and modes, their VELO */
        const env_set_t *e = &set.p.env[k];
        env_rates_t *r = k == 0 ? &amp_env : k == 1 ? &flt_env : &xenv_r[k - 2u];
        env_rates(r, e->a, e->d, e->s, e->r);
        env_mode(r, e->mode, e->al, e->d2, e->s2);
        vsens[k] = e->vel * 32768 / 63;
        env_par[k][0] = e->a; env_par[k][1] = e->d; env_par[k][2] = e->s; env_par[k][3] = e->r;
    }
}
/* ---- the patches (stage 6; the format 2 since 2026-11-15): 100 of them, 00..99. A patch is one record, whole: its
 * format's version, the size of patch_t when it was saved (16 bits), the name, the patch. PPS records a sector, in
 * PSEC_N sectors of their own (psec_load / psec_save: the slots' area, store.h). patch_t only grows at its end, so an
 * older (shorter) record loads: what it has over the defaults; a newer (longer) one: as far as this build knows it.
 * A record never saved (or broken) is the factory's sound of that number (patch_factory), else INIT. Saving rewrites
 * the patch's sector (one erase, ~50 ms: the sound must be silent).
 * The format 1 (2026-11-13 .. 15: records of PREC bytes in blobs 0..4, a size byte, patch_t's bytes past PHEAD in the
 * tails, blobs 6 and 7) is moved over once, when no sector of the format 2 is there yet (patch_migrate); the blobs
 * stay as they were */
#define NPATCH 100u
#include "bank.h"                              /* the format 1: PPB, PREC; older banks' layout */
#include "setbox.h"                            /* the settings in a box: they survive an update */
#define PATCH_VER 2u
#define PATCH_NAME 12u
#define PAT_BLOB 5u                             /* the arp's patterns (not the patches': one bank for all) */
#define PREC2 510u                              /* a record: 16 bytes of head and name, then patch_t, up to P2MAX */
#define PPS 8u                                  /* records a sector: 8 x 510 = 4080 of 4084 */
#define P2MAX (PREC2 - 16u)                     /* patch_t's room: 494 bytes (246 now) */
typedef struct { uint8_t ver, pad; uint16_t size; char name[PATCH_NAME]; uint8_t p[P2MAX]; } prec2_t;
typedef char prec2_fits[sizeof(prec2_t) == PREC2 && PPS * PREC2 <= SECTOR_MAX && sizeof(patch_t) <= P2MAX
                        && (NPATCH + PPS - 1u) / PPS <= PSEC_N ? 1 : -1];   /* (patch_t outgrowing it: no build) */
static union { prec2_t r; uint8_t raw[PREC2]; } bank[PPS];   /* one sector's records, read for a load or a save */
static int32_t bank_at = -1;                    /* which sector bank holds (-1: none) */
/* the format 1, for patch_migrate */
#define PATCH_VER1 1u
#define NBLOB1 ((NPATCH + PPB - 1u) / PPB)      /* its records' blobs, 0..4 */
#define TAIL_BLOB 6u                            /* its tails: patch_t's bytes from PHEAD */
#define TAIL2_BLOB 7u                           /* its second tails: from PHEAD + PTAIL */
#define PHEAD (PREC - 16u)
#define PTAIL 40u
typedef struct { uint8_t ver, size; char name[PATCH_NAME]; uint8_t pad[2]; uint8_t p[PHEAD]; } prec_t;
typedef char prec_fits[sizeof(prec_t) <= PREC && PHEAD + 2u * PTAIL <= P2MAX
                       && NBLOB1 * PPB * PREC + 2u * NPATCH * PTAIL <= sizeof cv_px ? 1 : -1];   /* (all of it fits
                                                   the canvas: patch_migrate reads it there) */
static struct {                                 /* immutable legacy bank, used only to normalize old references */
    uint16_t st[ARP_NPAT][ARP_PLEN];            /* the steps (arp.h) */
    uint8_t len[ARP_NPAT], tfac[ARP_NPAT];      /* each one's LENGTH (1..16) and T.AMT (0..127): the pattern's own,
                                                   not the patch's (SEQ edits them; since 2026-11-15) */
} pats;
#define PAT_OWN (ARP_NPAT + 1u)                 /* PATTERN 17, OWN: the patch's own (set.p.ost, arp.plen, arp.tfac): it
                                                   comes and goes with the patch, saved with it */
static uint32_t pat_num(void) { return PAT_OWN; }   /* the current patch's pattern, including SEQ and Undo */
static uint16_t *pat_st(uint32_t n) { return n == PAT_OWN ? set.p.ost : pats.st[(n - 1u) % ARP_NPAT]; }   /* pattern n's
                                                   steps, its LENGTH, its T.AMT (n 1..PAT_OWN) */
static uint8_t *pat_len(uint32_t n) { return n == PAT_OWN ? &set.p.arp.plen : &pats.len[(n - 1u) % ARP_NPAT]; }
static uint8_t *pat_tfac(uint32_t n) { return n == PAT_OWN ? &set.p.arp.tfac : &pats.tfac[(n - 1u) % ARP_NPAT]; }
static struct { uint32_t version, generation; __typeof__(pats) bank; } patlib;
static int32_t patlib_at = -1;
static void pattern_own(patch_t *p)
{
    uint32_t n = p->arp.pattern, i;
    if (p->patver) return;
    p->pat_source = 0;
    if (n && n < PAT_OWN) {
        for (i = 0; i < ARP_PLEN; i++) p->ost[i] = pats.st[n - 1u][i];
        p->arp.plen = pats.len[n - 1u]; p->arp.tfac = pats.tfac[n - 1u];
        p->pat_source = n;
    } else if (!n) {
        for (i = 0; i < ARP_PLEN; i++) p->ost[i] = AST_DEFAULT;
        p->arp.plen = ARP_PLEN; p->arp.tfac = 64;
    }
    p->arp.pattern = PAT_OWN;
    p->patver = 1;
}
static patch_t orig;                            /* the patch as it is stored: set.p against it is "edited" */
static struct { patch_t p; char name[PATCH_NAME]; uint8_t prog, ok; } recall;   /* the edits left behind (RECALL) */

static int patch_ok(const patch_t *p);
static void name_set(char *d, const char *s)    /* a name: up to 12 characters, spaces after */
{
    uint32_t i;
    for (i = 0; i < PATCH_NAME; i++) d[i] = *s ? *s++ : ' ';
}
/* the factory's sounds (Volna's own): number i into p and name. Since 2026-11-15 there are few of them, each worked
 * out with care (ten planned, added one at a time, one after another from 01); every other number is INIT. Their levels side by side:
 * tests/levels_check.py (a chord ~15000, a mono or arp sound's note ~7500, under the output's knee) */
static void patch_factory(uint32_t i, patch_t *p, char *name)
{
    static const osc_set_t FACT_OSC[NOSC] = {{24, 0, 0, 127}, {40, 0, 7, 90}, {12, -12, 0, 70}};   /* the factory
                                                   sounds were made on the first INIT's oscillators: they keep them */
    *p = PATCH_DEFAULT;
    p->volume = 90;
    if (i >= 1u && i <= 6u) memcpy(p->osc, FACT_OSC, sizeof p->osc);
    switch (i) {
    case 1:                                     /* a slow pad: the table drifts, a slight vibrato on osc 2 */
        name_set(name, "SOFT PAD");
        p->cat = PC_PAD;
        p->osc[0].wave = 20; p->osc[1].wave = 30; p->osc[1].semi = 12; p->osc[1].level = 70; p->osc[2].level = 60;
        p->env[0].a = 80; p->env[0].d = 90; p->env[0].s = 110; p->env[0].r = 95;
        p->flt[0].cut = 70; p->flt[0].env = 8;
        p->lfo[0].rate = 30; p->lfo[1].rate = 70;
        p->mod[0].src = MS_LFO1; p->mod[0].dst = MD_WAVE; p->mod[0].amt = 14;
        p->mod[1].src = MS_LFO2; p->mod[1].dst = MD_P2; p->mod[1].amt = 4;
        p->fx[0] = FX_DEF[FX_CHORUS];           /* a chorus, then a long plate */
        p->fx[1] = FX_DEF[FX_REVERB]; p->fx[1].mix = 48; p->fx[1].p[0] = 96;
        p->volume = 51;
        return;
    case 2:                                     /* seven saws, detuned and spread wide; a sine an octave down */
        name_set(name, "SUPERSAW");
        p->cat = PC_POLY;
        p->osc[0].wave = 63; p->uni[0] = 7; p->udet[0] = 64; p->uspr[0] = 110;
        p->osc[1].level = 0; p->osc[2].wave = 0; p->osc[2].level = 60;
        p->env[0].a = 6; p->env[0].d = 90; p->env[0].s = 110; p->env[0].r = 70;
        p->flt[0].cut = 96; p->flt[0].res = 10; p->flt[0].env = 12;
        p->fx[0] = FX_DEF[FX_DELAY]; p->fx[0].p[2] = 1; p->fx[0].p[0] = 4; p->fx[0].p[4] = 90; p->fx[0].mix = 24;
        p->fx[1] = FX_DEF[FX_REVERB]; p->fx[1].mix = 40;   /* a quarter's echo, then the plate */
        p->volume = 60;
        return;
    case 3:                                     /* BASS-1 (the user's, 2026-11-15): three SYNC waves, two and three
                                                   octaves down and one at the note (two copies, wide); mono, the arp
                                                   on its own groove (PATTERN OWN); TUBE, then LO-FI; the EQ lifted.
                                                   As saved but VOLUME 90 -> 50, DIST's LEVEL 58 -> 20: a note
                                                   25 000 -> 11 000 before the limiter, the brightness the same */
        name_set(name, "BASS-1");
        p->cat = PC_BASS;
        p->osc[0].table = 2; p->osc[0].wave = 33; p->osc[0].semi = -36;
        p->osc[1].table = 2; p->osc[1].wave = 34; p->osc[1].semi = -24; p->osc[1].cents = -3; p->osc[1].level = 127;
        p->osc[2].table = 2; p->osc[2].wave = 42; p->osc[2].cents = 4; p->osc[2].level = 102; p->uni[2] = 2;
        p->flt[0].cut = 70; p->flt[0].res = 0; p->flt[0].env = 33; p->flt[0].drive = 48;
        p->env[0].a = 0; p->env[0].d = 38; p->env[0].s = 46; p->env[0].vel = 51;
        p->env[1].d = 26; p->env[1].s = 10; p->env[1].vel = 60;
        p->mod[0].src = MS_LFO1; p->mod[0].dst = MD_W3; p->mod[0].amt = 18;
        p->vmode = VM_MONO;
        p->arp.mode = ARP_ON; p->arp.pattern = PAT_OWN;   /* its own groove (q: p, softer) */
        arp_pattern_text(p->ost, "=pxqxp=pxpXqxp=p");
        p->fx[0].type = FX_DIST; p->fx[0].mix = 47;
        p->fx[0].p[0] = 98; p->fx[0].p[1] = 92; p->fx[0].p[2] = DS_TUBE; p->fx[0].p[3] = 20;
        p->fx[1].type = FX_LOFI; p->fx[1].mix = 67;
        p->fx[1].p[0] = 62; p->fx[1].p[1] = 100; p->fx[1].p[2] = 100; p->fx[1].p[3] = 30; p->fx[1].p[4] = 20;
        p->eq.gain[0] = 6; p->eq.note[0] = 41; p->eq.gain[1] = 5; p->eq.note[1] = 69; p->eq.gain[2] = 7; p->eq.q = 48;
        p->volume = 50;
        return;
    case 4:                                     /* FILT (the user's, 2026-11-15): two filters side by side, LP24 and
                                                   a driven band pass, and LFO 1 swings OSC1 (FM, an octave down, two
                                                   copies) from one to the other while it walks the FM waves; LFO 2
                                                   stirs both cutoffs; a long decay to a low sustain; a slow chorus.
                                                   As saved but VOLUME and OSC2's UNISON (below) */
        name_set(name, "FILT");
        p->cat = PC_ATMO;
        p->osc[0].table = 4; p->osc[0].wave = 39; p->osc[0].semi = -12; p->uni[0] = 2;
        p->osc[1].table = 2; p->osc[1].wave = 22; p->osc[1].semi = -24; p->osc[1].level = 0;   /* (silent: its UNISON
                                                   2 as saved cost ~6 % of the CPU for nothing; 1, the sound the same
                                                   bit for bit) */
        p->osc[2].level = 0;
        p->bal[0] = -48;
        p->flt[0].type = SVF_LP24; p->flt[0].cut = 70; p->flt[0].env = 34; p->flt[0].ktrk = 20; p->flt[0].drive = 12;
        p->flt[1].type = SVF_BP; p->flt[1].cut = 79; p->flt[1].res = 12; p->flt[1].env = 22; p->flt[1].ktrk = 65;
        p->flt[1].drive = 84;
        p->route = 1;                           /* (PAR) */
        p->env[0].a = 0; p->env[0].d = 126; p->env[0].s = 40; p->env[0].r = 30;
        p->env[1].d = 38; p->env[1].s = 16; p->env[1].r = 0; p->env[1].vel = 12;
        p->arp.pattern = 1;
        p->lfo[0].rate = 40; p->lfo[1].shape = LS_SINE;
        p->mod[0].src = MS_LFO1; p->mod[0].dst = MD_W1; p->mod[0].amt = 29;
        p->mod[1].src = MS_LFO1; p->mod[1].dst = MD_BAL1; p->mod[1].amt = 47;
        p->mod[2].src = MS_LFO2; p->mod[2].dst = MD_CUT; p->mod[2].amt = 8;
        p->fx[0].type = FX_CHORUS; p->fx[0].mix = 26;
        p->fx[0].p[0] = 16; p->fx[0].p[1] = 86; p->fx[0].p[2] = CM_CHORUS; p->fx[0].p[3] = 78; p->fx[0].p[4] = 55;
        p->fx[0].p[5] = 42;
        p->volume = 65;                         /* (as saved 90: a chord 20 800, over the limiter's knee; 65: 15 000) */
        return;
    case 5:                                     /* FM EP (2026-11-15): a sine modulated by a sine at the same pitch
                                                   (FM, 1:1); ENV 3 lifts the depth at the strike and lets it fall,
                                                   harder with the velocity: bright, then round; a third sine an
                                                   octave up, quiet, as the tine; a slow chorus and a little room */
        name_set(name, "FM EP");
        p->cat = PC_KEYS;
        p->osc[0].wave = 0; p->osc[0].cents = 0; p->osc[0].level = 127;
        p->osc[1].wave = 0; p->osc[1].cents = 0; p->osc[1].level = 0;   /* (the modulator: silent in the mix) */
        p->osc[2].wave = 0; p->osc[2].semi = 12; p->osc[2].level = 22;
        p->fms[0] = FMS_O2; p->fma[0] = 34;     /* (I ~0.6 when the strike is gone) */
        p->flt[0].cut = 120; p->flt[0].res = 0; p->flt[0].env = 0; p->flt[0].ktrk = 0;
        p->env[0].a = 0; p->env[0].d = 92; p->env[0].s = 30; p->env[0].r = 62; p->env[0].vel = 40;
        p->env[2].a = 0; p->env[2].d = 84; p->env[2].s = 0; p->env[2].r = 50; p->env[2].vel = 50;   /* (~0.44 s
                                                   down to -60 dB) */
        p->mod[0].src = MS_ENV3; p->mod[0].dst = MD_FM1; p->mod[0].amt = 40;   /* (+80 of AMT at the strike: I ~5) */
        p->fx[0] = FX_DEF[FX_CHORUS]; p->fx[0].mix = 34; p->fx[0].p[0] = 30;
        p->fx[1] = FX_DEF[FX_REVERB]; p->fx[1].mix = 22; p->fx[1].p[0] = 50;
        p->volume = 106;                        /* (a chord ~15 000, as the others) */
        return;
    case 6:                                     /* SYNC LEAD (2026-11-15): a sawtooth on OSC2 synced to OSC3 (silent,
                                                   the master): the note's pitch stays OSC3's, OSC2's pitch moves the
                                                   tone; ENV 3 throws it an octave up at the strike and lets it fall,
                                                   the wheel (CC 1) lifts it more; mono, legato with a little glide,
                                                   a dotted echo and some room */
        name_set(name, "SYNC LEAD");
        p->cat = PC_LEAD;
        p->osc[0].level = 0;
        p->osc[1].wave = 63; p->osc[1].cents = 0; p->osc[1].level = 127;
        p->osc[2].wave = 0; p->osc[2].semi = 0; p->osc[2].level = 0;
        p->sync = 1;
        p->flt[0].cut = 100; p->flt[0].res = 18; p->flt[0].env = 16;
        p->env[0].a = 2; p->env[0].d = 90; p->env[0].s = 104; p->env[0].r = 50;
        p->env[2].a = 0; p->env[2].d = 92; p->env[2].s = 10; p->env[2].r = 60; p->env[2].vel = 30;   /* (~0.8 s
                                                   down to -60 dB: the sweep heard) */
        p->mod[0].src = MS_ENV3; p->mod[0].dst = MD_P2; p->mod[0].amt = 63;   /* (+12 semitones at the strike) */
        p->mod[1].src = MS_WHEEL; p->mod[1].dst = MD_P2; p->mod[1].amt = 50;   /* (+7.6 at the top) */
        p->vmode = VM_LEGATO; p->glide = 30;
        p->fx[0] = FX_DEF[FX_DELAY]; p->fx[0].p[2] = 1; p->fx[0].p[0] = 5; p->fx[0].mix = 22;   /* (1/8.) */
        p->fx[1] = FX_DEF[FX_REVERB]; p->fx[1].mix = 24;
        p->volume = 127;                        /* (a note ~6 800: one oscillator, mono) */
        return;
    default:
        name_set(name, "INIT");
        return;
    }
}
static void bank_read(uint32_t k)               /* sector k into bank (one never saved: every record empty) */
{
    if (bank_at == (int32_t)k) return;
    if (psec_load(k, bank, sizeof bank)) memset(bank, 0, sizeof bank);
    bank_at = (int32_t)k;
}
/* patch i into p and name (12 characters) */
static void patch_get(uint32_t i, patch_t *p, char *name)
{
    const prec2_t *r;
    i %= NPATCH;
    bank_read(i / PPS);
    r = &bank[i % PPS].r;
    if (r->ver != PATCH_VER || r->size < 8u || r->size > P2MAX) { patch_factory(i, p, name); pattern_own(p); return; }
    *p = PATCH_DEFAULT;
    memcpy(p, r->p, r->size < sizeof(patch_t) ? r->size : sizeof(patch_t));   /* (an older one: the rest stays the
                                                   default) */
    memcpy(name, r->name, PATCH_NAME);
    if (!patch_ok(p)) patch_factory(i, p, name);
    pattern_own(p);
}
static void patch_name(uint32_t i, char *name)  /* just its name */
{
    patch_t p;
    patch_get(i, &p, name);
}
/* patch p as number i, named name: 0 done (its sector rewritten: silence first) */
static int patch_put(uint32_t i, const patch_t *p, const char *name)
{
    prec2_t *r;
    int rc;
    i %= NPATCH;
    bank_read(i / PPS);
    r = &bank[i % PPS].r;
    r->ver = PATCH_VER;
    r->pad = 0;
    r->size = (uint16_t)sizeof(patch_t);
    memcpy(r->name, name, PATCH_NAME);
    memset(r->p, 0, P2MAX);
    memcpy(r->p, p, sizeof(patch_t));
    rc = psec_save(i / PPS, bank, sizeof bank);
    if (rc) bank_at = -1;                      /* a failed write must not masquerade as saved in the read cache */
    return rc;
}
/* the format 1 into the format 2, once: when no sector of the format 2 is there yet. Its records and tails are read
 * into the canvas (free before the first picture: called before ui_splash), each record put together whole (its
 * bytes as it was saved: the size as it was), a sector saved where one holds a record. A tail never saved: the
 * defaults' bytes (as the format 1 read it). Returns the records moved */
static uint32_t patch_migrate(void)
{
    uint8_t *v1 = (uint8_t *)cv_px, *tl = v1 + NBLOB1 * PPB * PREC;
    uint32_t b, k, i, t, j, moved = 0;
    for (k = 0; k < PSEC_N; k++)
        if (!psec_load(k, bank, sizeof bank)) { bank_at = -1; return 0; }   /* (the format 2 is there: done before) */
    bank_at = -1;
    for (b = 0; b < NBLOB1; b++) {              /* its records (a bank of older, shorter records: spread) */
        uint8_t *d = v1 + b * PPB * PREC;
        if (blob_load(b, d, PPB * PREC)) {
            memset(d, 0, PPB * PREC);
            if (!blob_load(b, d, PPB * PREC_OLD)) bank_spread(d);
        }
    }
    for (t = 0; t < 2u; t++) {                  /* its tails */
        uint8_t *d = tl + t * NPATCH * PTAIL;
        uint32_t at = PHEAD + t * PTAIL;
        if (!blob_load(t ? TAIL2_BLOB : TAIL_BLOB, d, NPATCH * PTAIL)) continue;
        for (i = 0; i < NPATCH; i++)
            for (j = 0; j < PTAIL; j++) d[i * PTAIL + j] = at + j < sizeof(patch_t) ? ((const uint8_t *)&PATCH_DEFAULT)[at + j] : 0u;
    }
    for (k = 0; k * PPS < NPATCH; k++) {
        uint32_t any = 0;
        memset(bank, 0, sizeof bank);
        for (i = k * PPS; i < (k + 1u) * PPS && i < NPATCH; i++) {
            const prec_t *o = (const prec_t *)(v1 + i * PREC);   /* (blob i / PPB, record i % PPB: in a row) */
            prec2_t *r = &bank[i % PPS].r;
            uint32_t sz = o->size;
            if (o->ver != PATCH_VER1 || sz < 8u) continue;
            r->ver = PATCH_VER;
            r->size = (uint16_t)sz;
            memcpy(r->name, o->name, PATCH_NAME);
            memcpy(r->p, o->p, sz < PHEAD ? sz : PHEAD);
            for (t = 0; t < 2u && sz > PHEAD + t * PTAIL; t++) {
                uint32_t at = PHEAD + t * PTAIL;
                memcpy(r->p + at, tl + t * NPATCH * PTAIL + i * PTAIL, sz - at < PTAIL ? sz - at : PTAIL);
            }
            any = 1;
            moved++;
        }
        if (any && psec_save(k, bank, sizeof bank)) break;
    }
    bank_at = -1;
    return moved;
}
#ifdef DINGHY_HOST
/* (the host: the format 1's writer as it was, its size given: tests/migrate's old patches) */
static void v1_put(uint32_t i, const patch_t *p, const char *name, uint32_t sz)
{
    static uint8_t b1[PPB * PREC], t1[NPATCH * PTAIL];
    prec_t *r;
    uint32_t t, j;
    if (blob_load(i / PPB, b1, sizeof b1)) memset(b1, 0, sizeof b1);
    r = (prec_t *)(b1 + (i % PPB) * PREC);
    r->ver = PATCH_VER1;
    r->size = (uint8_t)sz;
    memcpy(r->name, name, PATCH_NAME);
    r->pad[0] = r->pad[1] = 0;
    memset(r->p, 0, PHEAD);
    memcpy(r->p, p, sz < PHEAD ? sz : PHEAD);
    blob_save(i / PPB, b1, sizeof b1);
    for (t = 0; t < 2u; t++) {
        uint32_t at = PHEAD + t * PTAIL, nb = sz > at ? sz - at : 0u;
        if (nb > PTAIL) nb = PTAIL;
        if (blob_load(t ? TAIL2_BLOB : TAIL_BLOB, t1, sizeof t1))
            for (j = 0; j < sizeof t1; j++) t1[j] = at + j % PTAIL < sizeof(patch_t) ? ((const uint8_t *)&PATCH_DEFAULT)[at + j % PTAIL] : 0u;
        memcpy(t1 + i * PTAIL, (const uint8_t *)p + at, nb);
        blob_save(t ? TAIL2_BLOB : TAIL_BLOB, t1, sizeof t1);
    }
}
#endif
static void pat_load(void)                      /* the arp's patterns: saved, or the first fill */
{
    uint32_t k, ok = !blob_load(PAT_BLOB, &pats, sizeof pats), old = 0;
    if (!ok) ok = old = !blob_load(PAT_BLOB, pats.st, sizeof pats.st);   /* (a bank from before 2026-11-15: the steps
                                                   alone; LENGTH 16, T.AMT 64 to them) */
    for (k = 0; ok && k < ARP_NPAT * ARP_PLEN; k++)
        if (pats.st[k / ARP_PLEN][k % ARP_PLEN] > 0x3FFFu) ok = 0;
    for (k = 0; ok && !old && k < ARP_NPAT; k++)
        if (pats.len[k] < 1u || pats.len[k] > ARP_PLEN || pats.tfac[k] > 127u) ok = 0;
    if (!ok)
        for (k = 0; k < ARP_NPAT; k++) arp_pattern_text(pats.st[k], PAT_START[k]);
    if (!ok || old)
        for (k = 0; k < ARP_NPAT; k++) { pats.len[k] = ARP_PLEN; pats.tfac[k] = 64u; }
}
static int patlib_ok(void)
{
    uint32_t i, j;
    if (patlib.version != 1u) return 0;
    for (i = 0; i < ARP_NPAT; i++) {
        if (!patlib.bank.len[i] || patlib.bank.len[i] > ARP_PLEN || patlib.bank.tfac[i] > 127u) return 0;
        for (j = 0; j < ARP_PLEN; j++) if (patlib.bank.st[i][j] > 0x3FFFu) return 0;
    }
    return 1;
}
static void patlib_init(void)
{
    uint32_t i, newest = 0;
    for (i = 0; i < 2u; i++) {
        if (!patlib_load(i, &patlib, sizeof patlib) && patlib_ok()
            && (patlib_at < 0 || (int32_t)(patlib.generation - newest) > 0)) {
            patlib_at = (int32_t)i; newest = patlib.generation;
        }
    }
    if (patlib_at >= 0) { patlib_load((uint32_t)patlib_at, &patlib, sizeof patlib); return; }
    patlib.version = 1; patlib.generation = 0; patlib.bank = pats;
}
static int patlib_store(uint32_t slot, const uint16_t *st, uint8_t len, uint8_t tfac)
{
    __typeof__(patlib) next = patlib;
    __typeof__(patlib) verify;
    uint32_t i, target = patlib_at == 0 ? 1u : 0u;
    next.generation++;
    for (i = 0; i < ARP_PLEN; i++) next.bank.st[slot][i] = st[i];
    next.bank.len[slot] = len; next.bank.tfac[slot] = tfac;
    if (patlib_save(target, &next, sizeof next)) return -1;
    /* Commit the in-memory state only after the new copy was verified. */
    if (patlib_load(target, &verify, sizeof verify) || memcmp(&verify, &next, sizeof next)) return -1;
    patlib = next;
    patlib_at = (int32_t)target;
    return 0;
}
static void settings_default(void)
{
    set.oct = 0; set.ch = 0;
    set.vcurve = VC_LIN; set.keyvel = 100; set.setup_row = 0;
    set.sec = 0; set.osc_sel = 0; set.osc_row = 0; set.mix_row = 0;
    set.filt_sel = 0; set.filt_row = 0; set.env_sel = 0; set.env_row = 0; set.arp_row = 0;
    set.steps_row = 0; set.step_sel = 0; set.mod_lfo = 0; set.mod_slot = 0; set.mod_row = 0;
    set.morph = 0; set.tune = 0; set.trans = 0;
    set.ctl[0] = 16; set.ctl[1] = 17; set.ctl[2] = 18; set.ctl[3] = 19;   /* (General Purpose 1..4) */
    set.mod_mf = 0;
    set.prog = 0; set.patch_row = 0; set.fx_sel = 0; set.fx_row = 0;
    patch_factory(0, &set.p, set.name);
}
/* the settings and the sound being edited, kept in a box (setbox.h): the head (set up to p) and the patch, each
 * loaded as far as it was saved. set's fields before p only grow at their end, as patch_t's do. A box not there: the
 * layout before boxes (2026-11-14), set as a whole, still loads once; else the defaults */
static uint8_t setbox[SETBOX];
#define SET_HEAD ((uint32_t)__builtin_offsetof(__typeof__(set), p))   /* (a constant: the check below holds) */
typedef char setbox_fits[SET_HEAD + sizeof(patch_t) + 8u <= SETBOX ? 1 : -1];
static int settings_ok(void);
static void settings_put(void)
{
    setbox_pack(setbox, &set, SET_HEAD, &set.p, sizeof set.p);
    settings_save(setbox, SETBOX);
}
static void settings_get(void)
{
    settings_default();
    if (!settings_load(setbox, SETBOX) && !setbox_unpack(setbox, &set, SET_HEAD, &set.p, sizeof set.p) && settings_ok())
        return;
    settings_default();
    if (!settings_load(&set, sizeof set) && settings_ok()) return;   /* (saved before the box) */
    settings_default();
}
static int settings_ok(void)
{
    const patch_t *p = &set.p;
    if ((int8_t)set.oct < -2 || (int8_t)set.oct > 2 || set.ch > 16u || set.osc_sel >= NOSC || set.osc_row > 3u || set.mix_row > 2u
        || set.filt_sel > 1u || set.filt_row > 2u || set.env_sel >= NENV || set.env_row > 2u
        || set.morph < -63 || set.morph > 63 || set.tune < -50 || set.tune > 50 || set.trans < -12 || set.trans > 12
        || set.vcurve >= NVC || set.keyvel < 1u || set.keyvel > 127u || set.setup_row > 2u || set.arp_row > 2u
        || set.steps_row > 1u || set.step_sel >= ARP_PLEN || set.mod_lfo >= NLFO || set.mod_slot >= NMSLOT || set.mod_row > 3u
        || set.prog >= NPATCH || set.patch_row > 1u || set.fx_sel > 2u || set.fx_row > 1u || set.mod_mf >= NMODF
        || set.ctl[0] > 119u || set.ctl[1] > 119u || set.ctl[2] > 119u || set.ctl[3] > 119u)
        return 0;
    return patch_ok(p);
}
static int fx_ok(const fx_set_t *f)
{
    uint32_t i;
    if (f->type >= FX_N || f->mix > 127u) return 0;
    for (i = 0; i < 6u; i++) if (f->p[i] > 127u) return 0;
    if (f->type == FX_DIST && f->p[2] >= DS_N) return 0;
    if (f->type == FX_CHORUS && (f->p[2] >= CM_N || f->p[3] < 1u)) return 0;
    if (f->type == FX_PHASER && (f->p[2] < PH_FLO || f->p[2] > PH_FHI || f->p[3] < 1u || f->p[4] >= PH_NSTG)) return 0;
    if (f->type == FX_FILTER && (f->p[2] >= FF_NTYPE || f->p[3] >= FS_N || f->p[4] < 1u
                                 || (f->p[3] == FS_SYNC && f->p[5] >= ARP_NRATE))) return 0;
    return f->type != FX_DELAY || !f->p[2] || f->p[0] < ARP_NRATE;   /* (synced: TIME a note value) */
}
static int patch_ok(const patch_t *p)           /* a patch's values all in their ranges */
{
    uint32_t k;
    if (p->arp.mode >= ARP_NMODE || p->arp.dir >= AD_N || p->arp.order >= AO_N || p->arp.oct < 1u || p->arp.oct > 10u
        || p->arp.rate >= ARP_NRATE || p->arp.gate < 5u || p->arp.gate > 100u || p->arp.swing > 75u
        || p->arp.vel >= AV_N || p->arp.last < ARP_ON || p->arp.last >= ARP_NMODE || p->arp.tempo < 40u
        || p->arp.tempo > 300u || p->arp.pattern > PAT_OWN || p->arp.plen < 1u || p->arp.plen > ARP_PLEN
        || p->arp.tfac > 127u || p->patver > 1u || (p->patver && p->arp.pattern != PAT_OWN)
        || p->pat_source > ARP_NPAT) return 0;
    for (k = 0; k < ARP_PLEN; k++) if (p->ost[k] > 0x3FFFu) return 0;   /* (its own pattern's steps: 14 bits) */
    if (p->eq.q > EQ_QMAX) return 0;
    for (k = 0; k < EQ_NB; k++)
        if (p->eq.gain[k] < -EQ_DBMAX || p->eq.gain[k] > EQ_DBMAX || p->eq.note[k] < EQ_NLO[k] || p->eq.note[k] > EQ_NHI[k])
            return 0;
    for (k = 0; k < NLFO; k++)
        if (p->lfo[k].shape >= LS_N || p->lfo[k].rate > 127u || p->lfo[k].sync > 1u || p->lfo[k].delay > 127u
            || p->lfo[k].key > 1u || p->lfo[k].phase > 127u) return 0;
    for (k = 0; k < NMSLOT; k++) {
        const mod_slot_t *m = pslot((patch_t *)p, k);
        if (m->src >= MS_N || m->dst >= MD_N || m->amt < -63 || m->amt > 63) return 0;
    }
    for (k = 0; k < NOSC; k++) if (p->fms[k] >= FMS_N || p->fma[k] > 127u) return 0;
    if (p->sync > 1u || p->dcurve[0] >= DC_N || p->dcurve[1] >= DC_N) return 0;
    for (k = 0; k < 2u; k++)
        if (p->ffms[k] >= FMS_N || p->ffma[k] > 127u || p->fpan[k] < -63 || p->fpan[k] > 63) return 0;
    if (p->gmode >= GM_N || p->lfout > 7u || p->wlim > 7u || p->etrig > 15u || p->cat >= PC_N) return 0;
    for (k = 0; k < NOSC; k++) if (p->okt[k] > 48 || p->obend[k] > 49u) return 0;
    for (k = 0; k < NMODF; k++)
        if (p->modf[k].a >= MS_N || p->modf[k].b >= MS_N || p->modf[k].op >= MO_N || p->modf[k].c < -64
            || p->modf[k].c > 63) return 0;
    for (k = 0; k < NOSC; k++)
        if (p->osc[k].wave >= WT_WAVES || p->osc[k].semi < -36 || p->osc[k].semi > 36
            || p->osc[k].cents < -50 || p->osc[k].cents > 50 || p->osc[k].level > 127u || p->osc[k].table > 63u) return 0;
    for (k = 0; k < NSRC; k++)
        if (p->bal[k] < -63 || p->bal[k] > 63) return 0;
    for (k = 0; k < 2u; k++) {
        const filt_set_t *q = &p->flt[k];
        if (q->type >= SVF_NTYPE || q->cut > 127u || q->res > 127u || q->env < -63 || q->env > 63 || q->ktrk > 100u
            || q->drive > 127u || q->velo < -63 || q->velo > 63) return 0;
    }
    for (k = 0; k < NENV; k++) {
        const env_set_t *e = &p->env[k];
        if (e->a > 127u || e->d > 127u || e->s > 127u || e->r > 127u || e->vel < -63 || e->vel > 63
            || e->mode >= EM_N || e->al > 127u || e->d2 > 127u || e->s2 > 127u) return 0;
    }
    return p->noise <= 127u && p->ring <= 127u && p->color <= 127u && p->route <= 1u && p->volume <= 127u
        && p->glide <= 127u && p->vmode < VM_N && p->bend <= 24u
        && p->uni[0] >= 1u && p->uni[0] <= UMAX && p->uni[1] >= 1u && p->uni[1] <= UMAX && p->uni[2] >= 1u
        && p->uni[2] <= UMAX && p->udet[0] <= 127u && p->udet[1] <= 127u && p->udet[2] <= 127u && p->uspr[0] <= 127u
        && p->uspr[1] <= 127u && p->uspr[2] <= 127u && fx_ok(&p->fx[0]) && fx_ok(&p->fx[1]);
}

/* ---- the arpeggiator (main loop; and from app_idle while the screen draws: steps keep time meanwhile) */
static arp_t arp;
static uint32_t sus_on, sus_held[4];            /* the sustain pedal down; the notes it holds, a bit each */
static uint32_t arp_ms, arp_acc, arp_step_ms, clk_ms;   /* the last pass; the tempo's remainder; the last step; the
                                                   last MIDI clock */
/* MIDI out of the notes played: the keys' (the arp off) or the arp's; a note off only for a note sent on */
static uint32_t mout_on[4];                     /* the notes sent on, a bit each */
static void midi_note_out(uint32_t on, uint32_t note, uint32_t vel)
{
    uint32_t ch = set.ch ? set.ch - 1u : 0u, b = 1u << (note & 31u);
    if (note > 127u) return;
    if (on) { midi_send(0x90u | ch, note, vel); mout_on[note >> 5] |= b; }
    else if (mout_on[note >> 5] & b) { midi_send(0x80u | ch, note, 0); mout_on[note >> 5] &= ~b; }
}
static void arp_out(const arp_ev_t *ev, uint32_t n)   /* its note events to the voices, and to MIDI out */
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (ev[i].on) {                         /* (a step's glide or slide: ARP_GL_*) */
            play_g(ev[i].note, ev[i].vel, ev[i].glide == ARP_GL_SLIDE ? PG_SLIDE : ev[i].glide ? PG_GLIDE : PG_NONE,
                   ev[i].from);
            if (ev[i].glide == ARP_GL_SLIDE) midi_note_out(0, ev[i].from, 0);   /* (MIDI: the slid-from note ends) */
            midi_note_out(1, ev[i].note, ev[i].vel);
            arp_step_ms = fm1_ms;
        }
        else { stop(ev[i].note); midi_note_out(0, ev[i].note, 0); }
    }
}
static void arp_go(uint32_t dt)                 /* time on by dt (Q8 ticks), what is due plays */
{
    arp_ev_t ev[ARP_EVMAX];
    arp_par_t p = set.p.arp;                    /* (the patch's, with the pattern's own LENGTH and T.AMT) */
    const uint16_t *pat = 0;
    if (p.pattern) { pat = pat_st(p.pattern); p.plen = *pat_len(p.pattern); p.tfac = *pat_tfac(p.pattern); }
    arp_out(ev, arp_advance(&arp, &p, pat, dt, ev, ARP_EVMAX));
}
static int arp_ext(void) { return clk_ms && fm1_ms - clk_ms < 300u; }   /* MIDI clock comes: it sets the time */
static void arp_run(void)                       /* every pass: the internal tempo's time */
{
    uint32_t now = fm1_ms, dt = now - arp_ms;
    arp_ms = now;
    if (set.p.arp.mode == ARP_OFF || arp_ext()) return;
    if (dt > 200u) dt = 200u;                   /* (<= 200 x 300 x 6144: fits) */
    arp_acc += dt * set.p.arp.tempo * (24u * ARP_TICK);   /* ms x BPM x ticks a beat: 60000 per tick of Q8 */
    arp_go(arp_acc / 60000u);
    arp_acc %= 60000u;
}
static void arp_reset(void)                     /* the mode changed: its note ends, nothing held, voices let go */
{
    arp_ev_t ev[ARP_CHORD];
    arp_out(ev, arp_clear(&arp, ev));
    voices_off();
    sus_held[0] = sus_held[1] = sus_held[2] = sus_held[3] = 0;   /* (the pedal's notes: let go with the rest) */
}
/* a note from the keys or MIDI: to the arpeggiator when it is on, else straight to a voice. The sustain pedal (CC 64)
 * holds the notes let go while it is down, and lets them go with it (to the arp too: they stay in its chord) */
static void note_release(uint32_t note)
{
    if (set.p.arp.mode == ARP_OFF) stop(note);
    else arp_key_off(&arp, &set.p.arp, note);
}
static void note_on(uint32_t note, uint32_t vel)
{
    if (note <= 127u) sus_held[note >> 5] &= ~(1u << (note & 31u));   /* (played again: the key holds it now) */
    if (set.p.arp.mode == ARP_OFF) { play(note, vel); return; }
    arp_key_on(&arp, &set.p.arp, note, vel);
    arp_go(0);                                  /* (a first note plays now) */
}
static void note_off(uint32_t note)
{
    if (sus_on && note <= 127u) { sus_held[note >> 5] |= 1u << (note & 31u); return; }
    note_release(note);
}
static void sustain(uint32_t down)              /* CC 64 */
{
    uint32_t n;
    if (down) { sus_on = 1; return; }
    sus_on = 0;
    for (n = 0; n < 128u; n++)
        if (sus_held[n >> 5] >> (n & 31u) & 1u) { sus_held[n >> 5] &= ~(1u << (n & 31u)); note_release(n); }
}

/* ---- a patch into the sound, RECALL, SAVE (main loop) */
static int32_t save_to = -1;                    /* SAVE asked for: the number, waiting for silence (-1: none) */
static char save_name[PATCH_NAME];
static patch_t save_p;                          /* the sound as it was when SAVE was pressed: what gets written, even
                                                   if another patch is loaded while the tail dies out (2026-11-15) */
static uint8_t save_from, save_moved;           /* the number then; another patch loaded (or RECALL) since */
static uint32_t patch_edited(void) { return memcmp(&set.p, &orig, sizeof orig) != 0; }
static void patch_load(uint32_t i)              /* PRESETS: patch i now (the edits left behind: RECALL) */
{
    uint32_t was = set.p.arp.mode;
    if (save_to >= 0) save_moved = 1;           /* (a SAVE waiting: it writes save_p, not this) */
    if (patch_edited()) {
        recall.p = set.p;
        memcpy(recall.name, set.name, PATCH_NAME);
        recall.prog = set.prog;
        recall.ok = 1;
    }
    patch_get(i, &set.p, set.name);
    set.prog = (uint8_t)(i % NPATCH);
    orig = set.p;
    if (set.p.arp.mode != was) arp_reset();     /* (no note of the old arp left hanging) */
    sound_apply();
}
static void patch_recall(void)                  /* RECALL: the edits left behind; with none, the patch as stored */
{
    uint32_t was = set.p.arp.mode;
    char nm[PATCH_NAME];
    if (save_to >= 0) save_moved = 1;
    if (recall.ok) {
        set.p = recall.p;
        memcpy(set.name, recall.name, PATCH_NAME);
        set.prog = recall.prog;
        patch_get(set.prog, &orig, nm);
        recall.ok = 0;
    } else {
        patch_get(set.prog, &set.p, set.name);
        orig = set.p;
    }
    if (set.p.arp.mode != was) arp_reset();
    sound_apply();
}
static void patch_save_ask(uint32_t i, const char *name)   /* SAVE: the notes let go now, the write once silent */
{
    save_to = (int32_t)(i % NPATCH);
    memcpy(save_name, name, PATCH_NAME);
    save_p = set.p;
    save_from = set.prog;
    save_moved = 0;
    arp_reset();
}
static int patch_save_now(void)                 /* (silent): 1 saved, -1 failed, 0 nothing asked */
{
    int rc;
    if (save_to < 0) return 0;
    rc = patch_put((uint32_t)save_to, &save_p, save_name);
    if (!rc && !save_moved) {                   /* still on that sound: it is the saved one now (edits made while
                                                   waiting stay edits against it) */
        set.prog = (uint8_t)save_to;
        memcpy(set.name, save_name, PATCH_NAME);
        orig = save_p;
        recall.ok = 0;
    } else if (!rc) {                           /* moved on meanwhile: */
        if (set.prog == (uint8_t)save_to && !patch_edited()) {   /* to this very number (loaded before the write:
                                                   its old sound): the saved one instead */
            set.p = orig = save_p;
            memcpy(set.name, save_name, PATCH_NAME);
            sound_apply();
        }
        if (recall.ok && recall.prog == save_from && !memcmp(&recall.p, &save_p, sizeof save_p)) {
            recall.prog = (uint8_t)save_to;     /* the saved sound left for RECALL: it brings back the saved
                                                   number, unedited */
            memcpy(recall.name, save_name, PATCH_NAME);
        }
    }
    save_to = -1;
    return rc ? -1 : 1;
}

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
static uint32_t idle_ok, midi_src, clk_n, clk_t0, clk_bpm, clk_run, clk_beat_ms;
static uint32_t midi_act_ms;                    /* the last note or controller on our channel (0: none yet) */
static uint32_t pc_req;                         /* a program change asked for: the patch + 1 (0: none) */

static void midi_in(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {   /* 1 USB, 2 the TRS jack */
        uint32_t ty = st & 0xF0u;
        if (st == 0xF8u) {                      /* clock: 24 a quarter note */
            if (clk_n % 24u == 0u) {
                if (clk_n) {
                    uint32_t ms = fm1_ms - clk_t0;   /* a quarter's length */
                    clk_bpm = ms ? (60000u + ms / 2u) / ms : 0;
                }
                clk_t0 = fm1_ms;
                clk_beat_ms = fm1_ms;
            }
            clk_n++;
            clk_ms = fm1_ms;
            if (set.p.arp.mode != ARP_OFF) arp_go(ARP_TICK);   /* the arpeggiator follows the clock, a tick a clock */
            continue;
        }
        if (st == 0xFAu) {                      /* START: the next clock is beat 1 (the arp's step 0) */
            clk_run = 1; clk_n = 0;
            if (arp.run) { arp.k = 0; arp.grid = arp.next = arp.t; }
            continue;
        }
        if (st == 0xFBu) { clk_run = 1; continue; }              /* CONTINUE */
        if (st == 0xFCu) { clk_run = 0; continue; }              /* STOP */
        if (set.ch && (st & 15u) != set.ch - 1u) continue;      /* not our channel */
        midi_act_ms = fm1_ms ? fm1_ms : 1u;     /* (the footer's MIDI dot) */
        if (ty == 0xB0u) {                      /* the matrix's CC sources (2026-11-15); then what a CC does too */
            uint32_t c;
            for (c = 0; c < 4u; c++) if (set.ctl[c] && d1 == set.ctl[c]) mod_ctl[c] = (int32_t)d2 * 258;
            if (d1 == 2u) mod_breath = (int32_t)d2 * 258;
            if (d1 == 4u) mod_foot = (int32_t)d2 * 258;
            if (d1 == 64u) mod_sust = d2 >= 64u ? 32767 : 0;
            if (d1 == 121u) {                   /* reset all controllers */
                mod_press = mod_breath = mod_foot = mod_wheel = 0;
                for (c = 0; c < 4u; c++) mod_ctl[c] = 0;
                for (c = 0; c < NV; c++) voice[c].ppress = 0;
            }
        }
        if (ty == 0xD0u) { mod_press = (int32_t)d1 * 258; continue; }   /* aftertouch: source PRESS */
        if (ty == 0xA0u) {                      /* poly pressure: P.PRESS of the voices playing that note */
            uint32_t c;
            for (c = 0; c < NV; c++) if (voice[c].note == d1 && voice[c].gate) voice[c].ppress = (int32_t)d2 * 258;
            continue;
        }
        if (ty == 0xB0u && d1 == 1u) { mod_wheel = d2 * 258; continue; }   /* CC 1, the mod wheel: source WHEEL */
        if (ty == 0xB0u && d1 == 64u) { sustain(d2 >= 64u); continue; }    /* CC 64, the sustain pedal */
        if (ty == 0xC0u) { pc_req = d1 % NPATCH + 1u; continue; }         /* program change: the main loop loads it
                                                   (not while the screen draws) */
        if (ty == 0xE0u) { bend_pos = (int32_t)(d1 | d2 << 7) - 8192; continue; }   /* the pitch bend (BEND's range) */
        if (ty == 0x90u && d2) { note_on(d1, d2); midi_src = (uint32_t)src; }
        else if (ty == 0x80u || ty == 0x90u) note_off(d1);
    }
}
static void lfo_tempo(void)                     /* every pass: the LFOs' steps (a synced one follows the tempo) */
{
    uint32_t k, bpm = arp_ext() && clk_bpm ? clk_bpm : set.p.arp.tempo;
    for (k = 0; k < NLFO; k++) lfo_incs[k] = lfo_inc(&set.p.lfo[k], bpm, ARP_RATE_TICKS, ARP_NRATE);
    if (bpm != fx_bpm) { fx_bpm = bpm; fx_apply(); }   /* (a synced delay follows the tempo) */
}
static void app_idle(void)
{
    if (idle_ok) { midi_in(); arp_run(); }
}

#include "ui.h"                                 /* the screen and the panel (docs/ui/UI.md) */

/* ---- the editor (2026-11-17, docs/editor-protocol.md): a frame from USB is kept (ed_take; ota.c hands it over from
 * platform_poll), answered in the main loop (ed_service), where loading a sound is safe. A patch PUT to a number waits
 * for silence, as SAVE does (ed_put_*), and is answered once written */
#include "editor.h"
#ifdef DINGHY_HOST
#define ed_wire_send host_sysex_out             /* (tests/host.c: the replies into a file) */
#else
#define ed_wire_send ota_wire_send
#endif
static uint8_t ed_raw[ED_RAW_MAX];
static uint32_t ed_raw_n;                       /* a frame kept, not yet answered (0: none) */
static ed_req_t ed_q;
static uint8_t ed_out[ED_OUT_MAX - 4u], ed_wire[ED_WIRE_MAX];
static int32_t ed_put_to = -1;                  /* a PUT to a number waiting for silence (-1: none) */
static uint8_t ed_put_seq;
static patch_t ed_put_p;
static char ed_put_name[PATCH_NAME];
static int32_t ed_pat_to = -1;
static uint8_t ed_pat_seq, ed_pat_len, ed_pat_tfac;
static uint16_t ed_pat_st[ARP_PLEN];
static uint8_t ed_save_wait, ed_save_seq, ed_save_slot;
#if UWT_N
/* the protocol's version 3: a user wavetable from the site. TABLE BEGIN (slot, CRC, name) empties the slot and erases
 * it once silent (uwt_now), TABLE DATA writes its bytes in order, TABLE END checks the CRC read back and writes the
 * header: only then is the slot filled. TABLE NAME and TABLE CLEAR rewrite / erase the header, once silent */
enum { UO_NONE, UO_BEGIN, UO_NAME, UO_CLEAR };
static uint8_t uwt_op, uwt_op_slot, uwt_op_seq; /* an erase waiting for silence */
static int32_t uwt_up = -1;                     /* the slot being uploaded, erased (-1: none) */
static uint32_t uwt_up_next, uwt_up_crc;        /* its next byte expected; the CRC its BEGIN gave */
static char uwt_up_name[UWT_NAME];
static int uwt_head_write(uint32_t k, uint32_t crc, const char *name)   /* (the header sector erased) */
{
    uwt_head_t h;
    h.magic = UWT_MAGIC;
    h.bytes = UWT_BYTES;
    h.crc = crc;
    memcpy(h.name, name, UWT_NAME);
    return uwt_prog(k, 0, (const uint8_t *)&h, sizeof h);
}
#endif

static void ed_take(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    if (ed_raw_n || n > sizeof ed_raw) return;  /* (one at a time: the page waits for each answer) */
    for (i = 0; i < n; i++) ed_raw[i] = p[i];
    ed_raw_n = n;
}
static void ed_reply(uint8_t cmd, uint8_t seq, uint8_t status, const uint8_t *p, uint32_t n)
{
    uint32_t k = ed_build(cmd, seq, status, p, n, ed_wire);
    if (k) ed_wire_send(ed_wire, k);
}
static uint32_t ed_record(uint8_t *o, const patch_t *p, const char *name)   /* a patch as the record of the format 2 */
{
    o[0] = PATCH_VER;
    o[1] = 0;
    ed_wr16(o + 2, sizeof(patch_t));
    memcpy(o + 4, name, PATCH_NAME);
    memcpy(o + 16, p, sizeof(patch_t));
    return 16u + sizeof(patch_t);
}
static void ed_name_in(const uint8_t *s, char *name, uint32_t n)   /* what the panel can show: A-Z 0-9 - . spaces */
{
    uint32_t i, k;
    for (i = 0; i < n; i++) {
        char c = (char)s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        for (k = 0; NAME_CH[k] && NAME_CH[k] != c; k++) {}
        name[i] = NAME_CH[k] ? c : ' ';
    }
}
static int ed_unrecord(const uint8_t *r, uint32_t n, patch_t *p, char *name)   /* a record into p and name: 1 good */
{
    uint32_t size;
    if (n < 16u) return 0;
    size = ed_rd16(r + 2);
    if (r[0] != PATCH_VER || size < 8u || size > P2MAX || n != 16u + size) return 0;
    *p = PATCH_DEFAULT;
    memcpy(p, r + 16, size < sizeof(patch_t) ? size : sizeof(patch_t));   /* (as patch_get: older shorter, newer cut) */
    ed_name_in(r + 4, name, PATCH_NAME);
    if (!patch_ok(p)) return 0;
    pattern_own(p);
    return 1;
}
/* ---- the protocol's version 2: the panel's parameters (PARAM, pget, pset, the pages), each with the instance it is
 * of (an oscillator, a filter, an envelope, an LFO, a matrix slot, a modifier, an effect slot, a step); the editor
 * turns them as a knob does. What the panel changes reaches the page as CHANGED while it keeps WATCH on */
enum { EK_NONE, EK_OSC, EK_FILT, EK_ENV, EK_LFO, EK_SLOT, EK_MODF, EK_FX, EK_STEP, EK_N };
static const uint8_t EK_COUNT[EK_N] = {1, NOSC, 2, NENV, NLFO, NMSLOT, NMODF, 2, ARP_PLEN};
static uint8_t *ek_sel(uint32_t k)              /* the panel's choice of that instance */
{
    switch (k) {
    case EK_OSC: return &set.osc_sel;
    case EK_FILT: return &set.filt_sel;
    case EK_ENV: return &set.env_sel;
    case EK_LFO: return &set.mod_lfo;
    case EK_SLOT: return &set.mod_slot;
    case EK_MODF: return &set.mod_mf;
    case EK_FX: return &set.fx_sel;
    case EK_STEP: return &set.step_sel;
    default: return 0;
    }
}
static uint32_t ed_kind(uint32_t id)            /* whose parameter it is (as pget and pset read the panel's choice) */
{
    switch (id) {
    case P_WAVE: case P_SEMI: case P_FINE: case P_TABLE: case P_UNI: case P_UDET: case P_USPR: case P_OKT: case P_FMS:
    case P_FMA: case P_OBEND: case P_WLIM: return EK_OSC;
    case P_CUT: case P_RES: case P_FENV: case P_KTRK: case P_FTYPE: case P_DRIVE: case P_FVEL: case P_DCURVE: case P_FFMS:
    case P_FFMA: case P_FPAN: return EK_FILT;
    case P_ATK: case P_DEC: case P_SUS: case P_REL: case P_EVEL: case P_EMODE: case P_EALVL: case P_EDEC2: case P_ESUS2:
    case P_ETRIG: return EK_ENV;
    case P_LSHAPE: case P_LRATE: case P_LSYNC: case P_LDELAY: case P_LKEY: case P_LPHASE: case P_LFADE: return EK_LFO;
    case P_MSRC: case P_MDST: case P_MGRP: case P_MAMT: return EK_SLOT;
    case P_FA: case P_FOP: case P_FB: case P_FC: return EK_MODF;
    case P_STYPE: case P_SACC: case P_SLEN: case P_STIM: case P_SGLIDE: return EK_STEP;
    default: return (id >= P_FXTYPE && id <= P_CDLY) || (id >= P_PRATE && id <= P_WRATE) ? EK_FX : EK_NONE;
    }
}
static uint32_t ed_global(uint32_t id)          /* a setting, not the patch's */
{
    return id == P_MORPH || id == P_MIDI || id == P_VCURVE || id == P_KEYVEL || id == P_TUNE || id == P_TRANS
        || (id >= P_CTLW && id <= P_CTLZ);
}
static uint8_t ed_was;                          /* the panel's choice, while the editor borrows it */
static uint8_t *ed_borrow(uint32_t id, uint32_t inst)   /* the choice set to inst for pget / pset (0: inst too big) */
{
    uint32_t k = ed_kind(id);
    uint8_t *s = ek_sel(k);
    if (inst >= EK_COUNT[k]) return 0;
    if (s) { ed_was = *s; *s = (uint8_t)inst; }
    return s ? s : &ed_was;
}
static void ed_give_back(uint32_t id, uint8_t *s)
{
    if (s != &ed_was) *s = ed_was;
    (void)id;
}
/* a parameter's value in its instance, as PVAL and PSET answer: inst id value(16) hi(16) choices text */
static uint32_t ed_pval(uint32_t inst, uint32_t id, uint8_t *o)
{
    char *e;
    uint32_t nn = 0;
    int32_t v = pget(id);
    o[0] = (uint8_t)inst;
    o[1] = (uint8_t)id;
    ed_wr16(o + 2, (uint32_t)v);
    ed_wr16(o + 4, (uint32_t)param_hi(id));
    param_names(id, &nn);
    o[6] = (uint8_t)nn;
    e = pval_text(id, v, (char *)o + 7);
    return (uint32_t)((uint8_t *)e - o);
}
static patch_t ed_shadow;                       /* the sound as the page last knew it (WATCH: a change goes out) */
static uint8_t ed_shadow_g[16], ed_shadow_prog;
static uint32_t ed_watch_t;                     /* WATCH asked for, last (0: not watching); it lapses after 10 s */
static void ed_globals(uint8_t *g)              /* the settings the editor shows, side by side */
{
    uint32_t i;
    g[0] = set.ch; g[1] = set.vcurve; g[2] = set.keyvel; g[3] = (uint8_t)set.tune; g[4] = (uint8_t)set.trans;
    g[5] = (uint8_t)set.morph;
    for (i = 0; i < 4u; i++) g[6u + i] = set.ctl[i];
    for (i = 10; i < 16u; i++) g[i] = 0;
}
static void ed_known(void)                      /* the page knows the sound as it is now (its own edit) */
{
    ed_shadow = set.p;
    ed_globals(ed_shadow_g);
    ed_shadow_prog = set.prog;
}
/* (main loop) the sound or a setting changed on the panel, the page watching: CHANGED, at most every 100 ms */
static void ed_notify(void)
{
    static uint32_t t;
    uint8_t g[16], o[4];
    if (!ed_watch_t || fm1_ms - t < 100u) return;
    if (fm1_ms - ed_watch_t > 10000u) { ed_watch_t = 0; return; }
    t = fm1_ms;
    ed_globals(g);
    if (!memcmp(&ed_shadow, &set.p, sizeof(patch_t)) && !memcmp(g, ed_shadow_g, sizeof g) && ed_shadow_prog == set.prog)
        return;
    o[0] = set.prog;
    o[1] = (uint8_t)(patch_edited() != 0);
    o[2] = (uint8_t)(ed_shadow_prog != set.prog);   /* (another patch: everything to read again) */
    ed_known();
    ed_reply(ED_CHANGED & 0x7Fu, 0, ED_OK, o, 3);
}

/* the kept frame answered: 1 the sound changed (the screen to redraw) */
static int ed_service(void)
{
    uint32_t i, n;
    uint8_t slot;
    if (!ed_raw_n) return 0;
    n = ed_raw_n;
    ed_raw_n = 0;
    if (!ed_parse(ed_raw, n, &ed_q)) return 0;  /* (broken: no answer, the page asks again) */
    switch (ed_q.cmd) {
    case ED_SAVE:
        if (ed_q.n) break;
        if (save_to >= 0) { ed_reply(ed_q.cmd, ed_q.seq, ED_BUSY, 0, 0); return 0; }
        ed_save_wait = 1; ed_save_seq = ed_q.seq; ed_save_slot = set.prog;
        patch_save_ask(set.prog, set.name);
        return 0;
    case ED_PATINFO:
        if (ed_q.n) break;
        ed_out[0] = ARP_NPAT; ed_out[1] = ARP_PLEN; ed_out[2] = 1;
        ed_reply(ed_q.cmd, ed_q.seq, ED_OK, ed_out, 3); return 0;
    case ED_PATGET: {
        const uint16_t *st;
        if ((ed_q.n != 1u && ed_q.n != 2u) || (ed_q.p[0] >= ARP_NPAT && ed_q.p[0] != ED_SOUND)
            || (ed_q.n == 2u && (ed_q.p[0] != ED_SOUND || ed_q.p[1] != 1u))) break;
        slot = ed_q.p[0];
        ed_out[0] = slot; ed_out[1] = 1;
        ed_out[2] = slot == ED_SOUND ? set.p.arp.plen : patlib.bank.len[slot];
        ed_out[3] = slot == ED_SOUND ? set.p.arp.tfac : patlib.bank.tfac[slot];
        st = slot == ED_SOUND ? set.p.ost : patlib.bank.st[slot];
        for (i = 0; i < ARP_PLEN; i++) ed_wr16(ed_out + 4u + i * 2u, st[i]);
        if (ed_q.n == 2u) ed_out[36] = (uint8_t)set.p.pat_source;
        ed_reply(ed_q.cmd, ed_q.seq, ED_OK, ed_out, ed_q.n == 2u ? 37u : 36u); return 0;
    }
    case ED_PATPUT:
        if ((ed_q.n != 36u && ed_q.n != 37u) || (ed_q.p[0] >= ARP_NPAT && ed_q.p[0] != ED_SOUND)
            || (ed_q.n == 37u && (ed_q.p[0] != ED_SOUND || ed_q.p[36] > ARP_NPAT))
            || ed_q.p[1] != 1 || !ed_q.p[2] || ed_q.p[2] > ARP_PLEN || ed_q.p[3] > 127u) break;
        for (i = 0; i < ARP_PLEN; i++) if (ed_rd16(ed_q.p + 4u + i * 2u) > 0x3FFFu) break;
        if (i != ARP_PLEN) break;
        if (ed_pat_to >= 0) { ed_reply(ed_q.cmd, ed_q.seq, ED_BUSY, 0, 0); return 0; }
        slot = ed_q.p[0];
        if (slot == ED_SOUND) {
            for (i = 0; i < ARP_PLEN; i++) set.p.ost[i] = (uint16_t)ed_rd16(ed_q.p + 4u + i * 2u);
            set.p.arp.plen = ed_q.p[2]; set.p.arp.tfac = ed_q.p[3];
            set.p.arp.pattern = PAT_OWN; set.p.patver = 1;
            set.p.pat_source = ed_q.n == 37u ? ed_q.p[36] : 0u;
            sound_apply(); ed_known();
            ed_reply(ed_q.cmd, ed_q.seq, ED_OK, &slot, 1); return 1;
        }
        if (!quiet() || arp.run) { ed_reply(ed_q.cmd, ed_q.seq, ED_BUSY, 0, 0); return 0; }
        for (i = 0; i < ARP_PLEN; i++) ed_pat_st[i] = (uint16_t)ed_rd16(ed_q.p + 4u + i * 2u);
        ed_pat_len = ed_q.p[2]; ed_pat_tfac = ed_q.p[3]; ed_pat_to = slot; ed_pat_seq = ed_q.seq;
        return 0;
    case ED_PDESC: {
        uint32_t first, count;
        if (ed_q.n != 2u || !(count = ed_q.p[1]) || count > 16u || (first = ed_q.p[0]) + count > NPARAM) break;
        ed_out[0] = (uint8_t)first;
        ed_out[1] = (uint8_t)count;
        for (i = 0; i < count; i++) {
            const param_t *pr = &PARAM[first + i];
            uint8_t *o = ed_out + 2 + i * 16u;
            uint32_t k;
            for (k = 0; k < 8u; k++) o[k] = 0;
            for (k = 0; k < 8u && pr->label[k]; k++) o[k] = (uint8_t)pr->label[k];
            ed_wr16(o + 8, (uint32_t)(int32_t)pr->lo);
            ed_wr16(o + 10, (uint32_t)(int32_t)pr->hi);
            o[12] = pr->step;
            o[13] = pr->fmt;
            o[14] = (uint8_t)ed_kind(first + i);
            o[15] = (uint8_t)ed_global(first + i);
        }
        ed_reply(ED_PDESC, ed_q.seq, ED_OK, ed_out, 2u + count * 16u);
        return 0;
    }
    case ED_PVAL: case ED_PSET: {
        uint32_t inst, id;
        uint8_t *s;
        int changed = 0;
        if (ed_q.n != (ed_q.cmd == ED_PSET ? 4u : 2u) || (id = ed_q.p[1]) == P_NONE || id >= NPARAM) break;
        inst = ed_q.p[0];
        if (!(s = ed_borrow(id, inst))) break;
        if (ed_q.cmd == ED_PSET) {              /* as a knob: clamped, then the sound */
            int32_t v = clampi((int16_t)ed_rd16(ed_q.p + 2), PARAM[id].lo, param_hi(id));
            if (v != pget(id)) { pset(id, v); sound_apply(); changed = 1; }
        }
        n = ed_pval(inst, id, ed_out);
        ed_give_back(id, s);
        if (changed) ed_known();
        ed_reply(ed_q.cmd, ed_q.seq, ED_OK, ed_out, n);
        return changed;
    }
    case ED_PCHOICES: {
        uint32_t inst, id, first, count, nn = 0, k;
        const char *const *names;
        uint8_t *s, *o = ed_out + 4;
        if (ed_q.n != 4u || (id = ed_q.p[1]) == P_NONE || id >= NPARAM) break;
        inst = ed_q.p[0]; first = ed_q.p[2]; count = ed_q.p[3];
        if (!(s = ed_borrow(id, inst))) break;
        names = param_names(id, &nn);
        if (!count || count > 16u || first + count > nn) { ed_give_back(id, s); break; }
        for (i = 0; i < count; i++) {           /* each zero ended */
            for (k = 0; names[first + i][k] && k < 15u; k++) *o++ = (uint8_t)names[first + i][k];
            *o++ = 0;
        }
        ed_give_back(id, s);
        ed_out[0] = (uint8_t)inst; ed_out[1] = (uint8_t)id; ed_out[2] = (uint8_t)first; ed_out[3] = (uint8_t)count;
        ed_reply(ED_PCHOICES, ed_q.seq, ED_OK, ed_out, (uint32_t)(o - ed_out));
        return 0;
    }
    case ED_LAYOUT: {                           /* a section's rows, as its instance inst shows them */
        uint32_t sec, inst, rows, r, k, kind;
        uint8_t *sel = 0, was = 0;
        if (ed_q.n != 2u || (sec = ed_q.p[0]) >= NSEC) break;
        inst = ed_q.p[1];
        kind = sec == SEC_OSC ? EK_OSC : sec == SEC_FILT ? EK_FILT : sec == SEC_ENV ? EK_ENV : sec == SEC_FX ? EK_FX
             : sec == SEC_STEPS ? EK_STEP : EK_NONE;
        if (sec == SEC_FX && inst == FX_EQ) sel = &set.fx_sel;   /* (FX's third: the EQ) */
        else if (inst >= EK_COUNT[kind]) break;
        else sel = ek_sel(kind);
        if (sel) { was = *sel; *sel = (uint8_t)inst; }
        rows = sec_rows(sec);
        ed_out[0] = (uint8_t)sec; ed_out[1] = (uint8_t)inst; ed_out[2] = (uint8_t)kind; ed_out[3] = (uint8_t)rows;
        for (k = 0; k < 8u; k++) ed_out[4u + k] = 0;
        for (k = 0; k < 8u && SEC_NAME[sec][k]; k++) ed_out[4u + k] = (uint8_t)SEC_NAME[sec][k];
        for (r = 0; r < rows; r++)
            for (k = 0; k < 4u; k++) ed_out[12u + r * 4u + k] = (uint8_t)page_param(sec, r, k);
        if (sel) *sel = was;
        ed_reply(ED_LAYOUT, ed_q.seq, ED_OK, ed_out, 12u + rows * 4u);
        return 0;
    }
    case ED_WATCH:
        if (ed_q.n != 1u) break;
        if (ed_q.p[0]) { if (!ed_watch_t) ed_known(); ed_watch_t = fm1_ms ? fm1_ms : 1u; }
        else ed_watch_t = 0;
        ed_reply(ED_WATCH, ed_q.seq, ED_OK, 0, 0);
        return 0;
    case ED_INFO: {
        static const char NM[] = APP_NAME, VER[] = APP_VER;
        ed_out[0] = ED_PROTO;
        ed_out[1] = NPATCH;
        ed_wr16(ed_out + 2, sizeof(patch_t));
        ed_wr16(ed_out + 4, P2MAX);
        ed_out[6] = set.prog;
        ed_out[7] = (uint8_t)(patch_edited() != 0);
        for (i = 0; i < 8u; i++) {              /* (each zero padded, cut at 8) */
            ed_out[8u + i] = i < sizeof NM - 1u ? (uint8_t)NM[i] : 0u;
            ed_out[16u + i] = i < sizeof VER - 1u ? (uint8_t)VER[i] : 0u;
        }
        ed_out[24] = NPARAM;                    /* (version 2) */
        ed_out[25] = NSEC;
        ed_reply(ED_INFO, ed_q.seq, ED_OK, ed_out, 26);
        return 0;
    }
    case ED_NAMES: {
        uint32_t first, count;
        if (ed_q.n != 2u || !(count = ed_q.p[1]) || count > 25u || (first = ed_q.p[0]) + count > NPATCH) break;
        ed_out[0] = (uint8_t)first;
        ed_out[1] = (uint8_t)count;
        for (i = 0; i < count; i++) patch_name(first + i, (char *)ed_out + 2 + i * PATCH_NAME);
        ed_reply(ED_NAMES, ed_q.seq, ED_OK, ed_out, 2u + count * PATCH_NAME);
        return 0;
    }
    case ED_GET: {
        static patch_t p;
        char nm[PATCH_NAME];
        if (ed_q.n != 1u || ((slot = ed_q.p[0]) >= NPATCH && slot != ED_SOUND)) break;
        if (slot == ED_SOUND) { p = set.p; memcpy(nm, set.name, PATCH_NAME); }
        else patch_get(slot, &p, nm);
        ed_out[0] = slot;
        ed_reply(ED_GET, ed_q.seq, ED_OK, ed_out, 1u + ed_record(ed_out + 1, &p, nm));
        return 0;
    }
    case ED_PUT: {
        static patch_t p;
        char nm[PATCH_NAME];
        if (ed_q.n < 1u || ((slot = ed_q.p[0]) >= NPATCH && slot != ED_SOUND)) break;
        if (!ed_unrecord(ed_q.p + 1, ed_q.n - 1u, &p, nm)) {
            ed_reply(ED_PUT, ed_q.seq, ED_BADPATCH, &slot, 1);
            return 0;
        }
        if (slot == ED_SOUND) {                 /* into the sound: edited against its number, as by the knobs */
            uint32_t was = set.p.arp.mode;
            if (save_to >= 0) save_moved = 1;
            set.p = p;
            memcpy(set.name, nm, PATCH_NAME);
            if (set.p.arp.mode != was) arp_reset();
            sound_apply();
            ed_known();
            ed_reply(ED_PUT, ed_q.seq, ED_OK, &slot, 1);
            return 1;
        }
        if (ed_put_to >= 0 || save_to >= 0) {
            ed_reply(ED_PUT, ed_q.seq, ED_BUSY, &slot, 1);
            return 0;
        }
        ed_put_to = slot;                       /* written once silent (ed_put_now) */
        ed_put_seq = ed_q.seq;
        ed_put_p = p;
        memcpy(ed_put_name, nm, PATCH_NAME);
        return 0;
    }
#if UWT_N
    case ED_TINFO: {                            /* the slots: count, first number, a table's bytes, DATA's chunk; then
                                                   each: filled, CRC, name */
        ed_out[0] = UWT_N;
        ed_out[1] = TAB_U0;
        ed_wr16(ed_out + 2, UWT_BYTES & 0xFFFFu);
        ed_wr16(ed_out + 4, UWT_BYTES >> 16);
        ed_wr16(ed_out + 6, ED_TCHUNK);
        for (i = 0; i < UWT_N; i++) {
            uint8_t *o = ed_out + 8u + i * 17u;
            uint32_t c = uwt_ok[i] ? uwt_crc_of[i] : 0u;
            o[0] = uwt_ok[i];
            ed_wr16(o + 1, c & 0xFFFFu);
            ed_wr16(o + 3, c >> 16);
            memcpy(o + 5, uwt_nm[i], UWT_NAME);
        }
        ed_reply(ED_TINFO, ed_q.seq, ED_OK, ed_out, 8u + UWT_N * 17u);
        return 0;
    }
    case ED_TBEGIN: case ED_TNAME: case ED_TCLEAR: {   /* (an erase: answered once done, in silence) */
        uint32_t want = ed_q.cmd == ED_TBEGIN ? 17u : ed_q.cmd == ED_TNAME ? 13u : 1u;
        if (ed_q.n != want || (slot = ed_q.p[0]) >= UWT_N) break;
        if (uwt_op || ed_put_to >= 0 || save_to >= 0 || (ed_q.cmd == ED_TNAME && !uwt_ok[slot])) {
            ed_reply(ed_q.cmd, ed_q.seq, ED_BUSY, &slot, 1);
            return 0;
        }
        if (ed_q.cmd == ED_TBEGIN) {
            uwt_up_crc = (uint32_t)ed_rd16(ed_q.p + 1) | (uint32_t)ed_rd16(ed_q.p + 3) << 16;
            ed_name_in(ed_q.p + 5, uwt_up_name, UWT_NAME);
        } else if (ed_q.cmd == ED_TNAME) ed_name_in(ed_q.p + 1, uwt_up_name, UWT_NAME);
        if (uwt_up == (int32_t)slot) uwt_up = -1;   /* (a new BEGIN on it: from the start) */
        if (ed_q.cmd != ED_TNAME && uwt_ok[slot]) {  /* empty from now: the voices on it play BASIC */
            uwt_ok[slot] = 0;
            sound_apply();
        }
        uwt_op = ed_q.cmd == ED_TBEGIN ? UO_BEGIN : ed_q.cmd == ED_TNAME ? UO_NAME : UO_CLEAR;
        uwt_op_slot = slot;
        uwt_op_seq = ed_q.seq;
        return 1;
    }
    case ED_TDATA: {                            /* slot, offset (4), the bytes: in order, after BEGIN */
        uint32_t off, len;
        uint8_t o[5];
        if (ed_q.n < 6u || (slot = ed_q.p[0]) >= UWT_N) break;
        off = (uint32_t)ed_rd16(ed_q.p + 1) | (uint32_t)ed_rd16(ed_q.p + 3) << 16;
        len = ed_q.n - 5u;
        if (uwt_up != (int32_t)slot || off != uwt_up_next || len > ED_TCHUNK || len > UWT_BYTES - off) break;
        o[0] = slot;
        ed_wr16(o + 1, off & 0xFFFFu);
        ed_wr16(o + 3, off >> 16);
        if (uwt_prog(slot, UWT_DATA + off, ed_q.p + 5, len)) {
            uwt_up = -1;
            ed_reply(ED_TDATA, ed_q.seq, ED_FLASH, o, 5);
            return 0;
        }
        uwt_up_next = off + len;
        ed_reply(ED_TDATA, ed_q.seq, ED_OK, o, 5);
        return 0;
    }
    case ED_TEND: {                             /* slot: all its bytes in? the CRC read back, then the header */
        uint8_t st = ED_OK;
        if (ed_q.n != 1u || (slot = ed_q.p[0]) >= UWT_N || uwt_up != (int32_t)slot || uwt_up_next != UWT_BYTES) break;
        uwt_up = -1;
        if (uwt_crc32(0, uwt_xip(slot) + UWT_DATA, UWT_BYTES) != uwt_up_crc) st = ED_BADPATCH;
        else if (uwt_head_write(slot, uwt_up_crc, uwt_up_name)) st = ED_FLASH;
        else {
            uwt_scan(slot);
            if (!uwt_ok[slot]) st = ED_FLASH;
        }
        sound_apply();                          /* (the patches on that slot play it now) */
        ed_reply(ED_TEND, ed_q.seq, st, &slot, 1);
        ui_msg(st == ED_OK ? "TABLE RECEIVED" : "TABLE FAILED", (int32_t)slot + 1, 1);   /* (USER 1..) */
        return 1;
    }
#endif
    default:
        ed_reply(ed_q.cmd, ed_q.seq, ED_UNKNOWN, 0, 0);
        return 0;
    }
    ed_reply(ed_q.cmd, ed_q.seq, ED_BADREQ, 0, 0);
    return 0;
}
#if UWT_N
/* (silent) the TABLE erase waiting: BEGIN empties the whole slot, NAME rewrites the header, CLEAR erases it. 1 done */
static int uwt_now(void)
{
    uint8_t slot = uwt_op_slot, cmd, st = ED_OK;
    uint32_t k = slot;
    if (!uwt_op) return 0;
    cmd = uwt_op == UO_BEGIN ? ED_TBEGIN : uwt_op == UO_NAME ? ED_TNAME : ED_TCLEAR;
    if (uwt_op == UO_BEGIN) {
        if (uwt_erase(k, 0, UWT_SECT)) st = ED_FLASH;
        else { uwt_up = slot; uwt_up_next = 0; }
    } else if (uwt_op == UO_NAME) {
        uint32_t crc = uwt_crc_of[k];
        if (!uwt_ok[k]) st = ED_BADREQ;         /* (emptied meanwhile) */
        else if (uwt_erase(k, 0, 1) || uwt_head_write(k, crc, uwt_up_name)) st = ED_FLASH;
        uwt_scan(k);
    } else {
        if (uwt_erase(k, 0, 1)) st = ED_FLASH;
        uwt_scan(k);
    }
    uwt_op = UO_NONE;
    sound_apply();
    ed_reply(cmd, uwt_op_seq, st, &slot, 1);
    if (cmd != ED_TBEGIN || st != ED_OK)
        ui_msg(st != ED_OK ? "TABLE FAILED" : cmd == ED_TNAME ? "TABLE RENAMED" : "TABLE CLEARED", (int32_t)slot + 1, 1);
    return 1;
}
#endif
/* (silent) the PUT waiting written and answered: 1 done, 0 nothing waited */
static int ed_put_now(void)
{
    uint8_t slot;
    int rc, reload;
    if (ed_put_to < 0) return 0;
    slot = (uint8_t)ed_put_to;
    reload = set.prog == slot && !patch_edited();   /* (that number playing as stored: it plays the new one) */
    rc = patch_put(slot, &ed_put_p, ed_put_name);
    if (!rc && reload) patch_load(slot);
    ed_reply(ED_PUT, ed_put_seq, rc ? ED_FLASH : ED_OK, &slot, 1);
    ui_msg(rc ? "WRITE FAILED" : "RECEIVED", slot, !rc);
    ed_put_to = -1;
    return 1;
}

#ifdef DINGHY_HOST
/* (tests/migrate: patches of the format 1, written by its writer as it was, then patch_migrate, then each loaded back:
 * as saved (246 bytes: patch_t when the format 1 ended; a record from before the modifiers, 224) with the defaults
 * past its size; a second pass moves none) */
static const uint8_t V1_AT[] = {0, 7, 8, 19, 20, 55, 99};
#define V1_MAX 246u                             /* (the format 1's last patch_t: what its records held at most) */
static void v1_patch(uint32_t k, patch_t *p, char *nm)
{
    patch_factory(k % 6u, p, nm);
    nm[0] = 'V'; nm[1] = (char)('0' + k);
    p->volume = (uint8_t)(10u + k);
    p->fms[1] = FMS_O3; p->fma[1] = (uint8_t)(k * 9u);
    p->modf[3].a = MS_LFO2; p->modf[3].op = MO_XOR; p->modf[3].c = (int8_t)(-(int32_t)k);
}
static void host_v1_write(void)
{
    uint32_t k;
    patch_t p;
    char nm[PATCH_NAME];
    for (k = 0; k < sizeof V1_AT; k++) { v1_patch(k, &p, nm); v1_put(V1_AT[k], &p, nm, k == 6u ? 224u : V1_MAX); }
}
static void host_v1_check(void)
{
    uint32_t k, bad = 0;
    for (k = 0; k < sizeof V1_AT; k++) {
        patch_t p, e = PATCH_DEFAULT, q;
        char nm[PATCH_NAME], nq[PATCH_NAME];
        v1_patch(k, &p, nm);
        memcpy(&e, &p, k == 6u ? 224u : V1_MAX);   /* (the defaults past what the record held) */
        pattern_own(&e);                       /* the new representation preserves the legacy pattern's sound */
        patch_get(V1_AT[k], &q, nq);
        if (memcmp(&q, &e, sizeof q) || memcmp(nq, nm, PATCH_NAME)) { printf("v1 -> v2: patch %u not as saved\n", V1_AT[k]); bad++; }
    }
    if (patch_migrate()) { printf("v1 -> v2: moved again\n"); bad++; }
    printf("v1 -> v2: %u patches, %s\n", (uint32_t)sizeof V1_AT, bad ? "WRONG" : "all as saved, moved once");
    if (bad) exit(1);
}
#endif
static void app_main(void)
{
    uint32_t prev = 0, k, t_change = 0, dirty = 0;
    uint8_t held[27] = {0};                     /* the note each key plays */
#if BOARD_SMK37
    static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* white key -> key */
    uint8_t pad_note[16] = {0}, ext_note[FM1_NKEY] = {0};
    smk_pw_t wheel = {0};
    int32_t smk_bend = 0;
    smk_pad_events = 1;                         /* the pads as events (smk_pad_take): they play here, lit while they sound */
#endif
    settings_get();
#ifdef DINGHY_HOST
    for (k = 0; k < NPATCH; k++) {              /* (the host: every factory sound within its ranges, or no run) */
        patch_t fp;
        char fn[PATCH_NAME];
        patch_factory(k, &fp, fn);
        if (!patch_ok(&fp)) { printf("factory patch %u out of its ranges\n", k); exit(1); }
    }
    if (!PARAM[NPARAM - 1u].label) { printf("PARAM is short of NPARAM: an entry missing\n"); exit(1); }   /* (a P_
                                                   added to the enum without its line: the rest would shift) */
    for (k = 0; k < MD_N; k++) {                /* (and every destination in one of MATRIX's groups) */
        uint32_t g, i, c = 0;
        for (g = 0; g < NDG; g++)
            for (i = 0; i < DGROUP[g].n; i++) c += DGROUP[g].d[i] == k;
        if (c != 1) { printf("destination %u (%s) in %u groups\n", k, MDST_NAME[k], c); exit(1); }
    }
#endif
    pat_load();                                 /* the arp's patterns; the patches (the format 1 moved over, once);
                                                   the stored patch, to tell edits */
    patlib_init();
    pattern_own(&set.p);
#ifdef DINGHY_HOST
    if (getenv("VOLNA_V1")) host_v1_write();    /* (tests/migrate: old patches first) */
#endif
    k = patch_migrate();
#ifdef DINGHY_HOST
    if (k) printf("patches: %u moved to the format 2\n", k);
    if (getenv("VOLNA_V1")) host_v1_check();
#endif
    {
        char nm[PATCH_NAME];
        patch_get(set.prog, &orig, nm);
    }
#if UWT_N
    for (k = 0; k < UWT_N; k++) uwt_scan(k);    /* the user wavetables: which slots hold one (before the sound) */
#endif
    for (k = 0; k < NOSC; k++) {                /* a table saved by a build with local tables, this one without: BASIC */
        if (!tab_known(set.p.osc[k].table)) set.p.osc[k].table = 0;
        if (!tab_known(orig.osc[k].table)) orig.osc[k].table = 0;
    }
    for (k = 0; k < NV * NOSC * UMAX; k++)      /* the unison copies' phases: scattered (in step, they would start
                                                   as one loud spike) */
        voice[k / (NOSC * UMAX)].uph[k / UMAX % NOSC][k % UMAX] = 0x9E3779B9u * (k + 1u);
    for (k = 0; k < 128u; k++) {                /* COLOR's loudness for the matrix (as sound_apply works it out) */
        uint32_t kc = (uint32_t)noise_kc((int32_t)k);
        noise_comp_tab[k] = (uint16_t)isqrt32((65536u - kc) * 4096u / kc);
    }
    sound_apply();
    for (k = 0; k < NOSC; k++)
        osc[k].now = osc[k].target;             /* (no glide from wave 0 at power-on) */
    ui_splash();

    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0, base;
        int32_t e;
        platform_poll();
        poly_service();
        base = (uint32_t)((int32_t)KEY0 + 12 * (int8_t)set.oct + set.trans);

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {             /* (a key stops the note it started: the octave may change meanwhile) */
#if BOARD_SMK37
            uint32_t vel = fm1_in.key_vel[SMK_KEY_F3 + k];   /* the SMK's keys feel the velocity */
#else
            uint32_t vel = set.keyvel;          /* the FM-1's keys feel no velocity: SETUP's KEYVEL */
#endif
            if ((down >> k & 1u) && set.sec == SEC_STEPS && STEP_OF_KEY[k] < ARP_PLEN) {   /* STEPS: a white key is a
                                                   step: the first press picks it, the next one switches it on/off */
                held[k] = 255u;
                if (ui_step_key(STEP_OF_KEY[k])) { dirty = 1; t_change = fm1_ms; }
                continue;
            }
            if (down >> k & 1u) { held[k] = (uint8_t)(base + k); note_on(held[k], vel); if (set.p.arp.mode == ARP_OFF) midi_note_out(1, held[k], vel); }
            if ((up >> k & 1u) && held[k] <= 127u) { note_off(held[k]); midi_note_out(0, held[k], 0); }
        }
#if BOARD_SMK37
        while ((e = smk_extkey_take()) >= 0) {  /* the keys left and right of the FM-1's 27: key << 8 | velocity */
            uint32_t key = (uint32_t)e >> 8, vel = (uint32_t)e & 0x7Fu;
            if (key >= FM1_NKEY) continue;
            if (vel) { ext_note[key] = (uint8_t)(base + key - SMK_KEY_F3); note_on(ext_note[key], vel); if (set.p.arp.mode == ARP_OFF) midi_note_out(1, ext_note[key], vel); }
            else if (ext_note[key]) { note_off(ext_note[key]); midi_note_out(0, ext_note[key], 0); ext_note[key] = 0; }
        }
        while ((e = smk_pad_take()) >= 0) {     /* a pad: hardware pad << 8 | velocity (0 = let go) */
            uint32_t pad = ((uint32_t)e >> 8) & 15u, vel = (uint32_t)e & 0x7Fu;
            uint32_t w = pad < 8u ? pad + 8u : pad - 8u;   /* bottom row = white keys 1-8, top row 9-16 */
            if (vel) { pad_note[pad] = (uint8_t)(base + WHITE[w]); note_on(pad_note[pad], vel); if (set.p.arp.mode == ARP_OFF) midi_note_out(1, pad_note[pad], vel); }
            else if (pad_note[pad]) { note_off(pad_note[pad]); midi_note_out(0, pad_note[pad], 0); pad_note[pad] = 0; }
        }
        {
            int32_t bd = smk_pw_bend(&wheel, fm1_in.wheel[0], fm1_ms);   /* +-8192 at the ends (1/4096 semitone x 2) */
            if (bd != smk_bend) { smk_bend = bd; bend_pos = bd; }   /* (moved: it wins over MIDI's) */
        }
#else
        (void)e;
#endif
        bend_apply();
        mod_bend = bend_pos * 4 > 32767 ? 32767 : bend_pos * 4 < -32767 ? -32767 : bend_pos * 4;   /* (source BEND) */

        b = fm1_input_edges(&rel);              /* buttons pressed since the last call (bit = button id) */
        if (ui_input(b)) { dirty = 1; t_change = fm1_ms; }

        midi_in();
        if (pc_req) {                           /* a program change: that patch, now */
            patch_load(pc_req - 1u);
            pc_req = 0;
            ui.cells = ui.hero = ui.foot = 1;
            dirty = 1;
            t_change = fm1_ms;
        }
        if (ed_service()) {                     /* the editor: a sound came over USB, or a parameter */
            ui.cells = ui.hero = ui.foot = 1;
            dirty = 1;
            t_change = fm1_ms;
        }
        ed_notify();                            /* (the page watching: what the panel changed) */
        if (clk_bpm && fm1_ms - clk_t0 > 2000u) clk_bpm = 0, clk_run = 0;   /* the clock has stopped coming */
        arp_run();
        lfo_tempo();

        base = (uint32_t)((int32_t)KEY0 + 12 * (int8_t)set.oct + set.trans);
        for (k = 0; k < NV; k++)                /* what sounds, on the keys of this octave */
            if (voice[k].gate && voice[k].note >= base && voice[k].note < base + 27u)
                lit |= 1u << (voice[k].note - base);
        if (set.sec == SEC_STEPS) lit = steps_lit(lit);   /* (the white keys show the pattern's steps) */
        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), lit >> k & 1u);
        ui_leds();
#if BOARD_SMK37
        for (k = 0; k < 16u; k++) {             /* a pad glows lilac while its key sounds */
            uint32_t w = k < 8u ? k + 8u : k - 8u, on = lit >> WHITE[w] & 1u;
            padled_set(k, on ? 90 : 4, on ? 60 : 2, on ? 128 : 8);
        }
        padled_flush();                         /* (sends only what changed) */
#endif

        idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
        ui_draw();
        idle_ok = 0;

        if (ed_put_to >= 0 && quiet() && ed_put_now()) {   /* the editor's patch to a number: now that it is silent */
            ui.cells = ui.hero = ui.foot = 1;
            dirty = 1;
            t_change = fm1_ms;
        }
        if (ed_pat_to >= 0 && quiet() && !arp.run) {
            uint8_t slot = (uint8_t)ed_pat_to;
            int rc = patlib_store(slot, ed_pat_st, ed_pat_len, ed_pat_tfac);
            ed_reply(ED_PATPUT, ed_pat_seq, rc ? ED_FLASH : ED_OK, &slot, 1);
            ed_pat_to = -1;
        }
#if UWT_N
        if (uwt_op && quiet() && uwt_now())     /* a user wavetable's erase: now that it is silent */
            ui.cells = ui.hero = ui.foot = 1;
#endif
        if (save_to >= 0 && quiet()) {          /* SAVE: now that it is silent */
            int r = patch_save_now();
            if (ed_save_wait) {
                ed_reply(ED_SAVE, ed_save_seq, r > 0 ? ED_OK : ED_FLASH, &ed_save_slot, 1);
                ed_save_wait = 0;
            }
            ui_msg(r > 0 ? "SAVED" : "SAVE FAILED", 0, 0);
            ui.cells = ui.hero = 1;
            dirty = 1;
            t_change = fm1_ms;
        }
        if (dirty && quiet() && !arp.run && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an
                                                   erase stalls the sound; a running arp is not silent, only between
                                                   its notes) */
            settings_put();
            /* Legacy blob 5 is immutable: older patches resolve their references against it. */
            dirty = 0;
        }
    }
}
