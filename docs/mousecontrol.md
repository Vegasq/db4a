# Mouse control

Point at the map and the game's cursor comes to you. Left button is A, right is
B, middle is C.

```bash
DB4A_MOUSE=1 make play
DB4A_MOUSE=1 DB4A_FULLSCREEN=1 make play
```

Off by default; the pad and keyboard are untouched when it is off.

## How it works

The game has no mouse and no concept of one. Its cursor is moved a step at a
time by the d-pad, and the game clamps it and scrolls the view as it sees fit.

This works in two halves. `src/mouse.c` writes the cursor position and the
clamp box from the frontend, once per frame, between frames. `src/cursor.c` is
a native override: the cartridge's edge-scroll routine reimplemented in C, so
the constants that decide when the map scrolls are variables instead of
instruction immediates. See [natives.md](natives.md) for the override
mechanism and what it costs.

Both are inert unless mouse control is on. With `DB4A_MOUSE` unset the override
is not taken at all and the frontend never writes to RAM, so the default build
is byte-for-byte what it was.

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

## The system pointer

Two cursors on screen at once — the OS pointer and the game's, several pixels
behind it — reads as a bug even though both are working. So the system pointer
is hidden, but *only* while `mouse_steering_active()` is true: mouse mode is on
**and** the scene is one the game draws a cursor in.

Hiding it unconditionally is worse than leaving it alone. Menus, briefings and
cutscenes have no game cursor, so a blanket hide leaves those screens with no
pointer at all while the mouse still moves it invisibly — the window looks
frozen or broken. Tying visibility to the same predicate that gates steering
means there is always exactly one cursor: the game's during play, the system's
everywhere else.

SDL hides the pointer only over our own window, so the desktop is unaffected
and alt-tabbing away gives the pointer back for free.

`DB4A_SYSCURSOR=1` keeps the system pointer visible throughout, which is useful
when checking how far the game's cursor is lagging the real one.

## Behaviour

The cursor is written straight to `$FFBF12`/`$FFBF14`, so it is under the
pointer on the same frame. There is no chase, no acceleration curve and no
deadzone.

This replaced synthesised d-pad presses, which could never have worked. The
cartridge moves the cursor at one to three pixels a frame -- it accelerates by
1/16 px per frame from `$FFBF40` and caps at `$30000` -- so crossing the screen
takes over two seconds however cleverly the presses are chosen. The cursor was
not being steered badly; it was being asked to travel somewhere it physically
cannot reach in time.

Writing the position directly is safe because the ROM recomputes everything
downstream -- the map cell under the cursor, the sprite, the selection -- from
the position every frame, and because with no direction held its update routine
takes the `$6EB8` branch, which resets the speed and jumps straight to that
recompute without touching the position. Our value survives the frame intact.

**Last input wins.** If the pointer has not moved since the previous frame the
cursor is left alone, so the keyboard and pad still work and the mouse does not
snatch the cursor back. The exception is the scroll band, where the cursor has
to be held in place against `src/cursor.c` pulling it out -- otherwise parking
the pointer at the edge would scroll for three frames and stop.

## Scrolling, and where the real problem was

The first thing that looked wrong was the clamp box at `$FFBF1A`-`$FFBF20`,
which keeps the cursor 24 pixels clear of every edge. That is real, and it is
rewritten every frame with a 4-pixel margin so the outermost strip of the map
can be pointed at. It was not what made the cursor unfollowable.

The actual cause was a second, much larger dead zone. The routine at `$706C`
scrolls the map whenever the cursor leaves **X 120..200, Y 82..142** -- a box a
quarter of the screen wide, on a 320x224 display. Point anywhere else and the
map scrolls and drags the cursor back toward the middle. Warping the cursor to
(40, 40) and watching it crawl back to (120, 82) at three pixels a frame is
what finally identified it.

That routine is now `src/cursor.c`, and with mouse control on the dead zone
becomes the whole screen except a band at the edge:

| | ROM | mouse mode |
|---|---|---|
| dead zone X | 120..200 | 24..296 |
| dead zone Y | 82..142 | 24..200 |
| cursor clamp | 24 px | 4 px |
| top scroll speed | 3 px/frame | 6 px/frame |

`DB4A_MOUSE_EDGE` sets the band, `DB4A_MOUSE_CLAMP` the clamp margin, and
`DB4A_SCROLL_MAX` the top speed. The gain is derived rather than fixed: it is
chosen so that pushing the pointer right into the corner reaches the speed cap
given how deep the cursor can actually get, because a fixed gain either crawls
with a narrow band or slams with a wide one.

## Display

Fullscreen and scaling live here too, since they are wanted regardless of the
mouse:

```bash
DB4A_FULLSCREEN=1 make play    # start fullscreen; F11 toggles any time
DB4A_INTEGER=1 make play       # whole-number pixel scaling
```

Fullscreen keeps the monitor's current mode and scales into it, so there is no
mode switch. F11 rather than Alt+Enter, because Alt is bound to the B button.

## The build console

Pointing at an icon in the Construction Yard's console selects it, and left
click activates it. The console is a 3x6 grid navigated with the d-pad; it is a
sub-mode of gameplay rather than its own scene, so it is detected by watching
for its input handler to run. `src/buildmenu.c` and
[buildmenu.md](buildmenu.md) have the model and the measurements.

While the console is open it owns the d-pad and the map cursor is left alone.
`DB4A_MENU_MOUSE=0` turns just this part off.

## Before the mission

House selection and the mentat's YES/NO both take the pointer: point at a
shield or an answer and the highlight goes there, left click confirms. The
mentat's description and briefing pages have nothing to select and already
worked, because left click is A. [menus.md](menus.md) has the survey, including
why the title sequence and the attract mode need nothing and why there is no
main menu to click.

## Limits

- **Only during gameplay.** Menus, briefings, the mentat screen and the build
  list are all navigated with the keyboard; the pointer does nothing there.
- **The override is gated on mouse mode**, not on by default, because
  collapsing eight blocks into one C function moves where the 68000 yields to
  the Z80. Nothing is computed differently, but sprites end up a pixel or two
  out over a mission. [natives.md](natives.md) has the measurement and the two
  ways to fix it properly.
- **Scrolling is still the cartridge's**, driven through `$FFBF34`/`$FFBF36`.
  Owning the camera as well would mean taking on the clamp at
  `$FFE3CE`-`$FFE3D4` and the tile fetch behind it. That is the next routine to
  migrate, not this one.
