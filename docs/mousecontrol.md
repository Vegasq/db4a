# Mouse control

Point at the map and the game's cursor comes to you. Left button is A, right is
B, middle is C.

```bash
DB4A_MOUSE=1 make play
DB4A_MOUSE=1 DB4A_FULLSCREEN=1 make play
```

Off by default; the pad and keyboard are untouched when it is off.

## How it works

The game has no mouse and no concept of one. Its cursor moves a step at a time
in response to d-pad presses, and the game clamps it and scrolls the view as it
sees fit.

Rather than fight that, this reads where the cursor currently is and presses
whichever directions reduce the distance to the pointer. **The game's own
movement code does the work.** Its clamping, its edge scrolling and its idea of
where the cursor may go all still apply, so this cannot put the cursor anywhere
the game would not have allowed. Nothing in the game's logic is modified.

## Finding the cursor

`$FFBF12` (x) and `$FFBF14` (y), as screen pixels.

Found by moving the cursor and looking for a 16-bit word that changes on
left/right but *not* on up/down, and the reverse for y. That narrowed 59
candidates to three pairs. Confirmed by eye rather than by inference: driving
the cursor to each extreme and reading the variable gives 117 and 200, and the
bracket on screen sits at exactly those coordinates.

The first attempt read the cursor from the sprite table instead, which seemed
more robust because it needs no RAM archaeology. It is not: the cursor sprite
(tile `6C6`) **blinks**, so it is absent from the sprite list on roughly half
the frames.

## Only during gameplay

Steering is confined to gameplay, decided by the scene pointer at `$FFFFE002`:
`006D0C`, `00608E` (placing a building) and `00B540` are gameplay, everything
else is a menu, briefing or cutscene.

This is not a nicety. Outside gameplay the cursor variables read **(0, 0)**,
which is a perfectly plausible coordinate, so a range check passes and the
steering drives towards the pointer — holding right and down forever and making
the mentat screen impossible to get past. That is how this first shipped.

The related rule: **every path that declines to steer must release the d-pad
first.** Returning early without doing so leaves whatever was pressed held down.
`make check-mouse` drives a scripted route to the mission with steering active
and fails if the game does not arrive.

## Behaviour

A d-pad press moves the cursor about 7 pixels, so the pointer cannot be matched
exactly. Anything within 6 pixels counts as arrived — without that the cursor
oscillates around the pointer forever.

Measured convergence from the mid-mission save state, cursor starting at
(164, 101):

| Target | Settles at | Frames |
|---|---|---|
| (60, 180) | (64, 175) | ~120 |
| (250, 60) | (244, 67) | ~200 |
| (160, 110) | (164, 104) | immediate |

While the mouse is driving, it owns the d-pad — the keyboard's direction keys
are ignored, since otherwise they fight it every frame. Buttons still work from
either.

Outside gameplay the cursor variable holds nothing sensible, so a position far
off screen is taken as "no cursor in this scene" and the pad is left alone.

## Limits

- **The cursor cannot be placed exactly**, only within about 7 pixels, because
  the game moves it in steps. This is inherent to steering rather than setting.
- **It takes time to cross the screen** — roughly two seconds corner to corner,
  because each frame contributes one step. A modern RTS feel would need the
  cursor position written directly, which means understanding and modifying the
  game's cursor code rather than driving it.
