# Widescreen — design and plan

A phase-2 enhancement (M8), behind a flag, off by default. The goal is a wider
field of view — 400×224 or similar — rather than a stretched or pillarboxed
320×224 image.

Everything below was established by experiment against the running game, not
assumed. The measurements are from frame 6000 of
`data/recordings/level1atredis.txt`, mid-mission.

---

## What the hardware and the game actually give us

| Fact | Value | How it was established |
|---|---|---|
| Display mode | H40, 320×224 | reg 12 = `0x81` |
| Plane size | **64×32 cells = 512×256 px** | reg 16 = `0x01` |
| Plane A base | `$E000` | reg 2 |
| Plane B base | `$C000` | reg 4 |
| Window plane | **covers nothing during gameplay** | reg 17 = 0, reg 18 = 0 |
| Horizontal scroll | 340 at this frame | hscroll table at `$F400` |

**The planes are 512 px wide against a 320 px view — 192 px of slack already
exists.** That is what makes widescreen possible at all.

### Which layer draws what

Rendering each layer alone (plane A, plane B, sprites separately) settles this,
and the answer is not what the sidebar's appearance suggests:

- **Plane B is the world.** Terrain, buildings, everything the camera scrolls
  over. It also has a **black rectangle punched into it** where the minimap
  sits, so the minimap sprites have something to sit against.
- **Plane A is nearly empty** — a handful of markers.
- **Sprites are the entire UI.** Credits, unit portraits, the minimap frame, the
  selection brackets — *and* the units. There is no separate UI layer.

This matters: the sidebar is not a fixed region of a tilemap that could be moved
by changing a scroll value. It is a set of sprites at fixed screen coordinates.

### The experiment that proves the approach

Setting `FB_W` to 400 and rendering unchanged produces a **working wider view**:
real terrain and real units appear in the extra 80 px, not garbage or repeated
tiles. The game keeps the whole 512 px plane populated.

Two defects appear, and they define the work:

1. **The UI does not move.** It stays at x≈240–320, so it floats in the middle
   of the screen with world visible to its right.
2. **The minimap's black backing does not move either**, because it is part of
   plane B. It stays a black rectangle in the middle of the map.

---

## Design

Render the world at full width, then composite the UI at the new right edge.

```
        0                    240   320                      400
        |<------ world ------->|<--- UI --->|                 |     original
        |<---------- world ----------->|<--- UI --->|          widescreen
```

### 1. Make the framebuffer width a variable

`FB_W` is a compile-time constant used consistently in eight places
(`render.c`, `render.h`, the PPM writer, the SDL texture). The clean change is a
runtime `fb_width` with `FB_W` becoming the maximum allocation, so one binary
supports both and the flag is a runtime switch rather than a build variant.

The SDL texture and the PPM header both need to follow it.

### 2. Widen the world, keep the world centred on the same point

The extra width must not shift what the player was already looking at, or every
existing recording and screenshot desynchronises. Render world columns from
`-(extra/2)` to `320 + extra/2`, so the original 320 px view stays where it was
and the new pixels are added symmetrically.

Plane sampling already wraps correctly (`sample_plane` takes any x), so this is
a loop-bound change.

### 3. Classify sprites into world and UI

From the sprite table at frame 6000:

| Sprites | Position | Priority | What they are |
|---|---|---|---|
| links 0–2 | x=256, 272 y=16–48 | 1 | credits, portraits |
| links 10–11 | x=240, 272 y=176 | 1 | minimap |
| links 12–15 | x=81–137 | 1 | selection brackets — **world space** |
| links 16+ | scattered | 0 | units — world space |

Priority alone does not separate them, because the selection brackets are also
priority 1. **Position does**: a sprite whose horizontal extent lies entirely at
x ≥ 240 is sidebar UI. Everything else is world.

That rule is simple, cheap, and testable. It should be implemented as a single
predicate with the threshold named, so it can be corrected in one place if a
later mission puts UI elsewhere.

- **World sprites**: offset by the same amount as the world, so they stay
  aligned with the terrain.
- **UI sprites**: offset by `fb_width - 320`, pinning them to the right edge.

### 4. Deal with the minimap's black backing

