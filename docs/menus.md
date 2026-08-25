# Where else the pointer can work

A survey of every screen between boot and gameplay, to see which could take
mouse input. Companion to [buildmenu.md](buildmenu.md), which covers the build
console. Everything here is measured; commands are given so each claim can be
re-checked.

## The screens

Logged with `DB4A_LOG_SCENE=1`, which prints the function pointer at
`$FFFFE002` whenever it changes.

| scene | screen | interactive? |
|---|---|---|
| `00017C32` | Virgin / Westwood intro | no |
| (none) | DUNE title, then the START GAME menu at frame 2164 | not yet |
| `00013144` | attract-mode terrain tutorial, after ~4000 idle frames | no |
| `00004500` | **house selection** | **yes** |
| `00024724` | mentat: house description, then the join question | **yes**, at the question |
| `00024812` | region map | not investigated |
| `00006D0C` | gameplay (and the build console, as a sub-mode) | done |
| `0000608E` | placing a building | done |

**The title menu has no scene of its own.** `$FFFFE002` holds `00017C32` from
frame 351 to 1404 and then goes back to zero, because the planet zoom, the DUNE
title and the START GAME / OPTIONS / TUTORIAL menu are one routine that waits
for vblank itself rather than returning to the dispatcher -- the same shape as
the mentat's YES/NO. So it is identified by its input loop `$178C8` running,
which is what `src/skipintro.c` watches. This survey's first pass concluded
there was no main menu at all, on the strength of that scene log; the menu is
plainly there on screen from frame 2164.

The flow is intro, title menu, house selection, mentat, region map, mission.

```bash
DB4A_LOG_SCENE=1 ./build/db4a "$ROM" 6000        # unattended boot
```

## House selection — clickable

Three shields, left/right to move, any button to confirm.

There is no index variable. The game stores the highlight's X position and
derives the house from it at the moment you confirm:

```
004948  cmpi.w #$20, $ffbef8.l    ; left guard, x=32
004952  move.w #$ffa8, $ffbf02.l  ; LEFT  -> slide -88
00496E  cmpi.w #$c8, $ffbef8.l    ; right guard, x=200
004978  move.w #$58,  $ffbf02.l   ; RIGHT -> slide +88
004986  andi.w #$70, d0           ; A, B or C confirms
00498C  cmpi.w #$20, $ffbef8.l    ->  Atreides
00499C  cmpi.w #$78, $ffbef8.l    ->  Ordos
0049AC  cmpi.w #$d0, $ffbef8.l    ->  Harkonnen
```

| | |
|---|---|
| `$FFBEF8` | highlight X: 32 Atreides, 120 Ordos, 208 Harkonnen |
| `$FFBF02` | pixels left to slide; **must be 0** or a direction is ignored |
| block `$4808` | the per-frame handler; slides 4 px and plays a click on arrival |

The shields and their labels occupy exactly `x 32..111`, `120..199`, `208..287`
over `y 33..151` — the highlight X values are the left edges, 88 apart.

Driving it means pressing left/right and waiting for `$FFBF02` to reach 0
between steps. Two steps maximum, each 22 frames of slide, so under a second
worst case — and the game keeps its own animation and click.

**Scene `00004500` is not enough to identify this screen.** It is also used for
loading transitions between gameplay segments; it appears twice before the
first mission alone. Detection needs the same trick as the build console:
watch for the handler block to run.

## The mentat — clickable at the question only

The sequence is: two or three screens describing the house, then
"DO YOU WISH TO JOIN HOUSE ATREIDES?" with **YES/NO buttons**, then the mission
briefing.

The buttons are easy to miss. They appear only after the question has been on
screen for a while — sampling at 25-frame intervals across the question showed
no buttons at all, and the first conclusion drawn from that was that the whole
mentat sequence had nothing selectable. It was wrong. They are visible by frame
1700 of `data/recordings/level1atredis.txt`.

| | |
|---|---|
| YES | `x 195..270`, `y 170..181` |
| NO | `x 193..271`, `y 192..205` |
| selector | three sprites at `$FFA62D`/`$35`/`$3D`, Y `$28` = YES, `$40` = NO |
| block `$25CF4` | writes the selector every frame while it is up |

Up/down moves between them, A confirms. Same shape as house selection: the
game stores the selector's position, not an index.

The description and briefing screens on either side have nothing to select —
any button pages them, and left click already does that, so they need no work.

## Reproducing

Save states are gitignored (they contain cartridge VRAM), so these are commands
rather than files:

```bash
R=data/recordings/level1atredis.txt
DB4A_REPLAY=$R DB4A_SAVE_AT=1320:build/house.state   ./build/db4a "$ROM" 1400
DB4A_REPLAY=$R DB4A_SAVE_AT=1487:build/confirm.state ./build/db4a "$ROM" 1550
DB4A_LOAD=build/confirm.state DB4A_SAVE_AT=1700:build/yesno.state ./build/db4a "$ROM" 400
```

## Implemented

`src/menus.c`, enabled with mouse control and turned off by `DB4A_MENU_MOUSE=0`
along with the build console. `make check-menus`.

`tests/menus.sh` probes both screens at 320 and at `DB4A_WIDE=400`. The
widescreen probes are the 320 ones plus 40, because both screens run under a
scene that is not in the renderer's gameplay set -- `$004500` for house
selection, `$024724` for the mentat -- so `render_world_offset()` centres a
320-wide composition and returns `(400-320)/2`. That was measured, not assumed:

```bash
DB4A_LOAD=build/house.state DB4A_SCENE=1 ./build/db4a "$ROM" 130   # 004500
DB4A_LOAD=build/yesno.state DB4A_SCENE=1 ./build/db4a "$ROM" 130   # 024724
```

The coordinates matter more than they look. A widescreen probe only tests the
logical-to-game conversion if the same number means something different with
and without it -- 270 is inside the mentat's YES plate either way and proves
nothing, while 290 is inside it only after the offset comes off. Deleting the
`- render_world_offset()` in `src/main.c` must make the wide probes fail, and
with the coordinates chosen here three of them do.

The probe became a small array (`include/probe.h`) so each screen has its own
slot. Neither screen needs a native override.

**Two things this got wrong first, both worth keeping.**

*The mentat probe was put on the wrong block.* `$25CF4` writes the selector's
three sprites, which looked like the obvious detector — but it only executes
while the selector is actually travelling, so it reported "no screen" exactly
when the screen was idle and waiting for input. The probe belongs on `$25CAE`,
the loop head. That screen is not a dispatched handler at all: `$25C82` runs
its own loop, waiting for vblank itself at `$FD4`, reading the pad, and sliding
the selector 2 px a frame for twelve frames with more vblank waits inside.

*The selector's Y is a sprite coordinate.* A RAM diff shows the byte at
`$FFA62D` going `$28` to `$40`, and reading those as the values to compare
against never matches: the word at `$FFA62C` is `$128` and `$140`, because
Mega Drive sprite Y carries a +128 offset. `$128` is screen y 168 and `$140` is
y 192, which is where the plates were measured.

Both screens animate, so mid-slide the position matches neither choice. The
steering waits rather than guessing, which also stops it throwing presses at a
handler that discards them.

## Not investigated

The region map (`00024812`) between the briefing and the mission. It shows the
territory being fought over and may be purely presentational, but that has not
been checked.
