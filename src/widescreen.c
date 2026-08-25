/* Drawing the map columns the cartridge does not draw. See widescreen.h. */
#include "widescreen.h"
#include "render.h"
#include "vdp.h"
#include "m68k.h"
#include <stdlib.h>
#include <stdio.h>

/* The cartridge's tile table. Both column routines do `lea $4ade8.l, a4` and
   index it with the map value; plane A uses the high byte times 16, plane B
   the low 9 bits times 32. Same table, two encodings. */
#define TILE_TABLE 0x0004ADE8u

/* Shroud test lifted from $7546: with this flag set, map values from $67 up
   are drawn as 0. Getting it wrong would draw terrain through the fog. */
#define SHROUD_FLAG 0x00FFC198u

static int enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DB4A_WIDE_FILL");
        on = (e && !atoi(e)) ? 0 : 1;
    }
    return on;
}

/* Floor division by 4.
 *
 * A map cell is 4x4 tiles and one long wide, so the map pointer advances 4
 * bytes every FOUR tile columns -- verified directly: columns 0-3 share a
 * pointer, 4-7 the next, and so on, and a column one west of the ring origin
 * takes the pointer 4 bytes back. The d0 table phase agrees, its low bits
 * cycling through 0,2,4,6 across those same four columns.
 *
 * Floor, not C's truncation: truncation rounds toward zero, so a column west
 * of the origin would fetch the cell to its east. */
static int quarter(int v) { return v >= 0 ? v / 4 : -((-v + 3) / 4); }

/* One column, drawn exactly as $7504 / $7468 draw theirs: 32 entries downward
   from start_row with wrap, stepping the table index by 8 and advancing the
   map pointer one row ($100) every fourth entry. */
static void draw_column_to(uint32_t nt_base, unsigned col, uint32_t a3,
                           unsigned d0, unsigned start_row, int plane_b,
                           uint16_t *out, uint32_t *outa) {
    int shroud = m68k_read8(SHROUD_FLAG) != 0;
    for (unsigned r = 0; r < 32; r++) {
        unsigned idx;
        if (plane_b) {
            idx = ((m68k_read16(a3) & 0x1FFu) << 5) + d0;
        } else {
            unsigned v = m68k_read8(a3) & 0xFEu;
            if (shroud && v >= 0x67u) v = 0;
            idx = (v << 4) + d0;
        }
        uint16_t ent = m68k_read16(TILE_TABLE + idx);
        uint32_t na = nt_base + (((start_row + r) & 31u) * 64u + col) * 2u;
        if (out) { out[r] = ent; outa[r] = na; }
        else {
            VDP.vram[na & 0xFFFF]       = (uint8_t)(ent >> 8);
            VDP.vram[(na + 1) & 0xFFFF] = (uint8_t)ent;
        }
        d0 += 8;
        if (d0 >= 0x20u) { d0 -= 0x20u; a3 += 0x100u; }
    }
}

static void draw_column(uint32_t nt_base, unsigned col, uint32_t a3,
                        unsigned d0, unsigned start_row, int plane_b) {
    draw_column_to(nt_base, col, a3, d0, start_row, plane_b, NULL, NULL);
}

/* check-native for this routine.
 *
 * DB4A_WIDE_CHECK=1 predicts the column the cartridge is ABOUT to draw, two
 * ways, and compares both against what the cartridge actually wrote -- read
 * back on the next call, by which time it has written them.
 *
 *   loop:    our reproduction of the drawing loop, given the cartridge's own
 *            a3/d0/row. Tests the loop alone.
 *   derive:  the same column reached through our anchor arithmetic. Tests the
 *            map-pointer derivation, which is where the stride bug lived.
 *
 * Any comparison against a LATER frame confounds this with vertical scroll and
 * with units that moved. This has neither: same instant, same column. */
static struct { int live; uint32_t a[32]; uint16_t loop[32], der[32]; } pend;
static unsigned long chk_calls, chk_loop_bad, chk_der_bad;

static int checking(void) {
    static int c = -1;
    if (c < 0) c = getenv("DB4A_WIDE_CHECK") ? 1 : 0;
    return c;
}

void widescreen_check_report(void) {
    if (!checking()) return;
    fprintf(stderr, "[wsck] columns checked %lu   loop wrong %lu   derive wrong %lu\n",
            chk_calls, chk_loop_bad, chk_der_bad);
}