Plane B's black rectangle would otherwise stay mid-map. Options, in order of
preference:

1. **Draw the UI backing ourselves.** Fill the sidebar region at the new right
   edge with the backdrop colour before drawing UI sprites, and let the world
   render normally underneath where the hole used to be. The hole then shows
   terrain — which is correct, since it is simply map the player could not see
   before.
2. Copy the plane-B strip. More faithful to the original pixels, but it drags
   the hole along with it and needs the same classification work anyway.

Option 1 is simpler and produces the better image.

### 5. Leave the game's logic alone

The game still believes the screen is 320 wide. That is deliberate and must
stay: cursor bounds, scroll clamping and mouse/cursor mapping are all the game's
own arithmetic, and changing them means patching game code, which crosses from
"presentation" into "modifying the game" and breaks the reference oracle.

Consequence: **the extra width is a view, not extra play area.** The cursor
still cannot travel into it. That is the honest limitation of this approach and
should be documented rather than papered over.

---

## Risks and how to check them

| Risk | Check |
|---|---|
| Stale tiles at the edges when the camera scrolls fast | scroll hard in a mission and watch the new columns; the game may only maintain a few columns beyond the view |
| A later mission or menu puts UI outside x ≥ 240 | run `tests/houses.sh` and the menu scenarios in widescreen and look |
| Menus and cutscenes are 320-wide compositions | widescreen should be **disabled outside gameplay**, or they letterbox |
| Sprite X is 9-bit (−128…383) on hardware | our renderer is not bound by that, but a UI sprite pushed past 383 would be off-hardware; only matters if we ever feed positions back to game code |

**Fidelity is not at risk.** With the flag off, nothing changes: the same code
path, the same 320×224, and every existing test still compares against the
reference. The widescreen path must never be the default, and
`make compare-screen` must keep running in 320.

---

## Trying it before it is finished

Steps 1 and 2 are implemented as a spike so the feel can be judged before the
rest is built. The world widens and stays centred; the UI moves with it and so
sits short of the right edge — pinning it there is step 3.

### Display options

All frontend-only; none of them touch emulation.

| | |
|---|---|
| `DB4A_WIDE=398` | widescreen, gameplay only (see above) |
| `DB4A_FULLSCREEN=1` | start fullscreen |
| **F11** | toggle fullscreen at any time |
| `DB4A_INTEGER=1` | whole-number pixel scaling |
| `./build/db4a-sdl <rom> <scale>` | window scale, default 3 |

Fullscreen uses SDL's *desktop* flavour: it keeps the monitor's current mode and
scales into it rather than changing resolution, so there is no mode switch and
alt-tab behaves. Aspect is preserved by letterboxing.

On scaling: by default SDL scales fractionally to fill as much of the screen as
the aspect allows, which means some source pixels cover more output pixels than
others — on pixel art that shows as uneven edges, and it is most obvious in
fullscreen where the factor is rarely a whole number. `DB4A_INTEGER=1` restricts
it to whole multiples, which looks cleaner at the cost of a wider border. Which
is better is a matter of taste, so it is a flag rather than a decision.

F11 is the toggle rather than the more usual Alt+Enter because Alt is bound to
the B button, so that chord would also press two game buttons.

### Picking a width

The window is `fb_width * scale` by `224 * scale`, so its pixels are square.

| Want | `DB4A_WIDE` |
|---|---|
| **16:9** | **398**, or 400 — 400 is 1.786:1 against 16:9's 1.778:1, a 0.4% error nobody can see |
| 16:10 | 358 |
| Original 4:3 | 320 (the default; see the note below) |

A caveat on the 320 default: the Mega Drive showed 320×224 stretched to 4:3, so
its pixels were slightly taller than wide (0.933). We render square pixels, so
the default view is already 1.429:1 rather than the 1.333:1 a TV gave — the
whole image is a little wide compared to original hardware. Correcting that is
a separate question from widescreen, and would mean scaling the window's height
rather than changing `DB4A_WIDE`.

If you *did* want 16:9 with the original pixel shape preserved, the width would
be 427 — but our square-pixel window would then show it at 1.906:1, which is
wider still. 398 is the right answer for the renderer as it stands.

