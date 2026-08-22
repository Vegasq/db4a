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

## What overrides cost, and why this one is behind a flag

`m68k_run_until` checks the slice deadline **between blocks**. The cursor
routine is eight blocks and about a thousand cycles; the slice is five hundred.
The cartridge therefore always yields to the Z80 somewhere in the middle of it.
An override is one indivisible step and cannot yield there, so the 68000 runs
to the end and the Z80 gets its slice later.

Nothing is computed differently — the checker confirms that on every call — but
the two processors interleave at different points, and over a mission that
moves sprites. Measured at **0.62% of pixels at frame 6000**, the same order as
the residual already tracked as task #21.

So the override is taken only when mouse control is on, where behaviour is
deliberately different anyway, and a faithful run is bit-exact. `check-native`
asserts both halves: per-call equivalence, and that a default run is unchanged.

Two ways to lift the restriction, in preference order:

1. **Interleave on absolute cycle position rather than block boundaries.** The
   scheduler would let the Z80 catch up to wherever the 68000 actually is,
   instead of assuming it stops near the deadline. This is the real fix, it
   makes overrides free, and it would probably help task #21 as well.
2. **Register one C function per original block.** Preserves the yield points
   exactly, but fragments the C into a transliteration of the block graph and
   gives up the readable structure that is the entire point.

Do the first before migrating more routines.

## What to migrate next

The camera itself: the clamp at `$FFE3CE`-`$FFE3D4` and the routine at `$79CA`
that applies `$FFBF34`/`$FFBF36` to it. It is the other half of scrolling, it
is adjacent to code already understood, and owning it would let the widescreen
work resume — that branch is parked precisely because the viewport geometry is
still the cartridge's.
