#!/usr/bin/env bash
# Pointing at a build-console cell must move the highlight to it.
#
# The console is a 3x6 grid at $FFBF8A (column) / $FFBF8C (row), with the item
# in each cell at $FFBF8E + row*3 + col and $80 meaning empty. See
# docs/buildmenu.md.
#
# The state is regenerated from a committed recording rather than stored: save
# states contain cartridge VRAM and are gitignored.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
ST=build/buildmenu.state
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# Frame 2645 of the power-station recording sits in the console with the
# highlight on EXIT. NOT 2660: the recording presses A there, so a state saved
# then dismisses the console a few frames after loading.
DB4A_REPLAY=data/recordings/power.txt DB4A_SAVE_AT=2645:$ST ./build/db4a "$ROM" 2700 >/dev/null 2>&1
[ -f "$ST" ] || { echo "  FAIL: could not build the console state"; exit 1; }

fail=0
probe() {   # x y  expect_row expect_col  label
    local x=$1 y=$2 er=$3 ec=$4 label=$5 extra=${6:-}
    env $extra DB4A_LOAD=$ST DB4A_MOUSE=1 DB4A_MOUSE_TARGET="$x,$y" \
        DB4A_RAMDUMP=1 DB4A_SHOTS=2740 DB4A_PPM="$TMP/p" ./build/db4a "$ROM" 120 >/dev/null 2>&1
    python3 - "$TMP/p.2740.ram" "$x" "$y" "$er" "$ec" "$label" <<'PY'
import sys
r=open(sys.argv[1],'rb').read()
x,y,er,ec,label = int(sys.argv[2]),int(sys.argv[3]),int(sys.argv[4]),int(sys.argv[5]),sys.argv[6]
col=(r[0xBF8A]<<8)|r[0xBF8B]; row=(r[0xBF8C]<<8)|r[0xBF8D]
ok = row==er and col==ec
print("  %-28s pointer(%3d,%3d) -> row %d col %d  %s" % (label,x,y,row,col,"ok" if ok else "FAIL want row %d col %d"%(er,ec)))
raise SystemExit(0 if ok else 1)
PY
    [ $? -eq 0 ] || fail=1
}

# Cells are 32x24 from origin (32,48); these are cell centres.
probe  48 60 0 0 "EXIT"
probe  80 60 0 1 "FIX"
probe 112 60 0 2 "STOP"
probe  48 84 1 0 "concrete"
probe  80 84 1 1 "windtrap"
# $80 -- the game refuses to move there, so the highlight must not budge.
probe 112 84 0 0 "empty cell ignored"
# Off the grid entirely.
probe 250 180 0 0 "pointer off the grid"
# The escape hatch must actually disable it.
probe  80 84 0 0 "DB4A_MENU_MOUSE=0 off" "DB4A_MENU_MOUSE=0"

[ $fail -eq 0 ] || exit 1
echo "build console: pointer selects cells"
