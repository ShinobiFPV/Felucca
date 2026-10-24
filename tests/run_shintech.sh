#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 William Kew, ShinTech Electronics
# The ShinTech fork's own host tests (the chip cores; upstream's suite is tests/run_tests.sh).
#   tests/run_shintech.sh               the OPN2 port against ymfm (needs YMFM, a ymfm checkout; skipped without),
#                                       and the host benchmark of the two cores
#   YMFM=~/src/ymfm tests/run_shintech.sh
set -e
cd "$(dirname "$0")/.."
OUT=build/shintech/host
mkdir -p "$OUT"
CC="${CC:-cc} -O2 -std=gnu99 -Wall -Wno-unused-function"
CXX="${CXX:-c++} -O2 -std=c++17 -w"

echo "== OPN2 port (firmware/src/chips/opn2.c) against ymfm, sample for sample"
if [ -n "$YMFM" ] && [ -f "$YMFM/src/ymfm_opn.cpp" ]; then
    $CC -c tests/opn2_host.c -o "$OUT/opn2_host.o"
    for f in ymfm_opn ymfm_adpcm ymfm_ssg; do
        $CXX -I"$YMFM/src" -c "$YMFM/src/$f.cpp" -o "$OUT/$f.o"
    done
    $CXX -I"$YMFM/src" tests/opn2_ymfm_test.cpp "$OUT/opn2_host.o" "$OUT/ymfm_opn.o" "$OUT/ymfm_adpcm.o" \
        "$OUT/ymfm_ssg.o" -o "$OUT/opn2_ymfm_test"
    "$OUT/opn2_ymfm_test"
else
    echo "skip (set YMFM to a ymfm checkout: git clone https://github.com/aaronsgiles/ymfm)"
fi

echo "== the OPN2 engine (firmware/src/eng_opn2.c): frequency, registers, sound, CHIP LIMIT, voices, velocity, Legacy"
[ -f build/gen/felucca_tables.h ] || { echo "run ./build.sh once first (build/gen)"; exit 1; }
mkdir -p build/shintech/opn2_demo
$CC -w -Ibuild/gen -Ifirmware/src -o "$OUT/opn2_engine_test" tests/opn2_engine_test.c -lm
"$OUT/opn2_engine_test" build/shintech/opn2_demo
$CC -w -DOP2_ONE_CORE_CAP=4 -Ibuild/gen -Ifirmware/src -o "$OUT/opn2_cap_test" tests/opn2_engine_test.c -lm
"$OUT/opn2_cap_test"

echo "== the PSG engine (firmware/src/eng_psg.c, chips/psg.c): the chip, dividers, LFSRs, shapes, writes, sound, CHIP LIMIT, macros"
mkdir -p build/shintech/psg_demo
$CC -w -Ibuild/gen -Ifirmware/src -o "$OUT/psg_engine_test" tests/psg_engine_test.c -lm
"$OUT/psg_engine_test" build/shintech/psg_demo

echo "== the PCM8 engine (firmware/src/eng_pcm8.c): the DAC, the kits, the slices, modes, a source change, sound"
mkdir -p build/shintech/pcm8_demo
$CC -w -Ibuild/gen -Ifirmware/src -o "$OUT/pcm8_engine_test" tests/pcm8_engine_test.c -lm
"$OUT/pcm8_engine_test" build/shintech/pcm8_demo

echo "== OPN2 patches in projects (FUN11) and user presets (version 6)"
$CC -O1 -w -Ibuild/gen -Ifirmware/src -Itests -o "$OUT/opn2_persist_test" tests/opn2_persist_test.c -lm
"$OUT/opn2_persist_test"

echo "== the Akai MPK mini IV by default (ROUT SEL, channel 10 the DRUM track, CC 24..31 relative: the macros)"
$CC -O1 -w -Ibuild/gen -Ifirmware/src -Itests -o "$OUT/mpk_test" tests/mpk_test.c -lm
"$OUT/mpk_test"

echo "== the two OPN2 cores' cost on this host (Phase 1; the device figures come from FELUCCA_CHIPBENCH=1)"
$CC tests/chipbench_host.c -o "$OUT/chipbench"
"$OUT/chipbench"
echo "SHINTECH HOST TESTS PASSED"
