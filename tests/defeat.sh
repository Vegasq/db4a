#!/usr/bin/env bash
# Reach a DEFEAT, which mission 1 cannot produce.
#
# Mission 1 is the tutorial: build and harvest, with no enemy. Idling there for
# 28 minutes of game time changes nothing. A loss therefore needs mission 2,
# which is reached by winning mission 1 first.
#
# So: replay the recording, which wins mission 1 around frame 15200 and carries
# itself through the briefing into mission 2 by frame 17400, then stop touching
# the controls and let the enemy do the work.
#
# It used to synthesise B presses at frames 27700-31000 to page through that
# briefing, because the previous recording won much later. The recording now
# contains those presses itself, so adding more would land in mission 2 and
# press buttons during play.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
DB4A_REPLAY=data/recordings/level1atredis.txt \
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
