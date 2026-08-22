#!/usr/bin/env bash
# Capture the same scripted moment from db4a and from the reference emulator,
# then diff them. Both sides run the identical input script, so any difference
# is ours.
#
#   tools/compare_screen.sh <scenario> <frame>
set -u
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
CORE="ref/gpgx/genesis_plus_gx_libretro.so"
SCEN=${1:-houseselect}
FRAME=${2:-2750}
OUT=build/compare
mkdir -p "$OUT"

mapfile -t PLAY < <(python3 tests/playthrough.py "$SCEN" --emit-press)
PRESS="${PLAY[0]}"

echo "scenario=$SCEN frame=$FRAME"
echo "inputs: $PRESS"

DB4A_PRESS="$PRESS" DB4A_HOLD=6 DB4A_SHOTS="$FRAME" DB4A_PPM="$OUT/mine" \
    ./build/db4a "$ROM" $((FRAME+50)) >/dev/null 2>&1
DB4A_PRESS="$PRESS" ./build/refhost "$CORE" "$ROM" "$FRAME" "$OUT/ref.ppm" >/dev/null 2>&1

MINE="$OUT/mine.$FRAME.ppm"
[ -f "$MINE" ] || { echo "db4a produced no frame $FRAME"; exit 1; }
[ -f "$OUT/ref.ppm" ] || { echo "reference produced no frame"; exit 1; }

python3 tools/ppm2png.py "$MINE" "$OUT/$SCEN-$FRAME-mine.png" 2 >/dev/null
python3 tools/ppm2png.py "$OUT/ref.ppm" "$OUT/$SCEN-$FRAME-ref.png" 2 >/dev/null
python3 tools/framediff.py "$MINE" "$OUT/ref.ppm" "$OUT/diff.ppm" --quantize
python3 tools/ppm2png.py "$OUT/diff.ppm" "$OUT/$SCEN-$FRAME-diff.png" 2 >/dev/null
echo "wrote $OUT/$SCEN-$FRAME-{mine,ref,diff}.png"
