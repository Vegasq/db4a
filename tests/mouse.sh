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
#
# Note this pointer also lands on the Ordos shield when the route passes the
# house screen, so the mission played here is Ordos rather than Atreides. That
# is the pre-mission menu steering working, not a fault; the check below only
# asks that a mission was reached.
out=$(env DB4A_MOUSE_TARGET=160,110 DB4A_PRESS="$P" DB4A_HOLD=6 \
    DB4A_SHOTS=6000 DB4A_PPM=build/mousetest ./build/db4a "$ROM" 6100)

# Steering must never touch the d-pad on a screen nothing claims -- briefings,
# the mentat's text pages, the title sequence -- or those become unusable. It
# may hold a direction during gameplay and on the screens the build console and
# the pre-mission menu steering own, because driving the d-pad there is exactly
# what they are for.
if echo "$out" | grep -q FAIL; then
    echo "$out" | grep FAIL | head -3
    exit 1
fi
echo "  steering never held the d-pad on an unclaimed screen"

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

# ---------------------------------------------------------------------------
# Task #26: the arrow keys must still scroll the map while mouse control is on.
#
# They did not. The frontend suppresses steering while a keyboard direction is
# held -- correctly, so the keys win -- and mouse_steer() was the only thing
# that ever replaced the cartridge's own cursor clamp box. So on exactly the
# frames the player is using the arrows, the box reverted to the ROM's
# 24..296, and 296 is where cursor_scroll_band() puts the scroll threshold: the
# cursor could reach the threshold and never pass it, so the distance past it
# was always zero and the map never moved. Measured before the fix, holding
# RIGHT for 200 frames from a mission state: 495 px of scroll without mouse
# control, 0 px with it.
#
# The harness mirrors the frontend here -- steering is skipped while the
# scripted d-pad holds a direction -- so this reproduces without a display.
# ---------------------------------------------------------------------------
ST=build/mousescroll.state
[ -f "$ST" ] || DB4A_REPLAY=data/recordings/level1atredis.txt \
    DB4A_SAVE_AT=9000:$ST ./build/db4a "$ROM" 9010 >/dev/null 2>&1

fail=0
scroll() {  # direction  axis(x|y)  min-pixels  extra-env...
    local dir=$1 axis=$2 want=$3; shift 3
    env "$@" DB4A_LOAD=$ST DB4A_PRESS="9010:$dir" DB4A_HOLD=200 DB4A_RAMDUMP=1 \
        DB4A_SHOTS=9005,9200 DB4A_PPM=build/mscroll ./build/db4a "$ROM" 260 >/dev/null 2>&1
    python3 - build/mscroll.9005.ram build/mscroll.9200.ram "$dir" "$axis" "$want" "$*" <<'PY'
import sys
def rd(p):
    r = open(p, 'rb').read()
    def w(a):
        v = (r[a] << 8) | r[a+1]
        return v - 0x10000 if v >= 0x8000 else v
    return w
a, b = rd(sys.argv[1]), rd(sys.argv[2])
addr = 0xE3BE if sys.argv[4] == 'x' else 0xE3C0
moved = abs(b(addr) - a(addr))
want  = int(sys.argv[5])
ok = moved >= want
print("  holding %-5s scrolled cam%s by %4d px (need %d)  %s   [%s]"
      % (sys.argv[3], sys.argv[4].upper(), moved, want, "ok" if ok else "FAIL", sys.argv[6]))
raise SystemExit(0 if ok else 1)
PY
    [ $? -eq 0 ] || fail=1
}

# The pointer is parked in the middle of the map and never moves, which is the
# reported situation: hand on the keyboard, mouse untouched.
scroll right x 300 DB4A_MOUSE=1 DB4A_MOUSE_TARGET=160,110
scroll up    y 300 DB4A_MOUSE=1 DB4A_MOUSE_TARGET=160,110
# Down and left both run into the map's own edge well before 200 frames are up,
# so they ask for less. Zero is the failure being guarded against.
scroll down  y 200 DB4A_MOUSE=1 DB4A_MOUSE_TARGET=160,110
scroll left  x 100 DB4A_MOUSE=1 DB4A_MOUSE_TARGET=160,110
# And at a widened size, where the camera is deliberately held a strip-width
# short of the map's western edge.
scroll right x 300 DB4A_MOUSE=1 DB4A_WIDE=400 DB4A_MOUSE_TARGET=240,110
scroll up    y 300 DB4A_MOUSE=1 DB4A_WIDE=400 DB4A_MOUSE_TARGET=240,110

# The other half of the contract: the pointer must still take over when it
# moves. With no key held, the cursor belongs under the pointer on the frame
# after it arrives -- a fix that gave the keyboard the cursor by taking it away
# from the mouse would pass everything above and be worthless.
point() {  # logical_x logical_y  extra-env...
    local x=$1 y=$2; shift 2
    env "$@" DB4A_LOAD=$ST DB4A_MOUSE=1 DB4A_MOUSE_TARGET="$x,$y" DB4A_RAMDUMP=1 \
        DB4A_SHOTS=9060 DB4A_PPM=build/mpoint ./build/db4a "$ROM" 90 >/dev/null 2>&1
    python3 - build/mpoint.9060.ram "$x" "$y" "$*" <<'PY'
import sys
r = open(sys.argv[1], 'rb').read()
def w(a):
    v = (r[a] << 8) | r[a+1]
    return v - 0x10000 if v >= 0x8000 else v
cx, cy = w(0xBF12), w(0xBF14)
wx, wy = int(sys.argv[2]), int(sys.argv[3])
ok = cx == wx and cy == wy
print("  pointing at %3d,%3d put the cursor at %4d,%3d  %s   [%s]"
      % (wx, wy, cx, cy, "ok" if ok else "FAIL", sys.argv[4]))
raise SystemExit(0 if ok else 1)
PY
    [ $? -eq 0 ] || fail=1
}
point 160 110
point 100 150

[ $fail -eq 0 ] || { echo "mouse: FAIL"; exit 1; }
echo "  arrow keys still scroll the map with mouse control on"
