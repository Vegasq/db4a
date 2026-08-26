# Work journal

Newest entries at the bottom. Each entry records what was done, what it
produced, and anything that turned out to be wrong.

---

## 2026-08-21 — Session 1: analysis foundation

### ROM triage

Nine dumps in `roms/`. Hashed all of them, parsed the Mega Drive header of the
unmarked one, and computed the header checksum over `0x200..EOF`: `0x5E34`,
matching the stored value. That establishes
`Dune - The Battle for Arrakis (E).bin` (sha1 `133cc86b…`) as the clean base.
The `[b*]` files are bad dumps, `[h*]` are header hacks, `[T+Rus]` is a 2 MiB
fan translation. Recorded in `docs/rom.md`.

### Exception vectors

Wrote `tools/vectors.py`. Only two vectors are live: reset at `0x200` and
VBlank (level 6) at `0x17F4`; all others point at `0x200`. **No HBlank
handler**, which removes a class of raster-timing work from the VDP.

### Coarse ROM map — partly a dead end

Wrote `tools/rommap.py` to band the ROM by entropy and classify 1 KiB blocks.
**The classification was wrong.** The opcode heuristic accepted almost every
16-bit high nibble, so it labelled 671 KiB of a 1 MiB cart as code — implausible
on its face. Only the entropy column survives as useful signal. Kept the file
for entropy banding, marked untrustworthy in `CLAUDE.md`. Superseded by
reachability analysis, which is ground truth rather than guesswork.

### Code discovery

Wrote `tools/trace.py`: recursive descent from the vectors, following direct
branches and calls. Three bugs, in order:

1. Reading `ins.groups` raised `CsError` under capstone's SKIPDATA mode. The
   value was never used — deleted.
2. First real run reached only **156 instructions**. Cause: capstone's m68k
   `detail` API reports `disp=0` for absolute addressing, so `jmp $17a4.l` was
   silently unresolvable, and PC-relative targets came back negative and were
   discarded. Cross-checked `4eba f01e` at `0x180E` against
   `m68k-linux-gnu-objdump`, which resolves it to `0x82E`, confirming capstone's
   `op_str` is correct even where `detail` is not. Rewrote target resolution to
   parse `op_str`. Result: **25,280 instructions**, 465 functions, 0 failed
   decodes.
3. Output path was relative, so running from `tools/` crashed on write. Anchored
   to the repo root.

### Jump tables

The 48 unresolved indirect transfers were dominated by 35 instances of
`jmp tbl(pc,dN.w)` — the standard 68k dispatch idiom. Inspected four sites and
found three table formats (`BRA_B`, `BRA_W`, `OFFSET`), with entry counts
bounded by the `cmpi #N,dN / bgt` guard preceding each dispatch. Note: the first
inspection was misread because `od -t x2` byte-swaps on x86; re-read with
`-t x1`.

Wrote `tools/jumptab.py` and iterated the tracer to a fixpoint:

```
round 1: +36 tables, +6295 instructions
round 2: +10 tables,  +414 instructions
round 3:  +2 tables,   +36 instructions
round 4:  +0 tables,    +0 instructions
```

Final: **32,025 instructions, 108,014 bytes (10.3% of ROM), 772 functions,
48 tables resolved, 13 indirect sites unresolved, 76 bad decodes.**

The 76 bad decodes are *not* bogus table entries — attribution showed no table
produced them. They are flow running off function ends into inline data. 0.24%
of instructions; deferred.

### Why coverage stops at 10%

The 13 remaining indirect sites include `jsr (a0)` at `0x181A`, where the target
is a **function pointer read from `$FFFFE002`**. The game's main state machine
dispatches through RAM. Static analysis structurally cannot follow that, so
coverage plateaus near 10% regardless of effort.

Surfaced this to the user as a fork in the road. **Decision: static
recompilation**, plus emulator-based coverage tracing. Indirect transfers will
resolve at runtime, so the plateau is tolerable rather than fatal.

### Instruction mix

Histogrammed the discovered code. 79 base mnemonics but sharply top-heavy:
15 cover 80%, 35 cover 95%. No `TRAP`, `CHK`, BCD, or 68020+ addressing.
~40 instruction forms gets the translator to 99%.

**Finding: the ROM was compiled from C.** 33 matched `link`/`unlk` pairs,
stack-pushed arguments, `addq.l #4,a7` cleanup after calls. Regular calling
conventions and clean function boundaries help every later step.

### Runtime

Wrote `include/m68k.h`: CPU state, memory interface, and flag helpers. Flags
are discrete bytes rather than a packed CCR, because 68000 flag behaviour
varies per instruction and explicit helpers make each case auditable; SR is
only packed for the ~200 sites that touch it directly.

Wrote `tests/test_flags.c` covering signed overflow on `ADD`/`SUB`, carry and
borrow, `CMP` preserving X, signed conditions after an overflowing subtract,
sign extension, and SR packing. All pass. These are the foundation of every
generated instruction, so they were tested before generating anything.

### State at end of session

Analysis tooling and the runtime are done and reproducible via `make`. The
translator itself is **not started**. Remaining large pieces: translator,
dispatch, VDP, audio, input, differential testing.

Raised but undecided: dropping in Musashi as the interpreter fallback would
produce a running build far sooner and give a correctness oracle, avoiding
simultaneous translator and VDP debugging.

---

## 2026-08-21 — Session 2: documentation and goal definition

### Reproducibility pass

Added `CLAUDE.md` and a `Makefile` with `verify-rom`, `vectors`, `analyse`,
`test` and `clean`. Verified end to end from `make clean` that every number
quoted in the docs regenerates exactly, and that the working tree stays clean
afterwards.

Moved `tests/test_flags.c` out of `build/` — it was real source sitting in a
gitignored directory and would have been lost. Confirmed with
`git add -A --dry-run` before the first commit that no ROM or build artefact
could be staged.

Committed the session-1 work as six granular commits, with reproduction
commands and resulting numbers in the commit bodies so a coverage regression
shows up in `git log` rather than only in a working tree.

### The goal was never actually written down

On review, the only goal statement in the whole repository was a single line —
"decompile the ROM and recompile it into a native binary". The *approach* was
documented thoroughly; the *goal* had no definition of done, no success
criteria, no target platforms and no milestones. Nothing said when the project
is finished or how to tell a working build from a broken one.

Resolved by asking rather than guessing, since the answers materially change
the work:

- **Fidelity: faithful first, modernise later.** The reasoning that mattered:
  a faithful build can be diffed frame-by-frame against a reference emulator,
  which makes the emulator a correctness oracle. Allowing deliberate divergence
  forfeits that oracle and turns every bug into a judgement call.
- **v1: one mission playable end to end.** The title screen proves too little;
  the full campaign gives no signal for too long.
- **Platforms: Linux x86_64, Windows, macOS** (including Apple Silicon).

Written up in `docs/roadmap.md` with eight acceptance criteria, explicit
non-goals, and milestones M0–M8. Audio was deliberately excluded from v1 and
made its own milestone, since YM2612 + PSG + Z80 is large and largely
independent, and blocking playability on it would delay every other signal.

### Correction

While framing the platform question I claimed arm64 has "different endianness
handling" from x86_64. That is wrong — both are little-endian. The big-endian
ROM data needs explicit byte-swapping on *every* target equally; what actually
differs is alignment strictness and toolchain. `docs/roadmap.md` states it
correctly, and routes all ROM/RAM access through the `m68k_read*`/`m68k_write*`
helpers as the single place to get it right.

### Consequence

The Linux-only `Makefile` will not survive three platforms; CMake plus CI is
now tracked as milestone M6.

### Scope narrowed to Linux

Later the same day the user cut the target list to **Linux x86_64 only**, to
simplify. Consequences:

- The existing `Makefile` survives. CMake, cross-compilation and multi-platform
  CI are no longer needed; milestone M6 is retired rather than renumbered, so
  existing milestone references stay stable.
- v1 acceptance criterion 8 becomes `make clean && make` from a clean tree
  instead of CI on three platforms.
- The arm64 alignment concern is moot for now.

**Two disciplines were deliberately kept** despite the narrower target, because
they are required on Linux regardless and cost nothing:

1. Explicit byte-order conversion. This is *not* a portability nicety — the ROM
   is big-endian and x86_64 is little-endian, so conversion is mandatory on the
   only platform we now target.
2. All ROM/RAM access routed through the `m68k_read*` / `m68k_write*` helpers,
   as a single choke point for byte order and alignment.

Because those hold, re-adding a platform later is a build-system problem rather
than a correctness problem — which is the cheap part to redo.

### Two architecture decisions settled

**Interpreter fallback: one shared semantics definition, two backends.**
`tools/semantics.py` becomes the single source of truth for what each 68000
instruction form does; both the recompiled blocks and the fallback interpreter
are emitted from it.

The fallback is not optional — the `$FFFFE002` dispatch means execution can
reach a PC no static pass predicted, and something has to handle that at
runtime. The question was only how to build it. Considered and rejected:
dropping in Musashi (fastest to a running build, but a second independent
implementation of 68000 semantics), and hand-writing an interpreter (no
external dependency, but semantics written twice and synced by hand).

The deciding argument: if interpreted and recompiled code each carried their
own semantics, a disagreement between them would surface during differential
testing as a frame mismatch **indistinguishable from a real bug**. We would
end up debugging our own two implementations against each other instead of
against the hardware. Generating both from one definition makes that class of
bug unrepresentable. Since the recompiler needs the semantics anyway,
expressing them as data costs little beyond the initial design.

The cost accepted: slower to a first running build than Musashi would have
been, and correspondingly less early de-risking of the VDP work.

**Reference oracle: Genesis-Plus-GX.** Clean portable C and a CPU core that is
simple to patch for PC logging, with accuracy ample for a commercial title of
this era. BlastEm is held in reserve for genuine hardware-behaviour disputes
the simpler core cannot settle. It serves both coverage tracing and, from M2,
frame-hash diffing.

Recorded as an architecture-decision table in `docs/roadmap.md` so the
reasoning survives past the point where anyone remembers the discussion.

---

## 2026-08-21 — Session 3: a correction, and 76 bad decodes eliminated

### Cataloguing operand forms turned up a bug

Before writing the semantics generator, enumerated every distinct operand shape
in the corpus to ground the addressing-mode model in real data rather than
guesswork. 33 shapes — tractable. But five of them **cannot exist on a 68000**:
memory indirect `([$N, Rn])`, memory indirect with outer displacement,
a scale factor `Rn.l * 8`, and `invalid.w`. All are 68020+ forms that capstone
emits anyway despite `CS_MODE_M68K_000`.

Their example addresses clustered in `0x1DF00-0x1E400`, and the "displacement"
values were ASCII byte patterns. Byte-class analysis confirmed it: the span
`0x1D000-0x1F000` is **99.3% printable**, versus 30% printable and 19% nul in
known code. Roughly 8 KiB of text data that the tracer had walked into and
decoded as 268 fictional instructions.

### Correction to session 1

Session 1 recorded that the 76 bad decodes were "*not* bogus table entries —
attribution showed no table produced them." **That was wrong.**

The attribution only tested whether a table produced an *undecodable* target.
The offending target, `0x1DF44`, decoded perfectly well — into nonsense. Tables
producing decodable garbage were invisible to the check, so it reported a clean
bill of health it had no basis for.

The actual culprit: dispatch site `0x16F40`, whose table at `0x16F44` has **no
`cmpi` guard**. `find_bound` returned `None`, the scan fell back to
`MAX_ENTRIES = 512`, and it read straight past the end of a 32-entry table.

### Fix

Two mechanisms, both in `tools/jumptab.py`:

1. **`is_impossible()` as a validity oracle.** Capstone's willingness to emit
   68020+ forms is a wart, but it makes an excellent data detector: those forms
   are proof the bytes are not 68000 code. `probe_valid()` decodes ahead a few
   instructions from a candidate target and rejects it if any impossible form
   appears. Applied to every guard-less jump-table entry, and in the trace loop
   to stop flow.
2. **`locality_ok()`.** Real dispatch arms cluster together; an entry many KiB
   outside the span of those already accepted means we have read past the end.

Also widened the blame attribution to catch decodable-garbage targets, closing
the blind spot that hid this in the first place.

### Result

```
instructions decoded : 31525   (was 32025)
bytes covered as code: 106732 (10.2% of ROM)
function entry points: 768     (was 772)
failed decodes       : 0       (was 76)
```

The 500 lost instructions are the fabricated ones, not lost coverage. Table
`0x16F40` now terminates at exactly 32 entries, ending on a repeated
`016F84` — the default-case padding of a switch, which is the right boundary.
Zero instructions remain in the text region and zero use impossible forms.

**Lesson worth keeping:** the operand catalogue was meant to be routine
groundwork for the semantics generator. It found a real bug because it forced
every form in the corpus to be accounted for, rather than only the ones that
looked interesting. Exhaustive enumeration beats sampling.

### Addressing-mode model

Built `tools/ea.py`: the layer both backends of the semantics generator share.
It is deliberately independent of *how* an operand was obtained — the
recompiler parses capstone's `op_str` at build time and folds constants, the
interpreter will decode mode/register bits at runtime, but both produce the
same `Operand` objects and use the same C emitters.

Parsing is validated against the whole corpus: **31,525 / 31,525 instructions,
zero failures**, across 14 operand kinds. `tests/check_operands.py` keeps that
as an enforced invariant via `make check-operands`, and also fails if any
impossible-on-68000 form reappears — an unparseable operand means either a gap
in the model or the tracer decoding data as code, and both must fail loudly.

Emission is split into four steps so side effects land in the right order and
each address is computed exactly once: `setup` (where `-(An)` predecrement
happens), `load`, `store`, `post` (where `(An)+` postincrement happens).

Two 68000 rules that are easy to get wrong are encoded and tested explicitly:

- **Byte access via `(A7)+` / `-(A7)` moves the stack by 2, not 1**, keeping it
  word-aligned.
- **Address register writes are always full 32-bit and sign-extend from word.**
  `movea.w` sign-extends into the entire register; truncating would be wrong.

Partial-width data register writes preserve the upper bits, also tested.

`tests/gen_ea_test.py` generates C exercising every addressing mode, which is
then compiled and run by `make test`. Kept as a tracked generator rather than a
throwaway — the same mistake caught earlier with source living in gitignored
`build/`.

### Instruction semantics and the recompiler backend

`tools/semantics.py` now covers **100% of the corpus**, and `tools/recomp.py`
turns the code map into compilable C.

Built in three passes, measuring coverage after each:

1. Data movement and arithmetic — 67.8%.
2. Control flow — 92.9%. This needed a block model, not just expressions.
   Settled on: **every basic block is a function returning the next PC**, driven
   by a dispatch loop. No unbounded C stack growth, and an indirect transfer is
   just a computed return value, so the `$FFFFE002` RAM dispatch needs no
   special case at all.
3. Shifts, bit ops, `movem`, mul/div, `Scc`, extended arithmetic — 100%.

Shift and extended-arithmetic flag rules went into `include/m68k.h` as the
single source of truth, with 12 new unit tests. Two rules encoded explicitly
because they are easy to get wrong and silent when wrong:

- **`ASL` sets V if the sign bit changed at any point during the shift**, not
  merely if it differs at the end.
- **`ADDX`/`SUBX` have a sticky Z**: only ever cleared, never set. This is what
  makes multi-precision arithmetic work — a zero limb must not resurrect Z
  after an earlier non-zero limb cleared it.

Also encoded: MOVEM stores in **reverse** order in predecrement mode; bit ops
are mod 32 on a register but mod 8 on memory; MOVE to/from SR/CCR/USP does not
set condition codes from the value moved.

### Coverage by mnemonic is not coverage

Reaching "100%" by mnemonic proved only that a handler *existed*. Actually
running the generator over all 31,525 instructions found **225 failures** in
two categories the count could not see: status-register operands (~204), and
single-register `movem`, which capstone renders as a bare register rather than
a list (~21). Both fixed. Worth remembering as a general lesson — the useful
metric was "does it emit", not "is it in the table".

### Compiling the output

First compile produced **11,488 errors**, all one bug: `Ctx.tmp()` restarted
its counter per instruction, so temporaries collided inside a block. Fixed by
giving each instruction its own brace scope, which also makes the generated
code readable per-instruction.

A second, subtler bug: `~(T)0` promotes to `int` for narrow types, so the
`ASL`/`ASR` mask shifts were operating on `-1` — undefined behaviour that
compiled silently at first. Reworked to mask in a known-unsigned domain.

Result:

```
blocks emitted : 9265
instructions   : 31525
unimplemented  : 0
203062 lines of C, 0 errors, 0 warnings
```

The generated code's entire dependency surface is **10 symbols**: six memory
accessors, `set_sr`, two traps, and `CPU`. Nothing else leaks in, which is what
makes swapping the stub HAL for a real one a contained change.

`src/gen/blocks.c` is gitignored — it is reproducible output, and per ground
rule 2 nothing derived is tracked.

