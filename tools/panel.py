#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna's panel (stage 9): what each control does, in one place, and the printable overlay made from it.

The roles below are the firmware's (firmware/src/ui.h: SEC_BTN, SEC_COL, the SHIFT functions); tests/panel_check.py
holds the two together. The geometry is in millimetres on the FM-1's top face, from its top left corner (x to the
right, y down towards the keys). It comes from a CAD model of a skin for the FM-1 (Fusion, mesh "skin_fm1", read
2026-11-14): its cavity is 96.75 x 161.76 mm (the FM-1's 96.5 x 161.5 and 0.13 mm of play a side), its face 0.6 mm
with the cut-outs below. Not yet laid on a real FM-1: FIT_TESTED = False until the fit test (docs/panel/MEASURE.md)
has been done on one; the overlay says so in small print until then. The paper is cut to the frame's flat face
(SHEET), not to the FM-1's whole face, the cut-outs' corners as the skin's: the lines the stencil cuts along.

    python3 tools/panel.py           docs/panel/volna-overlay.svg (print and cut), volna-fit-test.svg (holes and
                                     a 100 mm ruler on plain paper: lay it on the FM-1, see what is off),
                                     volna-overlay-a4-bw.svg (the overlay on an A4 sheet for a black and
                                     white printer: black words on white, the cutting lines thin black),
                                     overlay.png (a preview), volna-frame-windows.json (the windows to cut in
                                     the frame's face: the skin printed in plastic, laid over the paper overlay,
                                     its words read through the windows; the Fusion model is cut from this file)
    python3 tools/panel.py --check   exit 1 if the files are not what this script makes, cut-outs are too close,
                                     a word runs into a cut-out, or the frame's windows leave too thin a bar, run
                                     off its flat face or do not show a word
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
OUT = HERE / "docs" / "panel"
FIT_TESTED = False                              # True once the fit test has been laid on a real FM-1

# ---- the roles (ui.h): the section colours, as SEC_COL
COL = {"OSC": (180, 140, 255), "MIX": (80, 200, 190), "FILT": (255, 150, 60), "ENV": (120, 220, 110),
       "MOD": (240, 210, 80), "FX": (90, 160, 255), "PATCH": (220, 220, 220), "ARP": (255, 110, 170),
       "SEQ": (70, 210, 240)}
GREY, DIM, BG = (205, 205, 205), (120, 120, 128), (30, 30, 34)

# buttons: (the stock label, Volna's role, with SHIFT, the colour's section), top row then bottom row, left to right
BUTTONS = [("FX", "OSC", "INIT", "OSC"), ("SCL", "MIX", "INIT", "MIX"), ("ENV", "FILT", "INIT", "FILT"),
           ("LFO", "ENV", "INIT", "ENV"), ("EDIT", "MOD", "INIT", "MOD"), ("GLO", "FX", "OFF", "FX"),
           ("HOME", "PATCH", "SETUP", "PATCH"), ("SAVE", "SAVE", "RECALL", "PATCH"), ("ARP", "ARP", "INIT", "ARP"),
           ("SEQ", "SEQ", "INIT", "SEQ"), ("PLAY", "RUN", "HOLD", "ARP"), ("REC", "SHIFT", "", None)]
# knobs: (the stock label, Volna's role, with SHIFT or a note)
KNOBS = [("MASTER", "VOLUME", ""), ("SELECT", "SELECT", "WHICH ONE"), ("PRESETS", "PATCH", "BROWSE"),
         ("ALGORITHM", "MACRO", "MORPH"), ("KNOB1", "1", "FINE"), ("KNOB2", "2", "FINE"), ("KNOB3", "3", "FINE"),
         ("KNOB4", "4", "FINE")]
OCT = [("OCT-", "OCT -", "TRANS -"), ("OCT+", "OCT +", "TRANS +")]

# ---- the geometry, mm, from the skin model: the face's corner at the skin's cavity's centre - (48.25, 80.75)
W, H = 161.5, 96.5
SHEET_H = H                                     # (the SVG's height: the whole face)
# the paper's outline: the frame's flat face (1.986 in from the FM-1's edge: the wall's R2 beyond, its corners
# R7.764), 0.5 mm inside. The stencil in the Fusion model (the body "stencil") has this outline.
SHEET = (2.486, 2.486, W - 2.486, H - 2.486)
SHEET_R = 7.264
KNOB_POS = {"MASTER": (13.75, 15.75), "SELECT": (30.75, 15.75), "PRESETS": (13.75, 32.75),
            "ALGORITHM": (30.75, 32.75), "KNOB1": (90.75, 15.75), "KNOB2": (108.75, 15.75),
            "KNOB3": (126.75, 15.75), "KNOB4": (144.75, 15.75)}
KNOB_HOLE = {k: 7.0 for k in KNOB_POS}          # diameters (the skin's)
OCT_BOX = (10.05, 41.75, 34.65, 49.15)          # x0 y0 x1 y1: both OCT buttons in one cut-out (the skin's)
OCT_X = [16.2, 28.5]                            # each button's middle: the box's halves [a guess]
BTN_BOX = (86.25, 27.15, 149.55, 49.05)         # the 12 buttons in one cut-out (the skin's)
BTN_X = [BTN_BOX[0] + (BTN_BOX[2] - BTN_BOX[0]) * (i + 0.5) / 6 for i in range(6)]   # [the box split evenly: a guess]
BTN_Y = [BTN_BOX[1] + (BTN_BOX[3] - BTN_BOX[1]) * (j + 0.5) / 2 for j in range(2)]
SCREEN = (42.75, 8.45, 78.95, 44.65)            # the screen's window (the skin's)
KEYS = (6.85, 55.35, 154.65, 90.15)             # the keys' window (the skin's)
# the cut-outs' corners (the skin's): one radius, or top left, top right, bottom right, bottom left
BOX_R = {"OCT": 1.0, "BUTTONS": 3.0, "SCREEN": 4.1, "KEYS": (5.0, 5.0, 5.5, 5.5)}
WHITE_X = [KEYS[0] + (KEYS[2] - KEYS[0]) * (i + 0.5) / 16 for i in range(16)]   # STEPS 1..16 [even: a guess]

# ---- the frame: the skin (Fusion, the body "frame" rebuilt from the mesh) over the paper overlay, windows in its
# face (1.0 mm: the skin's 0.6 and 0.4 outwards) to read the words through. Not printed yet; the words' sizes are
# word_w()'s estimate, not the font's.
FRAME_M = 0.5                                   # a word's margin in its window
FRAME_M_KEYS = 0.3                              # ... above the STEPS numbers (a wider bar to the keys' window)
FRAME_BAR = 1.1                                 # the thinnest bar between two openings that do not join
FRAME_FLAT = 2.0                                # the face is flat from here in from the FM-1's edge (R2 in the wall)
FRAME_SKIP = ("NOT YET FIT ON AN FM-1",)        # words with no window (this one goes once the fit test is done)
FRAME_INTO = {"BUTTONS": 3.5, "OCT": 1.5}       # how far a strip of words runs into its cut-out (past its corners)
MODEL_X0, MODEL_Y0 = 39.25, 170.75              # the Fusion model, mm: x = panel y + MODEL_X0, y = MODEL_Y0 - panel x


# DejaVu Sans, the advance of each character in em (from the font's own tables, 2026-11-14): regular, then bold
ADV = {False: {" ": 0.318, "+": 0.838, "-": 0.361, ":": 0.337, "A": 0.684, "B": 0.686, "C": 0.698, "D": 0.77,
               "E": 0.632, "F": 0.575, "G": 0.775, "H": 0.752, "I": 0.295, "J": 0.295, "K": 0.656, "L": 0.557,
               "M": 0.863, "N": 0.748, "O": 0.787, "P": 0.603, "Q": 0.787, "R": 0.695, "S": 0.635, "T": 0.611,
               "U": 0.732, "V": 0.684, "W": 0.989, "X": 0.685, "Y": 0.611, "Z": 0.685},
       True: {" ": 0.348, "+": 0.838, "-": 0.415, ":": 0.4, "A": 0.774, "B": 0.762, "C": 0.734, "D": 0.83,
              "E": 0.683, "F": 0.683, "G": 0.821, "H": 0.837, "I": 0.372, "J": 0.372, "K": 0.775, "L": 0.637,
              "M": 0.995, "N": 0.837, "O": 0.85, "P": 0.733, "Q": 0.85, "R": 0.77, "S": 0.72, "T": 0.682,
              "U": 0.812, "V": 0.774, "W": 1.103, "X": 0.771, "Y": 0.724, "Z": 0.725}}
for _b, _d in ((False, 0.636), (True, 0.696)):  # (the digits are all one width)
    ADV[_b].update({str(_i): _d for _i in range(10)})


def word_w(t, sz, bold):
    """a word's width: the sum of its characters' advances in DejaVu Sans (the ink is a little narrower: the
    sketch text in the Fusion model measured 10.28 mm for VOLUME, this gives 10.45). A character not in ADV
    (a lower-case letter, say) is an error: add its advance."""
    return sum(ADV[bool(bold)][c] for c in t) * sz


def box_hole(name, b):
    x0, y0, x1, y1 = b
    return (name, "r", (x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0)


def holes():
    """every cut-out: (name, kind, cx, cy, w, h) — kind 'c' a circle (w = h = diameter), 'r' a rounded rectangle"""
    hs = [(k, "c", x, y, KNOB_HOLE[k], KNOB_HOLE[k]) for k, (x, y) in KNOB_POS.items()]
    return hs + [box_hole("OCT", OCT_BOX), box_hole("BUTTONS", BTN_BOX), box_hole("SCREEN", SCREEN),
                 box_hole("KEYS", KEYS)]


def overlaps():
    """pairs of cut-outs closer than 1 mm (their boxes), any too near the sheet's edge, words over a cut-out"""
    bad, hs = [], holes()
    for i, a in enumerate(hs):
        if a[2] - a[4] / 2 < SHEET[0] + 1 or a[2] + a[4] / 2 > SHEET[2] - 1 or a[3] - a[5] / 2 < SHEET[1] + 1 \
                or a[3] + a[5] / 2 > SHEET[3] - 1:
            bad.append((a[0], "the edge"))
        for b in hs[i + 1:]:
            if abs(a[2] - b[2]) < (a[4] + b[4]) / 2 + 1 and abs(a[3] - b[3]) < (a[5] + b[5]) / 2 + 1:
                bad.append((a[0], b[0]))
    for t, x, y, sz, _, b, a in labels():       # a word's box against each cut-out's
        w = word_w(t, sz, b)
        x0 = x - w / 2 if a == "middle" else x
        for h in hs:
            if x0 < h[2] + h[4] / 2 + 0.3 and x0 + w > h[2] - h[4] / 2 - 0.3 and y - sz * 0.75 < h[3] + h[5] / 2 + 0.3 \
                    and y > h[3] - h[5] / 2 - 0.3:
                bad.append((repr(t), h[0]))
        if x0 < SHEET[0] + 0.8 or x0 + w > SHEET[2] - 0.8 or y - sz * 0.75 < SHEET[1] + 0.3 or y > SHEET[3] - 0.3:
            bad.append((repr(t), "the edge"))
    return bad


def labels():
    """every printed word: (text, x, y (baseline), size mm, colour, bold, anchor)"""
    L = []
    for name, role, sh in KNOBS:
        x, y = KNOB_POS[name]
        r = KNOB_HOLE[name] / 2
        col = GREY                              # (the cells take the section's colour on the screen)
        L.append((role, x, y - r - 1.0, 2.2, col, True, "middle"))
        if sh:
            L.append((sh, x, y + r + 2.0, 1.4, DIM, False, "middle"))
    for i, (_, role, sh, sec) in enumerate(BUTTONS):
        x, y = BTN_X[i % 6], BTN_Y[i // 6]
        col = COL[sec] if sec else GREY
        if i < 6:                               # top row: the role above the cut-out, SHIFT's above it
            L.append((role, x, BTN_BOX[1] - 0.8, 1.7, col, True, "middle"))
            if sh:
                L.append((sh, x, BTN_BOX[1] - 2.8, 1.2, DIM, False, "middle"))
        else:                                   # bottom row: the role below, SHIFT's under it
            L.append((role, x, BTN_BOX[3] + 2.2, 1.7, col, True, "middle"))
            if sh:
                L.append((sh, x, BTN_BOX[3] + 4.0, 1.2, DIM, False, "middle"))
    for (_, role, sh), x in zip(OCT, OCT_X):
        L.append((role, x, OCT_BOX[1] - 0.8, 1.7, GREY, True, "middle"))
        L.append((sh, x, OCT_BOX[3] + 2.0, 1.2, DIM, False, "middle"))
    L.append(("POWER-ON + OCT -: USB RESCUE", 22.35, 53.6, 1.15, DIM, False, "middle"))
    L.append(("VOLNA", (SCREEN[0] + SCREEN[2]) / 2, 6.3, 4.0, COL["OSC"], True, "middle"))
    L.append(("SCREEN CELLS", (KNOB_POS["KNOB1"][0] + KNOB_POS["KNOB4"][0]) / 2, 5.6, 1.3, DIM, False, "middle"))
    L.append(("SHIFT HELD: HELP ON SCREEN", 22.0, 4.2, 1.2, DIM, False, "middle"))
    if not FIT_TESTED:
        L.append(("NOT YET FIT ON AN FM-1", 143.0, 4.2, 1.0, (255, 90, 90), False, "middle"))
    for i, x in enumerate(WHITE_X):             # SEQ's steps: under the white keys
        L.append((str(i + 1), x, KEYS[3] + 2.6, 1.5, COL["SEQ"], False, "middle"))
    return L


def gap(a, b):
    """the distance between two boxes (x0, y0, x1, y1); 0 if they touch or overlap"""
    dx = max(a[0] - b[2], b[0] - a[2], 0)
    dy = max(a[1] - b[3], b[1] - a[3], 0)
    return (dx * dx + dy * dy) ** 0.5


def hole_boxes():
    return [(n, k, (x - w / 2, y - h / 2, x + w / 2, y + h / 2)) for n, k, x, y, w, h in holes()]


def words():
    """the words the frame shows: (text, its box x0 y0 x1 y1), the baseline its bottom"""
    out = []
    for t, x, y, sz, _, b, a in labels():
        if t not in FRAME_SKIP:
            x0 = x - word_w(t, sz, b) / 2 if a == "middle" else x
            out.append((t, (x0, y - 0.75 * sz, x0 + word_w(t, sz, b), y)))
    return out


def windows():
    """the frame's windows: (name, x0, y0, x1, y1), panel mm; rounded rectangles (r 0.5), cut through the face.
    A word nearer than FRAME_BAR to a cut-out has no bar to stand behind, so its window joins the cut-out:
    a knob's words run to the hole's middle; the words along a row of buttons share one strip, as wide as the
    cut-out and FRAME_INTO into it; a knob's word that ends too near a strip runs down into it."""
    hs = hole_boxes()
    wins, knob, strips = [], [], {}
    for t, wb in words():
        m_top = FRAME_M_KEYS if wb[1] > KEYS[3] else FRAME_M
        b = [wb[0] - FRAME_M, wb[1] - m_top, wb[2] + FRAME_M, wb[3] + FRAME_M]
        near = [(n, k, h) for n, k, h in hs if gap(b, h) < FRAME_BAR]
        if near and near[0][1] == "c":
            h = near[0][2]
            cy = (h[1] + h[3]) / 2
            b[3 if b[3] <= cy else 1] = cy
            knob.append([t] + b)
        elif near and near[0][0] in FRAME_INTO:
            n, _, h = near[0]
            above = b[3] <= (h[1] + h[3]) / 2
            into = FRAME_INTO[n]
            s = strips.setdefault((n, above), [h[0], b[1], h[2], h[1] + into] if above else
                                  [h[0], h[3] - into, h[2], b[3]])
            s[1], s[3] = min(s[1], b[1]), max(s[3], b[3])
        else:
            wins.append([t] + b)
    strips = [[f"{n} {'top' if above else 'bottom'}"] + s for (n, above), s in sorted(strips.items())]
    for s in strips:                            # a strip grows over the free words within its width and too near it
        for w in [w for w in wins if gap(w[1:], s[1:]) < FRAME_BAR and w[1] >= s[1] and w[3] <= s[3]]:
            s[2], s[4] = min(s[2], w[2]), max(s[4], w[4])
            wins.remove(w)
    for w in knob:
        for s in strips:
            if 0 < gap(w[1:], s[1:]) < FRAME_BAR and w[4] < s[2]:
                w[4] = s[2] + FRAME_M
    return [tuple(w) for w in knob + strips + wins]


def frame_bad():
    """what is wrong with the frame's windows: bars thinner than FRAME_BAR, windows off the flat face, words unseen"""
    bad, ws = [], windows()
    al = [(n, b) for n, _, b in hole_boxes()] + [(w[0], w[1:]) for w in ws]
    nh = len(al) - len(ws)
    for i, (an, a) in enumerate(al):
        for j in range(max(i + 1, nh), len(al)):
            g = gap(a, al[j][1])
            if 0 < g < FRAME_BAR - 1e-9:
                bad.append(f"the bar between {an} and {al[j][0]} is {g:.2f} mm (under {FRAME_BAR})")
    for n, x0, y0, x1, y1 in ws:
        if x0 < FRAME_FLAT or y0 < FRAME_FLAT or x1 > W - FRAME_FLAT or y1 > H - FRAME_FLAT:
            bad.append(f"the window {n} runs off the frame's flat face")
    for t, wb in words():
        if not any(wb[0] >= w[1] and wb[1] >= w[2] and wb[2] <= w[3] and wb[3] <= w[4] for w in ws):
            bad.append(f"no window shows {t!r} whole")
    return bad


def frame_json():
    ws = [{"name": n, "panel": [round(v, 3) for v in (x0, y0, x1, y1)],
           "model": [round(v, 3) for v in (y0 + MODEL_X0, MODEL_Y0 - x1, y1 + MODEL_X0, MODEL_Y0 - x0)]}
          for n, x0, y0, x1, y1 in windows()]
    return json.dumps({"about": "generated by tools/panel.py: do not edit. The windows to cut through the face of "
                                "the FM-1 frame (Fusion body 'frame'), rounded rectangles x0 y0 x1 y1 in mm: "
                                "'panel' as the overlay (from the face's top left corner), 'model' as the Fusion "
                                "model (x = panel y + %g, y = %g - panel x)." % (MODEL_X0, MODEL_Y0),
                       "corner_radius": 0.5, "windows": ws}, indent=1) + "\n"


def hexc(c):
    return "#%02X%02X%02X" % c


def rr_path(x0, y0, x1, y1, r):
    """a rounded rectangle as an SVG path; r: one radius, or top left, top right, bottom right, bottom left"""
    a, b, c, d = r if isinstance(r, tuple) else (r,) * 4
    return (f"M{x0 + a:.3f} {y0:.3f}H{x1 - b:.3f}A{b} {b} 0 0 1 {x1:.3f} {y0 + b:.3f}"
            f"V{y1 - c:.3f}A{c} {c} 0 0 1 {x1 - c:.3f} {y1:.3f}H{x0 + d:.3f}A{d} {d} 0 0 1 {x0:.3f} {y1 - d:.3f}"
            f"V{y0 + a:.3f}A{a} {a} 0 0 1 {x0 + a:.3f} {y0:.3f}Z")


def hole_svg(name, k, x, y, w, h, attr=""):
    if k == "c":
        return f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{w / 2:.2f}"{attr}/><!-- {name} -->'
    return f'<path d="{rr_path(x - w / 2, y - h / 2, x + w / 2, y + h / 2, BOX_R[name])}"{attr}/><!-- {name} -->'


def cut_svg():
    """the cutting lines: the sheet's outline, then every cut-out (as the stencil's)"""
    return [f'<path d="{rr_path(*SHEET, SHEET_R)}"/><!-- the sheet -->'] + [hole_svg(*h) for h in holes()]


def svg_overlay():
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{SHEET_H}mm" viewBox="0 0 {W} {SHEET_H}">',
         "<!-- generated by tools/panel.py: do not edit. Layer 'print': print it; layer 'cut': the cutting lines "
         "(0.1 mm red, the cut-outs and the outline). -->",
         '<g id="print">',
         f'<rect x="-1" y="-1" width="{W + 2}" height="{SHEET_H + 2}" fill="{hexc(BG)}"/>']
    for t, x, y, sz, c, b, a in labels():
        s.append(f'<text x="{x:.2f}" y="{y:.2f}" font-family="DejaVu Sans, Arial, sans-serif" font-size="{sz}" '
                 f'fill="{hexc(c)}" text-anchor="{a}"{" font-weight=\"bold\"" if b else ""}>{t}</text>')
    s.append('</g>\n<g id="cut" fill="none" stroke="#FF0000" stroke-width="0.1">')
    s += cut_svg()
    s.append("</g>\n</svg>\n")
    return "\n".join(s)


def svg_print_bw():
    """the overlay for a black and white printer: an A4 sheet (portrait), at 100 %: black words on white, the
    outline and the cut-outs as thin black cutting lines, a 100 mm ruler to check the scale"""
    aw, ah = 210, 297
    ox, oy = (aw - W) / 2, 20
    font = 'font-family="DejaVu Sans, Arial, sans-serif"'
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{aw}mm" height="{ah}mm" viewBox="0 0 {aw} {ah}">',
         "<!-- generated by tools/panel.py: do not edit. A4, print at 100 % (no fit to page), check the ruler. -->",
         f'<rect x="0" y="0" width="{aw}" height="{ah}" fill="#FFFFFF"/>',
         f'<g transform="translate({ox} {oy})">', '<g id="print" fill="#000000">']
    for t, x, y, sz, c, b, a in labels():
        s.append(f'<text x="{x:.2f}" y="{y:.2f}" {font} font-size="{sz}" text-anchor="{a}"'
                 f'{" font-weight=\"bold\"" if b else ""}>{t}</text>')
    s.append('</g>\n<g id="cut" fill="none" stroke="#000000" stroke-width="0.1">')
    s += cut_svg()
    s.append("</g>\n</g>")
    y = oy + SHEET_H + 14
    s.append(f'<path d="M{ox} {y}h100" stroke="#000" stroke-width="0.2"/>')
    for i in range(11):
        s.append(f'<path d="M{ox + 10 * i} {y - 2}v{4 if i % 5 == 0 else 2}" stroke="#000" stroke-width="0.2"/>')
    s.append(f'<text x="{ox}" y="{y - 3.5}" {font} font-size="2">100 mm: if this is not 100 mm, print at 100 % '
             '(no fit to page)</text>')
    s.append("</svg>\n")
    return "\n".join(s)


def svg_fit():
    """the fit test: on plain paper, at 100 %: the outline, the cut-outs, their centres, their names, a 100 mm ruler"""
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H + 22}mm" viewBox="0 0 {W} {H + 22}">',
         "<!-- generated by tools/panel.py: do not edit. Print at 100 % (no fit to page), check the ruler, lay it on "
         "the FM-1. -->",
         f'<rect x="0" y="0" width="{W}" height="{SHEET_H}" rx="4" fill="none" stroke="#000" stroke-width="0.15"/>',
         f'<path d="{rr_path(*SHEET, SHEET_R)}" fill="none" stroke="#000" stroke-width="0.1" '
         'stroke-dasharray="1 1"/><!-- the sheet of the overlay (the face of the FM-1 is the solid line) -->']
    for name, k, x, y, w, h in holes():
        s.append(hole_svg(name, k, x, y, w, h, ' fill="none" stroke="#000" stroke-width="0.15"'))
        s.append(f'<path d="M{x - 1:.2f} {y:.2f}h2M{x:.2f} {y - 1:.2f}v2" stroke="#C00" stroke-width="0.1"/>')
        s.append(f'<text x="{x:.2f}" y="{y + 0.5:.2f}" font-family="DejaVu Sans, Arial" font-size="1.1" '
                 f'text-anchor="middle" fill="#555">{name}</text>')
    for i, x in enumerate(WHITE_X):
        s.append(f'<path d="M{x:.2f} {KEYS[3]}v2" stroke="#000" stroke-width="0.15"/>'
                 f'<text x="{x:.2f}" y="{KEYS[3] + 4:.2f}" font-size="1.6" text-anchor="middle">{i + 1}</text>')
    y = H + 12
    s.append(f'<path d="M10 {y}h100" stroke="#000" stroke-width="0.2"/>')
    for i in range(11):
        s.append(f'<path d="M{10 + 10 * i} {y - 2}v{4 if i % 5 == 0 else 2}" stroke="#000" stroke-width="0.2"/>')
    s.append(f'<text x="10" y="{y - 3.5}" font-family="DejaVu Sans, Arial" font-size="2">100 mm: if this is not '
             '100 mm, print at 100 % (no fit to page)</text>')
    s.append(f'<text x="10" y="{H + 19:.2f}" font-family="DejaVu Sans, Arial" font-size="1.6">Volna overlay fit test '
             f'({"fit-tested" if FIT_TESTED else "geometry from the skin model, not yet fit on an FM-1"}): the numbers '
             'under the keys = SEQ steps 1..16</text>')
    s.append("</svg>\n")
    return "\n".join(s)


