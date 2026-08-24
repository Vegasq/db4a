#!/usr/bin/env bash
# The cartridge's own 320x224 picture must survive intact at any view size.
#
# Everything the widened view adds -- extra tilemap columns and rows, the units
# standing in them -- is additive by construction: the tilemap fill writes
# columns the cartridge does not maintain, and the sprite appender adds entries
# after the cartridge has finished with its list. Neither may disturb what the
# cartridge itself drew.
#
# This is the property that makes arbitrary resolutions safe, so it is checked
# at every size rather than argued about.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
REC=data/recordings/level1atredis.txt
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

DB4A_REPLAY=$REC DB4A_SHOTS=9000 DB4A_PPM="$TMP/base" ./build/db4a "$ROM" 9010 >/dev/null 2>&1

fail=0
for size in "320 224" "352 224" "400 224" "400 240" "320 240" "512 256"; do
    set -- $size
    DB4A_WIDE=$1 DB4A_TALL=$2 DB4A_REPLAY=$REC DB4A_SHOTS=9000 \
        DB4A_PPM="$TMP/v" ./build/db4a "$ROM" 9010 >/dev/null 2>&1
    python3 - "$TMP/v.9000.ppm" "$TMP/base.9000.ppm" "$1" "$2" <<'PY'
import sys
def load(p):
    b=open(p,'rb').read(); q=b.split(b'\n',3); w,h=map(int,q[1].split()); return w,h,q[3]
w4,h4,W = load(sys.argv[1]); w3,h3,N = load(sys.argv[2])
want_w, want_h = int(sys.argv[3]), int(sys.argv[4])
ok = (w4, h4) == (want_w, want_h)
off = w4 - 320                       # the view grows west, so the game sits right
bad = sum(1 for y in range(224)
          if W[(y*w4+off)*3:(y*w4+off+320)*3] != N[(y*w3)*3:(y*w3+320)*3])
print("  %3dx%-3d  output %3dx%-3d  cartridge's own view %s"
      % (want_w, want_h, w4, h4, "EXACT" if (ok and not bad) else "DIFFERS"))
raise SystemExit(0 if (ok and not bad) else 1)
PY
    [ $? -eq 0 ] || fail=1
done

[ $fail -eq 0 ] || { echo "resolutions: FAIL"; exit 1; }
echo "resolutions: the cartridge's picture survives every view size"
