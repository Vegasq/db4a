# Verification strategy

Debugging this project by running the whole game and looking at the screen puts
maximum distance between cause and symptom. Every check validates recompiler +
HAL + VDP + Z80 + timing at once, so nothing can be ruled out and every
investigation becomes a fishing expedition. Five hypotheses about one rendering
fault were tested and refuted in a single session, three of them failures in
ad-hoc analysis code rather than in the emulator.

The answer is layered verification, ordered by how much it lets you *eliminate*.

## Layer 1 — CPU instruction vectors  (`make check-cpu`)

Ground truth at the instruction level, with no game, VDP, Z80 or timing
involved. A failure names an exact opcode and an exact register.

Uses the [SingleStepTests m68000 suite](https://github.com/SingleStepTests/m68000):
per-instruction files with full initial and final CPU + memory state, generated
from MAME's microcoded core.

    cd ref && git clone --filter=blob:none --no-checkout --depth 1 \
        https://github.com/SingleStepTests/m68000.git m68k-tests
    cd m68k-tests && git checkout HEAD -- v1/LINK.json.bin  # or any subset
    cd ../.. && make check-cpu

`ref/` is gitignored; the vectors are not vendored. Because db4a is a static
recompiler, each vector's instruction is generated exactly as the real build
would generate it and then executed — this tests the shipping path, not a
separate interpreter.

**Excluded, deliberately:** vectors whose transaction log contains an address
error (`re`/`we` cycle types), and vectors that enter supervisor mode from user
mode. Both indicate a trap, and db4a does not implement exception entry. They
are reported as skipped rather than counted as failures, so the number stays
honest. Detecting traps from the transaction log is more reliable than
inferring them from the final register state, because a trap taken while
already in supervisor mode leaves the S bit unchanged.

Current state, full suite (127 instruction files, 60 vectors each):

    make check-cpu  ->  4586/4710 passed

Bugs this layer found, none of which game-level testing caught:

| Bug | Symptom without this layer |
|-----|----------------------------|
| `LINK` displacement not sign-extended | stack walked out of RAM, crash thousands of blocks later |
| `LINK A7` pushed the post-decrement SP | silently wrong by 4 |
| `UNLK A7` applied `SP += 4` on top of the popped value | silently wrong by 4 |
| memory-form shifts never applied `(An)+` | pointer drift |
| `NOT`/`NEG` memory forms never applied `(An)+` | pointer drift |
| `BCHG`/`BSET`/`BCLR`/`BTST` never applied `(An)+` | pointer drift |
| `CMP` with a memory destination never applied `(An)+` | pointer drift |
| `MOVEM (An)+` clobbered `An` when `An` was in its own list | wrong pointer after load |

Still failing, left visible rather than filtered: `DIVS`, `DIVU`, `MOVEfromSR`,
`MOVEM.l`, `MOVEM.w`, `MOVEtoCCR`, `ROXR.b`, `SUB.b`.

Unimplemented entirely, and skipped at 100%: `ABCD`, `SBCD`, `NBCD`, `MOVEP`,
`TAS`, `NEGX.b`, and the `ORI`/`ANDI`/`EORI` to-CCR forms. None appear in this
ROM's instruction histogram, so they are out of scope until something needs
them — but the skip counts make that visible rather than assumed.

**Harness bugs this layer exposed in itself**, which is why it reports skips and
counts separately: control-flow instructions `return` a target PC and cannot be
inlined into a `void` test body, and hardcoding the instruction length gave
`BSR`/`JSR` the wrong return address. Both looked like emulator failures first.

## Layer 2 — machine invariants  (always on)

Cheap tripwires that fire the moment state goes bad: SP inside RAM, SP even, PC
inside ROM, DMA length within VRAM. Checked once per 500-cycle slice.

Disable with `DB4A_NO_INVARIANTS=1`. Each site reports once, so a persistently
broken machine cannot flood the log.

The LINK bug surfaced originally as a jump to `6D00FF12` long after the damage.
With invariants it reports `stack pointer left RAM value=0000FFCA` at the point
of corruption.

## Layer 3 — execution-level differential oracle  (not built)

For integration bugs that only appear in the real game, once the CPU is already
known good. Run against Genesis-Plus-GX in lockstep from reset with identical
scripted input and report the first divergent block. Tracked as task 13.

## Layer 4 — component suites  (not built)

`zexdoc`/`zexall` for the Z80, which is currently entirely unvalidated. VDP test
ROMs exist but are less standardised.

## Rule for debug tooling

Debug facilities are permanent, flag-gated and themselves tested — never
throwaway. The crash dump and vsync-wait sampler earned their place and stayed.
A one-off DMA region tally mislabelled its own buckets and sent an
investigation chasing a non-existent VSRAM fault. That distinction is the whole
lesson.