### First execution

Built `src/dispatch.c` (PC→block binary search, flat run loop), `src/hal_stub.c`
(memory map with logged-not-emulated hardware) and `src/main.c` (load ROM,
reset, run, report). `make run` now executes the recompiled ROM.

**First run stopped after 5 blocks** at `0x238` with no block for that PC —
exactly the loud failure the design intended rather than silent corruption.
The cause was a tracer bug, not a dispatch bug: `0x23E` is `dbra d1, $238`, and
branch detection tested `m.startswith("b")`. `dbra` starts with `d`, so **all
161 DBcc branch targets were never recorded as block boundaries**. Loop bodies
had been decoded only because flow fell through them linearly. Fixed, +50
instructions recovered.

After the fix, execution ran millions of blocks without a missing block.

### The ROM self-test passes

Profiling showed block `0x293C` executing **524,032 times**, which is exactly
`(0x100000 - 0x200) / 2` — the precise word count of the ROM checksum region.
Reading the routine at `0x2928` confirms what it is: mask interrupts, sum words
from `0x200` to the ROM end, compare against the stored header checksum, and
execute `illegal` at `0x294C` on mismatch.

**No trap fired.** The recompiled code computed the checksum over half a
million iterations and matched the stored `0x5E34`. That exercises postincrement
addressing, 16-bit addition with wraparound, `cmpa.l`, and signed branch
control, and is the first real evidence the semantics are correct rather than
merely well-formed.

### Interrupts, and where boot actually stops

`0x28D6` is `bra.b $28d6`, an infinite self-loop, and the generated C for it
(`return 0x28D6u;`) is correct. This is not an error path — it is the standard
Mega Drive idle loop, where the main thread spins and the VBlank handler does
all the work via the pointer at `$FFFFE002`.

Implemented 68000 interrupt entry (latch SR, force supervisor, swap to the
supervisor stack, raise the mask, push PC and SR, vector through `0x78`) and
drove VBlank between frame-sized slices.

**It still does not fire.** Instrumenting every SR write shows only 27 of them,
all `0x2700` or `0x2708` — `imask` is never lowered below 7, so every VBlank is
correctly masked, and `$FFFFE002` is never populated.

That is not a CPU bug. The ROM never reaches its interrupt-enabling path
because `hal_stub.c` does not model the hardware well enough: VDP status is a
constant, controller reads return zero, TMSS writes are discarded. Some init
routine polls hardware and takes a branch real hardware would not.

Chasing that by inspection would be guesswork. It is exactly what the
differential oracle exists for, so **Genesis-Plus-GX (M2/task 8) is now the
critical path** — run both from reset and find the first divergent block.

Current state: 122 of 9,411 blocks executed, ~87M blocks/sec.

### The region register: one byte blocking the whole boot

Rather than build emulator infrastructure to find the divergence, logged all
26 I/O reads with the block that made each. The list was small enough to read
directly, and the last entry gave it away: `blk 002840 read A10001 -> 0000`.

`$A10001` is the console version register — bit 7 export, bit 6 PAL, bit 5 no
Mega CD. The boot code:

```
002840  move.b  $a10001.l, d0
002846  andi.b  #$40, d0        ; isolate the PAL bit
00284A  beq.w   $2860           ; not PAL -> skip
00285A  move.w  #$2000, sr      ; <- the ONLY instruction that unmasks IRQs
```

`move.w #$2000, sr` at `0x285A` is the sole instruction that lowers the
interrupt mask, and it sits on the PAL branch only. The stub returned 0, bit 6
was clear, the branch skipped it. That one byte explains the stuck `imask=7`,
the dead VBlank and the empty `$FFFFE002`. This is a region E (PAL Europe)
cartridge, so the HAL now reports a PAL export console (`0xE0`).

**Lesson: the cheap diagnostic beat the expensive one.** The plan was to
instrument Genesis-Plus-GX and diff execution. Reading 26 log lines found it in
minutes. Build the oracle when inspection actually fails, not before.

### Self-bootstrapping coverage

With interrupts live, execution hit `0x8C0` — a PC never statically decoded,
reachable only through the RAM dispatch.

Realised the running build *is* a PC tracer. `tools/bootstrap.sh` closes the
loop: run until an unknown PC, record it as a seed, re-run static discovery
from it, regenerate, repeat. `trace.py` reads `build/seeds.txt` as extra entry
points.

```
iter  1: blocks=580010    distinct=149 of 9411   irq=2/56    -> new entry 0008C0
iter  2: blocks=2780020   distinct=327 of 9426   irq=221/57  -> new entry 017C32
iter  3: blocks=3000000   distinct=338 of 9445   irq=243/57  -> converged
```

Two seeds were enough to converge. This is cheaper than instrumenting an
external emulator and needs no third-party code, though the emulator is still
required later as a *correctness* oracle — finding unknown PCs and verifying
behaviour are different jobs.

### The RAM dispatch works

```
RAM $FFFFE002 : 00017C32   (main-loop handler pointer)
IRQ taken     : 243
distinct blocks executed: 338 of 9445
```

`$FFFFE002` is populated and live. The mechanism that drove the entire
architecture decision — a state machine dispatching through a RAM function
pointer — now works end to end: boot installs the handler, VBlank fires, and
the flat dispatch loop resolves it at runtime with no special case, exactly as
the block model predicted.

### Pacing is the next honest gap

76% of blocks now run in `0x000FDA`:

```
000FD6  move.w  $e006.w, d0
000FDA  cmp.w   $e006.w, d0
000FDE  beq.b   $fda
```

That is wait-for-vsync, spinning until the VBlank handler increments the frame
counter at `$FFFFE006` (the `addq.w #$1, $e006.w` at `0x181C`). Correct game
behaviour, not a bug. It over-spins only because frames are sliced by a fixed
block count rather than a cycle budget. **Cycle counting is needed** for v1
acceptance criterion 6 (PAL 50 Hz, no drift).

### Cycle counting and real frame pacing

`tools/timing.py` gives each instruction a cycle cost from the 68000 manual —
a base cost per operation plus effective-address calculation per memory
operand. `recomp.py` sums it per block at generation time and emits a single
constant `CPU.cycles += N;` at block entry, so pacing costs one add per block
rather than per instruction.

Corrected a mistake in my own task note: I had written the PAL budget as
`3579545/50`. That is the Z80/colourburst clock. The 68000 on a PAL Mega Drive
runs at `53203424/7` = 7,600,489 Hz, so a 50 Hz frame is **152,009 cycles**.

Documented limitations, deliberately: the model ignores prefetch overlap and
Mega Drive bus contention (the 68000 stalls while the VDP holds the bus), and
uses typical values for data-dependent `MULU`/`DIVU` and shift counts. It is
accurate enough to pace frames, which is its only job. It is *not* accurate
enough for raster effects — acceptable here because this ROM has no HBlank
handler.

Switched the CLI from a block budget to a frame count, since pacing is now
cycle-based and frames are the meaningful unit.

The first paced run looked like a regression — `$FFFFE002` back to zero, 272
distinct blocks against 338. It was not. A cycle-bounded frame runs ~6,700
blocks where the old fixed slice ran 10,000, so 300 frames covered less work
than before. With frames as the unit:

```
 300 frames ( 6 s): 403? no -- 272 distinct, handler not yet installed
1200 frames (24 s): 403 distinct, $FFFFE002 = 00017C32, IRQ 1109
3000 frames (60 s): 403 distinct, $FFFFE002 = 00017C32, IRQ 2909
```

Cycle pacing is a net improvement: 403 distinct blocks versus 338. Masked
interrupts stay pinned at 91 no matter how long the run, i.e. all of them occur
during boot before interrupts are enabled, which is the expected shape.

The game now boots, installs its handler, and runs stably for a minute of game
time at correct PAL pacing. It plateaus at 403 blocks because it has reached a
steady state — rendering to a VDP that discards everything, and waiting for
input that never arrives. **The VDP is now the blocker**, with ~27k writes per
run going nowhere.

## 2026-08-21 — Session 4: the VDP renders

### VDP core

`src/hal_vdp.c` implements the address/code state machine, VRAM/CRAM/VSRAM
writes, auto-increment, and DMA (68000→VDP, VRAM fill, VRAM→VRAM copy). The
control port is a two-word protocol: a write with bits 15-14 == 10 is a
register write, anything else is half of an address/code pair assembled across
two writes. Byte writes to a VDP port duplicate the byte into both halves, and
a VRAM write to an odd address swaps byte order — both real hardware quirks.

Wiring it into the memory map immediately showed the ROM was doing real work
all along:

```
writes  vram=46723 cram=5888 vsram=80 reg=4317
dma     transfers=115 words=243917
vram    25606 of 65536 bytes non-zero (39.1%)
display ON   vint on   H40 (320px)
planes  A=C000 B=E000 window=6000 sprites=B000 hscroll=B800
```

Register values are self-consistent (reg2→A, reg4→B, reg5→sprites all agree
with the plane addresses), which is a good sign the control protocol is right.

### Renderer

`src/render.c` decodes both scroll planes and the sprite table into a 320x224
RGB framebuffer: 4bpp 8x8 tiles, per-line or per-cell horizontal scroll, column
vertical scroll, flip bits, per-tile palette select, and plane priority.

Deliberately a whole-frame renderer rather than per-scanline — this ROM has no
HBlank handler, so there are no mid-frame register changes to honour, and a
frame renderer is much easier to verify.

### First frames

A first capture at 1200 frames was 0.6% non-black and looked like a failure. It
was not — the game cycles through screens and that moment was a transition.
Sampling across time told the real story, including that the plane bases
**swap** between screens (A=E000 early, A=C000 later) as the game reconfigures
the VDP, which the renderer follows correctly.

```
 120 frames (2.4s): A@E000  48/4096 entries,  3.0% tiles,  3.7% non-black
 300 frames (6.0s): A@E000  48/4096 entries,  3.0% tiles, 99.8% non-black
 600 frames (12 s): A@C000 686/4096 entries, 48.1% tiles, 23.1% non-black
1200 frames (24 s): A@C000   7/4096 entries, 55.1% tiles,  0.6% non-black
```

At 300 frames the console logo renders; at 600 frames the publisher screen
renders **pixel-perfect** — correct palettes, correct compositing of a star
field against logo planes, clean text. The console-logo frame showed horizontal
streaking, which is the mid-wipe animation caught in progress rather than a
renderer defect; the static screen has no artifacts.

`tools/ppm2png.py` converts captures for viewing (stdlib only, no PIL).

### Honest gaps

- **Nothing is verified against a reference emulator yet.** "Looks right" is
  not the same as correct, and the fidelity policy depends on frame-hash
  diffing. Sprites in particular have not been exercised — the sprite table was
  empty in every frame captured so far.
- The mid-wipe streaking is *assumed* to be animation, not confirmed.
- DMA runs regardless of the reg1 DMA-enable bit.
- No SDL output yet; frames are written as PPM.

### Differential oracle

Built Genesis-Plus-GX as a libretro core and `tools/refhost.c`, a headless
libretro host that runs the reference for N frames and dumps PPM.
`tools/framediff.py` compares framebuffers and writes a diff map.

`ref/` is gitignored — third-party source is not vendored. GPGX's licence is
non-commercial with source-disclosure terms, but it is used purely as a
development oracle and is never linked into db4a, so it does not constrain what
the project ships.

**The first comparison was wrong, and the bug was in the oracle.** It reported
69% agreement, flat across every reference frame from 500 to 3600 — no peak,
which ruled out frame misalignment and looked like a serious renderer defect.
The reference images were green-tinted with content squeezed into the left of
the frame.

That is the signature of misreading RGB565 as XRGB8888: two 16-bit pixels get
consumed per 32-bit read, halving the apparent width and scrambling channels.
The host's environment callback returned false for RGB565, and GPGX simply
carried on using it — refusing a format is not the same as changing it. The
host now honours whatever format the core picks and converts all three.

Corrected result at frame 600:

```
exact match : 54883 / 71680  (76.57%)
near match  : 16388          (<= one palette step)
hard mismatch: 409 px        (0.57%), spread over 94 rows
```

99.4% of pixels agree within one palette step. The bulk of the residual is
quantisation: the oracle path goes 9-bit CRAM → RGB565 → 8-bit, while the
renderer goes 9-bit CRAM → 8-bit directly. Only 0.57% differ by more than one
step, thinly scattered rather than clustered, which does not look like a
structural fault.

**Lesson, again:** a disagreeing oracle is not automatically evidence about the
thing under test. The first instinct was to hunt for a renderer bug; the fault
was in the measuring instrument. Verify the oracle before trusting its verdict.

Remaining: the 0.57% hard mismatch is not yet explained, and exact comparison
is limited by RGB565 quantisation — comparing at palette-index level would be
sharper.

### The colour ramp was wrong — a real bug the oracle found

With the oracle fixed, comparison showed every non-black pixel disagreeing
while the image structure matched exactly. Sampling the mismatched pairs gave a
perfectly systematic per-level mapping:

```
mine:  0   36   73  109  146  182  219  255
ref:   0   32   68  101  139  172  205  238
```

Reading GPGX's `MAKE_PIXEL` explained it. A 3-bit VDP colour component does
**not** map linearly onto 0-255. The VDP shares one DAC range across shadow,
normal and highlight modes, so normal mode uses only part of it: the component
behaves as a 4-bit value of `(c << 1)` out of 15, and highlight adds 7 to reach
full scale. Full intensity in normal mode is therefore **238, not 255**.

Replaced the naive `c*255/7` ramp with `LVL(n) = (n*255+7)/15` and tabulated
all three intensity modes. Exact agreement went 76.57% → 95.77%.

This is precisely the class of bug that "looks right" hides. Both renders were
plausible; only the oracle could tell them apart.

### Two more bugs in the measuring instrument

The remaining 4.23% split into two causes, neither of them the renderer:

1. **~2625 pixels differed only in green, by exactly 4.** The comparison
   quantiser scaled proportionally where packing into RGB565 truncates.
   Quantising by `>>3`/`>>2` and expanding as `refhost.c` does removed all of
   them.
2. **The alignment sweep silently ran unquantised.** `--quantize` was passed as
   the third positional argument, so it landed in `diff_path` and the flag was
   never seen — the sweep reported raw numbers that looked like a flat plateau.
   Flags are now position-independent.

That is three separate defects in the oracle and its tooling versus one in the
renderer. Worth remembering when a measurement disagrees with expectation.

### Result

```
exact match : 71306 / 71680  (99.48%)
near match  : 0
mismatched  : 374            (0.52%)
```

The alignment sweep now shows a sharp transition (82.5% at ref frame 580 →
99.45% at 590) rather than a flat plateau, confirming frame alignment is
genuine and not an artifact.

The residual 374 pixels are spread evenly across all 20 horizontal bands and 94
rows, in pairs like `(0,0,0)` ↔ `(0,0,65)` — the starfield, single scattered
pixels present in one render but not the other. It never reaches zero at any
reference frame, which fits a sub-frame timing offset: the starfield animates
every frame and our frame boundary lands at a slightly different point in the
update cycle. That is the expected consequence of the approximate cycle model
(no prefetch overlap, no bus contention), documented when it was written.

**Not claimed:** that the last 0.52% is definitively benign. It is consistent
with animation phase and inconsistent with a structural fault, but it has not
been proven. Sprites also remain entirely untested — the sprite table was empty
in every frame captured.

### SDL frontend and input — and a roadmap correction

Added `src/hal_input.c` (3-button pad with TH multiplexing on bit 6, active-low
buttons) and `src/sdl_main.c` (SDL2 window, keyboard and gamepad bindings held
in a table so remapping is a data change, 50 Hz pacing). `make play` builds and
runs it; the headless harness stays as the batch/diff tool and gained scripted
input via `DB4A_PRESS="900:start"` so input can be tested without a display.

**Input did nothing.** Identical 410 distinct blocks with and without a
simulated Start press. Rather than guess, added a histogram of every hardware
address touched:

```
R A11100 : 2840     Z80 bus request
R A01B21 : 1408     Z80 RAM $1B21
R C00004 :  120     VDP status
R A10001 :    2     version register
```

`$A10003`, the controller data port, **is never read at all**. The ROM
configures the port control registers once at boot and then ignores them. What
it actually polls is a handshake with the Z80:

```
0014C8  move.b  #$1, $a01b20.l    ; request byte into Z80 RAM
0014D0  move.b  $a01b21.l, d0     ; poll for a reply
0014FA  move.b  #$0, $a01b20.l
```

Hypothesised the game was simply waiting for a non-zero reply and tested it by
forcing one. **The hypothesis was wrong**: with a forced reply, distinct blocks
fell from 410 to 127 and the VDP configuration regressed to its early-boot
layout. The ROM expects a real protocol, not any non-zero byte. Removed the
experiment rather than leaving a "fake the Z80" backdoor in the HAL — the
finding belongs in the journal, not in shipped code.

