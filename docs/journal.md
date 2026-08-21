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