/* What we learned from watching the cartridge draw a column.
 *
 * The cartridge only draws columns while scrolling horizontally, so hooking
 * its routine alone leaves the extension blank whenever it is not -- at the
 * start of a mission, most obviously, where the camera has never moved and the
 * whole strip stays black. So the observation is REMEMBERED and the fill runs
 * every frame from the camera position.
 *
 * M is the map pointer for world column 0. World columns are unwrapped: the
 * nametable is a 64-column ring, but the map pointer tracks the world, so
 * anchoring to the ring index alone loses a lap every 512 pixels. */
static struct {
    int      valid;
    uint32_t M;        /* map pointer at world column 0 */
    int      wc;       /* world column that hs_at corresponded to */
    int      hs_at;    /* hscroll when captured */
    unsigned row;      /* nametable row the column starts on */
    unsigned vsub;     /* vertical half of the table phase, d0 & $18 */
    uint16_t vs;       /* vertical scroll then -- M is only valid for this */
} anchor[2];

/* Tile column at screen x=0, unwrapped (negative), for hscroll hs.
 *
 * The leftmost visible plane pixel is -hs, so the tile column is floor(-hs/8).
 * C's division truncates toward zero, which for a negative numerator rounds
 * the WRONG WAY and lands one column east -- putting the first "margin"
 * column exactly on the leftmost visible one and overwriting what the
 * cartridge drew. Hence the explicit ceiling on the positive value. */
static int cam_col(int hs) {
    int m = ((hs % 512) + 512) % 512;
    return -((m + 7) / 8);
}

static void fill(int plane_b, uint32_t nt_base, int hs) {
    typeof(anchor[0]) *a = &anchor[plane_b];
    if (!a->valid) return;
    /* M is tied to a vertical position. The cartridge redraws whole rows when
       it scrolls vertically, so rather than extrapolate into a layout we no
       longer understand, stop filling until a fresh column-draw re-anchors
       us. Blank is recoverable; wrong is not. */
    if (VDP.vsram[0] != a->vs) { a->valid = 0; return; }

    int now = cam_col(hs);
    int step = now - a->wc;
    /* hscroll is 10 bits and wraps; a jump that large is the wrap, not a
       camera that teleported. Re-anchor rather than guess. */
    if (step < -32 || step > 32) { a->valid = 0; return; }

    /* Only draw when the camera is moving WEST (or holding still). The
       extension lies west of the view, so going east it is behind the camera
       and the cartridge drew it itself moments ago -- correct already, and
       better than anything we would extrapolate. Overwriting it measurably
       made things worse (2521 -> 131 mean visible pixels): the map-pointer
       relation is reliable near the column we were anchored on and not forty
       columns away from it. */
    if (step > 0) { a->wc = now; a->hs_at = hs; return; }

    int origin = a->wc + step;
    int need = (fb_width - 320 + 7) / 8 + 2;
    /* The nametable is a 64-column RING, and the cartridge's own view already
     * holds 41 of those columns. Asking for more than the remaining 23 wraps
     * round and overwrites what the cartridge drew -- silently, because the
     * write goes to VRAM and looks like any other column.
     *
     * Measured with DB4A_MAPCHECK over data/recordings/wide.txt, comparing the
     * cartridge's OWN 320x224 against the map it should hold:
     *
     *     488 wide   0.03%   (the figure at every size up to here)
     *     496 wide   0.06%
     *     504 wide   0.49%
     *     512 wide   1.12%
     *     640 wide  11.72%
     *     800 wide  28.20%
     *
     * so this was already corrupting the picture at the old 512 cap. With the
     * clamp it is 0.03% at every size up to 1024x1024.
     *
     * 41, not 40. The cartridge's 320 pixels are 40 tiles only when hscroll
     * lands exactly on a tile boundary; otherwise they straddle 41 columns,
     * and cam_col() deliberately rounds WEST to the first of them. Bounding
     * against 40 leaves that straddling column unprotected -- measured, that
     * held the figure at 0.06% instead of 0.03% at every width past 496.
     *
     * Clamping costs nothing. The margin has been drawn from the game's map
     * rather than from the tilemap since mapview landed, and rendering
     * wide.txt with this fill disabled entirely gives byte-identical frames;
     * what it still covers is the window before mapview has learned the map
     * base, and 23 columns is as much of that as the ring can hold. */
    if (need > 64 - 41) need = 64 - 41;
    for (int k = 1; k <= need; k++) {
        int w = origin - k;
        unsigned c  = (unsigned)(w & 63);
        /* d0 is TWO fields: the low bits step through the four tile columns of
           a map cell, the high bits pick the vertical quarter. Treating it as
           one number (col * 2 mod 32) happens to be right only while the
           vertical position is unchanged -- it was wrong on 1103 of 2127
           columns, which is what put wrong map in the extension. */
        unsigned d0 = a->vsub | (unsigned)((w * 2) & 6);
        draw_column(nt_base, c, a->M + (uint32_t)(quarter(w) * 4), d0, a->row, plane_b);
    }
    a->wc = now; a->hs_at = hs;
}

