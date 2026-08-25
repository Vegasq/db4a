/* Drawing the map from the game's own map data. See include/mapview.h. */
#include "mapview.h"
#include "widescreen.h"
#include "render.h"
#include "vdp.h"
#include "m68k.h"
#include <stdlib.h>
#include <stdio.h>

#define TILES      0x0004ADE8u     /* the cell -> tile-entry table in ROM */
#define SHROUD     0x00FFC198u     /* set: map values >= $67 draw as tile 0 */
#define CAM_X      0x00FFE3BEu
#define CAM_Y      0x00FFE3C0u
#define CELL_ROW   0x100u          /* bytes per cell row  */
#define CELL_COL   4u              /* bytes per cell      */

static uint32_t map_base;          /* address of cell (0, 0) */

uint32_t mapview_base(void)  { return map_base; }
int      mapview_ready(void) { return map_base != 0; }

/* Learn where the map lives, from one column draw.
 *
 * The cartridge hands the routine a3, the map pointer for the column it is
 * about to draw, and d1, whose low word is that column's nametable address.
 * The nametable column is only known modulo 64, so it is resolved against the
 * camera -- the drawn column is always within half a lap of it. From there the
 * cell coordinate is exact and the base follows by subtraction.
 *
 * The vertical side needs one correction: the draw starts one tile row ABOVE
 * the camera top, which is the margin row the cartridge keeps. Measured, not
 * assumed -- without it the row is consistently one out. */
/* Does a candidate base actually reproduce what the cartridge has drawn?
 *
 * Cheaper and far more reliable than reasoning about which lap a column
 * belongs to: try the base, and compare a band of tiles across the middle of
 * the screen against the nametable. A base that is wrong by a lap is wrong by
 * 16 cells and disagrees almost everywhere, so this rejects it immediately. */
static int base_agrees(uint32_t cand) {
    uint32_t save = map_base;
    map_base = cand;
    int camx = (int)(int16_t)m68k_read16(CAM_X);
    int camy = (int)(int16_t)m68k_read16(CAM_Y);
    uint32_t na = (uint32_t)(VDP.reg[2] & 0x38) << 10;
    int good = 0, seen = 0;
    for (int ty = 8; ty < 20; ty++) {
        for (int tx = 4; tx < 36; tx += 2) {
            int col = (camx >> 3) + tx, row = (camy >> 3) + ty;
            uint32_t e = na + (uint32_t)(((row & 31) * 64 + (col & 63)) * 2);
            uint16_t got = (uint16_t)((VDP.vram[e & 0xFFFF] << 8)
                                      | VDP.vram[(e + 1) & 0xFFFF]);
            seen++;
            if (got == mapview_entry(0, col, row)) good++;
        }
    }
    map_base = save;
    return seen && good * 10 >= seen * 9;      /* 90%: sprites and edits move a few */
}

void mapview_observe(uint32_t pc) {
    if (map_base) return;
    int plane_b = (pc == WS_COL_PLANE_B);
    uint32_t nb = plane_b ? (uint32_t)(VDP.reg[4] & 0x07) << 13
                          : (uint32_t)(VDP.reg[2] & 0x38) << 10;
    uint32_t off = (CPU.d[1] & 0xFFFFu) - nb;
    int ntcol = (int)((off & 0x7Eu) >> 1);
    int camx  = (int)(int16_t)m68k_read16(CAM_X);
    int camy  = (int)(int16_t)m68k_read16(CAM_Y);
    int wrow  = (camy >> 3) - 1;               /* the margin row above the top */

    if ((int)((CPU.d[0] & 0x18u) >> 3) != (wrow & 3)) return;

    /* The nametable column is known only modulo 64, and the cartridge draws at
       whichever edge it is scrolling towards -- so the column can be a tile
       west of the camera or forty east of it. Rather than guess the lap, try
       every one that could be on screen and keep the one that reproduces what
       the cartridge drew. Guessing here is what put the base a lap out (0x40,
       exactly 16 cells) and every tile wrong after it. */
    int base = camx >> 3;
    for (int lap = -1; lap <= 1; lap++) {
        int wcol = base + ((ntcol - base) & 63) + lap * 64;
        if (wcol - base < -8 || wcol - base > 56) continue;
        if ((int)((CPU.d[0] & 6u) >> 1) != (wcol & 3)) continue;
        uint32_t cand = CPU.a[3] - (uint32_t)((wcol >> 2) * (int)CELL_COL)
                                 - (uint32_t)((wrow >> 2) * (int)CELL_ROW);
        if (base_agrees(cand)) { map_base = cand; return; }
    }
}

