#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna's user wavetables through the whole firmware on the host (tests/host.c: the slots as NOR in RAM): the editor
protocol 3 (TABLE INFO, BEGIN, DATA, END, NAME, CLEAR), then the sound.

Volna's own SYNC table (firmware/gen/wt_public.h) goes into USER 1 as the site would send it; OSC1 on USER 1 must sound
exactly as OSC1 on SYNC (the same samples, read through the slot instead of the image). Also: BEGIN waits for silence,
DATA out of order is refused, a wrong CRC leaves the slot empty, NAME and CLEAR.

    python3 tests/uwt_check.py        (builds the host app with tests/run_host.sh first)
"""
import re
import struct
import subprocess
import sys
import wave
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from ed_vectors import frame, parse  # noqa: E402

INFO, PDESC, PSET = 0x01, 0x20, 0x22
TINFO, TBEGIN, TDATA, TEND, TNAME, TCLEAR = 0x30, 0x31, 0x32, 0x33, 0x34, 0x35
OK, BADREQ, BADPATCH, BUSY, FLASH, UNKNOWN = range(6)
P_TABLE = 4
SYNC = 2
HOST = ROOT / "build/host/volna"
OUT = ROOT / "build/host/uwt"

fails = []


def check(cond, what):
    print(("ok  " if cond else "FAIL"), what)
    if not cond:
        fails.append(what)


def hexf(b):
    return "".join(f"{x:02X}" for x in b)


def u32(v):
    return list(struct.pack("<I", v))


def own_table(t):
    """Volna's own table t as int16 bytes (little-endian), from the generated header"""
    text = (ROOT / "firmware/gen/wt_public.h").read_text()
    body = re.sub(r"/\*.*?\*/", "", text[text.index("WT_PUB[WT_PUB_N][24960] = {") + 27:], flags=re.S)
    rows = re.findall(r"\{([-0-9,\s]+)\}", body)
    vals = [int(x) for x in rows[t].split(",") if x.strip()]
    assert len(vals) == 24960, len(vals)
    return struct.pack("<24960h", *vals)


def upload(t, seq, slot, data, crc, name, gap=15):
    """BEGIN at t (answered once silent), the DATA from t + 3000, END after: the script's lines, the next free time"""
    lines = [f"{t} sysex {hexf(frame(TBEGIN, seq, [slot] + u32(crc) + list(name.encode().ljust(12)[:12])))}"]
    t += 3000
    for k, off in enumerate(range(0, len(data), 512)):
        lines.append(f"{t} sysex {hexf(frame(TDATA, (seq + 1 + k) % 128, [slot] + u32(off) + list(data[off:off + 512])))}")
        t += gap
    return lines, t


def run(lines, out):
    script = Path(str(out) + ".txt")
    script.write_text("\n".join(lines) + "\n")
    for p in (Path(str(out) + "-sysex.txt"), Path(str(out) + ".wav")):
        p.unlink(missing_ok=True)
    subprocess.run([str(HOST), str(out), str(script)], check=True, stdout=subprocess.DEVNULL)
    replies = []
    sx = Path(str(out) + "-sysex.txt")
    for ln in sx.read_text().split("\n") if sx.exists() else []:
        if ln.strip():
            t, h = ln.split()
            r = parse(bytes.fromhex(h))
            if r and r[0] != 0xE0:
                replies.append((int(t), r[0], r[1], bytes(r[2])))
    return replies


def reply(replies, cmd, seq):
    for t, c, s, p in replies:
        if c == (cmd | 0x80) and s == seq:
            return t, p
    return 0, b"\xff"


def slots(p):
    """TABLE INFO's payload (after the status): [(filled, crc, name)]"""
    n = p[1]
    return [(p[9 + i * 17], struct.unpack_from("<I", p, 10 + i * 17)[0], p[14 + i * 17:26 + i * 17]) for i in range(n)]


OUT.parent.mkdir(parents=True, exist_ok=True)
(ROOT / "build/host/uwt-build.txt").write_text("100 end\n")
subprocess.run(["sh", "tests/run_host.sh", "build/host/uwt-build.txt"], cwd=ROOT, check=True,
               stdout=subprocess.DEVNULL)   # (builds build/host/volna)
table = own_table(SYNC)
crc = zlib.crc32(table)

# ---- run A: the uploads, then OSC1 on USER 1
up1, t = upload(1300, 10, 0, table, crc, "sync copy")
up2, t2 = upload(t + 500, 20, 1, table, crc ^ 1, "broken")
lines = [f"1000 sysex {hexf(frame(INFO, 1))}", f"1050 sysex {hexf(frame(TINFO, 2))}",
         f"1100 sysex {hexf(frame(PDESC, 3, [P_TABLE, 1]))}",
         "1200 keys 1", "2200 keys 0"]                       # a note held: BEGIN waits for silence
lines += up1
lines += [f"{t} sysex {hexf(frame(TDATA, 5, [0] + u32(0) + list(table[:512])))}",   # out of order: refused
          f"{t + 100} sysex {hexf(frame(TEND, 6, [0]))}",
          f"{t + 200} sysex {hexf(frame(TINFO, 7))}"]
