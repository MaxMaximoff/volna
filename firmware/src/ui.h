/* SPDX-License-Identifier: GPL-3.0-only */
/* Volna: the screen and the panel, as docs/ui/UI.md designs them. Included by volna.c after the sound, the settings
 * (set) and MIDI in. The main loop calls ui_splash() once, then ui_input() with the button edges, ui_leds() and
 * ui_draw() every pass.
 *
 * The screen, three zones: the cells (y 0..55: KNOB1..4, left to right, as the knobs stand), the section's picture
 * (y 60..183, one 240 x 124 canvas), the footer (y 186..239: the section and what SELECT picks, the rows; the patch,
 * the octave, the four voices). Only what changed is drawn again; the pictures that move (the table gliding, the
 * voices on the envelope) at most every 40 ms. */

#define KEY0 53u                                /* key 0 = F3 */

/* ---- sections: one a button (the faceplate is Volna's own; the stock label in the comment) */
enum { SEC_OSC, SEC_MIX, SEC_FILT, SEC_ENV, SEC_MOD, SEC_FX, SEC_PATCH, SEC_ARP, SEC_SETUP, SEC_STEPS, NSEC };
static const char *const SEC_NAME[NSEC] = {"OSC", "MIX", "FILTER", "ENV", "MOD", "FX", "PATCH", "ARP", "SETUP", "SEQ"};
static const uint8_t SEC_BTN[NSEC] = {
    BTN_FX, BTN_SEL, BTN_ENV, BTN_LFO, BTN_EDIT, BTN_GLO,   /* OSC [FX] MIX [SCL] FILT [ENV] ENV [LFO] MOD [EDIT] FX [GLO] */
    BTN_HOME, BTN_ARP, 0xFFu, BTN_SEQ};                     /* PATCH [HOME] ARP; SETUP: SHIFT + PATCH; STEPS [SEQ] */
static const uint16_t SEC_COL[NSEC] = {
    RGB(180, 140, 255), RGB(80, 200, 190), RGB(255, 150, 60), RGB(120, 220, 110), RGB(240, 210, 80),
    RGB(90, 160, 255), RGB(220, 220, 220), RGB(255, 110, 170), RGB(220, 220, 220), RGB(70, 210, 240)};   /* (SEQ: the pattern's own, not ARP's) */
/* SAVE [SAVE], STEPS [SEQ], RUN [PLAY], SHIFT [REC]: the bottom row's others */
#define BTN_STEPS BTN_SEQ
#define BTN_RUN BTN_PLAY
#define BTN_SHIFT BTN_REC

/* the look (docs/ui/UI.md, "Внешний вид"): cool dark cards on a darker screen, the section's colour as the accent */
#define C_BG RGB(9, 12, 16)                     /* the screen behind the cards (the gaps, the footer) */
#define C_CARD RGB(17, 22, 28)                  /* a card: a cell, the picture */
#define C_EDGE RGB(27, 34, 42)                  /* a card's rim */
#define C_RULE RGB(31, 39, 47)                  /* tracks, grids, empty faders */
#define C_DARK RGB(64, 74, 86)                  /* inactive: empty steps, dashes */
#define C_OFF RGB(112, 123, 136)                /* secondary text, units */
#define C_VAL RGB(206, 212, 219)                /* values */
#define C_AMBER RGB(251, 191, 36)               /* edited, SAVE */
#define C_WARN RGB(255, 90, 70)                 /* the matrix pushes past the table's end: it stops there */

static uint16_t mixc(uint16_t a, uint16_t b, uint32_t k16)   /* a towards b by k16 / 16 */
{
    int32_t ra = a >> 11, ga = (a >> 5) & 63, ba = a & 31, k = (int32_t)k16;
    int32_t r = ra + (((b >> 11) - ra) * k >> 4), g = ga + ((((b >> 5) & 63) - ga) * k >> 4), bl = ba + (((b & 31) - ba) * k >> 4);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
static uint16_t dimc(uint16_t c, uint32_t k8)   /* a colour faded into the card: k8 / 8 of it */
{
    return mixc(C_CARD, c, k8 < 8u ? k8 * 2u : 16u);
}

/* ---- rounded boxes: a corner's row dy (0: the outer row) starts this far in; the pixel before it half covered */
static int32_t rr_in(int32_t r, int32_t dy)
{
    int32_t i, e = 2 * r - 2 * dy - 1;
    for (i = 0; i < r; i++) {
        int32_t d = 2 * r - 2 * i - 1;
        if (d * d + e * e <= 4 * r * r) break;
    }
    return i;
}
static void cv_half(int32_t x, int32_t y, uint16_t c)   /* c over the canvas pixel, half */
{
    y += cv_oy;
    if ((uint32_t)x < cv_w && (uint32_t)y < cv_h) {
        uint16_t *p = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
        *p = swap16(mixc(swap16(*p), c, 8u));
    }
}
static void cv_rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    int32_t j;
    if (2 * r > h) r = h / 2;
    if (2 * r > w) r = w / 2;
    for (j = 0; j < h; j++) {
        int32_t dy = j < r ? j : j >= h - r ? h - 1 - j : r, in = dy < r ? rr_in(r, dy) : 0;
        cv_rect(x + in, y + j, w - 2 * in, 1, c);
        if (in) { cv_half(x + in - 1, y + j, c); cv_half(x + w - in, y + j, c); }
    }
}
static void cv_rrect_o(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t fill, uint16_t rim)
{
    cv_rrect(x, y, w, h, r, rim);               /* (a rim 1 px: the box, then the fill inside it) */
    cv_rrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 1, fill);
}
static void cv_mix(int32_t x, int32_t y, uint16_t c, uint32_t k16)   /* c over the canvas pixel by k16 / 16 */
{
    y += cv_oy;
    if ((uint32_t)x < cv_w && (uint32_t)y < cv_h && k16) {
        uint16_t *p = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
        *p = swap16(k16 >= 16u ? c : mixc(swap16(*p), c, k16));
    }
}
static void cv_mixd(int32_t x, int32_t y, uint16_t c, uint32_t k256)   /* c over the canvas pixel by k256 / 256,
                                                   the rounding dithered 2 x 2 (a gradient without bands in RGB565) */
{
    static const uint8_t BAYER[4] = {32, 160, 224, 96};
    y += cv_oy;
    if ((uint32_t)x < cv_w && (uint32_t)y < cv_h && k256) {
        uint16_t *p = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
        uint32_t a = swap16(*p), d = BAYER[(x & 1) | (y & 1) << 1], j = 256u - k256;
        uint32_t r = ((a >> 11) * j + (uint32_t)(c >> 11) * k256 + d) >> 8;
        uint32_t g = (((a >> 5) & 63u) * j + (uint32_t)((c >> 5) & 63u) * k256 + d) >> 8;
        uint32_t b = ((a & 31u) * j + (uint32_t)(c & 31u) * k256 + d) >> 8;
        *p = swap16((r << 11) | (g << 5) | b);
    }
}
static void vdot(int32_t x, int32_t y, uint16_t col)   /* a voice on a curve: a white dot in a ring of the colour */
{
    cv_rrect(x - 5, y - 5, 11, 11, 5, mixc(C_CARD, col, 10u));
    cv_rrect(x - 3, y - 3, 7, 7, 3, C_WHITE);
}

/* ---- curves: a y for each column (crv[x], in 1/16 px), drawn column by column, every pixel once a pass: a fill under
 * it fading down, a dim halo, the line anti-aliased up and down (a steep part: the column's span from the midpoint
 * with the column before to the one with the column after, so a jump stays joined) */
static int16_t crv[240];
static void crv_seg(int32_t xa, int32_t ya, int32_t xb, int32_t yb)   /* a straight piece, x in px, y in 1/16 px;
                                                   xa == xb: a step at xa */
{
    int32_t x, dx = xb - xa;
    for (x = xa < 0 ? 0 : xa; x <= xb && x < 240; x++)
        crv[x] = (int16_t)(dx > 0 ? ya + (yb - ya) * (x - xa) / dx : yb);
}
static int32_t crv_half(int32_t y, int32_t n)   /* towards a neighbour: half way; a jump (over 4 px): all the way,
                                                   so a step is as wide as the line both sides of it */
{
    int32_t d = n - y;
    return d > 64 || d < -64 ? n : y + d / 2;
}
static void crv_span(int32_t x, int32_t xa, int32_t xb, int32_t *lo, int32_t *hi)   /* column x's reach, 1/16 px */
{
    int32_t y = crv[x], a = x > xa ? crv_half(y, crv[x - 1]) : y, b = x < xb ? crv_half(y, crv[x + 1]) : y;
    *lo = y < a ? (y < b ? y : b) : (a < b ? a : b);
    *hi = y > a ? (y > b ? y : b) : (a > b ? a : b);
}
static void crv_stroke(int32_t xa, int32_t xb, int32_t hw, uint16_t c)   /* the line, hw its half width (1/16 px):
                                                   a pixel as covered as its rows overlap the span */
{
    int32_t x;
    for (x = xa; x <= xb; x++) {
        int32_t lo, hi, r;
        crv_span(x, xa, xb, &lo, &hi);
        lo -= hw; hi += hw;
        for (r = lo >> 4; r <= hi >> 4; r++) {
            int32_t t = r << 4, a = (hi < t + 16 ? hi : t + 16) - (lo > t ? lo : t);
            if (a > 0) cv_mix(x, r, c, (uint32_t)a);
        }
    }
}
static void crv_line(int32_t xa, int32_t xb, uint16_t c)   /* the line, 2 px, with its halo */
{
    crv_stroke(xa, xb, 40, mixc(C_CARD, c, 3u));
    crv_stroke(xa, xb, 16, c);
}
static void crv_fill(int32_t xa, int32_t xb, int32_t ytop, int32_t base, uint16_t c, uint32_t k)   /* under the line
                                                   down to base: c at k / 16 at ytop, fading to none at base */
{
    int32_t x, r, span = base - ytop > 0 ? base - ytop : 1;
    for (x = xa; x <= xb; x++) {
        int32_t y = crv[x], r0 = y >> 4, part = 16 - (y & 15);
        for (r = r0; r < base; r++) {
            int32_t f = (int32_t)k * 16 * (base - r) / span;   /* (1/256) */
            if (f > (int32_t)k * 16) f = (int32_t)k * 16;
            cv_mixd(x, r, c, (uint32_t)(r == r0 ? f * part >> 4 : f));
        }
    }
}
static void dots_v(int32_t x, int32_t y0, int32_t y1, uint16_t c)   /* a dotted grid line, a dot every 4 px */
{
    for (; y0 < y1; y0 += 4) cv_rect(x, y0, 1, 1, c);
}
static void dots_h(int32_t x0, int32_t x1, int32_t y, uint16_t c)
{
    for (; x0 < x1; x0 += 4) cv_rect(x0, y, 2, 1, c);
}

/* ---- parameters: what a knob turns, and the pages (row A, row B) */
enum { P_NONE, P_WAVE, P_SEMI, P_FINE, P_TABLE, P_L1, P_L2, P_L3, P_NOISE, P_RING, P_COLOR,
       P_BAL1, P_BAL2, P_BAL3, P_BALN, P_BALR,
       P_CUT, P_RES, P_FENV, P_KTRK, P_FTYPE, P_ROUTE, P_DRIVE, P_FVEL,
       P_ATK, P_DEC, P_SUS, P_REL, P_EVEL, P_EMODE, P_EALVL, P_EDEC2, P_ESUS2, P_VOL, P_MORPH, P_MIDI, P_VCURVE, P_KEYVEL, P_TUNE, P_TRANS,
       P_AMODE, P_ADIR, P_AORD, P_AOCT, P_ATEMPO, P_ARATE, P_AGATE, P_ASWING, P_AVEL,
       P_APAT, P_APLEN, P_ATFAC, P_STYPE, P_SACC, P_SLEN, P_STIM, P_SGLIDE,
       P_LSHAPE, P_LRATE, P_LSYNC, P_LDELAY, P_LKEY, P_LPHASE, P_MSRC, P_MDST, P_MAMT, P_MGRP, P_GLIDE, P_VMODE, P_BEND, P_UNI, P_UDET, P_USPR,
       P_FXTYPE, P_FXMIX, P_DTIME, P_DFB, P_DSYNC, P_DTONE, P_DPING, P_RSIZE, P_RDAMP, P_RPRE, P_RWIDTH,
       P_XDRIVE, P_XTONE, P_XSHAPE, P_XLEVEL, P_CRATE, P_CDEPTH, P_CMODE, P_CFB, P_CWIDTH, P_CDLY,
       P_EQLO, P_EQMID, P_EQHI, P_EQQ, P_EQLF, P_EQMF, P_EQHF,
       P_PRATE, P_PDEPTH, P_PFREQ, P_PFB, P_PSTG, P_PWIDTH,
       P_ERATE, P_EDEPTH, P_EVIB, P_EVRATE, P_ETONE, P_EWIDTH,
       P_ORATE, P_OBITS, P_OTONE, P_OWOB, P_ONOISE, P_TRATE, P_TDEPTH, P_TCHO, P_TSH, P_TDRV,
       P_WCUT, P_WRES, P_WTYPE, P_WSRC, P_WDEPTH, P_WRATE, P_ACOPY,
       P_FA, P_FOP, P_FB, P_FC, P_CTLW, P_CTLX, P_CTLY, P_CTLZ, P_FMS, P_FMA, P_SYNC, P_DCURVE, P_FFMS, P_FFMA, P_FPAN, P_GMODE, P_OKT, P_OBEND, P_LFADE, P_WLIM, P_ETRIG, P_PCAT, NPARAM };   /* (P_FMS..: FM) */   /* (2026-11-15: a modifier; SETUP's CCs) */
enum { F_NUM, F_SIGNED, F_MIDI, F_FTYPE, F_ROUTE, F_BAL, F_CURVE, F_AMODE, F_ADIR, F_AORD, F_ARATE, F_AGATE, F_AVEL,
       F_APAT, F_STYPE, F_SACC, F_SLEN, F_STIM, F_ONOFF,
       F_LSHAPE, F_LRATE, F_MSRC, F_MDST, F_MGRP, F_EMODE, F_GLIDE, F_VMODE, F_FXTYPE, F_DTIME, F_XSHAPE, F_CMODE,
       F_HZ, F_EQQ, F_PSTG, F_SRATE, F_BITS, F_OTONE, F_WCUT, F_WTYPE, F_WSRC, F_WRATE, F_ACOPY, F_MOP, F_MSRCB, F_CC, F_FMS, F_DCURVE, F_GMODE, F_OKT, F_OBEND, F_LFADE, F_ETRIG, F_PCAT };
static const char *const VMODE_NAME[VM_N] = {"POLY", "MONO", "LEGATO"};
static const char *const FXTYPE_NAME[FX_N] = {"OFF", "DELAY", "REVERB", "DIST", "CHORUS", "PHASER", "ENSEMBLE", "LO-FI",
                                              "FILTER", "TRIPLE"};
static const char *const WTYPE_NAME[FF_NTYPE] = {"LP24", "LP12", "BP24", "BP12", "HP24", "HP12", "NOTCH"};
static const char *const WSRC_NAME[FS_N] = {"LFO", "STEREO", "SYNC", "ENV"};
static const char *const CMODE_NAME[CM_N] = {"CHORUS", "FLANGE"};
static const char *const XSHAPE_NAME[DS_N] = {"SOFT", "HARD", "FOLD", "TUBE"};
static const char *const EMODE_NAME[EM_N] = {"ADSR", "ADS1DS2R", "ONE SHOT", "LOOP S1S2", "LOOP ALL"};
static const char *const LSHAPE_NAME[LS_N] = {"SINE", "TRI", "SQR", "SAW", "RAND", "S&H"};
static const char *const MSRC_NAME[MS_N] = {"OFF", "LFO1", "LFO2", "LFO3", "ENV A", "ENV F", "VELO", "KEY", "MACRO",
                                            "WHEEL", "RAND", "ENV 3", "ENV 4", "PRESS", "P.PRESS", "BEND", "SUST",
                                            "BREATH", "FOOT", "CTL W", "CTL X", "CTL Y", "CTL Z", "LFO1*MW",
                                            "LFO2*PR", "MIN", "MAX", "MOD1", "MOD2", "MOD3", "MOD4"};
static const char *const MDST_NAME[MD_N] = {"OFF", "PITCH", "PIT1", "PIT2", "PIT3", "WAVE", "WAV1", "WAV2", "WAV3",
                                            "LVL1", "LVL2", "LVL3", "NOISE", "RING", "CUT", "CUT1", "CUT2", "RES1",
                                            "RES2", "VOL", "A.ATK", "A.DEC", "A.SUS", "A.REL", "F.ATK", "F.DEC",
                                            "F.SUS", "F.REL", "3.ATK", "3.DEC", "3.SUS", "3.REL", "4.ATK", "4.DEC",
                                            "4.SUS", "4.REL", "DET1", "DET2", "DET3", "COLOR", "BAL1", "BAL2",
                                            "BAL3", "BALN", "BALR", "F1.DRV", "F2.DRV", "F1.ENV", "F2.ENV", "PAN",
                                            "L1.SPD", "L2.SPD", "L3.SPD", "AMT1", "AMT2", "AMT3", "AMT4", "FM1",
                                            "FM2", "FM3", "F1.FM", "F2.FM", "F1.PAN", "F2.PAN"};
/* the matrix's destinations in groups (MATRIX: KNOB2 the group, KNOB3 one of it); every destination in one group
 * (the host checks it at start) */
static const uint8_t DG_OFF[] = {MD_OFF};
static const uint8_t DG_OSC[] = {MD_PITCH, MD_P1, MD_P2, MD_P3, MD_WAVE, MD_W1, MD_W2, MD_W3, MD_DET1, MD_DET2, MD_DET3,
                                 MD_FM1, MD_FM2, MD_FM3};
static const uint8_t DG_MIX[] = {MD_L1, MD_L2, MD_L3, MD_NOISE, MD_RING, MD_COLOR, MD_BAL1, MD_BAL2, MD_BAL3, MD_BALN,
                                 MD_BALR};
static const uint8_t DG_FILT[] = {MD_CUT, MD_CUT1, MD_CUT2, MD_RES1, MD_RES2, MD_DRV1, MD_DRV2, MD_FENV1, MD_FENV2,
                                  MD_FFM1, MD_FFM2, MD_FPAN1, MD_FPAN2};
static const uint8_t DG_AMP[] = {MD_VOL, MD_PAN};
static const uint8_t DG_MOD[] = {MD_LSPD1, MD_LSPD2, MD_LSPD3, MD_AMT1, MD_AMT2, MD_AMT3, MD_AMT4};   /* (the LFOs'
                                                   speed, slots 1..4's amount) */
static const uint8_t DG_ENV[] = {MD_AATK, MD_ADEC, MD_ASUS, MD_AREL, MD_FATK, MD_FDEC, MD_FSUS, MD_FREL,
                                 MD_3ATK, MD_3DEC, MD_3SUS, MD_3REL, MD_4ATK, MD_4DEC, MD_4SUS, MD_4REL};
#define NDG 7u
static const struct { const uint8_t *d; uint8_t n; } DGROUP[NDG] = {
    {DG_OFF, sizeof DG_OFF}, {DG_OSC, sizeof DG_OSC}, {DG_MIX, sizeof DG_MIX}, {DG_FILT, sizeof DG_FILT},
    {DG_AMP, sizeof DG_AMP}, {DG_ENV, sizeof DG_ENV}, {DG_MOD, sizeof DG_MOD}};
static const char *const DGROUP_NAME[NDG] = {"OFF", "OSC", "MIX", "FILT", "AMP", "ENV", "MOD"};
static const char *const FMS_NAME[FMS_N] = {"OFF", "OSC1", "OSC2", "OSC3", "NOISE"};   /* FM's sources */
static const char *const DCURVE_NAME[DC_N] = {"CLIP", "TUBE", "HARD", "MEDIUM", "SOFT", "PICKUP1", "PICKUP2",
                                              "RECTIFY", "SQUARE", "BINARY", "OVERFLOW", "SINE", "OSC1 MOD"};   /* (the
                                                   Blofeld's order on the knob) */
static const uint8_t DCURVE_ORDER[DC_N] = {DC_CLIP, DC_TUBE, DC_HARD, DC_MEDIUM, DC_SOFT, DC_PICK1, DC_PICK2, DC_RECT,
                                           DC_SQUARE, DC_BINARY, DC_OVER, DC_SINE, DC_OSC1};
static uint32_t dcurve_pos(uint32_t c)          /* a curve's place on the knob */
{
    uint32_t i;
    for (i = 0; i < DC_N; i++) if (DCURVE_ORDER[i] == c) return i;
    return 4u;
}
static const char *const GMODE_NAME[GM_N] = {"PORTA", "FING P", "GLISS", "FING G"};
static const char *const PCAT_NAME[PC_N] = {"INIT", "ARP", "ATMO", "BASS", "DRUM", "FX", "KEYS", "LEAD", "MONO", "PAD",
                                            "PERC", "POLY", "SEQ"};
static const char *const MOP_NAME[MO_N] = {"+", "-", "*", "AND", "OR", "XOR", "MAX", "MIN"};   /* the modifiers' */
static uint32_t dst_group(uint32_t d, uint32_t *at)   /* the group destination d is in (and where in it) */
{
    uint32_t g, i;
    for (g = 0; g < NDG; g++)
        for (i = 0; i < DGROUP[g].n; i++)
            if (DGROUP[g].d[i] == d) { if (at) *at = i; return g; }
    if (at) *at = 0;
    return 0;
}
static const char *const STYPE_NAME[AS_N] = {"NORM", "PAUSE", "PREV", "FIRST", "LAST", "F+L", "CHORD", "RAND"};
static const char *const SACC_NAME[8] = {"MUTE", "-96", "-64", "-32", "0", "+32", "+64", "+96"};
static const char *const SOFF_NAME[8] = {"", "-3", "-2", "-1", "0", "+1", "+2", "+3"};   /* LENGTH, TIMING; 0: LEG, RAND */
static const char *const AMODE_NAME[ARP_NMODE] = {"OFF", "ON", "1SHOT", "HOLD"};
static const char *const ADIR_NAME[AD_N] = {"UP", "DOWN", "UPDN", "DNUP"};
static const char *const AORD_NAME[AO_N] = {"PLAY", "REV", "KEY+", "KEY-", "VEL+", "VEL-"};
static const char *const AVEL_NAME[AV_N] = {"EACH", "1ST", "LAST", "32", "64", "100", "127"};
static const char *const CURVE_NAME[NVC] = {"LIN", "SOFT", "HARD", "FIX"};
/* the filter types in the knob's order (the Blofeld's: each 24 dB type before its 12), and where each type is in it */
static const char *const FTYPE_NAME[SVF_NTYPE] = {"OFF", "LP24", "LP12", "BP24", "BP12", "HP24", "HP12", "NOTCH24",
                                                  "NOTCH12", "COMB+", "COMB-", "PPG LP"};   /* (the Blofeld's order) */
static const uint8_t FTYPE_ORDER[SVF_NTYPE] = {SVF_OFF, SVF_LP24, SVF_LP, SVF_BP24, SVF_BP, SVF_HP24, SVF_HP, SVF_NOTCH24,
                                              SVF_NOTCH, SVF_COMBP, SVF_COMBN, SVF_PPG};
