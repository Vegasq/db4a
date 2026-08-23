# The money ticker: where its loudness comes from

Answering "where is the loudness of the money ticker defined or computed",
because the sound is too loud relative to the rest of the game.

## Which channel it is

The ticker is **FM channel 1**. Found by comparing YM2612 register activity
across a window where the credits climb against one where they do not
(`data/recordings/power.txt`, frames 2700-2850 against 2300-2450):

| register | while credits climb | quiet |
|---|---|---|
| `A0` / `A4` (channel 1 frequency) | **130** writes | 20 |
| everything else per channel | unchanged | unchanged |

The ticker is the channel being re-pitched as the number counts, not a
sequence of key-ons -- key-on rates are flat across both windows, which is why
looking for a burst of notes finds nothing.

It is also not routed through the game's "play sound" entry: `$1664` (and its
guarded wrapper `$2DDAE`) is called four times in the entire recording, so the
ticker is driven by the sound driver directly.

## Where its loudness is defined — in the game data

Channel 1's instrument, read straight off the bus during the climb:

| register | value | meaning |
|---|---|---|
| `B0` | `3A` | feedback 7, **algorithm 2** |
| `B4` | `C0` | both speakers, no AMS/FMS |
| `40` | `00` | operator S1 total level (modulator) |
| `44` | `0C` | operator S3 total level (modulator) |
| `48` | `0A` | operator S2 total level (modulator) |
| **`4C`** | **`10`** | **operator S4 total level — the carrier** |
| `5C` | `1F` | S4 attack rate 31 |
| `6C` | `17` | S4 decay rate 23 |
| `8C` | `FF` | S4 sustain level 15, release rate 15 |
| `90`-`9C` | `00` | SSG-EG **disabled** on all four operators |

In algorithm 2 the only carrier is S4 (S1 feeds S4; S2 feeds S3 feeds S4), so
**the single value that sets this sound's output level is register `4C` = `10`**
-- 16 steps of 0.75 dB, i.e. 12 dB of attenuation. Everything else shapes its
timbre rather than its volume.

## Where it is computed — in our code

1. `src/ym2612.c`, `op_out()` — total level becomes attenuation as
   `o->tl * 32`, alongside the envelope (`env * 4`) and the log-sine table.
2. `src/ym2612.c`, `fm_sample()` case 2 — the algorithm-2 routing that makes
   S4 the only operator reaching the output.
3. The channel is summed into the mix there, panned by `c->pan`.
4. `AUDIO_GAIN` in the frontend scales the final mix.

## What was checked and found correct

Every part of the chain that could make this one instrument too loud was
compared against Genesis-Plus-GX, and all of it matches:

- **Algorithm 2 routing** -- ours is `out = op_out(o3, m1 + b)`, carrier S4
  only. Correct; a modulator treated as a carrier would have been loud, and S1
  in particular has total level `00`, i.e. full volume.
- **Total level scaling** -- 32 units per 0.75 dB step against an attenuation
  unit of 6.02/256 dB. Correct.
- **Feedback shift** -- ours is `>> (10 - fb)`; the reference is
  `>> (SIN_BITS - fb)` with `SIN_BITS` 10. Identical, which matters here
  because this channel runs feedback 7, the maximum.
- **Sustain level 15** -- the OPN2 special case where SL 15 means silence
  rather than 15 steps. Ours maps it to 1023. Correct, and this instrument
  does use SL 15 on its carrier.
- **SSG-EG** -- parsed and ignored by `src/ym2612.c`, and the outstanding gap
  in task #22, but **not the cause here**: this instrument writes `00` to all
  four SSG-EG registers.

So the instrument is being reproduced faithfully as defined. The excess volume
is not in how this sound is specified or routed.

## What is left to explain it

The whole mix is louder than the reference -- mean envelope level 931 against
637, about +3.3 dB -- so the ticker may simply be inheriting a global gain
error rather than having a fault of its own. Distinguishing "this instrument is
too loud" from "everything is too loud and this one is exposed" needs the
per-channel comparison.

`DB4A_YM_MUTE=<hex mask>` was added for that (bit 0 = channel 1), but the first
measurement with it is self-inconsistent -- muting channel 1 dropped the mix by
1% while channel 1 alone measured 76% of it -- so either the window chosen does
not actually contain the ticker or the measurement method is wrong. That needs
resolving before any number from it is trusted.

The remaining suspects, in order: the global gain; then the envelope generator's
decay rates, since a carrier that decays too slowly stays loud without
sounding mistuned.
