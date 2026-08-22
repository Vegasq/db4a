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
