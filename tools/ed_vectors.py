#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Volna's editor protocol (docs/editor-protocol.md): the frames, for the examples in the doc and the tests.

    python3 tools/ed_vectors.py            the example frames
    python3 tools/ed_vectors.py HEX...     decode frames (F0 .. F7)
"""
import sys

TAG = bytes([0x7D, 0x56, 0x4C])


def pack7(data):
    out, acc, nb = bytearray(), 0, 0
    for b in data:
        acc |= b << nb
        nb += 8
        while nb >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            nb -= 7
    if nb:
        out.append(acc & 0x7F)
    return bytes(out)


def unpack7(s):
    out, acc, nb = bytearray(), 0, 0
    for b in s:
        acc |= b << nb
        nb += 7
        while nb >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nb -= 8
    return bytes(out)


def frame(cmd, seq, payload=b""):
    body = bytes([cmd, seq]) + bytes(payload)
    return b"\xF0" + TAG + pack7(body + bytes([~sum(body) & 0xFF])) + b"\xF7"


def parse(msg):
    """(cmd, seq, payload) or None"""
    if len(msg) < 6 or msg[0] != 0xF0 or msg[-1] != 0xF7 or msg[1:4] != TAG:
        return None
    d = unpack7(msg[4:-1])          # (m 7-bit bytes of n packed ones give back exactly n: m = ceil(8n / 7))
    if len(d) < 3 or len(pack7(d)) != len(msg) - 5 or (~sum(d[:-1]) & 0xFF) != d[-1]:
        return None
    return d[0], d[1], d[2:-1]


EXAMPLES = [
    ("INFO, seq 1", frame(0x01, 1)),
    ("NAMES 0..19, seq 2", frame(0x02, 2, [0, 20])),
    ("PATCH GET 7F, seq 3", frame(0x10, 3, [0x7F])),
    ("reply: PATCH PUT 05 done, seq 4", frame(0x91, 4, [0, 5])),
]

if __name__ == "__main__":
    if len(sys.argv) > 1:
        for h in sys.argv[1:]:
            print(h, "->", parse(bytes.fromhex(h)))
    else:
        for what, f in EXAMPLES:
            print(f"| {what} | `{' '.join(f'{b:02X}' for b in f)}` |")
            assert parse(f) is not None