What is **established**: the controller port is never read; the ROM gates on a
Z80 handshake; no stub value beats simply reporting an absent Z80. What is
**inferred but not proven**: that pad state reaches the 68000 via the Z80.

Either way the plan changes. Audio was deferred to M5 on the reasoning that it
is "large and largely independent". That was wrong about the Z80 specifically —
it is on the critical path to gameplay. Added milestone M2.5: Z80 core plus
68000/Z80 bus arbitration, RAM and bank register, with the sound *chips* still
stubbed and still in M5.

This is the second roadmap assumption that measurement overturned, after the
PAL region byte. Both were reasonable-sounding and both were wrong.

### Z80 core

Applied the methodology that made the 68000 tractable: **measure before
implementing**. The Z80 program is uploaded into Z80 RAM by the 68000 at
runtime, so capturing it (`DB4A_Z80DUMP`) and surveying it statically
(`tools/z80scan.py`, recursive descent from $0000) sized the work exactly:

```
reachable instructions : 2504
bytes covered          : 5655 of 8192 (69.0%)
distinct opcodes used  : 133 of 256
prefixes               : DD 296, FD 211, CB 179, ED 29
```

Unlike the 68000 — where 15 mnemonics covered 80% — this is **not** top-heavy:
the top 24 opcodes reach only 72%. And DD/FD are the two most common opcodes,
so IX/IY are a first-class path, not an optional extra. A partial core would
have been a long tail of crashes.

That argued for structured decoding over a 256-way switch. Every opcode splits
as `x x y y y z z z` (`x=op>>6, y=(op>>3)&7, z=op&7`), which mirrors the
instruction set's actual layout, so one branch per `(x,z)` pair covers whole
families. `src/z80.c` plus `src/z80_exec.h` implement base, CB, ED and DD/FD
pages including the block transfer/search instructions.

Caught while writing: the index-register write-back was a stub, so `INC IX`,
`ADD IX,rp`, `POP IX` and `EX (SP),IX` would have silently discarded their
results. Fixed by having the context hold a pointer to the live register.

### Bus arbitration, and a byte-vs-word trap

`src/hal_z80.c` implements Z80 RAM, BUSREQ/RESET, and the Z80's banked window
into the 68000 bus.

First attempt regressed the 68000 from 410 distinct blocks to **8**, hanging at
`0x24A` — `btst d0,(a1) / bne` against `$A11100`, the bus-grant poll. Cause:
`move.w #$0100, $A11100` decomposes into `0x01` at the even address and `0x00`
at the odd one, and the handler matched the whole 256-byte range, so the second
byte immediately revoked the request the first had made. Only the even byte
carries the control bit. Word writes also had to be routed at all — initially
only the byte path delegated to the Z80, so the ROM's word-sized bus writes
were silently discarded.

### State

```
Z80 state : pc=0038 sp=1B1C cycles=106416950 RUNNING
```

106M cycles is ~30 s at 3.55 MHz, so the core executes the real driver
continuously without wandering into garbage — a good sign for a from-scratch
implementation, though not proof of correctness.

**Not yet working:** the handshake byte at `$1B21` stays `00`, so the 68000's
poll is still unanswered and the game does not progress (still 410 blocks,
sprite table still empty). Likely candidates: the driver waits on YM2612 timer
status, which is currently stubbed to 0, or the protocol needs command values
the 68000 has not sent. Not yet diagnosed — asserting a cause here would be
guessing.

The Z80 core has also not been validated against anything. ZEXDOC/ZEXALL would
need CP/M scaffolding; the differential oracle is the cheaper route.

### Scheduling fix, and a correction about the handshake

Profiling the Z80 (`z80_pc_hits`) showed it alive and healthy: 4.3M
instructions across 675 distinct PCs, sitting in a tight loop around
`0x0041-0x0049`, and **it did write to `$1B2x`**. So the driver was running,
not crashed.

The real defect was my scheduling. The 68000 ran a whole 152,009-cycle frame
before the Z80 got any time at all, so a poll loop inside one frame could never
be answered within that frame. `m68k_run_frame()` now interleaves the two in
500-cycle slices, keeping the Z80's clock aligned even while the 68000 holds
the bus. Z80 writes to `$1B2x` went from 4 to 20 over the same run.

**Correction to the previous entry.** I described the `$1B20`/`$1B21` handshake
as gating the game and said the reply "never arrives". Reading the whole
routine shows the opposite:

```
0014D0  move.b  $a01b21.l, d0
0014DE  tst.b   d0
0014E0  beq.b   $14ea        ; reply == 0 -> SUCCESS, return
0014E2  moveq   #$44, d0     ; else delay and retry forever
```

It succeeds when the reply is **zero**, which is exactly what it reads. The
handshake completes and never blocked anything. That also explains the earlier
experiment cleanly: forcing a non-zero reply dropped the run from 410 blocks to
127 because it makes this loop retry indefinitely. I had the polarity backwards
and drew a conclusion from a fragment instead of reading the whole routine.

### The actual blocker

```
frame  600 : $FFFFE002 = 00017C32
frame 1800 : $FFFFE002 = 00017C32
distinct blocks: 410 at 1800, 2600 and 3400 frames -- identical
```

The state-machine pointer is **frozen on one handler** from frame 600 onward,
94.7% of execution is the wait-for-vsync loop, and nametable A holds 7 non-zero
entries — a blank screen. The build reaches the publisher screen, fades out,
and never loads the title screen the reference shows by frame 2600.

`0x17C32` itself looks like an ordinary attract animation (increments of
`0x800` against a `0x30000` limit, flags in `$FFD70A/0B`), so the fault is
upstream of it: something that should advance the state never does.

**Root cause not found.** Candidates not yet distinguished: a VDP status bit
the ROM waits on, a Z80 core bug producing a wrong value somewhere, or an
unimplemented hardware read. Guessing further from disassembly is what produced
the handshake error above.

The right instrument already exists. The differential oracle should be extended
from frame comparison to **execution comparison** — run both against
Genesis-Plus-GX and find the first block where state diverges. That is what it
was built for, and it is the honest next step rather than more inference.

### Input ruled out, definitively

User report from `make play`: the game runs and displays, but Start does
nothing and it never leaves the publisher screen. Added pad tracing
(`DB4A_LOG_PAD`) counting every injected press and every read the game makes,
specifically to separate "the game ignores input" from "the game never asks".

```
pad reads=0   ctrl-writes=2   data-writes=0

[pad] port0 CTRL <- 40
[pad] port1 CTRL <- 40
[pad] Start DOWN
[pad] Start up
```

The ROM configures both port control registers at boot (TH as output) and then
**never reads a data port**. Not once in 1500 frames. Key presses reach
`pad_set` correctly, so the SDL → pad chain works end to end; the game simply
never polls.

This confirms the earlier hardware histogram rather than resting on it, and
rules input out as a cause. It also matches the reference, which reaches the
title-screen menu by frame 2600 with no input at all — the publisher screen
self-advances on a timer, so Start was never the mechanism that would move it.

The stall remains the frozen state pointer at `$FFFFE002 = 00017C32`.

### Root cause found: word reads of the VDP data port

The user reported from `make play` that the game fades to black, shows "PRESENT"
over moving stars, and stops there. That detail corrected an assumption of mine:
I had read "nametable A: 7 non-zero entries" as an essentially blank screen.
"PRESENT" is seven characters — those 7 tiles *were* the word. The game was not
frozen at all; the starfield animation in `0x17C32` was running correctly.

Walking the stack from the vsync wait (sampling `4(A7)` at `$FDA`) identified
who was blocked — three consecutive call sites at ~107k samples each:

```
01756C  move.w  #$666, (a7)     ; colour A
017578  jsr     $4208.l         ; -> d0
017580  jsr     $43f2.l         ; wait vsync x3
017592  move.w  #$eee, (a7)     ; colour B
01759E  jsr     $4208.l         ; -> d0
0175A6  tst.w   d0
0175A8  beq.b   $1756c          ; d0 == 0 -> loop forever
```

A palette flash loop, spinning until `$4208` reports the fade complete. And
`$4208` decides that by reading the *current* CRAM value back from the VDP:

```
00422E  move.w  #$8f02, $c00004.l   ; auto-increment = 2
004236  move.l  d1, $c00004.l       ; address + CRAM-READ code
00423C  move.w  $c00000.l, d3       ; read CRAM back
00424A  cmp.w   d3, d2              ; reached the target?
```

**The bug was in memory routing.** A 16-bit read of the VDP data port was being
serviced as two `io_read8` calls, and `vdp_read_data()` auto-increments the
address on every call. So one word read consumed *two* CRAM entries and spliced
the high byte of one to the low byte of the next. The comparison could never
match, the fade never completed, and the loop ran forever.

The data port is a 16-bit register with a side effect; it must be a single
access. Fixed in `m68k_read16`.

```
distinct blocks : 410 -> 632
nametable A     : 7   -> 711 non-zero
sprite table    : 0   -> 38 bytes      (sprites active for the first time)
$FFFFE002       : frozen -> cleared    (the stop routine finally ran)
pad reads       : 0   -> 4480
```

The title screen now renders at **99.98% exact match** against the reference —
13 pixels of 71,680. Pressing Start advances the game further still.

Three notes worth keeping:

1. **Input was never broken.** It was measured as broken-looking (`pad reads=0`)
   because the game never reached a state that polls a controller. Ruling it out
   by measurement rather than assumption was right, but the measurement answered
   a different question than the one that mattered.
2. **The user's observation broke the deadlock.** "Fades to black, then PRESENT
   over moving stars" contained the fact that animation was still running, which
   contradicted my "frozen" reading and pointed at a loop rather than a hang.
3. This is the third bug found in the *plumbing* rather than the generated code.
   The recompiled 68000 has been correct throughout.

### Past the title screen: house-select does not render

Added scripted input to `tools/refhost.c` (same `DB4A_PRESS` syntax) so both
sides can be driven identically — comparing anything past the title screen is
meaningless otherwise. Also added `DB4A_HOLD` to tune press length and
`DB4A_LOG_DMA` to trace DMA.

With Start pressed at frame 2400 the reference reaches the **Select your
House** screen. The native build goes black immediately (frame 2420 onward) and
stays black, sitting in an input-wait loop at `$4716` — so the game logic
advanced, but the screen never loaded. 76% frame agreement, against 99.98% on
the title screen.

Three hypotheses tested and **all three refuted**:

1. *Double-consumed input* — a long Start hold being taken twice. Refuted:
   `DB4A_HOLD` of 1, 2, 4 and 8 frames all give byte-identical results (684
   blocks, VRAM zeroed). Press length is not a factor.
2. *A late spurious VRAM fill* — VRAM ends up 0 of 65536 bytes non-zero, and
   FILL #4 fires after 144390 writes with nothing written afterwards. Refuted
   as spurious: logging arm-vs-fire separately shows every ARM immediately
   followed by its FILL, correctly paired, `reg23=80` (mode 2), `code=21`. All
   four fills are legitimate full-VRAM clears, and fills #1-3 are the same
   shape as the ones the working title screen performs.
3. *Stale `dma_fill_pending`* — implied by (2), also refuted by the pairing.

What is established: the game clears VRAM, then enters an input wait **without
reloading graphics**, while the reference loads house-select at the same point.
That is a divergence in execution, not in rendering.

**Root cause not found.** Three plausible-sounding hypotheses died on contact
with measurement here, which is the signal to stop inferring from disassembly.
The execution-level differential oracle — run both against Genesis-Plus-GX and
find the first block where state diverges — has been deferred repeatedly and is
now clearly the cheapest remaining route. Frame comparison has taken us as far
as it can.

### The crash: LINK displacement sign extension

User report: `make play` crashes on Start with `no block for PC 6D00FF12`.

**First finding — the two frontends had diverged.** `sdl_main.c` had no Z80
references at all: no `hal_z80_init()`, no `m68k_run_frame()`, no `z80_irq()`.
Z80 support was added to `main.c` and never to the SDL path, so the interactive
build was running the 68000 with a dead Z80 while all my headless testing ran
both. That is why the crash was not reproducing here.

Fixed properly rather than by patching the copy: `src/system.c` now owns
`system_reset()` and `system_frame()`, and both frontends call them, so the two
cannot drift again.

**Second finding — a correction.** With the frontends unified the crash
reproduced headlessly, and it turned out it had been happening all along. The
previous entry recorded that the game "clears VRAM then enters an input wait
without reloading graphics". It had actually **crashed**; the black screen was
the frozen post-crash framebuffer. I had been grepping VDP statistics and never
looked at the `reason` line.

**Root cause.** Added a crash dump (register state, stack, and a 64-entry ring
buffer of recently executed blocks). It pointed straight at the problem:

```
A0-A7 ... FFFFFFF0 0000FFC8
                   ^^^^^^^^ A7
```

Mega Drive RAM is at `$FF0000-$FFFFFF`, so a valid stack pointer looks like
`0xFFFFFFC8`. The high word had been zeroed, so `rts` fetched garbage from ROM.

capstone renders LINK displacements as **unsigned** words:

```
00A934  link.w  a6, #$ffc8      ; this is -56, not +65480
04935C  link.w  a6, #$ffa8      ; -88
```

`i_link` used the value directly, so every stack frame moved SP *up* by ~64 KiB
instead of allocating a few dozen bytes. After enough calls A7 wrapped past
`0xFFFFFFFF` into low memory — and the crashed A7 still carried that
displacement in its low word. LINK's displacement is always signed 16-bit; now
sign-extended.

```
distinct blocks : 684 -> 981
VRAM            : 0 -> 10053 bytes non-zero
sprite table    : 40 bytes
crash           : gone
title screen    : still 99.98%, no regression
```

**A correction worth recording.** I had claimed every bug so far was in the HAL
or the measuring tools, never in the generated code. That is no longer true —
this one was a genuine semantics bug in the recompiler, and it survived because
`tests/test_flags.c` and `test_ea.c` cover flags and addressing modes but
nothing exercises LINK/UNLK. The gap was in the test suite, not just the code.

Remaining: house-select renders only partially (75% frame agreement, fragments
and an empty box where the crests belong) rather than not at all.

### House-select: narrowed, not solved

Added end-to-end instruction tests (`tests/gen_sem_test.py`, 11 cases) covering
LINK/UNLK, which the earlier bug had slipped through. Its first run failed on
`ext.w` — and the fault was in my expectation, not the implementation.

Then chased the partial house-select render. VDP state:

```
nametable A @E000:  68/4096 non-zero
nametable B @C000:   0/4096 non-zero
tile area        : 22.1% non-zero
cram             : 60/64 entries
```

Palette and tile graphics load; the tilemaps are near-empty. So the artwork is
in VRAM and nothing tells the VDP to draw it.

**Two false trails, both caused by my own analysis tooling.**

1. A DMA-destination tally showed `VSRAM: 259392 words` against a 40-entry
   VSRAM. That looked like a smoking gun. It was a labelling bug: the VRAM
   bucket was `(addr >> 13) & 7`, so VRAM at `0xE000+` landed in bucket 7,
   which I had labelled VSRAM. A CD-code histogram showed only codes `21`
   (VRAM) and `23` (CRAM) — no VSRAM DMA exists at all.
2. With that fixed, the 259392 words are real DMAs to nametable A, arriving as
   zeros. That also looked conclusive. It is not: the **working** title screen
   performs the identical zero-source DMAs to `0xE000`. They are nametable
   clears, normal on both screens.

What is actually established: house-select performs its clears plus one real
upload (`addr=F780 len=128 src=064246`, real ROM data — the fragments visible
on screen), and the crest tilemaps are then **never uploaded at all**. Absent,
not corrupted. So the game does not reach the code that would upload them.

**Root cause still open.** Running total for this screen: five hypotheses
tested, five refuted — three of them failures in the measuring tooling rather
than the emulator. That ratio is the argument for the execution-level oracle
that has now been deferred four times.

## Layer 2: machine invariants

Cheap tripwires that fire at the moment state goes bad rather than thousands of
blocks later. `include/invariant.h` + `src/invariant.c`, permanent and
flag-gated (`DB4A_NO_INVARIANTS=1` to disable), reporting once per site so a
persistently broken machine cannot flood the log.

Checks: SP inside RAM, SP even, PC inside ROM, DMA length no larger than VRAM.
Evaluated once per 500-cycle slice rather than per block — a corrupted SP stays
corrupted, so slice granularity still catches it immediately at no measurable
cost.

**Validated in both directions**, which is the part that matters:

1. No false positives — clean across the title screen and the house-select
   path. An invariant that cries wolf is worse than none.
2. It actually catches its target. Temporarily reintroducing the LINK
   sign-extension bug produces:

```
*** INVARIANT VIOLATED: stack pointer left RAM
    value=0000FFCA context=000203BC
```

versus the original symptom, which was a jump to `6D00FF12` long after the
damage was done. The first validation attempt was itself wrong — run at 600
frames with no input, where the corruption has not yet accumulated — and
reported "all clean". Testing the tripwire under the scenario that actually
crashed is what proved it.

