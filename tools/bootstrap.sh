#!/usr/bin/env bash
# Self-bootstrapping coverage loop.
#
# Run the recompiled build; when it reaches a PC with no block, record that PC
# as a new entry point, re-run static discovery from it, regenerate and repeat.
# The game dispatches through RAM function pointers, so these runtime-only
# targets are precisely the code static analysis cannot reach on its own.
set -u
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
SEEDS=build/seeds.txt
BUDGET=${BUDGET:-3000000}
MAX=${1:-12}
touch "$SEEDS"

for i in $(seq 1 "$MAX"); do
  before=$(wc -l < "$SEEDS")
  out=$(DB4A_SEEDS="$SEEDS" ./build/db4a "$ROM" "$BUDGET" 2>/dev/null)
  blocks=$(sed -n 's/^blocks executed *: //p' <<<"$out")
  dist=$(sed -n 's/^distinct blocks executed: //p' <<<"$out")
  irq=$(sed -n 's/^IRQ taken \/ masked: //p' <<<"$out")
  unk=$(sed -n 's/^reason *: no block for PC \([0-9A-F]*\).*/\1/p' <<<"$out")
  printf 'iter %2d: blocks=%-9s distinct=%-14s irq=%-8s ' "$i" "${blocks:-?}" "${dist:-?}" "${irq:-?}"
  if [ -z "$unk" ]; then echo "-> no unknown PC, converged"; break; fi
  echo "-> new entry $unk"
  sort -u "$SEEDS" -o "$SEEDS"
  after=$(wc -l < "$SEEDS")
  if [ "$before" -eq "$after" ]; then echo "         (seed already known - stuck)"; break; fi
  make analyse >/dev/null 2>&1
  python3 tools/recomp.py >/dev/null 2>&1
  make build/db4a >/dev/null 2>&1
done
echo
echo "seeds discovered: $(wc -l < "$SEEDS")"
