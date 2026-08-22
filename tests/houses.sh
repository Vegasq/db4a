#!/usr/bin/env bash
# Every house must be selectable and load its mission.
#
# The three are distinguished by faction colour, which is the cheapest reliable
# check: Atreides blue, Ordos green, Harkonnen red, visible in the base sprites
# and the minimap border. A house that failed to load would show the wrong
# colour or no mission at all.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
FRAMES=${FRAMES:-6600}

for n in 0 1 2; do
    MOVES=""
    for i in $(seq 1 "$n"); do MOVES="$MOVES,$((2700 + i*60)):right"; done
    P="2400:start${MOVES},3100:b,3300:b,3500:b,3700:b,3900:b,4100:b,4300:b,4500:b,4700:b,4900:b,5100:b,5300:b"
    DB4A_PRESS="$P" DB4A_HOLD=8 DB4A_SHOTS=$((FRAMES-100)) DB4A_PPM="build/house$n" \
        ./build/db4a "$ROM" "$FRAMES" 2>&1 | grep -E 'no block|invariant' || true
done

python3 - <<'PY'
from PIL import Image
NAMES = ["Atreides (blue)", "Ordos (green)", "Harkonnen (red)"]
ok = True
for n in range(3):
    import glob, re
    fs = sorted(glob.glob('build/house%d.*.ppm' % n))
    im = Image.open(fs[-1]).convert('RGB')
    # the minimap border sits bottom right and is painted in the faction colour
    px = [im.getpixel((x, y)) for y in range(150, 210) for x in range(250, 315)]
    r = sum(p[0] for p in px); g = sum(p[1] for p in px); b = sum(p[2] for p in px)
    got = max(((r, 'red'), (g, 'green'), (b, 'blue')))[1]
    want = ['blue', 'green', 'red'][n]
    good = got == want
    ok &= good
    print("   house %d %-18s minimap dominant %-5s want %-5s  %s"
          % (n, NAMES[n], got, want, "ok" if good else "FAIL"))
print("all three houses load" if ok else "HOUSE FAILURE")
raise SystemExit(0 if ok else 1)
PY