uint16_t mapview_entry(int plane_b, int col, int row) {
    if (!map_base) return 0;
    uint32_t cell = map_base + (uint32_t)((row >> 2) * (int)CELL_ROW)
                             + (uint32_t)((col >> 2) * (int)CELL_COL);
    unsigned sub = (unsigned)(((row & 3) << 3) | ((col & 3) << 1));
    unsigned idx;
    if (plane_b) {
        idx = ((m68k_read16(cell) & 0x1FFu) << 5) + sub;
    } else {
        unsigned v = m68k_read8(cell) & 0xFEu;
        if (m68k_read8(SHROUD) && v >= 0x67u) v = 0;   /* $7546 */
        idx = (v << 4) + sub;
    }
    return m68k_read16(TILES + idx);
}

/* One world pixel. The renderer works in screen space; the map is indexed in
   world space, and the two differ by exactly the camera -- the cartridge's
   horizontal scroll IS -camera, so no ring arithmetic is involved. */
unsigned mapview_pixel(int plane_b, int px, int py, int *prio) {
    uint16_t ent = mapview_entry(plane_b, px >> 3, py >> 3);
    *prio = (ent >> 15) & 1;
    unsigned tile = ent & 0x7FF;
    unsigned fx = (ent >> 11) & 1, fy = (ent >> 12) & 1;
    unsigned pal = (ent >> 13) & 3;
    unsigned ix = (unsigned)px & 7, iy = (unsigned)py & 7;
    if (fx) ix = 7 - ix;
    if (fy) iy = 7 - iy;
    uint32_t a = tile * 32u + iy * 4u + (ix >> 1);
    unsigned c = VDP.vram[a & 0xFFFF];
    c = (ix & 1) ? (c & 15) : (c >> 4);
    return c ? (pal * 16 + c) : 0;
}

/* ---- validation ------------------------------------------------------- */

static unsigned long chk_tiles, chk_bad, chk_frames;

void mapview_check(void) {
    static int on = -1;
    if (on < 0) on = getenv("DB4A_MAPCHECK") ? 1 : 0;
    if (!on || !map_base || !render_widescreen_gameplay()) return;

    int camx = (int)(int16_t)m68k_read16(CAM_X);
    int camy = (int)(int16_t)m68k_read16(CAM_Y);
    uint32_t na = (uint32_t)(VDP.reg[2] & 0x38) << 10;
    uint32_t nb = (uint32_t)(VDP.reg[4] & 0x07) << 13;

    /* Only the 320x224 the cartridge itself maintains -- outside it there is
       nothing to compare against, which is the whole reason this exists. */
    for (int ty = 0; ty < 28; ty++) {
        for (int tx = 0; tx < 40; tx++) {
            int col = (camx >> 3) + tx, row = (camy >> 3) + ty;
            for (int pb = 0; pb < 2; pb++) {
                uint32_t base = pb ? nb : na;
                uint32_t e = base + (uint32_t)(((row & 31) * 64 + (col & 63)) * 2);
                uint16_t got  = (uint16_t)((VDP.vram[e & 0xFFFF] << 8)
                                           | VDP.vram[(e + 1) & 0xFFFF]);
                uint16_t mine = mapview_entry(pb, col, row);
                chk_tiles++;
                if (got != mine) chk_bad++;
            }
        }
    }
    chk_frames++;
}

void mapview_report(void) {
    if (!chk_frames) return;
    fprintf(stderr, "[mapv] base=%06X  tiles=%lu  mismatched=%lu (%.2f%%)  frames=%lu\n",
            map_base, chk_tiles, chk_bad,
            chk_tiles ? 100.0 * (double)chk_bad / (double)chk_tiles : 0.0, chk_frames);
}
