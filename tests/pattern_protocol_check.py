#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Pattern commands through the real host firmware: independent patterns, source numbers and SAVE."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from ed_vectors import frame, parse

OUT = ROOT / "build/host/pattern-protocol"
subprocess.run(["cc", "-O2", "-w", "-DDINGHY_HOST", "-Ifirmware/hal", "-Ifirmware/src", "-Ifirmware/gen",
                "-o", str(OUT), "firmware/src/volna.c"], cwd=ROOT, check=True)
plain = bytes([1,16,64]) + (0x1240).to_bytes(2,"little") * 16
custom = bytes([1,7,44]) + (0x3fff).to_bytes(2,"little") * 16

def request(t, cmd, seq, payload=()):
    return f"{t} sysex {frame(cmd,seq,list(payload)).hex(' ')}"

def run(lines, fail=False):
    script = OUT.with_suffix(".txt")
    script.write_text("\n".join(lines + ["12000 end"]) + "\n")
    replies = Path(str(OUT) + "-sysex.txt")
    replies.unlink(missing_ok=True)
    env = dict(os.environ)
    if fail: env["VOLNA_PATTERN_WRITE_FAIL"] = "1"
    subprocess.run([str(OUT), str(OUT), str(script)], cwd=ROOT, check=True, stdout=subprocess.DEVNULL, env=env)
    result = {}
    for line in replies.read_text().splitlines():
        t, raw = line.split()
        cmd, seq, payload = parse(bytes.fromhex(raw))
        result[seq] = (int(t),cmd,bytes(payload))
    return result

r = run([
    request(1000,0x40,1), request(1100,0x41,2,[1]), request(1200,0x41,3,[127]),
    request(1300,0x42,4,bytes([127])+custom+bytes([2])), request(1400,0x41,5,[127,1]), request(1500,0x41,6,[1]),
    request(1410,0x41,19,[127]),
    request(1600,0x42,7,bytes([1])+custom), request(1700,0x41,8,[1]),
    request(1800,0x42,9,[127]), request(1900,0x42,10,bytes([127,1,0,44])+custom[3:]),
    request(1920,0x42,20,bytes([127])+custom+bytes([17])),
    request(1940,0x42,21,bytes([1])+plain+bytes([2])),
    request(2000,0x43,11), request(2100,0x10,12,[0]), request(2200,0x42,13,bytes([127])+plain),
    request(2220,0x41,22,[127,1]), "2250 midi 0xc0 42 0", request(2270,0x41,23,[127,1]),
    "2300 midi 0xc0 0 0", request(2400,0x41,14,[127,1]),
    "2700 midi 0x90 60 100", request(2800,0x42,15,bytes([1])+plain),
    request(2900,0x43,16), request(11000,0x01,18),
])
assert r[1][2] == bytes([0,16,16,1])
assert r[3][2] == bytes([0,127])+plain
assert r[4][2] == bytes([0,127]) and r[5][2] == bytes([0,127])+custom+bytes([2])
assert r[19][2] == bytes([0,127])+custom, "the old GET request still returns exactly 36 bytes after status"
assert r[6][2] == r[2][2], "editing a patch must not edit its template"
assert r[7][2] == bytes([0,1]) and r[8][2] == bytes([0,1])+custom
assert r[9][2][0] == 1 and r[10][2][0] == 1
assert r[20][2][0] == 1 and r[21][2][0] == 1
assert r[11][2] == bytes([0,0]) and r[12][2][0] == 0
assert r[22][2] == bytes([0,127])+plain+bytes([0]), "a raw PUT clears the source"
assert r[23][2][-1] == 0, "another patch must show its own source"
assert r[14][2] == bytes([0,127])+custom+bytes([2]), "SAVE and reloading must restore the pattern and its source"
assert r[15][2][0] == 3, "library writes must not erase while notes sound"
assert r[16][2] == bytes([0,0]) and r[16][0] > 2900, "SAVE waits for the release"
assert r[18][2][8] == 0, "SAVE clears the edited flag"
r = run([request(1000,0x41,1,[1]), request(1100,0x42,2,bytes([1])+custom), request(1300,0x41,3,[1])], True)
assert r[2][2][0] == 4 and r[1][2] == r[3][2], "write failure must be reported and preserve the template"
print("pattern protocol: copies, LOAD source, SAVE, validation, busy and failed writes OK")