static int hscroll_now(void) {
    uint32_t hb = (uint32_t)(VDP.reg[13] & 0x3F) << 10;
    return (int)(int16_t)((VDP.vram[hb & 0xFFFF] << 8) | VDP.vram[(hb + 1) & 0xFFFF]);
}

void widescreen_note_column(uint32_t pc) {
    if (!enabled() || !render_widescreen_gameplay()) return;

    int plane_b = (pc == WS_COL_PLANE_B);
    uint32_t nt_base = plane_b ? (uint32_t)(VDP.reg[4] & 0x07) << 13
                               : (uint32_t)(VDP.reg[2] & 0x38) << 10;

    /* What the cartridge is about to draw. d1's low word is the nametable
       address of the column's first entry, a3 the map pointer, and d0 the
       table phase -- (col * 2) mod 32, verified against the running game. */
    uint32_t off = (CPU.d[1] & 0xFFFFu) - nt_base;
    int      col = (int)((off & 0x7Eu) >> 1);
    unsigned row = (off >> 7) & 31u;
    uint32_t a3  = CPU.a[3];
    int      hs  = hscroll_now();

    /* Put the drawn column in the same lap as the camera. Both are ring
       indices; the short way round is the one that is actually adjacent. */
    int cam = cam_col(hs);
    int w_g = cam + ((((col - cam) + 32) & 63) - 32);

    /* Anchor only on columns drawn near the WEST edge, which is what the
       cartridge draws while scrolling west. Anchoring on an east-edge draw
       would put M forty columns from where we use it. */
    if (w_g - cam < -4 || w_g - cam > 4) return;

    if (checking()) {
        /* settle the previous prediction now that the cartridge has written it */
        if (pend.live) {
            int lb = 0, db = 0;
            for (int i = 0; i < 32; i++) {
                uint16_t got = (uint16_t)((VDP.vram[pend.a[i] & 0xFFFF] << 8)
                                          | VDP.vram[(pend.a[i] + 1) & 0xFFFF]);
                if (got != pend.loop[i]) lb = 1;
                if (got != pend.der[i])  db = 1;
            }
            chk_calls++; chk_loop_bad += lb; chk_der_bad += db;
        }
        /* (a) the loop, given the cartridge's own parameters */
        draw_column_to(nt_base, (unsigned)col, a3, (unsigned)((CPU.d[0]) & 0x1Fu),
                       row, plane_b, pend.loop, pend.a);
        /* (b) the same column via our anchor arithmetic */
        uint32_t M = a3 - (uint32_t)(quarter(w_g) * 4);
        uint32_t da[32];
        draw_column_to(nt_base, (unsigned)(w_g & 63),
                       M + (uint32_t)(quarter(w_g) * 4),
                       (unsigned)((CPU.d[0] & 0x18u) | (unsigned)((w_g * 2) & 6)),
                       row, plane_b,
                       pend.der, da);
        pend.live = 1;
    }

    anchor[plane_b].M     = a3 - (uint32_t)(quarter(w_g) * 4);
    anchor[plane_b].wc    = cam;
    anchor[plane_b].hs_at = hs;
    anchor[plane_b].row   = row;
    anchor[plane_b].vsub  = CPU.d[0] & 0x18u;
    anchor[plane_b].vs    = VDP.vsram[0];
    anchor[plane_b].valid = 1;
}

/* Called once per frame, before the frame is drawn. */
void widescreen_extend(void) {
    if (!enabled() || !render_widescreen_gameplay()) return;
    int hs = hscroll_now();
    fill(0, (uint32_t)(VDP.reg[2] & 0x38) << 10, hs);
    fill(1, (uint32_t)(VDP.reg[4] & 0x07) << 13, hs);
}