static uint32_t ftype_pos(uint32_t t)
{
    uint32_t i;
    for (i = 0; i < SVF_NTYPE; i++)
        if (FTYPE_ORDER[i] == t) return i;
    return 0;
}
typedef struct { const char *label; int16_t lo, hi; uint8_t step, fmt; } param_t;   /* step: a detent (SHIFT: 1) */
static const param_t PARAM[NPARAM] = {
    {"", 0, 0, 0, F_NUM},
    {"WAVE", 0, 63, 1, F_NUM}, {"SEMI", -36, 36, 1, F_SIGNED}, {"FINE", -50, 50, 1, F_SIGNED},
    {"TABLE", 0, NTAB + UWT_N, 1, F_NUM},
    {"OSC1", 0, 127, 4, F_NUM}, {"OSC2", 0, 127, 4, F_NUM}, {"OSC3", 0, 127, 4, F_NUM},
    {"NOISE", 0, 127, 4, F_NUM}, {"RING", 0, 127, 4, F_NUM}, {"COLOR", 0, 127, 4, F_NUM},
    {"BAL O1", -63, 63, 3, F_BAL}, {"BAL O2", -63, 63, 3, F_BAL}, {"BAL O3", -63, 63, 3, F_BAL},
    {"BAL N", -63, 63, 3, F_BAL}, {"BAL R", -63, 63, 3, F_BAL},
    {"CUTOFF", 0, 127, 2, F_NUM}, {"RESO", 0, 127, 2, F_NUM}, {"ENV", -63, 63, 1, F_SIGNED},
    {"KEYTRK", 0, 100, 5, F_NUM}, {"TYPE", 0, SVF_NTYPE - 1, 1, F_FTYPE}, {"ROUTE", 0, 1, 1, F_ROUTE},
    {"DRIVE", 0, 127, 4, F_NUM}, {"VELO", -63, 63, 2, F_SIGNED},
    {"ATTACK", 0, 127, 2, F_NUM}, {"DECAY", 0, 127, 2, F_NUM}, {"SUST", 0, 127, 2, F_NUM}, {"REL", 0, 127, 2, F_NUM},
    {"VELO", -63, 63, 3, F_SIGNED}, {"MODE", 0, EM_N - 1, 1, F_EMODE}, {"A.LVL", 0, 127, 2, F_NUM},
    {"DEC2", 0, 127, 2, F_NUM}, {"SUS2", 0, 127, 2, F_NUM},
    {"VOLUME", 0, 127, 4, F_NUM}, {"MORPH", -63, 63, 1, F_SIGNED},
    {"MIDI", 0, 16, 1, F_MIDI}, {"CURVE", 0, NVC - 1, 1, F_CURVE}, {"KEYVEL", 1, 127, 4, F_NUM},
    {"TUNE", -50, 50, 1, F_SIGNED}, {"TRANS", -12, 12, 1, F_SIGNED},
    {"MODE", 0, ARP_NMODE - 1, 1, F_AMODE}, {"DIR", 0, AD_N - 1, 1, F_ADIR}, {"ORDER", 0, AO_N - 1, 1, F_AORD},
    {"OCT", 1, 10, 1, F_NUM}, {"TEMPO", 40, 300, 2, F_NUM}, {"RATE", 0, ARP_NRATE - 1, 1, F_ARATE},
    {"GATE", 5, 100, 5, F_AGATE}, {"SWING", 0, 75, 3, F_NUM}, {"VELO", 0, AV_N - 1, 1, F_AVEL},
    {"LOAD", 0, ARP_NPAT, 1, F_APAT}, {"STEPS", 1, ARP_PLEN, 1, F_NUM}, {"T.AMT", 0, 127, 4, F_NUM},
    {"TYPE", 0, AS_N - 1, 1, F_STYPE}, {"ACCENT", 0, 7, 1, F_SACC}, {"LENGTH", 0, 8, 1, F_SLEN},
    {"TIMING", 0, 7, 1, F_STIM}, {"GLIDE", 0, 1, 1, F_ONOFF},
    {"SHAPE", 0, LS_N - 1, 1, F_LSHAPE}, {"RATE", 0, 127, 2, F_LRATE}, {"SYNC", 0, 1, 1, F_ONOFF},
    {"DELAY", 0, 127, 4, F_NUM}, {"KEY", 0, 1, 1, F_ONOFF}, {"PHASE", 0, 127, 4, F_NUM},
    {"SOURCE", 0, MS_N - 1, 1, F_MSRC}, {"DEST", 0, MD_N - 1, 1, F_MDST}, {"AMOUNT", -63, 63, 1, F_SIGNED},
    {"GROUP", 0, NDG - 1, 1, F_MGRP},
    {"GLIDE", 0, 127, 2, F_GLIDE}, {"MODE", 0, VM_N - 1, 1, F_VMODE}, {"BEND", 0, 24, 1, F_NUM},
    {"UNISON", 1, UMAX, 1, F_NUM}, {"DETUNE", 0, 127, 2, F_NUM}, {"SPREAD", 0, 127, 4, F_NUM},
    {"TYPE", 0, FX_N - 1, 1, F_FXTYPE}, {"MIX", 0, 127, 2, F_NUM}, {"TIME", 0, 127, 1, F_DTIME},
    {"FEEDBK", 0, 127, 2, F_NUM}, {"SYNC", 0, 1, 1, F_ONOFF}, {"TONE", 0, 127, 2, F_NUM}, {"PING", 0, 127, 4, F_NUM},
    {"SIZE", 0, 127, 2, F_NUM}, {"DAMP", 0, 127, 2, F_NUM}, {"PRE", 0, 127, 2, F_NUM}, {"WIDTH", 0, 127, 4, F_NUM},
    {"DRIVE", 0, 127, 2, F_NUM}, {"TONE", 0, 127, 2, F_NUM}, {"SHAPE", 0, DS_N - 1, 1, F_XSHAPE},
    {"LEVEL", 0, 127, 2, F_NUM},
    {"RATE", 0, 127, 2, F_NUM}, {"DEPTH", 0, 127, 2, F_NUM}, {"MODE", 0, CM_N - 1, 1, F_CMODE},
    {"FEEDBK", -63, 63, 2, F_SIGNED}, {"WIDTH", 0, 127, 4, F_NUM}, {"DELAY", 0, 127, 2, F_NUM},
    {"LOW", -EQ_DBMAX, EQ_DBMAX, 1, F_SIGNED}, {"MID", -EQ_DBMAX, EQ_DBMAX, 1, F_SIGNED},
    {"HIGH", -EQ_DBMAX, EQ_DBMAX, 1, F_SIGNED}, {"Q", 0, EQ_QMAX, 2, F_EQQ},
    {"L.FREQ", 24, 84, 2, F_HZ}, {"M.FREQ", 36, 120, 2, F_HZ}, {"H.FREQ", 72, 128, 2, F_HZ},   /* (EQ_NLO, EQ_NHI) */
    {"RATE", 0, 127, 2, F_NUM}, {"DEPTH", 0, 127, 2, F_NUM}, {"FREQ", PH_FLO, PH_FHI, 2, F_HZ},
    {"FEEDBK", -63, 63, 2, F_SIGNED}, {"STAGES", 0, PH_NSTG - 1, 1, F_PSTG}, {"WIDTH", 0, 127, 4, F_NUM},
    {"RATE", 0, 127, 2, F_NUM}, {"DEPTH", 0, 127, 2, F_NUM}, {"VIB", 0, 127, 2, F_NUM}, {"V.RATE", 0, 127, 2, F_NUM},
    {"TONE", 0, 127, 2, F_NUM}, {"WIDTH", 0, 127, 4, F_NUM},
    {"RATE", 0, 127, 2, F_SRATE}, {"BITS", 0, 127, 2, F_BITS}, {"TONE", 0, 127, 2, F_OTONE},
    {"WOBBLE", 0, 127, 2, F_NUM}, {"NOISE", 0, 127, 2, F_NUM},
    {"RATE", 0, 127, 2, F_NUM}, {"DEPTH", 0, 127, 2, F_NUM}, {"CHO MIX", 0, 127, 2, F_NUM},   /* (TRIPLE, in the enum's
                                                   place: after LO-FI's) */
    {"S&H", 0, 127, 2, F_NUM}, {"DRIVE", 0, 127, 2, F_NUM},
    {"CUTOFF", 0, 127, 2, F_WCUT}, {"RESO", 0, 127, 2, F_NUM}, {"FILTER", 0, FF_NTYPE - 1, 1, F_WTYPE},
    {"SOURCE", 0, FS_N - 1, 1, F_WSRC}, {"DEPTH", -63, 63, 1, F_SIGNED}, {"RATE", 0, 127, 2, F_WRATE},
    {"COPY", 0, ARP_NPAT, 1, F_ACOPY},          /* (ARP row C: a bank pattern into the patch's own, OWN on) */
    {"SRC A", 0, MS_N - 1, 1, F_MSRC}, {"OP", 0, MO_N - 1, 1, F_MOP}, {"SRC B", 0, MS_N - 1, 1, F_MSRCB},
    {"CONST", -64, 63, 2, F_SIGNED},            /* (MOD row D: the modifier SELECT picks; SRC B OFF: CONST) */
    {"CTL W", 0, 119, 1, F_CC}, {"CTL X", 0, 119, 1, F_CC}, {"CTL Y", 0, 119, 1, F_CC},
    {"CTL Z", 0, 119, 1, F_CC},                 /* (SETUP row C: the CCs of sources CTL W..Z; 0 none) */
    {"FM SRC", 0, FMS_N - 1, 1, F_FMS}, {"FM AMT", 0, 127, 2, F_NUM},   /* (OSC row C: FM) */
    {"SYNC", 0, 1, 1, F_ONOFF},                 /* (OSC2's row C: synced to OSC3) */
    {"CURVE", 0, DC_N - 1, 1, F_DCURVE},        /* (FILTER row C: DRIVE's curve, */
    {"FM SRC", 0, FMS_N - 1, 1, F_FMS}, {"FM AMT", 0, 127, 2, F_NUM},   /* Filter FM, */
    {"PAN", -63, 63, 2, F_SIGNED},              /* the filter's PAN) */
    {"G.MODE", 0, GM_N - 1, 1, F_GMODE},        /* (PATCH row B: the glide's mode) */
    {"KEYTRK", -128, 48, 5, F_OKT},             /* (OSC row B: KEYTRK, 100 + 2 x %) */
    {"BEND", 0, 49, 1, F_OBEND},                /* (OSC row C: the bend range, 0 the patch's) */
    {"FADE", 0, 1, 1, F_LFADE},                 /* (LFO row B: DELAY fades IN or OUT) */
    {"LIMIT", 0, 1, 1, F_ONOFF},                /* (OSC row D: LIMIT WT, whole waves only) */
    {"TRIGGER", 0, 1, 1, F_ETRIG},              /* (ENV row C: NORMAL or SINGLE) */
    {"CATEGORY", 0, PC_N - 1, 1, F_PCAT}};      /* (PATCH row B: the patch's category) */
/* MOD's two kinds of page: an LFO's (rows A, B) and a slot's */
static const uint8_t PAGE_LFO[2][4] = {{P_LSHAPE, P_LRATE, P_LSYNC, P_LDELAY}, {P_LKEY, P_LPHASE, P_LFADE, 0}};
static const uint8_t PAGE_SLOT[4] = {P_MSRC, P_MGRP, P_MDST, P_MAMT};   /* (KNOB2 the destination's group) */
static const uint8_t PAGE_MODF[4] = {P_FA, P_FOP, P_FB, P_FC};         /* row D: a modifier */
static const uint8_t PAGE_FX[FX_N][2][4] = {   /* FX: the slot's type picks its page (rows A, B) */
    {{P_FXTYPE, 0, 0, 0}, {0}},
    {{P_FXTYPE, P_FXMIX, P_DTIME, P_DFB}, {P_DSYNC, P_DTONE, P_DPING, 0}},
    {{P_FXTYPE, P_FXMIX, P_RSIZE, P_RDAMP}, {P_RPRE, P_RWIDTH, 0, 0}},
    {{P_FXTYPE, P_FXMIX, P_XDRIVE, P_XTONE}, {P_XSHAPE, P_XLEVEL, 0, 0}},
    {{P_FXTYPE, P_FXMIX, P_CRATE, P_CDEPTH}, {P_CMODE, P_CFB, P_CWIDTH, P_CDLY}},
    {{P_FXTYPE, P_FXMIX, P_PRATE, P_PDEPTH}, {P_PFREQ, P_PFB, P_PSTG, P_PWIDTH}},
    {{P_FXTYPE, P_FXMIX, P_ERATE, P_EDEPTH}, {P_EVIB, P_EVRATE, P_ETONE, P_EWIDTH}},
    {{P_FXTYPE, P_FXMIX, P_ORATE, P_OBITS}, {P_OTONE, P_OWOB, P_ONOISE, 0}},
    {{P_FXTYPE, P_FXMIX, P_WCUT, P_WRES}, {P_WTYPE, P_WSRC, P_WDEPTH, P_WRATE}},
    {{P_FXTYPE, P_FXMIX, P_TRATE, P_TDEPTH}, {P_TCHO, P_TSH, P_TDRV, 0}}};   /* (TRIPLE: S&H, DRIVE, the chorus) */
static const uint8_t PAGE_EQ[2][4] = {          /* FX, the third: the EQ (no type); the gains over their frequencies */
    {P_EQLO, P_EQMID, P_EQHI, P_EQQ}, {P_EQLF, P_EQMF, P_EQHF, 0}};
#define FX_EQ 2u                                /* (set.fx_sel: the EQ) */
static int32_t note_hz(int32_t n);
static int32_t exp2_x100(int32_t q16);
static char *put_dec(char *p, int32_t x100, int32_t places);
static char *put_int(char *p, int32_t v, int plus);
static char *put_str(char *p, const char *s);
static char *put_hz_cell(char *p, int32_t hz)   /* a frequency in a cell: "110", "1.2K", "13K" */
{
    if (hz < 1000) return put_int(p, hz, 0);
    if (hz < 10000) { p = put_int(p, hz / 1000, 0); *p++ = '.'; p = put_int(p, hz / 100 % 10, 0); }
    else p = put_int(p, (hz + 500) / 1000, 0);
    return put_str(p, "K");
}
static int32_t eq_q_x100(uint32_t r) { return exp2_x100((int32_t)r * 2912) / 2; }   /* MID's Q, x 100 (svf.h's) */
static const uint8_t PAGE[NSEC][3][4] = {      /* rows A, B, C */
    {{P_TABLE, P_WAVE, P_SEMI, P_FINE}, {P_UNI, P_UDET, P_USPR, P_OKT}, {P_FMS, P_FMA, P_SYNC, P_OBEND}},   /* OSC (the levels: MIX;
                                                   row C: FM) */
    {{P_L1, P_L2, P_L3, P_NOISE}, {P_BAL1, P_BAL2, P_BAL3, P_BALN}, {P_RING, P_BALR, P_COLOR, 0}},   /* MIX */
    {{P_CUT, P_RES, P_FENV, P_KTRK}, {P_FTYPE, P_ROUTE, P_DRIVE, P_FVEL}, {P_DCURVE, P_FFMS, P_FFMA, P_FPAN}},   /* FILTER
                                                   1/2 (row C: DRIVE's curve, Filter FM, PAN) */
    {{P_ATK, P_DEC, P_SUS, P_REL}, {P_EMODE, P_EALVL, P_EDEC2, P_ESUS2}, {P_EVEL, P_ETRIG, 0, 0}},   /* ENV: AMP FLT 3 4 */
    {{0}, {0}},                                 /* MOD (stage 4) */
    {{0}, {0}},                                 /* FX (stage 7) */
    {{P_VOL, P_GLIDE, P_VMODE, P_BEND}, {P_MORPH, P_GMODE, P_PCAT, 0}},   /* PATCH */
    {{P_AMODE, P_ADIR, P_AORD, P_AOCT}, {P_ATEMPO, P_ARATE, P_AGATE, P_ASWING}, {P_APAT, P_AVEL, 0, 0}},   /* ARP: LOAD copies
                                                   a library template into this patch; SEQ edits only this copy */
    {{P_MIDI, P_VCURVE, P_KEYVEL, P_TUNE}, {P_TRANS, 0, 0, 0}, {P_CTLW, P_CTLX, P_CTLY, P_CTLZ}},   /* SETUP (row C:
                                                   the CCs of the matrix's sources CTL W..Z) */
    {{P_STYPE, P_SACC, P_SLEN, P_STIM}, {P_SGLIDE, P_APLEN, P_ATFAC, 0}}};   /* STEPS: the step SELECT (or a
                                                   white key) picks; row B: its glide; the pattern's STEPS and T.AMT */

static uint32_t sec_rows(uint32_t s)
{
    return s == SEC_OSC ? 4u                     /* (OSC: row D, LIMIT WT, 2026-11-16) */
         : s == SEC_MIX || s == SEC_ARP || s == SEC_ENV || s == SEC_SETUP || s == SEC_FILT ? 3u
         : s == SEC_STEPS || s == SEC_PATCH || s == SEC_FX ? 2u
         : s == SEC_MOD ? 4u : 1u;                /* (MOD: the LFO's two rows, MATRIX, MODIF) */
}
static uint8_t *sec_row(uint32_t s)             /* the row a section shows (sections with one row: none) */
{
    return s == SEC_OSC ? &set.osc_row : s == SEC_MIX ? &set.mix_row : s == SEC_FILT ? &set.filt_row : s == SEC_ENV ? &set.env_row
         : s == SEC_SETUP ? &set.setup_row : s == SEC_ARP ? &set.arp_row : s == SEC_STEPS ? &set.steps_row
         : s == SEC_MOD ? &set.mod_row : s == SEC_PATCH ? &set.patch_row : s == SEC_FX ? &set.fx_row : 0;
}
static uint32_t ui_row(void)
{
    return sec_row(set.sec) ? *sec_row(set.sec) : 0u;
}
static uint32_t page_param(uint32_t s, uint32_t row, uint32_t k)   /* what knob k turns on section s's row */
{
    if (s == SEC_MOD) return row < 2u ? PAGE_LFO[row][k] : row == 2u ? PAGE_SLOT[k] : PAGE_MODF[k];   /* (rows A B:
                                                   the LFO; C: MATRIX; D: a modifier) */
    if (s == SEC_OSC && row == 2u && k == 2u && set.osc_sel != 1u) return 0;   /* (SYNC: OSC2's only) */
    if (s == SEC_OSC && row == 3u) return k == 0u ? P_WLIM : 0;   /* (row D: LIMIT WT) */
    if (s == SEC_FX) return set.fx_sel >= FX_EQ ? PAGE_EQ[row & 1u][k] : PAGE_FX[set.p.fx[set.fx_sel].type % FX_N][row & 1u][k];
    return PAGE[s][row][k];
}
static int32_t param_hi(uint32_t id)            /* a parameter's top (a synced LFO's RATE: the note values) */
{
    if (id == P_MDST) return (int32_t)DGROUP[dst_group((*pslot(&set.p, set.mod_slot)).dst, 0)].n - 1;   /* (its group) */
    if (id == P_DTIME && set.p.fx[set.fx_sel].p[2]) return (int32_t)ARP_NRATE - 1;   /* (a synced delay: the notes) */
    if (id == P_WRATE && set.p.fx[set.fx_sel].p[3] == FS_SYNC) return (int32_t)ARP_NRATE - 1;   /* (FILTER's SYNC) */
    return id == P_LRATE && set.p.lfo[set.mod_lfo].sync ? (int32_t)ARP_NRATE - 1 : PARAM[id].hi;
}
static uint32_t sec_insts(uint32_t s)           /* what SELECT picks from: OSC 1/2/3, FILTER 1/2, ENV AMP/FLT */
{
    return s == SEC_OSC ? NOSC : s == SEC_FILT ? 2u : s == SEC_FX ? 3u : s == SEC_ENV ? NENV : s == SEC_STEPS ? ARP_PLEN
         : s == SEC_MOD ? (set.mod_row == 2u ? NMSLOT : set.mod_row == 3u ? NMODF : NLFO) : 0u;   /* (MOD: the
                                                   LFOs, MATRIX: the slots, MODIF: the modifiers) */
}
static uint8_t *sec_inst(uint32_t s)            /* the one picked */
{
    return s == SEC_OSC ? &set.osc_sel : s == SEC_FILT ? &set.filt_sel : s == SEC_STEPS ? &set.step_sel
         : s == SEC_FX ? &set.fx_sel
         : s == SEC_MOD ? (set.mod_row == 2u ? &set.mod_slot : set.mod_row == 3u ? &set.mod_mf : &set.mod_lfo)
         : &set.env_sel;
}

static uint32_t ui_foot_flag;                   /* (the footer shows the arp's mode: draw it again) */
static void arp_mode_set(uint32_t m)            /* OFF, ON, ONE SHOT, HOLD: RUN brings back the last one on */
{
    if (m >= ARP_NMODE || m == set.p.arp.mode) return;
    set.p.arp.mode = (uint8_t)m;
    if (m != ARP_OFF) set.p.arp.last = (uint8_t)m;
    arp_reset();                                /* (nothing hangs: its note ends, the voices let go) */
    ui_foot_flag = 1;
}

static uint16_t *cur_step(void)                 /* the step STEPS edits: of the pattern playing (or the first) */
{
    return &pat_st(pat_num())[set.step_sel % ARP_PLEN];
}

/* TABLE on the knob: the own tables, ANALOG, the local ones, then the user slots (stored: own 0.., local WT_PUB_N..,
 * user TAB_U0.., ANALOG TAB_VA); the knob's last position: NTAB + UWT_N */
