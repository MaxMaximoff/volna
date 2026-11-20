#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: run tests/scripts/own.txt on the host and check the patch's own pattern (PATTERN OWN) from the arp's notes
as they go out over MIDI:
  03 BASS-1 plays its own groove: 13 notes a bar (three ties), the velocities of its accents, two bars alike;
  COPY puts the bank's pattern 2 into OWN (8 notes a bar); SEQ edits OWN (step 2 on: 9);
  SAVE writes the own steps (with the patch: 03's sector 0, the format 2); the bank's pattern 2 is as it was (8); the patch back: OWN as saved (9).
Exit 1 if not.   python3 tests/own_check.py"""
import re
import subprocess
import sys

r = subprocess.run(["build/host/volna", "build/host/own", "tests/scripts/own.txt"], check=True, capture_output=True,
                   text=True)
ons = [(int(m.group(1)), int(m.group(2))) for m in re.finditer(r"(\d+) ms  MIDI out 90 \d+ (\d+)", r.stdout)]
saved = [(int(m.group(1)), int(m.group(2))) for m in re.finditer(r"(\d+) ms  patches (\d+) saved", r.stdout)]
n = lambda t0, t1: sum(1 for t, _ in ons if t0 - 10 <= t < t1 - 10)   # (a step comes a ms early: a bar's next one out)
bad = 0


def check(what, ok, msg):
    global bad
    bad += not ok
    print(f"{what}: {msg} {'ok' if ok else 'FAIL'}")


BAR = [100, 100, 68, 100, 100, 100, 100, 100, 127, 68, 100, 100, 100]
v = [x for t, x in ons if 1490 <= t < 5490]
check("03 BASS-1, its own groove", v == BAR * 2, f"{len(v)} notes in two bars, velocities {v}")
check("COPY: the bank's 2 into OWN", n(7000, 9000) == 8, f"{n(7000, 9000)} notes a bar (want 8)")
check("SEQ edits OWN", n(11000, 13000) == 9, f"{n(11000, 13000)} notes a bar (want 9)")
check("SAVE: the own steps written", any(b == 0 and 13300 <= t < 14000 for t, b in saved), f"patch sectors saved {saved}")
check("the bank's pattern 2 as it was", n(14500, 16500) == 8, f"{n(14500, 16500)} notes a bar (want 8)")
check("the patch back: OWN as saved", n(17500, 19500) == 9, f"{n(17500, 19500)} notes a bar (want 9)")
sys.exit(1 if bad else 0)