lines += up2
T = t2 + 100
lines += [f"{T} sysex {hexf(frame(TEND, 8, [1]))}", f"{T + 100} sysex {hexf(frame(TINFO, 9))}",
          f"{T + 200} sysex {hexf(frame(TNAME, 120, [0] + list(b'renamed'.ljust(12))))}",
          f"{T + 400} sysex {hexf(frame(TCLEAR, 121, [2]))}",
          f"{T + 600} sysex {hexf(frame(TNAME, 122, [3] + list(b'empty'.ljust(12))))}",   # an empty slot: no name
          f"{T + 800} sysex {hexf(frame(TINFO, 123))}"]
NOTE = (T // 1000 + 2) * 1000                               # the same moment in both runs
sound = lambda pos: [f"{NOTE - 500} sysex {hexf(frame(PSET, 124, [0, P_TABLE, pos & 0xFF, pos >> 8]))}",
                     f"{NOTE} midi 0x90 57 100", f"{NOTE + 800} midi 0x80 57 0", f"{NOTE + 2000} end"]

r0 = run(["1100 sysex " + hexf(frame(PDESC, 3, [P_TABLE, 1])), "1500 end"], OUT)
_, p = reply(r0, PDESC, 3)
hi = struct.unpack_from("<h", p, 13)[0] if len(p) > 14 else -1
user1 = hi - 3                                              # (the knob: ..., the local ones, USER 1..4 last)
check(p[3:8] == b"TABLE" and hi >= 10, f"PDESC TABLE: 0..{hi}, USER 1 at {user1}")

rA = run(sorted(lines + sound(user1), key=lambda l: int(l.split()[0])), OUT)
_, p = reply(rA, INFO, 1)
check(p[:2] == bytes([OK, 4]), f"INFO: protocol {p[1] if len(p) > 1 else '?'}")
_, p = reply(rA, TINFO, 2)
s = slots(p) if p[0] == OK else []
check(p[0] == OK and p[1] == 4 and p[2] == 59 and struct.unpack_from("<I", p, 3)[0] == 49920
      and struct.unpack_from("<H", p, 7)[0] == 512 and not any(x[0] for x in s),
      f"TABLE INFO: 4 slots from 59, 49920 B, chunks of 512, all empty ({p[:9].hex()})")
tb, p = reply(rA, TBEGIN, 10)
check(p[:2] == bytes([OK, 0]) and tb > 2200, f"BEGIN 1: answered at {tb} ms, after the note let go at 2200")
ok_data = all(reply(rA, TDATA, (11 + k) % 128)[1][:1] == bytes([OK]) for k in range(98))
check(ok_data, "DATA: 98 chunks taken")
_, p = reply(rA, TDATA, 5)
check(p[:1] == bytes([BADREQ]), f"DATA out of order: refused ({p[:1].hex()})")
_, p = reply(rA, TEND, 6)
check(p[:2] == bytes([OK, 0]), f"END 1: OK ({p[:2].hex()})")
_, p = reply(rA, TINFO, 7)
s = slots(p)
check(s[0] == (1, crc, b"SYNC COPY   "), f"USER 1 filled: {s[0]}")
_, p = reply(rA, TEND, 8)
check(p[:2] == bytes([BADPATCH, 1]), f"END 2, a wrong CRC: BADPATCH ({p[:2].hex()})")
_, p = reply(rA, TINFO, 9)
check(not slots(p)[1][0], "USER 2 stays empty")
_, p = reply(rA, TNAME, 120)
check(p[:2] == bytes([OK, 0]), f"NAME 1: OK ({p[:2].hex()})")
_, p = reply(rA, TCLEAR, 121)
check(p[:2] == bytes([OK, 2]), f"CLEAR 3: OK ({p[:2].hex()})")
_, p = reply(rA, TNAME, 122)
check(p[:1] == bytes([BUSY]), f"NAME of an empty slot: refused ({p[:1].hex()})")
_, p = reply(rA, TINFO, 123)
s = slots(p)
check(s[0] == (1, crc, b"RENAMED     ") and [x[0] for x in s] == [1, 0, 0, 0], f"after NAME and CLEAR: {s}")
_, p = reply(rA, PSET, 124)
check(p[:1] == bytes([OK]) and struct.unpack_from("<h", p, 3)[0] == user1, f"PSET OSC1 TABLE {user1}: {p[:12].hex()}")

# ---- run B: no uploads, OSC1 on SYNC itself (the early note too: the same voice gets the note, at the same phase)
rB = run(["1200 keys 1", "2200 keys 0"] + sound(SYNC), OUT.with_name("uwt-sync"))


def frames(path):
    with wave.open(str(path)) as w:
        fs = w.getframerate()
        d = w.readframes(w.getnframes())
    return fs, struct.unpack(f"<{len(d) // 2}h", d)


fs, a = frames(Path(str(OUT) + ".wav"))
_, b = frames(OUT.with_name("uwt-sync.wav"))
i0, i1 = int(NOTE / 1000 * fs) * 2, int((NOTE + 1500) / 1000 * fs) * 2
loud = max(abs(x) for x in a[i0:i1]) if i1 <= len(a) else 0
same = i1 <= len(a) and i1 <= len(b) and a[i0:i1] == b[i0:i1]
check(loud > 1000 and same, f"OSC1 on USER 1 sounds as on SYNC, sample for sample (peak {loud})")

if fails:
    print(f"uwt_check: {len(fails)} failed")
    sys.exit(1)
print("uwt_check: ok")
