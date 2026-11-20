#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna's editor protocol through the whole firmware on the host (tests/host.c "sysex" lines): INFO, NAMES, GET, PUT
into the sound and into a number (waiting for silence while a note sounds), BUSY, refusals, an older shorter record.

    python3 tests/editor_check.py        (builds and runs the host app; replies: build/host/volna-sysex.txt)
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from ed_vectors import frame, parse  # noqa: E402

INFO, NAMES, GET, PUT = 0x01, 0x02, 0x10, 0x11
SOUND = 0x7F
OK, BADREQ, BADPATCH, BUSY, FLASH, UNKNOWN = range(6)

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)
        print("FAIL", what)


def hexf(b):
    return " ".join(f"{x:02X}" for x in b)


NOTES = []                                          # CHANGED, unasked (seq 0): (time, payload)


def run(lines):
    NOTES.clear()
    script = ROOT / "build/host/editor.txt"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text("\n".join(lines) + "\n")
    out = ROOT / "build/host/volna-sysex.txt"
    out.unlink(missing_ok=True)
    subprocess.run(["sh", "tests/run_host.sh", str(script.relative_to(ROOT))], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    replies = {}
    for ln in out.read_text().split("\n") if out.exists() else []:
        if not ln.strip():
            continue
        t, h = ln.split()
        r = parse(bytes.fromhex(h))
        check(r is not None, f"a reply that does not parse: {h}")
        if r:
            cmd, seq, payload = r
            if cmd == 0xE0:
                NOTES.append((int(t), payload))
            else:
                replies[seq] = (int(t), cmd, payload)
    return replies


def record(name, patch):
    return bytes([2, 0, len(patch) & 0xFF, len(patch) >> 8]) + name.encode().ljust(12)[:12] + bytes(patch)


# ---- the first run: read what is there
r = run([
    f"1000 sysex {hexf(frame(INFO, 1))}",
    f"1100 sysex {hexf(frame(NAMES, 2, [0, 25]))}",
    f"1200 sysex {hexf(frame(GET, 3, [SOUND]))}",
    f"1300 sysex {hexf(frame(GET, 4, [1]))}",
    "2000 end",
])
t, cmd, p = r.get(1, (0, 0, b"\xff"))
check(cmd == 0x81 and p[0] == OK and len(p) == 27, f"INFO: {cmd:#x} {p.hex()}")
proto, npatch, size, pmax, prog = p[1], p[2], p[3] | p[4] << 8, p[5] | p[6] << 8, p[7]
check(proto == 4 and npatch == 100 and pmax == 494 and 8 <= size <= pmax, f"INFO fields {proto} {npatch} {size} {pmax}")
check(p[9:17].rstrip(b"\0") == b"VOLNA", f"INFO name {p[9:17]}")
print(f"INFO: protocol {proto}, {npatch} patches, patch_t {size} B, the sound from {prog:02d}, version {p[17:25].rstrip(bytes(1))}")

t, cmd, p = r.get(2, (0, 0, b"\xff"))
check(cmd == 0x82 and p[0] == OK and p[1] == 0 and p[2] == 25 and len(p) == 3 + 25 * 12, "NAMES 0..24")
names = [p[3 + i * 12:15 + i * 12].decode() for i in range(25)]
print("names:", ", ".join(n.strip() for n in names[:8]), "...")

t, cmd, p = r.get(4, (0, 0, b"\xff"))
check(cmd == 0x90 and p[0] == OK and p[1] == 1 and p[2] == 2 and (p[4] | p[5] << 8) == size and len(p) == 2 + 16 + size,
      "GET 1")
check(p[6:18].decode() == names[1], f"GET 1's name {p[6:18]} is NAMES' {names[1]}")
rec1 = bytes(p[2:])
patch1 = bytearray(rec1[16:])

# ---- the second run: writes (patch 1 under other names: stored and read back as sent)
bad_ver = bytearray(record("BAD", patch1)); bad_ver[0] = 3
bad_val = bytearray(patch1); bad_val[0] = 200          # OSC1's wave: 200 of 64
short = record("short one", patch1[:100])

r = run([
    f"1000 sysex {hexf(frame(PUT, 10, [SOUND] + list(record('from web', patch1))))}",
    f"1100 sysex {hexf(frame(GET, 11, [SOUND]))}",
    "1200 keys 1",                                       # a note held: a write to a number waits for silence
    f"1300 sysex {hexf(frame(PUT, 12, [7] + list(record('slot seven', patch1))))}",
    f"1400 sysex {hexf(frame(PUT, 13, [8] + list(record('too soon', patch1))))}",
    "2200 keys 0",
    f"8000 sysex {hexf(frame(NAMES, 14, [5, 5]))}",
    f"8100 sysex {hexf(frame(GET, 15, [7]))}",
    f"8200 sysex {hexf(frame(PUT, 16, [9] + list(bad_ver)))}",
    f"8300 sysex {hexf(frame(PUT, 17, [9] + list(record('bad value', bad_val))))}",
    f"8400 sysex {hexf(frame(0x3F, 18))}",                    # (0x30..0x35: the tables, protocol 3)
    f"8500 sysex {hexf(frame(NAMES, 19, [0, 0]))}",
    f"8600 sysex {hexf(frame(GET, 20, [100]))}",
    f"8700 sysex {hexf(frame(PUT, 21, [9] + list(short)))}",
    f"12000 sysex {hexf(frame(GET, 22, [9]))}",
    f"12100 sysex {hexf(frame(INFO, 23))}",
    "13000 end",
])

t, cmd, p = r.get(10, (0, 0, b"\xff"))
check(cmd == 0x91 and p[0] == OK and p[1] == SOUND, "PUT into the sound")
t, cmd, p = r.get(11, (0, 0, b"\xff"))
check(cmd == 0x90 and p[0] == OK and p[6:18] == b"FROM WEB    " and bytes(p[18:]) == bytes(patch1),
      f"the sound now: {p[6:18]}, the patch as sent")

t, cmd, p = r.get(12, (0, 0, b"\xff"))
check(cmd == 0x91 and p[0] == OK and p[1] == 7, f"PUT 7 answered: {p.hex()}")
check(t > 2200, f"PUT 7 written after the note let go (at {t} ms)")
print(f"PUT 7: answered at {t} ms (the note let go at 2200)")
t, cmd, p = r.get(13, (0, 0, b"\xff"))
check(cmd == 0x91 and p[0] == BUSY, f"PUT 8 while 7 waits: BUSY ({p.hex()})")

t, cmd, p = r.get(14, (0, 0, b"\xff"))
check(cmd == 0x82 and p[0] == OK and p[3 + 2 * 12:3 + 3 * 12] == b"SLOT SEVEN  ", f"NAMES: 7 is SLOT SEVEN ({p[27:39]})")
check(p[3 + 3 * 12:3 + 4 * 12] != b"TOO SOON    ", "8 not written")
t, cmd, p = r.get(15, (0, 0, b"\xff"))
check(cmd == 0x90 and p[0] == OK and bytes(p[18:]) == bytes(patch1), "GET 7: as sent")

for seq, want, what in [(16, BADPATCH, "a record of version 3"), (17, BADPATCH, "a wave out of range"),
                        (18, UNKNOWN, "an unknown command"), (19, BADREQ, "NAMES of none"), (20, BADREQ, "GET 100")]:
    t, cmd, p = r.get(seq, (0, 0, b"\xff"))
    check(p[:1] == bytes([want]), f"{what}: status {p[:1].hex()}, want {want}")

t, cmd, p = r.get(21, (0, 0, b"\xff"))
check(cmd == 0x91 and p[0] == OK, f"an older, shorter record (100 B) accepted: {p.hex()}")
t, cmd, p = r.get(22, (0, 0, b"\xff"))
check(cmd == 0x90 and p[0] == OK and (p[4] | p[5] << 8) == size and bytes(p[18:118]) == bytes(patch1[:100]),
      "GET 9: its 100 bytes as sent, the full size")
t, cmd, p = r.get(23, (0, 0, b"\xff"))
check(cmd == 0x81 and p[8] == 1, f"INFO: the sound is edited after a PUT into it ({p[8]})")

# ---- the third run: the parameters (protocol 2)
PDESC, PVAL, PSET, PCHOICES, LAYOUT, WATCH = 0x20, 0x21, 0x22, 0x23, 0x24, 0x25


def s16(v):
    return [v & 0xFF, (v >> 8) & 0xFF]


def i16(b):
    return int.from_bytes(bytes(b[:2]), "little", signed=True)


P_WAVE, P_CUT, P_FTYPE = 1, 16, 20
r = run([
    f"1000 sysex {hexf(frame(PDESC, 30, [0, 16]))}",
    f"1100 sysex {hexf(frame(LAYOUT, 31, [0, 1]))}",            # OSC, OSC2
    f"1200 sysex {hexf(frame(PVAL, 32, [1, P_WAVE]))}",
    f"1300 sysex {hexf(frame(PSET, 33, [1, P_WAVE] + s16(40)))}",
    f"1400 sysex {hexf(frame(PSET, 34, [1, P_CUT] + s16(300)))}",   # past its top: 127
    f"1500 sysex {hexf(frame(PSET, 35, [0, P_FTYPE] + s16(1)))}",
    f"1600 sysex {hexf(frame(PCHOICES, 36, [0, P_FTYPE, 0, 3]))}",
    f"1700 sysex {hexf(frame(PVAL, 37, [3, P_WAVE]))}",          # OSC 4: none
    f"1800 sysex {hexf(frame(GET, 38, [SOUND]))}",
    f"1900 sysex {hexf(frame(WATCH, 39, [1]))}",
    f"2000 sysex {hexf(frame(PSET, 40, [0, P_CUT] + s16(20)))}",   # the editor's own: no CHANGED for it
    "2500 enc 2 3",                                              # the panel: KNOB1 (OSC row A: TABLE)
    "14000 enc 2 -3",                                            # 12 s on, no WATCH since: no CHANGED
    "15000 end",
])
t, cmd, p = r.get(30, (0, 0, b"\xff"))
check(cmd == 0xA0 and p[0] == OK and p[1] == 0 and p[2] == 16 and len(p) == 3 + 16 * 16, f"PDESC 0..15 ({len(p)} B)")
d = p[3 + 16:3 + 32]                                         # id 1: WAVE
check(d[:8].rstrip(b"\0") == b"WAVE" and i16(d[8:]) == 0 and i16(d[10:]) == 63 and d[14] == 1,
      f"PDESC WAVE: {d[:8]} {i16(d[8:])}..{i16(d[10:])} kind {d[14]}")
t, cmd, p = r.get(31, (0, 0, b"\xff"))
check(cmd == 0xA4 and p[0] == OK and p[3] == 1 and p[4] == 4 and p[5:13].rstrip(b"\0") == b"OSC", f"LAYOUT OSC: {p.hex()}")
check(list(p[13:17]) == [4, 1, 2, 3], f"OSC row A: TABLE WAVE SEMI FINE ({list(p[13:17])})")
t, cmd, p = r.get(33, (0, 0, b"\xff"))
check(cmd == 0xA2 and p[0] == OK and i16(p[3:]) == 40 and bytes(p[8:]) == b"40", f"PSET OSC2 WAVE 40: {p.hex()}")
t, cmd, p = r.get(34, (0, 0, b"\xff"))
check(p[0] == OK and i16(p[3:]) == 127, f"PSET CUTOFF 300: clamped to {i16(p[3:])}")
t, cmd, p = r.get(35, (0, 0, b"\xff"))
t2, cmd2, c = r.get(36, (0, 0, b"\xff"))
names = bytes(c[5:]).split(b"\0")[:3]
check(p[0] == OK and p[7] >= 3 and c[0] == OK and bytes(p[8:]) == names[1], f"FILTER TYPE 1: {p[8:]}, choices {names}")
t, cmd, p = r.get(37, (0, 0, b"\xff"))
check(p[:1] == bytes([BADREQ]), f"OSC 4: BADREQ ({p.hex()})")
t, cmd, p = r.get(38, (0, 0, b"\xff"))
check(p[0] == OK and len(p) > 23 and p[18 + 5] == 40, "the sound: OSC2's wave 40")
print("CHANGED:", [(t, n.hex()) for t, n in NOTES])
check(len(NOTES) == 1 and 2500 <= NOTES[0][0] < 3000, f"one CHANGED, for the panel's knob only: {NOTES}")

if fails:
    print(f"editor_check: {len(fails)} failed")
    sys.exit(1)
print("editor_check: ok")
