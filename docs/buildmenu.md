# The build console (and the Starport)

The Starport is the same widget with different data behind it -- same chrome,
same cells, same handler shape -- so `src/buildmenu.c` drives both from one
table. The differences are set out at the end.

For the other screens -- house selection, the mentat, the title sequence -- see
[menus.md](menus.md).

Research notes for hooking mouse control into the build menu. Everything here
is measured, not inferred from the disassembly alone; the reproduction command
is given for each claim.

## It is not a scene

The obvious approach — look for a new value at `$FFFFE002` and gate on it, the
way mouse control gates on gameplay — does not work. The console is a sub-mode
of gameplay and the scene pointer never changes:

```bash
DB4A_LOG_SCENE=1 DB4A_REPLAY=data/recordings/level1atredis.txt ./build/db4a "$ROM"
```

A full winning mission produces only eight distinct scenes: `00000000`,
`00017C32` (title), `00004500`, `00024724`/`00024812` (briefing, mentat),
`00006D0C` (gameplay), `0000608E` (placing a building) and `0000B540`. The
console opens and closes entirely inside `00006D0C`.

## Getting one on screen

```bash
DB4A_REPLAY=data/recordings/power.txt \
  DB4A_SAVE_AT=2645:data/states/buildmenu.state ./build/db4a "$ROM" 2700
```

Frame 2645 of the power-station recording sits in the console with the
highlight on EXIT and nothing pending. Frame 2660 is *not* usable: the
recording presses A there, so a state saved at 2660 dismisses the console a few
frames after it loads. Save states are gitignored — they contain cartridge
VRAM — so this is a command to re-run, not a file to commit.

## The grid

The handler is at `$8462`, called from `$288E4`, and it is a jump table on the
low four pad bits:

```
008472  movem.w  $ffbf8a.l, d0-d1     ; d0 = column, d1 = row
00847A  jmp      $847e(pc, a0.w)      ; dispatch on the direction bits
0084AC  cmpi.w   #$3, d0              ; columns 0..2
0084C0  cmpi.w   #$6, d1              ; rows 0..5
0084C8  ...      d2 = d1*3 + d0       ; linear cell index
0084D6  lea      $ffbf8e.l, a0
0084DC  move.b   (a0, d2.w), d3       ; the item in that cell
0084E0  cmpi.b   #$80, d3
0084E4  beq      $856e                ; empty -> refuse the move
0084E8  movem.w  d0-d1, $ffbf8a.l     ; commit
```

So:

| | |
|---|---|
| `$FFBF8A` | column, word, 0..2 |
| `$FFBF8C` | row, word, 0..5 |
| `$FFBF8E + row*3 + col` | item code in that cell, byte |
| `$80` | empty cell — the game refuses to move onto it |

Dumped from the state above, the table reads exactly what is on screen:

```
row 0: FF FC FB      EXIT, FIX, STOP
row 1: 01 09 80      concrete, a building, empty
row 2: 80 80 80      ...and the rest empty
```

That last point matters: **the game already tells us which cells are
clickable.** A hit test does not have to guess.

## Where the cells are

Measured by driving the highlight to a known cell and finding the red outline:

```bash
DB4A_LOAD=data/states/buildmenu.state DB4A_PRESS="2660:right" DB4A_HOLD=6 \
  DB4A_SHOTS=2700 DB4A_PPM=/tmp/x ./build/db4a "$ROM" 100
```

| cell | rectangle |
|---|---|
| row 0, col 0 (EXIT) | x 32..63, y 48..71 |
| row 0, col 1 (FIX) | x 64..95, y 48..71 |
| row 1, col 0 (icon) | x 32..63, y 72..95 |

A regular grid: **32 x 24 pixel cells, origin (32, 48)**, so

```
col = (x - 32) / 32        row = (y - 48) / 24
```

covering x 32..127 and y 48..191.

## How to drive it

Two options, and the obvious one is wrong.

**Writing `$FFBF8A`/`$FFBF8C` directly** moves the highlight but stops there.
The code after `$84E8` also writes the item code to `$FFBFA1`, calls `$8302` to
redraw the preview panel, and updates the list scroll. Warping the words leaves
the panel showing the previous item's picture and price — the map cursor
tolerates a direct write precisely because everything downstream is recomputed
from it every frame, and this is the opposite case.

