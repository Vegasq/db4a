#!/usr/bin/env bash
# Mouse control must not hold the d-pad outside gameplay.
#
# The first version did. Outside gameplay the cursor variables read (0,0),
# which passes any plausibility check, so the steering drove towards the
# pointer and held right+down forever -- making the mentat screen impossible to
# get past. This drives a scripted route to the mission with steering active
# and requires that the game still arrives.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
P=$(python3 tests/playthrough.py mission --emit-press | head -1)

# Aim inside the map. A target in the sidebar steers the cursor onto the HUD,
# which legitimately changes what the game does and is not what this tests.
out=$(env DB4A_MOUSE_TARGET=160,110 DB4A_PRESS="$P" DB4A_HOLD=6 \
    DB4A_SHOTS=6000 DB4A_PPM=build/mousetest ./build/db4a "$ROM" 6100)

# Steering must never touch the d-pad outside gameplay: menus, briefings and
# the construction-yard build list are all navigated with it.
if echo "$out" | grep -q FAIL; then
    echo "$out" | grep FAIL | head -3
    exit 1
fi
echo "  steering never held the d-pad outside gameplay"

python3 - <<'PY'
from PIL import Image, ImageStat
im = Image.open('build/mousetest.6000.ppm').convert('RGB')
mean = sum(ImageStat.Stat(im).mean) / 3
cols = len(im.getcolors(70000) or [])
# The mission view is a busy sand map. The mentat screen is a dark portrait and
# the world map is mostly black, both far below this.
ok = mean > 25 and cols > 15
print("  frame 6000: mean=%.1f colours=%d -> %s" %
      (mean, cols, "reached the mission" if ok else "STUCK before the mission"))
raise SystemExit(0 if ok else 1)
PY
