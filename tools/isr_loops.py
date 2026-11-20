#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna: the audio ISR's loops and their sizes, from build/volna.dis (a static estimate of what a sample costs).

Every backward branch inside app_audio (or, where the compiler inlined it, fm1_alnk0_irq) is a loop: its size in
instructions (from its target to the branch) and its stack accesses (spills: the registers ran out). The loops are
named by what they hold where that is plain (the FX: a call-free stretch at the function's end), otherwise by address.
A loop's size is not its cost per sample (branches skip parts), but it moves when the code does: a guard against
growth no one meant. With --max N, exit 1 if the function holds more than N instructions.

    python3 tools/isr_loops.py [build/volna.dis] [--max N]
"""
import re
import sys
from pathlib import Path

DIS = Path(next((a for a in sys.argv[1:] if not a.startswith("-") and not a.isdigit()), "build/volna.dis"))
LIMIT = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else None


def function(lines, name):
    try:
        s = next(i for i, l in enumerate(lines) if l.startswith(name + ":"))
    except StopIteration:
        return None
    ins = []
    for l in lines[s + 1:]:
        if re.match(r"^[A-Za-z_]\w*:\s*$", l):
            break
        m = re.match(r"\s*([0-9a-f]+):\s", l)
        if m:
            ins.append((int(m.group(1), 16), l))
    return ins


def main():
    lines = DIS.read_text().split("\n")
    for name in ("app_audio", "fm1_alnk0_irq"):
        ins = function(lines, name)
        if ins and len(ins) > 200:
            break
    else:
        raise SystemExit("isr_loops: no app_audio in " + str(DIS))
    addr = [a for a, _ in ins]
    loops = []
    for a, l in ins:
        m = re.search(r"goto -\d+ <[^:]*: ([0-9a-f]+) >", l)
        if not m:
            continue
        t = int(m.group(1), 16)
        body = [ll for x, ll in ins if t <= x <= a]
        if len(body) >= 20:
            loops.append((t, a, len(body), sum(1 for ll in body if "sp" in ll),
                          sum(1 for ll in body if "*" in ll)))
    print(f"{name}: {len(ins)} instructions, {len(loops)} loops of 20 or more")
    print("   from      to        size  stack  mul")
    for t, a, n, st, mu in sorted(loops):
        print(f"  {t:08x}  {a:08x}  {n:5d}  {st:5d}  {mu:4d}")
    if LIMIT is not None and len(ins) > LIMIT:
        print(f"isr_loops: {len(ins)} instructions, over --max {LIMIT}")
        sys.exit(1)


if __name__ == "__main__":
    main()
