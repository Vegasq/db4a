# db4a

A native Linux rebuild of **Dune: The Battle for Arrakis** (Mega Drive, 1993).

Not an emulator running a ROM in a generic machine — the game's 68000 code is
statically recompiled into C, compiled into the executable, and linked against
an SDL2 hardware layer. The result is a single native binary that plays the
game.

```
$ make
$ make play
```

**You must supply your own ROM.** No game data is included in this repository,
and none ever will be — see [Requirements](#requirements).

---

## Status

Mission one is playable end to end and can be won or lost. All three houses
load and play. There is sound.

| | |
|---|---|
| **CPU** | 19667/19667 SingleStepTests m68000 vectors |
| **Z80** | zexdoc, plus per-instruction T-state timing |
| **Rendering** | menus pixel-exact against Genesis-Plus-GX; gameplay 99.2% |
| **Audio** | YM2612 + PSG, audible; fidelity work outstanding |
| **Pacing** | PAL 49.7015 Hz, 0.0002% drift |
| **Build** | ~35 s from clean, no warnings |

The fidelity policy is **faithful first**: phase one matches the cartridge so
that a reference emulator stays usable as a correctness oracle. Conveniences
like save states are frontend-only and never alter emulation.

### Known limitations

- **In-game audio is not yet right.** Both chips work and the music plays, but
  balance and pacing are off. SSG-EG is parsed and ignored. Tracked, with
  everything already measured and ruled out, in the task notes.
- **Gameplay rendering is 99.2%, not 100%.** The residual is unit *positions* a
  pixel or two out, not anything drawn incorrectly — closing it needs
  cycle-exact 68000 timing.
- **Not every mission has been played.** The campaign's structure is verified
  (all houses, mission progression, win and lose), but the later missions of
  each house have not been played through.

---

## Requirements

Developed on Fedora 44; any distribution with these packages should work:

```bash
sudo dnf install -y python3-capstone binutils-m68k-linux-gnu asl SDL2-devel
```

| Package | Used for |
|---|---|
| `python3-capstone` | 68000 disassembly during code discovery |
| `binutils-m68k-linux-gnu` | cross-checking capstone's output |
| `asl` | 68k assembler, for round-tripping experiments |
| `SDL2-devel` | video, input and audio |

### The ROM

Place a legally obtained dump at:

```
roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin
```

```
size  1048576
sha1  133cc86b43afe133fc9c9142b448340c17fa668e
```

`make verify-rom` checks it. The build reads the ROM to generate code and the
binary loads it at runtime; **neither the ROM nor anything extracted from it is
distributed here.** `roms/` is gitignored, as are framebuffers, RAM dumps and
audio captures, all of which are derived from the cartridge.

Obtaining the ROM is your responsibility. In most jurisdictions this means
dumping a cartridge you own.

---

## Building and playing

```bash
make            # generate, build, and run the unit tests  (~35 s)
make play       # play
```

### Controls

| | |
|---|---|
| Arrow keys | D-pad |
| **Q / W / E** | A / B / C  (Z/X/C, Space, Alt and Shift also work) |
| Enter | Start (Tab also works) |
| **F5 / F9** | save state / load state |
| **P** | pause |
| **`** or **F** | fast-forward while held |
| Esc | quit |

Gamepads are supported. Any button can be remapped without rebuilding:

```bash
DB4A_KEYS="a=q,b=w,c=r" make play
DB4A_GAIN=8 make play          # louder
```

### Recording and replaying

Input can be recorded and replayed deterministically, which is how the
regression tests are made:

```bash
make record REC=data/recordings/mine.txt
make replay REC=data/recordings/mine.txt SHOTS=6000,12000
```

---

## How it works

**Static recompilation.** 68000 basic blocks are translated to generated C
functions, each returning the next PC, driven by a flat dispatch loop in
`src/dispatch.c`. Indirect transfers are computed return values rather than
nested calls, so the C stack never grows.

This was chosen over a manual matching decompilation — realistically
person-years for a 1 MiB ROM — and over extracting assets into a new engine.
The deciding factor was that the main state machine dispatches through a
function pointer read from `$FFFFE002`, so purely static analysis cannot reach
most of the ROM. Static recompilation degrades gracefully where analysis is
incomplete.

**Instruction semantics are defined exactly once**, as data, in
`tools/semantics.py`. That is the load-bearing decision in the project: the
recompiler and every generated test come from the same definition, so an
instruction cannot behave one way in one place and differently in another.

**Code discovery** starts from the exception vectors and follows branches and
calls, then iterates jump-table recovery to a fixpoint. Several passes recover
entry points that nothing branches to — handlers whose address is a long
immediate, script-interpreter tables reached through an address register,
anchored tables of absolute pointers. Currently 41107 instructions across 11977
generated blocks.

**Verification** runs at four levels, described in `docs/verification.md`:
instruction vectors, machine invariants, frame and memory comparison against
Genesis-Plus-GX, and the Z80 exercisers.

---

## Testing

```bash
make                  # unit tests: flags, addressing, semantics, PSG, YM2612, Z80 timing
make check-cpu        # 19667 m68000 instruction vectors
make check-z80        # zexdoc
make check-operands   # every discovered instruction parses
make check-state      # save a state, resume from it, require an identical frame
make check-houses     # all three houses load
make analyse          # regenerate code discovery from the ROM

# against the reference emulator (needs ref/gpgx, see docs/verification.md)
make compare-screen SCENARIO=houseselect FRAME=2800

# the deepest test in the tree: a recorded mission played to Victory
make replay REC=data/recordings/level1atredis.txt SHOTS=18000
./tests/defeat.sh     # win mission 1, then lose mission 2
```

---

## Layout

```
tools/semantics.py   single source of truth for 68000 instruction semantics
tools/trace.py       code discovery
tools/recomp.py      68000 -> C translation
tools/refhost.c      drives Genesis-Plus-GX headlessly as an oracle
src/gen/             generated code (not committed; reproducible)
src/                 dispatch loop, HAL, VDP, Z80, PSG, YM2612, frontends
tests/               unit tests and scripted playthroughs
docs/roadmap.md      goals, acceptance criteria, milestones
docs/verification.md how correctness is established
docs/journal.md      chronological record, including what went wrong
CLAUDE.md            working notes and hard-won gotchas
```

`docs/journal.md` is worth reading if you are doing anything similar. It records
the bugs that were hard to find and, more usefully, the several occasions where
a measurement pointed confidently in the wrong direction.

---

## Legal

This repository contains original tools and code. It contains **no game data**:
no ROM, no extracted assets, no dumps of cartridge memory.

*Dune: The Battle for Arrakis* is © Virgin Interactive / Westwood Studios. This
project is an unaffiliated technical exercise in binary translation and requires
you to supply your own legally obtained copy of the game.
