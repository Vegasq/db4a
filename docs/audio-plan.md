# The music problem: what it is, and the plan

The symptom reported is that the music "goes slower and faster, making it a
mess". The measurements below say the timing is right and the **levels** are
wrong, sometimes by a factor of three in each direction. That changes what
needs fixing, so the evidence comes first.

## What was measured, and ruled out

All figures from a 52-second capture from boot (`./build/db4a "$ROM" 2600`).

| suspect | measurement | verdict |
|---|---|---|
| YM2612 Timer A pacing | 212.37 overflows/frame, **sd 2.52**, over 4056 frames | correct and steady |
| Timer A rate | period 1019 -> overflow every 1024-1019 = 5 FM samples; 52781 Hz / 49.7 Hz / 5 = 212 | correct |
| Z80 time per frame | 71365 cycles/frame; expected 3546893 / 49.7015 = 71363 | correct |
| host sample rate | 887.6 samples/frame; expected 44100 / 49.7015 = 887.3 | correct |
| output queue | 6-10 KB steady, **0 clears, 0 truncations** over 1400 frames | healthy |
| total duration | ours 2307823 frames, reference 2306974 — 0.037% apart | no drift |

The driver paces itself by spinning on the Timer A overflow flag, so a steady
overflow count per frame *is* steady tempo. It is steady. Whatever the ear is
hearing, the sequencer is not being starved or over-fed.

`DB4A_LOG_TEMPO=1` prints the distribution; `=2` also lists the first frames.
Note it only counts frames where Timer A is actually running — the driver stops
and reprograms it constantly, and including the stopped frames buries the
signal under zeroes (mean 86 sd 104, which is what the first measurement here
said, and it meant nothing).

## What is actually wrong

RMS envelope, 500 ms windows, ours against Genesis-Plus-GX on the same boot:

```
  t(s)   ours    ref
   7-29    110-230   320-900     we are ~3x too QUIET
  29-35   700-7457   750-2036    we are ~3.5x too LOUD, and clipping
  36-43    280-640   730-1080    ~2x too quiet again
  44-52   1000-1530   650-1060   ~1.5x too loud
```

Envelope correlation over the whole capture is **0.541**; mean level ours 813
against 637.

So the loudness contour does not follow the reference at all, and the 29-35 s
section is loud enough to clip. A burst that clips and then falls back is
exactly what "slower and faster, a mess" sounds like from the outside, and it
is consistent with notes that should be decaying instead sustaining and
piling up.

**Caveat, stated plainly:** total duration matching and Timer A being steady
rule out global tempo drift and sequencer starvation. They do not rule out
*local* timing error — a correlation of 0.541 could hide note-level
misalignment as well as level error. The first job below distinguishes the two.

## The plan

**1. Separate level error from timing error. DONE — and the answer is "both".**

Envelope cross-correlation at 50 ms resolution, +/-2 s of lag, per section:

| section | best lag | corr at best | corr at 0 |
|---|---|---|---|
| 7-29 s (quiet) | +550 ms | 0.222 | 0.075 |
| 29-36 s (loud) | -450 ms | **0.855** | 0.212 |
| 36-44 s | +1350 ms | 0.258 | -0.031 |
| 44-52 s | -1950 ms | 0.219 | -0.078 |

Read carefully, because three of these four rows say nothing. A best-lag
correlation of 0.22 means the two signals do not match at *any* offset, so the
lag that produced it is noise, not a measurement. Only the 29-36 s row is a
real result: 0.855 is a genuine match, and it sits at **-450 ms**.

Two conclusions:

- Where the content does match, it is **time-shifted by ~450 ms**, so there is
  a real local timing error on top of the level error. The shifts are not a
  constant offset in one direction, so it is not a simple rate difference.
- Everywhere else the content itself differs enough that no alignment helps.
  We are not playing the same notes quietly; we are playing something else.

That kills any plan that tunes gain or mixing levels against the whole mix, and
promotes step 2 from "useful" to "required": with whole-mix correlation this
low, nothing further can be diagnosed without separating the channels.

**2. Compare the command streams. DONE — and it moved the diagnosis off the
synthesis entirely.**

Both YM2612s were instrumented to log every bus write (`DB4A_YMLOG=<path>`;
the reference patch is in `ref/gpgx/core/sound/ym2612.c`, alongside the
existing PC-trace hook — that tree is gitignored, so the patch is a local dev
change to re-apply, not a commit).

Over the same 52 seconds from boot:

| | ours | reference | ratio |
|---|---|---|---|
| YM bus writes | 424,810 | 789,437 | 0.54 |
| of which timer restarts (`27`=`15`) | 98,365 | 186,842 | 0.53 |
| YM status reads | 262,561 | 490,664 | 0.54 |
| Timer A overflows | 139,397 | 273,813 | 0.51 |
| **Z80 instructions retired** | **18,479,524** | **18,859,200** | **0.98** |
| **FM sample ticks** | **2,761,320** | **2,761,117** | **1.00** |

The writes are the *same commands in the same order* — the reference simply has
more of them. So this is not a synthesis bug at all, and no amount of envelope
work would have fixed it.

