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

**The cartridge is included in this repository**, because the build cannot run
without it: the game's 68000 code is translated from the ROM at build time. See
[Legal](#legal) for what that means and how to ask for its removal.

---

On start-up the game shows a short notice saying that this is an unofficial
native port, not a Sega release and not connected to the rights holders, and
where the source lives. Any key dismisses it; `splash = 0` in `db4a.conf`
turns it off. Please leave it on in anything you hand to someone else.

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

**Already present**, at the path below. `make verify-rom` checks it against the
known-good SHA-1, and every analysis target depends on that check passing:

```
roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin
```

```
size  1048576
sha1  133cc86b43afe133fc9c9142b448340c17fa668e
```

The build reads it to generate code, and the binary loads it at runtime rather
than baking it in. If it is ever removed from this repository (see
[Legal](#legal)), put your own legally obtained dump at that exact path and
everything works again -- the path and the SHA-1 are all the build cares about.

Everything *derived* from the cartridge is still gitignored: framebuffers, RAM
dumps, save states, audio captures and packaged builds, every one of them
regenerable from a `make` target.

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

### Mouse control

**On by default.** Left = A, right = B, middle = C.

The cursor goes exactly where you point, and the outer 24 pixels of the screen
scroll the map. In the Construction Yard's build console, pointing at an icon
selects it — with its name and price — and left click builds it. House
selection and the mentat's yes/no take the pointer too.

The cartridge cannot do this itself: it moves its cursor at three pixels a
frame and starts scrolling a quarter of the way in from each edge, which suits
a d-pad and cannot be made to follow a pointer. So the cursor position is
written directly instead, and the scroll band replaces the centre box.

```bash
DB4A_MOUSE=0 make play        # or `mouse = 0` in db4a.conf
```

| | |
|---|---|
| `DB4A_MOUSE_EDGE=24` | how deep the scroll band is |
| `DB4A_SCROLL_MAX=6` | top scroll speed, px/frame (the cartridge's is 3) |
| `DB4A_SYSCURSOR=1` | keep the system pointer visible over the window |
| `DB4A_MENU_MOUSE=0` | leave the build console on d-pad control |

Gamepads are supported. Any button can be remapped without rebuilding:

```bash
DB4A_KEYS="a=q,b=w,c=r" make play
DB4A_GAIN=8 make play          # louder
```

### Widescreen

**On by default, at 400x224.** Anything from 320 to 1024 in either direction
works, so 640x480 and 800x600 do too.

In gameplay the picture shifts right so the HUD stays flush against the edge
with its backdrop under it, and the new space opens on the left as more map.
Menus, the mentat and the cutscenes are 320-wide compositions with nothing to
anchor a wider one, so they are centred with black bars — the widened strip is
gated on the gameplay scene handlers rather than drawn everywhere.

The extra picture comes from the game's own **map data**, read straight out of
work RAM, rather than from whatever the tilemap happens to still hold: the
cartridge only ever writes tiles for the 320 pixels it believes are visible, so
reading those back gives you leftovers that depend on which way you last
scrolled. Map data has no such dependence. It cannot invent map, so ground you
have never explored stays dark exactly as the cartridge would leave it.

```bash
make play WIDE=320          # or `wide = 320` in db4a.conf
```

That is a real off switch, not an approximation of one. The cartridge's own 320
pixels come out byte-identical either way — `make check-res` proves the
cartridge's picture EXACT inside nine view sizes up to 1024x1024, and
`make check-mission` replays the recorded mission unchanged at any of them.
`docs/widescreen.md` has the measurements.

### Recording and replaying

Input can be recorded and replayed deterministically, which is how the
regression tests are made:

```bash
make record REC=data/recordings/mine.txt
make replay REC=data/recordings/mine.txt SHOTS=6000,12000
```

### Handing a build to someone else

```bash
make dist              # -> build/dist/db4a-<branch>-<sha>/
make dist DIST=-t      # ... and the same folder as a .tar.gz
```

Builds, then collects the game, the cartridge it reads its data from, and a
`db4a.conf` into one folder that runs from anywhere: `./play.sh`. It also
carries a `BUILD.txt` naming the exact commit and binary, and a `report/`
directory for the two things worth sending back -- `./play.sh --record` writes
a session there, and an unknown PC appends itself to `report/seeds.txt`.

The package contains the ROM, which is why it is built into `build/` and why
`tools/package.sh` refuses an output path git does not ignore.

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

**Some game logic is ours.** `src/cursor.c` reimplements the cartridge's
cursor edge-scrolling in C, so its thresholds are variables rather than
instruction immediates. A *native override* replaces one basic block with a C
function that does the same job and returns the same next PC; `make
check-native` runs both implementations on every call and diffs RAM, registers,
flags, cycles and the exit PC. `docs/natives.md` covers the method, how to pick
the next routine, and what an override costs.

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
make check-native     # C overrides match the cartridge code they replace
make check-menu       # pointing at a build-console cell selects it
make check-menus      # pointing at a house shield or mentat answer selects it
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
docs/natives.md      replacing cartridge code with C
docs/buildmenu.md    the build console: grid, RAM layout, how to drive it
docs/menus.md        survey of every screen: which can take mouse input
docs/verification.md how correctness is established
docs/journal.md      chronological record, including what went wrong
CLAUDE.md            working notes and hard-won gotchas
```

`docs/journal.md` is worth reading if you are doing anything similar. It records
the bugs that were hard to find and, more usefully, the several occasions where
a measurement pointed confidently in the wrong direction.

---

## Legal

*Dune: The Battle for Arrakis* is **© Virgin Interactive / Westwood Studios**.
Westwood was acquired by Electronic Arts in 1998, so the rights are EA's today.
This project is unaffiliated with any of them, and is a technical exercise in
binary translation.

**The repository includes a copy of the cartridge**, at
`roms/Dune-The-Battle-for-Arrakis_Genesis_EN/`. It is here for one reason: the
build translates the ROM's 68000 code into C, so without it the project cannot
be built at all — not from a clean checkout, and not in CI. Nothing about the
game's age changes who owns it, and no claim is made here that including it is
permitted.

**If a rights holder asks for it to be removed, it will be removed** — promptly,
and from the history rather than just the tip, so clones stop carrying it. Open
an issue or contact the repository owner. The rest of the project stands without
it: every tool, every test and every line of hand-written C here is original
work, and the build falls back to requiring a user-supplied dump exactly as it
did before.

Everything else derived from the cartridge stays out of the repository, and
`.gitignore` enforces it: framebuffers, RAM dumps, save states, audio captures
and packaged builds are all regenerable from a `make` target.
