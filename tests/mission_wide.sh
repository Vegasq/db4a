#!/usr/bin/env bash
# The recorded mission-1 playthrough must play out identically at any view size.
#
# This is the deepest test in the tree -- a full mission driven by recorded
# input -- and it is the one that decides whether the widened view is a
# presentation change or a change to the game. Everything the view adds is
# supposed to be additive: tilemap columns and rows the cartridge does not
# maintain, and sprite entries appended after it has finished with its list.
#
# So the mission's RAM must come out the same, byte for byte, EXCEPT the tail
# of the sprite shadow we append into. Anything else differing means the view
# has reached back into the simulation.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
REC=data/recordings/level1atredis.txt
FR=6000,9000,12000,15000,18000
TMP=$(mktemp -d build/tmp.XXXXXX)   # relative: db4a is native on Windows, /tmp is not
trap 'rm -rf "$TMP"' EXIT

DB4A_REPLAY=$REC DB4A_SHOTS=$FR DB4A_RAMDUMP=1 DB4A_PPM="$TMP/b" \
    ./build/db4a "$ROM" 18100 >/dev/null 2>&1

fail=0
for size in "400 224" "400 240" "512 256"; do
    set -- $size
    DB4A_WIDE=$1 DB4A_TALL=$2 DB4A_REPLAY=$REC DB4A_SHOTS=$FR DB4A_RAMDUMP=1 \
        DB4A_PPM="$TMP/w" ./build/db4a "$ROM" 18100 >/dev/null 2>&1
    python3 - "$TMP" "$1" "$2" <<'PY'
import sys
tmp, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
# Since phase 2 the object layer is drawn from the game's list rather than
# appended to its table, so nothing of ours reaches RAM at all and the
# comparison is over the WHOLE of it. The window is kept at zero width so that
# any future write shows up rather than hiding here.
SAT_LO, SAT_HI = 0, 0
worst = 0
for fr in (6000, 9000, 12000, 15000, 18000):
    a = open(f"{tmp}/b.{fr}.ram", 'rb').read()
    b = open(f"{tmp}/w.{fr}.ram", 'rb').read()
    outside = [i for i in range(len(a))
               if a[i] != b[i] and not (SAT_LO <= i < SAT_HI)]
    worst = max(worst, len(outside))
    if outside:
        print("    frame %5d: %d bytes differ OUTSIDE the sprite shadow, first $%04X"
              % (fr, len(outside), outside[0]))
print("  %3dx%-3d  mission RAM bit-identical to the 320 run: %s"
      % (w, h, "yes" if not worst else "NO"))
raise SystemExit(0 if not worst else 1)
PY
    [ $? -eq 0 ] || fail=1
done

[ $fail -eq 0 ] || { echo "mission at size: FAIL"; exit 1; }
echo "mission at size: the recorded mission is bit-identical at every size"
