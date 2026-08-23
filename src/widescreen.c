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

/* Floor division by 2. The map pointer advances one long per PAIR of columns,
   so a column west of the origin needs the floor, not C's truncation -- which
   rounds toward zero and would fetch the wrong map cell for odd negatives. */
static int half(int v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); }

/* One column, drawn exactly as $7504 / $7468 draw theirs: 32 entries downward
   from start_row with wrap, stepping the table index by 8 and advancing the
   map pointer one row ($100) every fourth entry. */
static void draw_column(uint32_t nt_base, unsigned col, uint32_t a3,
                        unsigned d0, unsigned start_row, int plane_b) {
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
        VDP.vram[na & 0xFFFF]       = (uint8_t)(ent >> 8);
        VDP.vram[(na + 1) & 0xFFFF] = (uint8_t)ent;
        d0 += 8;
        if (d0 >= 0x20u) { d0 -= 0x20u; a3 += 0x100u; }
    }
}

void widescreen_note_column(uint32_t pc) {
    if (!enabled() || !render_widescreen_gameplay()) return;

    int plane_b = (pc == WS_COL_PLANE_B);
    uint32_t nt_base = plane_b ? (uint32_t)(VDP.reg[4] & 0x07) << 13
                               : (uint32_t)(VDP.reg[2] & 0x38) << 10;

    /* What the cartridge is about to draw. d1's low word is the nametable
       address of the column's first entry, a3 the map pointer, and d0 the
       table phase -- which is (col * 2) mod 32, verified against the running
       game across a full scroll. */
    uint32_t off = (CPU.d[1] & 0xFFFFu) - nt_base;
    int      col = (int)((off & 0x7Eu) >> 1);
    unsigned row = (off >> 7) & 31u;
    uint32_t a3  = CPU.a[3];

    /* Which columns to draw is decided by the CAMERA, not by the column the
       cartridge happens to be drawing. Scrolling east it draws at the east
       edge of the view, and columns counted west from there land inside the
       visible picture -- we would be redrawing what the game just drew, and
       any error in our arithmetic would corrupt the real view rather than the
       extension. The extension is always west of screen x=0, so that is where
       to aim. */
    uint32_t hb = (uint32_t)(VDP.reg[13] & 0x3F) << 10;
    int hs = (int)(int16_t)((VDP.vram[hb & 0xFFFF] << 8) | VDP.vram[(hb + 1) & 0xFFFF]);
    int leftcol = (((-hs) % 512 + 512) % 512) / 8;   /* tile column at screen x=0 */

    /* leftcol and col are both indices into the 64-column ring, and the map
       pointer tracks the WORLD column, not the ring one. Subtracting them
       directly is right only while they sit in the same lap: scrolling east
       the cartridge draws at the far edge, the two are ~40 apart, and the
       difference that matters can be the short way round the ring rather than
       the long way. Reducing it to [-32, 32) picks the lap that is actually
       adjacent -- without this, east-scrolling extrapolates the map pointer 64
       columns wrong and the extension fills with the wrong part of the map. */
    int diff = (((leftcol - col) + 32) & 63) - 32;

    /* Only extend when the cartridge is drawing at the WEST edge, which is
       what it does while scrolling west -- the case that needs us.
       .
       Scrolling east it draws at the far edge instead, ~40 columns away, and
       the extension is BEHIND the camera: the game drew those columns itself a
       moment ago and they are already correct. Extrapolating the map pointer
       that far to redraw them measurably made things worse (mean visible
       pixels 2521 -> 131), because the pointer relation we derived holds
       locally and not across the whole ring. So do nothing there. Drawing
       ahead is only ever needed in the direction of travel. */
    if (diff < -4 || diff > 4) return;
    int origin = col + diff;                   /* leftcol, in the same lap as col */

    /* One tile column per 8 pixels of extension, plus a margin so a column
       only partly scrolled in at the edge is covered too. */
    int need = (fb_width - 320 + 7) / 8 + 2;

    for (int k = 1; k <= need; k++) {
        int w = origin - k;
        unsigned c  = (unsigned)(w & 63);
        unsigned d0 = (unsigned)(((w * 2) % 32 + 32) % 32);
        /* The map pointer advances one long per PAIR of columns. */
        uint32_t ax = a3 + (uint32_t)((half(w) - half(col)) * 4);
        draw_column(nt_base, c, ax, d0, row, plane_b);
    }
}
