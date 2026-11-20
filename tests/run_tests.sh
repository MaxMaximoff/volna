#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Dinghy's host tests (no hardware). Run from the repo root after building, the SMK-37 first, the FM-1 last
# (both boards write the same build/ intermediates; the tests expect the FM-1 binaries):
#   BOARD=smk37 sh build.sh; BOARD=smk37 MODEL=elite sh build.sh; BOARD=smk37 MODEL=mke sh build.sh; sh build.sh
#   sh tests/run_tests.sh
# The FM-1 app on the host (tests/run_host.sh), MIDI and the SMK-37 on the host (tests/scripts), the flash map, the
# update path (ota.c, the loader, tools/fm1_install.py; from the fm1-to-smk37 kit's reference layer,
# genkigenki/sloop-fm1 smk37) and the packages.
set -e
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"   # (fm1pkg_make.py: the SDK files of a package)
cd "$(dirname "$0")/.."
OUT=build/host
mkdir -p "$OUT"
CC="${CC:-cc} -O1 -Wall -Wno-unused-function"
# the scenarios were written on the first INIT: they keep it (VOLNA_INIT1); tests/init_check.py hears today's
export VOLNA_CFLAGS="${VOLNA_CFLAGS--DVOLNA_INIT1}"
fail=0
run() { n="$1"; echo "== $n"; shift; "$@" || { fail=1; echo "FAILED: $n"; }; }

[ -f build/volna.fwsc ] || { echo "run ./build.sh first"; exit 1; }

run "FM-1 app on the host: the default script plays, screens and sound" sh tests/run_host.sh
run "SMK-37 app on the host: the default script" env BOARD=smk37 sh tests/run_host.sh
run "FM-1 MIDI: USB, the TRS jack, a clock, the channel" sh tests/run_host.sh tests/scripts/midi.txt
run "SMK-37: velocity, a pad, an outer key, the pitch wheel" env BOARD=smk37 sh tests/run_host.sh tests/scripts/smk37.txt