```bash
make explore                 # 320, resumed mid-mission at frame 6000
make explore WIDE=400        # the same, widescreen
make explore WIDE=448 STATE=data/states/mission1-f6000.state
```

`make explore` resumes from a save state rather than replaying from boot, so
you arrive in the mission with live control. The state is regenerated from the
recorded playthrough if missing, so it is reproducible rather than an opaque
binary.

Wide screenshots without a window:

```bash
DB4A_WIDE=400 DB4A_LOAD=data/states/mission1-f6000.state \
    DB4A_SHOTS=6060 DB4A_PPM=build/w ./build/db4a "$ROM" 100
```

Worth looking for specifically: stale or repeated tiles at the extreme edges
while scrolling, sprites appearing or vanishing at the new boundaries, and
whether the wider view makes the cursor's inability to reach the new area feel
wrong.

## Telling gameplay apart from everything else

The first attempt inferred the scene from what was on screen — a high-priority
sprite in the sidebar region — which worked for gameplay and menus but produced
oddities elsewhere, because cutscenes and briefings are not obliged to respect
that rule.

The game answers the question directly. Its main loop dispatches through a
function pointer at `$FFFFE002`, so **that pointer is the scene identifier**.
Traced across a full playthrough and confirmed identical on all three houses:

| `$FFFFE002` | Scene | Widescreen |
|---|---|---|
| `006D0C` | gameplay | **yes** |
| `00608E` | gameplay, placing a building | **yes** |
| `00B540` | gameplay | **yes** |
| `017C32` | publisher logos | no |
| `024724` | mentat / world map | no |
| `024812` | house select | no |
| `004500` | transitions | no |
| `000000` | cutscenes, including Victory and Defeat | no |

Outside gameplay the view stays at its original 320 and is centred, with the
surplus left as black bars. Verified frame by frame: logos, mentat, Victory and
the world map all pillarbox; gameplay and building placement go full width.

`DB4A_WIDE_SCENES=6d0c,608e,b540` overrides the set, for a mission or house
that turns out to use a handler not listed here.

## Status: UNPARKED 2026-08-23, rebased onto remaster

Brought forward from its old base onto `remaster` so the remaining fault can be
reproduced from a recorded session rather than argued about from still frames.
The account below of *why* it was parked still stands and has not been
weakened — nothing here fixes the underlying reconstruction problem.

What the rebase changed:

- **Three of the nine commits were partly or wholly redundant.** Fullscreen,
  integer scaling, F11 and `DB4A_LOAD` were all reimplemented independently on
  remaster while this branch sat parked, and remaster's versions read through
  `cfg()`. Those were resolved in remaster's favour; the widescreen-specific
  part (sizing the window and the logical size from `fb_width` rather than
  `FB_W`) was kept.
- **One real bug was created by the rebase, and fixed.** The sprite-priority
  mask `plane_hi` — the fog-of-war fix — landed on remaster during the park,
  so the two met for the first time here. `render_frame()` has two early
  `continue` paths belonging to widescreen (the pillarbox bars, and the columns
  where the extension would wrap the 512-pixel plane); neither wrote
  `plane_hi`. That array is static and which columns are bars or wrap moves as
  the view scrolls, so a stale 1 made the sprite pass drop low-priority sprites
  over columns that were by then plain backdrop. Units would vanish near the
  left edge with nothing wrong in the plane itself.
- **Sprites are now clipped out of the pillarbox.** The hardware clips sprites
  to the 320-pixel window; when a 320 composition is centred, the bars are not
  part of that window. In gameplay the clip opens to the full width, because
  there the extension is meant to reveal more.

Verified after the rebase:

| | |
|---|---|
| Default 320 output | **byte-identical** to remaster at frames 1320, 2800, 6000, 9000, 12000 |
| `check-native`, `check-menu`, `check-menus`, `check-state`, `check-houses` | all pass |
| `wide = 400` from `db4a.conf` | applies in the SDL build, ignored by the headless build |
| `DB4A_WIDE` over a conf entry | env wins, as the config contract requires |

The byte-identical result is the one that matters for the branch policy: with
widescreen off, this branch renders exactly what the faithful build renders.

### Reproducing the fault