## Layer 1 across the full instruction set

Fetched all 127 vector files (132 MB, gitignored) and ran the whole suite.

```
make check-cpu  ->  4586/4710 passed
```

Five more real semantics bugs, all of the same family — read-modify-write
handlers that never applied `(An)+` postincrement: `NOT`, `NEG`, the four bit
operations, and `CMP` with a memory destination. Plus `UNLK A7`, the exact
mirror of the `LINK A7` bug: An and SP are the same register, so the popped
value is the final SP and `SP += 4` must not be applied on top of it.

Every one of these is invisible at game level until a pointer drifts far enough
to corrupt something, at which point the symptom is thousands of blocks from
the cause. That is the entire argument for this layer.

**The harness found two bugs in itself first**, both of which initially looked
like emulator failures:

1. Control-flow instructions `return` a target PC, because in the real
   recompiler every block is a `uint32_t` function. Inlining them into a `void`
   test body produced 291 compile errors. Restructured so each vector's
   instruction becomes its own block function and the test calls it — which
   also makes the harness mirror the shipping architecture rather than
   approximate it.
2. The instruction length was hardcoded to 2, so `BSR`/`JSR` pushed a wrong
   return address. Now taken from capstone.

Also worth recording: `make vectors` silently collided with the pre-existing
target that prints the exception vector table, so the suite never ran in the
commit that introduced it. Caught only because the commit's own verification
output showed an exception table. Renamed to `make check-cpu`.

No regressions: title screen still 99.98%, house-select unchanged, all other
suites pass, invariants clean.

Remaining failures are eight classes (`DIVS`, `DIVU`, `MOVEfromSR`, `MOVEM.l`,
`MOVEM.w`, `MOVEtoCCR`, `ROXR.b`, `SUB.b`) and ten instructions that are not
implemented at all. None of the unimplemented ones appear in this ROM's
histogram, but the skip counts keep that visible instead of assumed.

## Layer 4: Z80 exerciser

`tests/z80_zex.c` runs the Frank Cringle CP/M exercisers against `src/z80.c`.
The core takes its bus through externs, so the harness swaps the Mega Drive
memory map for a flat 64 KiB space and tests the shipping core unchanged.

`prelim.com` **passes** — basic instruction behaviour is sound.

`zexdoc` **fails**. It derails during the very first test group,
`<adc,sbc> hl,<bc,de,hl,sp>` — an ED-prefix group. Added derailment detection
(the exerciser never restarts itself, so a return to `0x0100` is proof of a
fault) with a PC ring buffer. The trail is unambiguous:

```
last PCs : 00E0 00E1 ... 00FE 00FF
opcodes  : all 00 (NOP) until 00FF:C3 (JP)
SP=0598  stack: 0010 0000 ...
```

Execution fell into the zero page below the load address, NOP-slid up to the
`JP` at `0x00FF`, and jumped back to the entry point. So the fault is a bad
address computed *upstream* — a wrong jump or a corrupted return — not a decode
failure where it lands.

The Z80 was completely unvalidated before this and is now a live suspect for
the house-select fault, which is exactly the elimination this layer exists to
provide.

### The Z80 bug: a stale HL cache

`zexdoc` derailed on its first group. The cause was structural, not a single
opcode: `z80_step` cached HL in the instruction context at entry and wrote it
back unconditionally at exit.

Every 8-bit write to H or L goes through the `REG8` table and never touched
that cache, so it was silently overwritten. A four-case probe made it obvious:

```
LD H,$55  -> H=11 L=22   (want H=55)
INC L     -> H=AA L=0F   (want L=10)
LD H,B    -> H=11 L=22   (want H=77)
DD EXX    -> IX=0000     (want 1234)
```

All four failed. `LD H,n` and `LD H,B` are among the most common Z80
instructions; it is remarkable `prelim.com` passed at all. Under a DD/FD prefix
the same write-back put HL into the *index* register, which is the fourth case.

Fixed by deleting the cache rather than patching the instances: `xget`/`xset`/
`xaddr` always read and write the live register, choosing HL or IX/IY from the
prefix. Caching a register other paths can modify was the bug; not caching it
removes the class. Zero `x.v` references remain.

All four probes now pass, `prelim.com` still passes, and the groups that
previously derailed report OK:

```
<adc,sbc> hl,<bc,de,hl,sp>....  OK
add hl,<bc,de,hl,sp>..........  OK
```

### It did not fix house-select

Title screen unchanged at 99.98%, invariants clean, no regression. But the
house-select path is byte-for-byte where it was: 984 blocks, 68 nametable
entries, same VRAM occupancy.

So the Z80 was a genuine bug and a reasonable suspect, and it was **not** the
cause of the rendering fault. Worth stating plainly rather than implying the
fix was more than it was. The remaining suspects are the VDP and timing — and
the full `zexdoc` run may yet surface more Z80 faults.

### zexdoc complete: 59 OK / 8 ERROR, then two more fixes

The full run finished — 46.7 billion cycles, 68 groups. The eight failures fell
into exactly two families, both real:

**Seven: undocumented index half-registers.** A DD/FD prefix on an instruction
that does *not* reference `(HL)` redirects the H and L operands to the high and
low halves of the index register (`IXH`/`IXL`/`IYH`/`IYL`). The core only
applied the prefix to `(HL)` addressing. Affected `aluop a,<ixh...>`,
`<inc,dec> ixh/ixl/iyh/iyl`, `ld <ixh...>,nn` and `ld <bcdexya>,<bcdexya>`.

The two readings never collide, which makes the rule clean: if the instruction
uses `(HL)`, the prefix supplies a displacement and H/L keep their meaning; if
it does not, H/L become the index halves. Computed once per instruction as
`x.halves` and consulted by `rd_r`/`wr_r`, which also let the previous ad-hoc
special cases in `LD r,r'` be deleted.

**One: accumulator rotates.** `RLCA`/`RRCA`/`RLA`/`RRA` differ from their CB
counterparts — S, Z and P/V are *preserved*, not recomputed. The code routed
them through the generic `rot()` helper, which overwrites all of them. It also
assigned `Z80.f` twice, the second assignment discarding the first. Now the
preserved bits are captured before the rotate and only C is taken from it.

No regression: probe passes, `prelim.com` passes, title screen still 99.98%,
all three other suites green, invariants clean.

House-select is unchanged at 984 blocks and 68 nametable entries, confirming
again that the Z80 is not that fault.

### zexdoc passes

Confirming re-run after the index-half-register and accumulator-rotate fixes:

```
67 groups OK, 0 ERROR, completed after 46,764,012,741 cycles
```

The Z80 core is now validated against the documented-flag exerciser. Three
bugs were found and fixed in total, none of them visible at game level:

1. a cached HL written back at instruction exit, discarding every 8-bit write
   to H or L, and corrupting the index register under a DD/FD prefix
2. undocumented index half-registers (IXH/IXL/IYH/IYL) unimplemented
3. accumulator rotates recomputing S/Z/P instead of preserving them

`zexall`, which additionally checks the undocumented flag bits, is fetched but
not yet run.

This closes the Z80 as a suspect for the house-select fault. Remaining
suspects: the VDP and timing.

## Layer 3: the execution oracle, and what it actually measures

Patched the reference core's 68000 instruction loop to emit `(PC, FNV-1a hash
of D0-D7/A0-A7)` per instruction, added the matching emission at block entry in
`dispatch.c` with a byte-identical hash, and wrote `tools/tracediff.py` to
filter the reference trace to block-start PCs and compare in order.

It works. First run located an exact divergence with the 12 preceding blocks
matching on both PC and register hash:

```
record 558505:  both at PC=000506
   db4a      reghash=4E281874
   reference reghash=013F56D0
```

`$4FE` is a VDP status poll — read status, test bit 1 (DMA busy), spin. Modelled
DMA duration (transfers are instantaneous, so a completion deadline drives the
busy flag), which changed block counts but not this divergence: both sides
*branch* identically and reach `$506` together. Only the value left in `d0`
differs — the status register's unused upper bits, which are open bus on real
hardware and which we return as a constant.

Extending the tool to report many divergences rather than the first showed the
real shape:

```
68763 divergent records of 627268 (10.96%)
   000FDA : 24602   wait-for-vsync
   000522 : 20165   VDP status poll
   000FEE :  6745   second wait loop
```

Every hot site is a polling loop. The cycle model is approximate by design, so
spin loops iterate different counts on each side and leave different scratch
values, then reconverge.

**Honest reading: the oracle currently measures timing divergence, not logic
divergence.** That is a limitation of how it compares, not of the trace
machinery — which does exactly what was wanted. Making it useful for logic bugs
means ignoring spin-loop noise: compare only where the two provably resync, or
exclude registers a polling loop scribbles on.

Reporting the first divergence alone would have been actively misleading here:
it looked like a single crisp fault, and it is one instance of 68,763 mostly
benign ones.

## Closing the CPU vector failures

Fixed three real classes; the fourth turned out not to be ours.

**DIVS/DIVU — overflow.** When the quotient does not fit in 16 bits the 68000
sets V and leaves the destination register **completely unchanged**. The code
computed and wrote the truncated result regardless. The vectors made it
unmistakable: expected CCR `0x0A` (N+V) with the destination still holding the
original dividend. 150/150 after the fix. This one probably mattered — the ROM
performs 29 divisions, and a silently wrong quotient corrupts whatever it feeds.

**MOVEfromSR — the trace bit.** Every difference was exactly `0x8000`. The SR
model had no trace bit at all, so bit 15 always read back as zero. Added it to
`m68k_t` and to `get_sr`, and — the part that actually made the tests pass —
to every `set_sr`, since discarding it on the way in makes reading it back
impossible.

**MOVEtoCCR — operand width.** `MOVE <ea>,CCR` is a *word* bus access even
though only the low byte reaches CCR. Reading a byte fetches the high half on a
big-endian bus, i.e. the wrong one entirely. 161/161 across
MOVEfromSR/MOVEtoCCR/MOVEtoSR after the fix.

**ROXR.b — a capstone bug, not ours.** `0xEC31` is `ROXR.B D6,D1`: bits 7-6 =
`00` (byte), bit 5 = `1` (count in a register), bits 11-9 = `110` (D6).
capstone reports `roxr.l #$6, d1` — wrong size *and* wrong count source.
`m68k-linux-gnu-objdump` agrees with the manual, not capstone.

I initially wrote that this "matters well beyond ROXR" because the recompiler
trusts capstone's `op_str`. **That was wrong and the data said so**: all 84
register-count shifts in the corpus (`lsl.l d1,d0` etc.) decode correctly
against objdump, and all three ROX instructions here are the immediate-count
form, decoded correctly. Impact on this ROM is zero.

It remains a live trap for any other ROM, so `recomp.py` now detects the
register-count ROX encoding and refuses to generate rather than emitting
silently wrong code. Failing loudly is the only acceptable behaviour when the
decoder underneath is known to lie.

```
make check-cpu -> 4666/4710   (was 4586)
title screen   -> 71680/71680 exact, 100.00%   (was 99.98%)
```

The title screen becoming pixel-perfect is a side effect of these fixes — the
last 13 differing pixels had been outstanding since the colour-ramp work.