# Volna
$CC -Ifirmware/hal -Ifirmware/src -Ifirmware/gen -o "$OUT/pattern_library_test" tests/pattern_library_test.c
run "Volna: independent patterns, legacy migration and library recovery" "$OUT/pattern_library_test"
run "Volna: pattern protocol, patch SAVE and explicit library writes" python3 tests/pattern_protocol_check.py
run "Volna: the generated wavetables (own, with mip-maps) are what tools/gen_tables.py makes" python3 tools/gen_tables.py --check
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/wt_osc_test" tests/wt_osc_test.c -lm
run "Volna: the wavetable oscillator (sine at wave 0, a sweep without steps, pitch offsets)" "$OUT/wt_osc_test"
run "Volna: three oscillators, SELECT and KNOB1..4" sh tests/run_host.sh tests/scripts/osc3.txt
run "Volna: the generated envelope tables are what tools/gen_env.py makes" python3 tools/gen_env.py --check
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/env_test" tests/env_test.c -lm
run "Volna: the envelopes (attack times, sustain, release to idle, retrigger without a jump; the modes)" "$OUT/env_test"
run "Volna: 4 voices with the envelope, a stolen voice, the ENV and MIX pages" sh tests/run_host.sh tests/scripts/stage2.txt
run "Volna: the generated filter tables are what tools/gen_svf.py makes" python3 tools/gen_svf.py --check
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/svf_test" tests/svf_test.c -lm
run "Volna: the state-variable filter (coefficients, response, top resonance, no blow-ups, drive, the screen's curve, the 24 dB types)" "$OUT/svf_test"
run "Volna: the filters under a chord, a cutoff sweep, row B, filter 2, the filter envelope" sh tests/run_host.sh tests/scripts/filter.txt
run "Volna: the 24 dB filter types heard (LP24 darker than LP12, HP24 thinner than HP12)" python3 tests/filter24_check.py
run "Volna: NOISE, COLOR, RING and BAL are heard (the default filter, an open one, O1 past a shut filter)" sh tests/run_host.sh tests/scripts/mix.txt
run "Volna: VELO (ENV row B): AMP 63 / 0 / -63, FLT 63, soft and hard MIDI notes" sh tests/run_host.sh tests/scripts/velo.txt
run "Volna: SETUP CURVE (LIN SOFT HARD FIX) and KEYVEL for the FM-1 keys" sh tests/run_host.sh tests/scripts/curve.txt
python3 tools/gen_uwt.py >/dev/null
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/wt_local_test" tests/wt_local_test.c
run "Volna: the local wavetables, if local/wavetables has them (guards, peaks, no overshoot)" "$OUT/wt_local_test"
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/mip_test" tests/mip_test.c -lm
run "Volna: the mip-maps (layout, every level band-limited, a saw aliases 13-24 dB less on high notes)" "$OUT/mip_test"
run "Volna: TABLE on OSC (KNOB1), a held note through the tables" sh tests/run_host.sh tests/scripts/tables.txt
run "Volna: without the local tables (the stub): the host build and the table script still work" env VOLNA_LOCAL_WT=0 sh tests/run_host.sh tests/scripts/tables.txt
python3 tools/gen_uwt.py >/dev/null                 # (back to what local/ has)
$CC -Ifirmware/src -o "$OUT/arp_test" tests/arp_test.c
run "Volna: the arpeggiator engine (orders, directions, octaves, timing, gate, swing, modes, velocity, patterns, no hanging notes)" "$OUT/arp_test"
run "Volna: the arpeggiator in the firmware (RUN, a chord, tempo and rate, HOLD, MIDI clock, off)" sh tests/run_host.sh tests/scripts/arp.txt
run "Volna: its steps counted in the sound" python3 tests/arp_check.py
run "Volna: the arp's patterns (PATTERN 2, STEPS: a white key switches a step on, TYPE chord)" sh tests/run_host.sh tests/scripts/steps.txt
run "Volna: the pattern's steps counted in the sound" python3 tests/arp_check.py build/host/volna.wav "pattern 2, 4 a second:1.55:2.95:4" "step 2 on, 4.5 a second:3.3:5.45:4.5" "let go, silent:5.6:6.4:0"
run "Volna: ties in an arp pattern (LENGTH past +3: TIE; one, then a chain)" sh tests/run_host.sh tests/scripts/tie.txt
run "Volna: the tied steps counted in the sound" python3 tests/arp_check.py build/host/volna.wav "straight, 8 a second:1.55:2.95:8" "step 1 tied, 7.5 a second:3.3:4.95:7.5" "steps 1 3 4 tied, 6.5 a second:5.6:7.45:6.5"
run "Volna: the generated LFO table is what tools/gen_lfo.py makes" python3 tools/gen_lfo.py --check
run "Volna: the screen fonts (Barlow, trimmed) are what tools/gen_font.py makes" python3 tools/gen_font.py --check
run "Volna: the generated distortion tables are what tools/gen_dist.py makes" python3 tools/gen_dist.py --check
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/lfo_test" tests/lfo_test.c -lm
run "Volna: the LFOs (rates, sync, shapes, S&H, RAND, KEY; the Hz shown) and the matrix's pitch and scales" "$OUT/lfo_test"
run "Volna: the matrix in the firmware (LFO 1 -> PITCH, CUTOFF, WAVE; the MOD pages)" sh tests/run_host.sh tests/scripts/mod.txt
run "Volna: the modulation heard (two octaves, the brightness, the table kept whole)" python3 tests/mod_check.py
run "Volna: the envelopes' modes in the firmware (LOOP S1S2 held, ONE SHOT and ADSR tapped)" sh tests/run_host.sh tests/scripts/envmode.txt
run "Volna: the modes heard (the loop swings, the one shot outlasts its tap)" python3 tests/envmode_check.py
run "Volna: the patches (PRESETS, an edit marked, SAVE with a name to another number, RECALL; the factory's in range)" sh tests/run_host.sh tests/scripts/patches.txt
run "Volna: a SAVE waiting for silence writes the sound as it was, another patch loaded meanwhile (or its very number)" python3 tests/savewait_check.py
run "Volna: the patch's own pattern (03 BASS-1's groove; COPY, SEQ on OWN, saved with the patch, the bank untouched)" python3 tests/own_check.py
run "Volna: the envelopes' A D S R as destinations (VELO -> A.ATK)" sh tests/run_host.sh tests/scripts/envmod.txt
run "Volna: a hard note's attack slower than a soft one's" python3 tests/envmod_check.py
run "Volna: all 16 envelope destinations heard (CC 1 at 0 and 127, a note each)" python3 tests/envdst_check.py
run "Volna: the newer destinations heard (DET1, COLOR, BAL1, F1.DRV, F1.ENV, PAN)" python3 tests/newdst_check.py
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/modf_test" tests/modf_test.c -lm
run "Volna: the modifiers' arithmetic (+ - * AND OR XOR MAX MIN, the constant) and the LFOs' speed modulation" "$OUT/modf_test"
run "Volna: the matrix's newer sources and destinations and the modifiers heard (PRESS, P.PRESS, BEND, CTL W, MOD1, L1.SPD, AMT1)" python3 tests/modmatrix_check.py
run "Volna: the modifiers and the newer sources on the screen (MATRIX, MODIF, SETUP row C)" sh tests/run_host.sh tests/scripts/modf.txt
run "Volna: FM heard (J0 and J1 at AMT 30 and 70, the lines in place, FM EP's strike, little folding back at A6)" python3 tests/fm_check.py
run "Volna: FM on the screen (OSC row C, FM SRC's list, ENV 3 > FM1)" sh tests/run_host.sh tests/scripts/fm.txt
run "Volna: hard sync heard (the master's pitch, little folding back at A4 and A6 with the polyBLEP, SYNC LEAD's wheel)" python3 tests/sync_check.py
run "Volna: hard sync on the screen (OSC2's row C, OSC1's without SYNC)" sh tests/run_host.sh tests/scripts/sync.txt
run "Volna: the Blofeld's filter types heard on noise (COMB+ and COMB- peaks, NOTCH24 wider, PPG LP's peak)" python3 tests/ftypes_check.py
run "Volna: the filter types on the screen (NOTCH24, NOTCH12, COMB+, COMB-, PPG LP)" sh tests/run_host.sh tests/scripts/ftypes.txt
run "Volna: the filters' DRIVE curves heard on a sine (odd or even harmonics, each its own tone, no DC)" python3 tests/dcurve_check.py
run "Volna: the DRIVE curves on the screen (FILTER row C: the list, SOFT, RECTIFY, SINE, OSC1 MOD, DRIVE 0)" sh tests/run_host.sh tests/scripts/dcurve.txt
run "Volna: Filter FM and the filters' PAN heard (another tone, still periodic; PAR and SER panned all the way)" python3 tests/ffm_check.py
run "Volna: Filter FM and PAN on the screen (FILTER row C, the matrix's F1.FM .. F2.PAN)" sh tests/run_host.sh tests/scripts/ffm.txt
run "Volna: the smaller things heard (GLISS, FINGERED, KEYTRK, an oscillator's BEND, LFO FADE OUT, LIMIT WT)" python3 tests/small_check.py
run "Volna: the smaller things on the screen (KEYTRK, BEND, LIMIT, FADE, G.MODE)" sh tests/run_host.sh tests/scripts/small.txt
run "Volna: the envelopes' SINGLE trigger heard (MONO: NORMAL attacks again, SINGLE goes on)" python3 tests/trig_check.py
run "Volna: TRIPLE heard (clean, the drive's odd harmonics, S&H's images, the chorus's sides)" python3 tests/triple_check.py
run "Volna: TRIPLE on the screen (its rows, S&H and the drive drawn)" sh tests/run_host.sh tests/scripts/triple.txt
run "Volna: a patch's category on the screen (KEYS, LEAD on the card; the list)" sh tests/run_host.sh tests/scripts/pcat.txt
run "Volna: SHIFT + SELECT pages the rows (OSC, MOD)" sh tests/run_host.sh tests/scripts/rows.txt
run "Volna: INIT undone (SHIFT + the key again, nothing changed since)" sh tests/run_host.sh tests/scripts/undo.txt
run "Volna: a peek (a section's key held, let go: back)" sh tests/run_host.sh tests/scripts/peek.txt
run "Volna: MOD held + a knob: a matrix slot onto it" sh tests/run_host.sh tests/scripts/massign.txt
run "Volna: the battery on SETUP (3 2 1 0 bars, USB)" sh tests/run_host.sh tests/scripts/batt.txt
run "Volna: GLIDE, MODE (POLY MONO LEGATO), BEND on the PATCH page, notes and pitch bend over MIDI" sh tests/run_host.sh tests/scripts/glide.txt
run "Volna: the glides and the bend heard (the pitch along the way)" python3 tests/glide_check.py
$CC -Ifirmware/src -o "$OUT/poly_test" tests/poly_test.c
run "Volna: the voices' limit's decision (a heavy sound: 3 voices; back to 4; no flapping)" "$OUT/poly_test"
run "Volna: the voices' limit in the firmware (a voice dropped at 2300 ms: the oldest held goes)" env VOLNA_CFLAGS=-DVOLNA_POLY_DROP_MS=2300 sh tests/run_host.sh tests/scripts/poly.txt
run "Volna: the voices' limit heard (C4 taken at once, D5 steals a voice past the limit)" python3 tests/poly_check.py
run "Volna: slides in an arp pattern (a saw built from INIT, pattern 14, MONO then POLY)" sh tests/run_host.sh tests/scripts/acid.txt
run "Volna: a slide starts no note (9 attacks in 16 steps, not 12)" python3 tests/arp_check.py build/host/volna.wav "MONO, 3 slides in 16 steps:1.6:5.4:4.72" "POLY, the same:6.6:10.4:4.72" "let go, silent:10.9:11:0"
run "Volna: unison (OSC row B: UNISON DETUNE SPREAD), a sine, both channels" sh tests/run_host.sh tests/scripts/unison.txt
run "Volna: the copies heard (detuned +-100 cents, panned apart, mono at SPREAD 0, the level kept)" python3 tests/uni_check.py
run "Volna: INIT as it is (2026-11-16): OSC1 alone on a sine, OSC2 and OSC3 silent" env VOLNA_CFLAGS= sh tests/run_host.sh tests/scripts/init.txt
run "Volna: INIT's note is one sine at 220 Hz, left = right" python3 tests/init_check.py
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/fx_test" tests/fx_test.c -lm
run "Volna: the effects engine (DELAY: time, feedback, PING, a gliding time, bounded; REVERB: stereo, SIZE, bounded, dies out)" "$OUT/fx_test"
run "Volna: the effects in the firmware (FX: DELAY, synced with PING, REVERB in slot 2, the two in a row)" sh tests/run_host.sh tests/scripts/fx.txt
run "Volna: the effects heard (echo time and feedback, ping-pong sides, a stereo tail that ends)" python3 tests/fx_check.py
run "Volna: DIST in the firmware (a sine dry, through SOFT, through TUBE)" sh tests/run_host.sh tests/scripts/dist.txt
run "Volna: DIST heard (SOFT: odd harmonics, about as loud; TUBE: the 2nd)" python3 tests/dist_check.py
run "Volna: CHORUS in the firmware (a sine dry, CHORUS, FLANGER with feedback)" sh tests/run_host.sh tests/scripts/chorus.txt
run "Volna: CHORUS heard (the copy beats against the dry, the sides apart; the flanger bounded)" python3 tests/chorus_check.py
run "Volna: the generated phaser tables are what tools/gen_phaser.py makes" python3 tools/gen_phaser.py --check
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/phaser_test" tests/phaser_test.c -lm
run "Volna: PHASER's engine (allpass, notches where theory has them, feedback, a sweep without steps, WIDTH, bounded, every tail ends at 0)" "$OUT/phaser_test"
run "Volna: PHASER in the firmware (a sine dry, the defaults, FEEDBK 0.95 and 8 stages)" sh tests/run_host.sh tests/scripts/phaser.txt
run "Volna: PHASER heard (the level swings, the sides apart, no clicks, bounded)" python3 tests/phaser_check.py
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/ensemble_test" tests/ensemble_test.c -lm
run "Volna: ENSEMBLE's engine (still taps, the Catmull-Rom read, the bass kept, it moves without steps, WIDTH, bounded, the tail ends)" "$OUT/ensemble_test"
run "Volna: ENSEMBLE in the firmware (C6 and C2 sines dry, then through it)" sh tests/run_host.sh tests/scripts/ensemble.txt
run "Volna: ENSEMBLE heard (C6 beats, the sides apart, no clicks; C2 as loud as dry)" python3 tests/ensemble_check.py
run "Volna: the generated LO-FI tables are what tools/gen_lofi.py makes" python3 tools/gen_lofi.py --check
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/lofi_test" tests/lofi_test.c -lm
run "Volna: LO-FI's engine (at rest, SNR by bits, no DC, the hold's droop and image, wobble, slewed, noise, silence)" "$OUT/lofi_test"
run "Volna: LO-FI in the firmware (a sine dry, the defaults, WOBBLE 0, RATE 2.4 kHz)" sh tests/run_host.sh tests/scripts/lofi.txt
run "Volna: LO-FI heard (as loud, it wobbles, the dirt, the image, silence after)" python3 tests/lofi_check.py
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/filter_test" tests/filter_test.c -lm
run "Volna: FILTER's engine (still as the filter in doubles, a fast sweep without the zipper, ENV in dB, STEREO, bounded, every tail ends)" "$OUT/filter_test"
run "Volna: FILTER in the firmware (noise dry, an LFO sweep, ENV with a hard and a soft note)" sh tests/run_host.sh tests/scripts/fxfilter.txt
run "Volna: FILTER heard (the brightness swings, the sides the same, the hard note brighter, silence after)" python3 tests/fxfilter_check.py
run "Volna: the generated EQ tables are what tools/gen_eq.py makes" python3 tools/gen_eq.py --check
$CC -Ifirmware/src -Ifirmware/gen -o "$OUT/eq_test" tests/eq_test.c -lm
run "Volna: the EQ engine (coefficients over the ranges, the bands' response against the analog, the screen's curve, bypass, bounded, the tail ends)" "$OUT/eq_test"
run "Volna: the EQ in the firmware (FX's third slot: MID -12 dB at a moved M.FREQ, SAVE and PRESETS)" sh tests/run_host.sh tests/scripts/eq.txt
run "Volna: the EQ heard (a quarter of dry at the bell, both sides; the same after the patch is loaded back)" python3 tests/eq_check.py
run "Volna: the mip-maps in the firmware (a sawtooth at C7 and C8, no filter)" sh tests/run_host.sh tests/scripts/mip.txt
run "Volna: the ANALOG table heard (a full sawtooth at A1, little aliasing at C8, the square, the narrow pulse)" python3 tests/analog_check.py
run "Volna: high notes band-limited (aliasing under -30 dB)" python3 tests/mip_check.py
run "Volna: the OSC picture follows the matrix (LFO 1 on WAV1, a note held)" sh tests/run_host.sh tests/scripts/wavemod.txt
run "Volna: the picture moves with the LFO, stands still in silence" python3 tests/wavemod_check.py
run "Volna: an LFO on WAV1 past the table's start (W9 +33: it stops at wave 0), the OSC and MATRIX pages" sh tests/run_host.sh tests/scripts/lfoclamp.txt
run "Volna: the stop at the table's end shown (VOICE line and the ruler's wall in red, MATRIX's reach)" python3 tests/lfoclamp_check.py
run "Volna: the overlay says what the firmware does (sections, buttons, colours; files up to date, no cut-outs too close)" python3 tests/panel_check.py
run "Volna: the screen ideas from the mockup (the value lens, the wave live, the cells' marks, the footer)" sh tests/run_host.sh tests/scripts/lens.txt
$CC -Ifirmware/src -o "$OUT/setbox_test" tests/setbox_test.c
run "Volna: the settings' box (older and newer layouts load, the rest at the defaults)" "$OUT/setbox_test"
$CC -Ifirmware/src -o "$OUT/bank_test" tests/bank_test.c
run "Volna: an older patch bank (192-byte records) spread to the new layout" "$OUT/bank_test"
run "Volna: the patches' format 1 (records and tails in blobs) moved to the format 2 (whole records) once, as saved" sh -c 'VOLNA_V1=1 build/host/volna build/host/migrate tests/scripts/migrate.txt | grep "all as saved, moved once"'
run "Volna: the matrix's 16th slot (LFO 1 -> PITCH), saved and loaded with the patch" sh tests/run_host.sh tests/scripts/slot16.txt
run "Volna: slot 16 heard, before and after SAVE" python3 tests/slot16_check.py
$CC -Ifirmware/src -o "$OUT/editor_test" tests/editor_test.c
run "Volna: the editor's frames (the doc's examples, round trips, refusals)" "$OUT/editor_test"
run "Volna: the editor protocol through the firmware (INFO, NAMES, GET, PUT into the sound and a number after silence, BUSY, refusals, an older record)" python3 tests/editor_check.py
run "Volna: the user wavetables (protocol 3: TABLE INFO BEGIN DATA END NAME CLEAR; a slot sounds as the table sent)" python3 tests/uwt_check.py
run "Volna: the factory sounds side by side (levels before the output's limiter)" python3 tests/levels_check.py
run "Volna: the sustain pedal, program change, the arp's MIDI out (heard, and what goes out)" python3 tests/midi2_check.py
run "Volna: the audio ISR's loops (a static cost estimate: app_audio under 4500 instructions)" python3 tools/isr_loops.py --max 4500
run "Volna: the interface, every page (docs/ui/UI.md)" sh tests/run_host.sh tests/scripts/ui.txt
run "Volna: KNOB2 (WAVE) sweeps the table under a held note" sh tests/run_host.sh tests/scripts/wave.txt

