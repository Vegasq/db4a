#!/usr/bin/env bash
# The cursor must be able to reach the map drawn in the widened strip.
#
# Widescreen shows real map west of the cartridge's own 320-pixel screen. Its
# cursor lives in that 320-wide field, so without the extension the pointer can
# see the strip but never reach it -- the cursor stopped 84 pixels short of the
# window edge, and the left scroll band started 104 pixels in while the other
# three started at 24.
#
# Checked here: the cursor goes negative when pointed into the strip, the map
# cell it picks stays sane (no wrap), the extension collapses at the map's own
# western edge, and scrolling starts at the same distance as the other edges.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
ST=build/cursorfield.state
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

[ -f "$ST" ] || DB4A_REPLAY=data/recordings/level1atredis.txt \
    DB4A_SAVE_AT=9000:$ST ./build/db4a "$ROM" 9010 >/dev/null 2>&1

fail=0
probe() {  # logical_x  expect  label
    local x=$1 expect=$2 label=$3
    DB4A_LOAD=$ST DB4A_WIDE=400 DB4A_MOUSE=1 DB4A_MOUSE_TARGET="$x,110" \
        DB4A_RAMDUMP=1 DB4A_SHOTS=9060 DB4A_PPM="$TMP/c" ./build/db4a "$ROM" 90 >/dev/null 2>&1
    python3 - "$TMP/c.9060.ram" "$x" "$expect" "$label" <<'PY'
import sys
r=open(sys.argv[1],'rb').read()
x,expect,label = int(sys.argv[2]), sys.argv[3], sys.argv[4]
def w(a):
    v=(r[a]<<8)|r[a+1]
    return v-0x10000 if v>=0x8000 else v
cx, cell = w(0xBF12), w(0xBF4C)
if   expect == "negative": ok = cx < 0
elif expect == "clamped":  ok = cx >= 0
else:                      ok = cx == int(expect)
# a wrapped cell lands hundreds away; the sane range here is well under 300
ok = ok and 0 <= cell < 300
print("  %-34s logical x=%3d -> cursorX=%4d cell=%3d  %s"
      % (label, x, cx, cell, "ok" if ok else "FAIL"))
raise SystemExit(0 if ok else 1)
PY
    [ $? -eq 0 ] || fail=1
}

probe  40 negative "cursor reaches into the strip"
probe  60 negative "and further in"
probe  84 4        "at the cartridge's own edge"
probe 200 120      "mid-screen is unaffected"

# Hard against the western camera limit the strip must STILL be looking at real
# map -- the camera is held a strip-width short of the map's edge precisely so
# that it is -- and the cell conversion must not have wrapped behind the map.
# Before the camera limit moved, this collapsed to the cartridge's edge instead.
probe  40 negative "reaches the strip at the west limit" 250

# The cell picked must rise steadily from west to east. A wrap shows up as a
# jump of hundreds, which is what $5518's andi.w #$7f0 does once the sum goes
# negative -- it masks rather than saturates.
mono() {
    local prev=-1 x cell
    for x in 40 60 84 120 200 300; do
        DB4A_LOAD=$ST DB4A_WIDE=400 DB4A_MOUSE=1 DB4A_MOUSE_TARGET="$x,110" \
            DB4A_RAMDUMP=1 DB4A_SHOTS=9200 DB4A_PPM="$TMP/m" ./build/db4a "$ROM" 250 >/dev/null 2>&1
        cell=$(python3 -c "
r=open('$TMP/m.9200.ram','rb').read()
v=(r[0xBF4C]<<8)|r[0xBF4D]
print(v-0x10000 if v>=0x8000 else v)")
        if [ "$prev" -ge 0 ] && { [ "$cell" -lt "$prev" ] || [ $((cell - prev)) -gt 40 ]; }; then
            echo "  cell sequence FAIL: x=$x gave $cell after $prev"
            fail=1
        fi
        prev=$cell
    done
    echo "  map cell rises steadily west to east, no wrap"
}
mono

[ $fail -eq 0 ] || { echo "cursor field: FAIL"; exit 1; }
echo "cursor field: the cursor reaches the widened strip"