```bash
make record REC=data/recordings/wide.txt WIDE=400
```

Scroll around, especially into and along the map's western and northern edges,
and where units cross the boundary between the game's own 320 columns and the
extension. The extension is at the **left**, so anything wrong will be there
while the right-hand HUD stays correct. `make replay REC=... WIDE=400
SHOTS=<frames>` then re-renders any moment headlessly for inspection.

What is expected to be wrong, from the analysis below: the extra columns are
reconstructed from tilemap contents the game has not updated, so they should
lag or smear when the view moves and settle into something plausible when it
stops. If the fault turns out **not** to match that description, that is the
useful finding — it would mean there is a second, separate bug, which is the
outcome this rebase is meant to establish one way or the other.

## What the recording actually showed — and what it corrects

`data/recordings/wide.txt`, played at `WIDE=400`. Reported symptom: **the
leftmost strip goes black when scrolling left, but not when scrolling right.**
Reproduced, and traced. Two things in the analysis below turn out to be wrong.

### It is not the wrap guard

The obvious suspect was the "refuse to wrap the plane" guard, and it looked
guilty: it fires on 245 frames, and the condition it tests is genuinely wrong.
It asks whether the extension straddles **plane column 0** — the 512-pixel
ring's origin, an artifact of where the tilemap happens to start — which
reduces to `(hscroll mod 512) <= 80`. That has nothing to do with the map's
edge, which is what the comment claimed it detected.

But it is also **inert**. Rendering the 32 frames where it is most active with
and without it (`DB4A_WIDE_NOGUARD=1`) gives byte-identical output, 32 of 32 —
including at the map's west edge, where hscroll pins at 512 and it blanks all
17920 extension pixels. Every pixel it paints was already backdrop. The guard
is kept, with the measurement recorded in place of the old reasoning, but it
explains nothing and should not be trusted.

### The extension is never WRONG. It is either right or absent

This is the correction that matters. The doc below says the extra columns are
"a ring buffer, not spare map" and that a good-looking still is "luck". That
is not what the measurements show.

Where the extension has content, that content is **the correct map**. Take a
frame at hscroll H and a frame at hscroll H+80: the second is showing, in its
own real 320-pixel view, exactly the columns the first put in its extension.
They agree:

| | |
|---|---|
| frame 3020 extension vs frame 3106 columns 81–160 | **99.0%** identical |
| frame 3065 extension vs frame 3106 columns 80–159 | **99.0%** identical |
| frame 3066 extension vs frame 3107 columns 82–161 | **98.9%** identical |

The residual 1% is sprites, which move between the two frames. The terrain
matches. The extension is not showing stale content from the far side of the
ring — it is showing the genuine neighbouring map.

When it is not showing that, it is showing **black**: not wrong tiles, just
tiles the game has not drawn.

### Which is why the fault is directional

The extension sits to the **west** of the game's view. So it is *behind* the
camera when you scroll east and *ahead* of it when you scroll west, and the
game only maintains the columns it believes are visible:

| scrolling | extension is | mean visible pixels (of 17920) |
|---|---|---|
| east (hscroll falling) | behind the camera, recently drawn | **2521** |
| west (hscroll rising) | ahead of the camera, not yet drawn | **439** |
| stationary | not being maintained | 299 |

Scroll east and you are uncovering ground the game drew a moment ago. Scroll
west and you are asking for ground it has not drawn yet, so you get black.
That is the reported asymmetry, measured.

### What this changes

The feature is in better shape than the post-mortem below believed. The
failure mode is *missing* map, not *wrong* map, which is a much easier thing to
fix and a much less objectionable thing to look at. It also means option 2
below — making the game maintain a wider view — is aimed at the right target:
the game's tilemap update is drawing exactly the visible columns, and would
need to draw a margin on the leading edge.

It does **not** rescue the current implementation. Black bars that appear when
you scroll one way are still not shippable. But "the renderer is inventing
terrain" was the reason to abandon the approach, and that reason is not true.

## Option 2, implemented: drawing the missing columns

`src/widescreen.c`. The cartridge's column-draw is reimplemented in C and used
to draw the columns the extension needs, which the game does not draw because
it only maintains what it believes is visible.