static uint32_t tab_pos(uint32_t t)
{
    return t == TAB_VA ? WT_PUB_N : t < WT_PUB_N ? t : t >= TAB_U0 && t < TAB_U0 + UWT_N ? NTAB + 1u + (t - TAB_U0)
         : t + 1u;
}
static uint32_t tab_of(int32_t v)
{
    return v <= 0 ? 0u : v < (int32_t)WT_PUB_N ? (uint32_t)v : v == (int32_t)WT_PUB_N ? TAB_VA
         : v <= (int32_t)NTAB ? (uint32_t)v - 1u
         : UWT_N && v <= (int32_t)(NTAB + UWT_N) ? TAB_U0 + (uint32_t)v - NTAB - 1u : NTAB - 1u;
}
static int32_t pget(uint32_t id)
{
    const osc_set_t *o = &set.p.osc[set.osc_sel];
    const filt_set_t *q = &set.p.flt[set.filt_sel];
    const env_set_t *en = &set.p.env[set.env_sel];
    switch (id) {
    case P_WAVE: return o->wave;
    case P_TABLE: return (int32_t)tab_pos(o->table);
    case P_SEMI: return o->semi;
    case P_FINE: return o->cents;
    case P_L1: case P_L2: case P_L3: return set.p.osc[id - P_L1].level;
    case P_NOISE: return set.p.noise;
    case P_RING: return set.p.ring;
    case P_COLOR: return set.p.color;
    case P_BAL1: case P_BAL2: case P_BAL3: case P_BALN: case P_BALR: return set.p.bal[id - P_BAL1];
    case P_CUT: return q->cut;
    case P_RES: return q->res;
    case P_FENV: return q->env;
    case P_KTRK: return q->ktrk;
    case P_FTYPE: return (int32_t)ftype_pos(q->type);
    case P_ROUTE: return set.p.route;
    case P_DRIVE: return q->drive;
    case P_FVEL: return q->velo;
    case P_ATK: return en->a;
    case P_DEC: return en->d;
    case P_SUS: return en->s;
    case P_REL: return en->r;
    case P_EVEL: return en->vel;
    case P_EMODE: return en->mode;
    case P_EALVL: return en->al;
    case P_EDEC2: return en->d2;
    case P_ESUS2: return en->s2;
    case P_VOL: return set.p.volume;
    case P_MORPH: return set.morph;
    case P_MIDI: return set.ch;
    case P_AMODE: return set.p.arp.mode;
    case P_ADIR: return set.p.arp.dir;
    case P_AORD: return set.p.arp.order;
    case P_AOCT: return set.p.arp.oct;
    case P_ATEMPO: return set.p.arp.tempo;
    case P_ARATE: return set.p.arp.rate;
    case P_AGATE: return set.p.arp.gate;
    case P_ASWING: return set.p.arp.swing;
    case P_AVEL: return set.p.arp.vel;
    case P_LSHAPE: return set.p.lfo[set.mod_lfo].shape;
    case P_LRATE: return set.p.lfo[set.mod_lfo].rate;
    case P_LSYNC: return set.p.lfo[set.mod_lfo].sync;
    case P_LDELAY: return set.p.lfo[set.mod_lfo].delay;
    case P_LKEY: return set.p.lfo[set.mod_lfo].key;
    case P_LPHASE: return set.p.lfo[set.mod_lfo].phase;
    case P_MSRC: return (*pslot(&set.p, set.mod_slot)).src;
    case P_MDST: { uint32_t at; dst_group((*pslot(&set.p, set.mod_slot)).dst, &at); return (int32_t)at; }   /* (in its group) */
    case P_MGRP: return (int32_t)dst_group((*pslot(&set.p, set.mod_slot)).dst, 0);
    case P_MAMT: return (*pslot(&set.p, set.mod_slot)).amt;
    case P_GLIDE: return set.p.glide;
    case P_VMODE: return set.p.vmode;
    case P_BEND: return set.p.bend;
    case P_UNI: return set.p.uni[set.osc_sel];
    case P_UDET: return set.p.udet[set.osc_sel];
    case P_USPR: return set.p.uspr[set.osc_sel];
    case P_FXTYPE: return set.p.fx[set.fx_sel].type;
    case P_FXMIX: return set.p.fx[set.fx_sel].mix;
    case P_DTIME: case P_RSIZE: case P_XDRIVE: return set.p.fx[set.fx_sel].p[0];
    case P_DFB: case P_RDAMP: case P_XTONE: return set.p.fx[set.fx_sel].p[1];
    case P_DSYNC: case P_RPRE: case P_XSHAPE: return set.p.fx[set.fx_sel].p[2];
    case P_DTONE: case P_RWIDTH: case P_XLEVEL: return set.p.fx[set.fx_sel].p[3];
    case P_DPING: return set.p.fx[set.fx_sel].p[4];
    case P_CRATE: case P_CDEPTH: case P_CMODE: case P_CWIDTH: case P_CDLY:
        return set.p.fx[set.fx_sel].p[id == P_CRATE ? 0 : id == P_CDEPTH ? 1 : id == P_CMODE ? 2 : id == P_CWIDTH ? 4 : 5];
    case P_CFB: return (int32_t)set.p.fx[set.fx_sel].p[3] - 64;
    case P_EQLO: case P_EQMID: case P_EQHI: return set.p.eq.gain[id - P_EQLO];
    case P_EQLF: case P_EQMF: case P_EQHF: return set.p.eq.note[id - P_EQLF];
    case P_EQQ: return set.p.eq.q;
    case P_PRATE: case P_PDEPTH: case P_PFREQ: case P_PSTG: case P_PWIDTH:
        return set.p.fx[set.fx_sel].p[id == P_PRATE ? 0 : id == P_PDEPTH ? 1 : id == P_PFREQ ? 2 : id == P_PSTG ? 4 : 5];
    case P_PFB: return (int32_t)set.p.fx[set.fx_sel].p[3] - 64;
    case P_ERATE: case P_EDEPTH: case P_EVIB: case P_EVRATE: case P_ETONE: case P_EWIDTH:
        return set.p.fx[set.fx_sel].p[id - P_ERATE];
    case P_ORATE: case P_OBITS: case P_OTONE: case P_OWOB: case P_ONOISE: return set.p.fx[set.fx_sel].p[id - P_ORATE];
    case P_TRATE: case P_TDEPTH: case P_TCHO: case P_TSH: case P_TDRV: return set.p.fx[set.fx_sel].p[id - P_TRATE];
    case P_WCUT: case P_WRES: case P_WTYPE: case P_WSRC: case P_WRATE:
        return set.p.fx[set.fx_sel].p[id == P_WRATE ? 5 : id - P_WCUT];
    case P_WDEPTH: return (int32_t)set.p.fx[set.fx_sel].p[4] - 64;
    case P_APAT: case P_ACOPY: return (int32_t)set.p.pat_source;
    case P_APLEN: return *pat_len(pat_num());
    case P_ATFAC: return *pat_tfac(pat_num());
    case P_STYPE: return (int32_t)AST_TYPE(*cur_step());
    case P_SACC: return (int32_t)AST_ACC(*cur_step());
    case P_SLEN: return AST_TIE(*cur_step()) ? 8 : (int32_t)AST_LEN(*cur_step());   /* (8: TIE) */
    case P_STIM: return (int32_t)AST_TIM(*cur_step());
    case P_SGLIDE: return (int32_t)AST_GLIDE(*cur_step());
    case P_VCURVE: return set.vcurve;
    case P_KEYVEL: return set.keyvel;
    case P_TUNE: return set.tune;
    case P_TRANS: return set.trans;
    case P_SYNC: return set.p.sync;
    case P_GMODE: return set.p.gmode;
    case P_PCAT: return set.p.cat;
    case P_LFADE: return set.p.lfout >> set.mod_lfo & 1u;
    case P_ETRIG: return set.p.etrig >> set.env_sel & 1u;
    case P_WLIM: return set.p.wlim >> set.osc_sel & 1u;
    case P_OKT: return set.p.okt[set.osc_sel];
    case P_OBEND: return set.p.obend[set.osc_sel];
    case P_DCURVE: return (int32_t)dcurve_pos(set.p.dcurve[set.filt_sel & 1u]);
    case P_FFMS: return set.p.ffms[set.filt_sel & 1u];
    case P_FFMA: return set.p.ffma[set.filt_sel & 1u];
    case P_FPAN: return set.p.fpan[set.filt_sel & 1u];
    case P_FMS: return set.p.fms[set.osc_sel];
    case P_FMA: return set.p.fma[set.osc_sel];
    case P_FA: return set.p.modf[set.mod_mf % NMODF].a;
    case P_FOP: return set.p.modf[set.mod_mf % NMODF].op;
    case P_FB: return set.p.modf[set.mod_mf % NMODF].b;
    case P_FC: return set.p.modf[set.mod_mf % NMODF].c;
    case P_CTLW: case P_CTLX: case P_CTLY: case P_CTLZ: return set.ctl[id - P_CTLW];
    default: return 0;
    }
}
static void pset(uint32_t id, int32_t v)
{
    osc_set_t *o = &set.p.osc[set.osc_sel];
    filt_set_t *q = &set.p.flt[set.filt_sel];
    env_set_t *en = &set.p.env[set.env_sel];
    switch (id) {
    case P_WAVE: o->wave = (uint8_t)v; break;
    case P_TABLE: o->table = (uint8_t)tab_of(v); break;
    case P_SYNC: set.p.sync = (uint8_t)v; break;
    case P_GMODE: set.p.gmode = (uint8_t)v; break;
    case P_PCAT: set.p.cat = (uint8_t)v; break;
    case P_ETRIG: set.p.etrig = (uint8_t)((set.p.etrig & ~(1u << set.env_sel)) | (uint32_t)(v & 1) << set.env_sel); break;
    case P_LFADE: set.p.lfout = (uint8_t)((set.p.lfout & ~(1u << set.mod_lfo)) | (uint32_t)(v & 1) << set.mod_lfo); break;
    case P_WLIM: set.p.wlim = (uint8_t)((set.p.wlim & ~(1u << set.osc_sel)) | (uint32_t)(v & 1) << set.osc_sel); break;
    case P_OKT: set.p.okt[set.osc_sel] = (int8_t)v; break;
    case P_OBEND: set.p.obend[set.osc_sel] = (uint8_t)v; break;
    case P_DCURVE: set.p.dcurve[set.filt_sel & 1u] = DCURVE_ORDER[v < 0 ? 0 : v >= (int32_t)DC_N ? DC_N - 1 : v]; break;
    case P_FFMS: set.p.ffms[set.filt_sel & 1u] = (uint8_t)v; break;
    case P_FFMA: set.p.ffma[set.filt_sel & 1u] = (uint8_t)v; break;
    case P_FPAN: set.p.fpan[set.filt_sel & 1u] = (int8_t)v; break;
    case P_FMS: set.p.fms[set.osc_sel] = (uint8_t)v; break;
    case P_FMA: set.p.fma[set.osc_sel] = (uint8_t)v; break;
    case P_FA: set.p.modf[set.mod_mf % NMODF].a = (uint8_t)v; break;
    case P_FOP: set.p.modf[set.mod_mf % NMODF].op = (uint8_t)v; break;
    case P_FB: set.p.modf[set.mod_mf % NMODF].b = (uint8_t)v; break;
    case P_FC: set.p.modf[set.mod_mf % NMODF].c = (int8_t)v; break;
    case P_CTLW: case P_CTLX: case P_CTLY: case P_CTLZ: set.ctl[id - P_CTLW] = (uint8_t)v; break;
    case P_SEMI: o->semi = (int8_t)v; break;
    case P_FINE: o->cents = (int8_t)v; break;
    case P_L1: case P_L2: case P_L3: set.p.osc[id - P_L1].level = (uint8_t)v; break;
    case P_NOISE: set.p.noise = (uint8_t)v; break;
    case P_RING: set.p.ring = (uint8_t)v; break;
    case P_COLOR: set.p.color = (uint8_t)v; break;
    case P_BAL1: case P_BAL2: case P_BAL3: case P_BALN: case P_BALR: set.p.bal[id - P_BAL1] = (int8_t)v; break;
    case P_CUT: q->cut = (uint8_t)v; break;
    case P_RES: q->res = (uint8_t)v; break;
    case P_FENV: q->env = (int8_t)v; break;
    case P_KTRK: q->ktrk = (uint8_t)v; break;
    case P_FTYPE: q->type = FTYPE_ORDER[v < 0 ? 0 : v >= (int32_t)SVF_NTYPE ? SVF_NTYPE - 1 : v]; break;
    case P_ROUTE: set.p.route = (uint8_t)v; break;
    case P_DRIVE: q->drive = (uint8_t)v; break;
    case P_FVEL: q->velo = (int8_t)v; break;
    case P_ATK: en->a = (uint8_t)v; break;
    case P_DEC: en->d = (uint8_t)v; break;
    case P_SUS: en->s = (uint8_t)v; break;
    case P_REL: en->r = (uint8_t)v; break;
    case P_EVEL: en->vel = (int8_t)v; break;
    case P_EMODE: en->mode = (uint8_t)v; break;
    case P_EALVL: en->al = (uint8_t)v; break;
    case P_EDEC2: en->d2 = (uint8_t)v; break;
    case P_ESUS2: en->s2 = (uint8_t)v; break;
    case P_VOL: set.p.volume = (uint8_t)v; break;
    case P_MORPH: set.morph = (int8_t)v; break;
    case P_MIDI: set.ch = (uint8_t)v; break;
    case P_AMODE: arp_mode_set((uint32_t)v); break;
    case P_ADIR: set.p.arp.dir = (uint8_t)v; break;
    case P_AORD: set.p.arp.order = (uint8_t)v; break;
    case P_AOCT: set.p.arp.oct = (uint8_t)v; break;
    case P_ATEMPO: set.p.arp.tempo = (uint16_t)v; break;
    case P_ARATE: set.p.arp.rate = (uint8_t)v; break;
    case P_AGATE: set.p.arp.gate = (uint8_t)v; break;
    case P_ASWING: set.p.arp.swing = (uint8_t)v; break;
    case P_AVEL: set.p.arp.vel = (uint8_t)v; break;
    case P_LSHAPE: set.p.lfo[set.mod_lfo].shape = (uint8_t)v; break;
    case P_LRATE: set.p.lfo[set.mod_lfo].rate = (uint8_t)v; break;
    case P_LSYNC: {                             /* (the rate's meaning changes: a note value or a speed) */
        lfo_set_t *lf = &set.p.lfo[set.mod_lfo];
        lf->sync = (uint8_t)v;
        lf->rate = v ? (uint8_t)(lf->rate * (ARP_NRATE - 1u) / 127u) : (uint8_t)(lf->rate * 127u / (ARP_NRATE - 1u));
        break;
    }
    case P_LDELAY: set.p.lfo[set.mod_lfo].delay = (uint8_t)v; break;
    case P_LKEY: set.p.lfo[set.mod_lfo].key = (uint8_t)v; break;
    case P_LPHASE: set.p.lfo[set.mod_lfo].phase = (uint8_t)v; break;
    case P_MSRC: (*pslot(&set.p, set.mod_slot)).src = (uint8_t)v; break;
    case P_MDST: (*pslot(&set.p, set.mod_slot)).dst = DGROUP[dst_group((*pslot(&set.p, set.mod_slot)).dst, 0)].d[v]; break;
    case P_MGRP: (*pslot(&set.p, set.mod_slot)).dst = DGROUP[v % NDG].d[0]; break;   /* (a group: its first) */
    case P_MAMT: (*pslot(&set.p, set.mod_slot)).amt = (int8_t)v; break;
    case P_GLIDE: set.p.glide = (uint8_t)v; break;
    case P_VMODE: set.p.vmode = (uint8_t)v; break;   /* (sound_apply lets every voice go) */
    case P_BEND: set.p.bend = (uint8_t)v; break;
    case P_UNI: set.p.uni[set.osc_sel] = (uint8_t)v; break;
    case P_UDET: set.p.udet[set.osc_sel] = (uint8_t)v; break;
    case P_USPR: set.p.uspr[set.osc_sel] = (uint8_t)v; break;
    case P_FXTYPE: set.p.fx[set.fx_sel] = FX_DEF[v % FX_N]; break;   /* (a new type: its starting values) */
    case P_FXMIX: set.p.fx[set.fx_sel].mix = (uint8_t)v; break;
    case P_DTIME: case P_RSIZE: case P_XDRIVE: set.p.fx[set.fx_sel].p[0] = (uint8_t)v; break;
    case P_DFB: case P_RDAMP: case P_XTONE: set.p.fx[set.fx_sel].p[1] = (uint8_t)v; break;
    case P_XSHAPE: set.p.fx[set.fx_sel].p[2] = (uint8_t)v; break;
    case P_DSYNC: {                             /* (TIME's meaning changes: a note value or a time) */
        fx_set_t *f = &set.p.fx[set.fx_sel];
        f->p[2] = (uint8_t)v;
        f->p[0] = v ? (uint8_t)(f->p[0] * (ARP_NRATE - 1u) / 127u) : (uint8_t)(f->p[0] * 127u / (ARP_NRATE - 1u));
        break;
    }
    case P_RPRE: set.p.fx[set.fx_sel].p[2] = (uint8_t)v; break;
    case P_DTONE: case P_RWIDTH: case P_XLEVEL: set.p.fx[set.fx_sel].p[3] = (uint8_t)v; break;
    case P_DPING: set.p.fx[set.fx_sel].p[4] = (uint8_t)v; break;
    case P_CRATE: case P_CDEPTH: case P_CMODE: case P_CWIDTH: case P_CDLY:
        set.p.fx[set.fx_sel].p[id == P_CRATE ? 0 : id == P_CDEPTH ? 1 : id == P_CMODE ? 2 : id == P_CWIDTH ? 4 : 5] = (uint8_t)v;
        break;
    case P_CFB: set.p.fx[set.fx_sel].p[3] = (uint8_t)(v + 64); break;
    case P_EQLO: case P_EQMID: case P_EQHI: set.p.eq.gain[id - P_EQLO] = (int8_t)v; break;
    case P_EQLF: case P_EQMF: case P_EQHF: set.p.eq.note[id - P_EQLF] = (uint8_t)v; break;
    case P_EQQ: set.p.eq.q = (uint8_t)v; break;
    case P_PRATE: case P_PDEPTH: case P_PFREQ: case P_PSTG: case P_PWIDTH:
        set.p.fx[set.fx_sel].p[id == P_PRATE ? 0 : id == P_PDEPTH ? 1 : id == P_PFREQ ? 2 : id == P_PSTG ? 4 : 5] = (uint8_t)v;
        break;
    case P_PFB: set.p.fx[set.fx_sel].p[3] = (uint8_t)(v + 64); break;
    case P_ERATE: case P_EDEPTH: case P_EVIB: case P_EVRATE: case P_ETONE: case P_EWIDTH:
        set.p.fx[set.fx_sel].p[id - P_ERATE] = (uint8_t)v;
        break;
    case P_TRATE: case P_TDEPTH: case P_TCHO: case P_TSH: case P_TDRV:
        set.p.fx[set.fx_sel].p[id - P_TRATE] = (uint8_t)v;
        break;
    case P_ORATE: case P_OBITS: case P_OTONE: case P_OWOB: case P_ONOISE:
        set.p.fx[set.fx_sel].p[id - P_ORATE] = (uint8_t)v;
        break;
    case P_WCUT: case P_WRES: case P_WTYPE: case P_WRATE:
        set.p.fx[set.fx_sel].p[id == P_WRATE ? 5 : id - P_WCUT] = (uint8_t)v;
        break;
    case P_WSRC: {                              /* (to or from SYNC: RATE's meaning changes, a note value or a speed) */
        fx_set_t *f = &set.p.fx[set.fx_sel];
        int was = f->p[3] == FS_SYNC, is = v == FS_SYNC;
        f->p[3] = (uint8_t)v;
        if (is && !was) f->p[5] = (uint8_t)(f->p[5] * (ARP_NRATE - 1u) / 127u);
        else if (was && !is) f->p[5] = (uint8_t)(f->p[5] * 127u / (ARP_NRATE - 1u));
        break;
    }
    case P_WDEPTH: set.p.fx[set.fx_sel].p[4] = (uint8_t)(v + 64); break;
    case P_APAT: case P_ACOPY:                   /* COPY is the old panel command for LOAD. */
        set.p.pat_source = (uint32_t)v;
        if (v > 0 && v <= ARP_NPAT) {
            uint32_t i;
            for (i = 0; i < ARP_PLEN; i++) set.p.ost[i] = patlib.bank.st[v - 1][i];
            set.p.arp.plen = patlib.bank.len[v - 1]; set.p.arp.tfac = patlib.bank.tfac[v - 1];
            set.p.arp.pattern = PAT_OWN; set.p.patver = 1;
        }
        break;
    case P_APLEN: *pat_len(pat_num()) = (uint8_t)v; break;
    case P_ATFAC: *pat_tfac(pat_num()) = (uint8_t)v; break;
    case P_STYPE: case P_SACC: case P_SLEN: case P_STIM: case P_SGLIDE: {
        uint16_t *st = cur_step(), x = *st;
        uint32_t ty = AST_TYPE(x), gl = AST_GLIDE(x), ac = AST_ACC(x), ln = AST_LEN(x), tm = AST_TIM(x), tie = AST_TIE(x);
        if (id == P_STYPE) ty = (uint32_t)v; else if (id == P_SACC) ac = (uint32_t)v;
        else if (id == P_SLEN) { tie = v == 8; ln = v == 8 ? 4u : (uint32_t)v; }   /* (LENGTH past +3: TIE) */
        else if (id == P_STIM) tm = (uint32_t)v; else gl = (uint32_t)v;
        *st = (uint16_t)(AST_MAKE(ty, gl, ac, ln, tm) | (tie ? AST_TIEBIT : 0u));
        break;
    }
    case P_VCURVE: set.vcurve = (uint8_t)v; break;
    case P_KEYVEL: set.keyvel = (uint8_t)v; break;
    case P_TUNE: set.tune = (int8_t)v; break;
    case P_TRANS: set.trans = (int8_t)v; break;
    default: break;
    }
}

/* ---- the screen's state */
static struct {
    uint32_t cells, hero, foot;                 /* to draw again */
    int32_t focus;                              /* the cell last turned (-1: none), lit for 2 s */
    uint32_t focus_t, msg_t, shift, t_anim, t_cpu, cpu, cpu_n;
    uint32_t cpu_vox, cpu_fx, cpu_out, cpu_avg;  /* the longest block's split (%): voices, effects, the rest; the mean */
    uint32_t shift_sel;                         /* SELECT turned while SHIFT is held: the page, not SHIFT's help */
    uint8_t peek_on, peek_s, peek_prev;         /* a section's key held: its section, the one to go back to */
    uint32_t peek_t;                            /* (when it was pressed) */
    uint8_t ma_on, ma_used;                     /* MOD held on another page: the knobs make a matrix slot; one made */
    int8_t ma_slot;                             /* (the slot made or found; -1: none yet) */
    uint32_t voices_drawn, env_drawn, pos_drawn, cut_drawn, arp_drawn;
    uint32_t live_drawn, foot_drawn, band_t;    /* the cells' live marks; the footer's MIDI dot and tempo; the bands'
                                                   window */
    int32_t band_lo[2], band_hi[2];             /* where the modulation took WAVE (0) and CUTOFF (1) lately */
    char msg[28];                               /* a word for 1.2 s in the footer ("MORPH +12") */
    uint32_t save, save_to, cur, patch_drawn;   /* SAVE: asking (the number, the cursor in the name); the patch shown */
    char sname[12];                             /* the name being written */
    uint8_t cpu_hist[48];                       /* the audio's load, every half second */
} ui = {1, 1, 1, -1};