Remaining: ROXR.b (capstone's fault, the harness feeds it a bad decode),
MOVEM.l/w (the `(An)+` with An in its own list case) and SUB.b. House-select is
unchanged at 981 blocks, so none of this was that fault.

## State-pointer handlers are statically discoverable after all

User report from `make play`: pressing B on house selection showed a
split-second of the world map, then `BAD PC 00024724`. The crash dump ended:

```
1: 001812      move.l $e002.w, d0     ; read the state pointer
0: 001818      movea.l d0, a0 ; jsr (a0)
A0 = D0 = 00024724
```

The game installed a new state handler and jumped into a block that did not
exist. But this one **was** discoverable: `$2603E` contains
`move.l #$24724, $e002.w` — the address is a literal in the ROM.

The tracer follows branches and calls, and nothing branches to these handlers,
so recursive descent never saw them. They surfaced only when play reached that
state. Added a pass that treats an immediate written to `$FFFFE002` as an entry
point, which is precise: that pointer *is* the dispatch mechanism, so anything
stored to it is by definition code.

```
instructions   : 31702 -> 33137
entry points   :   768 ->   796
jump tables    :    48 ->    50
```

All five literal handlers now decode: `$4500`, `$608E`, `$6D0C`, `$24724`,
`$24812`.

Replaying the user's path headlessly (Start at the title, B at house select)
no longer crashes, and the world map renders — advisor portrait, the Arrakis
territory map in the three house colours, briefing text:

```
distinct blocks : 981 -> 1287
VRAM            : 15.3% -> 39.6% non-zero
nametable A     : 68 -> 2094 entries
sprite table    : 40 -> 263 bytes
```

No regression: title screen still 100.00% exact, invariants clean, three test
suites green, vectors 4666/4710.

Two things worth recording. **Interactive play found a state four sessions of
scripted testing never reached** — the scripts only ever pressed Start.
And this was a static-analysis gap masquerading as a runtime-dispatch problem:
the bootstrap loop would have caught it by replaying, but only after someone
reached that screen, whereas the ROM had the answer in an immediate all along.

House-select is still 981 blocks and 68 nametable entries, so it remains its
own separate fault.

### The A0-return idiom, and three attempts at the right heuristic

Second crash from interactive play: `BAD PC 00049B1E`, reached by pressing B on
house selection. The trail showed ~60 blocks of a new subsystem executing
cleanly first.

The cause is a hand-written calling convention:

```
049B18  lea.l $49b1e(pc), a0     ; load the return address
049B1C  bra.b $49b4c             ; enter the routine
049B1E  ...                      ; routine returns here via jmp (a0)
```

Nothing *branches* to the return point, so recursive descent never reached it.
(The routine itself is a 32-bit divide built from 16-bit `DIVU`, branching on
the V flag — the overflow semantics fixed earlier the same day.)

Getting the heuristic right took three goes, each corrected by measurement:

1. **Seed every `lea` target** (validated). +6455 instructions, but
   `unimplemented` went 0 → **3112**. Real code in this ROM is 100% emittable,
   so that number is a direct measure of data being decoded as code. Too loose.
2. **Restrict to nearby PC-relative targets.** Better — 3112 → 771 — but still
   far from clean.
3. **Match the idiom exactly**: a PC-relative `lea` whose *immediately
   following* instruction is an unconditional transfer. **3 seeds.**

Then a second bug surfaced: 769 instructions still would not emit, all of them
`dc` — capstone's marker for data it could not decode. Unlike a failed decode,
`dc` "succeeds", so the tracer walked straight through data tables. Added it as
a stop condition.

That fix appeared to do nothing, because the mnemonic is `dc.w`, not `dc` — and
the check I wrote to *verify* the fix had the identical blind spot, so it
reported "dc count: 0" while 769 sat there. Comparing the base mnemonic fixed
both.

Final state — precise rather than merely larger:

```
instructions   : 33147   (+10 over the pre-lea baseline)
lea seeds      : 3
state seeds    : 4
failed decodes : 0
unimplemented  : 0
```

Verified: title screen 100.00% exact, world map path no longer crashes
(1287 blocks, 2094 nametable entries), three suites green, vectors 4666/4710.

**`unimplemented` turned out to be the useful signal throughout.** It is a
proxy for "am I decoding data as code" that costs nothing to read, and it was
what refuted attempts 1 and 2 within seconds each.

## Scripted playthroughs

Interactive play kept reaching states the automated tests never did, because
those only ever pressed Start. `tests/playthrough.py` closes that gap: a
scenario is a list of `wait` / `press` / `shot` / `mash` steps, compiled into
an input script and a set of capture frames, run headlessly with a screenshot
per screen and a report of exactly where it stopped.

    make playthrough SCENARIO=house

`main.c` gained `DB4A_SHOTS` for mid-run capture — a playthrough needs to see
each screen it passes through, and the interesting one is rarely the last.

The `house` scenario replays the reported route: title, Start, B to select a
house, then repeated B through the advisor briefing. It captures the title,
house-select and briefing screens, all rendering correctly.

**The first version did not reproduce the crash**, which was itself
informative. A sweep over press timing and count showed the trigger is the
*count*, not the timing: 8 presses completes cleanly, 14 reaches the dispatch
at `$49C14` reliably at every gap from 60 to 200 frames. The scenario now
mashes properly and fails loudly.

That is the real value here — a bug found by hand is now a one-command
regression test that either crashes or does not.

Current end state: 1651 distinct blocks, invariants clean, crash at `$49C14`.
That address is reached through a computed dispatch:

```
049C08  suba.l #$49c0e, a0
049C0E  lea.l  $49c0e(pc, a0.l), a0
049C12  bra.b  $49c2c
049C14  <dispatch arms start here>
```

A position-independent jump: normalise `a0`, re-add the PC-relative base, and
the arms follow the branch. Same family as the A0-return idiom, different
shape.

### pea return addresses, and the limit of static discovery

`$49C14` turned out not to be a jump table at all — my first reading was wrong.
It is a return address pushed with `pea`:

```
049BFC  move.l  a2, -(a7)
049BFE  pea.l   $49c14(pc)            ; push the return address
049C02  movea.l #$49b1e, a0
049C08  suba.l  #$49c0e, a0           ; position-independent address
049C0E  lea.l   $49c0e(pc, a0.l), a0  ; arithmetic, not a dispatch
049C12  bra.b   $49c2c                ; enter the routine
049C14  <returns here via rts>
```

The same idiom as the A0-return case, using the stack instead of a register.
Generalised the seeding pass to cover both shapes. The frame-relative form
(`pea -$12(a6)`) is excluded by the pattern, and the validity probe correctly
rejected `pea $492e4(pc)` — that one points backwards and is a data pointer
passed as an argument, not a return address.

```
lea/pea targets seeded : 11
instructions           : 33160  (+13)
unimplemented          : 0
```

The reported route now gets much further: **1651 -> 2538 distinct blocks**.

It then stops at `$19DB8`, and this one is different in kind:

```
019FE4  jsr (a2)
```

A2 holds a function pointer loaded at runtime. Nothing in the ROM references
`$19DB8` statically — no `lea`, no `pea`, no immediate. This is a genuine
virtual call, and no amount of static analysis will find it.

**That is the limit.** Each fix so far revealed the next undiscovered target,
and the remaining ones are runtime-computed by construction. Continuing to
chase them individually is whack-a-mole; the answer is to discover them
automatically by replay, which `tools/bootstrap.sh` already does — it just
needs to be driven by a realistic playthrough rather than a single Start press.

## Bootstrap: runtime discovery driven by a playthrough

`$19DB8` was reached by `jsr (a2)` with the pointer loaded at run time. Nothing
in the ROM references it, so static analysis cannot find it — and the same is
true of the family behind it. Chasing these one at a time was whack-a-mole.

`tools/bootstrap.sh` already learned unknown PCs by replay, but drove the game
with no input, so it only ever explored the title screen. It now replays a
scripted playthrough via `playthrough.py --emit-press`.

**Its stall detector was wrong first time round.** It compared seed-file counts
before and after a run, so a seed left behind by an earlier interrupted run
looked like "no progress" and stopped the loop after one iteration. A seed
being *already recorded* is not the same as it being *already applied*. The
correct condition is narrower: the same address still unknown after
regenerating with it.

```
iter  1: 2997 blocks
iter  5: 3203  -> 02E30E     (the address reported from play)
iter 25: 3937  -> 044C78
31 seeds
```

**It hit the 25-iteration cap, not convergence** — new entries were still
appearing on the final iteration. More runs are needed.

Verified after a clean rebuild: title screen 71680/71680 exact, three suites
green, all 35546 instructions parse, and the reported route now completes
without crashing (3966 distinct blocks, up from 2538).

### Seeds are discovered data, not derived output

Ground rule 2 says everything in `build/` is reproducible and therefore not
tracked. These seeds sit awkwardly against that: reproducing them costs a
25-iteration bootstrap run, and losing them means doing it again. They are the
*output of an experiment*, not a function of the ROM.

Moved to `data/seeds.txt` and tracked, with the reasoning in the file header.
`build/seeds.txt` remains the scratch file a running build appends to; the
tracer reads both.

### Jump-table slots are entry points, not just their targets

Three crashes reported from play at `$6E78`, `$6E80`, `$6E88` — eight bytes
apart, which looked like an 8-byte stride the resolver did not know. **That
reading was wrong.** The dispatch is:

```
006E6E  andi.w  #$f, d0        ; index 0-15
006E72  lsl.w   #$2, d0        ; x4
006E74  jmp     $6e78(pc, d0.w)
006E78  6000 003E  bra.w ...   ; 16 slots, 4 bytes each
```

Stride 4, `BRA_W` — a format already handled, and the table had in fact
resolved perfectly, with all 16 correct targets recorded.

The bug was subtler. `jmp $6e78(pc, d0.w)` jumps **into the table**: it lands
on a `bra.w` slot which then branches onward. The slots are themselves
executable addresses, but only the branch *targets* were being seeded. The
first slot survived by fallthrough, so arm 0 worked and every other arm died —
which is exactly the pattern the reports showed, one new address per attempt.

Fixed by seeding each slot of a `BRA_B`/`BRA_W` table alongside its targets.
This is general, not specific to that site: **82 slots across 11 dispatches**
were previously unreachable, so a whole family of latent crashes went with it.

Verified: title screen 71680/71680 exact, three suites green, 35624
instructions parse, scripted route completes without crashing.

Task 17 (infer stride from the scaling instruction) was filed on the wrong
diagnosis and is no longer the issue here, though deriving the bound from
`andi.w #$f` rather than only `cmpi` would still be an improvement.

## Button sweeps: the coverage the scenarios were missing

The `house` scenario only pressed Start and B, so it selected one dispatch arm
and stopped at the briefing. Every crash reported from real play turned out to
be a *later arm of a table whose first arm worked fine* — an arm is reached
only if something selects it, and no amount of waiting selects arm 7.

Added a `gameplay` scenario: through the menus into the mission, then sweep all
eight inputs three times, the d-pad six times, and the action buttons four
times. 82 inputs over 7476 frames, with `sweep()` alongside `mash()`.

It reached a crash the menu route never approached on its first run
(`$2E310`, 4283 distinct blocks against 3966).

Driving bootstrap with it then **converged** — the first genuine convergence
rather than an iteration cap:

```
iter 1: 4283 blocks -> 02E310
iter 6: 4475 blocks -> converged, no unknown PC
```

Five new seeds, 36 tracked in total.

Verified after a clean rebuild:

```
title screen   : 71680/71680 exact
suites         : 3 green, 35739 instructions parse
house route    : 3966 blocks, no crash
gameplay route : 4462 blocks, no crash
```

The lesson is about test design rather than emulation. Chasing each reported
address in turn was treating symptoms; the cause was that the automated route
exercised a fraction of the input space while real play exercised much more.
Pressing everything, then letting replay-based discovery converge, closed the
whole family at once.

## The house-select screen: the window plane was never implemented

Reported as "looks broken while functioning just right" — the screen showed a
handful of stray gold fragments, yet a house could still be selected and the
game proceeded correctly.

Added a `houseselect` scenario that stops on that screen, and
`tools/compare_screen.sh`, which drives db4a **and** the reference emulator
through the same input script and diffs the result. Both sides receive
identical inputs, so any difference is ours.

At frame 2750: 75.93% exact. The reference showed the full screen; ours was
2% non-black. Sampling five frames over 700 showed it never filled in —
oscillating between 1.1% and 2.1% — so not a loading delay.

The VDP state pointed straight at it:

```
tile area 0000-B000     : 22.1% non-zero     <- graphics ARE loaded
cram                    : 60/64              <- palette IS loaded
nametable A @E000       : 68/4096
nametable B @C000       : 0/4096
window nametable @6000  : 334/4096           <- the screen is HERE
grep window src/render.c: 0 matches          <- and we never drew it
```

The window is a third tilemap layer that replaces plane A over a rectangular
region and does not scroll. `reg17=0x14` and `reg18=0x1E` put it over the whole
display, so the game had legitimately drawn everything to it and plane A was
correctly almost empty. Nothing was broken except that we never looked at the
layer holding the picture — which is exactly why it *functioned* perfectly.

Implemented `window_covers()` and `sample_window()` per the register semantics:
reg17 bits 0-4 give the horizontal split in 2-cell units with bit 7 selecting
which side, reg18 the vertical split in cell units, and the window nametable is
always 64 entries wide in H40 regardless of the plane-size register.

```
house-select frame 2750 : 75.93% -> 100.00% exact (71680/71680)
frames 2800, 2950, 3400 : 100.00%
frames 2700, 3100       : 99.49%
```

The residual 368 pixels form an 80x16 rectangle outline at x=32-111,
y=136-151 — the selection box around the first plaque, caught in opposite
blink phases. Benign, and it vanishes at the frames where the phases align.

No regressions: title screen still 71680/71680, three suites green, both
scripted routes complete without crashing.

**A note on my own analysis.** Localising that residual, I first compared raw
bytes and got 15787 mismatches spanning a huge region — then remembered the
comparison must be quantised through RGB565 to match the oracle's path, which
gives 368 in a tight rectangle. I had made and documented that exact mistake
earlier in the project and still repeated it. `framediff.py` already emits a
quantised diff map; hand-rolling the comparison is what reintroduced the error.

`make compare-screen SCENARIO=houseselect FRAME=2800` now wraps this up.

## Attempting a concrete slab: infrastructure yes, sequence no

Goal: script the first real gameplay action end to end.

**Delivered.** Three scenarios — `mission` (reach the map and stop), `cursor`
(step one input at a time with a capture after each), `slab` (drive the build
interface) — plus `make compare-screen`, which drives db4a *and* the reference
through identical inputs and diffs the result.

**Found and fixed three silent harness defects** while trying to move the
cursor. The button-name lookup had no `left` or `right`, so those presses
parsed, matched nothing and were discarded; the script array held 16 entries
and its buffer 256 bytes against a scenario of 82 inputs and ~700 bytes; and
none of it warned. Every horizontal input ever scripted had been dropped.

That invalidated an earlier claim: the "gameplay sweep converged" result was
obtained without ever pressing left or right. Re-running bootstrap with a
working d-pad converged again in 7 iterations, found 6 more seeds including
`$7468` — the `jsr (a5)` dispatch reported from interactive play — and took the
gameplay route from 4462 to **5260 distinct blocks**.

**What the interface does**, read off the captures: `a` selects the
Construction Yard and opens the side panel; `a` again opens the full build
interface (EXIT/FIX/STOP tabs, two build thumbnails, preview pane); `down` and
`right` move the selection within it; `a` exits. `b`, `c`, `start`, `up` and
`left` do nothing there.

**Not achieved: no slab was built.** No combination tried produced the credits
change (990 -> 976) that marks a build starting.

The useful part is *why*. Across the whole sequence db4a matches the reference
at **99.58%–99.71%**, and the reference does not build a slab either. Both
implementations behave the same; the input sequence is simply wrong. This is a
gap in my knowledge of the game's interface, not a defect in the emulation —
and having the oracle is what makes that distinction available at all rather
than leaving it as "it doesn't work".

Next step is to find the confirm action, most likely a press-and-hold or a
two-button combination the single-press sweep cannot express.

### Building a concrete slab

With the sequence supplied from real play, the scenario works: select the
Construction Yard with `a`, then `down` + `a` to order the slab, wait, then
`down` + `a` to place it.

Observable progression across the captures:

```
00-arrive          credits 990
01-yard-selected   side panel opens
02-slab-ordered    credits 983    <- build started
03-building        progress shown
04-placement-mode  striped overlay across the yard
05/06-settled      credits 976, "OK" indicator on the yard
```

Against the reference at every stage: **99.57%, 99.57%, 100.00%, 99.59%,
99.58%**. One frame exact, the rest differing by ~300 sprite-animation pixels.
So the whole build interaction is faithful, not merely reachable.

`make playthrough SCENARIO=slab`.

**Three times in this task an ad-hoc pixel-count proxy gave a confidently wrong
answer** — a "progress bar" region that returned identical values for 700,
1500 and 2500 frame waits, and a "stripe detector" that reported the overlay
gone while the screenshot plainly showed it. Each time the fix was to stop
measuring a made-up region and either look at the frame or run
`compare_screen.sh`. The lesson is the one already written down after the
RGB565 episode: use the instrument that is calibrated, do not invent a new one
per question.

### Placing the slab

The first attempt ordered the slab correctly but never placed it: placement
starts centred on the Construction Yard, which is not a valid site.

Walking the cursor down clear of the yard is required, and the distance
matters:

| downs | result |
|-------|--------|
| 1 | leaves placement mode; the confirm reopens the build menu |
| 2 | **valid site adjacent to the yard — slab placed** |
| 3 | site out of range, overlay turns red, confirm refused |

That also corrected an earlier misreading. In the single-press sweep I had
recorded `a` as "does nothing" in placement mode, because the overlay stayed
on screen. It was not doing nothing — it was **refusing an invalid site**.
`a` is the confirm throughout.

Final state, verified against the reference: both place an identical 2x2
concrete block adjacent to the Construction Yard, credits 976 in both.

```
01-yard-selected  99.57%
02-slab-ordered   99.71%
04-placement-mode 99.61%
05-cursor-moved   99.53%
07-settled        96.17%
```

The last frame's larger gap is a selection border still drawn around the yard
in the reference plus sprites a frame or two out of phase — not a placement
difference. The slab itself is pixel-identical.

`make playthrough SCENARIO=slab` now drives an entire gameplay action —
select, order, build, position, place — and captures every stage.

### Extending to power station and refinery: partial

Extended the scenario with `build_item()` covering all three structures --
`down` for the slab, `down right` for the power station, `down right right` for
the refinery.

Running it exposed `$2E314`, another slot of the table at `$2E30E`. The
slot-seeding added earlier only covers `BRA_B`/`BRA_W` tables and this one's
first arm is `rts`, so it was not recognised as an instruction table at all.
The dispatch states the answer outright:

```
andi.w #$c000, d0      2 bits set -> 4 possible values
rol.w  #$3, d0         -> offsets 0, 2, 4, 6
jmp    $2e30e(pc, d0.w)
```

`enumerate_offsets()` now walks back for a mask bound and the scaling applied
to that register, enumerates every value the mask allows, and pushes each
through the scaling. That gives the reachable offsets directly with no guess
about entry format or size. This closes task 17, which had been filed as
"infer the stride" -- the fix is enumeration, not a better guess.

Replay discovery then converged in 6 iterations (5 seeds, 47 tracked), and the
route runs all eleven stages without crashing.

**But only the slab is actually built.** The final frame shows the 2x2 concrete
block and credits at 976 -- the same value as after the slab alone. No power
station or refinery appears.

Worth recording how nearly I got this wrong. A credits-hash proxy showed
changes at the power and refinery stages, which reads as "purchases happened".
The frame shows otherwise: those were transient display changes. **Four times
in this task an ad-hoc pixel proxy has given a confident wrong answer** -- a
progress bar identical across three wait lengths, a stripe detector that missed
a visible overlay, a stale-file glob, and now this. The reliable moves have
been looking at the frame and running `compare_screen.sh`.

Likely cause for the missing structures: `build_item()` re-selects the yard
with `a` for each item, but the interface state after a placement is not the
same as after arrival, so `down right a` probably does not land on the intended
item. Needs the same empirical stepping used to find the slab sequence.

## Input recording and replay

Scripted scenarios express *intent* — "press down twice" — and then have to
guess at things the game never makes observable: how far a cursor travels, where
the camera scrolls to, which tile ends up under it. Several attempts at building
a power station failed on exactly that, and each fix was a guess at geometry.

Recording removes the guessing. `src/inputlog.c` captures every press and
release with its frame number, and replays the file back into either frontend.

```
make record REC=data/recordings/slab.txt      # play; inputs are saved
make replay REC=data/recordings/slab.txt SHOTS=6000,9000
```

The format is plain text so a recording can be read, edited and diffed:

```
# db4a input recording
# frame button down
2400 start 1
2406 start 0
```

Validated before use rather than after: a recording generated from the known
good scripted slab sequence replays to 5399 distinct blocks, matching the
scripted run.

Two details that would otherwise bite:

- Keyboard auto-repeat is filtered (`e.key.repeat`), or holding a direction
  records a stream of phantom presses.
- The headless run auto-extends to cover the recording *and* any requested
  capture. Without the second half, a screenshot scheduled after the last input
  silently never happens — which it did on the first attempt.

While replaying, live keyboard input is ignored, so a replay cannot be
accidentally perturbed.

## 2026-08-22 — input: auto-repeat, and three keys that never arrive

Three separate problems wearing the same costume ("button X does not work"),
worth separating because only two were ours.

**Auto-repeat (real bug, fixed).** Pressing A logged `DOWN` then immediately
`up` while the key was still held. Auto-repeat emits a KEYUP/KEYDOWN pair per
repeat and the repeat flag marks only the KEYDOWN, so filtering on
`e.key.repeat` dropped the re-press but honoured the release, latching the
button off for the rest of the hold. Whether a key was affected came down to
whether it was held past the repeat delay, which is why it looked key-specific.
Pad state is now polled from `SDL_GetKeyboardState` once per frame; a key that
goes up and back down inside one frame reads as held, which is correct.

**Poll matching (real bug, fixed).** The first polled version checked only two
scancodes -- the US-layout position and `SDL_GetScancodeFromKey` -- while the
event path had matched on the layout-aware keysym. Keys that had matched by
symbol stopped matching entirely. The poll now walks the keys that are actually
down and asks SDL what symbol each produces.

**E, Z and C never reach SDL (not ours, deferred).** SDL reports Q, W, X, Enter
and the arrows as held and never reports E, Z or C at all. Not the PAD_C
plumbing: driving PAD_C headlessly gives `TH=1 -> 5F` (bit 5 clear) and
`TH=0 -> 33`, both correct. A/B/C therefore default to Q/W/E, and `DB4A_KEYS`
remaps any button at runtime so a key the machine will not deliver never needs
a rebuild to work around. See task #18.

The lesson worth keeping: three "the button does not work" reports had three
different causes, and the useful signal each time came from the user's
description of the TIMING ("prints DOWN and UP with no delay"), not from the
symptom. The pad log now reports reads while a button is held, and names every
key SDL sees, so the next one of these starts from evidence.

## 2026-08-22 — the PAL frame is 152,923 cycles, not 152,009

Correcting the entry above. It reasoned "the 68000 runs at 53203424/7 =
7,600,489 Hz, so a 50 Hz frame is 152,009 cycles". The premise is wrong: the
PAL Mega Drive does not run at 50 Hz. A PAL frame is 313 lines of 3420 master
clocks = 1,070,460, giving 1070460/7 = **152,922.86** cycles at **49.7015 Hz**.
Genesis-Plus-GX reports 49.70 fps for this ROM.

The error gave the CPU 914 fewer cycles every frame, 0.6% less work than
hardware. Menus never showed it. Gameplay did: the map's smooth scroll moves by
a relative delta rather than toward an absolute target, so a transient rate
difference became permanent, and gameplay frames sat 3 pixels off the reference
forever after. Fixing the frame length took frame 6000 from 64.63% to 99.35%
exact and made the camera variable $FFE3BA agree exactly.

**How it was found**, which is the transferable part. Pixel comparison said
"3 pixels down matches at 96%" and nothing more; I twice drew a wrong conclusion
from it, first blaming mid-frame scroll and then building a per-scanline
renderer that changed no output. What settled it was comparing 68K work RAM
between the two emulators: $FFE3BA held 46 against the reference's 43, which
proved the GAME had computed a different camera and the renderer was innocent.
Bisecting RAM frame by frame then showed agreement through frame 84 and a
counter at $FFE007 running exactly 2 frames behind from 94 onward -- a phase
error in a timed sequence, which points at frame length rather than arithmetic.

A screen diff tells you something is wrong. A memory diff tells you what.

(Genesis-Plus-GX returns work RAM byte-swapped within each 16-bit word, the same
x86 trap CLAUDE.md records for `od -t x2`. Un-swap before diffing.)

## 2026-08-22 — audio, from silence to a game with a soundtrack

Went from both sound chips stubbed to music and effects throughout. Five real
bugs, and the pattern of how they were found is the part worth keeping.

**The PSG is a red herring for this game.** Implemented and unit-tested first,
on the theory that a small chip would get a pipeline in place. It did, but Dune
writes the PSG 38 times at init to mute all four channels and never touches it
again: its output is *exactly* zero for the entire mission. All the sound is
YM2612. Recorded so the silence is never mistaken for a defect.

**The driver stall.** Music crawled, then stopped entirely a minute in. Both
were one bug: `psg_run`/`ym_run` were called once at the END of each frame, so
the YM2612's timers could not advance while the Z80 was executing. Dune's driver
writes 0x15 to register 0x27 and then spins on `bit 0,(hl)` waiting for the
Timer A overflow — it was polling a flag that was physically unable to change,
burning its whole slice, and getting one timer event per frame instead of the
~10000 it expects. The chips now advance before the Z80 runs each slice.

**Z80 instruction timing.** Every non-indexed CB-prefixed instruction was 4
T-states heavy, because the tables carry the full documented cost including the
prefix fetch and `z80_step` had already charged 4 for it. `bit 0,(hl)` cost 16
instead of 12 — the exact instruction the driver spins on, and it is CPU-bound
there, so the music ran slow. Fixing it took RMS against the reference from
622.5 to an exact 599.6.

zexdoc had been green throughout. It proves the core computes the right
RESULTS and says nothing about how long anything takes, so this class of bug was
invisible to every test we had. `tests/test_z80_timing.c` now covers it.

**Modulation depth was 4x too shallow**, and the LFO did not exist at all
(register 0x22 ignored, though Dune enables it). Both show up as *per-effect*
volume errors rather than a global one, because how bright an FM voice is
depends entirely on modulation depth.

**A DC offset from the DAC.** It holds its last sample once the driver stops
feeding it; on hardware nothing downstream passes DC, but here the held value
became a -708 offset that the whole mix rode on and clipped against. That was
the "engine sound never clears, then everything is broken" report. Fixed with a
one-pole high pass at ~27 Hz, matching the console's AC-coupled output.

### What went wrong in the debugging, which is the transferable part

Three of my own measurements actively misled me:

- I concluded Timer A was running at 54% speed. It was not: the timer only ticks
  while enabled, and the game does not start it until the music begins.
- I blamed a level decay on the DC blocker. It was a missing accumulator reset I
  had introduced myself one commit earlier, which made `sum_n` grow without
  bound so each output sample averaged an ever-longer stretch — a 1/n decay that
  looks exactly like an envelope fault.
- `DB4A_REPLAY` silently extended every run to cover the whole recording, so a
  series of runs at 200, 600 and 1500 frames all simulated 27609 and returned
  identical numbers. I read that as "the driver has stopped".

The envelope tests failed twice for reasons that were the test's fault, not the
chip's: measuring the PEAK across a release window reports its loud first
milliseconds, and reading the ring buffer straight after a key-off returns audio
generated before it. Both made a working chip look broken. A test for a stuck
note that errs toward "stuck" is worse than no test.

Meanwhile every single fix originated in the user describing what they heard —
"too slow", "the engine sound never clears", "only clicks after house select",
"the money counter is too loud". The automated tempo comparisons never resolved
anything: onset-flux correlation peaked at 0.18 with 0.97x, 1.14x and 1.27x
indistinguishable. A described symptom beat a correlation score every time.

Remaining work is task #22, deferred as low priority: in-game audio is still
wrong, the intro has "weird slowdowns", and the money-counter effect is too
loud. Prime suspect is SSG-EG, which is parsed and ignored.

## 2026-08-22 — closing the roadmap

Finished the remaining milestones in one pass. What each turned out to need:

**v1's last criterion — a Defeat.** Not an emulator problem at all: mission 1
is the tutorial and has no enemy, so idling there for 28 minutes of game time
leaves the base untouched and credits drifting from 990 to 985. Defeat needs
mission 2, which needs mission 1 won. `tests/defeat.sh` replays the recorded
victory, presses through the briefing, then stops touching the controls. Defeat
lands by frame 90000. All eight v1 criteria met.

**M7, the campaign.** Playing 27 missions by hand is not a test anyone will
re-run, so this is scoped to demonstrating the campaign's structure: all three
houses load with the right faction colour, none is a special case over a
40000-frame soak, missions progress, and both outcomes are reached. What is not
claimed is written into the roadmap rather than left implied.

**M8, modern enhancements.** Save states, pause, fast-forward, on top of the
remappable input and recording that already existed. All frontend-only — the
emulated machine runs identical frames whether they are used or not, which is
what keeps the faithful-first policy intact.

**The fallback interpreter was retired rather than built.** The original
reasoning was sound: the state machine dispatches through a function pointer at
$FFFFE002, so a PC no static pass predicted is always possible. But five seeding
passes closed the gap, and mission 1 to Victory, mission 2 to Defeat, and soaks
of all three houses now run with no unknown PC at all. An interpreter would also
have had to be generated from semantics.py to avoid two implementations
disagreeing — a lot of machinery for a case that no longer arises. The remaining
insurance is cheaper: an unknown PC appends itself to build/seeds.txt and says
what to run.

### The save state test earned itself immediately

Writing `tests/savestate.sh` — save at frame 5000, play to 5600, reload, play to
5600 again, require byte-identical frames — caught two bugs the moment it ran.
The save recorded `pc`, the reset PC, rather than `end`, the live one the loop
advances. And it recorded the current frame although the save happens AFTER
system_frame, so a resumed run re-executed that frame. Either alone produced a
state that loaded without complaint and was wrong.

That is the same lesson as the audio work, in a different key: a feature that
"looks like it works" and a feature that IS correct are separated only by a test
that would fail if it weren't.

### What is deliberately left open

Task #22, audio fidelity, is the one visibly short result: in-game audio is
still wrong and SSG-EG is parsed but ignored. Task #21 is a 0.8% gameplay pixel
difference that is unit positions, not rendering. Task #18 is three keys that
never reach SDL on the development machine, which is environmental. All three
carry what has already been ruled out, so none starts from scratch.

## 2026-08-22 — the cursor could never have followed the mouse

Mouse control worked but the cursor trailed the pointer badly. The assumption
was that the steering was mistuned. It was not: the cartridge cannot move its
cursor that fast, and a second dead zone nobody had found was dragging it back.

**Finding the code.** Added a RAM watchpoint, `DB4A_WATCH=FFBF12`, which prints
every write to an address together with the block that made it. Two writers
appeared. `$4BDC` was a red herring — it adds a pending delta that is almost
always zero. The real one was `$706C`.

**What the ROM does.** Cursor at `$FFBF12`/`$FFBF14` in screen pixels, with
sub-pixel accumulators at `$FFBF3C`/`$FFBF3E` and a 16.16 speed at `$FFBF40`
that ramps by `$1000` a frame and caps at `$30000`. Three pixels a frame at
50 Hz is 150 px/s: over two seconds corner to corner. No steering policy can
beat that.

Worse, `$706C` scrolls the map whenever the cursor leaves **X 120..200,
Y 82..142** — a box a quarter of the screen wide — and pulls the cursor back
towards it. Warping the cursor to (40,40) and watching it crawl back to
(120,82) is what identified it. The 24-pixel clamp box at `$FFBF1A` that had
looked like the culprit was a side issue.

**The fix.** Write the cursor position directly, and own the routine. Both
wanted changes are governed by two thresholds per axis, a shift and two speed
caps, all instruction immediates — patching a dozen of those in the ROM image
would work and would be unreadable. `src/cursor.c` is the first native
override: one block entry replaced by C that does the same job and returns the
same next PC. `docs/natives.md` has the method.

**Three things went wrong, all worth keeping.**

*The equivalence checker passed while the run diverged.* It compared RAM, the
cycle count and the exit PC — 9319 calls, zero mismatches, visibly different
frames. The override was leaving `d0`-`d2` and the X flag holding the caller's
values, and the code after the exit reads them. A checker that does not compare
registers finds nothing and reads like success. Fixed by comparing the whole
CPU; the arithmetic now goes through the same `add16`/`sub16`/`cmp16` helpers
the generated code uses, so the flags are right by construction rather than by
a second reading of the manual.

*Then it still diverged.* With registers compared and every call matching
exactly, `DB4A_NATIVE=1` and `=check` produced identical frames to each other
and different ones from `=0`. That grouping was the clue: check mode discards
the C's results entirely, so the cause could not be the C. It is atomicity.
`m68k_run_until` tests the slice deadline *between* blocks; the routine is
eight blocks and ~1000 cycles against a 500-cycle slice, so the cartridge
always yields to the Z80 in the middle of it and an override cannot. 0.62% of
pixels at frame 6000 — the same order as task #21. The override is therefore
gated on mouse control, and a faithful run is bit-exact. Task #23 is the real
fix: interleave on absolute cycle position rather than block boundaries.

*The control that should have been first.* Two runs of the same configuration,
to establish the harness is deterministic, before drawing any conclusion from
two runs of different ones. It cost a detour into a semantics bug that was not
there.

Verified: `make check-native` (9319 calls, 0 mismatched; faithful frames
identical), all unit tests, `check-state`, `check-houses`, `tests/mouse.sh`,
the full 27609-frame winning mission, and `tests/defeat.sh`.

## 2026-08-22 — clicking icons in the build console

Extending mouse control from the map into the Construction Yard's build
console. The research is in `docs/buildmenu.md`; three things are worth
repeating here.

**It is not a scene.** The obvious approach was to look for a new value at
`$FFFFE002` and gate on it, the way map steering gates on gameplay. A full
winning mission produces only eight distinct scenes and the console opens and
closes entirely inside `00006D0C`, so there was nothing to gate on. What works
instead is execution rather than state: the console is open exactly when its
input handler at `$8462` runs, which the dispatcher already sees. No RAM flag
was found that distinguishes the two states — `$FFBF86`, `$FFBF32`, `$FFBFB4`
and `$FFC728` all read the same either way, and the cell table keeps its
contents after the console closes so it cannot be used as a proxy.

**The technique that fixed the map cursor is wrong here.** The map cursor is
warped by writing `$FFBF12`, so writing `$FFBF8A`/`$FFBF8C` looked like the
same job. It is the opposite case. The map cursor tolerates a direct write
precisely because the ROM recomputes everything downstream from it every frame;
the code after `$84E8` instead writes the item code to `$FFBFA1`, calls `$8302`
to redraw the preview panel and price, and updates the list scroll. Writing the
words moves the highlight and leaves the panel describing the previous item. So
this synthesises presses — and gets hover previews for free, because moving the
highlight is what makes the game redraw the panel. The objection that sank
presses for the map cursor does not apply to a 3x6 grid.

**The grid is ragged, so a per-axis walk gets stuck.** The handler refuses to
move onto a cell holding `$80`, and empty cells are the normal case early in a
mission when two items are buildable out of eighteen. Stepping down from STOP
at (0,2) is refused; the walk has to cross the top row first. Each step now
checks the destination before pressing, and falls back to the other axis.

The byte-write watchpoint was what made this tractable at all: the navigation
state looked like a byte at `$FFBF8B` and was invisible until `m68k_write8` was
hooked, at which point it turned out to be a word at `$FFBF8A`.

Verified: `make check-menu` (all three top buttons, both items, an empty cell, a
point off the grid, and the `DB4A_MENU_MOUSE=0` escape hatch), plus the whole
suite unchanged — unit tests, `check-native` still bit-exact, `check-state`,
`check-houses`, `tests/mouse.sh`, the full 27609-frame winning mission and
`tests/defeat.sh`. Tagged `pre-menu-mouse` beforehand as a revert point.

## 2026-08-22 — the pointer on the pre-mission screens

House selection and the mentat's YES/NO now take the pointer. `src/menus.c`,
`docs/menus.md`, `make check-menus`.

**Neither screen stores an index.** Both keep the highlight's *position* and
read the choice off it: the house is decided by comparing `$FFBEF8` against 32,
120 and 208 at the moment you confirm, and the answer by the selector's sprite
Y. That makes them easy to drive and impossible to drive by writing an index
that does not exist.

**The mentat probe went on the wrong block first.** `$25CF4` writes the
selector's three sprites, which looks like the detector you want — but it only
runs while the selector is travelling, so it reported "no screen" precisely
when the screen was idle and waiting for input. Every pointer test came back
"YES" regardless of where the pointer was. The probe belongs on `$25CAE`, the
loop head. That screen turns out not to be a dispatched handler at all: it runs
its own loop, waiting for vblank itself at `$FD4`.

**A RAM diff can point at the wrong number.** The diff showed the byte at
`$FFA62D` going `$28` to `$40`, and comparing against those never matched — the
word at `$FFA62C` holds `$128` and `$140`, because Mega Drive sprite Y carries a
+128 offset. Reading it back and printing it was what settled it: `$128` is
screen y 168 and `$140` is y 192, exactly where the plates were measured.

**A passing invariant became the wrong invariant.** `tests/mouse.sh` asserted
that steering never holds the d-pad outside gameplay, which was right when only
the map cursor existed and is wrong now that menu steering owns the d-pad on
the screens it drives. It failed on scene `004500` — correctly, because
steering was selecting a house. The rule is now "never on a screen nothing
claims", checked against what the steerers actually report rather than against
a list of scene numbers, so it cannot go stale the same way again.

A side effect worth knowing: that test aims at (160,110), which lands on the
Ordos shield, so its scripted route now plays Ordos rather than Atreides. The
check only asks that a mission was reached, so it still holds.

Verified: `make check-menus` (three shields, both answers, two off-target
cases, the escape hatch on both screens), and the whole suite unchanged --
unit tests, `check-native` still bit-exact, `check-menu`, `check-state`,
`check-houses`, `tests/mouse.sh`, the 27609-frame winning mission and
`tests/defeat.sh`.

## 2026-08-24 — B4: the southern margin was never empty, and the metric was wrong

Acceptance B4 says the widened view's margin must fill in every direction. It
was recorded NOT MET on the strength of a pixel count: at frame 4125 of
`data/recordings/artifacts.txt`, 400x256 shows only 128 more non-black pixels
than 400x224, against ~10000 for a full southern margin. That number is real
and the conclusion drawn from it was wrong.

**Non-black is not the same as drawn.** Unexplored map is fog; the fog tile is
a solid block of colour index 12; palette entry 12 is `$0000`, black. Counting
non-black pixels therefore measures how much of the map the player has
explored, not whether the renderer filled the margin. Replaying `mapview_pixel`
offline against the RAM, VRAM and ROM at the same frames:

| frame | south non-black | south DRAWN from the map |
|---|---|---|
| artifacts 4125 | 128 / 12800 | **12800 / 12800** |
| level1atredis 6000 | 5532 / 12800 | **12800 / 12800** |
| level1atredis 9000 | 5627 / 12800 | **12800 / 12800** |
| level1atredis 12000 | 8535 / 12800 | **12800 / 12800** |

Frame 4125 is simply an early mission where the explored blob happens to be
exactly the seven cell rows the camera is showing: cell rows 33-39 at cols
21-30, with cell row 40 -- the whole southern margin -- still at `$F6`. That the
blob does not follow the camera is checked directly: at frame 3800 the camera
is on cell rows 35-42 and the blob is still 33-39, and at frame 4000 the camera
is on rows 40-47 with the blob unmoved, so the whole screen is fog.

The other giveaway in the original measurement was that 512x256 and 400x256
report the *same* southern count to the pixel. The extra 112 columns a 512-wide
view adds are west of the camera, and there they are unexplored too.

**The margin was also checked against the cartridge itself**, the way the
western strip was: the southern margin at frame 12000 covers world rows
1266-1297, and by frame 12037 the camera has dropped 40 px so the cartridge
draws those rows in its own view. They agree 68.7%. That looks poor until the
same comparison is run on the cartridge's own rows, which agree 66.0% -- 37
frames of animating terrain cost that much on any band. The margin is as true
to the map as the cartridge's own picture is.

### What WAS wrong: the camera's southern limit

The same defect the western strip had. `CAM_YMAX` bounds the camera so that
camera + 224 lands exactly on the map's southern edge; a taller view grows
downwards, so those extra lines hang off the map. Measured at the southern
limit from `build/cursorfield.state`, driving the pointer into the corner:

| view | before | after |
|---|---|---|
| 320x224 | south edge at world y 1535 | 1535 |
| 320x256 | **1567** — 31 of 32 lines past the map | 1535 |
| 400x256 | **1567** | 1535 |
| 512x256 | **1567** | 1535 |

The map's playfield is cell rows 16-47, so world y 1536 upward is the all-zero
ring outside it, which renders as backdrop. `own_camera_limit()` now takes over
`CAM_YMAX` exactly as it already took over `CAM_XMIN`: remember what the
cartridge last set, publish that minus the extra lines. Gated on mouse control
and widescreen, so with either off it republishes the cartridge's own value and
RAM is identical.

`make check-margins` (`tests/margins.sh`) is the command B4 was missing. It
checks both halves: the margin shows the true map, calibrated against the
cartridge's own rows so terrain animation cannot make it flaky, and the view
stops at the map's edge -- west and south -- at every size.

Verified: `check-margins` (fails on all four sizes without the `CAM_YMAX`
change), `check-native` 472185 calls 0 mismatched, `check-res` all six sizes
exact, `check-cursor`, `check-state`, `check-menu`. The faithful 320x224 path
is byte-identical at frames 1320, 2800, 6000, 9000 and 12000, as is a 400x256
replay without mouse control, both compared against binaries differing only in
`cursor.o`.

---

## 2026-08-24 — D2 in widescreen, and what D4 actually is

Two acceptance criteria from `docs/acceptance.md`: D2, the pre-mission screens
taking the pointer at every size, and D4, the picture not jumping when a menu
or console opens.

### D2 — the pointer on the pre-mission screens at 400 wide

`tests/buildmenu.sh` already probed the build console in widescreen;
`tests/menus.sh` probed nothing but 320. It now probes house selection and the
mentat's YES/NO at `DB4A_WIDE=400` as well.

The offset was measured rather than assumed, which is the whole trap the
buildmenu comments warn about:

```bash
DB4A_LOAD=build/house.state DB4A_SCENE=1 ./build/db4a "$ROM" 130   # 004500
DB4A_LOAD=build/yesno.state DB4A_SCENE=1 ./build/db4a "$ROM" 130   # 024724
```

Neither scene is in `render.c`'s gameplay set, so both are centred and
`render_world_offset()` returns 40, not 80 — the same answer the console gets,
for the same reason.

**The coordinates are the part that needed thought.** A widescreen probe only
tests the logical-to-game conversion if the number means something different
with and without it. The shields are 80 px wide and 88 apart, so no coordinate
can flip between two different shields under a 40 px offset — but 200 and 290
land in the gaps if taken as game coordinates, so they select Ordos and
Harkonnen only when the conversion happens. The mentat's plates span x 193..271,
which makes the obvious 270 useless: it is inside them either way. 290 is not.

Verified by breaking it: with `- render_world_offset()` deleted from
`src/main.c`, three of the new probes fail (Ordos, Harkonnen, mentat NO) and
the rest still pass, which is exactly the set that should be able to tell.

### D4 — the premise was wrong, and the real defect is elsewhere

Task #28 said the build console runs under scene `$004500`, is therefore
centred rather than right-anchored, and so the picture jumps 40 px and grows
pillarbox bars the moment the console opens.

The scene claim checks out — `DB4A_SCENE=1` on `build/buildmenu.state` prints
`004500`, and the console frame at 400 wide has exactly 40 black columns down
each side. The visible consequence does not. **The cartridge fades to black
across its own screen changes.** Rendering every frame and recording the
brightest pixel either side of each offset change (`DB4A_LOG_JUMP=1`,
added for this) over `data/recordings/power.txt`, which opens and closes the
console three times:

```
  [jump] frame  2557 offset 40 -> 80  peak 238 -> 238  scene 006D0C  VISIBLE
  [jump] frame  2616 offset 80 -> 40  peak   0 ->   0  scene 000000  covered-by-black
  [jump] frame  2685 offset 40 -> 80  peak   0 ->   0  scene 006D0C  covered-by-black
  [jump] frame  2913 offset 80 -> 40  peak   0 ->   0  scene 000000  covered-by-black
  [jump] frame  2981 offset 40 -> 80  peak   0 ->   0  scene 006D0C  covered-by-black
  [jump] frame  3447 offset 80 -> 40  peak   0 ->   0  scene 000000  covered-by-black
  [jump] frame  3548 offset 40 -> 80  peak   0 ->   0  scene 006D0C  covered-by-black
```

23 blank frames before the console appears, 5 after it goes. Same in
`wide.txt` and `level1atredis.txt`: every console transition covered, one
visible change per run.

That one is **entering a mission**. The cartridge turns the display on and
draws the mission map for five frames while `$FFFFE002` still holds `$000000`,
then installs `$006D0C`:

```
  2551  display off, scene 000000
  2552  LIT, offset 40, content x  65..320     <- map on screen, no HUD yet
  2556  LIT, offset 40, content x  65..320
  2557  LIT, offset 80, content x 105..360     <- the jump
```

Five frames, once per mission.

**None of #28's three options reaches it.** (a) is the wrong scene entirely —
and `$004500` is also house selection, which right-anchoring would leave with
an 80 px bar down one side. (c) cannot help, because the offending frames are
before any console exists, and it would make the picture's geometry depend on
`DB4A_MENU_MOUSE`, since `probe_watch` is only called with mouse control on.

(b) was implemented and measured rather than argued about — anchor on whether a
sprite lies entirely at x >= 240, as `render.c`'s own comment block suggests.
It is ten times worse:

```
anchor = scene pointer       1 visible change
anchor = HUD on screen      10 visible changes
```

Six of them inside `$024724`, the mentat, which has sprites in that region and
so flickers in and out of "gameplay" while the player reads it; three are the
console itself, which would be dragged hard right and drawn lopsided; and the
mission-entry jump survives anyway, moving from 2557 to 2558. Reverted.

Fixing mission entry needs a signal that says "a mission is on screen" one
frame before `$FFFFE002` does. `mapview_ready()` is not it — it only becomes
true once the cartridge draws a map column, and reads 0 for the whole of
`power.txt`. `CAM_X`/`CAM_Y` are already the mission's values by frame 2539 and
are `0000 0000` on the title screen, so they are a candidate, but they keep
their value between missions and would misfire on later `$000000` stretches.
Left alone rather than guessed at: a wrong guess here moves the picture
mid-mission, which is worse than the 100 ms it would save.

`make check-jump` (`tests/nojump.sh`) asserts what holds — every menu and
console transition covered by black — and pins the one exception so that fixing
it makes the test say so.

Verified: `check-menu`, `check-menus`, `check-jump`, `check-native` (472185
calls, 0 mismatched), `check-res` all six sizes exact, `check-cursor`,
`check-state`, `tests/mouse.sh`. The faithful 320x224 path is byte-identical at
frames 1320, 2800, 6000, 9000 and 12000 of `level1atredis.txt`, compared in a
clean tree against the same frames from before the change.

## 2026-08-25 — B5: the cap was arbitrary, and the build was already wrong below it

B5 asks whether view sizes beyond 512x256 work, or whether the cap is
deliberate. Measured, it was neither.

**The 512x256 cap was already producing a wrong picture.**
`widescreen_extend()` fills `(width - 320)/8 + 2` tile columns west of the
cartridge's view. The nametable is a **64-column ring**, and the cartridge's own
320 pixels straddle **41** of those columns — 40 only when hscroll lands exactly
on a tile boundary, and `cam_col()` deliberately rounds west to the first of
them. So past roughly 496 wide the fill laps the ring and overwrites columns the
cartridge itself drew. `DB4A_MAPCHECK` over `wide.txt`, comparing the
cartridge's OWN 320x224 against the map it should hold:

```
488 wide   0.03%      504 wide   0.49%      640 wide  11.72%
496 wide   0.06%      512 wide   1.12%      800 wide  28.20%
```

0.03% is the B3 baseline. The shipped cap was sitting at 1.12%.

Clamping `need` to the 23 columns the ring can spare holds it at 0.03% at
**every** size up to 1024x1024. The clamp costs nothing, and that is measurable
rather than assumed: the margin has been drawn from the game's map rather than
from the tilemap since mapview landed, and rendering `wide.txt` with the fill
disabled outright gives byte-identical frames at five sampled frames. What the
fill still covers is the window before mapview has learned the map base.

**Where the real cap is: 1024x1024, and the cartridge says so.** The camera
limits it writes — `$FFE3D2`/`$FFE3D4` and `$FFE3CE`/`$FFE3D0` — read X
512..1216 and Y 512..1312 in both missions measured, which with the 320x224 the
camera frames is a world **exactly 1024 pixels square**. At that size the whole
map is on screen and the camera is pinned; anything larger can only add
backdrop. Rendered at 1280x1024 to confirm: 98.5% of the margin is black, and
the non-black pixel count is identical to 1024x1024's. `own_camera_limit()` now
clamps against the far limit so the two cannot cross on a smaller map.

**What it costs.** `FB` is 3 bytes a pixel and `plane_hi`, `plane_any` and
`taken` are 1 each, so 6 bytes a pixel:

```
 512x256    768 KiB        1024x768   4.50 MiB
 640x480   1.76 MiB        1024x1024  6.00 MiB
```

All BSS, so the binary on disk does not grow and untouched pages are never
faulted in. Peak RSS with every frame rendered, 300 frames of gameplay:

```
320x224   7.97 MB      640x480    9.36 MB
512x256   7.92 MB      1024x1024 11.92 MB
```

**The sprite table never saturates**, which was the one hardware limit a much
larger view could plausibly have exhausted. `widescreen_append_sprites()` stops
at the cartridge's own `SAT_MAX` of 80, so it is now counted rather than argued
about. Over the whole of `level1atredis.txt`:

```
 400x224   sprite table full on 0 of 14661 frames, 0 pieces dropped
 640x480   sprite table full on 0 of 14661 frames, 0 pieces dropped
1024x1024  sprite table full on 0 of 14661 frames, 0 pieces dropped
```

The population is 34 sprite pieces a frame against a table of 80, and appending
plateaus at 3.9 a frame from 640x480 upward because there are no further units
to find. **SAT_MAX does not limit usable view size.**

What DOES limit it is fog of war, and that is the game's, not ours. At frame
14500 of mission 1 the explored region is about 77 000 pixels; a 640x480 view
already contains all of it, and 1024x1024 shows the same 77 000 with more black
around them. A bigger window buys resolution of the map you have not been to.

`splash.c` centred its notice on `FB_W`, the ALLOCATION, not `fb_width`. At 320
the text already sat 96 px right of centre and nobody had noticed, because
`put()` clips; raising the allocation would have pushed it off screen entirely.
Now centred on the live width.

Verified: `check-res` extended to 640x480, 800x600 and 1024x1024 — the
cartridge's own 320x224 byte-exact inside all nine sizes; `check-native`
(472185 calls, 0 mismatched), `check-mission`, `check-cursor`, `check-margins`,
`check-jump`, `check-state`, `check-menu`, `check-menus`, `tests/mouse.sh`.
Frames 1320, 2800, 6000, 9000 and 12000 of `level1atredis.txt` at 320x224 are
byte-identical to the tree before the change.

## 2026-08-25 — D5 (task #26): the arrow keys and the clamp box that came back

Reported: with `DB4A_MOUSE=1` the arrow keys no longer scroll the map.
Reproduced headlessly from the mission-1 state at frame 9000, holding RIGHT for
200 frames:

```
no mouse control    camera X 699 -> 1194   scrolled 495 px
mouse control on    camera X 699 ->  699   scrolled   0 px
```

The cursor walked to x=296 and sat there for the remaining 190 frames. All four
directions were dead the same way.

**296 is not a coincidence.** It is both the maximum the cartridge's own cursor
clamp box allows and — because `cursor_scroll_band()` defaults to 24 — exactly
where the modern scroll threshold sits. The cursor can reach the threshold and
never pass it, so the distance past it is always zero, so the velocity is always
zero. `db4a.conf.example` already warned about this shape of fault under
`mouse_clamp`: *"if the two meet there is no depth to measure and the map will
not scroll at all."* What it did not say is that the ROM's box could come back
and make them meet.

It came back because the frontend suppresses steering while a keyboard direction
is held — which is right, the keys should win — and `mouse_steer()` was the only
thing that ever replaced the ROM's box. So on exactly the frames the player uses
the arrows, the box reverts.

**The first fix did not work, and the reason is worth recording.** Moving the
box write into `native_cursor_scroll()` looked obviously correct: it runs every
gameplay frame regardless of who is steering. It changed nothing. `DB4A_WATCH=FFBF1E`
over one frame says why:

```
FFBF1E <- 013C  from block 00706C     ours   (316)
FFBF1E <- 0128  from block 004DA8     the cartridge's (296)
```

`$4DA8` rewrites the box **every frame** — not once per mission, which is what
`src/mouse.c` claimed and what the first fix was built on — and it runs after
both the cursor routine at `$6DF8` and the scroll at `$706C`. Anything written
from inside the frame is stale before `$6DF8` next reads it.

So `mouse_own_clamp_box()` is called from `system_frame()`, ahead of the VBlank
handler that contains all three writers. That is also the only place that gives
every path the same behaviour: SDL, the headless harness, and replay of a
recorded session.

After, holding a direction for 200 frames from the same state:

```
right 403 px   up 453 px   down 236 px   left 187 px
right 403 px   up 453 px   (at DB4A_WIDE=400)
```

down and left stop early because the map does. `tests/mouse.sh` gains the case
and fails on the previous tree with 0 px. It also checks the other half of the
contract — that the pointer still takes the cursor the moment it moves — because
a fix that gave the keyboard the cursor by taking it from the mouse would pass
everything else and be worthless.

`src/main.c`'s harness now mirrors the frontend's gate: steering is skipped while
the scripted d-pad holds a direction. It tracks what the SCRIPT asked for, never
the pad itself, because steering used to press directions and gating on the pad
self-locks — that trap is already recorded in the comment there.

## 2026-08-25 — the defaults, and the settings file that could not say no

`remaster` shipped its two headline features off by default. Mouse control was
opt-in from the day it landed, widescreen has been opt-in since the merge, and
`make play WIDE=400` was how anyone actually saw either. That is backwards for
a branch whose entire purpose is those departures: the faithful build already
exists, on `master`, and someone running `remaster` has already chosen not to
have it.

Flipping them turned out to be blocked on something smaller, and it is the part
worth recording.

**Every boolean setting was a presence test.** `if (cfg("DB4A_MUTE"))`,
`if (cfg("DB4A_MOUSE"))`, and three more. `cfg()` returns the string or NULL, so
the test asks "is this key set at all" — and `mouse = 0` in `db4a.conf` sets the
key. It reads as yes.

That is harmless while every default is off, because nobody needs to write `0`;
you turn a thing on by adding a line and off by deleting it. It becomes a real
fault the moment a default is ON, because then removing the line is not the off
switch — there is no off switch, and the one a player would reach for does the
opposite of what it says. Two settings had already hit this and hand-rolled
`e && *e == '0'` around it (`MENU_MOUSE`, `PLACE_SCROLL`), which is the same
patch applied twice without the underlying gap being closed.

`cfg_bool(name, default)` closes it: 0/no/off/false and 1/yes/on/true in either
case, an empty value read as no so `DB4A_MOUSE= make play` means what a shell
user expects, and a warning rather than a guess when the value is neither —
because a typo silently meaning yes is exactly how someone ends up believing
they turned something off.

`PLACE_SCROLL` is the one to be careful with, and `tests/test_config.c` pins it:
the setting names the CARTRIDGE's behaviour, so the override is its inverse and
unset follows mouse control. `!cfg_bool("DB4A_PLACE_SCROLL", !mouse_enabled())`
reproduces all six cases.

**Where the defaults live matters more than what they are.** Both flips are in
`src/sdl_main.c` only. `render.c`'s `int fb_width = 320` is shared by both
binaries and stays at 320, and the headless binary keeps reading `DB4A_WIDE`
with `getenv` rather than `cfg`. So no config file and no default can change the
size of the frames a comparison test renders. Confirmed directly: with
`wide = 400, mouse = 1` in a conf file, `./build/db4a` ignores both. And no test
drives `build/db4a-sdl` at all, which is what makes the flip provably invisible
to the suite.

The off switches are real rather than approximate. `wide = 320` gives back the
cartridge's own view byte-for-byte — `make check-res` proves the cartridge's own
picture is EXACT inside nine view sizes up to 1024x1024 — and `mouse = 0` hands
the cursor back to the keyboard and pad.

```
view: 400x224 window 1200x672
      widescreen: 80 px of extra map on the left during play;
      `wide = 320` in db4a.conf gives back the original view
mouse control: on -- left=A, right=B, middle=C
               `mouse = 0` in db4a.conf turns it off
```

**One thing the flip makes true that was not before.** The cursor native
override is gated on mouse control, and CLAUDE.md's note that it is gated
"instead of being on by default" was written when mouse control was opt-in. It
is now the default path, so `remaster` pays its 0.62%-of-pixels interleave cost
on every run. Bounded, measured, and the price of the branch's headline feature
— but interleaving on absolute cycle position stopped being optional cleanup
when this default flipped.

**Found on the way in.** The widescreen banner had two faults in one line: it
read `DB4A_WIDE_UNITS` through `cfg()` while `src/widescreen.c` reads it with
`getenv`, so a `wide_units` line in a config file moved the banner and not the
behaviour; and its polarity was left over from when that switch defaulted on, so
the one line whose stated purpose is that you should not have to guess whether a
widescreen run diverges from a 320 one was reporting that it did, by default,
when it does not.
## 2026-08-25 — Start skips the intro

The cartridge takes 2164 frames — about 44 seconds — from reset to the START
GAME menu, and offers no way past it. `remaster` now has one: press Start
during the opening and you land on the menu.

**It is a fast-forward, not a jump.** The skipped frames are really simulated,
just without rendering, audio or pacing, so the machine arrives in exactly the
state waiting would have produced. Nothing is written into RAM and no cartridge
code is bypassed. That was not the cheap option chosen over a proper one — it
*is* the proper one here. Poking the intro's state forward would mean finding
and setting every variable the sequence leaves behind, verifiable only by
eyeballing the result; this way the landing frame is provably identical to the
unattended run's frame of the same number, which `tests/skipintro.sh` asserts
pixel for pixel. It also keeps input recordings replayable across a skip, since
the frame numbers still line up. The whole thing costs 0.7 s of wall clock.

**There is no scene value that means "the menu is up".** `$FFFFE002` holds
`00017C32` from frame 351 to 1404 and then returns to zero: the planet zoom,
the title and the menu are one routine that waits for vblank itself instead of
going back through the dispatcher. So the landing point had to be found by
execution, not state — two block traces cut either side of the menu appearing
differ by 67 blocks, all in `$177D4`-`$17B56`, and `$178C8` is the head of the
loop that reads the pad and runs the idle countdown into the attract demo. A
fifth probe slot watches it. Same lesson as the mentat's YES/NO, arrived at
from the other direction.

While reading that loop: `$4D46` maps pad bits to letters, `S` for Start, and
the title loop exits on any of them. So `docs/menus.md` was wrong to conclude
there is no main menu — the scene log had nothing to show for it, and the
survey trusted the log over the screen.

**The press has to be swallowed.** Holding Start through the intro on the
unmodified game reaches the menu at frame 981 and then immediately confirms
START GAME, landing on house selection by 1019 — the opening does honour a held
Start in places, just not enough to be useful. So the skip marks Start as held
without pressing the pad: the game sees nothing until the player lets go and
presses again. Without that, one press would skip the intro *and* pick a menu
entry.

Two smaller things that would have been quiet bugs. The SDL pacing deadline is
`t0 + frames / 49.7 Hz`, so a jump of 2163 frames puts the next target 43
seconds out and the loop sleeps through it; `t0` is rebased onto the landing
frame. And `build/dispatch.o` did not depend on `include/probe.h`, so adding
the fifth slot left the dispatcher scanning four and the probe never fired —
the first run reported "gave up after 3600 frames".

Verified: `make check-skipintro` (lands on frame 2164, that frame identical to
the unattended run's, refused once the menu is up, and `skipintro = 0` sits
through it), plus the suite unchanged.

## 2026-08-25 — the intro skip splices the soundtrack, and the emulation is innocent

Reported as "some funkiness with music when we skip intro". The symptom is a
hard splice: whatever is playing at the press cuts off mid-note, there is about
0.7 s of dead silence while the fast-forward runs, and then the title theme
snaps in some 43 seconds into the piece -- mid-phrase, at full mix, with notes
that were keyed on during the skipped span sounding without their attacks, plus
a step-click where the new stream starts at a non-zero sample.

**The emulation is provably right, which is the useful half of this.** Capture a
skipped run and an unattended one and compare the raw samples:

```
DB4A_WAV=A.wav ./build/db4a "$ROM" 3000
DB4A_WAV=B.wav DB4A_SKIP_AT=800 ./build/db4a "$ROM" 3000

common prefix         : 2840260 bytes = frame 800.3
common suffix         : 2968236 bytes = 836.3 frames
prefix+suffix covers B: 5808496 of 5808496 bytes -> ENTIRELY
audio missing from B  : 4843000 bytes = 1364.5 frames   (the skipped span)
```

B is A's first 800 frames followed by A from frame 2164, bit-for-bit, with
nothing else in it. Tempo, Timer A, the envelopes and the Z80 driver all come
through the skip intact -- which they had to, since the skip runs the frames
rather than fabricating a state, but it is worth having measured rather than
assumed. Save states either side of the landing differ in two bytes, and those
are uninitialised struct padding in `z80bus_t`, not state.

So nothing is damaged. The soundtrack position is simply part of what gets
fast-forwarded, and the player hears one point of the score cut to another.
That puts the fix in the output stage, not the emulation: a one-second ramp
armed where the skip lands, beside the existing `t0` rebase and the queue
clear. The theme fades up instead of slamming in, and starting the ramp at zero
also kills the step-click.

It lives in `src/sdl_main.c` and touches no emulation state at all -- confirmed
by re-running the headless capture after the change and getting a byte-identical
file. `check-skipintro` still reports the landed frame identical to the
unattended run.

**What this does not do** is start the theme from the top. That needs the music
re-cued at the landing through the game's own play-sound entry at `$2DDAE`,
which means reverse-engineering the track id and the mailbox protocol, and which
writes sound-driver state -- deliberately outside what the skip does today,
since "arrives in exactly the state waiting would have produced" is the property
the whole feature is built on. A separate decision, not a bug fix.

## 2026-08-25 — the black bar at a mission's start: nothing to learn from yet

Reported from a live session: the left strip goes black "sometimes". It is not
sometimes. It is every mission, for as long as the camera stands still.

Reproduced from `data/recordings/tour.txt` at `WIDE=400`, measuring the lit
fraction of the strip (x < 80) against the 80 columns beside it (x 80..160):

```
  frame 3109   gameplay begins
  3110-3200    strip  6.5% lit   neighbouring columns  97% lit
  3201         the first column or row draw of the whole mission
  3210         strip 56.7%
  3220         strip 97.0%
```

Two seconds of black bar, ending on the first scroll. The cause is not the
margin renderer: `render.c` reads the map only when `mapview_ready()`, and
until then falls through to the tilemap, which the cartridge maintains for its
own 320 pixels only. West of that, at a mission's start, it has never written
anything.

**The base was knowable the whole time.** `DB4A_LOG_MAPV=1`, added here, prints
what each candidate scored:

```
  [mapv]   cand=FF7D9C checked=272 nseen=1 bad=0   -> rejected: sample not diverse
  [mapv]   cand=FF759C checked=272 nseen=0 bad=272 -> rejected: sample not diverse
```

The correct base reproduced **272 of 272** sampled tiles on the very first
draw, and was refused 24 times running. `base_agrees` demands a diverse sample
-- six distinct entries -- and a mission opens on one repeated shroud tile, so
`nseen` is 1 however right the candidate is. That gate was added for a good
reason (a decoy found by searching work RAM once passed on a uniform screen and
then mismatched 58% of the picture), but it was being applied to a candidate
that is not searched at all: it is the cartridge's own `a3` minus the cell
offset, with only the lap in doubt.

Worse than the two seconds: **a session that never scrolls never learns at
all.** The scripted Harkonnen and Ordos runs in `tests/houses.sh` reach
gameplay and sit there, and their margin was on the tilemap fallback for the
entire run.

### The game knows where its map is

`DB4A_RAMDUMP` at frame 3150 -- before any draw -- has exactly one longword in
work RAM pointing into the map region:

```
  FFE404 = 00FF7D9C        the base the draws would agree on 50 frames later
```

Same value in mission 1, mission 2, and all three houses.

So `mapview_poll()` reads `$FFE404` once per rendered frame. It **proposes**;
`base_agrees` still decides, exactly as strictly as before. A diverse screen
confirms it outright. A uniform one -- what a mission opens on -- can only say
the candidate is exact over the sample, so the base is taken PROVISIONALLY and
every subsequent draw either confirms it or drops it. Nothing draws the margin
until it has reproduced what the cartridge itself put on screen.

### Result

```
  [mapv] base=FF7D9C provisional from $FFE404          (mission start)
  [mapv] base=FF7D9C CONFIRMED by a draw at pc=007468 (draw 25)
  [mapv] base=FF7D9C tiles=14611272 mismatched=48 (0.00%) frames=7420
```

Zero drops over the 10612-frame session. The strip at frame 3150 goes from
6.5% to 36.5% lit; the remainder is genuine shroud and must stay black, which
is why the number is not 97%. Visually the terrain now runs continuously into
the margin where there is terrain to run.

`check-res`, `check-map`, `check-margins`, `check-mission`, `check-state`,
`check-houses`, `check-cursor` and the unit tests all pass unchanged --
including the cartridge's own picture staying byte-identical at nine view sizes
and the recorded mission staying bit-identical at four.

### Gotcha, recorded so it is not rediscovered

**Headless renders only at the `DB4A_SHOTS` frames**, so anything hanging off
`render_frame` -- `mapview_check`, and now `mapview_poll` -- does not run
without `DB4A_RENDER_ALL=1`. An earlier pass at this concluded the base was
never learned in *any* recording, which was entirely an artifact of the
diagnostic never running.

### Still open

In the last 3000 frames of `tour.txt` the cartridge's own view is black while
the margin shows map, and 2.0M cells are counted `unfilled` -- the cartridge
leaving `$0000` where it has nothing to show. That is the documented
camera-limit case rather than a disagreement (mismatched stays at 0.00%), but
it has not been explained frame by frame.

## 2026-08-25 — the black bar was ours: a guard that outlived its measurement

Reported from play and pinned down from a save state the player took at the
moment it was visible (a later Atreides
mission, `CAM_X=1493` inside limits 112..1696 -- nowhere near a camera edge).
The state itself is not in the repository and cannot be: it is a dump of
cartridge memory. `data/recordings/tour-password.txt` is the session that
reached that mission, through the Options screen's password entry.

The bar is **39 pixels wide, with a straight vertical edge**: 138 of 217 rows
have their first lit pixel at exactly x=39. Fog has a blobby, cell-aligned
boundary; this did not. And the map under it was not fog either -- the plane-B
words there are real terrain (`$007E`, `$008F`), and the same value renders
blank at x<39 and drawn at x>=39, so the map data cannot be what decides it.

What identified it was measuring the bar at several view widths:

```
  margin  80 px   bar  39      640x480 ... every one of them
  margin 120 px   bar  79      is margin - 41
  margin 160 px   bar 119
  margin 240 px   bar 199
  margin 320 px   bar 279
```

A constant subtracted from the margin, not a fraction of it. 41 was that
frame's `hscroll mod 512`, and the player's own description -- "you can press
left/right to move it a bit" -- is that constant changing as they scroll.

### The cause

`render.c` had a wrap guard that runs BEFORE the margin is drawn and
`continue`s onto the backdrop:

```c
if (!noguard && x < render_world_offset()) {
    int hs = ((hs_b % 512) + 512) % 512;
    if (x - render_world_offset() + hs < 0) { ...backdrop...; continue; }
}
```

It paints exactly `render_world_offset() - (hscroll mod 512)` pixels of black
at the left edge. Its own comment carried the measurement that made it look
harmless -- rendering 32 frames with and without it gave byte-identical output,
and "every pixel it paints was already backdrop".

**That measurement was taken when the margin came from the tilemap**, which
held black there anyway. Since mapview landed, the margin comes from the game's
map, and the guard erases real map before the map path below it ever runs. The
comment even says not to trust its reasoning without re-measuring; re-measured,
it is now the cause of the very symptom it disclaims.

Confirmed by the switch that was already there:

```
  guard on   (default)        black bar 39 px
  guard off  (NOGUARD set)    black bar  0 px
```

Note `DB4A_WIDE_NOGUARD` tests only whether the variable is SET, so `=0`
disables the guard as surely as `=1`. That cost a measurement here.

### The fix

The guard now applies only where it was measured inert: `!mapview_ready()`,
the fallback path before the map base is known, where the tilemap really can
wrap its 512-pixel plane. The map path cannot wrap -- it is indexed by absolute
world position, with no ring involved.

At the reported state the bar goes to 0 px at 400 and 480 wide. `check-res`
(nine sizes EXACT), `check-map` (0.00% at every size), `check-margins`,
`check-mission` (bit-identical at three sizes) and `check-cursor` all pass.

### Still open

At 640 wide the same state keeps a 44-pixel black edge, and it is NOT fog: the
cells under it hold real plane-B tiles (143, 127, 142, 139, 138), none of them
the map's fog tile `$00B`. Something else bounds the fill that far west. It
does not affect the default 400, which is where this was reported.

## 2026-08-25 — placing a building meant not being able to look for a site

Reported from play: with a building waiting to be placed, the map could not be
scrolled by any means -- neither the pointer at the screen edge nor the arrow
keys -- so a site off the current screen could not be reached.

The cause is the override that suppresses the auto-centring. Placement runs its
own copy of the edge-scroll routine at `$64D2`, and `native_placement_scroll`
replaced it with the outcome that routine produces when the cursor is already
inside its dead zone: `OUT_X = OUT_Y = 0`, rejoining the ROM at `$66F4`. That
removes the auto-centring, which was the point -- and with it every other
reason the view moves.

Measured by holding LEFT through a placement in `data/recordings/tour.txt`:

```
  override on (before)   CAM_X 1695 -> 1695 -> 1695     pinned
  PLACE_SCROLL=1         CAM_X 1283 -> 1240 -> 1060     scrolls
  override on (after)    CAM_X 1408 -> 1365 -> 1185     scrolls
```

### The fix

The modern band scroll is now factored out of `native_cursor_scroll` and run by
both overrides. Placement writes its result to the same two scroll outputs and
rejoins `$66F4`, which reads and applies them as it always does.

The auto-centring stays gone, and for a reason worth stating precisely: it does
not live in the scrolling, it lives in the DEAD ZONE. The cartridge computes
that zone as `[120 - extent, 200 - extent]`, shifted by the building's own
size so its centre is dragged into view. The band scroll does not reproduce
that shift -- it scrolls only within `mouse_edge` pixels of the screen edge, so
a cursor anywhere in the middle of the screen produces no scroll at all.

**Honest limit on that half.** The four placement onsets in `tour.txt` move the
camera 0 px both with the override and with the cartridge's own routine,
because the player kept the cursor mid-screen throughout, so this recording
cannot exhibit the lurch either way. The claim above is therefore structural --
the extent shift is not reproduced -- rather than measured against a frame
where the cartridge visibly lurches. A recording that parks the cursor at a
screen edge as a building completes would settle it, and does not exist yet.

`check-cursor`, `check-native` (9093 calls, 0 mismatched), `check-menu`,
`check-state` and `check-mission` (bit-identical at three sizes) pass.
