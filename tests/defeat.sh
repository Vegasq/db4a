#!/usr/bin/env bash
# Reach a DEFEAT, which mission 1 cannot produce.
#
# Mission 1 is the tutorial: build and harvest, with no enemy. Idling there for
# 28 minutes of game time changes nothing. A loss therefore needs mission 2,
# which is reached by winning mission 1 first.
#
# So: replay the recorded mission-1 victory, press through the briefing into
# mission 2, then stop touching the controls and let the enemy do the work.
# Defeat appears by frame 90000, about 30 minutes of game time.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
PRESS=$(python3 -c "print(','.join(f'{f}:b' for f in range(27700, 31000, 300)))")

DB4A_REPLAY=data/recordings/level1atredis.txt DB4A_PRESS="$PRESS" DB4A_HOLD=8 \
    DB4A_SHOTS="${SHOTS:-90000}" DB4A_PPM=build/defeat \
    ./build/db4a "$ROM" 95000 | grep -E 'captured|frames simulated|no block'

python3 - <<'PY'
from PIL import Image
import glob, re, sys
fs = sorted(glob.glob('build/defeat.*.ppm'), key=lambda p: int(re.search(r'\.(\d+)\.', p).group(1)))
if not fs:
    sys.exit("no capture produced")
im = Image.open(fs[-1]).convert('RGB')
# The defeat screen is a wide cinematic band on black, unlike any gameplay view.
top = sum(sum(im.getpixel((x, y))) for y in range(0, 20) for x in range(0, 320, 4))
mid = sum(sum(im.getpixel((x, y))) for y in range(90, 110) for x in range(0, 320, 4))
print("last capture %s: top band=%d mid band=%d" % (fs[-1], top, mid))
print("DEFEAT reached" if top < mid / 4 else "no defeat screen detected")
PY
