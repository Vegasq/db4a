#!/usr/bin/env bash
# Self-bootstrapping coverage loop.
#
# Run the recompiled build; when it reaches a PC with no block, record that PC
# as a new entry point, re-run static discovery from it, regenerate and repeat.
#
# The game dispatches through function pointers held in RAM (jsr (a2) and
# friends), so those targets are runtime-computed by construction and no static
# analysis can find them. Replay is the only way to learn them -- and replay
# only reaches a state if the inputs take it there, which is why this drives a
# scripted playthrough rather than letting the game sit on the title screen.
set -u
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
SEEDS=build/seeds.txt
SCENARIO=${SCENARIO:-house}
MAX=${1:-25}

mapfile -t PLAY < <(python3 tests/playthrough.py "$SCENARIO" --emit-press)
PRESS="${PLAY[0]}"
FRAMES="${PLAY[1]}"
touch "$SEEDS"

echo "bootstrapping with scenario '$SCENARIO' ($FRAMES frames)"
last_unk=""
for i in $(seq 1 "$MAX"); do
  out=$(DB4A_PRESS="$PRESS" DB4A_HOLD=6 DB4A_SEEDS="$SEEDS" \
        ./build/db4a "$ROM" "$FRAMES" 2>/dev/null)
  blocks=$(sed -n 's/^distinct blocks executed: \([0-9]*\).*/\1/p' <<<"$out")
  unk=$(sed -n 's/^reason *: no block for PC \([0-9A-F]*\).*/\1/p' <<<"$out")

  printf 'iter %2d: distinct=%-6s ' "$i" "${blocks:-?}"
  if [ -z "$unk" ]; then echo "-> converged, no unknown PC"; break; fi

  # Stall means the SAME address is still unknown after we regenerated with
  # it as a seed. A seed that is merely already in the file is fine -- an
  # interrupted earlier run leaves seeds recorded but not yet applied.
  if [ "$unk" = "$last_unk" ]; then
    echo "-> $unk still unknown after regenerating; seeding is not helping"
    break
  fi
  echo "-> new entry $unk"
  last_unk="$unk"

  sort -u "$SEEDS" -o "$SEEDS"
  make analyse >/dev/null 2>&1
  python3 tools/recomp.py >/dev/null 2>&1
  make build/db4a >/dev/null 2>&1 || { echo "         build failed"; break; }
done
echo
echo "seeds: $(wc -l < "$SEEDS")"
