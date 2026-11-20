#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run firmware/src/volna.c on this computer (tests/host.c): build/host/dinghy.wav and the screens as PNG.
#   tests/run_host.sh [script]               your own script (tests/host.c says what a line can do)
#   BOARD=smk37 tests/run_host.sh [script]   the SMK-37 build (velocity, pads, faders, wheels, outer keys)
#   VOLNA_CFLAGS=-DVOLNA_INIT1 ...           INIT as it was before 2026-11-16 (the test scenarios: tests/run_tests.sh)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
N=volna; D=
if [ "${BOARD:-fm1}" = smk37 ]; then N=volna-smk37; D=-DBOARD_SMK37=1; fi
python3 tools/gen_uwt.py >/dev/null   # firmware/gen/wt_local.h: the local wavetables, or a stub
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST $D ${VOLNA_CFLAGS:-} -DAPP_VER="\"${APP_VER:-dev}\"" -Ifirmware/hal -Ifirmware/src -Ifirmware/gen -o build/host/$N firmware/src/volna.c
build/host/$N build/host/$N ${1:+"$1"}
python3 - "$N" <<'PY'
import sys
from pathlib import Path
from PIL import Image
for p in sorted(Path("build/host").glob(sys.argv[1] + "-*.ppm")):
    Image.open(p).resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    p.unlink()
    print("screen ->", p.with_suffix(".png"))
PY
