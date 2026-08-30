# Taking control of rendering

A plan for drawing the game ourselves, from its own state, instead of
emulating a VDP and patching the result.

---

## Why the current design keeps failing

`src/render.c` is a VDP. It paints what is in VRAM: nametables, the sprite
attribute table, CRAM. That is exactly right for reproducing the cartridge and
exactly wrong for showing more than the cartridge intended, because the
cartridge only ever fills those structures for the 320x224 it believes in.

Everything we have built on top is a workaround for that one fact:

| workaround | what it patches around |
|---|---|
| `widescreen.c` column fill | the tilemap has no columns outside the view |
| the `$11B4` cull override | the emitter discards sprites outside the view |
| the `$11DC` band override | ...and charges them against a per-band budget |
| the sprite appender | ...so instead we bolt entries on afterwards |
| `mapview.c` | the tilemap is the wrong source; read the map instead |

The last one is the only one that removed a class of bug rather than an
instance. It is also the model for what follows: **stop reading the
cartridge's output, read its state.**

The scoreboard is honest about the rest. Tasks #31, #33 and #34 are all the
same shape — a sprite or a column that the cartridge declined to produce, which
we then try to reconstruct from what it did produce. #33 in particular is the
cursor simply vanishing, because the emitter culled it and our appender could
not find the object again.

## What this is, and what it is not

**It is** drawing the world layer and the object layer ourselves, from the
game's map and object list, at any resolution, with no 80-sprite table, no
per-band budget and no culling to 320.

**It is not** a new engine. The simulation stays exactly as it is: recompiled
cartridge code, driving the same RAM. We take the *view*, not the game.

**It is not** asset extraction. Tiles live in VRAM because the game uploads
them there; we read them at runtime, as we already do. Ripping tiles to files
would be a different project with a different licence question, and it buys
nothing here.

**The VDP path stays.** It is the oracle. At 320x224 the new renderer must
produce the same picture, and the only way to know that is to keep the old one
and diff. This is precisely how `mapview` was validated — 39 million tiles at
0.03-0.09% — and it is why that piece has not regressed since.

## What we already own

Established by measurement this week, not inferred:

**The map.** Cells of 4x4 tiles at `MAP + cy*$100 + cx*4`. Plane A takes the
byte `& $FE` and looks up `TILES + val*16 + sub`; plane B the word `& $1FF` at
`TILES + val*32 + sub`; `sub = (row&3)<<3 | (col&3)<<1`; `TILES = $4ADE8`.
Shroud: values `>= $67` draw as 0 when `$FFC198` is set. `MAP` is learned at
runtime and validated before it is believed.

**The camera.** `$FFE3BE`/`$FFE3C0` in pixels, limits at `$FFE3D2`/`$FFE3D4`
and `$FFE3CE`/`$FFE3D0`. Horizontal scroll is the negated camera, which is why
world and screen differ by exactly it.

**The object list.** Head reached via `$FFF3AC`, walked by the emitter at
`$1088`. Per object: `(a2)` position pointer, `$6(a2)` palette/flip, `$7(a2)`
flags, `$8(a2)` piece list, `$E(a2)` next.

**Object to screen**, `$1124..$11A2`: bit 6 of the flags means the position is
already screen-space; otherwise swap the two words, `>> 3`, subtract the
camera, and if bit 2 is set add a `(dx, dy)` pair from the table at `$1164`.
Then bias both by `$80`.

**The piece records**, 12 bytes each after a leading count:

    +0  width      +2  height     +4  y offset
    +6  tile and size word        +8  attributes    +$A  x offset

**What the cartridge then does to them**, and what we would stop doing: cull at
`$11A4`/`$11AC`/`$11B4`/`$11C0`, charge the per-band counters at `$11DC`, test
them at `$11F8..$123A`, emit into the shadow at `$FFE428`, cap at 80, and DMA
the lot at `$6716`.

## The gap

Three things are not yet nailed down, and phase 0 exists to close them.

1. **Which list the emitter really walks.** We use `$FFF3AC` because `$6704`
   loads it, but our walk does not yield the cursor at its current position
   while `$123C` demonstrably emits it. An earlier probe read `a2` at entry,
   which still holds the *previous* value; `a0` is the parameter. This is
   task #33 and it blocks everything else.
