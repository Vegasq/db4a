# Contributing

Thanks for looking. This is a small project with a few load-bearing rules, and
most of them exist because breaking them produced a bug that was quiet rather
than loud. Reading this first will save you a review round.

## What it is, in one paragraph

The game's 68000 code is **statically recompiled**: `tools/` translate the
cartridge's basic blocks into generated C, which is compiled into the binary and
run against a CPU state struct, with an SDL2 layer standing in for the VDP, the
sound chips and the pad. It is not an emulator with a ROM loaded beside it.

## Getting a build

```bash
sudo dnf install -y python3-capstone binutils-m68k-linux-gnu asl SDL2-devel
make                 # generate, build, run every unit test  (~35 s)
make play
```

Fedora shown; any distribution with SDL2 and capstone works, and `README.md`
has the macOS and Windows/MSYS2 equivalents. CI builds and tests all three.

You need the cartridge. It is in the repository — see [Legal](README.md#legal)
for why, and what its status is.

## The rules that matter

**1. Instruction semantics are defined exactly once.** `tools/semantics.py` is
the single source of truth for what each 68000 instruction form does, and the
generated code comes from it. So:

> Never fix a semantics bug in `src/gen/`. It is a build output. Fix
> `tools/semantics.py` and regenerate.

Flag behaviour belongs in `include/m68k.h` and is tested by
`tests/test_flags.c`. This rule exists so that two implementations can never
drift apart and turn a frame mismatch into a debugging session against
ourselves.

**2. A departure from the cartridge goes behind a setting.** Anything that
changes what the game *does* — mouse control, the widened view, the intro skip
— is a key in `db4a.conf`, and the cartridge's own behaviour stays reachable by
turning it off. Default it on in `src/sdl_main.c` and read it with `cfg()`
there, never in code the headless `build/db4a` also runs: that binary is what
every comparison against the reference emulator uses, and it must stay faithful
on its own.

**3. Verify before trusting, and label a heuristic as a heuristic.** Several
early results in this project were wrong in ways that looked entirely plausible.
If a number came from a guess, say so where you write it down.

**4. Nothing derived from the cartridge gets committed.** Framebuffers, RAM
dumps, save states, audio captures, packaged builds — all regenerable from a
`make` target, all gitignored. Everything in `build/` must be reproducible.

## Before you open a PR

```bash
make                 # build + unit tests: flags, addressing, semantics, PSG, YM2612, Z80 timing
make check-cpu       # 19667 SingleStepTests m68000 vectors
make check-z80       # zexdoc
make check-state     # save a state, resume, require an identical frame
make check-native    # C overrides match the cartridge code they replace
make check-menus     # pointer selection on the pre-mission screens
make check-skipintro # Start lands on the title menu, having really run the frames
```

`make check-cpu` and `make check-z80` are the slow ones; run them if you touched
anything under `tools/` or `include/m68k.h`. CI runs the full set on Linux,
macOS and Windows.

If you change something the reference emulator can see, say what you compared
against and at which frame. `make compare-screen SCENARIO=houseselect FRAME=2800`
is the usual form.

## Commits

One logical change per commit, subject prefixed with the area:

```
tools:    analysis and codegen scripts
runtime:  include/m68k.h and the C runtime
hal:      VDP, audio, input
docs:     documentation
build:    Makefile and tooling
```

**Put the reproduction command and its numbers in the commit body** where the
change affects analysis output or fidelity, so a regression is visible in
`git log` rather than only in a working tree. Append a `docs/journal.md` entry
in the same commit as the work it describes — the journal is the project's
memory and it is genuinely used.

## Good places to start

Look for issues labelled `good first issue`. The three open ones are honest
about their difficulty:

- **[#10 — three keys never reach SDL](https://github.com/Vegasq/db4a/issues/10)** — an input bug that is almost certainly outside
  db4a, and needs someone on other hardware to confirm. `make keytest` is a
  standalone probe. The easiest one to help with, because it mostly needs a
  second machine.
- **[#11 — 0.8% of gameplay pixels differ from the reference](https://github.com/Vegasq/db4a/issues/11)** — unit positions a pixel
  or two out, not a rendering fault. Wants cycle-accurate interleaving between
  the 68000 and the Z80.
- **[#12 — in-game audio fidelity](https://github.com/Vegasq/db4a/issues/12)** — the chips work and the music is right, but the
  mix runs hot and SSG-EG is parsed and ignored. Self-contained, and needs YM2612
  knowledge more than knowledge of this codebase.

## Reporting a bug

`play.sh --record` / `play.bat --record` writes your inputs to `report/`, and a
recording **replays deterministically**. A recording plus the `BUILD.txt` from
your archive turns a bug from something to be hunted into something to be run.
`make record` does the same from a source build.

## Licensing

Contributions are under the [MIT licence](LICENSE), same as the rest of the
project's own work. There is no CLA.