/* ---------------------------------------------------------------------------
 * Units in the extension.
 *
 * Terrain and buildings live in the tilemap, so the column fill above brings
 * them back. Units do not: they are sprites, and the cartridge culls them to
 * the 320-pixel screen before they ever reach the sprite table. Extending the
 * tilemap therefore gives an extension with correct ground and no units on it.
 *
 * The cull is at $11A4..$11C4, right after $1184 biases the coordinates by $80
 * (the VDP's sprite origin, where $80 is screen x=0):
 *
 *     cmpi.w #$1bf,d2   past the right edge  (128 + 319)   -> drop
 *     cmpi.w #$15f,d3   past the bottom      (128 + 223)   -> drop
 *     add.w  d2,d0
 *     cmpi.w #$80,d0    right edge of the sprite still left of screen -> drop
 *
 * The third is the one that matters. Our extension is west of the view, so a
 * unit standing in it has a sprite whose right edge is left of screen x=0, and
 * that test drops it.
 *
 * $11B4 is its own block entry -- the three instructions above and nothing
 * else -- so it can be replaced outright rather than patched. Widening the
 * threshold by the extension width keeps exactly the units that fall in the
 * extension and nothing further out.
 *
 * The RIGHT cull needs no change: the extension is on the left, so the
 * cartridge's own 320 columns still end where they always did.
 *
 * With widescreen off the threshold is $80 and this is the cartridge's block
 * instruction for instruction -- which is why it is registered as faithful and
 * `make check-native` verifies it.
 *
 * WHAT THIS COSTS, and it is not small. Unlike the column fill, this is NOT
 * presentation-only. Keeping a sprite the cartridge would have dropped means
 * taking the other branch, emitting another entry, bumping the sprite index
 * and the per-column counters at $F700, and executing a different number of
 * blocks -- so the cycle count moves, and with it the frame pacing. A
 * widescreen run therefore DIVERGES from a 320 run: replaying
 * level1atredis.txt at WIDE=400 differs from the same replay at 320 across
 * 50-73% of the picture by frame 6000. The game is healthy, it is simply a
 * different battle.
 *
 * That breaks the property the column fill was careful to keep -- "the
 * extension only ever adds" -- and it means a recording made at one width does
 * not reproduce at another. It is a deliberate departure, which is what the
 * remaster line is for, but it belongs behind its own switch rather than
 * bundled in silently.
 *
 * The honest alternative, if the divergence ever matters more than the units
 * do, is to leave the cartridge's cull alone and draw the missing units
 * ourselves from the unit table in RAM. That needs the unit format and their
 * sprite tiles worked out, and is a much larger job than this three-line
 * block.
 * ------------------------------------------------------------------------- */
uint32_t native_sprite_left_cull(void) {
    CPU.cycles += 26;                     /* same as blk_0011B4 */
    uint16_t r1 = add16((uint16_t)CPU.d[0], (uint16_t)CPU.d[2]);
    CPU.d[0] = (CPU.d[0] & 0xFFFF0000u) | ((r1) & 0xFFFFu);

    /* DB4A_WIDE_UNITS=0 keeps the cartridge's own threshold, which leaves the
       extension's ground correct and its units missing -- but keeps a
       widescreen run bit-for-bit identical to a 320 one. See the note below. */
    /* OFF by default now. The appender below does the same job without
       perturbing the cartridge -- 2.1 sprites a frame against this path's 2.3,
       so essentially the same population -- and without the divergence this
       one costs. DB4A_WIDE_UNITS=1 restores it for comparison. */
    static int units = -1;
    if (units < 0) { const char *e = getenv("DB4A_WIDE_UNITS");
                     units = (e && atoi(e)) ? 1 : 0; }
    int extra = (units && enabled() && render_widescreen_gameplay())
                ? fb_width - 320 : 0;
    cmp16((uint16_t)CPU.d[0], (uint16_t)(0x80u - extra));

    if (cond_lt()) return 0x1284u;        /* dropped */
    return 0x11BEu;                       /* kept */
}

