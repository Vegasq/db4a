#!/usr/bin/env bash
# The widened view's margin must fill in EVERY direction -- acceptance B4.
#
# The view grows west (extra columns) and south (extra lines), so those are the
# two margins there are, and both are drawn from the game's own map rather than
# from the tilemap, which the cartridge only maintains for the 320x224 it
# believes is visible.
#
# WHAT "FILLS" MEANS, because the obvious metric is wrong. Counting non-black
# pixels does NOT measure the margin: unexplored map is fog, the fog tile is a
# solid block of colour index 12, and colour 12 is $0000 -- black. A margin
# looking at fog is filled correctly and reads as empty. Measured on
# artifacts.txt frame 4125, where the whole explored blob happens to be exactly
# the seven cell rows the camera is showing: the southern margin scores 128
# non-black pixels of 12800 while being 12800/12800 drawn from the map.
#
# So the two things actually checked here are:
#
#   A. the margin shows the TRUE map, by comparing the southern margin at one
#      frame against the cartridge's own picture of those same world rows at a
#      later frame, once the camera has scrolled down onto them. Terrain
#      animates between the two, so the same comparison is run on the
#      cartridge's own rows as a control and the margin only has to do as well.
#
#   B. the view stops at the map's EDGE in every direction, so the margin never
#      hangs off it. The camera limits have to move with the view size --
#      CAM_XMIN east by the extra columns, CAM_YMAX north by the extra lines.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
REC=data/recordings/level1atredis.txt
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0

# ---- A. the margin shows the true map ------------------------------------
# 12000 and 12037 are 37 frames apart, over which the camera drops 40 pixels --
# far enough that the margin's world rows land inside the cartridge's own view,
# close enough that little else has moved.
F=12000; G=12037
DB4A_WIDE=400 DB4A_TALL=256 DB4A_REPLAY=$REC DB4A_SHOTS=$F \
    DB4A_RAMDUMP=1 DB4A_PPM="$TMP/wide" ./build/db4a "$ROM" $((F+10)) >/dev/null 2>&1
DB4A_REPLAY=$REC DB4A_SHOTS=$G \
    DB4A_RAMDUMP=1 DB4A_PPM="$TMP/late" ./build/db4a "$ROM" $((G+10)) >/dev/null 2>&1

python3 - "$TMP/wide.$F.ppm" "$TMP/wide.$F.ram" "$TMP/late.$G.ppm" "$TMP/late.$G.ram" <<'PY' || fail=1
import sys, struct
def ppm(p):
    b = open(p,'rb').read(); q = b.split(b'\n',3)
    w,h = map(int,q[1].split()); return w,h,q[3]
def cam(p):
    r = open(p,'rb').read()
    g = lambda a: struct.unpack(">h", r[a:a+2])[0]
    return g(0xE3BE), g(0xE3C0)
W,H,A = ppm(sys.argv[1]); ax,ay = cam(sys.argv[2])
w,h,B = ppm(sys.argv[3]); bx,by = cam(sys.argv[4])
off = W - 320
# The margin's world rows must lie inside the later frame's own view, or the
# comparison has nothing to compare against and the numbers below are noise.
lo, hi = ay + 224, ay + H - 1
if not (by <= lo and by + 223 >= hi):
    print("  margin rows %d..%d are not inside the later view %d..%d -- pick another pair"
          % (lo, hi, by, by + 223))
    raise SystemExit(1)
x0, x1 = max(ax - off, bx), min(ax + 319, bx + 319)
def agree(y0, y1):
    same = tot = 0
    for wy in range(y0, y1):
        for wx in range(x0, x1):
            axx, ayy = wx - ax + off, wy - ay
            bxx, byy = wx - bx,       wy - by
            if not (0 <= ayy < H and 0 <= byy < h): continue
            tot += 1
            if A[(ayy*W+axx)*3:(ayy*W+axx)*3+3] == B[(byy*w+bxx)*3:(byy*w+bxx)*3+3]: same += 1
    return same, tot
band = H - 224
m_same, m_tot = agree(ay + 224, ay + 224 + band)
# Control: the same comparison over bands of the cartridge's OWN picture, which
# is by definition as right as it gets. Whatever the animation costs there, it
# costs the margin too.
ctl = [agree(ay + t, ay + t + band) for t in (64, 128, 192)]
c_same = sum(s for s,_ in ctl); c_tot = sum(t for _,t in ctl)
mp = 100.0*m_same/max(1,m_tot); cp = 100.0*c_same/max(1,c_tot)
print("  southern margin vs the cartridge's own later view : %6.2f%%" % mp)
print("  the cartridge's own rows, compared the same way    : %6.2f%%  (control)" % cp)
ok = mp >= cp - 5.0
print("  the margin is as true to the map as the cartridge's own picture: %s"
      % ("yes" if ok else "NO"))
raise SystemExit(0 if ok else 1)
PY

# ---- B. the view stops at the map's edge, in every direction ---------------
# Drive the pointer into the south-west corner until the camera pins against
# both limits, and check where the EDGES OF THE VIEW ended up -- not where the
# camera did. A bigger view has to stop earlier by exactly the amount it grew,
# or the margin is hanging over the edge of the map looking at nothing.
ST=build/cursorfield.state
[ -f "$ST" ] || DB4A_REPLAY=$REC DB4A_SAVE_AT=9000:$ST ./build/db4a "$ROM" 9010 >/dev/null 2>&1

edges() {  # wide tall -> "left bottom"
    DB4A_LOAD=$ST DB4A_WIDE=$1 DB4A_TALL=$2 DB4A_MOUSE=1 DB4A_MOUSE_TARGET="0,219" \
        DB4A_RAMDUMP=1 DB4A_SHOTS=9600 DB4A_PPM="$TMP/e" ./build/db4a "$ROM" 700 >/dev/null 2>&1
    python3 -c "
import struct,sys
r=open('$TMP/e.9600.ram','rb').read()
g=lambda a: struct.unpack('>h', r[a:a+2])[0]
print(g(0xE3BE)-($1-320), g(0xE3C0)+$2)"
}

read -r base_l base_b <<<"$(edges 320 224)"
echo "  320x224  view stops with its west edge at $base_l and its south edge at $base_b"
for size in "400 224" "320 256" "400 256" "512 256"; do
    set -- $size
    read -r l b <<<"$(edges "$1" "$2")"
    if [ "$l" -eq "$base_l" ] && [ "$b" -eq "$base_b" ]; then
        echo "  ${1}x${2}  west edge $l, south edge $b  same map edge"
    else
        echo "  ${1}x${2}  west edge $l, south edge $b  FAIL (want $base_l / $base_b)"
        fail=1
    fi
done

[ $fail -eq 0 ] || { echo "margins: FAIL"; exit 1; }
echo "margins: the widened view fills from the map, west and south, and stops at its edge"
