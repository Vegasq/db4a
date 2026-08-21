# Goal, definition of done, and roadmap

## Ultimate goal

A **native, self-contained executable** of *Dune: The Battle for Arrakis* that
runs on Linux, Windows and macOS, is **behaviourally identical to the Mega
Drive original**, and is playable with modern controls.

Modern enhancements come *after* that, behind opt-in flags — never at the cost
of the faithful baseline.

## Fidelity policy: faithful first, modernise later

Decided 2026-08-21.

Phase 1 targets a byte-faithful port: 320x224, PAL 50 Hz pacing, identical
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

| # | Criterion | How it is verified |
|---|-----------|--------------------|
| 1 | Boots from reset through VDP init to the title screen | frame hash matches reference emulator |
| 2 | Intro and house-select screens navigable | manual play |
| 3 | Mission 1 loads and renders correctly | frame hash matches reference at fixed checkpoints |
| 4 | Core loop works: build, harvest spice, combat | manual play |
| 5 | Both win **and** lose conditions reachable | manual play |
| 6 | Correct pacing — PAL 50 Hz, no drift | frame timing measured over 60 s |
| 7 | No crash or hang across a full mission | soak run |
| 8 | Builds and runs on Linux, Windows and macOS | CI on all three |

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

Targets: Linux x86_64, Windows, macOS (including Apple Silicon / arm64).

- **Byte order must be handled explicitly everywhere.** The ROM is big-endian;
  every supported target (x86_64 and arm64 alike) is little-endian. Conversion
  is therefore always required — never rely on host byte order, on any target.
- **Do not assume unaligned access is free.** x86_64 tolerates it; arm64 is
  stricter for some instruction forms. All ROM/RAM access goes through the
  `m68k_read*` / `m68k_write*` helpers, which is the single place to get this
  right.
- **The current `Makefile` is Linux-only.** Cross-platform builds need CMake
  before M6. Tracked as a task.
- Language baseline: C11 plus SDL2. No compiler-specific extensions in
  generated code.

## Milestones

| ID | Milestone | Status |
|----|-----------|--------|
| M0 | Analysis foundation: ROM verified, code discovery, runtime + flag tests | **done** |
| M1 | 68k→C translator, `dispatch.c`, interpreter fallback; ROM executes from reset | in progress |
| M2 | VDP + SDL2 renderer; title screen matches reference emulator | |
| M3 | Modern input; menus navigable | |
| M4 | **v1 — one mission playable end to end** | |
| M5 | Audio: YM2612, PSG, Z80 sound CPU | |
| M6 | CMake cross-platform build; CI on Linux, Windows, macOS | |
| M7 | Full campaign, all three houses | |
| M8 | Phase 2 — optional modern enhancements behind flags | |

Differential testing against a reference emulator underpins M2 onward and is
built alongside M2, not deferred — it is the oracle the whole fidelity policy
depends on.
