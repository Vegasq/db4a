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
