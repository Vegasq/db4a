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

## Layer 3 — execution-level differential oracle  (in progress)

For integration bugs that only appear in the real game, once the CPUs are
already known good.

The reference core is patched locally (`ref/` is gitignored and never shipped)
with a trace hook in the 68000 instruction loop, emitting one 8-byte record per
instruction: PC plus a 32-bit FNV-1a hash of D0-D7/A0-A7. Full state per
instruction would be 68 bytes and hundreds of gigabytes; the hash is cheap
enough to run to the point of interest and still pinpoints where two
implementations part company. Once a divergence PC is known, re-run with full
state around it.

Rate: ~13.7 MB per 100 frames, so roughly 0.3 GB to reach the house-select
transition. Enable with `DB4A_TRACE=<path>`.

Granularity differs between the two sides — the recompiler executes whole
blocks, the reference executes instructions — so comparison is at block-entry
PCs, which both can report.

**Status: built and working, but the signal is dominated by timing noise.**
It located a first divergence precisely — a VDP status poll at `$4FE`, with the
12 preceding blocks matching on both PC and register hash — which is exactly
the kind of exact answer the layer exists to give.

Across 100 frames, though, 10.96% of records diverge, and every hot site is a
polling loop:

    000FDA : 24602   wait-for-vsync
    000522 : 20165   VDP status poll
    000FEE :  6745   second wait loop

The cycle model is approximate by design (no prefetch overlap, no bus
contention), so spin loops iterate a different number of times on each side and
leave different scratch values. Both sides still *branch* identically and
reconverge; the difference is dead register content.

So the oracle currently measures timing divergence, not logic divergence. To
find logic bugs it needs to ignore spin-loop noise — compare only where the two
provably resync, or exclude registers a polling loop scribbles on. That
refinement is the remaining work, not the trace machinery.

## Layer 4 — Z80 exerciser  (`make check-z80`)

Ground truth for the Z80 with no Mega Drive, no sound driver and no 68000
involved. Uses the Frank Cringle CP/M exercisers, which run each instruction
over many operand/flag permutations and CRC the results.

    cd ref && git clone --filter=blob:none --no-checkout --depth 1         https://github.com/superzazu/z80.git z80-tests
    cd z80-tests && git checkout HEAD -- roms/prelim.com roms/zexdoc.cim
    cd ../.. && make check-z80

`src/z80.c` takes its bus through externs, so the harness supplies a flat
64 KiB space in place of the Mega Drive memory map: the core under test is
byte-for-byte the one that ships. Only BDOS functions 2 and 9 are emulated.

**Status: `prelim.com` and `zexdoc` both pass — 67 groups OK, 0 errors,
46.7 billion cycles.**

Three bugs found and fixed getting there, none visible at game level:

| Bug | Effect |
|-----|--------|
| `z80_step` cached HL and wrote it back at exit | **every** 8-bit write to H or L was discarded (`LD H,n`, `INC L`, `LD H,B`); under DD/FD it wrote HL into the index register |
| undocumented index half-registers unimplemented | a DD/FD prefix on an instruction not using `(HL)` must redirect H/L to `IXH`/`IXL`/`IYH`/`IYL` |
| accumulator rotates recomputed S/Z/P | `RLCA`/`RRCA`/`RLA`/`RRA` must preserve them |

The first was catastrophic and the sound driver leans hard on DD/FD — they were
the two most common opcodes in it — so the second was very likely being hit
constantly.

**`zexall` also passes — 67 groups OK, 0 errors.** That is the stricter suite,
checking the undocumented flag bits (Y and X) as well as the documented ones,
so the Z80 core is fully validated against both exercisers.

VDP test ROMs exist but are less standardised; not attempted.

## Rule for debug tooling

Debug facilities are permanent, flag-gated and themselves tested — never
throwaway. The crash dump and vsync-wait sampler earned their place and stayed.
A one-off DMA region tally mislabelled its own buckets and sent an
investigation chasing a non-existent VSRAM fault. That distinction is the whole
lesson.
