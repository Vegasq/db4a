#!/usr/bin/env bash
# Pointing at a pre-mission choice must select it.
#
# Two screens, both of which store the highlight's POSITION rather than an
# index: house selection ($FFBEF8 = 32/120/208) and the mentat's YES/NO
# ($FFA62C = $128/$140). See docs/menus.md.
#
# States are regenerated from a committed recording rather than stored: save
# states contain cartridge VRAM and are gitignored.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
R=data/recordings/level1atredis.txt
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0

# Frames are specific to this recording. It reaches the house screen at 1063
# (scene 00004500, gone again by 1096) and the join question at 1137, with the
# YES/NO plates visible by 1232 -- they appear only after the question has been
# up a while, which is why this is not simply the first frame of that scene.
# Re-recording the mission moves all of these; DB4A_LOG_SCENE=1 finds them
# again.
DB4A_REPLAY=$R DB4A_SAVE_AT=1075:build/house.state   ./build/db4a "$ROM" 1150 >/dev/null 2>&1
DB4A_REPLAY=$R DB4A_SAVE_AT=1205:build/confirm.state ./build/db4a "$ROM" 1260 >/dev/null 2>&1
# Saved from the question BEFORE the plates appear, then left to run with no
# input so they arrive on their own. Saving once they are already visible does
# not work: the recording answers a few frames later, so the state captures the
# moment of dismissal and the screen is gone by the time the test looks.
DB4A_LOAD=build/confirm.state DB4A_SAVE_AT=1300:build/yesno.state ./build/db4a "$ROM" 200 >/dev/null 2>&1

probe() {  # state x y  addr  want  label  [extra env]
    local st=$1 x=$2 y=$3 addr=$4 want=$5 label=$6 extra=${7:-}
    env $extra DB4A_LOAD=$st DB4A_MOUSE=1 DB4A_MOUSE_TARGET="$x,$y" \
        DB4A_RAMDUMP=1 DB4A_SHOTS=$SHOT DB4A_PPM="$TMP/p" ./build/db4a "$ROM" 120 >/dev/null 2>&1
    python3 - "$TMP/p.$SHOT.ram" "$addr" "$want" "$label" "$x" "$y" <<'PY'
import sys
r=open(sys.argv[1],'rb').read(); a=int(sys.argv[2],16); want=int(sys.argv[3],16)
v=(r[a]<<8)|r[a+1]
ok = v==want
print("  %-34s pointer(%3s,%3s) -> %04X  %s" % (sys.argv[4],sys.argv[5],sys.argv[6],v,
      "ok" if ok else "FAIL want %04X"%want))
raise SystemExit(0 if ok else 1)
PY
    [ $? -eq 0 ] || fail=1
}

SHOT=1150
probe build/house.state  70  90 BEF8 0020 "house: Atreides"      || true
probe build/house.state 160  90 BEF8 0078 "house: Ordos"         || true
probe build/house.state 250  90 BEF8 00D0 "house: Harkonnen"     || true
probe build/house.state 160 200 BEF8 0020 "house: below shields ignored" || true
probe build/house.state 160  90 BEF8 0020 "house: DB4A_MENU_MOUSE=0 off" DB4A_MENU_MOUSE=0 || true

SHOT=1380
probe build/yesno.state 230 176 A62C 0128 "mentat: YES"          || true
probe build/yesno.state 230 198 A62C 0140 "mentat: NO"           || true
probe build/yesno.state 100 100 A62C 0128 "mentat: off the plates ignored" || true
probe build/yesno.state 230 198 A62C 0128 "mentat: DB4A_MENU_MOUSE=0 off" DB4A_MENU_MOUSE=0 || true

[ $fail -eq 0 ] || exit 1
echo "pre-mission screens: pointer selects"
