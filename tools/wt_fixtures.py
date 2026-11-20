#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Reference wavetables for the site's converter (web/src/fw/volna/wavetable.ts, its tests/wavetable.test.ts): a few
inputs drawn here from formulas (no one else's waves: they go into git), and what the firmware's own tools make of
them (wt_import.load, gen_uwt.parse, the peak as gen_uwt.header, wt_mip.rows). The site must make the same tables.

    python3 tools/wt_fixtures.py     -> web/tests/fixtures/wavetables/: the inputs and expected.json (CRC-32 of each
                                        table, the frames chosen); the tables themselves -> build/wt_fixtures/*.bin
"""
import json
import math
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_uwt  # noqa: E402
import wt_import  # noqa: E402
import wt_mip  # noqa: E402

HERE = Path(__file__).resolve().parents[1]
OUT = HERE.parents[1] / "web" / "tests" / "fixtures" / "wavetables"
BINS = HERE / "build" / "wt_fixtures"
PEAK = 30000


def wav(path, frames, fmt, clm=None):
    """frames: lists of floats in -1..1; fmt: 16, 24, 32 (PCM) or "f32" """
    data = bytearray()
    for f in frames:
        for v in f:
            if fmt == "f32":
                data += struct.pack("<f", v)
            elif fmt == 16:
                data += struct.pack("<h", max(-32768, min(32767, round(v * 32767))))
            elif fmt == 24:
                data += max(-8388608, min(8388607, round(v * 8388607))).to_bytes(3, "little", signed=True)
            else:
                data += struct.pack("<i", max(-2147483648, min(2147483647, round(v * 2147483647))))
    tag, bits = (3, 32) if fmt == "f32" else (1, fmt)
    fmtc = struct.pack("<HHIIHH", tag, 1, 48000, 48000 * bits // 8, bits // 8, bits)
    chunks = b"fmt " + struct.pack("<I", len(fmtc)) + fmtc
    if clm:
        body = f"<!>{clm} 00000000 wavetable (fixture)".encode()
        chunks += b"clm " + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")
    chunks += b"data" + struct.pack("<I", len(data)) + bytes(data)
    path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)


def morph(n, frames):
    """sine -> saw over the first half, a jump to a square, then the square narrowing to a pulse"""
    out = []
    for f in range(frames):
        t = f / (frames - 1)
        row = []
        for i in range(n):
            ph = i / n
            if t < 0.5:
                a = t * 2
                v = (1 - a) * math.sin(2 * math.pi * ph) + a * (2 * ph - 1)
            else:
                w = 0.5 - (t - 0.5) * 0.8
                v = 0.8 if ph < w else -0.8
            row.append(0.9 * v)
        out.append(row)
    return out


def sweep(n, frames):
    """a harmonic sweep: more and more of a saw's partials, a little noise-free wobble"""
    out = []
    for f in range(frames):
        top = 1 + f * 40 // max(1, frames - 1)
        row = [0.6 * sum(math.sin(2 * math.pi * k * i / n + 0.3 * f) / k for k in range(1, top + 1)) for i in range(n)]
        out.append(row)
    return out


def serum_like():
    """Serum's own export (256 frames of 2048, float 32, a clm chunk), drawn by exact arithmetic alone, so the site's
    test draws the same samples (tests/wavetable.test.ts serumLike) and the 2 MB WAV stays out of git: a saw turning
    into a triangle over 128 frames, then a square narrowing to a pulse"""
    n, out = 2048, []
    for f in range(256):
        row = []
        for i in range(n):
            saw = (2 * i - n) / n
            tri = 1 - 4 * abs(i - n // 2) / n
            if f < 128:
                row.append(((127 - f) * saw + f * tri) / 127 * 0.875)
            else:
                row.append(0.75 if i < 1024 - (f - 128) * 7 else -0.75)
        out.append(row)
    return out


def blofeld_syx(path, slot, name):
    frames = []
    for w in range(64):
        s = [round(900000 * math.sin(2 * math.pi * i / 128 + w * 0.05) * (1 - w / 80)
                   + 200000 * math.sin(2 * math.pi * (1 + w % 7) * i / 128)) for i in range(128)]
        f = bytearray([0xF0, 0x3E, 0x13, 0x00, 0x12, slot, w, 0x00])   # (byte 7: the format, 0)
        for v in s:
            u = v & 0x1FFFFF
            f += bytes([(u >> 14) & 0x7F, (u >> 7) & 0x7F, u & 0x7F])     # 8..391
        f += name.encode().ljust(14)[:14] + b"\0\0"                       # 392..405, 406..407
        f += bytes([sum(f[7:408]) & 0x7F, 0xF7])                          # the checksum (408), F7
        assert len(f) == 410
        frames.append(bytes(f))
    path.write_bytes(b"".join(frames))


def table(waves):
    """as gen_uwt.header + wt_mip: scaled to PEAK, then the levels, wave by wave"""
    peak = max(1, max(abs(v) for w in waves for v in w))
    per = [wt_mip.rows([int(round(v * PEAK / peak)) for v in w]) for w in waves]
    flat = [v for lv in range(len(wt_mip.LEVELS)) for w in range(64) for v in per[w][lv]]
    assert len(flat) == wt_mip.WORDS
    return struct.pack(f"<{len(flat)}h", *flat)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    BINS.mkdir(parents=True, exist_ok=True)
    inputs = {
        "morph16.wav": lambda p: wav(p, morph(256, 128), 16, clm=256),          # FFT, more frames than waves (DP)
        "sweep24.wav": lambda p: wav(p, sweep(512, 32), 24, clm=512),           # FFT, fewer frames than waves
        "odd-f32.wav": lambda p: wav(p, morph(200, 80), "f32", clm=200),        # a plain DFT (200 is no power of 2)
        "plain32.wav": lambda p: wav(p, sweep(2048, 3), 32),                    # no clm chunk: 2048 a cycle
        "blofeld.syx": lambda p: blofeld_syx(p, 80, "Fixture Wave"),
    }
    inputs["serum256.wav"] = lambda p: wav(p, serum_like(), "f32", clm=2048)   # (drawn, not in git: BINS)
    expected = {}
    for name, make in inputs.items():
        p = (BINS if name == "serum256.wav" else OUT) / name
        make(p)
        if name.endswith(".syx"):
            (slot, nm, waves), = gen_uwt.parse(p.read_bytes())
            frames, nm = None, gen_uwt.safe_name(nm)
        else:
            waves, _, frames = wt_import.load(p, PEAK)
            nm = gen_uwt.safe_name(p.stem)
        t = table(waves)
        (BINS / (name + ".bin")).write_bytes(t)
        expected[name] = {"crc": zlib.crc32(t), "frames": frames, "name": nm, "first": list(struct.unpack("<8h", t[:16]))}
        print(f"{name}: {len(t)} B, crc {zlib.crc32(t):08x}, frames {frames if frames is None else len(frames)}")
    (OUT / "expected.json").write_text(json.dumps(expected, indent=1) + "\n")


if __name__ == "__main__":
    main()
