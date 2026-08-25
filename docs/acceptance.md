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
| A3 | Menus, mentat and console pixel-match the reference emulator at 320 | `make compare-screen SCENARIO=house FRAME=...` | **NOT MET**, pre-existing |
| A4 | The resolution work changes nothing at 320 | byte-compare vs master | MET |

A3 and A4 are the ones that mean "matches the original release". Measured
against Genesis-Plus-GX, with `ref/gpgx` built:

    houseselect 2800   100.00%      house 2400   100.00%
    house       2658   100.00%      house 2916    97.27%
    house       4176    75.05%      house 4576    94.91%

So the standing claim of "menus 100.00%" holds for static screens and not for
animated ones. The world-map reveal at 4176 is the worst.

**None of it is ours.** Building master in a separate worktree gives the same
percentages to two decimal places, and this branch's frames at 2916, 4176 and
4576 are BYTE-IDENTICAL to master's. The resolution work changes nothing at
320, which is A4 and is what the fidelity policy actually requires of it.
Fixing A3 is task #32 and is orthogonal to widescreen.

## B. Arbitrary resolution

| # | Criterion | Command | Status |
|---|---|---|---|
| B1 | Width and height are runtime settings, not build constants | `wide`/`tall` in db4a.conf | MET |
| B2 | The cartridge's own 320x224 is byte-exact inside every supported size | `make check-res` | MET |
| B3 | The margin shows the true map, not leftovers, regardless of scroll history | `DB4A_MAPCHECK=1` over the recordings | MET (39M tiles, <=0.04%) |
| B4 | The margin fills in EVERY direction, including below | `make check-margins` | MET |
| B5 | Sizes beyond 512x256 work, or the cap is documented as deliberate | none yet | **NOT MET** |
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
| D2 | House select and mentat answers take the pointer, at every size | `make check-menus` | MET at 320 only |
| D3 | The cursor reaches every pixel of the view | `make check-cursor` | MET |
| D4 | The picture does not jump when a menu or console opens | none yet | **NOT MET** (task #28) |
| D5 | Arrow keys scroll the map while mouse control is on | none yet | **NOT MET** (task #26) |

## E. Production readiness

| # | Criterion | Command | Status |
|---|---|---|---|
| E1 | `make clean && make` is warning-free | `make` | MET |
| E2 | Every check runs from a clean tree | `make check-*` | MET |
| E3 | Settings documented for a player, not a developer | `db4a.conf.example` | MET |
| E4 | Defaults are safe: widescreen off unless asked for | inspection + `check-res` | MET |
| E5 | No diagnostic left on by default in a release build | inspection | **UNVERIFIED** |

---

## Not in scope

Audio fidelity (task #22), PAL60 (#24), and the three keys SDL never reports
on the development machine (#18). None of them are resolution questions.
