#!/usr/bin/env bash
# What we predict the cartridge's sprite table will contain must be what it
# does contain.
#
# The object layer is drawn from the game's object list rather than from the
# 80-entry table the cartridge builds. Before drawing anything from that
# reading, it has to reproduce the table exactly -- a predictor that cannot
# match the shadow has no business painting pixels. Every entry, every frame.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"

fail=0
check() {   # recording frames
    local out pct
    out=$(DB4A_OBJCHECK=1 DB4A_WIDE=400 DB4A_REPLAY="data/recordings/$1.txt" \
          ./build/db4a "$ROM" "$2" 2>&1 >/dev/null | grep -oE 'entries=[0-9]+ mismatched=[0-9]+ \([0-9.]+%\)' || true)
    [ -n "$out" ] || { printf "  %-16s no entries compared -- FAIL\n" "$1"; fail=1; return; }
    pct=$(echo "$out" | grep -oE '\([0-9.]+%\)' | tr -d '(%)')
    if [ "$pct" = "0.00" ]; then printf "  %-16s %s  ok\n" "$1" "$out"
    else printf "  %-16s %s  FAIL (must be exact)\n" "$1" "$out"; fail=1; fi
}

check wide           4520
check artifacts      4180
check cursor         5000
check fog            2750
check level1atredis 12010

[ $fail -eq 0 ] || { echo "objects: FAIL"; exit 1; }
echo "objects: the predicted sprite table matches the cartridge's exactly"