**Synthesising direction presses** keeps all of that correct, and the objection
that sank it for the map cursor does not apply here: the grid is 3x6, so the
worst case is five steps rather than a 320-pixel journey at 3 px/frame. The
handler is edge-triggered — a six-frame hold moved exactly one cell in the test
above — so each step needs a press frame and a release frame, giving a worst
case around ten frames, or 200 ms, and typically far less.

## Detecting that the console is open

No RAM flag was found that reliably distinguishes open from closed: `$FFBF86`,
`$FFBF32`, `$FFBFB4` and `$FFC728` all read the same either way, and the cell
table at `$FFBF8E` keeps its contents after the console closes, so it cannot be
used as a proxy.

The reliable signal is execution, not state: **the console is open exactly when
the handler at `$8462` runs.** The dispatcher already sees every block entry,
so this costs one comparison and is exact by construction — the same trick as
the native-override table in `include/native.h`.

## The implementation

`src/buildmenu.c`, enabled with mouse control and disabled on its own with
`DB4A_MENU_MOUSE=0`.

1. `menu_probe_pc` holds `$8462` while the feature is on, and the dispatch loop
   compares each block entry against it. Zero when off, so the probe costs one
   comparison against a global and nothing else.
2. The pointer is hit-tested to `(row, col)`; cells reading `$80` and points
   outside the grid are ignored, so the highlight simply does not move.
3. The highlight steps towards the target with pulsed presses — one press
   frame, one release frame, because the handler is edge-triggered.
4. The step picks the longer axis first, but only onto a cell that exists. A
   naive per-axis walk gets stuck the moment the grid is ragged, which it
   usually is: from STOP at (0,2) straight down is `$80` and the handler
   refuses, so the walk has to go along the top row first.
5. `buildmenu_steer()` returns 1 whenever the console is open, and the callers
   skip `mouse_steer()` in that case. Both drive the d-pad, and `mouse_steer`
   would additionally be writing map-cursor variables underneath a screen that
   is not the map.

Hovering gets item previews for free: moving the highlight is what makes the
game redraw the panel, so pointing at an item shows its name and price —
CONCRETE 15, WINDTRAP 300/100/400 — exactly as if it had been reached with the
d-pad. That is the payoff for pressing buttons rather than writing the words.

```bash
make check-menu
```

Regenerates the console state from the committed recording and checks all three
top buttons, both items, an empty cell, a point off the grid, and that
`DB4A_MENU_MOUSE=0` really disables it.

## The Starport

Same console, vehicles instead of buildings. Two of its four differences would
be easy to assume and wrong, so all four are listed:

| | build console | Starport |
|---|---|---|
| d-pad handler | `$8462` | `$916C` |
| column / row | `$FFBF8A` / `$FFBF8C` | `$FFBFC8` / `$FFBFCA` |
| cell table | `$FFBF8E` | `$FFBFCC` |
| grid rows | 6 (`cmpi.w #$6,d1` at `$84C0`) | **4** (`cmpi.w #$4,d1` at `$91CA`) |
| empty marker | `$80` (`$84E0`) | **`$FF`** (`$91EA`) |

Cell geometry is shared -- 32x24 from origin (32,48) -- and was measured rather
than assumed: driving the highlight one cell right moves the changed pixels
from x 32..63 to 64..95, and one cell down from y 48..71 to 72..95, on both
screens. A live cell table reads `1C 1D 20 / 01 07 09 / 0A 0D 0F / 10 11 FF`,
which is EXIT/FIX/STOP and then eight purchasable vehicles.

**The handler does not run every frame.** The Starport's alternates, so the
probe reports starport/none/starport/none. Detection therefore holds a console
open for a few frames after its handler was last seen instead of dropping it
the first frame it is missing. Without that the steering saw a closed screen
every other frame, never finished its press/release pulse, and the highlight
did not move at all -- which is exactly how this first behaved, with every
pointer target leaving the selection at (0,0).

`DB4A_LOG_CONSOLE=1` prints which console the probe thinks is up; it is what
showed the alternation.

## The crash this came with

Pressing a direction in the Starport used to stop with `BAD PC 00009228`.
`$921E` is `jmp $9222(pc,d4.w)` indexed by byte offset, so d4 is 0/2/4/6. Three
slots hold `bra.b` entries; the fourth case's body is inlined at `$9228` and the
jump lands straight on it. The table walker stops at the first implausible
entry and `56 41` is not a `bra.b`, so the fourth handler was never decoded.
Fixed on `master` by seeding `$9228`.
