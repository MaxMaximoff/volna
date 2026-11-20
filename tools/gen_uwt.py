#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna's local wavetables: firmware/gen/wt_local.h from <repo>/local/wavetables/ (UWT.mid, *.wav), or a stub.

UWT.mid (Achim Gratz "Stromeko", http://synth.stromeko.net/sounds/UWT.zip) is 18 Waldorf Blofeld user wavetables,
slots 80..97, as Blofeld wavetable SysEx (WTBD) in a standard MIDI file: per wave one 410-byte frame
  F0 3E 13 dev 12 slot wave, 128 samples of 3 bytes (signed 21 bits, 7 bits a byte, high first),
  14 bytes of name, a checksum (the sum of bytes 7..407 & 0x7F), F7.
64 waves of 128 samples: Volna's own table shape (engine/wt_osc.h), so they go in as they are, each table scaled to
a 30000 peak (wt_sample's products stay under 2^31), with their mip-maps (tools/wt_mip.py: ~49 KB a table).

Most of these tables are ROM waves of commercial synths (Prophet VS, microWave, SQ80): they live in local/ (not in
git, not published, copied between machines by hand) and so does what is made from them: wt_local.h is ignored by
git, and a firmware build (build.py) leaves them out unless asked (--with-local; since 2026-11-17: they go into the user
wavetable slots from the site instead). The host tests (tests/run_host.sh) still build them in when local/ is there. Without the file, or with
VOLNA_LOCAL_WT=0, the header is a stub (WT_LOCAL_N 0) and Volna has its own tables only (tools/gen_tables.py).

Besides, every *.wav in local/wavetables/ (Serum's wavetable WAV and its kin: single cycles end to end) goes in
after the UWT ones, made into Volna's shape by tools/wt_import.py (band-limited to 63 harmonics, 64 of its frames
picked); its name is the file's. VOLNA_WT_WAV picks them: unset or "all", "none", or names without .wav, comma-
separated. Where such a WAV comes from and whether it may be published goes in local/README.md.

Which slots go in: SLOTS below (one table is ~49 KB of flash with its mip-maps: two or three fit next to Volna's own,
not all 18), or VOLNA_UWT_SLOTS, e.g. "81", "81,83" or "80-97" / "all" (the build then says the image is too big).

    python3 tools/gen_uwt.py            (writes firmware/gen/wt_local.h when it changes; says what it embedded)
    python3 tools/gen_uwt.py --stub     (the stub, whatever is in local/)
"""
import hashlib
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wt_import  # noqa: E402
import wt_mip  # noqa: E402

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]                          # fm-firmwares/
UWT = Path(os.environ.get("VOLNA_UWT", ROOT / "local" / "wavetables" / "UWT.mid"))
OUT = HERE / "firmware" / "gen" / "wt_local.h"
FRAME, NSAMP, NWAVE, PEAK = 410, 128, 64, 30000
SLOTS = "81"                                    # VS-ROM 32-95 (Prophet VS ROM waves) for now: one table, not all 18


def slots_wanted():
    spec = os.environ.get("VOLNA_UWT_SLOTS", SLOTS).strip().lower()
    if spec == "all":
        return None
    out = set()
    for part in spec.replace(" ", "").split(","):
        if "-" in part:
            a, b = part.split("-")
            out.update(range(int(a), int(b) + 1))
        elif part:
            out.add(int(part))
    return out


def frames(buf):
    """the WTBD frames in a standard MIDI file (sysex events: F0, a length, the rest up to F7) or in raw .syx"""
    i = 0
    while i < len(buf):
        if buf[i] != 0xF0:
            i += 1
            continue
        if buf[:4] == b"MThd":                  # SMF: a variable-length quantity after F0
            n, j = 0, i + 1
            while j < len(buf):
                b = buf[j]
                j += 1
                n = n << 7 | (b & 0x7F)
                if not b & 0x80:
                    break
            data = buf[j:j + n]
            if len(data) == FRAME - 1 and data[-1] == 0xF7:
                yield bytes([0xF0]) + data
            i = j + n
        else:
            j = buf.find(b"\xF7", i)
            if j < 0:
                break
            if j - i + 1 == FRAME:
                yield buf[i:j + 1]
            i = j + 1


def parse(buf):
    tables = {}
    for f in frames(buf):
        if f[1] != 0x3E or f[2] != 0x13 or f[4] != 0x12:
            continue
        if sum(f[7:408]) & 0x7F != f[408]:
            raise SystemExit(f"UWT: a frame of slot {f[5]} wave {f[6]} fails its checksum")
        slot, wave = f[5], f[6]
        samples = []
        for k in range(NSAMP):
            hi, mid, lo = f[8 + 3 * k:11 + 3 * k]
            v = (hi & 0x7F) << 14 | (mid & 0x7F) << 7 | (lo & 0x7F)
            samples.append(v - (1 << 21) if v & 0x100000 else v)
        name = bytes(b & 0x7F for b in f[392:406]).decode("ascii", "replace").strip()
        t = tables.setdefault(slot, {"name": name, "waves": {}})
        t["waves"][wave] = samples
    out = []
    for slot in sorted(tables):
        t = tables[slot]
        if sorted(t["waves"]) != list(range(NWAVE)):
            raise SystemExit(f"UWT: slot {slot} has {len(t['waves'])} of 64 waves")
        out.append((slot, t["name"], [t["waves"][w] for w in range(NWAVE)]))
    return out


def safe_name(s):
    s = "".join(c if c.isalnum() or c in " -+." else " " for c in s.upper()).strip()
    return " ".join(s.split())[:12] or "USER"


def wavs_wanted():
    """the local *.wav tables to embed (VOLNA_WT_WAV: unset or "all", "none", or names without .wav)"""
    found = sorted(UWT.parent.glob("*.wav")) if UWT.parent.is_dir() else []
    spec = os.environ.get("VOLNA_WT_WAV", "all").strip().lower()
    if spec == "all":
        return found
    names = {n.strip() for n in spec.split(",") if n.strip() and n.strip() != "none"}
    missing = names - {f.stem.lower() for f in found}
    if missing:
        raise SystemExit(f"VOLNA_WT_WAV: no {sorted(missing)} in {UWT.parent}")
    return [f for f in found if f.stem.lower() in names]


def wav_table(f):
    """a WAV made into 64 waves (tools/wt_import.py, a few seconds of pure Python), kept in build/wt_cache/ by the
    hash of the file and of the importer: every host run and build calls this script"""
    key = hashlib.sha1(f.read_bytes() + Path(wt_import.__file__).read_bytes() + str(PEAK).encode()).hexdigest()
    cache = HERE / "build" / "wt_cache" / f"{key}.json"
    try:
        d = json.loads(cache.read_text())
        return d["waves"], d["note"]
    except (OSError, ValueError, KeyError):
        pass
    waves, note, _ = wt_import.load(f, PEAK)
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps({"waves": waves, "note": note}))
    return waves, note


def header(tables):
    head = ("/* SPDX-License-Identifier: GPL-3.0-only */\n"
            "/* generated by tools/gen_uwt.py: do not edit, do not commit (.gitignore). */\n"
            "#ifndef WT_LOCAL_H\n#define WT_LOCAL_H\n#include <stdint.h>\n")
    if not tables:
        return head + ("/* no local wavetables (local/wavetables/ absent, or left out): Volna's own only */\n"
                       "#define WT_LOCAL_N 0\n#endif\n")
    lines = [head,
             f"/* {len(tables)} tables from local/wavetables/ (not to be published), each scaled to a {PEAK} peak */\n",
             f"#define WT_LOCAL_N {len(tables)}\n",
             "static const char *const WT_LOCAL_NAME[WT_LOCAL_N] = {"
             + ", ".join('"' + safe_name(n) + '"' for _, n, _ in tables) + "};\n",
             f"static const int16_t WT_LOCAL[WT_LOCAL_N][{wt_mip.WORDS}] = {{\n"]
    for src, name, waves in tables:
        peak = max(1, max(abs(v) for w in waves for v in w))
        lines.append(f"  {{ /* {src}: {safe_name(name)} */\n")
        lines.append(wt_mip.table_c([[int(round(v * PEAK / peak)) for v in w] for w in waves]) + "\n")
        lines.append("  },\n")
    lines.append("};\n#endif\n")
    return "".join(lines)


def main():
    stub = "--stub" in sys.argv or os.environ.get("VOLNA_LOCAL_WT", "1") == "0"
    tables = [] if stub or not UWT.exists() else parse(UWT.read_bytes())
    want = slots_wanted()
    if want is not None:
        missing = sorted(want - {slot for slot, _, _ in tables})
        if tables and missing:
            raise SystemExit(f"VOLNA_UWT_SLOTS: no slot {missing} in {UWT.name}")
        tables = [t for t in tables if t[0] in want]
    tables = [(f"{UWT.name} slot {slot}", n, w) for slot, n, w in tables]
    for f in [] if stub else wavs_wanted():         # after the UWT ones: their indices in saved patches stay
        waves, note = wav_table(f)
        print(f"wavetables: {f.name}: {note}")
        tables.append((f.name, f.stem, waves))
    text = header(tables)
    if not OUT.exists() or OUT.read_text() != text:
        OUT.write_text(text, newline="\n")
    if tables:
        print(f"wavetables: Volna's own + {len(tables)} local ("
              + ", ".join(f"{src}: {safe_name(n)}" for src, n, _ in tables) + ") -- a build to keep to yourself")
    else:
        print("wavetables: Volna's own only" + (" (local ones left out)" if stub and UWT.parent.is_dir() else ""))


if __name__ == "__main__":
    main()
