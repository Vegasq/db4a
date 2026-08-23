#!/usr/bin/env bash
# Native overrides must be exactly equivalent to the cartridge code they replace.
#
# Two separate claims, because they can fail independently:
#
#   1. Per call, the C computes the same thing. DB4A_NATIVE=check runs both
#      implementations on every invocation and diffs the whole of RAM, all
#      registers, the flags, the cycle count and the exit PC. This is the claim
#      that the transliteration is right.
#
#   2. A faithful run does not take the override at all, so nothing about the
#      default build changed. Overrides collapse several blocks into one
#      indivisible step, which moves where the 68000 yields to the Z80 -- see
#      the comment on native_active() in src/cursor.c. That is why the default
#      is gated on mouse control rather than simply switched on.
#
# The first version of the checker compared RAM, cycles and the exit PC only.
# It passed on all 9319 calls while the run still diverged, because the
# override was leaving d0-d2 and the X flag holding the caller's values.
# Comparing registers is not optional.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
REC=data/recordings/level1atredis.txt
FRAMES=${FRAMES:-12000}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

echo "-- per-call equivalence, $FRAMES frames of the recorded mission"
DB4A_NATIVE=check DB4A_REPLAY=$REC ./build/db4a "$ROM" "$FRAMES" >/dev/null 2>"$TMP/err"
line=$(grep -o '\[native\] [0-9]* calls checked, [0-9]* mismatched' "$TMP/err" | tail -1 || true)
if [ -z "$line" ]; then echo "  FAIL: the override never ran"; exit 1; fi
calls=$(echo "$line" | awk '{print $2}')
bad=$(echo   "$line" | awk '{print $5}')
echo "  $calls calls, $bad mismatched"
if [ "$calls" -lt 1000 ]; then echo "  FAIL: only $calls calls, expected thousands"; exit 1; fi
if [ "$bad" != "0" ]; then grep '\[native\]' "$TMP/err" | head -8; exit 1; fi

echo "-- a faithful run is unchanged by the override existing"
for mode in default off; do
    env $([ $mode = off ] && echo DB4A_NATIVE=0) DB4A_REPLAY=$REC \
        DB4A_SHOTS=6000,12000 DB4A_PPM="$TMP/$mode" ./build/db4a "$ROM" 12100 >/dev/null 2>&1
done
for f in 6000 12000; do
    if ! cmp -s "$TMP/default.$f.ppm" "$TMP/off.$f.ppm"; then
        echo "  FAIL: frame $f differs with the override compiled in"; exit 1
    fi
    echo "  frame $f identical"
done
echo "native overrides verified"