### How the cartridge draws a map column

Neither routine has a static caller -- both are reached through a function
pointer -- so they were found with `DB4A_LOG_NT` (which block wrote the
nametable) and read with `DB4A_REGS` (what it was passed):

| | |
|---|---|
| `$7504` | plane A, indexes the tile table by the map's high byte × 16 |
| `$7468` | plane B, indexes it by the map's low 9 bits × 32 |

Both set VDP autoincrement to 128 — one nametable row — so they write *down* a
column of 32 entries, and both use the one table at `$4ADE8`.

| register | meaning |
|---|---|
| `d1.w` | nametable address of the column's first entry |
| `a3` | map pointer; one long per **pair** of columns, `$100` per map row |
| `d0` | table phase, which is `(col * 2) mod 32` |
| `$FFC198` | when set, map values ≥ `$67` draw as 0 — the shroud |

Every one of those was checked against the running game across a full scroll,
not inferred from the disassembly.

### It is an observer, not a native override

It does not replace the block, spend cycles, or touch a register. It writes
tilemap entries the game will not read at 320 and would have written itself had
it thought the screen were wider. So `check-native` does not apply, and there
is no equivalence contract to keep.

### Two things that had to be right

**Aim from the camera, not from the column being drawn.** Scrolling east the
cartridge draws at the far edge; columns counted west from *there* land inside
the visible picture, where an arithmetic slip corrupts the real view instead of
the extension.

**Then only act at the west edge.** Extrapolating the map pointer ~40 columns
to redraw the eastern case made it *worse* — mean visible pixels 2521 → 131 —
because the pointer relation holds locally, not across the ring. Drawing ahead
is only ever needed in the direction of travel; going east the extension is
behind the camera and already correct.

### Result

Mean visible pixels of 17920 in the extension, `data/recordings/wide.txt`:

| scrolling | before | after |
|---|---|---|
| west | 439 | **2274** |
| east | 2521 | 2553 |
| stationary | 299 | 345 |

The asymmetry is gone. And both properties that matter hold:

- the cartridge's own 320 view is **byte-exact** inside the widened frame,
  10/10 sampled frames, including east scrolls
- the new content is the **real map**: one frame's extension against a later
  frame showing those same columns in its own view agrees **98.9–99.0%**, the
  residual being sprites that moved between them

At the map's west edge it stays black rather than inventing terrain, which is
what the game itself shows there.

`DB4A_WIDE_FILL=0` disables it, for A/B.

### What is still open

Stationary frames are still mostly blank (345 of 17920). The fill only runs
when the cartridge draws a column, which it does only while scrolling
horizontally — so stopping somewhere the extension was never filled leaves it
blank until you move again. Vertical scrolling is fine: `$75B0` redraws whole
64-column rows, margin included.

Whether anything else looks wrong in motion is not yet established. This fixes
the black edge that was reported; it is not a claim that widescreen is now
correct.

## Why it was parked, and the analysis that still stands

*The section below predates the measurements above. Where the two disagree —
specifically the claim that the extension shows stale wrapped content — the
measurements win.*

Two rounds of fixes made the captured frames look right — the map-edge wrap is
genuinely gone and the game's own 320 view is provably unaltered inside the
widescreen frame — but **it still looks wrong in actual play**. Whatever remains
is not visible in the still frames I was checking against, which is exactly the
trap this whole feature kept falling into.

Parked deliberately rather than patched further. The renderer is reconstructing
what the game meant from a tilemap that only ever describes 320 pixels, and each
fix so far has addressed one symptom of that without changing the underlying
position. Revisit when more of the game is converted to readable C and the
camera and tilemap code can be understood directly rather than inferred from
VRAM, at which point the honest version of this feature — the game itself
maintaining a wider view — becomes possible.

What is worth keeping from the attempt, all of it independent of widescreen:
save states, `make explore`, fullscreen, integer scaling, the SDL pacing fix,
and the scene identifier at `$FFFFE002`, which turned out to be the reliable way
to know which screen the game is on.

## What working looked like on paper

Implemented and flag-gated. Gameplay anchors the HUD flush right with its
backdrop aligned, menus are centred and pillarboxed, and the 320 path is
byte-identical to before.

