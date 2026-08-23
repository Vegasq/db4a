# Replacing cartridge code with C

The recompiler turns 68000 basic blocks into generated C that manipulates a CPU
state struct. That is faithful, and it is what makes the game run at all, but
it is not *ours*: the blocks are machine-written, unreadable, and the only way
to change what the game does is to patch immediates in the ROM image.

A **native override** replaces one block entry with a hand-written C function
that does the same job against the same RAM and returns the PC the cartridge
would have continued from. `src/cursor.c` is the first one.

```
include/native.h    the interface
src/cursor.c        the first override, and its registration table
src/dispatch.c      the hook, and the differential checker
tests/native.sh     make check-native
```

## Picking a routine

`src/cursor.c` was chosen because it scored well on every axis that matters,
and those are the axes to score the next candidate on:

- **One entry, one exit.** Enter at `$706C`, leave at `$71E0`. No subroutine
  calls to reproduce, no second entry point sharing the tail.
- **Every boundary is already a block entry.** The dispatcher looks up whole
  blocks, so an override on a PC in the middle of one would silently never run.
- **State lives in named RAM**, not in registers threaded through from far away.
- **It runs every frame of every mission**, so the existing recorded-mission
  replay exercises it thousands of times without writing a new test.
- **There was a concrete reason to own it.** Both changes wanted from it — a
  cursor that can keep up with a mouse, and scrolling that starts at the screen
  edge — are governed by two thresholds per axis, a shift and two speed caps,
  all baked in as instruction immediates.

## Finding out what it does

The RAM watchpoint is the tool that made this tractable:

```bash
DB4A_WATCH=FFBF12 ./build/db4a "$ROM" 40      # who writes the cursor?
```

It prints every write to that address with the block that made it, which feeds
straight back into the disassembler. Two minutes of that beat an afternoon of
reading static analysis: the cursor turned out to be written from two places,
and the one that mattered was not the one recursive descent made obvious.

## The three rules

**1. Override only at a real block entry.** Enforced by looking the override up
*after* `find_block` succeeds, so registering a bad address fails loudly.

**2. Account for cycles.** The 68000 cycle count drives frame pacing and the
Z80 interleave. An override that does the same work in zero cycles changes the
game's timing. Each one adds the same counts the blocks it replaced would have,
taken from the `CPU.cycles +=` line at the top of each generated block.

**3. Be exactly equivalent by default.** New behaviour goes behind a flag.

## The checker

```bash
DB4A_NATIVE=check ./build/db4a "$ROM" 12000
make check-native
```

At every call it snapshots RAM and the CPU, runs the C, snapshots the result,
rewinds, runs the cartridge blocks, and diffs. The cartridge's result is the
one kept, so a checked run is still a faithful run and can be recorded from.

**Compare everything, not just what you think the routine touches.** The first
version of this checker compared RAM, the cycle count and the exit PC. It
passed on all 9319 calls of a mission while the run visibly diverged, because
the override left `d0`-`d2` and the X flag holding the caller's values and the
code after the exit reads them. A whole-RAM diff plus every register plus the
flags is the minimum; anything less finds nothing and reads like success.

Getting the flags right is not a matter of re-reading the 68000 manual. Route
the arithmetic through the same `add16`/`sub16`/`cmp16` helpers the generated
code uses and they are correct by construction.

## What overrides cost: nothing

A faithful override is free. Measured 2026-08-23 across the whole recorded
mission, `DB4A_NATIVE=0` against `=1`, at frames 2000/6000/10000/14000/18000:
**zero differing pixels and zero differing RAM bytes at every one**, with the
equivalence checker confirming the override really did execute (9093 calls, 0
mismatched). So they are on by default.

**This corrects an earlier claim in this file**, which said an override costs
0.62% of pixels because `m68k_run_until` checks the slice deadline BETWEEN
blocks, so collapsing an eight-block, ~1000-cycle routine into one indivisible
step moves where the 68000 yields to the Z80. Task #23 was raised to fix that
before migrating more routines, and the cursor override was gated behind mouse
control because of it.

The reasoning was plausible and the number was wrong. It was almost certainly
taken before the override's register and flag fixes landed, and then read as
still-diverging from stale output -- the same mistake as measuring a counter
that was never compiled in, which happened twice more in that session. Two
candidate explanations for why it "went away" were tested and both refuted
(the Z80 interrupt fix, and the removal of the eager `ym_run`), which is what
pointed at the measurement itself rather than at any change since.

The lesson is narrow and worth keeping: **re-measure a number before building
on it, not after.** A blocking task sat in the backlog for a day because of it.

## Where this lives

The mechanism and the faithful override are on `master`: replacing recompiled
blocks with equivalent C is fidelity work, not a change to the game. `remaster`
rebases on top and adds the parts that deliberately differ -- the modern scroll
band, the placement override -- along with `include/probe.h`, which only the
mouse-driven menu screens use.

That is why `TABLE` carries a `faithful` flag and `native_lookup` refuses
non-faithful entries: on `master` there are none, and the check costs nothing,
but it means `remaster` can add them without touching the lookup.

## Overrides that deliberately differ

`$64D2` is the first override that is not a faithful reimplementation. It
removes the view re-centring that happens when a building becomes ready to
place -- helpful with a d-pad, disorienting with a mouse, because the map lurches
out from under a pointer that is already where the player is looking.

Such an override needs two things the faithful ones do not:

- **It is not registered unless it is switched on**, so a faithful run never
  takes it. `native_active()` also has to know about it, or a flag that turns
  the override on without `DB4A_MOUSE` silently does nothing.
- **The equivalence checker skips it.** `make check-native` verifies only
  entries marked `faithful` in the table. Comparing a deliberate behaviour
  change against the code it deliberately differs from fails by construction,
  and a checker that is expected to fail is worth nothing. Marking it wrong
  costs a real failure: check-native went red the moment this override was
  added, which is the mechanism working.

Note what this one does NOT do. It does not patch the ROM -- nothing in this
project does, the HAL holds the cartridge as `const uint8_t *` and
`make verify-rom` checks the file. And it does not reimplement `$64D2`: it
writes the outcome the routine produces when no scroll is due and rejoins the
ROM at `$66F4`, the routine's own tail, which applies that (zero) scroll and
returns normally. So it still depends on the cartridge routine existing. Owning
`$64D2` end to end is the fully native version of this, and is not done.

Skipping the routine outright was not available: all twelve callers reach
`$64D2` by `bra`/`beq`/`jmp` rather than `bsr`/`jsr`, so it has no return
address of its own on the stack to emulate an `rts` with.

## What to migrate next

The camera itself: the clamp at `$FFE3CE`-`$FFE3D4` and the routine at `$79CA`
that applies `$FFBF34`/`$FFBF36` to it. It is the other half of scrolling, it
is adjacent to code already understood, and owning it would let the widescreen
work resume — that branch is parked precisely because the viewport geometry is
still the cartridge's.
