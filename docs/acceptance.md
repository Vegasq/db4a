# Widescreen: acceptance criteria

What "done" means for arbitrary-resolution support, written as things a test
can decide rather than things a person can judge. Each has a command, and the
command is the definition.

Status column: **MET** only when the named command passes from a clean tree.

---

## A. Fidelity at the original resolution

The point of the fidelity policy is that a reference emulator stays usable as
an oracle. A resolution feature must not cost that.

| # | Criterion | Command | Status |
|---|---|---|---|
| A1 | At 320x224 the picture is byte-identical to the build before any resolution work | `make check-res` (320x224 row) | MET |
| A2 | The recorded mission renders identically at 320 against the faithful branch | `make replay` + compare vs remaster | MET |
| A3 | Menus pixel-match the reference at 320 | `make compare-screen SCENARIO=house FRAME=...` | pages 96-100%, **fades dip**, pre-existing |
| A4 | The resolution work changes nothing at 320 | byte-compare vs master | MET |

A3 and A4 are the ones that mean "matches the original release". Measured
against Genesis-Plus-GX with `ref/gpgx` built, scenario `house`, all figures
quantised through RGB565 the way `tools/framediff.py --quantize` does:

    2400 100.00   2658 100.00   2800 100.00   2900  99.60   3000  99.23
    3100  96.43   3200 100.00   3400  97.90   3600  97.84   3800  96.43
    4000  70.10   4100  92.14   4176  75.05   4220  82.13   4280  91.46
    4360  98.32

Menu pages sit at 96-100%. The three dips all land on FADE TRANSITIONS, where
our fade runs slower than the reference's -- it reaches black around frame
4360 and we do not until ~4440. Task #32, pre-existing, same family as #21.

**Quantise, or the numbers are fiction.** The reference core emits RGB565, so
its 8-bit values have already lost their low bits. Comparing our output raw
reports 62-70% on frames that are really 96-100%. This cost a full round of
investigation and a wrong conclusion before it was noticed.

**None of it is ours.** Building master in a separate worktree gives the same
percentages to two decimals, and this branch's frames at 2916, 4176 and 4576
are BYTE-IDENTICAL to master's. The resolution work changes nothing at 320,
which is A4 and is what the fidelity policy asks of a presentation feature.

## B. Arbitrary resolution

| # | Criterion | Command | Status |
|---|---|---|---|
| B1 | Width and height are runtime settings, not build constants | `wide`/`tall` in db4a.conf | MET |
| B2 | The cartridge's own 320x224 is byte-exact inside every supported size | `make check-res` | MET |
| B3 | The margin shows the true map, not leftovers, regardless of scroll history | `make check-map` | MET to 448 wide (<=0.09%) |
| B4 | The margin fills in EVERY direction, including below | `make check-margins` | MET |
| B5 | Sizes beyond 512x256 work, or the cap is documented as deliberate | `make check-map`, `make check-res` | **PARTIAL**: correct to 448, allowed to 1024 |
| B6 | Units appear in the margin and are hidden by fog like any other | `make check-res` + fog frames | MET |

## C. The game is unaffected

| # | Criterion | Command | Status |
|---|---|---|---|
| C1 | A recorded mission plays out identically at any size | `make check-mission` | MET |
| C2 | Native overrides remain equivalent to the cartridge | `make check-native` | MET |
| C3 | Save/load round-trips identically | `make check-state` | MET |
| C4 | Mission progression (win, then lose) unaffected | `./tests/defeat.sh` at WIDE=400 | MET |

## D. Menus and input remain usable

| # | Criterion | Command | Status |
|---|---|---|---|
| D1 | Pointing at a build-console cell selects it, at every size | `make check-menu` | MET at 320 and 400 |
| D2 | House select and mentat answers take the pointer, at every size | `make check-menus` | MET at 320 and 400 |
| D3 | The cursor reaches every pixel of the view | `make check-cursor` | MET |
| D4 | The picture does not jump when a menu or console opens | `make check-jump` | MET for menus and consoles; one 5-frame exception at mission entry |
| D5 | Arrow keys scroll the map while mouse control is on | `./tests/mouse.sh` | MET |

**D4, as measured.** The original statement of task #28 was that the build
console runs under scene `$004500`, is therefore not anchored like gameplay,
and so the picture jumps 40 px and grows pillarbox bars the moment the console
opens. The scene claim is right; the visible consequence is not. The cartridge
fades to black across its own screen changes -- 23 blank frames before the
console appears and 5 after it goes -- so every one of those offset changes
happens behind a black screen. `make check-jump` reports six such changes in
`data/recordings/power.txt` and all six are covered.

What IS visible is a different transition: entering a mission. The cartridge
turns the display on and draws the mission map for five frames while
`$FFFFE002` still holds `$000000`, then installs `$006D0C`, and the picture
slides 40 px sideways at 400 wide. None of the three fixes proposed under #28
address that -- see docs/widescreen.md, "Anchoring, and what D4 actually
measures".

## E. Production readiness

| # | Criterion | Command | Status |
|---|---|---|---|
| E1 | `make clean && make` is warning-free | `make` | MET |
| E2 | Every check runs from a clean tree | `make check-*` | MET |
| E3 | Settings documented for a player, not a developer | `db4a.conf.example` | MET |
| E4 | Defaults are safe: widescreen off unless asked for | inspection + `check-res` | MET |
| E5 | No diagnostic left on by default in a release build | inspection | **UNVERIFIED** |

---

## The size cap is allowed wider than it is verified

`wide`/`tall` accept up to 1024, and `make check-res` shows the cartridge's own
320x224 surviving intact at every one of them. But `make check-map` -- which
compares every visible tile against what the cartridge drew, across whole
recordings rather than one frame -- only holds to **448 wide**:

    artifacts.txt   400  0.09%    496  2.96%    640   0.72%
                    448  0.09%    512  4.13%   1024  19.89%

The cause is the camera limit. We hold it a view-width short of the map edge so
the margin always has map to show; past ~448 that pushes the camera into ground
the map does not describe, and at 1024 it pins the camera outright --
CAM_XMIN == CAM_XMAX == 1216 -- with the view extending past the map, where
mapview reads beyond the row and the cartridge draws backdrop.

So 1024 is *allowed* and not *earned*. Anything up to 448 is verified; beyond
that expect the margin to be wrong near the map's edges. Task #34.

Note also that `make check-res` PASSED at 512x256 while the tilemap was 1.12%
corrupt, because it samples a single frame. That is why `check-map` exists.

## Not in scope

Audio fidelity (task #22), PAL60 (#24), and the three keys SDL never reports
on the development machine (#18). None of them are resolution questions.