/* ---------------------------------------------------------------------------
 * Keeping extension sprites out of the sprite BUDGET.
 *
 * Widening the left cull let units in the extension reach the emitter, and
 * that had a side effect well beyond the strip: enemies showed through the fog
 * of war on entering a mission. Confirmed by the user against DB4A_WIDE_UNITS=0,
 * which made it go away.
 *
 * The reason is $11DC, which every surviving sprite passes through before the
 * visibility tests that follow it:
 *
 *     0011DC  addq.b #$1, $1(a5, d1.w)     one more sprite in this band
 *     0011E0  move.w (a4), d0
 *     0011E2  lsr.w  #$3, d0
 *     0011E4  add.b  d0, $11(a5, d1.w)     and this much more width
 *
 * a5 is scratch at $F700, cleared each pass, and d1 = (Y - $80) >> 5 buckets
 * the screen into eight bands. Those two counters are then compared at
 * $11F8..$123A against per-band limits to decide whether a sprite is emitted
 * at all. So an extension sprite does not merely add itself -- it spends band
 * budget, and changes the verdict for sprites elsewhere on the screen,
 * including ones the cartridge was suppressing.
 *
 * The fix is to let extension sprites be drawn without being counted. They are
 * ours, outside the screen the cartridge is budgeting for, so they should not
 * be charged against it.
 *
 * Identifying them: d0 still holds width + X from $11B4, the sum the original
 * cull compared against $80. Below $80 means the sprite lies entirely west of
 * the cartridge's screen -- exactly the ones our widened threshold let in.
 *
 * The arithmetic is still performed so the flags are what the cartridge would
 * have left; only the two stores are skipped. And with widescreen off nothing
 * reaches here with d0 < $80, because the unwidened cull already dropped it --
 * so this is the cartridge's block exactly, and check-native verifies it.
 * ------------------------------------------------------------------------- */
unsigned long ws_band_calls, ws_band_ext;

uint32_t native_sprite_band_count(void) {
    CPU.cycles += 80;                     /* same as blk_0011DC */

    int ext = (int16_t)(uint16_t)CPU.d[0] < 0x80;
    { extern unsigned long ws_band_calls, ws_band_ext;
      ws_band_calls++; if (ext) ws_band_ext++; }

    { /* 0011DC  addq.b #$1, $1(a5, d1.w) */
        uint32_t a1 = CPU.a[5] + sx16((uint16_t)CPU.d[1]) + 1;
        uint8_t r2 = add8(m68k_read8(a1), 0x1u);
        if (!ext) m68k_write8(a1, r2);
    }
    { /* 0011E0  move.w (a4), d0 */
        uint16_t m3 = m68k_read16(CPU.a[4]);
        flags_logic16(m3);
        CPU.d[0] = (CPU.d[0] & 0xFFFF0000u) | ((m3) & 0xFFFFu);
    }
    { /* 0011E2  lsr.w #$3, d0 */
        uint16_t r1 = lsr16((uint16_t)CPU.d[0], 0x3u);
        CPU.d[0] = (CPU.d[0] & 0xFFFF0000u) | ((r1) & 0xFFFFu);
    }
    { /* 0011E4  add.b d0, $11(a5, d1.w) */
        uint32_t a1 = CPU.a[5] + sx16((uint16_t)CPU.d[1]) + 17;
        uint8_t r2 = add8(m68k_read8(a1), (uint8_t)CPU.d[0]);
        if (!ext) m68k_write8(a1, r2);
    }
    { /* 0011E8  btst.b #$1, $7(a2) */
        uint8_t b2 = m68k_read8(CPU.a[2] + 7);
        CPU.z = ((b2 >> 0x1u) & 1) ^ 1;
    }
    { /* 0011EE  beq.b $123c */
        if (cond_eq()) return 0x123Cu;
        return 0x11F0u;
    }
}

/* ---------------------------------------------------------------------------
 * Appending the strip's units to the sprite list, without touching the
 * cartridge's own run.
 *
 * Widening the cull at $11B4 works, but it is not free: keeping a sprite the
 * cartridge would have dropped means taking the other branch, emitting another
 * entry, bumping the sprite index, and running a different number of blocks --
 * so cycles move and a widescreen run drifts away from a 320 one.
 *
 * The alternative is to leave the cartridge's emitter completely alone and add
 * our sprites AFTERWARDS. The list is a shadow at $FFE428, eight bytes an
 * entry, DMA'd whole by $6716; the chain is a link byte per entry, and $1294
 * ends it by clearing the link of the last one. So appending is: walk the
 * chain to the end, write our entries after it, point the old last entry at
 * the first of ours, and clear the link on ours.
 *
 * Nothing about the cartridge's execution changes -- same branches, same cycle
 * count, same RAM except the tail of a table it has finished with.
 *
 * The objects come from the same list the emitter walks: head at $FFF3AC, next
 * at $E(a2), and the object-to-screen conversion at $1124..$11A2 reproduced
 * below. Read-only: the blink counters at $4(a2)/$5(a2) are deliberately NOT
 * ticked, because the cartridge has already ticked them this frame and doing
 * it again would be exactly the kind of state perturbation this exists to
 * avoid.
 * ------------------------------------------------------------------------- */
