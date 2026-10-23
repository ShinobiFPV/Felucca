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

echo "== the two OPN2 cores' cost on this host (Phase 1; the device figures come from FELUCCA_CHIPBENCH=1)"
$CC tests/chipbench_host.c -o "$OUT/chipbench"
"$OUT/chipbench"
echo "SHINTECH HOST TESTS PASSED"
