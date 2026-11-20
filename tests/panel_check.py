#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna stage 9: the overlay (tools/panel.py) and the firmware (firmware/src/ui.h) say the same:
  the section buttons: each section on the stock button ui.h gives it (SEC_BTN), in the colour it draws (SEC_COL);
  the overlay's files are what tools/panel.py makes, no cut-outs closer than 1 mm;
  the frame's windows (volna-frame-windows.json) show every word, leave no bar under 1.1 mm, stay on its flat face.
Exit 1 if not."""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / "tools"))
import panel  # noqa: E402

ui = (HERE / "firmware" / "src" / "ui.h").read_text()
secs = re.search(r"enum \{ (SEC_OSC.*?), NSEC \}", ui).group(1).replace("SEC_", "").split(", ")
btns = re.search(r"SEC_BTN\[NSEC\] = \{(.*?)\};", ui, re.S).group(1)
btns = [b.strip() for b in re.sub(r"/\*.*?\*/", "", btns, flags=re.S).split(",") if b.strip()]
cols = [tuple(map(int, m)) for m in re.findall(r"RGB\((\d+), (\d+), (\d+)\)", re.search(
    r"SEC_COL\[NSEC\] = \{(.*?)\};", ui, re.S).group(1))]
STOCK = {"BTN_FX": "FX", "BTN_SEL": "SCL", "BTN_ENV": "ENV", "BTN_LFO": "LFO", "BTN_EDIT": "EDIT", "BTN_GLO": "GLO",
         "BTN_HOME": "HOME", "BTN_SAVE": "SAVE", "BTN_ARP": "ARP", "BTN_SEQ": "SEQ", "BTN_PLAY": "PLAY", "BTN_REC": "REC"}
ROLE = {"FILT": "FILT"}
bad = 0
over = {b[0]: b for b in panel.BUTTONS}
for sec, btn, col in zip(secs, btns, cols):
    if btn == "0xFFu":
        continue                                # (SETUP: SHIFT + PATCH)
    stock = STOCK.get(btn)
    b = over.get(stock)
    want = {"STEPS": "SEQ"}.get(sec, sec)   # (the enum SEC_STEPS: on screen and overlay SEQ)
    ok = b is not None and b[1] == want and b[3] is not None and panel.COL[b[3]] == col
    bad += not ok
    print(f"{sec:6s} on [{stock}]: overlay {b[1] if b else '-'} in {panel.COL.get(b[3]) if b and b[3] else '-'} "
          f"(ui.h {col}) {'ok' if ok else 'FAIL'}")
r = subprocess.run([sys.executable, str(HERE / "tools" / "panel.py"), "--check"], capture_output=True, text=True)
print(r.stdout.strip())
bad += r.returncode != 0
sys.exit(1 if bad else 0)