#define OBJ_HEAD   0x00FFF3ACu
#define SAT_SHADOW 0x00FFE428u
#define SAT_MAX    0x50            /* the cartridge's own cap, $127C */
#define CAM_X_ADDR 0x00FFE3BEu
#define CAM_Y_ADDR 0x00FFE3C0u
#define ADJ_TABLE  0x00001164u     /* the eight (dx,dy) pairs at $1164 */

unsigned long ws_appended, ws_append_frames;
unsigned long ws_calls, ws_objs, ws_pieces, ws_rej_full, ws_rej_kept, ws_rej_far, ws_rej_yx;
/* How often the cartridge's 80-entry sprite table runs out while we are
   appending. This is the number that decides whether a much larger view is
   usable, so it is counted rather than argued about -- see DB4A_LOG_WIDE.
   Measured over a whole mission at 1024x1024: never. */
unsigned long ws_cap_pieces, ws_cap_frames;

static int sat_chain_end(unsigned *count) {
    /* Walk the link chain from entry 0, exactly as the VDP would. */
    unsigned idx = 0, n = 0, last = 0;
    for (; n < SAT_MAX; n++) {
        uint32_t e = SAT_SHADOW + idx * 8u;
        unsigned link = m68k_read8(e + 3) & 0x7Fu;
        last = idx;
        if (!link) break;
        idx = link;
    }
    *count = n + 1;
    return (int)last;
}

static int appending(void) {
    static int a = -1;
    if (a < 0) { const char *e = getenv("DB4A_WIDE_APPEND");
                 a = (e && !atoi(e)) ? 0 : 1; }
    return a;
}

