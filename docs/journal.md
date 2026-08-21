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