It is also not a cycle-budget bug. The Z80 retires 98% as many instructions,
gets the arithmetically correct 71,365 cycles per frame, and is only stopped
for the 68000's bus for 3.6% of slices. Our chip generates FM samples at
exactly the reference's rate.

What differs is how long **Timer A is enabled**: 696,985 running ticks against
an implied ~1.37M. The driver restarts the timer half as often, so the timer
runs half as long, so it overflows half as often. Cause and effect are circular
inside that loop; what breaks it must be outside.

A Z80 PC histogram, per address, says where:

| Z80 address | ours | reference |
|---|---|---|
| `02B7` | 936,370 | 833,467 |
| `0041`-`0063` (straight-line loop body) | 554,186 | 447,881 |

The same code runs on both sides. Ours goes round the idle loop about 24% more
often and spends correspondingly less in the music engine on page `02`
(18.3% of instructions against 28.6%).

So: the driver executes the same number of instructions, but completes half as
many iterations, because each one waits longer on something that is not the
timer. **The next step is to disassemble Z80 `$0041`-`$0063` and `$02B7` and
find what that loop polls** — a 68000 handshake variable in Z80 RAM, or a
status bit — and compare that signal's timing against the reference.

**Two instrumentation errors made while getting here**, both caught by
cross-checking, both worth remembering: a missing pair of braces in the
reference patch counted overflows that the flag-enable check should have
excluded (it changed nothing, because the driver always enables the flag, but
the comparison was invalid until fixed); and our `ta_ticks` counter only
increments while the timer runs while the reference's counted every call, so
the two were never comparable. `fmsamples` against the reference's tick count
is the like-for-like pair.

### Narrowing, and two hypotheses killed

Everything measurable about the chip now matches exactly:

| | ours | reference |
|---|---|---|
| FM ticks generated | 2,761,320 | 2,761,117 (ratio 1.000) |
| ticks per Timer A overflow | 5.00 | 5.00 |
| values ever written to reg `27` | `15` only | `15` only |
| stream position of the first Timer A enable | write 20,520 | write 20,521 |
| Timer A overflows | 139,397 | 273,813 |
| **Timer A enabled, as a fraction of ticks** | **25.2%** | **49.6%** |

**Killed: "our Timer A runs at half speed."** It does not. The tick rate is
identical to four figures and the period is identical. The overflow count is
half because the timer is *enabled* for half as long, not because it counts
slowly.

**Killed: "our music starts late."** Both sides enable Timer A after the same
number of YM writes -- 20,520 against 20,521 -- so the driver reaches the same
point in its own program at the same point in its own command stream.

Also killed earlier, and worth keeping killed: the chip being advanced ahead of
the Z80. `z80_write` now brings the chip to the Z80's position before applying
a register write, symmetric with `z80_read`, and the slice no longer runs the
chip to the end of the slice before the Z80 executes it. Both are correct
changes and neither moved the overflow count (139,397 -> 139,396), so the
asymmetry was real but was not this bug.

### Where it actually is (corrected)

Stamping both logs with the **FM tick count** -- the one clock both sides
generate at an identical rate, unlike the Z80 counters -- gives the answer:

| | ours | reference |
|---|---|---|
| first Timer A enable | FM tick 2,064,335 | FM tick 1,392,052 |
| equivalent frame | ~1944 | ~1313 |
| YM write rate *after* that point | 0.6206 /tick | 0.5788 /tick |

**The music starts 672,283 FM ticks -- 12.7 seconds -- later in our build, and
once it starts our write rate is slightly HIGHER than the reference's.** The
"half the writes" figure that started this whole investigation is an artifact
of the late start, not of anything running at half speed.

That corrects the "killed: our music starts late" entry above. Both sides do
enable Timer A at the same *position in the command stream* (write 20,520
against 20,521), and I read that as meaning they reach it at the same time.
Same stream position does not mean same time -- it is the same program making
the same writes, so of course the position matches; only the clock differs.

The 68000 is not the cause. Frames 1400 and 2000 are pixel-identical between
the two builds -- planet, then title screen -- so the game's visual state is in
lockstep while the sound driver is 631 frames behind.

So: our sound driver takes 631 more frames to get through its first ~20,520 YM
writes, and then runs at the correct rate. Whatever gates that first phase is
the bug.

### Solved: the Z80 interrupt was an edge, not a level

`z80_irq()` was called once per frame from `system_frame` and began:

```c
if (!Z80.iff1) return;      /* dropped outright */
```

On hardware the VDP **asserts** the Z80 INT line at the start of the VBlank
line and releases it at the end of that line -- the reference does exactly this
in `core/system.c`, `Z80.irq_state = ASSERT_LINE` ... `CLEAR_LINE`. It is a
level held for a window, so a driver that has interrupts disabled when VBlank
arrives still takes it the moment it re-enables them. Delivering a single edge
at the frame boundary instead loses the interrupt whenever `IFF1` happens to be
clear right then -- and a sound driver disables interrupts constantly.

**32.2% of Z80 interrupts were being dropped.** Taking only 68% of them
predicts the driver's first phase taking 1/0.68 = 1.47x longer; the measured
figure was 1.48x. That is the whole bug.