void widescreen_append_sprites(void) {
    if (!appending() || !enabled() || !render_widescreen_gameplay()) return;
    int ext  = render_world_offset();          /* strip to the west  */
    int exth = fb_height - 224;                /* strip to the south */
    if (ext <= 0 && exth <= 0) return;

    ws_calls++;
    unsigned count = 0;
    int last = sat_chain_end(&count);
    if (count >= SAT_MAX) { ws_rej_full++; return; }

    int camx = (int)(int16_t)m68k_read16(CAM_X_ADDR);
    int camy = (int)(int16_t)m68k_read16(CAM_Y_ADDR);
    unsigned slot = count;
    unsigned added = 0;
    int capped = 0;

    uint32_t obj = 0xFFFF0000u | m68k_read16(OBJ_HEAD);
    for (int guard = 0; guard < 512 && (obj & 0xFFFFu); guard++) {
        ws_objs++;
        unsigned f7 = m68k_read8(obj + 7);
        /* Only bit 7 drops the object outright.
           .
           $10C2 branches to $111E, and $111E is `tst.w d6; bne $128C` -- it
           skips only when d6 is NON-zero. So bit 7 set leaves d6 = $80 and
           skips, while bit 5 CLEAR leaves d6 = 0 and falls straight through to
           be drawn. Reading `beq $111e` as "skip" inverts it, and that is
           exactly what it did: seven objects in eight discarded, four pieces
           a frame reaching the culls instead of twenty-odd. */
        if (f7 & 0x80u) goto next;                      /* $10BA */
        /* The cartridge's blink logic at $10CE..$111E cannot be reproduced
           here: it DECREMENTS the counter at $4(a2) and toggles bit 4 of the
           flags as it goes, and it has already run this frame. Ticking it a
           second time is exactly the state perturbation this whole approach
           exists to avoid, and reading the post-tick state does not tell us
           which way it went. So blink is ignored: a unit that would have been
           mid-blink is drawn steadily in the strip. That is a cosmetic
           difference confined to the strip, and the alternative -- guessing --
           was skipping seven objects in every eight. */

        {
        uint32_t pos = m68k_read32(obj);
        int d4 = (int16_t)m68k_read16(pos);             /* word 0 */
        int d5 = (int16_t)m68k_read16(pos + 2);         /* word 1 */
        if (!(f7 & 0x40u)) {                            /* btst #6 clear */
            int t = d4; d4 = d5; d5 = t;                /* exg.l d4,d5 */
            d4 = (int16_t)(((uint16_t)d4) >> 3);
            d5 = (int16_t)(((uint16_t)d5) >> 3);
            d4 -= camx; d5 -= camy;
            if (f7 & 0x04u) {                           /* btst #2 set */
                /* $1148 does `lea -$c(a0), a0`, but a0 is already pos+2 -- the
               post-increment at $1126 moved it -- so the index byte is at
               pos + 2 - 12 + $72 = pos + $68. Reading pos + $66 instead picks
               up the neighbouring byte, which differs often enough to shift
               one sprite by one pixel and no more: 1.7% of entries wrong, all
               of them x off by exactly 1. */
            unsigned k = m68k_read8(pos + 0x68u) & 7u;
                d4 += (int16_t)m68k_read16(ADJ_TABLE + k * 4u);
                d5 += (int16_t)m68k_read16(ADJ_TABLE + k * 4u + 2u);
            }
        }
        d4 += 0x80; d5 += 0x80;

        uint32_t a4 = m68k_read32(obj + 8);
        int pieces = (int16_t)m68k_read16(a4);
        a4 += 2;
        for (int i = 0; i <= pieces; i++, a4 += 0xC) {
            /* Same stopping point as the `slot < SAT_MAX` this replaces --
               nothing more is written either way -- but it counts what was
               dropped instead of leaving the ceiling invisible. */
            if (slot >= SAT_MAX) { ws_cap_pieces++; capped = 1; continue; }
            int y = d5 + (int16_t)m68k_read16(a4 + 4);
            int hh = (int16_t)m68k_read16(a4 + 2);
            int x = d4 + (int16_t)m68k_read16(a4 + 0xA);
            int ww_ = (int16_t)m68k_read16(a4);

            ws_pieces++;
            { static unsigned long call_seen = 0; static int done = 0;
              if (getenv("DB4A_WIDE_DBG")) {
                  if (call_seen != ws_calls) { call_seen = ws_calls; done = (done ? 2 : 1); }
                  if (done == 1) fprintf(stderr, "[dbg] %d,%d\n", x, y);
              } }
            /* The cartridge's own bounds, widened by whatever strip we have
               asked for on that side. East and north are untouched: the view
               grows west and south only. */
            if (x > 0x1BF || y > 0x15F + exth) { ws_rej_yx++; continue; }
            if (y + hh < 0x80) { ws_rej_yx++; continue; }
            /* The one the cartridge uses to drop it, and the reason it is
               missing: the whole sprite lies west of the cartridge's screen.
               Ours are exactly those, and only as far west as the strip. */
            /* Keep only what the cartridge DROPPED, and only as far out as
               our strips reach. A piece it kept is already in the list. */
            int off_west  = (x + ww_ <  0x80);
            int off_south = (y       >  0x15F);
            if (!off_west && !off_south) { ws_rej_kept++; continue; }
            if (off_west  && x + ww_ < 0x80 - ext)  { ws_rej_far++; continue; }
            if (off_south && y > 0x15F + exth)      { ws_rej_far++; continue; }

            uint32_t e = SAT_SHADOW + slot * 8u;
            m68k_write16(e, (uint16_t)y);
            m68k_write16(e + 2, (uint16_t)(m68k_read16(a4 + 6) + slot));
            {
                uint16_t at = m68k_read16(a4 + 8);
                uint16_t v  = (uint16_t)(at & 0x9FFFu);
                v ^= (uint16_t)(m68k_read16(obj + 6) & 0xF800u);
                if (at & 0x6000u) { v = (uint16_t)(v & 0x9FFFu); v |= at; }
                else              { v |= (uint16_t)(at & 0x6000u); }
                m68k_write16(e + 4, v);
            }
            m68k_write16(e + 6, (uint16_t)x);
            slot++; added++;
        }
        }
    next:
        obj = 0xFFFF0000u | m68k_read16(obj + 0xE);
    }

    if (capped) ws_cap_frames++;
    if (!added) return;
    /* Splice: the cartridge's last entry now points at the first of ours, and
       ours ends the chain the same way $1294 does. */
    m68k_write8(SAT_SHADOW + (uint32_t)last * 8u + 3, (uint8_t)count);
    m68k_write8(SAT_SHADOW + (uint32_t)(slot - 1) * 8u + 3, 0);
    ws_appended += added; ws_append_frames++;
}
