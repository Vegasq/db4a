#!/usr/bin/env bash
# Acceptance criterion D4: the picture must not jump when a menu or console
# opens.
#
# The renderer picks its horizontal offset from the scene pointer at $FFFFE002
# (render_world_offset in src/render.c): the gameplay scenes are shifted right
# by the full extra width so the right-anchored HUD stays flush, everything
# else is centred with pillarbox bars. So the offset changes whenever the
# screen does -- and a change is only a JUMP if the player can see it.
#
# The game fades to black across its own screen changes, so most of those
# changes happen behind a black screen and are invisible. DB4A_LOG_JUMP=1
# reports each change with the brightest pixel in the frame either side of it,
# which is what settles the difference. It needs DB4A_RENDER_ALL=1, because
# headless otherwise renders only at the DB4A_SHOTS frames.
#
# data/recordings/power.txt is the fixture: it plays into mission 1 and opens
# and closes the build console three times, so it exercises every transition
# D4 is about.
#
# EXPECTED: exactly ONE visible change, at mission entry.
#
# The console transitions are all covered by black -- measured, 23 blank frames
# before the console appears and 5 after it goes. The one that is visible is
# the mission starting: the cartridge turns the display on and draws the
# mission map for five frames while $FFFFE002 still holds $000000, then
# installs $006D0C, and the picture slides 40 px at 400 wide. That is a real
# defect and it is not the one D4 was written about; see docs/widescreen.md.
# It is asserted here rather than ignored so that fixing it makes this test
# say so.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

DB4A_WIDE=400 DB4A_RENDER_ALL=1 DB4A_LOG_JUMP=1 \
    DB4A_REPLAY=data/recordings/power.txt ./build/db4a "$ROM" 3600 2>/dev/null \
    | grep '\[jump\]' > "$TMP/jumps" || true

sed 's/^/  /' "$TMP/jumps"

vis=$(grep -c VISIBLE "$TMP/jumps" || true)
tot=$(wc -l < "$TMP/jumps")
blk=$(grep -c covered-by-black "$TMP/jumps" || true)

echo "  offset changes: $tot   covered by black: $blk   visible: $vis"

fail=0
[ "$tot" -ge 6 ] || { echo "  FAIL: expected at least 6 offset changes, got $tot -- did the fixture change?"; fail=1; }
[ "$vis" -eq 1 ] || { echo "  FAIL: expected exactly 1 visible offset change, got $vis"; fail=1; }
# Every console open and close is a change into or out of $006D0C via $000000,
# and every one of them must be behind a black screen.
if grep VISIBLE "$TMP/jumps" | grep -qv 'offset 40 -> 80'; then
    echo "  FAIL: a visible change other than the mission-entry one"; fail=1
fi

[ $fail -eq 0 ] || exit 1
echo "widescreen: menu and console transitions are covered by black"