static char *put_str(char *p, const char *a) { while (*a) *p++ = *a++; return p; }
static char *put_int(char *p, int32_t v, int sign)   /* v in decimal (with + when sign); returns the end */
{
    char t[12];
    uint32_t n = 0, u = (uint32_t)(v < 0 ? -v : v);
    if (v < 0) *p++ = '-';
    else if (sign && v > 0) *p++ = '+';
    do { t[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (n) *p++ = t[--n];
    return p;
}
static char *put_hex6(char *p, uint32_t v)
{
    int32_t i;
    for (i = 20; i >= 0; i -= 4) *p++ = "0123456789ABCDEF"[(v >> i) & 15u];
    return p;
}
static void ui_msg(const char *a, int32_t v, int with_v)
{
    char *p = put_str(ui.msg, a);
    if (with_v) { *p++ = ' '; p = put_int(p, v, 1); }
    *p = 0;
    ui.msg_t = fm1_ms;
    ui.foot = 1;                                /* (the footer shows it) */
}
static int32_t ctext(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)   /* centred */
{
    return cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}
static void dot(int32_t x, int32_t y, uint16_t c, int filled)   /* a small round mark, 9 x 9 around (x, y) */
{
    if (filled) {
        cv_rect(x - 3, y - 4, 7, 9, c);
        cv_rect(x - 4, y - 3, 9, 7, c);
    } else {
        cv_rect(x - 2, y - 4, 5, 1, c); cv_rect(x - 2, y + 4, 5, 1, c);
        cv_rect(x - 4, y - 2, 1, 5, c); cv_rect(x + 4, y - 2, 1, 5, c);
        cv_pset(x - 3, y - 3, c); cv_pset(x + 3, y - 3, c); cv_pset(x - 3, y + 3, c); cv_pset(x + 3, y + 3, c);
    }
}

/* ---- the cells: label, value (big if it fits in 3 characters), a bar (from the centre when it can go below 0) */
static const char NAME_CH[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.";   /* what a name is made of */
static void cell_card(int32_t x0, int hot, uint16_t col)   /* a cell's card, 58 x 55 (the knob last turned: tinted,
                                                   a rim in the section's colour) */
{
    if (hot) cv_rrect_o(x0 + 1, 0, 58, 55, 6, mixc(C_CARD, col, 3u), mixc(C_CARD, col, 12u));
    else cv_rrect(x0 + 1, 0, 58, 55, 6, C_CARD);
}
static void draw_save_cells(void)               /* SAVE asking: KNOB1 the number (PRESETS too), KNOB2 the cursor, KNOB3
                                                   the letter: each cell over its knob */
{
    static const char *const L[3] = {"TO", "CURSOR", "LETTER"};
    uint32_t i;
    char v[3][4];
    v[0][0] = (char)('0' + ui.save_to / 10u); v[0][1] = (char)('0' + ui.save_to % 10u); v[0][2] = 0;
    v[1][0] = (char)('0' + (ui.cur + 1u) / 10u); v[1][1] = (char)('0' + (ui.cur + 1u) % 10u); v[1][2] = 0;
    v[2][0] = ui.sname[ui.cur] == ' ' ? '_' : ui.sname[ui.cur]; v[2][1] = 0;
    cv_begin(240, 56, C_BG);
    for (i = 0; i < 3u; i++) {
        int32_t x0 = (int32_t)i * 60;
        cell_card(x0, 0, C_AMBER);
        ctext(x0 + 30, 3, &FONT_SM, L[i], C_AMBER);
        ctext(x0 + 30, 14, &FONT_LG, v[i], C_WHITE);
    }
    cell_card(180, 0, C_AMBER);
    ctext(210, 19, &FONT_MD, "-", C_DARK);
    cv_blit(0, 0);
}
static uint32_t osc_pos_shown(uint32_t k);
static uint32_t newest_voice(void)              /* the newest sounding voice (NV: none) */
{
    uint32_t v, best = NV;
    for (v = 0; v < NV; v++)
        if (voice[v].busy && (best == NV || voice[v].age - voice[best].age < 0x80000000u)) best = v;
    return best;
}
/* the value the newest sounding voice has now, in the knob's units: 0 WAVE (of the oscillator picked), 1 CUTOFF (of
 * the filter picked); 0: nothing sounds, or the matrix does not move it */
static int cell_live(int32_t which, int32_t *val)
{
    uint32_t v = newest_voice(), k;
    int moved = 0;
    if (v == NV) return 0;
    for (k = 0; k < NMSLOT; k++) {              /* (only when a slot moves it: a still mark says nothing new) */
        uint32_t d = pslot(&set.p, k)->dst;
        if (!pslot(&set.p, k)->amt || !pslot(&set.p, k)->src) continue;
        if (which == 0 && (d == MD_WAVE || d == MD_W1 + set.osc_sel)) moved = 1;
        if (which == 1 && (d == MD_CUT || d == MD_CUT1 + set.filt_sel || d == MD_FENV1 + set.filt_sel)) moved = 1;
    }
    if (!moved) return 0;
    if (which == 0) *val = (int32_t)((osc_pos_shown(set.osc_sel) + 0x8000u) >> 16);
    else *val = clampi((voice[v].fn[set.filt_sel] >> 8) - 4, 0, 127);
    return 1;
}
/* a value as a cell shows it (and the editor, 2026-11-17): into e, its end returned (not terminated) */
static char *pval_text(uint32_t id, int32_t v, char *e)
{
    const param_t *pr = &PARAM[id];
    if (pr->fmt == F_MIDI && v == 0) e = put_str(e, "OMNI");
    else if (pr->fmt == F_FTYPE) e = put_str(e, FTYPE_NAME[v]);
    else if (pr->fmt == F_CURVE) e = put_str(e, CURVE_NAME[v]);
    else if (pr->fmt == F_AMODE) e = put_str(e, AMODE_NAME[v]);
    else if (pr->fmt == F_ADIR) e = put_str(e, ADIR_NAME[v]);
    else if (pr->fmt == F_AORD) e = put_str(e, AORD_NAME[v]);
    else if (pr->fmt == F_ARATE) e = put_str(e, ARP_RATE_NAME[v]);
    else if (pr->fmt == F_AGATE && v == 100) e = put_str(e, "LEG");
    else if (pr->fmt == F_AVEL) e = put_str(e, AVEL_NAME[v]);
    else if (pr->fmt == F_APAT && v == 0) e = put_str(e, "-");
    else if (pr->fmt == F_APAT && v == PAT_OWN) e = put_str(e, "OWN");
    else if (pr->fmt == F_ACOPY && v == 0) e = put_str(e, "-");
    else if (pr->fmt == F_STYPE) e = put_str(e, STYPE_NAME[v]);
    else if (pr->fmt == F_SACC) e = put_str(e, SACC_NAME[v]);
    else if (pr->fmt == F_SLEN) e = put_str(e, v == 8 ? "TIE" : v ? SOFF_NAME[v] : "LEG");
    else if (pr->fmt == F_STIM) e = put_str(e, v ? SOFF_NAME[v] : "RAND");
    else if (pr->fmt == F_ONOFF) e = put_str(e, v ? "ON" : "OFF");
    else if (pr->fmt == F_LSHAPE) e = put_str(e, LSHAPE_NAME[v]);
    else if (pr->fmt == F_LRATE && set.p.lfo[set.mod_lfo].sync) e = put_str(e, ARP_RATE_NAME[v % ARP_NRATE]);
    else if (pr->fmt == F_MSRC) e = put_str(e, MSRC_NAME[v]);
    else if (pr->fmt == F_MDST) e = put_str(e, MDST_NAME[(*pslot(&set.p, set.mod_slot)).dst]);
    else if (pr->fmt == F_MGRP) e = put_str(e, DGROUP_NAME[v % NDG]);
    else if (pr->fmt == F_EMODE) e = put_str(e, EMODE_NAME[v]);
    else if (pr->fmt == F_MOP) e = put_str(e, MOP_NAME[v % MO_N]);
    else if (pr->fmt == F_FMS) e = put_str(e, FMS_NAME[v % FMS_N]);
    else if (pr->fmt == F_DCURVE) e = put_str(e, DCURVE_NAME[v % DC_N]);
    else if (pr->fmt == F_GMODE) e = put_str(e, GMODE_NAME[v % GM_N]);
    else if (pr->fmt == F_PCAT) e = put_str(e, PCAT_NAME[v % PC_N]);
    else if (pr->fmt == F_LFADE) e = put_str(e, v ? "OUT" : "IN");
    else if (pr->fmt == F_ETRIG) e = put_str(e, v ? "SINGLE" : "NORMAL");
    else if (pr->fmt == F_OKT) e = put_str(put_int(e, 100 + 2 * v, 0), "%");
    else if (pr->fmt == F_OBEND && v == 0) e = put_str(e, "PATCH");
    else if (pr->fmt == F_OBEND) e = put_int(e, v - 25, 1);
    else if (pr->fmt == F_MSRCB) e = put_str(e, v ? MSRC_NAME[v % MS_N] : "CONST");
    else if (pr->fmt == F_CC && v == 0) e = put_str(e, "OFF");
    else if (pr->fmt == F_VMODE) e = put_str(e, VMODE_NAME[v]);
    else if (pr->fmt == F_FXTYPE) e = put_str(e, FXTYPE_NAME[v % FX_N]);
    else if (pr->fmt == F_XSHAPE) e = put_str(e, XSHAPE_NAME[v % DS_N]);
    else if (pr->fmt == F_CMODE) e = put_str(e, CMODE_NAME[v % CM_N]);
    else if (pr->fmt == F_DTIME && set.p.fx[set.fx_sel].p[2]) e = put_str(e, ARP_RATE_NAME[v % ARP_NRATE]);
    else if (pr->fmt == F_DTIME) e = put_int(e, (int32_t)(fx_time(&set.p.fx[set.fx_sel]) * 10u / 441u), 0);
    else if (pr->fmt == F_GLIDE && v == 0) e = put_str(e, "OFF");
    else if (pr->fmt == F_ROUTE) e = put_str(e, v ? "PAR" : "SER");
    else if (pr->fmt == F_BAL && (v == -63 || v == 63)) e = put_str(e, v < 0 ? "F1" : "F2");
    else if (pr->fmt == F_HZ) e = put_hz_cell(e, note_hz(v));
    else if (pr->fmt == F_EQQ) e = put_dec(e, eq_q_x100((uint32_t)v), 1);
    else if (pr->fmt == F_PSTG) e = put_int(e, 2 * (v + 1), 0);
    else if (pr->fmt == F_SRATE) e = put_hz_cell(e, LF_HZ[v & 127]);
    else if (pr->fmt == F_BITS) e = put_dec(e, 1600 - v * 1400 / 127, 1);
    else if (pr->fmt == F_OTONE) e = put_hz_cell(e, note_hz(80 + v * 52 / 127));
    else if (pr->fmt == F_WCUT) e = put_hz_cell(e, note_hz(v + 4));
    else if (pr->fmt == F_WTYPE) e = put_str(e, WTYPE_NAME[v % FF_NTYPE]);
    else if (pr->fmt == F_WSRC) e = put_str(e, WSRC_NAME[v % FS_N]);
    else if (pr->fmt == F_WRATE && set.p.fx[set.fx_sel].p[3] == FS_SYNC) e = put_str(e, ARP_RATE_NAME[v % ARP_NRATE]);
    else if (id >= P_EQLO && id <= P_EQHI && !v) e = put_str(e, "0");   /* (the band off: no sign) */
    else e = put_int(e, v, pr->fmt == F_SIGNED);
    return e;
}
static void draw_cells(void)
{
    uint32_t i, row = ui_row();
    uint16_t col = SEC_COL[set.sec];
    if (ui.save) { draw_save_cells(); return; }
    cv_begin(240, 56, C_BG);
    for (i = 0; i < 4u; i++) {
        uint32_t id = page_param(set.sec, row, i);
        int32_t x0 = (int32_t)i * 60, v, span;
        int hot = (int32_t)i == ui.focus;
        const param_t *pr = &PARAM[id];
        const felucca_font_t *f;
        char s[12], *e = s;
        cell_card(x0, hot && id, col);
        if (!id) { ctext(x0 + 30, 19, &FONT_MD, "-", C_DARK); continue; }
        ctext(x0 + 30, 3, &FONT_SM, pr->label, hot ? col : dimc(col, 6));
        v = pget(id);
        e = pval_text(id, v, e);
        *e = 0;
        f = text_w(&FONT_LG, s) <= 54 ? &FONT_LG : text_w(&FONT_MD, s) <= 56 ? &FONT_MD : &FONT_SM;   /* (SM: a long
                                                   word, ENSEMBLE) */
        ctext(x0 + 30, f == &FONT_LG ? 14 : f == &FONT_MD ? 19 : 21, f, s, hot ? C_WHITE : C_VAL);
        cv_rect(x0 + 6, 45, 48, 3, hot ? mixc(C_CARD, col, 5u) : C_RULE);   /* the bar: its track, then the value */
        if (!hot) col = dimc(col, 6);
        span = param_hi(id) - pr->lo;                 /* (0: one value only, e.g. TABLE without local tables: no bar) */
        if (!span) ;
        else if (pr->lo < 0) {
            int32_t m = x0 + 30, ex = x0 + 6 + 48 * (v - pr->lo) / span;
            cv_rect(m < ex ? m : ex, 45, ex > m ? ex - m : m - ex ? m - ex : 1, 3, col);
            cv_rect(m, 43, 1, 7, C_OFF);        /* (the centre) */
        } else {
            cv_rect(x0 + 6, 45, 48 * (v - pr->lo) / span, 3, col);
        }
        col = SEC_COL[set.sec];
        {                                       /* WAVE, CUTOFF: where the matrix has it now (a white mark), where it
                                                   took it lately (a band over the bar) */
            int32_t lv, b = id == P_WAVE ? 0 : id == P_CUT ? 1 : -1;
            if (b >= 0 && cell_live(b, &lv) && span) {
                int32_t lo = ui.band_lo[b], hi = ui.band_hi[b];
                cv_rect(x0 + 6 + 48 * (lo - pr->lo) / span, 43, 48 * (hi - lo) / span + 1, 1, C_VAL);
                cv_rect(x0 + 5 + 48 * (lv - pr->lo) / span, 42, 2, 9, C_WHITE);
            }
        }
    }
    cv_blit(0, 0);
}

/* ---- the pictures (a 240 x 124 canvas, at y 60) */
static void wave_crv(const wt_row_t *tab, uint32_t pos, int32_t x0, int32_t y0)   /* a cycle at table position pos
                                                   into crv, 150 px from x0, +-40 px about y0 (tab 0: the ANALOG
                                                   table's shape) */
{
    int32_t i;
    for (i = 0; i <= 150; i++) {
        uint32_t ph = (uint32_t)i * 28633115u;  /* (2^32 / 150) */
        int32_t sm = tab ? wt_sample(tab, ph, pos) : va_shape(ph, pos);
        if (x0 + i >= 0 && x0 + i < 240) crv[x0 + i] = (int16_t)(y0 * 16 - sm * 640 / 30000);
    }
}
/* the table position the screen shows for oscillator k: the newest sounding voice's (the shared one plus that voice's
 * modulation: an LFO or an envelope on WAVE moves the picture), else the shared one (the knob, MORPH) */
static uint32_t osc_pos_shown(uint32_t k)
{
    uint32_t best = newest_voice();
    int32_t p = (int32_t)osc[k].now;
    if (best < NV) p += voice[best].wofs[k];    /* (the ISR's, as its last block ended) */
    return p < 0 ? 0u : p > (int32_t)WT_POS_MAX ? WT_POS_MAX : (uint32_t)p;
}
static uint32_t osc_at_edge(uint32_t k)         /* 1: the matrix pushes oscillator k's position past the table's end
                                                   (the newest voice's): it stands at the end */
{
    uint32_t best = newest_voice();
    return best < NV && (voice[best].wedge >> k & 1u);
}
static void hero_osc(uint16_t col)              /* the table around the oscillator's wave, back to front; the ruler */
{
    uint32_t sel = set.osc_sel, pos = osc_pos_shown(sel), k, t = set.p.osc[sel].table;
    const wt_row_t *tab = t == TAB_VA ? 0 : (const wt_row_t *)wt_table(t);   /* (level 0: the waves as they are) */
    const char *name = wt_name(t);
    for (k = 7; k >= 1u; k--) {                 /* the waves behind: 2, 4 .. 14 back, fainter */
        int32_t pk = (int32_t)pos - (int32_t)((k * 2u) << 16);
        if (pk >= 0) {                          /* (thin, anti-aliased) */
            wave_crv(tab, (uint32_t)pk, 22 + (int32_t)k * 7, 68 - (int32_t)k * 6);
            crv_stroke(22 + (int32_t)k * 7, 172 + (int32_t)k * 7 < 239 ? 172 + (int32_t)k * 7 : 239, 9,
                       dimc(col, 1u + (7u - k) * 4u / 6u));
        }
    }
    {                                           /* the knob's wave (thick); the one that sounds now, when the matrix
                                                   moves it (white), and who moves it: "W24 > 31 LFO1" */
        uint32_t base = osc[sel].now, k2, n2 = 0, edge = osc_at_edge(sel), lo = pos < (WT_POS_MAX >> 1);
        int32_t d = (int32_t)pos - (int32_t)base;
        wave_crv(tab, base, 22, 68);            /* (a fill under it, fading to the ruler) */
        crv_fill(22, 172, 28, 108, col, 6u);
        crv_line(22, 172, col);
        if (d > 4096 || d < -4096 || edge) {    /* (at an end: "W9 > [0 LFO1" in C_WARN, the wall on the ruler) */
            char s[24], *e;
            const char *who = 0;
            wave_crv(tab, pos, 22, 68);
            crv_stroke(22, 172, 12, C_WHITE);
            for (k2 = 0; k2 < NMSLOT; k2++) {
                const mod_slot_t *m = pslot(&set.p, k2);
                if (m->amt && m->src && (m->dst == MD_WAVE || m->dst == MD_W1 + sel)) { if (!who) who = MSRC_NAME[m->src]; n2++; }
            }
            e = put_int(put_str(s, "W"), (int32_t)((base + 0x8000u) >> 16), 0);
            e = put_str(e, edge && lo ? " > [" : " > ");
            e = put_int(e, (int32_t)((pos + 0x8000u) >> 16), 0);
            if (edge && !lo) *e++ = ']';
            if (who) { e = put_str(put_str(e, " "), who); if (n2 > 1u) *e++ = '+'; }
            *e = 0;
            cv_text(236 - text_w(&FONT_MD, s), 20, &FONT_MD, s, edge ? C_WARN : C_WHITE);
            if (edge) cv_rect(lo ? 11 : 228, 104, 2, 14, C_WARN);   /* (the ruler's end: a wall) */
        }
    }
    cv_text(236 - text_w(&FONT_MD, name), 4, &FONT_MD, name,   /* the table's name (TABLE turned: in colour) */
            ui.focus >= 0 && page_param(set.sec, ui_row(), (uint32_t)ui.focus) == P_TABLE ? col : C_OFF);
    if (set.p.uni[sel] > 1u) {                  /* unison: "x5 16c", and where the copies sit left to right */
        uint32_t nc = set.p.uni[sel], d = set.p.udet[sel], c;
        char s[12], *e = put_str(put_int(put_str(s, "x"), (int32_t)nc, 0), " ");
        e = put_str(put_int(e, (int32_t)((100u * d * d + 8064u) / 16129u), 0), "c");
        *e = 0;
        cv_text(4, 0, &FONT_MD, s, set.osc_row == 1u ? col : C_OFF);
        cv_rect(4, 22, 56, 1, C_DARK);
        for (c = 0; c < nc; c++) {
            int32_t pan = ((int32_t)(2u * c) - (int32_t)(nc - 1u)) * 26 / (int32_t)(nc - 1u) * set.p.uspr[sel] / 127;
            cv_rect(31 + pan, 18, 2, 9, col);
        }
    }
    {                                           /* FM: "FM < OSC2 40"; OSC2's sync: "SYNC < OSC3" (under the
                                                   unison's mark, if any) */
        int32_t y = set.p.uni[sel] > 1u ? 28 : 0;
        if (set.p.fms[sel] && set.p.fma[sel]) {
            char s[20], *e = put_int(put_str(put_str(put_str(s, "FM < "), FMS_NAME[set.p.fms[sel] % FMS_N]), " "),
                                     set.p.fma[sel], 0);
            *e = 0;
            cv_text(4, y, &FONT_MD, s, set.osc_row == 2u ? col : C_OFF);
            y += 18;
        }
        if (sel == 1u && set.p.sync) cv_text(4, y, &FONT_MD, "SYNC < OSC3", set.osc_row == 2u ? col : C_OFF);
    }
    cv_rect(14, 116, 212, 1, C_DARK);           /* the 64 waves: where each oscillator is */
    for (k = 0; k < NOSC; k++) {
        int32_t x = 14 + (int32_t)((osc_pos_shown(k) >> 10) * 211u / (63u << 6));
        if (k == sel) cv_rect(x - 1, 110, 3, 6, col);
        else cv_rect(x - 1, 112, 3, 4, set.p.osc[k].level ? C_OFF : C_DARK);
    }
}
static void hero_mix(uint16_t col)              /* this row's faders, each under its cell (rows B, C: the next pages);
                                                   a balance from the middle: up towards filter 2, down towards 1 */
{
    uint32_t i, row = ui_row();
    for (i = 0; i < 4u; i++) {
        uint32_t id = PAGE[SEC_MIX][row][i];
        int32_t x0 = (int32_t)i * 60 + 14, v, h;
        uint16_t c = (int32_t)i == ui.focus ? col : dimc(col, 6);
        const char *n = id == P_L1 || id == P_BAL1 ? "O1" : id == P_L2 || id == P_BAL2 ? "O2"
                      : id == P_L3 || id == P_BAL3 ? "O3" : id == P_NOISE || id == P_BALN ? "NOI"
                      : id == P_RING || id == P_BALR ? "RING" : "COLOR";
        if (!id) continue;
        v = pget(id);
        cv_rrect(x0, 10, 32, 92, 5, C_RULE);
        if (PARAM[id].fmt == F_BAL) {
            h = v * 46 / 63;
            cv_rect(x0, h >= 0 ? 56 - h : 56, 32, h >= 0 ? h : -h, c);
            cv_rect(x0, 56, 32, 1, C_OFF);
            ctext(x0 + 16, 12, &FONT_MD, "F2", v > 0 ? C_BLACK : C_OFF);
            ctext(x0 + 16, 82, &FONT_MD, "F1", v < 0 ? C_BLACK : C_OFF);
        } else {
            h = v * 92 / 127;
            if (h) cv_rrect(x0, 102 - h, 32, h, 5, c);
        }
        ctext(x0 + 16, 106, &FONT_MD, n, (int32_t)i == ui.focus ? C_WHITE : C_VAL);
    }
}
static const uint16_t EXP24[25] = {             /* 0.001^(i/24), Q15: the decay's and the release's shape */
    32768, 24573, 18427, 13818, 10362, 7771, 5827, 4370, 3277, 2457, 1843, 1382, 1036,
    777, 583, 437, 328, 246, 184, 138, 104, 78, 58, 44, 33};
static const env_t *voice_env(uint32_t v, uint32_t e)   /* voice v's envelope e: AMP, FLT, ENV 3, ENV 4 */
{
    return e == 0 ? &voice[v].env : e == 1u ? &voice[v].fenv : &voice[v].xenv[(e - 2u) & 1u];
}
static void exp_seg(int32_t xa, int32_t w, int32_t ya, int32_t yb)   /* a decay from ya to yb, w px, into crv */
{
    int32_t i, px = xa, py = ya * 16;
    for (i = 1; i <= 24; i++) {
        int32_t x = xa + w * i / 24, y = yb * 16 + (ya - yb) * 16 * EXP24[i] / 32768;
        if (x > px) crv_seg(px, py, x, y);
        px = x; py = y;
    }
}
static int32_t exp_x(int32_t xa, int32_t w, int32_t ya, int32_t yb, int32_t yl)   /* where that decay is at yl */
{
    int32_t i, best = 0, bd = 99999;
    for (i = 0; i <= 24; i++) {
        int32_t y = yb + (ya - yb) * EXP24[i] / 32768, d = y > yl ? y - yl : yl - y;
        if (d < bd) { bd = d; best = i; }
    }
    return xa + w * best / 24;
}
static void env_label(int32_t xl, int32_t xr, const char *t)   /* a stage's name under it, centred (narrow: small) */
{
    const felucca_font_t *f = text_w(&FONT_MD, t) <= xr - xl - 2 ? &FONT_MD : &FONT_SM;
    cv_text((xl + xr - text_w(f, t)) / 2, f == &FONT_MD ? 106 : 108, f, t, C_OFF);
}
/* ENV: the curve as the mode shapes it, times on a log scale: the attack (to full, or the attack level), the decay
 * (to S), the decay 2 (to S2: not in ADSR), the hold (not in ONE SHOT), the release; a loop drawn as a line over the
 * top back to where it goes again; a dot where each sounding voice is */
static void hero_env(uint16_t col)
{
    const env_set_t *en = &set.p.env[set.env_sel];
    uint32_t mode = en->mode;
    int32_t wa = 24 + 55 * en->a / 127, wd = 24 + 55 * en->d / 127, wd2 = mode == EM_ADSR ? 0 : 24 + 55 * en->d2 / 127;
    int32_t ws = mode == EM_ONESHOT ? 0 : 30, wr = 24 + 55 * en->r / 127, sum = wa + wd + wd2 + ws + wr;
    int32_t x0 = 12, base = 104, eh = 84, ytop, ys1, ys2, yh, xa, xd, xd2, xs, v;
    if (sum > 216) {                            /* too long: the times above 24 px shrink, the 24 px and the hold stay
                                                   (every stage keeps room for its name, D2 for S beside it too) */
        int32_t fl = 24 * (wd2 ? 4 : 3) + ws, k = 216 - fl, over = sum - fl;
        wa = 24 + (wa - 24) * k / over; wd = 24 + (wd - 24) * k / over; wr = 24 + (wr - 24) * k / over;
        if (wd2) wd2 = 24 + (wd2 - 24) * k / over;
    }
    ytop = base - eh * (mode == EM_ADSR ? 127 : en->al) / 127;   /* (eh: the mode's name above) */
    ys1 = base - eh * en->s / 127;
    ys2 = base - eh * en->s2 / 127;
    yh = mode == EM_ADSR ? ys1 : ys2;
    xa = x0 + wa; xd = xa + wd; xd2 = xd + wd2; xs = xd2 + ws;
    cv_rect(12, base, 216, 1, C_RULE);
    dots_v(xa, ytop, base, C_RULE);             /* (where each stage ends, dotted) */
    dots_v(xd, ys1, base, C_RULE);
    if (wd2) dots_v(xd2, ys2, base, C_RULE);
    if (ws) dots_v(xs, yh, base, C_RULE);
    crv_seg(x0, base * 16, xa, ytop * 16);      /* the curve: the attack, the decays, the hold, the release */
    exp_seg(xa, wd, ytop, ys1);
    if (wd2) exp_seg(xd, wd2, ys1, ys2);
    if (ws) crv_seg(xd2, yh * 16, xs, yh * 16);
    exp_seg(xs, wr, yh, base);
    crv_fill(x0, xs + wr, base - eh, base, col, 6u);
    crv_line(x0, xs + wr, col);
    if (mode == EM_LOOP12 || mode == EM_LOOPALL) {   /* (the loop: back to the decay, or to the attack) */
        int32_t xb = mode == EM_LOOP12 ? xa : x0;
        cv_rect(xb, 0, xd2 - xb, 1, C_VAL);
        cv_rect(xb, 0, 1, 4, C_VAL);
        cv_rect(xd2, 0, 1, 4, C_VAL);
    }
    env_label(x0, xa, "A");                     /* (the stages by the Blofeld's names: the hold is at S2 but in ADSR) */
    if (!wd2) env_label(xa, xd, "D");
    else {                                      /* S: no stage here, the level D goes to; in the row, under the corner
                                                   D | D2 (the dotted line from it up to the dot on the curve) */
        int32_t hs = (text_w(&FONT_MD, "S") + 3) / 2;
        env_label(xa, xd - hs, "D");
        env_label(xd - hs, xd + hs, "S");
        env_label(xd + hs, xd2, "D2");
        cv_rect(xd - 1, ys1 - 1, 3, 3, C_VAL);
    }
    if (ws) env_label(xd2, xs, mode == EM_ADSR ? "S" : "S2");
    env_label(xs, xs + wr, "R");
    cv_text(236 - text_w(&FONT_MD, EMODE_NAME[mode % EM_N]), 4, &FONT_MD, EMODE_NAME[mode % EM_N], C_OFF);
    for (v = 0; v < (int32_t)NV; v++) {         /* the voices: by their stage and level */
        const env_t *ev = voice_env((uint32_t)v, set.env_sel);
        int32_t lvl = ev->lvl >> 15, yl = base - eh * lvl / 32768, x = x0, st = ev->st, tl = (base - ytop) * 32768 / eh;
        if (!voice[v].busy || st == ENV_IDLE) continue;
        if (st == ENV_ATTACK) x = x0 + (tl > 0 ? wa * (lvl > tl ? tl : lvl) / tl : 0);
        else if (st == ENV_DECAY) x = exp_x(xa, wd, ytop, ys1, yl);
        else if (st == ENV_DECAY2) x = exp_x(xd, wd2, ys1, ys2, yl);
        else x = exp_x(xs, wr, yh, base, yl);
        vdot(x, yl, col);
    }
}
/* the filter: both responses on 20 Hz .. 20 kHz (22 px an octave), +18 .. -48 dB; the one SELECT picks bright;
 * ticks where each sounding voice's cutoff is now (its envelope, key and velocity) */
static int32_t cut_x12(int32_t n)               /* a cutoff (Q8 note) on the axis, x 12 (1/264 octave) */
{
    return 1297 + (n - (69 << 8)) * 11 / 128;   /* 440 Hz at 10 + 22 log2(22) = 108.1 px */
}
static int filt_crv(uint32_t f)                 /* filter f's response into crv, x 6 .. 233 (0: off, none) */
{
    const filt_set_t *q = &set.p.flt[f];
    int32_t pc = cut_x12(((int32_t)q->cut + 4) << 8), x;
    if (q->type == SVF_OFF || q->type >= SVF_NTYPE) return 0;
    for (x = 6; x <= 233; x++) {
        int32_t db = svf_db_q8(q->type, q->res, x * 12 - pc);
        crv[x] = (int16_t)clampi(320 + (18 * 256 - db) * 84 / (66 * 16), 14 * 16, 104 * 16);
    }
    return 1;
}
static int32_t note_hz(int32_t n)               /* a note's frequency, Hz */
{
    uint32_t r = (uint32_t)n % 12u, o = (uint32_t)n / 12u;
    return (int32_t)((((uint32_t)(((uint64_t)535809u * SEMI_Q16[r]) >> 16) << o) + 32768u) >> 16);   /* 8.1758 Hz
                                                   (note 0), Q16; rounded (110, not 109) */
}
static char *put_hz(char *p, int32_t hz)
{
    if (hz < 1000) return put_str(put_int(p, hz, 0), " HZ");
    if (hz < 10000) { p = put_int(p, hz / 1000, 0); *p++ = '.'; p = put_int(p, hz / 100 % 10, 0); }
    else p = put_int(p, (hz + 500) / 1000, 0);
    return put_str(p, " KHZ");
}
static int32_t exp2_x100(int32_t q16);
static char *put_dec(char *p, int32_t x100, int32_t places);
static void hero_filter(uint16_t col)           /* (CUTOFF and RESO turning keep this picture: the curve moves) */
{
    static const int32_t GX[3] = {61, 134, 207};   /* 100 Hz, 1 kHz, 10 kHz */
    static const char *const GL[3] = {"100", "1K", "10K"};
    uint32_t sel = set.filt_sel, v;
    const filt_set_t *q = &set.p.flt[sel];
    char s[24], *p;
    int32_t i;
    for (i = 0; i < 3; i++) {                   /* the grid, dotted */
        dots_v(GX[i], 14, 104, C_DARK);
        ctext(GX[i], 106, &FONT_MD, GL[i], C_OFF);
    }
    dots_h(6, 234, 43, C_DARK);                 /* 0 dB */
    if (filt_crv(sel ^ 1u)) crv_stroke(6, 233, 10, dimc(col, 3));   /* the other filter: thin, dim */
    if (filt_crv(sel)) {                        /* the one SELECT picks: a fill under it, the line */
        crv_fill(6, 233, 14, 104, col, 6u);
        crv_line(6, 233, col);
    }
    if (q->type != SVF_OFF)
        for (v = 0; v < NV; v++)
            if (voice[v].busy && voice[v].env.st != ENV_IDLE) {
                int32_t x = clampi(cut_x12(voice[v].fn[sel]) / 12, 6, 233);
                cv_rect(x - 1, 98, 3, 6, C_WHITE);
            }
    p = put_str(s, FTYPE_NAME[ftype_pos(q->type)]);
    if (q->type != SVF_OFF) p = put_hz(put_str(p, "  "), note_hz(q->cut + 4));
    *p = 0;
    i = cv_text(6, 0, &FONT_MD, s, q->type != SVF_OFF ? col : C_OFF);
    if (q->type != SVF_OFF) {                   /* the resonance as Q (0.5 .. 25), white while RESO turns */
        int hot = ui.focus >= 0 && page_param(set.sec, ui_row(), (uint32_t)ui.focus) == P_RES;
        p = SVF_ISCOMB(q->type) ? put_str(put_int(put_str(s, "FB "), (int32_t)q->res * 98 / 127, 0), "%")   /* (a comb:
                                                   its feedback) */
          : put_dec(put_str(s, "Q "), exp2_x100((int32_t)q->res * 2912) / 2, 1);
        *p = 0;
        cv_text(i + 12, 0, &FONT_MD, s, hot ? C_WHITE : C_OFF);
    }
    cv_text(196, 0, &FONT_MD, set.p.route ? "PAR" : "SER", C_OFF);
}
/* FILTER row C: DRIVE's curve, out against in (in: +-1.5 x full scale across; out: +-1 up), at the filter's DRIVE
 * (0: as at 1, dim); x 1 (no curve) thin; OSC1 MOD drawn with OSC1 at full scale */
static void hero_dcurve(uint16_t col)
{
    uint32_t f = set.filt_sel & 1u, cv = set.p.dcurve[f], d = set.p.flt[f].drive;
    int32_t g8 = 256 + (int32_t)(d ? d : 1u) * 14, x;
    char s[24], *p;
    dots_h(14, 226, 66, C_DARK);                /* (0 at y 66, full scale 38 px: under the words on top) */
    dots_v(120, 22, 112, C_DARK);
    for (x = 14; x <= 226; x++) {               /* x 1 */
        int32_t in = (x - 120) * 49152 / 106 / 4;   /* (+-1.5 x full scale, as the voice's mix is in / 4) */
        crv[x] = (int16_t)(66 * 16 - clampi(in * 4, -32768, 32768) * 38 * 16 / 32768);
    }
    crv_stroke(14, 226, 10, C_RULE);
    for (x = 14; x <= 226; x++) {
        int32_t in = (x - 120) * 49152 / 106;
        crv[x] = (int16_t)(66 * 16 - clampi(drive_curve(in, g8, cv, 32767), -36000, 36000) * 38 * 16 / 32768);
    }
    crv_fill(14, 226, 22, 112, col, 4u);
    crv_line(14, 226, d ? col : dimc(col, 3));
    p = put_str(put_str(put_int(put_str(s, "F"), (int32_t)f + 1, 0), " "), DCURVE_NAME[dcurve_pos(cv)]);
    *p = 0;
    cv_text(6, 0, &FONT_MD, s, col);
    p = d ? put_str(put_int(put_str(s, "DRIVE "), (int32_t)d, 0), "") : put_str(s, "DRIVE 0: OFF");
    *p = 0;
    cv_text(236 - text_w(&FONT_MD, s), 0, &FONT_MD, s, d ? C_VAL : C_OFF);
    p = s;                                      /* Filter FM and PAN, under the curve */
    if (set.p.ffms[f] && set.p.ffma[f])
        p = put_int(put_str(put_str(put_str(p, "FM < "), FMS_NAME[set.p.ffms[f] % FMS_N]), " "), set.p.ffma[f], 0);
    if (set.p.fpan[f]) {
        if (p != s) p = put_str(p, "   ");
        p = put_int(put_str(p, set.p.fpan[f] < 0 ? "PAN L" : "PAN R"), set.p.fpan[f] < 0 ? -set.p.fpan[f] : set.p.fpan[f], 0);
    }
    *p = 0;
    if (p != s) cv_text(6, 106, &FONT_MD, s, C_VAL);
}
static void hero_velo(uint16_t col)             /* ENV row B: the velocity (across) against what it gives (up); a dot
                                                   for each sounding voice at its velocity */
{
    int32_t s = set.p.env[set.env_sel % NENV].vel * 32768 / 63, x, v;
    cv_rect(20, 8, 1, 97, C_RULE);
    cv_rect(20, 104, 208, 1, C_RULE);
    for (x = 20; x <= 228; x++)                 /* velocity across, a column each: what it gives */
        crv[x] = (int16_t)(104 * 16 - (vel_gain((x - 20) * 32766 / 208, s) * 96 >> 11));
    crv_fill(20, 228, 8, 104, col, 6u);
    crv_line(20, 228, col);
    for (v = 0; v < (int32_t)NV; v++)
        if (voice[v].busy && voice[v].env.st != ENV_IDLE) {
            int32_t vel = voice[v].vel;
            vdot(20 + vel * 208 / 127, 104 - (vel_gain(vel * 258, s) * 96 >> 15), col);
        }
    cv_text(26, 4, &FONT_MD, set.env_sel == 1u ? "ENV REACH" : "LEVEL", C_OFF);
    cv_text(150, 106, &FONT_MD, "VELOCITY", C_OFF);
}
/* ARP: the tempo (or the MIDI clock's) and the rate; the steps of one pass of the sequence (up to 16 around the one
 * playing), a bar a note, as high as its pitch among them; the one sounding white */
/* the pattern, 16 columns: as high as the step's type does (a pause a dash; a chord the tallest), as bright as its
 * accent (a muted step an outline), a legato one reaching into the next, the timing moving it sideways, a glide a
 * mark on top, its letter under it; the steps past LENGTH dark; the one playing white; in STEPS the one picked
 * underlined */
static void hero_pattern(uint16_t col, int sel)
{
    static const uint8_t H[AS_N] = {44, 0, 34, 58, 58, 72, 90, 50};
    const uint16_t *pat = pat_st(pat_num());
    uint32_t i, plen = *pat_len(pat_num()), play = arp.run && arp.s ? (arp.s - 1u) % plen : 99u;
    char s[24], *e;
    e = put_str(s, "PATCH PATTERN");
    *e = 0;
    cv_text(6, 0, &FONT_MD, s, C_VAL);
    e = arp_ext() ? put_int(put_str(s, "MIDI "), (int32_t)clk_bpm, 0) : put_str(put_int(s, set.p.arp.tempo, 0), " BPM");
    e = put_str(put_str(e, "  "), ARP_RATE_NAME[set.p.arp.rate]);   /* the tempo (or the clock's) and the rate */
    *e = 0;
    cv_text(236 - text_w(&FONT_MD, s), 0, &FONT_MD, s, arp_ext() ? C_WHITE : C_VAL);
    int32_t tieh = 0;                           /* (a tie's note going on: its height) */
    for (i = 0; i < ARP_PLEN; i++) {
        uint32_t st = pat[i], ty = AST_TYPE(st), ac = AST_ACC(st), tm = AST_TIM(st);
        int32_t x = 6 + (int32_t)i * 14 + (tm ? ((int32_t)tm - 4) : 0), h = H[ty], w = AST_LEN(st) && !AST_TIE(st) ? 11 : 14;
        uint16_t c = i >= plen ? C_DARK : i == play ? C_WHITE : dimc(col, ac ? 2u + ac : 3u);
        if (tieh && i < plen) {                 /* (held on by the tie before: its note goes on, it plays nothing) */
            int32_t xc = 6 + (int32_t)i * 14;
            cv_rect(xc, 100 - tieh, AST_TIE(st) ? 14 : 11, 6, i == play ? C_WHITE : dimc(col, 3));
            cv_rect(xc, 100 - tieh + 6, 1, tieh - 6, dimc(col, 2));
            if (sel && i == set.step_sel) cv_rect(xc - 1, 103, 13, 2, C_WHITE);
            ctext(xc + 5, 106, &FONT_MD, "~", i == set.step_sel && sel ? C_WHITE : C_DARK);
            if (!AST_TIE(st)) tieh = 0;
            continue;
        }
        tieh = AST_TIE(st) && h && ac ? h : 0;
        if (!h) cv_rect(x, 98, 11, 2, i >= plen ? C_DARK : i == play ? C_WHITE : C_OFF);   /* (a pause) */
        else if (!ac) {                         /* (muted: an outline) */
            cv_rect(x, 100 - h, w, 1, c); cv_rect(x, 99, w, 1, c);
            cv_rect(x, 100 - h, 1, h, c); cv_rect(x + w - 1, 100 - h, 1, h, c);
        } else cv_rect(x, 100 - h, w, h, c);
        if (AST_GLIDE(st) && h) cv_rect(x + 3, 96 - h, 5, 2, c);
        if (sel && i == set.step_sel) cv_rect(6 + (int32_t)i * 14 - 1, 103, 13, 2, C_WHITE);
        {
            char l[2] = {"N.PFLBCR"[ty], 0};
            ctext(6 + (int32_t)i * 14 + 5, 106, &FONT_MD, l, i == set.step_sel && sel ? C_WHITE : C_DARK);
        }
    }
}
static void hero_arp(uint16_t col)
{
    static uint8_t notes[ARP_MAXN * 10u], vels[ARP_MAXN * 10u];
    const arp_par_t *p = &set.p.arp;
    uint32_t len = arp_build(&arp, p, notes, vels), per, cur, first, i, n, lo = 127, hi = 0;
    char s[24], *e;
    if (p->pattern) { hero_pattern(col, 0); return; }   /* (a pattern: its steps; else the sequence's) */
    e = arp_ext() ? put_str(put_int(put_str(s, "MIDI "), (int32_t)clk_bpm, 0), " BPM")
                  : put_str(put_int(s, p->tempo, 0), " BPM");
    *e = 0;
    cv_text(6, 0, &FONT_MD, s, arp_ext() ? C_WHITE : C_VAL);
    cv_text(236 - text_w(&FONT_MD, ARP_RATE_NAME[p->rate]), 0, &FONT_MD, ARP_RATE_NAME[p->rate], C_VAL);
    if (p->mode == ARP_OFF) { ctext(120, 50, &FONT_MD, "RUN: ARP ON", C_OFF); return; }
    if (!len) { ctext(120, 50, &FONT_MD, p->mode == ARP_HOLD ? "HOLD: PLAY A CHORD" : "PLAY A CHORD", C_OFF); return; }
    per = arp_period(p->dir, len);
    cur = arp.k ? (arp.k - 1u) % per : 0u;      /* the step sounding (or the last one) */
    n = per < 16u ? per : 16u;
    first = cur / 16u * 16u;
    if (first + n > per) first = per - n;
    for (i = 0; i < len; i++) { if (notes[i] < lo) lo = notes[i]; if (notes[i] > hi) hi = notes[i]; }
    for (i = 0; i < n; i++) {
        uint32_t st = first + i, nt = notes[arp_index(p->dir, len, st)];
        int32_t h = 10 + (int32_t)(hi > lo ? (nt - lo) * 70u / (hi - lo) : 35u), x = 6 + (int32_t)i * 14;
        int now = st == cur && arp.sounding;
        cv_rect(x, 104 - h, 11, h, now ? C_WHITE : st == cur ? col : dimc(col, 4));
    }
    cv_rect(6, 106, 228, 1, C_DARK);
}
/* MOD: an LFO: two cycles of its shape, where it is now (the shared one's, or the first sounding voice's), its speed,
 * what reads it; a slot: the matrix, a line a slot, the one picked bright */
/* a list to choose from, while its knob turns: three before, the one chosen in the middle, three after; where it is
 * of how many */
static void hero_choice(uint16_t col, const char *const *names, uint32_t n, uint32_t cur, const char *what)
{
    int32_t d;
    char s[16], *e = put_int(put_str(put_str(put_str(s, what), " "), ""), (int32_t)cur + 1, 0);
    *e++ = '/';
    e = put_int(e, (int32_t)n, 0);
    *e = 0;
    cv_text(236 - text_w(&FONT_MD, s), 0, &FONT_MD, s, C_OFF);
    for (d = -3; d <= 3; d++) {
        int32_t i = (int32_t)cur + d, y = 52 + d * 17;
        if (i < 0 || i >= (int32_t)n) continue;
        if (!d) cv_rect(0, y - 1, 240, 18, dimc(col, 2));
        cv_text(24, y, &FONT_MD, names[i], d ? (d == -1 || d == 1 ? C_VAL : C_OFF) : C_WHITE);
    }
}
/* the waves a slot on a table position reaches by itself (WAVE: the oscillator OSC shows), rounded: the knob's (with
 * MORPH) +- the amount (LFOs, KEY, MACRO, RAND swing both ways), or + it (the envelopes, VELO, WHEEL) */
static void slot_reach(const mod_slot_t *m, int32_t *lo, int32_t *hi)
{
    uint32_t k = m->dst == MD_WAVE ? set.osc_sel : (uint32_t)(m->dst - MD_W1);
    int32_t b = (int32_t)(osc[k].target >> 8), a = mod_scale(m->dst, m->amt);   /* Q8 waves */
    int32_t bi = (m->src >= MS_LFO1 && m->src <= MS_LFO3) || m->src == MS_KEY || m->src == MS_MACRO || m->src == MS_RAND;
    int32_t p = b + a, q = bi ? b - a : b;
    *lo = ((p < q ? p : q) + 128) >> 8;         /* (>> of a negative: toward -inf; + 128: rounded) */
    *hi = ((p < q ? q : p) + 128) >> 8;
}
static void hero_modf(uint16_t col)             /* MODIF: the four, "M1 = LFO1 * WHEEL", the picked one lit, and under
                                                   each what reads it */
{
    uint32_t i, k;
    char s[40], *e;
    for (i = 0; i < NMODF; i++) {
        const modf_set_t *f = &set.p.modf[i];
        int now = i == set.mod_mf, read = 0;
        int32_t y = (int32_t)i * 30;
        e = put_str(put_int(put_str(s, "M"), (int32_t)i + 1, 0), " = ");
        e = put_str(put_str(put_str(put_str(e, MSRC_NAME[f->a % MS_N]), " "), MOP_NAME[f->op % MO_N]), " ");
        if (f->b) e = put_str(e, MSRC_NAME[f->b % MS_N]);
        else if (f->c < 0) { *e++ = '('; e = put_int(e, f->c, 0); *e++ = ')'; }   /* ("+ (-20)", not "+ -20") */
        else e = put_int(e, f->c, 0);
        *e = 0;
        if (now) cv_rect(0, y + 1, 240, 29, dimc(col, 2));
        cv_text(6, y, &FONT_MD, s, now ? C_WHITE : C_VAL);
        for (k = 0, e = s; k < NMSLOT; k++) {   /* (what reads it) */
            const mod_slot_t *m = pslot(&set.p, k);
            if (m->src == MS_MOD1 + i && m->dst && m->amt && e - s < 28) {
                e = put_str(put_str(e, read ? " " : "> "), MDST_NAME[m->dst]);
                read = 1;
            }
        }
        *e = 0;
        cv_text(18, y + 15, &FONT_SM, read ? s : "NO SLOT READS IT", read ? (now ? C_VAL : C_OFF) : C_DARK);
    }
}
static void hero_mod(uint16_t col)
{
    uint32_t i;
    char s[28], *e;
    if (set.mod_row < 2u) {                     /* (rows A B: the LFO SELECT picks) */
        uint32_t l = set.mod_lfo, x, ph, v;
        const lfo_set_t *lf = &set.p.lfo[l];
        lfo_t t = {0, 0x1234567u, 0, 0};
        e = put_str(put_str(put_int(put_str(s, "LFO "), (int32_t)l + 1, 0), "  "), LSHAPE_NAME[lf->shape]);
        *e = 0;
        cv_text(6, 0, &FONT_MD, s, C_VAL);
        if (lf->sync) e = put_str(s, ARP_RATE_NAME[lf->rate % ARP_NRATE]);
        else {                                  /* (Hz, from the step: 1 Hz = 1558259) */
            uint32_t h = lfo_hz100(LFO_INC[lf->rate]);   /* Hz x 100 */
            e = put_int(s, (int32_t)(h / 100u), 0);
            *e++ = '.';
            if (h % 100u < 10u) *e++ = '0';
            e = put_str(put_int(e, (int32_t)(h % 100u), 0), " HZ");
        }
        *e = 0;
        cv_text(236 - text_w(&FONT_MD, s), 0, &FONT_MD, s, C_VAL);
        dots_h(14, 226, 60, C_DARK);
        for (x = 0; x <= 210u; x++)             /* two cycles over 210 px */
            crv[14 + x] = (int16_t)(60 * 16 - lfo_step(&t, lf->shape, x ? (uint32_t)(0x100000000ull / 105u) : 0u) * 576 / 32767);
        crv_line(14, 224, col);
        ph = glfo[l].ph;
        if (lf->key)
            for (v = 0; v < NV; v++) if (voice[v].busy) { ph = voice[v].lfo[l].ph; break; }
        x = 14u + (uint32_t)(((uint64_t)ph * 105u) >> 32);
        cv_rect((int32_t)x, 22, 1, 78, C_WHITE);
        for (i = 0, e = s; i < NMSLOT; i++) {   /* what reads it */
            const mod_slot_t *m = pslot(&set.p, i);
            if (m->src == MS_LFO1 + l && m->dst && m->amt && e - s < 20) { e = put_str(e, MDST_NAME[m->dst]); *e++ = ' '; }
        }
        *e = 0;
        cv_text(6, 106, &FONT_MD, s[0] ? s : "NO SLOT READS IT", s[0] ? C_VAL : C_OFF);
        return;
    }
    if (set.mod_row == 3u) { hero_modf(col); return; }
    for (i = set.mod_slot / 8u * 8u; i < set.mod_slot / 8u * 8u + 8u; i++) {   /* the matrix: the 8 slots of
                                                   the picked one's half (1..8 or 9..16) */
        const mod_slot_t *m = pslot(&set.p, i);
        int now = i == set.mod_slot, on = m->src && m->dst && m->amt;
        int32_t x;
        e = put_int(s, (int32_t)i + 1, 0);
        if (on || now) {
            e = put_str(put_str(put_str(put_str(e, " "), MSRC_NAME[m->src]), " > "), MDST_NAME[m->dst]);
            e = put_int(put_str(e, " "), m->amt, 1);
        } else e = put_str(e, " -");
        *e = 0;
        if (now) cv_rect(0, (int32_t)(i % 8u) * 15 + 1, 240, 15, dimc(col, 2));
        x = cv_text(6, (int32_t)(i % 8u) * 15, &FONT_MD, s, now ? C_WHITE : on ? C_VAL : C_DARK);
        if (on && m->dst >= MD_WAVE && m->dst <= MD_W3) {   /* a table position: the waves it reaches, "-8..26";
                                                   past an end (it stops there) in C_WARN */
            int32_t lo, hi;
            slot_reach(m, &lo, &hi);
            e = put_int(put_str(put_int(put_str(s, " "), lo, 0), ".."), hi, 0);
            *e = 0;
            cv_text(x, (int32_t)(i % 8u) * 15, &FONT_MD, s,
                    lo < 0 || hi > (int32_t)WT_WAVES - 1 ? C_WARN : now ? C_WHITE : C_OFF);
        }
    }
}
static char *put_prog(char *p, uint32_t i, const char *name)   /* "07 SOFT PAD" (the name's trailing spaces off) */
{
    uint32_t k, n = PATCH_NAME;
    *p++ = (char)('0' + i / 10u);
    *p++ = (char)('0' + i % 10u);
    *p++ = ' ';
    while (n && name[n - 1u] == ' ') n--;
    for (k = 0; k < n; k++) *p++ = name[k];
    return p;
}
/* PATCH (HOME): the patch before and after small, this one big on a plate (edited: an amber dot); under it the patch at
 * a glance, a card a section in its colour: OSC its three waves (as bright as loud), FILTER the responses, AMP the
 * envelope, FX the two slots, ARP its pattern (or its direction, or OFF) */
static void prog_line(int32_t y, uint32_t i)    /* a neighbour: "06  ARP SEQ", dim */
{
    char nm[PATCH_NAME], s[20], *e;
    patch_name(i, nm);
    e = put_prog(s, i, nm);
    *e = 0;
    s[2] = 0;
    cv_text(30, y, &FONT_MD, s, C_DARK);
    cv_text(52, y, &FONT_MD, s + 3, C_OFF);
}
static void tile(int32_t x, const char *label, uint16_t c)   /* a card of the glance: 43 x 44 at y 77 */
{
    cv_rrect_o(x, 77, 43, 44, 5, mixc(C_CARD, C_BG, 8u), C_EDGE);
    cv_text(x + 5, 78, &FONT_SM, label, c);
}
static void hero_patch(void)
{
    static const uint8_t H[AS_N] = {44, 0, 34, 58, 58, 72, 90, 50};   /* (hero_pattern's heights) */
    static const char *const FX3[FX_N] = {"-", "DLY", "REV", "DST", "CHO", "PHS", "ENS", "LOF", "FLT", "TRI"};
    char s[20], *e;
    int32_t x, i, k;
    prog_line(1, (set.prog + NPATCH - 1u) % NPATCH);
    cv_rrect_o(6, 20, 228, 34, 8, mixc(C_CARD, C_WHITE, 1u), mixc(C_CARD, C_WHITE, 4u));
    cv_rrect(12, 28, 28, 18, 5, C_RULE);
    e = put_prog(s, set.prog, set.name);
    *e = 0;
    s[2] = 0;
    ctext(26, 30, &FONT_SM, s, C_VAL);
    x = cv_text(48, 23, &FONT_LG, s + 3, C_WHITE);
    if (patch_edited()) cv_rrect(x + 6 < 220 ? x + 6 : 220, 34, 7, 7, 3, C_AMBER);
    if (set.p.cat && x + 20 + text_w(&FONT_SM, PCAT_NAME[set.p.cat % PC_N]) < 228)   /* the category, where the name
                                                   leaves room (INIT: none) */
        cv_text(228 - text_w(&FONT_SM, PCAT_NAME[set.p.cat % PC_N]), 37, &FONT_SM, PCAT_NAME[set.p.cat % PC_N], C_OFF);
    prog_line(56, (set.prog + 1u) % NPATCH);
    /* OSC: each oscillator's wave where it stands now, 33 px, brighter as it is louder (silent: not drawn) */
    tile(6, "OSC", SEC_COL[SEC_OSC]);
    for (k = NOSC - 1; k >= 0; k--) {
        uint32_t t = set.p.osc[k].table, lv = set.p.osc[k].level, pos = osc[k].now;
        const wt_row_t *tab = t == TAB_VA ? 0 : (const wt_row_t *)wt_table(t);
        if (!lv) continue;
        for (i = 0; i <= 32; i++) {
            int32_t sm = tab ? wt_sample(tab, (uint32_t)i << 27, pos) : va_shape((uint32_t)i << 27, pos);
            crv[11 + i] = (int16_t)(103 * 16 - sm * 176 / 30000);
        }
        crv_stroke(11, 43, 11, mixc(C_CARD, SEC_COL[SEC_OSC], 5u + lv * 11u / 127u));
    }
    /* FILTER: filter 1's response (and 2's, dim), a fill under the picked one */
    tile(52, "FILTER", SEC_COL[SEC_FILT]);
    for (k = 1; k >= 0; k--) {
        const filt_set_t *q = &set.p.flt[k];
        int32_t pc = cut_x12(((int32_t)q->cut + 4) << 8);
        if (q->type == SVF_OFF || q->type >= SVF_NTYPE) continue;
        for (i = 0; i <= 32; i++) {
            int32_t db = svf_db_q8(q->type, q->res, (6 + i * 227 / 32) * 12 - pc);
            crv[57 + i] = (int16_t)clampi(94 * 16 + (18 * 256 - db) * 26 / (66 * 16), 94 * 16, 117 * 16);
        }
        if (k) crv_stroke(57, 89, 9, dimc(SEC_COL[SEC_FILT], 4));
        else { crv_fill(57, 89, 94, 118, SEC_COL[SEC_FILT], 6u); crv_stroke(57, 89, 11, SEC_COL[SEC_FILT]); }
    }
    /* AMP: the amplitude envelope's attack, decay, sustain, release, straight */
    tile(98, "AMP", SEC_COL[SEC_ENV]);
    {
        const env_set_t *en = &set.p.env[0];
        int32_t wa = 1 + 8 * en->a / 127, wd = 1 + 8 * en->d / 127, wr = 1 + 9 * en->r / 127, ys = 117 - 22 * en->s / 127;
        int32_t x0 = 103, xa = x0 + wa, xd = xa + wd, xs = xd + 8;
        crv_seg(x0, 117 * 16, xa, 95 * 16);
        crv_seg(xa, 95 * 16, xd, ys * 16);
        crv_seg(xd, ys * 16, xs, ys * 16);
        crv_seg(xs, ys * 16, xs + wr, 117 * 16);
        crv_fill(x0, xs + wr, 95, 118, SEC_COL[SEC_ENV], 6u);
        crv_stroke(x0, xs + wr, 11, SEC_COL[SEC_ENV]);
    }
    /* FX: the two slots, a pill each (on: filled) */
    tile(144, "FX", SEC_COL[SEC_FX]);
    for (k = 0; k < 2; k++) {
        uint32_t ty = set.p.fx[k].type % FX_N;
        int32_t y = 94 + k * 13;
        if (ty) cv_rrect(149, y, 33, 12, 6, dimc(SEC_COL[SEC_FX], 4));
        else cv_rrect_o(149, y, 33, 12, 6, mixc(C_CARD, C_BG, 8u), C_EDGE);
        ctext(165, y - 2, &FONT_SM, FX3[ty], ty ? C_WHITE : C_DARK);
    }
    /* ARP: its pattern, 16 thin bars; no pattern: its direction; off: OFF */
    tile(190, "ARP", SEC_COL[SEC_ARP]);
    if (set.p.arp.mode == ARP_OFF) ctext(211, 100, &FONT_SM, "OFF", C_DARK);
    else if (!set.p.arp.pattern) ctext(211, 100, &FONT_SM, ADIR_NAME[set.p.arp.dir % AD_N], C_VAL);
    else {
        const uint16_t *pat = pat_st(set.p.arp.pattern);
        for (i = 0; i < ARP_PLEN; i++) {
            uint32_t st = pat[i], h = H[AST_TYPE(st)] * 22u / 90u;
            if ((uint32_t)i >= *pat_len(set.p.arp.pattern)) break;
            if (!h) cv_rect(195 + i * 2, 116, 1, 1, C_DARK);
            else cv_rect(195 + i * 2, 117 - (int32_t)h, 1, (int32_t)h, dimc(SEC_COL[SEC_ARP], AST_ACC(st) ? 4u + AST_ACC(st) : 3u));
        }
    }
}
/* SAVE asking: where to; the name a letter a box (KNOB2 the cursor: ringed amber; KNOB3 the letter: the ones before and
 * after it above and under the box); what it replaces; the way out */
static void hero_save(void)
{
    char s[24], nm[PATCH_NAME], *e;
    uint32_t i, n = sizeof NAME_CH - 1u, at = 0;
    e = put_str(s, "SAVE TO ");
    *e++ = (char)('0' + ui.save_to / 10u);
    *e++ = (char)('0' + ui.save_to % 10u);
    *e = 0;
    cv_text(10, 1, &FONT_SM, s, C_AMBER);
    for (i = 0; i < n; i++)
        if (NAME_CH[i] == ui.sname[ui.cur]) at = i;
    for (i = 0; i < PATCH_NAME; i++) {          /* 12 boxes, 18 x 28 */
        char c[2] = {ui.sname[i], 0};
        int32_t x = 6 + (int32_t)i * 19;
        if (i == ui.cur) {
            char a[2] = {NAME_CH[(at + n - 1u) % n], 0}, b[2] = {NAME_CH[(at + 1u) % n], 0};
            cv_rrect_o(x, 34, 18, 28, 4, mixc(C_CARD, C_AMBER, 3u), C_AMBER);
            ctext(x + 9, 18, &FONT_SM, a[0] == ' ' ? "_" : a, C_DARK);
            ctext(x + 9, 62, &FONT_SM, b[0] == ' ' ? "_" : b, C_DARK);
        } else {
            cv_rrect(x, 34, 18, 28, 4, C_RULE);
        }
        if (c[0] != ' ') ctext(x + 9, 36, &FONT_LG, c, C_WHITE);
        else cv_rect(x + 8, 47, 2, 2, C_DARK);
    }
    patch_name(ui.save_to, nm);
    e = put_prog(s, ui.save_to, nm);
    *e = 0;
    cv_rrect(10, 86, 5, 5, 2, C_AMBER);
    cv_text(20, 81, &FONT_SM, "REPLACES", C_OFF);
    cv_text(72, 79, &FONT_MD, s, C_VAL);
    cv_rrect_o(10, 100, 48, 19, 7, C_CARD, C_AMBER);   /* the way out, a legend of the keys (not a menu: nothing to
                                                   pick): SAVE writes; any other key cancels (plain text, no key-cap:
                                                   2026-11-16, "OTHER" in a box read as an item to move to) */
    ctext(34, 101, &FONT_SM, "SAVE", C_AMBER);
    cv_text(64, 101, &FONT_SM, "WRITE", C_OFF);
    cv_text(232 - text_w(&FONT_SM, "OTHER  KEY:  CANCEL"), 101, &FONT_SM, "OTHER  KEY:  CANCEL", C_OFF);
}
/* the battery (SETUP, top right). The FM-1: the stock firmware's thresholds on the divider's raw reading (531 / 561 /
 * 591, as Felucca and fm1-base take them) give 0..3 bars; a level moves once the reading is 4 past its threshold (no
 * flicker on the edge). There is no charger line: "USB" under it says a host is there, and the reading is then the
 * charging voltage (higher than the battery's own). The SMK-37: its thresholds are unknown, nothing. Nothing before
 * the first reading. */
static void draw_batt(void)
{
    static int32_t lvl = -1;
    int32_t raw = batt_q4 / 16;
    if (raw <= 0) return;
#if !BOARD_SMK37
    {
        static const int16_t AT[3] = {531, 561, 591};
        int32_t i;
        uint16_t c, rim;
        if (lvl < 0) lvl = (raw >= AT[0]) + (raw >= AT[1]) + (raw >= AT[2]);
        while (lvl < 3 && raw >= AT[lvl] + 4) lvl++;
        while (lvl > 0 && raw < AT[lvl - 1] - 4) lvl--;
        c = lvl >= 2 ? SEC_COL[SEC_ENV] : C_AMBER;   /* (green; one bar left: amber; none: the outline red) */
        rim = lvl ? C_OFF : C_WARN;
        cv_rrect_o(206, 6, 26, 14, 3, C_CARD, rim);
        cv_rect(232, 10, 3, 6, rim);
        for (i = 0; i < lvl; i++) cv_rect(209 + i * 7, 9, 6, 8, c);
        if (batt_usb()) ctext(220, 22, &FONT_SM, "USB", C_VAL);
    }
#else
    (void)lvl;
#endif
}
static void hero_setup(void)
{
    char s[40], *p;
    uint32_t i;
    ctext(120, 0, &FONT_LG, APP_NAME, SEC_COL[SEC_OSC]);
    draw_batt();
    p = put_str(s, "v" APP_VER "  ");
#ifdef FELUCCA_ID
    p = put_str(p, FELUCCA_ID);
#else
    p = put_str(p, "HOST");
#endif
    *p = 0;
    ctext(120, 36, &FONT_MD, s, C_VAL);
#ifndef DINGHY_HOST
    p = put_hex6(put_str(s, "FLASH "), flash_jedec);
    p = put_str(p, flash_jedec == 0x856014u ? " PUYA" : flash_jedec == 0xB36014u ? " UC25HQ80"
                 : flash_ok ? "" : " ?");
#else
    p = put_str(s, "FLASH: HOST");
#endif
    *p = 0;
    ctext(120, 54, &FONT_MD, s, C_VAL);
    p = clk_bpm ? put_str(put_int(put_str(s, "MIDI CLOCK "), (int32_t)clk_bpm, 0), " BPM") : put_str(s, "NO MIDI CLOCK");
    *p = 0;
    ctext(120, 72, &FONT_MD, s, C_OFF);
    cv_text(4, 98, &FONT_MD, "CPU", C_OFF);      /* the audio's load: the longest block of each half second */
    if (poly_n < NV) {                          /* a heavy sound: a voice less (engine/poly.h) */
        p = put_int(put_str(s, "POLY "), (int32_t)poly_n, 0);
        *p = 0;
        cv_text(196, 111, &FONT_SM, s, C_VAL);   /* (under the figure, right of the history) */
    }
    if (ui.cpu <= 999u) {                       /* that block's split and the mean (2026-11-16: where the time goes) */
        p = put_int(put_str(s, "VOX "), (int32_t)ui.cpu_vox, 0);
        p = put_int(put_str(p, "   FX "), (int32_t)ui.cpu_fx, 0);
        p = put_int(put_str(p, "   OUT "), (int32_t)ui.cpu_out, 0);
        p = put_int(put_str(p, "   AVG "), (int32_t)ui.cpu_avg, 0);
        *p = 0;
        cv_text(40, 90, &FONT_SM, s, C_OFF);
    }
    for (i = 0; i < 48u; i++) {                 /* (the history under it, 16 px high) */
        uint32_t v = ui.cpu_hist[(ui.cpu_n + i) % 48u], h = v > 100u ? 16u : v * 16u / 100u;
        cv_rect(40 + (int32_t)i * 3, 122 - (int32_t)h, 2, (int32_t)h ? (int32_t)h : 1, v ? SEC_COL[SEC_ENV] : C_DARK);
    }
    p = ui.cpu <= 999u ? put_str(put_int(s, (int32_t)ui.cpu, 0), "%") : put_str(s, "--");
    *p = 0;
    cv_text(196, 98, &FONT_MD, s, C_VAL);
}
static int32_t cho_y(uint32_t p, uint32_t base, uint32_t dep, uint32_t span)   /* CHORUS's delay at LFO phase p, as
                                                   the picture's y (112: none, 46: span samples) */
{
    int32_t d = (int32_t)base + (int32_t)(((int64_t)cho_tri(p) * (int32_t)dep) >> 15);
    if (d < 2) d = 2;                           /* (as the ISR clamps it) */
    return 112 - d * 66 / (int32_t)span;
}
/* the EQ: its response on the filter's axis (20 Hz .. 20 kHz, 22 px an octave), -12 .. +12 dB, a dot at each band
 * that is on (white: the one a knob turns) */
static int32_t eq_y16(int32_t db_q8)            /* dB (Q8) as the picture's y x 16: 0 dB at 59, 3.5 px a dB */
{
    return clampi(59 * 16 - db_q8 * 56 / 256, 14 * 16, 104 * 16);
}
static void hero_eq(uint16_t col)
{
    static const int32_t GX[3] = {61, 134, 207};   /* 100 Hz, 1 kHz, 10 kHz (as the filter's) */
    static const char *const GL[3] = {"100", "1K", "10K"};
    const eq_set_t *q = &set.p.eq;
    int32_t i, x, b, hot = -1, pc[EQ_NB];
    uint32_t on = 0;
    if (ui.focus >= 0) {                        /* the band a knob turns (its gain or its frequency) */
        uint32_t id = page_param(set.sec, ui_row(), (uint32_t)ui.focus);
        hot = id >= P_EQLO && id <= P_EQHI ? (int32_t)(id - P_EQLO) : id >= P_EQLF && id <= P_EQHF ? (int32_t)(id - P_EQLF)
            : id == P_EQQ ? EQ_MID : -1;
    }
    for (i = 0; i < 3; i++) {
        dots_v(GX[i], 14, 104, C_DARK);
        ctext(GX[i], 106, &FONT_MD, GL[i], C_OFF);
    }
    dots_h(6, 234, 17, C_DARK);                 /* +12, 0, -12 dB */
    dots_h(6, 234, 59, C_EDGE);
    dots_h(6, 234, 101, C_DARK);
    cv_text(6, 18, &FONT_SM, "+12", C_DARK);
    cv_text(6, 86, &FONT_SM, "-12", C_DARK);
    for (b = 0; b < (int32_t)EQ_NB; b++) {
        pc[b] = cut_x12((int32_t)q->note[b] << 8);
        if (q->gain[b]) on |= 1u << b;
    }
    for (x = 6; x <= 233; x++) {
        int32_t db = 0;
        for (b = 0; b < (int32_t)EQ_NB; b++)
            if (on >> b & 1u) db += eq_db_q8((uint32_t)b, q->gain[b], q->q, x * 12 - pc[b]);
        crv[x] = (int16_t)eq_y16(db);
    }
    if (on) crv_line(6, 233, col);
    else {
        crv_stroke(6, 233, 10, dimc(col, 4));
        ctext(120, 36, &FONT_MD, "FLAT", C_OFF);
    }
    for (b = 0; b < (int32_t)EQ_NB; b++) {      /* the bands: a dot where each is, on the curve */
        int32_t bx = clampi(pc[b] / 12, 6, 233), by = crv[bx] >> 4;
        if (!(on >> b & 1u) && b != hot) continue;
        cv_rrect(bx - 3, by - 3, 7, 7, 3, b == hot ? C_WHITE : col);
    }
}
/* PHASER: the response now (the dry and the line, the feedback as a straight line would give it), on the filter's
 * axis (20 Hz .. 20 kHz), +12 .. -30 dB (0 dB at y 40); under it the sweep's reach, a white tick where the centre is
 * now; the right side (WIDTH) thin and dim. With A = e^-j theta, theta = N 2 atan(f / fc), the line with feedback is
 * A / (1 - fb A) = ((cos - fb) - j sin) / (1 - 2 fb cos + fb^2) */
static void ph_crv(int32_t cn, uint32_t nst, int32_t fb, int32_t dry, int32_t wet)   /* into crv, x 6 .. 233 */
{
    int32_t x, pc = cut_x12(cn), d = dry << 2, w = wet << 2;   /* (Q14) */
    for (x = 6; x <= 233; x++) {
        int32_t o = clampi(x * 12 - pc, -1584, 1584) + 1584, i = o / 12, f = o % 12, at, c, s, dn, re, im, db;
        uint32_t th;
        at = PH_AT[i] + (i < 264 ? ((PH_AT[i + 1] - PH_AT[i]) * f) / 12 : 0);
        th = (uint32_t)at * nst;
        c = fx_cos14(th << 16);
        s = fx_cos14((th - 16384u) << 16);
        dn = 16384 - ((2 * fb * c) >> 14) + ((fb * fb) >> 14);   /* (>= (1 - 0.95)^2: 41) */
        re = d + w * (c - fb) / dn;
        im = w * s / dn;
        db = ((eq_log2_q8((uint64_t)((int64_t)re * re + (int64_t)im * im)) - (28 << 8)) * 771) >> 8;
        crv[x] = (int16_t)clampi(14 * 16 + (12 * 256 - db) * (90 * 16) / (42 * 256), 14 * 16, 104 * 16);
    }
}
static void hero_phaser(uint16_t col)
{
    static const int32_t GX[3] = {61, 134, 207};
    static const char *const GL[3] = {"100", "1K", "10K"};
    uint32_t k = set.fx_sel, nst = (uint32_t)fxp[k].f, inc = fxp[k].d > 0 ? (uint32_t)fxp[k].d : 0u;
    uint32_t ph = fxs[k].pha.ph, hz = (uint32_t)(((uint64_t)inc * 4410000u) >> 32);
    int32_t cn = fxp[k].a, dep = fxp[k].b, fb = fxp[k].c, i, c;
    char s[32], *e;
    for (i = 0; i < 3; i++) {
        dots_v(GX[i], 14, 104, C_DARK);
        ctext(GX[i], 106, &FONT_MD, GL[i], C_OFF);
    }
    dots_h(6, 234, 40, C_EDGE);                 /* 0 dB */
    cv_rect(clampi(cut_x12(clampi(cn - dep, PH_NLO, PH_NHI)) / 12, 6, 233), 101,   /* the sweep's reach */
            clampi(cut_x12(clampi(cn + dep, PH_NLO, PH_NHI)) / 12, 6, 233)
            - clampi(cut_x12(clampi(cn - dep, PH_NLO, PH_NHI)) / 12, 6, 233) + 1, 3, dimc(col, 3));
    if (fxp[k].e) {                             /* the right side, WIDTH later in the cycle */
        ph_crv(cn + (int32_t)(((int64_t)ph_lfo(ph + (uint32_t)fxp[k].e) * dep) >> 15), nst, fb, fxp[k].dry, fxp[k].wet);
        crv_stroke(6, 233, 10, dimc(col, 4));
    }
    c = cn + (int32_t)(((int64_t)ph_lfo(ph) * dep) >> 15);
    ph_crv(c, nst, fb, fxp[k].dry, fxp[k].wet);
    crv_line(6, 233, col);
    cv_rect(clampi(cut_x12(clampi(c, PH_NLO, PH_NHI)) / 12, 6, 233), 98, 1, 6, C_WHITE);
    e = put_str(put_int(s, (int32_t)nst, 0), " STAGES  ");
    e = put_int(e, (int32_t)(hz / 100u), 0); *e++ = '.';
    *e++ = (char)('0' + hz / 10u % 10u); *e++ = (char)('0' + hz % 10u);
    e = put_str(e, " HZ");
    *e = 0;
    i = text_w(&FONT_SM, s);
    cv_rrect(230 - i, 16, i + 8, 14, 4, C_CARD);   /* (on a plate: the peaks pass under it) */
    cv_text(234 - i, 16, &FONT_SM, s, C_OFF);
}
/* ENSEMBLE: the three taps' delays over one of the chorus LFO's cycles (up: longer), the vibrato's wobble on them;
 * the left side bright, the right (WIDTH) thin and dim; the rates and TONE on top */
static void ens_crv(uint32_t o, uint32_t dt, int32_t lo, int32_t span)   /* one tap (its phase offset o) into crv */
{
    uint32_t k = set.fx_sel, ps = fxs[k].ens.phs + o, pf = fxs[k].ens.phf + o;
    uint32_t ss = (uint32_t)fxp[k].c * dt, sf = (uint32_t)fxp[k].d * dt;
    int32_t x;
    for (x = 8; x <= 232; x++, ps += ss, pf += sf) {
        int32_t d = ens_tap(fxp[k].a, fxp[k].b, ps, pf) >> 8;   /* (Q8 samples) */
        crv[x] = (int16_t)((112 * 16) - (int32_t)((int64_t)(d - lo) * (66 * 16) / span));
    }
}
static void hero_ensemble(uint16_t col)
{
    uint32_t k = set.fx_sel, is = fxp[k].c > 0 ? (uint32_t)fxp[k].c : 1u, iv = fxp[k].d > 0 ? (uint32_t)fxp[k].d : 0u;
    uint32_t dt = 0xFFFFFFFFu / is / 224u, hs = (uint32_t)(((uint64_t)is * 4410000u) >> 32);
    uint32_t hv = (uint32_t)(((uint64_t)iv * 441000u) >> 32), j;
    int32_t reach = (fxp[k].a + fxp[k].b) >> 8, lo = (ENS_BASE >> 8) - reach, span = 2 * reach > 64 ? 2 * reach : 64;
    char s[40], *e;
    cv_rect(8, 112, 225, 1, C_RULE);            /* (the shortest delay the taps reach) */
    if (fxp[k].e)
        for (j = 0; j < 3u; j++) {
            ens_crv(j * ENS_THIRD + (uint32_t)fxp[k].e, dt, lo, span);
            crv_stroke(8, 232, 6, dimc(col, 2));
        }
    for (j = 3u; j-- > 0u;) {                   /* (each tap its own brightness: the three braid, and one colour
                                                   reads as their outline, not as three lines) */
        ens_crv(j * ENS_THIRD, dt, lo, span);
        if (j) crv_stroke(8, 232, 16, dimc(col, j == 1u ? 6u : 4u));
        else crv_line(8, 232, col);
    }
    cv_rect(8, 46, 1, 67, C_WHITE);             /* now */
    e = put_str(s, "");
    e = put_int(e, (int32_t)(hs / 100u), 0); *e++ = '.';
    *e++ = (char)('0' + hs / 10u % 10u); *e++ = (char)('0' + hs % 10u);
    e = put_str(e, " HZ  VIB ");
    e = put_int(e, (int32_t)(hv / 10u), 0); *e++ = '.'; *e++ = (char)('0' + hv % 10u);
    e = put_str(e, " HZ  ");
    e = put_hz_cell(e, note_hz(90 + set.p.fx[k].p[4] * 42 / 127));
    *e = 0;
    cv_text(4, 22, &FONT_MD, s, C_VAL);
}
/* LO-FI: two cycles of a 1 kHz sine (88 samples across), as it is (thin, dim) and as RATE and BITS leave it (the held,
 * quantised steps, bright: the engine's own lf_quant and hold); the rate and the bits on top */
static void hero_lofi(uint16_t col)
{
    const fx_set_t *f = &set.p.fx[set.fx_sel];
    static int16_t held[89];
    uint32_t i, ph = 0, hp = 0x7FFFFFFFu, inc = LF_INC[f->p[0] & 127u], b = f->p[1] & 127u, qs, qi;
    int32_t x, h = 0;
    char s[32], *e;
    lf_step(b, 28000, &qs, &qi);                /* (BITS against the sine's own level, as the engine does) */
    for (i = 0; i < 89u; i++, ph += 97391263u) {   /* (1 kHz a sample: 2^32 x 1000 / 44100) */
        hp += inc;
        if (hp >> 31) { hp &= 0x7FFFFFFFu; h = lf_quant((fx_cos14(ph - 0x40000000u) * 28000) >> 14, qs, qi); }
        held[i] = (int16_t)h;
    }
    cv_rect(8, 79, 225, 1, C_RULE);
    for (x = 8, ph = 0; x <= 232; x++, ph += 38261210u)   /* (88 samples over 224 px) */
        crv[x] = (int16_t)(79 * 16 - ((fx_cos14(ph - 0x40000000u) * 28000) >> 14) * 512 / 28000);
    crv_stroke(8, 232, 10, dimc(col, 4));
    for (x = 8; x <= 232; x++) crv[x] = (int16_t)(79 * 16 - held[(x - 8) * 88 / 224] * 512 / 28000);
    crv_line(8, 232, col);
    e = put_str(put_hz(s, LF_HZ[f->p[0] & 127u]), "  ");
    e = put_str(put_dec(e, 1600 - (int32_t)b * 1400 / 127, 1), " BITS");
    *e = 0;
    cv_text(4, 22, &FONT_MD, s, C_VAL);
}
/* FILTER: its response where SOURCE has the cutoff now (the main filter's picture: 20 Hz .. 20 kHz, +18 .. -48 dB),
 * STEREO's right side thin and dim; under it the cutoff's reach, a white tick where it is now; the type and the
 * source on top */
static char *put_ms(char *p, int32_t ms100);
static int32_t ff_now(uint32_t k, uint32_t right)   /* the cutoff now (a note, Q8), as fx_filter has it */
{
    const fx_flt_t *s = &fxs[k].flt;
    uint32_t src = ((uint32_t)fxp[k].f >> 8) & 255u;
    int32_t m;
    if (src == FS_ENV) {
        int32_t lv = svf_log2_q8((uint32_t)(s->env >> 8) + 1u) - (7 << 8);
        m = lv <= 0 ? 0 : lv >= (8 << 8) ? 32767 : lv * 32767 / (8 << 8);
    } else m = fx_cos14(s->ph - 0x40000000u + (right && src == FS_STEREO ? (uint32_t)fxp[k].d : 0u)) * 2;
    return fxp[k].a + (int32_t)(((int64_t)m * fxp[k].b) >> 15);
}
static void ff_crv(uint32_t type, uint32_t res, int32_t n)   /* the response at cutoff n into crv */
{
    int32_t x, pc = cut_x12(n);
    for (x = 6; x <= 233; x++)
        crv[x] = (int16_t)clampi(320 + (18 * 256 - svf_db_q8(type, res, x * 12 - pc)) * 84 / (66 * 16), 14 * 16, 104 * 16);
}
static void hero_wfilter(uint16_t col)
{
    static const int32_t GX[3] = {61, 134, 207};
    static const char *const GL[3] = {"100", "1K", "10K"};
    const fx_set_t *f = &set.p.fx[set.fx_sel];
    uint32_t k = set.fx_sel, type = (uint32_t)fxp[k].f & 255u, src = ((uint32_t)fxp[k].f >> 8) & 255u, res = (uint32_t)fxp[k].e;
    int32_t i, cn = fxp[k].a, dep = fxp[k].b, lo, hi, now;
    char s[32], *e;
    for (i = 0; i < 3; i++) {
        dots_v(GX[i], 14, 104, C_DARK);
        ctext(GX[i], 106, &FONT_MD, GL[i], C_OFF);
    }
    dots_h(6, 234, 43, C_DARK);                 /* 0 dB */
    lo = src == FS_ENV ? (dep < 0 ? cn + dep : cn) : cn - (dep < 0 ? -dep : dep);   /* (ENV: one way; an LFO: both) */
    hi = src == FS_ENV ? (dep < 0 ? cn : cn + dep) : cn + (dep < 0 ? -dep : dep);
    lo = clampi(cut_x12(clampi(lo, 0, SVF_NMAX)) / 12, 6, 233);
    hi = clampi(cut_x12(clampi(hi, 0, SVF_NMAX)) / 12, 6, 233);
    cv_rect(lo, 101, hi - lo + 1, 3, dimc(col, 3));
    if (src == FS_STEREO) {
        ff_crv(type, res, ff_now(k, 1));
        crv_stroke(6, 233, 10, dimc(col, 3));
    }
    now = ff_now(k, 0);
    ff_crv(type, res, now);
    crv_fill(6, 233, 14, 104, col, 6u);
    crv_line(6, 233, col);
    cv_rect(clampi(cut_x12(clampi(now, 0, SVF_NMAX)) / 12, 6, 233), 98, 1, 6, C_WHITE);
    e = put_str(put_str(s, WTYPE_NAME[f->p[2] % FF_NTYPE]), "  ");
    e = put_str(put_str(e, WSRC_NAME[src % FS_N]), " ");
    if (src == FS_SYNC) e = put_str(e, ARP_RATE_NAME[f->p[5] % ARP_NRATE]);
    else if (src == FS_ENV) e = put_ms(e, 2000 * LFO_INC[(f->p[5] & 127u) >> 1] / LFO_INC[0]);   /* (x 100 ms) */
    else {
        uint32_t hz = (uint32_t)(((uint64_t)(uint32_t)fxp[k].c * 4410000u) >> 32);
        e = put_int(e, (int32_t)(hz / 100u), 0); *e++ = '.';
        *e++ = (char)('0' + hz / 10u % 10u); *e++ = (char)('0' + hz % 10u);
        e = put_str(e, " HZ");
    }
    *e = 0;
    i = text_w(&FONT_SM, s);
    cv_rrect(230 - i, 16, i + 8, 14, 4, C_CARD);
    cv_text(234 - i, 16, &FONT_SM, s, C_OFF);
}
static void hero_triple(uint16_t col)           /* TRIPLE: two cycles of a sine (thin) and what S&H and the drive make
                                                   of it (the sound's own code, at 1 kHz); the settings on top */
{
    const fx_set_t *f = &set.p.fx[set.fx_sel % FX_EQ];
    uint32_t sh = 1u + f->p[3] * f->p[3] * 31u / 16129u, x, hc = 0;
    int32_t g = f->p[4] ? DIST_GAIN[f->p[4] & 127u] : 0, held = 0;
    char s[32], *e;
    dots_h(14, 226, 64, C_DARK);
    for (x = 0; x <= 210u; x++) {               /* (210 px: two cycles; a pixel ~ 0.42 sample at 1 kHz) */
        int32_t in = ph_lfo((uint32_t)x * (uint32_t)(0x100000000ull / 105u)) >> 1;   /* (half scale) */
        crv[14 + x] = (int16_t)(64 * 16 - in * 38 * 16 / 32768);
    }
    crv_stroke(14, 224, 10, C_RULE);
    for (x = 0; x <= 210u; x++) {
        int32_t in = ph_lfo((uint32_t)x * (uint32_t)(0x100000000ull / 105u)) >> 1;
        uint32_t si = x * 42u / 100u;           /* (the sample under this pixel: 0.42 a pixel) */
        if (si >= hc * sh) { held = in; hc = si / sh + 1u; }   /* (S&H: a new sample every sh) */
        crv[14 + x] = (int16_t)(64 * 16 - tri_drive(held, g) * 38 * 16 / 32768);
    }
    crv_line(14, 224, col);
    e = put_str(put_int(put_str(s, "S&H "), (int32_t)sh, 0), "  DRIVE x");
    e = put_str(put_int(put_str(put_int(e, g ? g / 256 : 1, 0), "  CHO "), (int32_t)f->p[2] * 100 / 127, 0), "%");
    *e = 0;
    cv_text(6, 106, &FONT_MD, s, C_VAL);         /* (under the curves) */
}
static void hero_fx(uint16_t col)               /* the chain on top; the slot's picture: its echoes, or its tail */
{
    const fx_set_t *f = &set.p.fx[set.fx_sel < FX_EQ ? set.fx_sel : 0u];
    char s[32], *e;
    uint32_t k;
    int32_t x = 4;
    for (k = 0; k < 3u; k++) {                  /* "1 DELAY > 2 REVERB > EQ" */
        if (k < FX_EQ) {
            e = put_str(put_int(s, (int32_t)k + 1, 0), " ");
            e = put_str(put_str(e, FXTYPE_NAME[set.p.fx[k].type % FX_N]), " >");
        } else e = put_str(s, "EQ");
        *e = 0;
        x = cv_text(x, 0, &FONT_MD, s, k == set.fx_sel ? col : C_OFF) + 6;
    }
    if (set.fx_sel >= FX_EQ) hero_eq(col);
    else if (f->type == FX_DELAY) {               /* the echoes: each the feedback times the last; PING: left (bright)
                                                   and right (dim) by turns */
        uint32_t t = fx_time(f), ms = t * 10u / 441u;
        int32_t g = 16384, i;
        e = s;
        if (f->p[2]) e = put_str(put_str(e, ARP_RATE_NAME[f->p[0] % ARP_NRATE]), "  ");
        e = put_str(put_int(e, (int32_t)ms, 0), " MS");
        *e = 0;
        cv_text(4, 22, &FONT_MD, s, C_VAL);
        cv_rect(8, 114, 224, 1, C_RULE);
        cv_rrect(8, 54, 4, 61, 2, C_VAL);       /* the note itself */
        for (i = 1; i <= 8; i++) {
            int32_t h = (int32_t)((g * f->mix / 127) * 60 / 16384), xe = 8 + i * 27;
            int ping = f->p[4] > 64u;
            g = (g * f->p[1] * 127) >> 14;
            if (h < 1) break;
            cv_rrect(xe, 114 - h, 4, h + 1, 2, ping && !(i & 1) ? dimc(col, 4) : col);
        }
    } else if (f->type == FX_REVERB) {          /* the tail in dB: from the predelay down to -60 dB at RT */
        uint32_t rt = fx_rt_ms(f->p[0]), pre = f->p[2] * 100u / 127u, win = rt + rt / 4u + pre + 1u;
        int32_t x0 = 8 + (int32_t)(pre * 224u / win), x1 = 8 + (int32_t)((pre + rt) * 224u / win), c;
        e = put_str(s, "RT ");
        e = put_int(e, (int32_t)(rt / 1000u), 0); *e++ = '.'; e = put_int(e, (int32_t)(rt % 1000u / 100u), 0);
        e = put_str(e, " S  PRE "); e = put_str(put_int(e, (int32_t)pre, 0), " MS");
        *e = 0;
        cv_text(4, 22, &FONT_MD, s, C_VAL);
        cv_rect(8, 114, 224, 1, C_DARK);
        for (c = x0; c < x1; c++) {             /* (the highs, DAMP: fainter on top) */
            int32_t h = 64 * (x1 - c) / (x1 - x0 > 0 ? x1 - x0 : 1), hd = h * (127 - (int32_t)f->p[1]) / 127;
            cv_rect(c, 114 - h, 1, h - hd, dimc(col, 3));
            cv_rect(c, 114 - hd, 1, hd, col);
        }
    } else if (f->type == FX_DIST) {            /* the curve: what comes out (up, -1 .. 1) for what goes in (across,
                                                   -0.25 .. 0.25: where a note is), the drive in it; the line: x 1 */
        uint32_t g = DIST_GAIN[f->p[0] & 127u], sh = f->p[2] % DS_N;
        int32_t i;
        e = put_str(put_str(s, XSHAPE_NAME[sh]), "  x");
        e = put_int(e, (int32_t)(g >> 8), 0); *e++ = '.'; e = put_int(e, (int32_t)((g & 255u) * 10u >> 8), 0);
        *e = 0;
        cv_text(4, 22, &FONT_MD, s, C_VAL);
        dots_h(60, 180, 82, C_DARK);            /* the axes: 120 px each way, the centre at (120, 82) */
        dots_v(120, 44, 120, C_DARK);
        crv_seg(60, 91 * 16, 180, 73 * 16);     /* x 1 */
        crv_stroke(60, 180, 9, C_OFF);
        for (i = 0; i <= 120; i++) {
            int32_t x = (i - 60) * 8192 / 60, y = dist_shape((int32_t)(((int64_t)x * g) >> 8), sh);
            crv[60 + i] = (int16_t)(82 * 16 - y * 576 / 32768);
        }
        crv_line(60, 180, col);
    } else if (f->type == FX_CHORUS) {          /* the delay over two of the LFO's cycles, the left (bright) and the
                                                   right (WIDTH later), where it is now (a mark) */
        uint32_t inc = fxp[set.fx_sel].d > 0 ? (uint32_t)fxp[set.fx_sel].d : 0u, hz = (uint32_t)(((uint64_t)inc * 4410000u) >> 32);
        uint32_t base = (uint32_t)fxp[set.fx_sel].a >> 8, dep = (uint32_t)fxp[set.fx_sel].b >> 8, ph = fxs[set.fx_sel].cho.ph;
        uint32_t span = base + dep + 1u, k;
        int32_t i;
        e = put_str(s, "");
        e = put_int(e, (int32_t)(hz / 100u), 0); *e++ = '.';
        *e++ = (char)('0' + hz / 10u % 10u); *e++ = (char)('0' + hz % 10u);
        e = put_str(e, " HZ  ");
        e = put_int(e, (int32_t)(base * 10u / 441u), 0); e = put_str(e, " MS");
        *e = 0;
        cv_text(4, 22, &FONT_MD, s, C_VAL);
        cv_rect(8, 114, 224, 1, C_RULE);
        for (i = 0; i <= 224; i++)              /* two cycles across; up: a longer delay. The right, dim */
            crv[8 + i] = (int16_t)(cho_y((uint32_t)i * (0xFFFFFFFFu / 112u) + (uint32_t)fxp[set.fx_sel].e, base, dep, span) * 16);
        crv_stroke(8, 232, 12, dimc(col, 4));
        for (i = 0; i <= 224; i++)              /* the left, bright */
            crv[8 + i] = (int16_t)(cho_y((uint32_t)i * (0xFFFFFFFFu / 112u), base, dep, span) * 16);
        crv_line(8, 232, col);
        k = 8u + (uint32_t)(((uint64_t)ph * 112u) >> 32);   /* (the first cycle: now) */
        cv_rect((int32_t)k, 46, 1, 68, C_WHITE);
    } else if (f->type == FX_PHASER) {
        hero_phaser(col);
    } else if (f->type == FX_ENSEMBLE) {
        hero_ensemble(col);
    } else if (f->type == FX_LOFI) {
        hero_lofi(col);
    } else if (f->type == FX_FILTER) {
        hero_wfilter(col);
    } else if (f->type == FX_TRIPLE) {
        hero_triple(col);
    } else {
        ctext(120, 36, &FONT_MD, "TYPE: DELAY REVERB DIST", C_OFF);
        ctext(120, 56, &FONT_MD, "CHORUS PHASER ENSEMBLE", C_OFF);
        ctext(120, 76, &FONT_MD, "LO-FI FILTER", C_OFF);
    }
}
/* ---- the value lens: a number knob turning shows its value in its own units, as the picture (2 s, as the cell):
 * the full name, the value big, a line about it, the range. Only for the knobs whose page picture does not show
 * them already (WAVE, CUTOFF, the envelopes, the mixer, the effects keep their pictures) */
static const char *const INTERVAL[13] = {"UNISON", "MINOR 2ND", "MAJOR 2ND", "MINOR 3RD", "MAJOR 3RD", "PERFECT 4TH",
                                         "TRITONE", "PERFECT 5TH", "MINOR 6TH", "MAJOR 6TH", "MINOR 7TH", "MAJOR 7TH",
                                         "OCTAVE"};
static char *put_note(char *p, int32_t n)       /* "C#4" (MIDI 60 = C4) */
{
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    return put_int(put_str(p, NN[n % 12]), n / 12 - 1, 0);
}
static char *put_dec(char *p, int32_t x100, int32_t places)   /* x100 as a number with 1 or 2 decimals */
{
    int32_t a = x100 < 0 ? -x100 : x100;
    if (x100 < 0) *p++ = '-';
    p = put_int(p, a / 100, 0);
    *p++ = '.';
    *p++ = (char)('0' + a / 10 % 10);
    if (places > 1) *p++ = (char)('0' + a % 10);
    return p;
}
static int32_t exp2_x100(int32_t q16)           /* 100 x 2^(q16 / 65536), 0 <= q16 < 20 octaves */
{
    int32_t ip = q16 >> 16, m = svf_exp2_q12((int32_t)(((uint32_t)q16 & 0xFFFFu) * 264u >> 16));   /* Q12 */
    return (int32_t)(((uint64_t)(uint32_t)m * 100u << ip) >> 12);
}
static char *put_ms(char *p, int32_t ms100)     /* a time, ms x 100: "4.2 MS", "420 MS", "1.25 S" */
{
    if (ms100 < 1000) return put_str(put_dec(p, ms100, 1), " MS");
    if (ms100 < 100000) return put_str(put_int(p, (ms100 + 50) / 100, 0), " MS");
    return put_str(put_dec(p, ms100 / 1000, 2), " S");
}
static char *put_db(char *p, uint32_t num, uint32_t den)   /* 20 log10(num / den), one decimal */
{
    int32_t d;
    if (!num) return put_str(p, "-INF DB");
    d = (svf_log2_q8(num * 64u) - svf_log2_q8(den * 64u)) * 6021 / 25600;   /* (dB x 10: 6.0206 dB an octave) */
    if (d > 0) *p++ = '+';
    return put_str(put_dec(p, d * 10, 1), " DB");
}
static char *put_interval(char *p, int32_t st)  /* "PERFECT 5TH", "OCT + MINOR 3RD", "DOWN MAJOR 2ND" */
{
    int32_t a = st < 0 ? -st : st;
    if (st < 0) p = put_str(p, "DOWN ");
    if (a > 12) { p = put_str(p, a >= 24 && a % 12 == 0 ? "" : a >= 24 ? "2 OCT + " : "OCT + "); if (a >= 24 && a % 12 == 0) return put_str(put_int(p, a / 12, 0), " OCTAVES"); a %= 12; if (!a) return p; }
    return put_str(p, INTERVAL[a]);
}
/* id's value v in its units: the full name, the value (big), its unit, a line about it; 0: no lens for id */
static int lens_text(uint32_t id, int32_t v, const char **full, char *big, const char **unit, char *sub)
{
    char *e = big, *u = sub;
    *unit = "";
    switch (id) {
    case P_SEMI: case P_TRANS:
        *full = id == P_SEMI ? "SEMITONES" : "TRANSPOSE";
        e = put_int(e, v, 1); *unit = "ST";
        u = put_str(put_interval(u, v), "  ");
        u = put_note(put_str(put_note(u, 60), " > "), 60 + v);
        break;
    case P_FINE: case P_TUNE:                   /* (A4 against 440 Hz) */
        *full = id == P_FINE ? "FINE TUNE" : "MASTER TUNE";
        e = put_int(e, v, 1); *unit = "CT";
        u = put_str(put_dec(put_str(u, "A4 = "), (int32_t)(((uint64_t)44000u * pitch_mul_q20(0, v)) >> 20), 2), " HZ");
        break;
    case P_VOL:
        *full = "PATCH VOLUME";
        e = put_int(e, v, 0);
        u = put_db(u, (uint32_t)v, 128u);
        break;
    case P_BEND:
        *full = "BEND RANGE";
        e = put_str(put_int(e, v, 0), ""); *unit = "ST";
        u = v ? put_str(put_str(u, "UP AND DOWN: "), v == 12 ? "AN OCTAVE" : v == 24 ? "TWO OCTAVES" : "") : put_str(u, "OFF");
        if (v && v != 12 && v != 24) u = put_interval(u, v);
        break;
    case P_GLIDE: case P_LDELAY:
        *full = id == P_GLIDE ? "GLIDE TIME" : "LFO DELAY";
        if (!v) { e = put_str(e, "OFF"); break; }
        e = put_ms(e, exp2_x100(v * 6857));     /* (1 ms .. 10 s: 2^(v x 13.29 / 127) ms) */
        u = put_str(u, id == P_GLIDE ? "TO 1/1000 OF THE WAY" : "FADING IN AFTER THE NOTE");
        break;
    case P_DRIVE:
        *full = "DRIVE";
        if (!v) { e = put_str(e, "OFF"); break; }
        e = put_dec(put_str(e, "X"), (256 + v * 14) * 100 / 256, 1);
        u = put_str(u, "INTO A SOFT CLIP, BEFORE THE FILTER");
        break;
    case P_FENV: case P_FVEL:
        *full = id == P_FENV ? "ENV AMOUNT" : "VELO > CUTOFF";
        e = put_int(e, id == P_FENV ? v * 390 / 256 : v * 1573 * 127 / 1024 / 256, 1); *unit = "ST";
        u = put_str(u, id == P_FENV ? "AT THE ENVELOPE'S TOP" : "AT VELOCITY 127");
        break;
    case P_KTRK:
        *full = "KEY TRACK";
        e = put_int(e, v, 0); *unit = "%";
        u = put_str(put_int(put_str(u, "AN OCTAVE UP: CUTOFF +"), v * 12 / 100, 0), " ST");
        break;
    case P_LRATE:
        if (set.p.lfo[set.mod_lfo].sync) return 0;
        *full = "LFO RATE";
        e = put_dec(e, (int32_t)lfo_hz100(LFO_INC[v & 127]), 2); *unit = "HZ";   /* (Hz x 100) */
        u = put_str(put_dec(put_str(u, "A CYCLE "), (int32_t)(10000u * 15583u / (LFO_INC[v & 127] ? LFO_INC[v & 127] : 1u)), 2), " S");
        break;
    case P_LPHASE:
        *full = "START PHASE";
        e = put_int(e, v * 360 / 128, 0); *unit = "DEG";
        u = put_str(u, "WITH KEY ON: WHERE EACH NOTE STARTS IT");
        break;
    case P_UDET:
        *full = "UNISON DETUNE";
        e = put_int(put_str(e, "+-"), (100 * v * v + 8064) / 16129, 0); *unit = "CT";
        u = put_str(u, "THE OUTER COPIES");
        break;
    case P_USPR:
        *full = "UNISON SPREAD";
        e = put_int(e, v * 100 / 127, 0); *unit = "%";
        u = put_str(u, v ? "THE COPIES ACROSS LEFT .. RIGHT" : "MONO");
        break;
    case P_ATEMPO:
        *full = "TEMPO";
        e = put_int(e, v, 0); *unit = "BPM";
        u = put_str(put_int(put_str(u, "A QUARTER "), 60000 / (v ? v : 1), 0), " MS");
        break;
    case P_KEYVEL:
        *full = "KEY VELOCITY";
        e = put_int(e, v, 0);
        u = put_str(u, "THE FM-1'S KEYS PLAY AT THIS");
        break;
    default:
        return 0;
    }
    *e = 0; *u = 0;
    return 1;
}
static void hero_lens(uint16_t col, uint32_t id)
{
    const param_t *pr = &PARAM[id];
    const char *full, *unit;
    char big[20], sub[44], s[8], *e;
    int32_t v = pget(id), hi = param_hi(id), span = hi - pr->lo, x, w, z;
    if (!lens_text(id, v, &full, big, &unit, sub)) return;
    cv_text(8, 0, &FONT_MD, full, col);
    w = text_w(&FONT_LG, big) + (unit[0] ? 6 + text_w(&FONT_MD, unit) : 0);
    x = cv_text(120 - w / 2, 20, &FONT_LG, big, C_WHITE);
    if (unit[0]) cv_text(x + 6, 29, &FONT_MD, unit, C_VAL);
    ctext(120, 60, &FONT_MD, sub, C_VAL);
    if (span <= 0) return;
    cv_rect(20, 92, 200, 4, C_DARK);            /* the range: the value from 0 (or the bottom), the ends' numbers */
    x = 20 + 200 * (v - pr->lo) / span;
    z = pr->lo < 0 ? 20 + 200 * (0 - pr->lo) / span : 20;
    cv_rect(z < x ? z : x, 92, z < x ? x - z : z - x, 4, col);
    cv_rect(x - 1, 88, 3, 12, C_WHITE);
    e = put_int(s, pr->lo, pr->lo < 0); *e = 0;
    cv_text(20, 104, &FONT_MD, s, C_OFF);
    e = put_int(s, hi, pr->lo < 0); *e = 0;
    cv_text(220 - text_w(&FONT_MD, s), 104, &FONT_MD, s, C_OFF);
    if (pr->lo < 0) ctext(z, 104, &FONT_MD, "0", C_OFF);
}
/* ---- INIT undone (2026-11-16): SHIFT + a section's key again, nothing changed since, puts back what INIT set back. The
 * patch as it was, and the pattern SEQ's INIT fills (it lives outside the patch); what was set back: the section, MOD's
 * kind of page, the one picked; the checksum of everything after INIT (anything changed since: INIT again) */
static struct {
    patch_t p;
    uint16_t st[ARP_PLEN];
    uint8_t len, tfac, pat, ok;
    uint32_t key, sum;
} undo;
static uint32_t undo_key(uint32_t s)
{
    uint32_t r = s == SEC_MOD && set.mod_row >= 2u ? set.mod_row : 0u;
    uint32_t i = s == SEC_STEPS ? pat_num() : s == SEC_MIX || s == SEC_ARP || !sec_insts(s) ? 0u : *sec_inst(s);
    return s | r << 8 | i << 16;
}
static uint32_t fnv(uint32_t h, const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}
static uint32_t undo_sum(void)
{
    uint32_t n = pat_num(), h = fnv(2166136261u, &set.p, sizeof set.p);
    h = fnv(h, pat_st(n), 2u * ARP_PLEN);
    h = fnv(h, pat_len(n), 1u);
    return fnv(h, pat_tfac(n), 1u);
}
static uint32_t undo_ready(void)                /* the key INIT can be undone on now (none: ~0) */
{
    return undo.ok && undo.sum == undo_sum() ? undo.key : ~0u;
}
static void undo_take(uint32_t s)
{
    uint32_t n = pat_num();
    undo.p = set.p;
    memcpy(undo.st, pat_st(n), sizeof undo.st);
    undo.len = *pat_len(n);
    undo.tfac = *pat_tfac(n);
    undo.pat = (uint8_t)n;
    undo.key = undo_key(s);
}
static void undo_back(void)
{
    uint32_t m = undo.p.arp.mode;
    undo.p.arp.mode = set.p.arp.mode;           /* (the arp's mode through arp_mode_set: it starts or stops it) */
    set.p = undo.p;
    arp_mode_set(m);
    memcpy(pat_st(undo.pat), undo.st, sizeof undo.st);
    *pat_len(undo.pat) = undo.len;
    *pat_tfac(undo.pat) = undo.tfac;
    undo.ok = 0;
    sound_apply();
}

/* ---- MOD held on another page (2026-11-16): a knob turned makes a matrix slot onto what it turns (LFO 1 on it, the
 * turns its amount; one already on it: that one), SELECT picks its source. What a knob turns, as a destination (the
 * one SELECT picked; none: MD_OFF) */
static uint32_t mod_dest(uint32_t id)
{
    uint32_t o = set.osc_sel % NOSC, f = set.filt_sel & 1u, n = set.env_sel % NENV;
    switch (id) {
    case P_WAVE: return MD_W1 + o;
    case P_SEMI: case P_FINE: return MD_P1 + o;
    case P_UDET: return MD_DET1 + o;
    case P_FMA: return MD_FM1 + o;
    case P_L1: case P_L2: case P_L3: return MD_L1 + (id - P_L1);
    case P_NOISE: return MD_NOISE;
    case P_RING: return MD_RING;
    case P_COLOR: return MD_COLOR;
    case P_BAL1: case P_BAL2: case P_BAL3: return MD_BAL1 + (id - P_BAL1);
    case P_BALN: return MD_BALN;
    case P_BALR: return MD_BALR;
    case P_CUT: return MD_CUT1 + f;
    case P_RES: return MD_RES1 + f;
    case P_FENV: return MD_FENV1 + f;
    case P_DRIVE: return MD_DRV1 + f;
    case P_FFMA: return MD_FFM1 + f;
    case P_FPAN: return MD_FPAN1 + f;
    case P_ATK: case P_DEC: case P_SUS: case P_REL: return MD_AATK + 4u * n + (id - P_ATK);
    case P_VOL: return MD_VOL;
    case P_MORPH: return MD_WAVE;
    default: return MD_OFF;
    }
}
static uint32_t mod_assign(uint32_t k, int32_t e)   /* knob k turned e under MOD: 1 when the patch changed */
{
    uint32_t id = page_param(set.sec, ui_row(), k), d = id ? mod_dest(id) : MD_OFF, i;
    mod_slot_t *m;
    if (d == MD_OFF) { ui_msg("NOT A MOD DEST", 0, 0); return 0; }
    if (ui.ma_slot < 0 || pslot(&set.p, (uint32_t)ui.ma_slot)->dst != d) {
        int32_t at = -1;
        for (i = 0; i < NMSLOT && at < 0; i++)  /* (a slot already on it: that one) */
            if (pslot(&set.p, i)->src != MS_OFF && pslot(&set.p, i)->dst == d) at = (int32_t)i;
        for (i = 0; i < NMSLOT && at < 0; i++)  /* (or the first free) */
            if (pslot(&set.p, i)->src == MS_OFF) at = (int32_t)i;
        if (at < 0) { ui_msg("MATRIX FULL", 0, 0); return 0; }
        m = pslot(&set.p, (uint32_t)at);
        if (m->src == MS_OFF) { m->src = MS_LFO1; m->dst = (uint8_t)d; m->amt = 0; }
        ui.ma_slot = (int8_t)at;
    }
    m = pslot(&set.p, (uint32_t)ui.ma_slot);
    m->amt = (int8_t)clampi(m->amt + e, -63, 63);
    set.mod_slot = (uint8_t)ui.ma_slot;         /* (MOD's page then opens on it) */
    set.mod_row = 2u;
    ui.ma_used = 1;
    ui.hero = 1;
    sound_apply();
    return 1;
}
static void hero_assign(void)                   /* MOD held: what a knob does now, then the slot it made */
{
    uint16_t col = SEC_COL[SEC_MOD];
    const mod_slot_t *m;
    char s[32], *e;
    cv_text(6, 1, &FONT_MD, "MOD +", C_WHITE);
    if (ui.ma_slot < 0) {
        ctext(120, 34, &FONT_MD, "TURN A KNOB:", C_VAL);
        ctext(120, 52, &FONT_MD, "A MATRIX SLOT ONTO IT", C_VAL);
        ctext(120, 84, &FONT_SM, "LET GO UNTURNED: THE MOD PAGE", C_OFF);
        return;
    }
    m = pslot(&set.p, (uint32_t)ui.ma_slot);
    e = put_int(put_str(s, "SLOT "), ui.ma_slot + 1, 0);
    *e = 0;
    cv_text(234 - text_w(&FONT_MD, s), 1, &FONT_MD, s, col);
    e = put_str(put_str(put_str(s, MSRC_NAME[m->src % MS_N]), "  >  "), MDST_NAME[m->dst % MD_N]);
    *e = 0;
    ctext(120, 28, &FONT_MD, s, C_WHITE);
    e = put_int(s, m->amt, 1);
    *e = 0;
    ctext(120, 50, &FONT_LG, s, col);
    ctext(120, 100, &FONT_SM, "KNOB: AMOUNT    SELECT: SOURCE", C_OFF);
}

static char *shift_target(char *e)              /* what SHIFT + the current section's key sets back (none: e) */
{
    static const char *const EN[NENV] = {"AMP ENV", "FLT ENV", "ENV 3", "ENV 4"};
    switch (set.sec) {
    case SEC_OSC: return put_int(put_str(e, "OSC "), set.osc_sel + 1, 0);
    case SEC_MIX: return put_str(e, "MIXER");
    case SEC_FILT: return put_int(put_str(e, "FILTER "), set.filt_sel + 1, 0);
    case SEC_ENV: return put_str(e, EN[set.env_sel % NENV]);
    case SEC_MOD: return set.mod_row < 2u ? put_int(put_str(e, "LFO "), set.mod_lfo + 1, 0)
                         : set.mod_row == 3u ? put_int(put_str(e, "MOD "), set.mod_mf + 1, 0)
                                          : put_int(put_str(e, "SLOT "), set.mod_slot + 1, 0);
    case SEC_FX: return set.fx_sel >= FX_EQ ? put_str(e, "EQ") : put_int(put_str(e, "FX "), set.fx_sel + 1, 0);
    case SEC_ARP: return put_str(e, "ARP");
    case SEC_STEPS: return put_str(e, "PATTERN");
    default: return e;
    }
}
/* SHIFT held: what it does, laid out as the panel is. The two rows of keys as they sit on the FM-1 (each with its own
 * name too: the SMK-37's lie elsewhere), the current section's key in its colour; on top what its INIT sets back
 * now; under them SELECT and OCT, and the knobs, as they lie left and right of the keys */
static void shift_key(int32_t x, int32_t y, int32_t w, const char *key, const char *act, uint16_t kc, uint16_t fill,
                      uint16_t ac)
{
    cv_rrect(x, y, w, 30, 5, fill);
    ctext(x + w / 2, y + 3, &FONT_SM, key, kc);
    ctext(x + w / 2, y + 16, &FONT_SM, act, ac);
}
static void hero_shift(void)
{
    static const uint8_t TOP[6] = {SEC_OSC, SEC_MIX, SEC_FILT, SEC_ENV, SEC_MOD, SEC_FX};
    static const char *const TOPK[6] = {"OSC", "MIX", "FILT", "ENV", "MOD", "FX"};
    static const char *const BOTK[6] = {"PATCH", "SAVE", "ARP", "SEQ", "RUN", "SHIFT"};
    static const char *const BOTA[6] = {"SETUP", "RECALL", "INIT", "INIT", "HOLD", "HELD"};
    static const uint8_t BOTS[6] = {SEC_PATCH, 0xFFu, SEC_ARP, SEC_STEPS, 0xFFu, 0xFFu};
    uint32_t i, cur = set.sec == SEC_SETUP ? SEC_PATCH : set.sec, rd = undo_ready();
    uint16_t col = SEC_COL[cur];
    char s[24], *e;
    cv_text(6, 1, &FONT_MD, "SHIFT +", C_WHITE);
    e = shift_target(put_str(s, rd == undo_key(cur) ? "UNDO: " : "INIT: "));
    *e = 0;
    if (e - s > 6) cv_text(234 - text_w(&FONT_SM, s), 4, &FONT_SM, s, col);
    for (i = 0; i < 12u; i++) {                 /* the keys: 6 a row, 38 px each, 2 apart */
        uint32_t top = i < 6u, k = i % 6u, sec = top ? TOP[k] : BOTS[k];
        int32_t x = 1 + (int32_t)k * 40, y = top ? 22 : 55;
        const char *act = sec != 0xFFu && sec != SEC_PATCH && rd == undo_key(sec) ? "UNDO" : top ? "INIT" : BOTA[k];
        uint16_t c = sec != 0xFFu ? SEC_COL[sec] : C_OFF;
        if (!top && k == 5u)                    /* SHIFT itself: lit, as its LED */
            shift_key(x, y, 38, BOTK[k], BOTA[k], C_AMBER, mixc(C_CARD, C_AMBER, 4u), C_AMBER);
        else if (sec == cur)                    /* the current section's key: in its colour */
            shift_key(x, y, 38, top ? TOPK[k] : BOTK[k], act, C_WHITE, mixc(C_CARD, c, 6u), C_WHITE);
        else
            shift_key(x, y, 38, top ? TOPK[k] : BOTK[k], act, sec != 0xFFu ? dimc(c, 6) : C_OFF,
                      C_EDGE, C_VAL);
    }
    e = put_int(put_str(s, "TRANS "), set.trans, 1);   /* OCT (left of the keys) and the knobs (right) */
    *e = 0;
    shift_key(1, 89, 78, "SELECT", "ROWS", C_OFF, C_EDGE, C_VAL);   /* (SELECT and OCT left of the keys) */
    shift_key(81, 89, 78, "OCT-  OCT+", s, C_OFF, C_EDGE, C_VAL);
    shift_key(161, 89, 78, "KNOBS", "FINE", C_OFF, C_EDGE, C_VAL);
}
/* the names of a parameter's values, when they are words: the list and how many (0: a number) */
static const char *const SLEN_NAME[9] = {"LEG", "-3", "-2", "-1", "0", "+1", "+2", "+3", "TIE"};
static const char *const STIM_NAME[8] = {"RAND", "-3", "-2", "-1", "0", "+1", "+2", "+3"};
static const char *const ROUTE_NAME[2] = {"SER", "PAR"};
static const char *const *param_names(uint32_t id, uint32_t *n)
{
    switch (PARAM[id].fmt) {
    case F_FTYPE: *n = SVF_NTYPE; return FTYPE_NAME;
    case F_ROUTE: *n = 2; return ROUTE_NAME;
    case F_CURVE: *n = NVC; return CURVE_NAME;
    case F_AMODE: *n = ARP_NMODE; return AMODE_NAME;
    case F_ADIR: *n = AD_N; return ADIR_NAME;
    case F_AORD: *n = AO_N; return AORD_NAME;
    case F_ARATE: *n = ARP_NRATE; return ARP_RATE_NAME;
    case F_AVEL: *n = AV_N; return AVEL_NAME;
    case F_STYPE: *n = AS_N; return STYPE_NAME;
    case F_SACC: *n = 8; return SACC_NAME;
    case F_SLEN: *n = 9; return SLEN_NAME;
    case F_STIM: *n = 8; return STIM_NAME;
    case F_LSHAPE: *n = LS_N; return LSHAPE_NAME;
    case F_LRATE: if (set.p.lfo[set.mod_lfo].sync) { *n = ARP_NRATE; return ARP_RATE_NAME; } break;
    case F_MSRC: *n = MS_N; return MSRC_NAME;
    case F_MDST: {                              /* the group's names */
        static const char *names[24];
        uint32_t g = dst_group((*pslot(&set.p, set.mod_slot)).dst, 0), i;
        for (i = 0; i < DGROUP[g].n && i < 24u; i++) names[i] = MDST_NAME[DGROUP[g].d[i]];
        *n = i;
        return names;
    }
    case F_MGRP: *n = NDG; return DGROUP_NAME;
    case F_MOP: *n = MO_N; return MOP_NAME;
    case F_FMS: *n = FMS_N; return FMS_NAME;
    case F_DCURVE: *n = DC_N; return DCURVE_NAME;
    case F_GMODE: *n = GM_N; return GMODE_NAME;
    case F_PCAT: *n = PC_N; return PCAT_NAME;
    case F_MSRCB: {                             /* (SRC B: OFF is the constant) */
        static const char *names[MS_N];
        uint32_t i;
        for (i = 0; i < MS_N; i++) names[i] = i ? MSRC_NAME[i] : "CONST";
        *n = MS_N;
        return names;
    }
    case F_EMODE: *n = EM_N; return EMODE_NAME;
    case F_VMODE: *n = VM_N; return VMODE_NAME;
    case F_FXTYPE: *n = FX_N; return FXTYPE_NAME;
    case F_XSHAPE: *n = DS_N; return XSHAPE_NAME;
    case F_CMODE: *n = CM_N; return CMODE_NAME;
    case F_DTIME: if (set.p.fx[set.fx_sel].p[2]) { *n = ARP_NRATE; return ARP_RATE_NAME; } break;
    case F_WTYPE: *n = FF_NTYPE; return WTYPE_NAME;
    case F_WSRC: *n = FS_N; return WSRC_NAME;
    case F_WRATE: if (set.p.fx[set.fx_sel].p[3] == FS_SYNC) { *n = ARP_NRATE; return ARP_RATE_NAME; } break;
    default: break;
    }
    *n = 0;
    return 0;
}
static void draw_hero(void)
{
    uint16_t col = SEC_COL[set.sec];
    const char *const *names = 0, *ln_full, *ln_unit;
    char ln_big[20], ln_sub[44];
    uint32_t nn = 0, cid = 0;
    if (ui.focus >= 0 && !ui.save && !ui.shift) {   /* a word-valued knob turning: its choices (2 s, as the cell) */
        cid = page_param(set.sec, ui_row(), (uint32_t)ui.focus);
        if (cid) names = param_names(cid, &nn);
    }
    cv_begin(240, 124, C_BG);
    cv_rrect(0, 0, 240, 124, 7, C_CARD);       /* the picture lies on a card */
    if (ui.save) hero_save();
    else if (ui.ma_on) hero_assign();
    else if (ui.shift && !ui.shift_sel) hero_shift();
    else if (names) hero_choice(col, names, nn, (uint32_t)(pget(cid) - PARAM[cid].lo), PARAM[cid].label);
    else if (cid && lens_text(cid, pget(cid), &ln_full, ln_big, &ln_unit, ln_sub)) hero_lens(col, cid);
    else if (set.sec == SEC_OSC) hero_osc(col);
    else if (set.sec == SEC_MIX) hero_mix(col);
    else if (set.sec == SEC_FILT) { if (set.filt_row == 2u) hero_dcurve(col); else hero_filter(col); }
    else if (set.sec == SEC_ENV) { if (set.env_row == 2u) hero_velo(col); else hero_env(col); }
    else if (set.sec == SEC_PATCH) hero_patch();
    else if (set.sec == SEC_SETUP) hero_setup();
    else if (set.sec == SEC_ARP) hero_arp(col);
    else if (set.sec == SEC_STEPS) hero_pattern(col, 1);
    else if (set.sec == SEC_MOD) hero_mod(col);
    else hero_fx(col);                          /* (SEC_FX: every section has its picture now) */
    cv_blit(0, 60);
}

/* ---- the footer: the section, what SELECT picks, the rows; the patch, the octave, the voices */
static uint32_t voice_q(uint32_t v)             /* 0: silent, 1..9: how loud the envelope is */
{
    return voice[v].busy && voice[v].env.st != ENV_IDLE ? 1u + ((uint32_t)voice[v].env.lvl >> 27) : 0u;
}
static int32_t pill(int32_t x, const char *t, uint16_t fill, uint16_t rim, uint16_t tc)   /* a word in a pill, 20 px
                                                   high at y 3 (fill or rim 0: none); returns its end */
{
    int32_t w = text_w(&FONT_MD, t) + 14;
    if (fill) cv_rrect(x, 3, w, 20, 10, fill);
    else if (rim) cv_rrect_o(x, 3, w, 20, 10, C_BG, rim);
    cv_text(x + 7, 5, &FONT_MD, t, tc);
    return x + w;
}
static void draw_footer(void)
{
    static const char *const NUM[4] = {"1", "2", "3", "4"}, *const ENVN[NENV] = {"AMP", "FLT", "3", "4"};
    uint16_t col = ui.save ? C_AMBER : SEC_COL[set.sec];
    int32_t x, i, n = (int32_t)sec_insts(set.sec), sel = n ? *sec_inst(set.sec) : 0;
    char s[20], *p = s;
    cv_begin(240, 54, C_BG);
    x = pill(4, ui.save ? "SAVE" : SEC_NAME[set.sec], col, 0, C_BG) + 4;   /* the section, a pill in its colour */
    if (ui.save) n = 0;
    else if (set.sec == SEC_MOD && (set.mod_row < 2u || set.mod_row == 3u))   /* the LFOs, the modifiers: "LFO" or
                                                   "MODIF", then their tabs */
        x = cv_text(x + 2, 5, &FONT_MD, set.mod_row == 3u ? "MODIF" : "LFO", C_OFF) + 5;
    else if (set.sec == SEC_STEPS || set.sec == SEC_MOD) {   /* (16 steps, 8 slots: a name, not tabs) */
        char t[12], *q = put_int(put_str(t, set.sec == SEC_STEPS ? "STEP " : "MATRIX "), sel + 1, 0);
        *q = 0;
        pill(x, t, 0, col, C_WHITE);
        n = 0;
    }
    for (i = 0; i < n; i++) {                   /* the tabs: what SELECT picks, the one picked ringed (OSC: each one's
                                                   level under it, crossed out when silent; an LFO no slot reads dim) */
        const char *t = set.sec == SEC_ENV ? ENVN[i] : set.sec == SEC_FX && i == (int32_t)FX_EQ ? "EQ" : NUM[i];
        int32_t w = text_w(&FONT_MD, t) + 12;
        int osc = set.sec == SEC_OSC, mute = osc && !set.p.osc[i].level;
        if (set.sec == SEC_FX)                  /* (a slot off, the EQ flat: dim) */
            mute = i < (int32_t)FX_EQ ? !set.p.fx[i].type : !set.p.eq.gain[0] && !set.p.eq.gain[1] && !set.p.eq.gain[2];
        if (set.sec == SEC_MOD) {               /* (an LFO, a modifier: dim when no slot reads it) */
            uint32_t k, sr = set.mod_row == 3u ? MS_MOD1 + (uint32_t)i : MS_LFO1 + (uint32_t)i;
            for (k = 0, mute = 1; k < NMSLOT && mute; k++) {
                const mod_slot_t *m = pslot(&set.p, k);
                if (m->src == sr && m->dst && m->amt) mute = 0;
            }
        }
        if (i == sel) cv_rrect_o(x, 3, w, 20, 10, C_BG, col);
        cv_text(x + 6, 5, &FONT_MD, t, i == sel ? C_WHITE : mute ? C_DARK : C_OFF);
        if (osc) {
            cv_rect(x + 4, 24, w - 8, 1, C_RULE);
            cv_rect(x + 4, 24, (w - 8) * set.p.osc[i].level / 127, 1, i == sel ? col : C_OFF);
            if (mute) cv_rect(x + 4, 13, w - 8, 1, C_OFF);
        }
        x += w + 1;
    }
    if (sec_rows(set.sec) > 1u && !ui.save)     /* the rows: the one shown a pill, the others dots */
        for (i = 0; i < (int32_t)sec_rows(set.sec); i++) {
            int32_t xr = 236 - ((int32_t)sec_rows(set.sec) - i) * 14;
            if ((uint32_t)i == ui_row()) cv_rrect(xr, 10, 12, 6, 3, col);
            else cv_rrect(xr + 4, 11, 4, 4, 2, C_DARK);
        }
    if (ui.msg[0])                              /* a word for 1.2 s ("MORPH +12") in the patch's place */
        cv_text(6, 30, &FONT_MD, ui.msg, C_WHITE);
    else {
        char pn[20], *q = put_prog(pn, set.prog, set.name);   /* the patch: "07" dim, "SOFT PAD" bright, an amber dot
                                                   when edited */
        *q = 0;
        pn[2] = 0;
        x = cv_text(6, 30, &FONT_MD, pn, C_OFF) + 4;
        x = cv_text(x, 30, &FONT_MD, pn + 3, C_WHITE);
        if (patch_edited()) { cv_rrect(x + 3, 38, 5, 5, 2, C_AMBER); x += 8; }
        x += 8;
        if (set.p.arp.mode != ARP_OFF) {         /* the arpeggiator is on, its tempo (as room allows: the voices at 196) */
            char tb[8], *e = put_int(tb, (int32_t)(arp_ext() && clk_bpm ? clk_bpm : set.p.arp.tempo), 0);
            *e = 0;
            x = cv_text(x, 30, &FONT_MD, set.p.arp.mode == ARP_ON ? "A" : set.p.arp.mode == ARP_HOLD ? "H" : "1",
                        SEC_COL[SEC_ARP]) + 3;
            if (x + text_w(&FONT_MD, tb) < 176) x = cv_text(x, 30, &FONT_MD, tb, C_OFF) + 6;
        }
        if (set.p.vmode != VM_POLY)             /* one voice: M (MONO) or L (LEGATO) */
            x = cv_text(x, 30, &FONT_MD, set.p.vmode == VM_MONO ? "M" : "L", C_WHITE) + 6;
        if ((int8_t)set.oct) { p = put_int(put_str(p, "O"), (int8_t)set.oct, 1); *p++ = ' '; }
        if (set.trans) p = put_int(put_str(p, "T"), set.trans, 1);
        *p = 0;
        if (x + text_w(&FONT_MD, s) < 182) cv_text(x, 30, &FONT_MD, s, C_OFF);
    }
    if (midi_act_ms && fm1_ms - midi_act_ms < 150u) cv_rrect(186, 36, 5, 5, 2, SEC_COL[SEC_MIX]);   /* MIDI coming in */
    for (i = 0; i < (int32_t)NV; i++) {         /* the four voices: a meter each, as high as its envelope */
        uint32_t q = voice_q((uint32_t)i);
        int32_t xv = 197 + i * 10, h = (int32_t)q * 20 / 9;
        cv_rrect(xv, 28, 6, 20, 3, C_RULE);
        if (q) cv_rrect(xv, 48 - (h < 6 ? 6 : h), 6, h < 6 ? 6 : h, 3, mixc(C_RULE, col, 6u + q));
    }
    cv_blit(0, 186);
}

/* ---- the splash: the wordmark (a picture, tools/gen_font.py WORDS: "VOLNA" baked in), what it is, the version; under
 * it five waves rolling by while it stands, the front one filled down to the edge. Under a second */
static void cv_word(int32_t x, int32_t y, const uint8_t *d, int32_t w, int32_t h, uint16_t c)   /* a baked word */
{
    int32_t i, j, bpr = (w + 1) / 2;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            uint32_t a = d[j * bpr + i / 2];
            a = (i & 1) ? (a & 15u) : (a >> 4);
            if (a) cv_mix(x + i, y + j, c, (a * 16u + 7u) / 15u);
        }
}
static void splash_waves(uint32_t t)            /* the lower half (120 rows) at t ms: five sines, 1.5 cycles across,
                                                   each its own phase and speed, brighter to the front */
{
    const wt_row_t *sine = (const wt_row_t *)wt_table(0);   /* (BASIC's wave 0: a sine) */
    uint16_t c = SEC_COL[SEC_FX];
    uint32_t k;
    int32_t x;
    cv_begin(240, 120, C_BG);
    for (k = 0; k < 5u; k++) {
        uint32_t ph0 = k * 410134549u + t * (3006477u + k * 700000u);   /* (0.6 rad apart; ~0.7 cycles a second) */
        int32_t base = (40 + (int32_t)k * 9) * 16;
        for (x = 0; x < 240; x++)
            crv[x] = (int16_t)(base - wt_sample(sine, ph0 + (uint32_t)x * 26843546u, 0) * 160 / 30000);
        if (k < 4u) crv_stroke(0, 239, 10, mixc(C_BG, c, 3u + k * 3u));
        else {
            crv_fill(0, 239, 66, 120, c, 5u);
            crv_line(0, 239, c);
        }
    }
    cv_blit(0, 120);
    lcd_sync();
}
static void ui_splash(void)
{
    uint32_t t0 = fm1_ms;
    lcd_fill(0, 0, 240, 240, C_BG);
    cv_begin(240, 120, C_BG);
    cv_word((240 - WORD_SPLASH_W) / 2, 50, WORD_SPLASH, WORD_SPLASH_W, WORD_SPLASH_H, C_WHITE);
    ctext(120, 95, &FONT_SM, "WAVETABLE SYNTH", C_OFF);
#if !defined(DINGHY_HOST) || !defined(VOLNA_HIDE_SPLASH_VERSION)
    ctext(120, 108, &FONT_SM, "v" APP_VER, C_DARK);
#endif
    cv_blit(0, 0);
    while (fm1_ms - t0 < 700u) {                /* (a frame, then the platform and MIDI, until 0.7 s) */
        splash_waves(fm1_ms - t0);
        platform_poll();
        midi_in();
    }
    lcd_fill(0, 0, 240, 240, C_BG);
    ui.cells = ui.hero = ui.foot = 1;
}

/* ---- the panel */
#define PEEK_MS 400u                            /* a section's key held longer: let go, back where it was (a peek) */
static void ui_go(uint32_t s)
{
    set.sec = (uint8_t)s;
    ui.focus = -1;
    ui.cells = ui.hero = ui.foot = 1;
}
static int section_init(uint32_t s)             /* SHIFT + a section: its defaults (the one SELECT picks) */
{
    uint32_t k, i;
    switch (s) {
    case SEC_OSC:                               /* (all its rows: what lives outside osc[] too, 2026-11-16) */
        k = set.osc_sel % NOSC;
        set.p.osc[k] = PATCH_DEFAULT.osc[k];
        set.p.uni[k] = PATCH_DEFAULT.uni[k]; set.p.udet[k] = PATCH_DEFAULT.udet[k]; set.p.uspr[k] = PATCH_DEFAULT.uspr[k];
        set.p.fms[k] = PATCH_DEFAULT.fms[k]; set.p.fma[k] = PATCH_DEFAULT.fma[k];
        set.p.okt[k] = PATCH_DEFAULT.okt[k]; set.p.obend[k] = PATCH_DEFAULT.obend[k];
        set.p.wlim = (uint8_t)((set.p.wlim & ~(1u << k)) | (PATCH_DEFAULT.wlim & 1u << k));
        if (k == 1u) set.p.sync = PATCH_DEFAULT.sync;   /* (SYNC: OSC2's) */
        break;
    case SEC_MIX:
        for (k = 0; k < NOSC; k++) set.p.osc[k].level = PATCH_DEFAULT.osc[k].level;
        set.p.noise = PATCH_DEFAULT.noise; set.p.ring = PATCH_DEFAULT.ring; set.p.color = PATCH_DEFAULT.color;
        for (k = 0; k < 5u; k++) set.p.bal[k] = PATCH_DEFAULT.bal[k];   /* (row B: the balances) */
        break;
    case SEC_FILT:
        k = set.filt_sel & 1u;
        set.p.flt[k] = PATCH_DEFAULT.flt[k]; set.p.route = PATCH_DEFAULT.route;
        set.p.dcurve[k] = PATCH_DEFAULT.dcurve[k];   /* (row C) */
        set.p.ffms[k] = PATCH_DEFAULT.ffms[k]; set.p.ffma[k] = PATCH_DEFAULT.ffma[k]; set.p.fpan[k] = PATCH_DEFAULT.fpan[k];
        break;
    case SEC_ENV:
        k = set.env_sel % NENV;
        set.p.env[k] = PATCH_DEFAULT.env[k];
        set.p.etrig = (uint8_t)((set.p.etrig & ~(1u << k)) | (PATCH_DEFAULT.etrig & 1u << k));
        break;
    case SEC_ARP:                               /* (the patch's own pattern, OWN, is SEQ's: its LENGTH and T.AMT
                                                   stay, as its steps do) */
        k = set.p.arp.plen; i = set.p.arp.tfac;
        arp_mode_set(ARP_OFF); set.p.arp = PATCH_DEFAULT.arp;
        set.p.arp.pattern = PAT_OWN;
        set.p.arp.plen = (uint8_t)k; set.p.arp.tfac = (uint8_t)i;
        break;
    case SEC_MOD:                               /* the LFO picked back to its default; a slot emptied */
        if (set.mod_row < 2u) {
            k = set.mod_lfo % NLFO;
            set.p.lfo[k] = PATCH_DEFAULT.lfo[k];
            set.p.lfout = (uint8_t)((set.p.lfout & ~(1u << k)) | (PATCH_DEFAULT.lfout & 1u << k));
        }
        else if (set.mod_row == 3u) set.p.modf[set.mod_mf % NMODF] = PATCH_DEFAULT.modf[0];
        else (*pslot(&set.p, set.mod_slot)) = PATCH_DEFAULT.mod[0];
        break;
    case SEC_FX:                                /* the slot off; the EQ flat, at its starting frequencies */
        if (set.fx_sel >= FX_EQ) set.p.eq = PATCH_DEFAULT.eq;
        else set.p.fx[set.fx_sel] = FX_DEF[FX_OFF];
        break;
    case SEC_STEPS:                             /* reset only this patch's pattern */
        for (i = 0; i < ARP_PLEN; i++) set.p.ost[i] = AST_DEFAULT;
        set.p.arp.plen = ARP_PLEN; set.p.arp.tfac = 64u;
        break;
    default: return 0;
    }
    sound_apply();
    return 1;
}
/* STEPS: white key w (a step) pressed: picked, or (picked already) on <-> pause; returns 1 (a change) */
static const uint8_t STEP_OF_KEY[27] = {        /* key (0 = F3) -> step: the 16 white keys F3 .. G5; black: 255 */
    0, 255, 1, 255, 2, 255, 3, 4, 255, 5, 255, 6, 7, 255, 8, 255, 9, 255, 10, 11, 255, 12, 255, 13, 14, 255, 15};
static uint32_t ui_step_key(uint32_t w)
{
    if (w == set.step_sel) {
        uint16_t *st = cur_step();
        *st = (uint16_t)(AST_MAKE(AST_TYPE(*st) == AS_PAUSE ? AS_NORMAL : AS_PAUSE, AST_GLIDE(*st), AST_ACC(*st),
                                  AST_LEN(*st), AST_TIM(*st)) | (AST_TIE(*st) ? AST_TIEBIT : 0u));
    } else set.step_sel = (uint8_t)w;
    ui.cells = ui.hero = ui.foot = 1;
    return 1;
}
static uint32_t steps_lit(uint32_t lit)         /* STEPS: the white keys show the steps that sound, the playing one
                                                   the other way round; the black keys what the voices play */
{
    const uint16_t *pat = pat_st(pat_num());
    uint32_t k, plen = *pat_len(pat_num()), play = arp.run && arp.s ? (arp.s - 1u) % plen : 99u;
    for (k = 0; k < 27u; k++) {
        uint32_t w = STEP_OF_KEY[k];
        if (w >= ARP_PLEN) continue;
        lit &= ~(1u << k);
        if ((w < plen && AST_TYPE(pat[w]) != AS_PAUSE && AST_ACC(pat[w])) != (w == play)) lit |= 1u << k;
    }
    return lit;
}

/* the edges of the buttons since the last pass, the encoders: returns 1 when a setting changed (to be saved) */
static uint32_t ui_input(uint32_t b)
{
    uint32_t changed = 0, s, k, shift = fm1_in.buttons >> BTN_SHIFT & 1u;
    int32_t e;
    if (shift != ui.shift) { ui.shift = shift; ui.shift_sel = 0; ui.hero = 1; }
    if (ui.save) {                              /* SAVE asking: SAVE writes, any other button: no */
        static const uint8_t OTHER[3] = {ENC_SELECT, ENC_ALGORITHM, ENC_KNOB1 + 3};
        if (b >> BTN_SAVE & 1u) {
            patch_save_ask(ui.save_to, ui.sname);   /* (the notes let go: it writes once silent) */
            ui.save = 0;
            ui_msg("SAVING", 0, 0);
        } else if (b & ~(1u << BTN_SHIFT)) {
            ui.save = 0;
            ui_msg("NOT SAVED", 0, 0);
        }
        if ((e = fm1_enc_take(ENC_PRESETS) + fm1_enc_take(ENC_KNOB1)) != 0)   /* the number: KNOB1 or PRESETS */
            ui.save_to = (uint32_t)clampi((int32_t)ui.save_to + e, 0, NPATCH - 1);
        if ((e = fm1_enc_take(ENC_KNOB1 + 1)) != 0) ui.cur = (uint32_t)clampi((int32_t)ui.cur + e, 0, PATCH_NAME - 1);
        if ((e = fm1_enc_take(ENC_KNOB1 + 2)) != 0) {   /* the letter under the cursor: through NAME_CH */
            int32_t n = (int32_t)sizeof NAME_CH - 1, c = 0;
            while (c < n && NAME_CH[c] != ui.sname[ui.cur]) c++;
            c = ((c + e) % n + n) % n;
            ui.sname[ui.cur] = NAME_CH[c];
        }
        for (k = 0; k < 3u; k++) fm1_enc_take(OTHER[k]);   /* (the rest do nothing meanwhile) */
        ui.cells = ui.hero = ui.foot = 1;
        return 0;
    }
    if (ui.peek_on && !(fm1_in.buttons >> SEC_BTN[ui.peek_s] & 1u)) {   /* a section's key let go: held long (a
                                                   peek), back where it was */
        ui.peek_on = 0;
        if (fm1_ms - ui.peek_t >= PEEK_MS && set.sec == ui.peek_s) { ui_go(ui.peek_prev); changed = 1; }
    }
    if (ui.ma_on && !(fm1_in.buttons >> SEC_BTN[SEC_MOD] & 1u)) {   /* MOD let go: no slot made, its page (as a
                                                   tap) */
        ui.ma_on = 0;
        ui.hero = 1;
        if (!ui.ma_used) { ui_go(SEC_MOD); changed = 1; }
    }
    for (s = 0; s < NSEC; s++) {
        if (SEC_BTN[s] == 0xFFu || !(b >> SEC_BTN[s] & 1u)) continue;
        if (!shift && s == SEC_MOD && set.sec != SEC_MOD && (fm1_in.buttons >> SEC_BTN[s] & 1u)) {   /* MOD held on
                                                   another page: the knobs make slots; its page when let go */
            ui.ma_on = 1; ui.ma_used = 0; ui.ma_slot = -1;
            ui.hero = 1;
            continue;
        }
        if (shift && s == SEC_PATCH && set.sec == SEC_SETUP) {   /* SETUP's rows: SHIFT + PATCH again */
            set.setup_row = (uint8_t)((set.setup_row + 1u) % 3u);
            ui.focus = -1;
            ui.cells = ui.hero = ui.foot = 1;
        } else if (shift && s == SEC_PATCH) ui_go(SEC_SETUP);
        else if (shift) {                       /* INIT; again, nothing changed since: undone */
            if (undo_ready() == undo_key(s)) { undo_back(); ui_msg("UNDO", 0, 0); ui.cells = ui.hero = 1; changed = 1; }
            else {
                undo_take(s);
                if (section_init(s)) {
                    undo.sum = undo_sum(); undo.ok = 1;
                    ui_msg("INIT", 0, 0); ui.cells = ui.hero = 1; changed = 1;
                }
            }
        } else if (s == set.sec && sec_rows(s) > 1u) {   /* the same button again: the other row */
            uint8_t *r = sec_row(s);
            *r = (uint8_t)((*r + 1u) % sec_rows(s));
            ui.focus = -1;
            ui.cells = ui.hero = ui.foot = 1;
        } else {
            if (fm1_in.buttons >> SEC_BTN[s] & 1u) {   /* (still held: let go after PEEK_MS, back) */
                ui.peek_on = 1; ui.peek_s = (uint8_t)s; ui.peek_prev = set.sec; ui.peek_t = fm1_ms;
            }
            ui_go(s);
        }
        changed = 1;                            /* (the section and the row are kept too) */
    }
    if (b >> BTN_SAVE & 1u) {
        if (shift) {                            /* RECALL: the edits left behind, or the patch as stored */
            patch_recall();
            ui_msg("RECALL", 0, 0);
            changed = 1;
        } else {                                /* SAVE: ask where and under what name */
            ui.save = 1;
            ui.save_to = set.prog;
            ui.cur = 0;
            memcpy(ui.sname, set.name, PATCH_NAME);
        }
        ui.cells = ui.hero = ui.foot = 1;
    }
    if (b >> BTN_RUN & 1u) {                    /* RUN: the arp on / off; SHIFT + RUN: HOLD on / off */
        uint32_t m = shift ? (set.p.arp.mode == ARP_HOLD ? ARP_ON : ARP_HOLD)
                   : set.p.arp.mode != ARP_OFF ? ARP_OFF : set.p.arp.last;
        arp_mode_set(m);
        ui_msg(m == ARP_OFF ? "ARP OFF" : m == ARP_ON ? "ARP ON" : m == ARP_HOLD ? "ARP HOLD" : "ARP 1SHOT", 0, 0);
        if (set.sec == SEC_ARP) ui.cells = ui.hero = 1;
        changed = 1;
    }
    if (ui_foot_flag) { ui_foot_flag = 0; ui.foot = 1; }
    if (b & (1u << BTN_OCT_DOWN | 1u << BTN_OCT_UP)) {   /* the octave; SHIFT: the transpose */
        int32_t d = (b >> BTN_OCT_UP & 1u) ? 1 : -1;
        if (shift) { set.trans = (int8_t)clampi(set.trans + d, -12, 12); ui_msg("TRANS", set.trans, 1); }
        else set.oct = (uint8_t)clampi((int8_t)set.oct + d, -2, 2);
        if (set.sec == SEC_SETUP) ui.cells = 1;
        ui.foot = 1;
        changed = 1;
    }
    if ((e = fm1_enc_take(ENC_SELECT)) != 0 && ui.ma_on) {   /* MOD held: the slot's source */
        if (ui.ma_slot >= 0) {
            mod_slot_t *m = pslot(&set.p, (uint32_t)ui.ma_slot);
            m->src = (uint8_t)clampi((int32_t)m->src + e, MS_LFO1, MS_N - 1);
            sound_apply();
            ui.hero = 1;
            changed = 1;
        }
    } else if (e != 0 && shift) {               /* SHIFT + SELECT: the section's rows (2026-11-16) */
        if (sec_rows(set.sec) > 1u) {
            uint8_t *r = sec_row(set.sec);
            uint8_t n = (uint8_t)clampi((int32_t)*r + e, 0, (int32_t)sec_rows(set.sec) - 1);
            if (n != *r) { *r = n; ui.focus = -1; changed = 1; }
            ui.shift_sel = 1;                   /* (the page shows, not the help) */
            ui.cells = ui.hero = ui.foot = 1;
        }
    } else if (e != 0 && sec_insts(set.sec)) {  /* which one in the section */
        uint8_t *in = sec_inst(set.sec);
        *in = (uint8_t)clampi((int32_t)*in + e, 0, (int32_t)sec_insts(set.sec) - 1);
        ui.cells = ui.hero = ui.foot = 1;
        changed = 1;
    }
    if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {   /* MACRO: MORPH */
        set.morph = (int8_t)clampi(set.morph + e, -63, 63);
        sound_apply();
        ui_msg("MORPH", set.morph, 1);
        if (set.sec == SEC_PATCH) ui.cells = 1;
        changed = 1;
    }
    if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {   /* PRESETS: another patch, now */
        patch_load((uint32_t)clampi((int32_t)set.prog + e, 0, NPATCH - 1));
        ui.cells = ui.hero = ui.foot = 1;
        changed = 1;
    }
    for (k = 0; k < 4u; k++) {                  /* KNOB1..4: the cells */
        uint32_t id;
        int32_t v;
        if ((e = fm1_enc_take(ENC_KNOB1 + k)) == 0) continue;
        if (ui.ma_on) { changed |= mod_assign(k, e); continue; }   /* (MOD held: a slot onto it) */
        id = page_param(set.sec, ui_row(), k);
        if (!id) continue;
        v = clampi(pget(id) + e * (shift ? 1 : PARAM[id].step), PARAM[id].lo, param_hi(id));
        if (v != pget(id)) { pset(id, v); sound_apply(); changed = 1; }
        ui.focus = (int32_t)k;
        ui.focus_t = fm1_ms;
        ui.cells = ui.hero = 1;
        if (id == P_TRANS || id == P_VMODE || id == P_ATEMPO) ui.foot = 1;   /* (the footer shows
                                                   them) */
    }
    return changed;
}
static void ui_leds(void)
{
    uint32_t s;
    for (s = 0; s < NSEC; s++)
        if (SEC_BTN[s] != 0xFFu)
            fm1_led_key(SEC_BTN[s], s == set.sec || (s == SEC_PATCH && set.sec == SEC_SETUP) || (s == SEC_MOD && ui.ma_on));
    fm1_led_key(BTN_SHIFT, (int)ui.shift);
    fm1_led_key(BTN_OCT_DOWN, (int8_t)set.oct < 0);
    fm1_led_key(BTN_OCT_UP, (int8_t)set.oct > 0);
    if (set.p.arp.mode != ARP_OFF)              /* the arp on: lit, dark a moment on each step */
        fm1_led_key(BTN_RUN, !(arp.run && fm1_ms - arp_step_ms < 40u));
    else                                        /* off: it blinks with the MIDI clock */
        fm1_led_key(BTN_RUN, clk_run || (clk_bpm && fm1_ms - clk_beat_ms < 80u));
}

/* ---- every pass: what changed, drawn again */
static void ui_draw(void)
{
    uint32_t now = fm1_ms, v;
    if (ui.focus >= 0 && now - ui.focus_t > 2000u) { ui.focus = -1; ui.cells = 1; ui.hero = 1; }   /* (the picture too: a choice list back to the
                                                   section's picture, a fader dimmed) */
    if (ui.msg[0] && now - ui.msg_t > 1200u) { ui.msg[0] = 0; ui.foot = 1; }
    if (now - ui.t_cpu >= 500u) {               /* the audio's load, every half second */
        uint32_t t = cpu_ticks, pv = cpu_part[0], pf = cpu_part[1], sum = cpu_sum, cnt = cpu_cnt, nv = cpu_nv;
        cpu_reset++;
#ifndef DINGHY_HOST
        poly_learn(t, pv, nv);                  /* (the voices' limit: is the sound heavy? engine/poly.h) */
#else
        (void)nv;
#endif
        ui.t_cpu = now;
#ifndef DINGHY_HOST
        ui.cpu = t / 1393u;                     /* % of 256 frames at 44.1 kHz: 5805 us x 24 ticks = 139320 */
        ui.cpu_vox = pv / 1393u;
        ui.cpu_fx = pf / 1393u;
        ui.cpu_out = t >= pv + pf ? (t - pv - pf) / 1393u : 0u;
        ui.cpu_avg = cnt ? sum / cnt / 1393u : 0u;
#else
        (void)t; (void)pv; (void)pf; (void)sum; (void)cnt;
        ui.cpu = 1000u;                         /* (the host has no tick counter: no figure) */
#endif
        ui.cpu_hist[ui.cpu_n++ % 48u] = (uint8_t)(ui.cpu > 100u ? 0u : ui.cpu);
        if (set.sec == SEC_SETUP) ui.hero = 1;
    }
    if (now - ui.t_anim >= 40u) {               /* what moves: the voices, the table, at most 25 times a second */
        uint32_t q = 0, env = 0, cut = 0, pos;
        ui.t_anim = now;
        for (v = 0; v < NV; v++) {
            const env_t *ev = voice_env(v, set.env_sel);
            q |= voice_q(v) << (v * 4u);
            env += (voice[v].busy ? ((uint32_t)ev->lvl >> 22) + ev->st * 300u : 0u) << (v * 8u);
            cut = cut * 977u + (voice_q(v) ? (uint32_t)voice[v].fn[set.filt_sel] / 48u + 1u : 0u);   /* (1/2 px) */
        }
        if (q != ui.voices_drawn) { ui.voices_drawn = q; ui.foot = 1; }
        {                                       /* the cells' live marks and their bands (a window of 1.5 s) */
            int32_t b, lv;
            uint32_t key = 0;
            if (now - ui.band_t > 1500u) { ui.band_t = now; ui.band_lo[0] = ui.band_lo[1] = 1000; ui.band_hi[0] = ui.band_hi[1] = -1000; }
            for (b = 0; b < 2; b++)
                if (cell_live(b, &lv)) {
                    if (lv < ui.band_lo[b]) ui.band_lo[b] = lv;
                    if (lv > ui.band_hi[b]) ui.band_hi[b] = lv;
                    key = key * 977u + (uint32_t)lv * 7u + (uint32_t)ui.band_lo[b] * 131u + (uint32_t)ui.band_hi[b] + 1u;
                }
            if (key != ui.live_drawn && ((set.sec == SEC_OSC && set.osc_row == 0u) || (set.sec == SEC_FILT && set.filt_row == 0u)))
                ui.cells = 1;
            ui.live_drawn = key;
        }
        {                                       /* the footer: MIDI coming in, the MIDI clock's tempo */
            uint32_t f = (midi_act_ms && now - midi_act_ms < 150u) + clk_bpm * 2u;
            if (f != ui.foot_drawn) { ui.foot_drawn = f; ui.foot = 1; }
        }
        if (env != ui.env_drawn) { ui.env_drawn = env; if (set.sec == SEC_ENV) ui.hero = 1; }
        if (cut != ui.cut_drawn) { ui.cut_drawn = cut; if (set.sec == SEC_FILT) ui.hero = 1; }
        {                                       /* (the patch, edited or not: the footer, PATCH's list) */
            uint32_t pd = set.prog * 2u + patch_edited();
            if (pd != ui.patch_drawn) { ui.patch_drawn = pd; ui.foot = 1; if (set.sec == SEC_PATCH) ui.hero = 1; }
        }
        {
            uint32_t a = arp.k * 7u + arp.s * 13u + arp.n * 131u + arp.sounding * 3u + (uint32_t)arp_ext() * 5u
                       + clk_bpm * 977u;
            if (a != ui.arp_drawn) { ui.arp_drawn = a; if (set.sec == SEC_ARP) ui.hero = 1; }
            if (set.sec == SEC_MOD && set.mod_row < 2u) ui.hero = 1;   /* (the LFO's marker moves: 25 a second) */
            if (set.sec == SEC_FX && set.fx_sel < FX_EQ && ui.focus < 0
                && (set.p.fx[set.fx_sel].type == FX_CHORUS || set.p.fx[set.fx_sel].type == FX_PHASER
                    || set.p.fx[set.fx_sel].type == FX_ENSEMBLE || set.p.fx[set.fx_sel].type == FX_FILTER)) ui.hero = 1;   /* (and
                                                   the chorus's) */
        }
        pos = (osc_pos_shown(0) >> 12) + (osc_pos_shown(1) >> 12) * 977u + (osc_pos_shown(2) >> 12) * 1931u;   /* (the
                                                   matrix's WAVE moves it too: the picture follows, 25 a second) */
        if (pos != ui.pos_drawn) { ui.pos_drawn = pos; if (set.sec == SEC_OSC) ui.hero = 1; }
    }
    if (ui.cells) { ui.cells = 0; draw_cells(); }
    if (ui.hero) { ui.hero = 0; draw_hero(); }
    if (ui.foot) { ui.foot = 0; draw_footer(); }
}