One artifact appeared in real play and has since been fixed; see below.

While the camera scrolls, the extra columns show **stale tilemap** — content
left over from where the camera used to be. Driving the cursor hard left and
then right from the mid-mission state puts a building fragment at the far left
edge that does not belong there.

This was measured at four widths, and every one of them shows it:

| Extra width | Result while scrolling |
|---|---|
| +16 px | sliver of stale content at the left edge |
| +32 px | clearly visible |
| +48 px | clearly visible |
| +80 px | a whole building fragment |

**The game maintains no usable margin.** The planes are 512 px wide, but the
game only writes tiles for the 320 px it believes are visible, leaving the rest
as whatever was there when the camera was somewhere else. The 192 px of "slack"
that made this look easy is a ring buffer, not spare map.

It looks correct at frame 6000 because the camera has been stationary long
enough for the surrounding columns to be whatever was last drawn there. That is
luck, not a property to build on.

### The cause, and the fix

The artifact showed up as a detached lump of terrain at the map's western edge.
Comparing the same moment at 320 and at 398 settles what is happening:

- At 320 the game draws **plain black** there. There is nothing west of the map,
  and it knows.
- At 398 the extension shows terrain, because sampling past the map's edge
  **wraps around the 512-pixel plane** and comes back on the map's eastern side.

So the game was never scrolling too far, and nothing in its logic needs
changing. The renderer was wrapping where the game shows nothing.

The fix is to refuse the wrap: for a column in the extension, if it does not sit
in the same 512-block as the leftmost column the game itself is drawing, paint
the backdrop instead. Ten lines, no game code touched.

Verified across a full scroll sweep — into the western edge, back across the
map, out to the east — with no stray terrain at any point. And the property
that matters: **the game's own 320 view appears byte-identical inside the
widescreen frame, 100.0% of sampled pixels at both extremes.** The extension
only ever adds; it never alters what the game intended to draw.

### Where that leaves it

Three ways forward, none of them small:

1. **Repurpose the extra width for the HUD** rather than for more map. The world
   keeps its valid 320 columns and the HUD moves out of them, so the player
   gains the map currently hidden *underneath* the HUD. Artifact-free, because
   nothing new is sampled from the tilemap. The catch is plane B's black
   rectangle behind the minimap, which stays in the map area and would need
   filling or leaving as a hole.
2. **Make the game maintain a wider tilemap** by patching its update routine.
   This is real reverse engineering, and it crosses from presentation into
   modifying the game, which breaks the reference oracle.
3. **Reconstruct the edge columns** from the game's own map data in RAM, which
   needs the map format worked out first.

Option 1 is the only one that stays on the presentation side of the line.

The current implementation is kept, behind `DB4A_WIDE`, because it is the
evidence for all of the above and because it is genuinely fine in a static
scene. It should not become a default or be advertised as working.

## Plan


1. `fb_width` as a runtime value; `FB_W` becomes the maximum. Verify 320 output
   is byte-identical to before — `make compare-screen` and the mission replay.
2. Widen the world render symmetrically. Screenshot at 400 and confirm the
   original view is unshifted.
3. Add the world/UI sprite predicate and offset each group. Confirm the UI pins
   to the right edge and units stay on their terrain.
4. Draw the UI backing at the new position; confirm no black rectangle remains
   mid-map.
5. Gate on `DB4A_WIDE=400` (or similar), default off. Confirm the default path
   is unchanged by the full regression set.
6. Restrict to gameplay, so menus and cutscenes stay 320.
7. Scroll test for stale edge columns; document whatever is found.

Steps 1–2 are mechanical. Step 3 is where the real uncertainty is, and step 7 is
the one most likely to produce an unpleasant surprise.

---

## Tooling added while investigating

- `DB4A_VRAM=file` dumps VRAM and CRAM for offline inspection of the tilemaps.

Two throwaway diagnostics were used and then reverted rather than committed,
since both put a `getenv` in the per-pixel loop: a plane mask
(`DB4A_PLANES=1|2|4`) to render layers separately, and a sprite-table dump.
Either is a few lines to reinstate if needed — the plane mask in particular is
what identified that the UI is sprites.