def preview(path):                              # (8 px a mm, the same shapes and words as the SVG)
    from PIL import Image, ImageDraw, ImageFont
    k = 8
    im = Image.new("RGB", (int(W * k), int(SHEET_H * k)), BG)
    d = ImageDraw.Draw(im)
    fonts = {}

    def font(sz, bold):
        key = (round(sz * k), bold)
        if key not in fonts:
            try:
                fonts[key] = ImageFont.truetype("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf", key[0] + 2)
            except OSError:
                fonts[key] = ImageFont.load_default()
        return fonts[key]
    for name, kind, x, y, w, h in holes():
        box = [(x - w / 2) * k, (y - h / 2) * k, (x + w / 2) * k, (y + h / 2) * k]
        if kind == "c":
            d.ellipse(box, fill=(0, 0, 0), outline=(255, 0, 0))
        else:
            r = BOX_R[name]                     # (one radius a box here: the keys' larger corners as the smaller)
            d.rounded_rectangle(box, radius=(min(r) if isinstance(r, tuple) else r) * k, fill=(0, 0, 0),
                                outline=(255, 0, 0))
    for t, x, y, sz, c, b, a in labels():
        d.text((x * k, y * k), t, fill=c, font=font(sz, b), anchor="ms" if a == "middle" else "ls")
    d.rounded_rectangle([v * k for v in SHEET], radius=SHEET_R * k, outline=(255, 0, 0))
    im.save(path)


def main():
    files = {OUT / "volna-overlay.svg": svg_overlay(), OUT / "volna-fit-test.svg": svg_fit(),
             OUT / "volna-overlay-a4-bw.svg": svg_print_bw(),
             OUT / "volna-frame-windows.json": frame_json()}
    bad, fbad = overlaps(), frame_bad()
    if "--check" in sys.argv:
        ok = not bad and not fbad and all(p.exists() and p.read_text() == t for p, t in files.items())
        for a, b in bad:
            print(f"panel: {a} and {b} closer than 1 mm")
        for m in fbad:
            print(f"panel: the frame: {m}")
        print("panel:", "up to date" if ok else "OUT OF DATE or overlapping (run tools/panel.py)")
        sys.exit(0 if ok else 1)
    if bad:
        raise SystemExit("panel: cut-outs too close: " + ", ".join(f"{a}/{b}" for a, b in bad))
    if fbad:
        raise SystemExit("panel: the frame: " + "; ".join(fbad))
    OUT.mkdir(parents=True, exist_ok=True)
    for p, t in files.items():
        p.write_text(t, newline="\n")
        print("->", p)
    preview(OUT / "overlay.png")
    print("->", OUT / "overlay.png")


if __name__ == "__main__":
    main()
