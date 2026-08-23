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

**1. Separate level error from timing error.** Cross-correlate the two
envelopes at a range of lags per section. If the best lag is ~0 everywhere, the
timing is right and this is purely levels. If the best lag drifts, there is a
local timing bug as well and it gets its own investigation. This is cheap and
decides what the rest of the work is.

**2. Get a per-channel oracle.** The mix is six FM channels plus DAC; a whole-
mix comparison cannot say which is wrong. Genesis-Plus-GX can be built with
individual channels muted, and our own chip can do the same, so capture seven
pairs (each channel alone) and compare them one at a time. Whichever channels
diverge name the feature that is broken.

**3. Work through the envelope generator, in the order the evidence points.**
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
