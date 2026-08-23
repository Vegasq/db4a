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
| `00017C32` | Virgin / Westwood intro, DUNE title | no |
| `00013144` | attract-mode terrain tutorial, after ~4000 idle frames | no |
| `00004500` | **house selection** | **yes** |
| `00024724` | mentat: house description, then the join question | **yes**, at the question |
| `00024812` | region map | not investigated |
| `00006D0C` | gameplay (and the build console, as a sub-mode) | done |
| `0000608E` | placing a building | done |

**There is no main menu.** Booting with no input at all goes intro -> title ->
attract loop; there is no New Game / Options screen to click. The flow is
intro, house selection, mentat, region map, mission.

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

## What implementing these needs

Both follow the build console's pattern and neither needs a native override:

1. A probe slot per screen — blocks `$4808` and `$25CF4` — since neither screen
   can be identified from the scene pointer or from RAM alone. The existing
   probe is a single global; it would become a small array.
2. Hit test the pointer against the rectangles above.
3. Step towards the target with pulsed presses, respecting `$FFBF02` on the
   house screen so a direction is not thrown away mid-slide.
4. Left click already maps to A, which confirms on both.

## Not investigated

The region map (`00024812`) between the briefing and the mission. It shows the
territory being fought over and may be purely presentational, but that has not
been checked.
