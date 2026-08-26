#!/usr/bin/env bash
# Start must skip the opening sequence and land on the title menu.
#
# The skip is a fast-forward, so the interesting claims are not about speed:
# they are that it stops in the right place, that the frames really ran, and
# that it stays out of the way once there is nothing left to skip.
#
# DB4A_SKIP_AT=<frame> is the headless stand-in for the key press.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
TMP=$(mktemp -d build/tmp.XXXXXX)   # relative: db4a is native on Windows, /tmp is not
trap 'rm -rf "$TMP"' EXIT
fail=0

# Fixture. An unattended boot first runs the title menu's input loop on frame
# 2164; asking for the skip on frame 1 must land on exactly that frame, because
# it simulates the same frames the player would have sat through.
LAND=2164

out=$(DB4A_SKIP_AT=1 DB4A_SHOTS=$LAND DB4A_PPM="$TMP/skip" ./build/db4a "$ROM" $((LAND + 40)) 2>&1)
if grep -q "the title menu is up at frame $LAND" <<<"$out"; then
    echo "  skip from frame 1 lands on the title menu at $LAND ok"
else
    echo "  skip from frame 1 lands on the title menu at $LAND FAIL"
    grep -E "skipped|gave up" <<<"$out" || true
    fail=1
fi

# Nothing was bypassed: the landed frame must be the frame an unattended boot
# draws at that same number, pixel for pixel. This is what separates a
# fast-forward from a jump -- poking the game forward would not match.
DB4A_SHOTS=$LAND DB4A_PPM="$TMP/wait" ./build/db4a "$ROM" $((LAND + 40)) >/dev/null 2>&1
if cmp -s "$TMP/skip.$LAND.ppm" "$TMP/wait.$LAND.ppm"; then
    echo "  landed frame identical to the unattended run        ok"
else
    echo "  landed frame identical to the unattended run        FAIL"
    fail=1
fi

# Once the menu is up there is nothing to skip, so Start belongs to the game
# again and the request must do nothing at all.
out=$(DB4A_SKIP_AT=$((LAND + 100)) ./build/db4a "$ROM" $((LAND + 200)) 2>&1)
if grep -q "skipping the intro" <<<"$out"; then
    echo "  a request after the menu is up is refused           FAIL"
    fail=1
else
    echo "  a request after the menu is up is refused           ok"
fi

# The setting turns it off.
out=$(DB4A_SKIPINTRO=0 DB4A_SKIP_AT=1 ./build/db4a "$ROM" 300 2>&1)
if grep -q "skipping the intro" <<<"$out"; then
    echo "  skipintro = 0 sits through the intro                FAIL"
    fail=1
else
    echo "  skipintro = 0 sits through the intro                ok"
fi

[ $fail -eq 0 ] || exit 1