The line is now modelled properly: `z80_irq_assert()` holds it for one
scanline's worth of Z80 cycles and `z80_run` checks it before every
instruction, because the point of a level is that it can be accepted at any
moment while held.

| | before | after | reference |
|---|---|---|---|
| Z80 interrupts dropped | 32.2% | **0.0%** | — |
| interrupts per frame | 0.65 | **0.963** | 1 VBlank/frame |
| first Timer A enable (FM tick) | 2,064,335 | **1,393,114** | 1,392,052 |
| Timer A overflows | 139,397 | **273,640** | 273,813 |
| YM writes | 432,536 | **791,830** | 789,437 |
| envelope correlation vs reference | 0.541 | **0.803** | 1.0 |

The music now starts within 1062 FM ticks -- 0.02 s, under one frame -- of the
reference, where it was 12.7 s late.

### What remains

Envelope correlation is 0.803, not 1.0, and our mean level is 931 against the
reference's 637. With the command stream now matching within 0.3%, that
residual really is synthesis: levels and the envelope generator, SSG-EG first,
exactly as this plan originally proposed. That work is now worth doing, and was
not before.

### Regression this caused, now resolved

The interrupt fix changed the game's behaviour enough that the old recording no
longer played to Victory -- the Z80 writes into 68000 RAM through its bank
window, so changing when the driver runs changes the game. The recording was
remade on 2026-08-23 and everything is green again.

The remake is shorter and wins much earlier: mission 1 falls around frame 15200
against roughly 27700 before. `tests/defeat.sh` had synthesised B presses at
frames 27700-31000 to page through the victory briefing, which would now land
in the middle of mission 2; the new recording pages through the briefing
itself, so those presses were removed rather than moved.


Neither side ever disables Timer A, and both enable it at the same place. So
after that point the reference issues roughly twice as many writes in the same
number of FM ticks. Our driver is doing the same work more slowly *in chip
time* while executing 98% as many instructions and receiving the correct number
of Z80 cycles per frame.

The next measurement is to stamp the reference's write log with frame numbers
-- its Z80 cycle counter is rebased per frame, so the timestamps already
collected are not comparable with ours, which is monotonic -- and find the
frame at which it first enables Timer A. Ours is frame 1944 of 2600. If the
reference's is materially earlier, the 68000 side of our build reaches the
music later in wall-clock time despite reaching it at the same point in the
command stream, and the fault is in the 68000/Z80 handshake rather than in
either processor's speed.

**Per-channel oracle** — deferred. With the command streams differing
this much, per-channel audio comparison would be measuring the consequence
rather than the cause. Revisit once the streams match.

**(was 2) Per-channel oracle.** The mix is six FM channels plus DAC; a whole-
mix comparison cannot say which is wrong. Genesis-Plus-GX can be built with
individual channels muted, and our own chip can do the same, so capture seven
pairs (each channel alone) and compare them one at a time. Whichever channels
diverge name the feature that is broken.

**4. Work through the envelope generator, in the order the evidence points.**
Only after the command streams match — until then any level difference is
explained by the missing commands.
The known gap is SSG-EG: `src/ym2612.c` parses it and ignores it. SSG-EG makes
an envelope loop or hold instead of decaying, so ignoring it produces both
failure modes seen above — notes that fade when they should repeat, and notes
that hold when they should stop. It is the single most likely cause of a
loudness contour that wanders this far.

After that, in decreasing order of likely impact:

- **envelope rates and key-scaling (KSR)** — an attack or decay a step out is
  inaudible on one note and cumulative across a section
- **total level and the operator-to-carrier mapping per algorithm** — a
  modulator treated as a carrier is loud and wrong in exactly this way
- **LFO depth (AMS/FMS)** applied to the wrong operators
- **DAC mixing level** against the FM channels

**4. Regression-test it.** `make check-audio`: capture N seconds, compare the
envelope against a stored reference summary, and fail if the correlation drops.
The reference WAV itself is cartridge-derived and cannot be committed, so store
the envelope summary, which is not.

## How other emulators do this

Worth knowing before rewriting anything:

- **Genesis-Plus-GX uses the MAME/Nuked lineage of YM2612 core**, where the
  envelope generator is a table-driven state machine stepped at a fixed rate,
  with SSG-EG handled as part of the EG state rather than bolted on. If our EG
  is structured differently, matching it bug-for-bug is easier than inventing.
- **Nuked-OPN2** is a gate-level reimplementation and is the accuracy reference
  the others are checked against. It is the right thing to consult for exact
  behaviour of a specific register, and the wrong thing to copy wholesale --
  it is much slower and models detail this project does not need.
- The common trap, well documented in those projects, is that **the EG runs at
  a rate derived from the chip clock, not per output sample**, and that
  attack is not a linear ramp. Getting either wrong changes levels
  section-by-section without changing tempo, which is the shape of what we see.

## Not the problem

Recorded so these are not re-investigated: the 68000/Z80 interleave, the frame
length, the host audio queue, the resampler ratio, and the sound driver's own
pacing. All measured above.