$CC -DBOARD_SMK37=1 -Ifirmware/hal -o "$OUT/smk_flash_test" tests/smk_flash_test.c
run "SMK-37 flash map: settings, blobs, owner tag, slots in the store; never the head, BTIF/USRTRIM, USR, key_mac" "$OUT/smk_flash_test"
$CC -o "$OUT/smk_wheel_test" tests/smk_wheel_test.c
run "SMK-37 pitch wheel: the bend is back to 0 at rest, however slowly the wheel was moved" "$OUT/smk_wheel_test"

$CC -o "$OUT/ota_test" tests/ota_test.c
run "M-UPGRADE entry (FM-1)" "$OUT/ota_test" build/volna.fwsc
head -c 60000 build/volna.bin > "$OUT/old_app.bin"
python3 tools/fm1pkg_make.py "$OUT/old_app.bin" build/loader/ota.bin "$OUT/old.fwsc" >/dev/null
$CC -o "$OUT/ldr_screen_test" tests/ldr_screen_test.c
run "update loader's screen (from Skiff): UPDATE MODE, WRITING and its bar, UPDATE FAILED" "$OUT/ldr_screen_test" "$OUT"
$CC -o "$OUT/ldr_test" tests/ldr_test.c
run "update loader (FM-1): other app -> this build" "$OUT/ldr_test" "$OUT/old.fwsc" build/volna.fwsc

