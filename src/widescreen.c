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
