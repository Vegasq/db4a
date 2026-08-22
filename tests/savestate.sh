#!/usr/bin/env bash
# A save state must restore the machine EXACTLY.
#
# Save at a frame, keep playing, and capture a later frame. Then reload the
# state, play the same number of frames, and capture again. The two must be
# byte-identical: anything left out of the state -- a VDP register, the Z80's
# bus ownership, an audio chip's phase -- shows up as a difference here rather
# than as a mysterious glitch minutes into a resumed game.
set -eu
cd "$(dirname "$0")/.."
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"
REC=data/recordings/level1atredis.txt
ST=build/roundtrip.state
SAVE=5000
LATER=5600

rm -f "$ST" build/rt_a.*.ppm build/rt_b.*.ppm

# straight through, saving on the way
DB4A_REPLAY=$REC DB4A_SAVE_AT="$SAVE:$ST" DB4A_SHOTS=$LATER DB4A_PPM=build/rt_a \
    ./build/db4a "$ROM" $((LATER + 10)) | grep -E 'state saved|captured'

# resume from the state and run the same distance
# The state carries its own frame number, so the resumed run continues the
# count and the input replay stays aligned. Capture the SAME absolute frame.
DB4A_REPLAY=$REC DB4A_LOAD="$ST" DB4A_SHOTS=$LATER DB4A_PPM=build/rt_b \
    ./build/db4a "$ROM" $((LATER - SAVE + 10)) | grep -E 'resumed|captured'

python3 - "$LATER" <<'PY'
import sys
a = open('build/rt_a.%s.ppm' % sys.argv[1], 'rb').read()
b = open('build/rt_b.%s.ppm' % sys.argv[1], 'rb').read()
if a == b:
    print("save state round trip: frames identical (%d bytes)" % len(a))
else:
    n = sum(1 for x, y in zip(a, b) if x != y)
    print("save state round trip: %d of %d bytes DIFFER" % (n, len(a)))
    raise SystemExit(1)
PY
