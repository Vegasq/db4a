# Goal, definition of done, and roadmap

## Ultimate goal

A **native, self-contained executable** of *Dune: The Battle for Arrakis* that
runs on **Linux**, is **behaviourally identical to the Mega Drive original**,
and is playable with modern controls.

Modern enhancements come *after* that, behind opt-in flags — never at the cost
of the faithful baseline.

## Fidelity policy: faithful first, modernise later

Decided 2026-08-21.

Phase 1 targets a byte-faithful port: 320x224, PAL 49.70 Hz pacing, identical
graphics and audio. Input handling is the **only** thing that changes.

This is not conservatism for its own sake. A faithful build can be diffed
against a reference emulator frame by frame, which makes the emulator a
**correctness oracle**: any visible difference is a bug, mechanically
detectable. The moment output is allowed to diverge deliberately, that oracle
is gone and every bug becomes a judgement call. So the oracle is preserved
until the base is proven correct.

Phase 2 layers optional features behind flags (`--widescreen`, `--hires`,
quality-of-life changes). Each must be defeatable, and with all flags off the
build must still match the oracle.

## Definition of done — v1

**v1 is: one mission playable end to end.**

Not the title screen (proves too little) and not the full campaign (gives no
signal for too long). One complete mission exercises every subsystem together.

v1 is done when all of the following hold:

| # | Criterion | How it is verified | Status |
|---|-----------|--------------------|--------|
| 1 | Boots from reset through VDP init to the title screen | frame hash matches reference emulator | **done** — 71680/71680 |
| 2 | Intro and house-select screens navigable | manual play | **done** |
| 3 | Mission 1 loads and renders correctly | frame hash matches reference at fixed checkpoints | **done** — 99.35% at frame 6000; residual is task #21 |
| 4 | Core loop works: build, harvest spice, combat | manual play | **done** — mission won |
| 5 | Both win **and** lose conditions reachable | manual play | **win only** — a loss has not been played |
| 6 | Correct pacing — PAL 49.70 Hz, no drift | frame timing measured over 60 s | **done** — 0.0002% drift |
| 7 | No crash or hang across a full mission | soak run | **done** — 27609 frames, invariants clean |
| 8 | Builds and runs on Linux x86_64 from a clean tree | `make clean && make` | **done** — 32 s, no warnings |

**Seven of eight.** The whole of mission 1 has been played through to Victory
and is captured in `data/recordings/level1atredis.txt`, which replays
deterministically and serves as the deepest regression test in the tree. Only
the lose condition is unverified, and it needs a play session rather than any
missing capability: `make record REC=data/recordings/defeat.txt`, then lose.

**Audio is explicitly *not* required for v1.** It may be stubbed silent. It is
its own milestone (M5) because YM2612 + PSG + a Z80 core is a large, largely
independent body of work, and blocking playability on it would delay every
other signal.

## Non-goals

Stating these to prevent scope drift:

- **Not a general-purpose Mega Drive emulator.** Only the hardware behaviour
  this one ROM actually exercises needs to be correct. There is no HBlank
  handler, for instance, so no raster-timing work is required.
- **Not a new engine.** Game logic comes from the recompiled original, not a
  reimplementation.
- **Not multiplayer**, and not a rebalance of game design.
- **No game data is redistributed.** See below.

## Distribution constraint

The build **loads a user-supplied ROM at runtime**. No game data is committed
to this repository or shipped with the binary — the repo contains tools and
generated source only. This is why `roms/` is gitignored and why the ROM path
is a runtime argument rather than baked in.

## Portability constraints

**Target: Linux x86_64 only.** Windows and macOS were considered and
deliberately dropped on 2026-08-21 to cut scope. This keeps the existing
`Makefile` viable — no CMake, no cross-compilation, no multi-platform CI.

Two disciplines are kept anyway, because they are required on Linux regardless
and cost nothing:

- **Byte order must be handled explicitly.** This is not a portability nicety.
  The ROM is big-endian and x86_64 is little-endian, so conversion is
  *mandatory* on the only platform we target. Never rely on host byte order.
- **All ROM/RAM access goes through the `m68k_read*` / `m68k_write*`
  helpers.** Single choke point for byte order and alignment.

Because those two hold, adding a platform later is a build-system problem
rather than a correctness problem. Nothing else should assume Linux: no
compiler-specific extensions in generated code, and the language baseline
stays C11 plus SDL2.

## Milestones

| ID | Milestone | Status |
|----|-----------|--------|
| M0 | Analysis foundation: ROM verified, code discovery, runtime + flag tests | **done** |
| M1 | Shared semantics table emitting translator + interpreter; `dispatch.c`; ROM executes from reset | in progress |
| M2 | VDP + SDL2 renderer; title screen matches reference emulator | |
| M2.5 | **Z80 core + 68000/Z80 bus** — reclassified as a v1 blocker, see below | |
| M3 | Modern input; menus navigable | |
| M4 | **v1 — one mission playable end to end** | |
| M5 | **Audio — YM2612, PSG, Z80 sound bus** — chips implemented and audible; fidelity work outstanding, see task #22 | **mostly done** |
| M6 | *(retired — was cross-platform build; Linux-only now)* | dropped |
| M7 | Full campaign, all three houses | |
| M8 | Phase 2 — optional modern enhancements behind flags | |

Differential testing underpins M2 onward and is built alongside M2, not
deferred — it is the oracle the whole fidelity policy depends on. The reference
is **Genesis-Plus-GX**, patched to log executed PCs; see `CLAUDE.md`.

## The Z80 is a v1 blocker, not an audio feature

Recorded 2026-08-21, after input did nothing.

The ROM **never reads the controller data port at `$A10003`**. It touches the
port control registers once at boot and then ignores them. What it does poll,
heavily, is a handshake with the Z80:

```
0014C8  move.b  #$1, $a01b20.l    ; request byte into Z80 RAM
0014D0  move.b  $a01b21.l, d0     ; poll for a reply
```

1108 polls in a 1500-frame run, alongside 2840 Z80 bus requests at `$A11100`.
Forcing a non-zero reply makes things *worse*, not better — distinct blocks
executed drop from 410 to 127 and the VDP configuration regresses to its
early-boot layout, so the ROM expects a real protocol rather than any non-zero
byte.

**Established:** the controller port is never read; the ROM gates on a Z80
handshake; no stub value improves on simply reporting an absent Z80.
**Inferred, not proven:** that pad state reaches the 68000 through the Z80.

Either way the consequence is the same and it changes the plan: audio was
deferred to M5 on the reasoning that it is "large and largely independent".
That reasoning was wrong about the Z80 specifically. The Z80 and its bus are on
the critical path to gameplay and must land before v1. The sound *chips*
(YM2612, PSG) can still be stubbed silent and stay in M5.

## Architecture decisions

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-08-21 | Static recompilation over manual decompilation | manual is person-years for 1 MiB |
| 2026-08-21 | Faithful first, modernise later | keeps the reference emulator usable as a correctness oracle |
| 2026-08-21 | v1 = one mission end to end | title screen proves too little, full campaign signals too late |
| 2026-08-21 | Linux x86_64 only | cuts CMake, cross-compilation and multi-platform CI |
| 2026-08-21 | Audio deferred to M5 | large, independent; would delay every other signal |
| 2026-08-21 | **Z80 core pulled forward to M2.5** | ROM gates gameplay on a Z80 handshake; controller port is never read |
| 2026-08-21 | Single shared semantics definition emitting both backends | makes interpreter/recompiler divergence unrepresentable |
| 2026-08-21 | Genesis-Plus-GX as reference oracle | simple to instrument; accuracy ample for this title |
