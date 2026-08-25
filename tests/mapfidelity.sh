#!/usr/bin/env bash
# What the map renderer computes must match what the cartridge actually drew.
#
# The margin is drawn from the game's own map data rather than from the
# tilemap. The way to know that is right is to point it at tiles the cartridge
# HAS drawn -- its own 320x224 -- and compare every one.
#
# This exists because make check-res did not catch a real regression. It
# compares one frame at each size; a fill that lapped the 64-column nametable
# ring and overwrote columns the cartridge had drawn left that frame intact
# while corrupting 1.12% of the tiles across a whole recording. A per-frame
# whole-screen comparison catches it; a single frame does not.
#
# The baseline is 0.03-0.09%, which is the cartridge writing a little of its
# own over the tilemap -- buildings and effects the map does not describe.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
LIMIT=${LIMIT:-30}          # hundredths of a percent

fail=0
check() {   # recording frames width height
    local rec=$1 fr=$2 w=$3 h=$4
    local out
    out=$(DB4A_MAPCHECK=1 DB4A_RENDER_ALL=1 DB4A_WIDE="$w" DB4A_TALL="$h" \
          DB4A_REPLAY="data/recordings/$rec.txt" ./build/db4a "$ROM" "$fr" 2>&1 >/dev/null \
          | grep -oE 'tiles=[0-9]+  mismatched=[0-9]+ \([0-9.]+%\)' || true)
    [ -n "$out" ] || { printf "  %-16s %4sx%-4s  no tiles compared -- FAIL\n" "$rec" "$w" "$h"; fail=1; return; }
    local pct hundredths
    pct=$(echo "$out" | grep -oE '\([0-9.]+%\)' | tr -d '(%)')
    hundredths=$(python3 -c "print(int(round(float('$pct')*100)))")
    if [ "$hundredths" -le "$LIMIT" ]; then
        printf "  %-16s %4sx%-4s  %s  ok\n" "$rec" "$w" "$h" "$out"
    else
        printf "  %-16s %4sx%-4s  %s  FAIL (limit %s.%02d%%)\n" "$rec" "$w" "$h" "$out" \
               $((LIMIT/100)) $((LIMIT%100)); fail=1
    fi
}

# Sizes we actually claim. Past ~448 wide the camera's western limit, which we
# hold a view-width short of the map edge so the margin always has map to show,
# starts pushing the camera into ground the map does not describe -- at 1024 it
# pins the camera outright (XMIN == XMAX == 1216) and the view extends past the
# map, where mapview reads beyond the row and the cartridge draws backdrop.
# Measured on artifacts.txt: 0.09% at 400 and 448, then 2.96% at 496, 4.13% at
# 512, 19.89% at 1024. See the task notes; the cap is not yet earned.
# 320x224 is skipped deliberately: with no margin there is nothing for the map
# renderer to draw, so it never runs and there are no tiles to compare.
for size in "352 224" "400 224" "400 256" "448 256"; do
    set -- $size
    check wide      4520 "$1" "$2"
    check artifacts 4180 "$1" "$2"
done
check cursor 5000 400 224

[ $fail -eq 0 ] || { echo "map fidelity: FAIL"; exit 1; }
echo "map fidelity: the map renderer agrees with the cartridge at every size"