# SMK-37: the OTA staging and the loader windows on SMK-layout packages (made here from the FM-1 binaries: only the
# layout and the protocol are under test), then the real SMK packages
python3 tools/fm1pkg_make.py --board smk37 build/volna.bin build/loader/ota.bin "$OUT/smk_new.fwsc" >/dev/null
python3 tools/fm1pkg_make.py --board smk37 "$OUT/old_app.bin" build/loader/ota.bin "$OUT/smk_old.fwsc" >/dev/null
run "SMK-37 package layout (fm1pkg_make --board smk37): PRCT 0x9D000, stock region table" \
    python3 tests/smk_pkg_test.py "$OUT/smk_new.fwsc" "SMK-37 Pro_900"
$CC -DBOARD_SMK37=1 -o "$OUT/ota_test_smk" tests/ota_test.c
run "M-UPGRADE entry, SMK-37 Pro (staging 0xBB000, parm ota-SMK-37 Pro_016)" "$OUT/ota_test_smk" "$OUT/smk_new.fwsc"
for m in 1 2; do
    $CC -DBOARD_SMK37=1 -DT_MODEL=$m -o "$OUT/ota_test_smk_m$m" tests/ota_test.c
    run "M-UPGRADE entry, SMK-37 model $m (1 Elite, 2 MKE-P37): identity and parm under its name" "$OUT/ota_test_smk_m$m" "$OUT/smk_new.fwsc"
done
$CC -DBOARD_SMK37=1 -o "$OUT/ldr_test_smk" tests/ldr_test.c
run "update loader, SMK-37: app area 0x4000-0x9CFFF, protected ranges untouched" "$OUT/ldr_test_smk" "$OUT/smk_old.fwsc" "$OUT/smk_new.fwsc"
found=0
for m in ":SMK-37 Pro_9" "-elite:SMK-37 Elite_9" "-mke:MKE-P37_9"; do
    for f in build/volna*-smk37${m%%:*}.fwsc; do
        [ -f "$f" ] || continue
        found=1
        v=46                                    # Volna: a fixed identity tail (tools/build.py IDENTITY_TAIL)
        run "SMK-37 package $f: layout, identity ${m#*:}$v, the loader marker" python3 tests/smk_pkg_test.py "$f" "${m#*:}$v"
    done
done
[ "$found" = 1 ] || echo "(no SMK-37 packages in build/: BOARD=smk37 sh build.sh first)"

run "installer (tools/fm1_install.py): FM-1 and SMK-37, install and the update-mode resume check the model" python3 tests/install_test.py

[ "$fail" = 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit "$fail"