2. **Blink.** `$10CE..$111E` decrements a counter in the object and toggles a
   flag as it goes. It cannot be replayed read-only, so we must either run it
   ourselves at the right point or read its result.
3. **Which objects are world and which are UI.** The HUD, the cursor and the
   console are all objects too. Drawing them in world space would be wrong.

## Architecture

Three layers, drawn in order, all from game state:

    ground    mapview: planes A and B from the map, for the whole view
    objects   every object in the list, at any position, no table limit
    interface the HUD, cursor and console -- screen space, anchored, not scaled

`render.c` keeps its VDP for everything that is not gameplay: menus, the
mentat, cutscenes, the publisher logos. Those are 320-wide compositions the
cartridge builds in VRAM and there is nothing to gain by reinterpreting them.

## Phases

Each phase ships behind a flag, is verified against the VDP path at 320, and
leaves the tree green. No phase depends on a later one being finished.

### Phase 0 — close the object model  *(small; unblocks the rest)*

Confirm the list head with `DB4A_REGS` reading `a0`, and confirm `$1088` has no
other caller. Then log every object our walk visits against every entry the
cartridge emits, and account for the difference.

*Done when:* our walk reproduces the cartridge's chain length and every entry's
position on a full recording, and task #33's cursor is explained.

### Phase 1 — predict the sprite table  *(medium)*

Write `objects.c` that produces the entries the cartridge *would* have emitted,
including its culls and band budget, and compare against `$FFE428` every frame.
Nothing is drawn yet.

*Done when:* `make check-objects` reports zero mismatched entries across
`wide`, `artifacts`, `cursor` and `level1atredis` — the same bar `check-map`
holds for the ground layer.

### Phase 2 — draw objects in the margin  *(medium)*

Use the predictor, minus the culls, to draw objects outside the cartridge's
320x224 directly into the framebuffer. Delete the appender, the `$11B4`
override and the `$11DC` override.

*Done when:* the margin shows units at any size with no sprite-table cap; the
cartridge's own 320x224 is byte-identical to the VDP path; #33 and #31 close.

### Phase 3 — draw objects everywhere in gameplay  *(large)*

Draw the object layer for the whole view, VDP sprites off during gameplay.

*Done when:* at 320x224 the frame is byte-identical to the VDP path across
every recording. This is the phase that either works or does not, and the
comparison is unforgiving by design.

### Phase 4 — retire the tilemap fill  *(small)*

With ground and objects both ours, `widescreen.c`'s column fill has no reader.
Delete it, with `check-map` and `check-margins` as the guard.

*Done when:* the fill is gone and every check still passes.

### Phase 5 — the interface layer  *(medium, and a design decision)*

Only once the rest is stable. The HUD is currently sprites anchored by shifting
the whole picture, which is why the console jump (#28) and the offset rules
exist. Drawing it ourselves in screen space would remove that whole category —
but it changes what the game looks like, so it needs a setting of its own and
a deliberate decision rather than a slide.

## Risks, and how each is contained

**We reproduce the emitter imperfectly.** Contained by phase 1: predict before
drawing, and compare against what the cartridge actually emitted. A predictor
that cannot match the shadow has no business painting pixels.

**Timing changes.** Reading state cannot change it — but writing can, and the
overrides we would delete are exactly the writes that caused divergence. This
should *reduce* the difference between a widescreen run and a 320 one, and
`check-mission` measures it.

**Sprite-to-sprite priority.** The hardware resolves by link order; we would
resolve by list order. These are the same order only if the emitter emits in
list order, which phase 1 will show.

**Scope.** Phase 3 is the large one. Phases 0-2 are worth doing on their own —
they close three open tasks and remove three overrides — and phase 3 can be
declined without wasting them.

## What this buys

- The cursor and every unit appear wherever they are, at any resolution, with
  no 80-entry table and no per-band budget.
- Three overrides and one fill deleted, and with them the last writes that make
  a widescreen run diverge from a 320 one.
- Tasks #31, #33 and #34 stop being individual bugs and become consequences of
  a design we no longer use.
- Resolution stops being a thing we patch around and becomes a parameter.
