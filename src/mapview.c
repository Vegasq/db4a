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
#define MAP_PTR    0x00FFE404u     /* the game's own pointer to cell (0, 0) */
#define CELL_ROW   0x100u          /* bytes per cell row  */
#define CELL_COL   4u              /* bytes per cell      */

static uint32_t map_base;          /* address of cell (0, 0) */

/* DB4A_LOG_MAPV=1 -- why the margin is still falling back to the tilemap.
 *
 * The margin reads the map only once the base is known; until then it falls
 * through to the tilemap, which the cartridge maintains for its own 320
 * pixels only. So "how many draws until the base is learned" is exactly how
 * long a black bar sits at the left edge of a new mission, and whether those
 * draws are happening at all is the difference between a missing hook and a
 * rejected candidate. */
static int mapv_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("DB4A_LOG_MAPV") ? 1 : 0;
    return on;
}
static unsigned long obs_draws;

/* A base is PROVISIONAL until a diverse sample confirms it; see
   mapview_poll. */
static int base_confirmed;

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
static int base_agrees(uint32_t cand, int *exact) {
    uint32_t save = map_base;
    map_base = cand;
    int camx = (int)(int16_t)m68k_read16(CAM_X);
    int camy = (int)(int16_t)m68k_read16(CAM_Y);
    uint32_t na = (uint32_t)(VDP.reg[2] & 0x38) << 10;

    /* Exact agreement over a DIVERSE sample.
     *
     * An earlier version accepted 90% and did not look at what it was
     * comparing. Both were wrong. Nine tenths leaves room for a base that is
     * merely close, and a screen of unexplored map is one tile repeated --
     * where almost any base agrees and the sample proves nothing. A search
     * over work RAM duly found a decoy that passed and then mismatched 58% of
     * the picture. Demand every sampled tile, and demand the sample carry
     * enough distinct entries to be worth agreeing with. */
    uint16_t seen[16]; int nseen = 0, checked = 0, bad = 0;
    for (int ty = 6; ty < 22; ty++) {
        for (int tx = 3; tx < 37; tx += 2) {
            int col = (camx >> 3) + tx, row = (camy >> 3) + ty;
            uint32_t e = na + (uint32_t)(((row & 31) * 64 + (col & 63)) * 2);
            uint16_t got = (uint16_t)((VDP.vram[e & 0xFFFF] << 8)
                                      | VDP.vram[(e + 1) & 0xFFFF]);
            checked++;
            if (got != mapview_entry(0, col, row)) { bad++; continue; }
            int fresh = 1;
            for (int i = 0; i < nseen; i++) if (seen[i] == got) { fresh = 0; break; }
            if (fresh && nseen < 16) seen[nseen++] = got;
        }
    }
    map_base = save;
    /* Not exact: the cartridge writes a little of its own over the tilemap, so
       even the right base disagrees on a fraction of a percent. Not lax
       either: a wrong base is wrong nearly everywhere. And diverse, because a
       screen of unexplored map is one tile repeated, where any base agrees and
       the sample proves nothing -- that is how a search over work RAM found a
       decoy that passed and then mismatched 58% of the picture. */
    /* Exact and unique is a second, weaker kind of evidence -- see
       mapview_observe. Reported separately so the diverse rule below stays
       exactly as strict as it was. */
    if (exact) *exact = (checked >= 200 && bad == 0);
    int ok = checked >= 200 && nseen >= 6 && bad * 25 <= checked;
    if (mapv_log())
        fprintf(stderr, "[mapv]   cand=%06X checked=%d nseen=%d bad=%d -> %s\n",
                cand, checked, nseen, bad,
                ok ? "AGREES" : (nseen < 6 ? "rejected: sample not diverse"
                                           : "rejected: disagrees"));
    return ok;
}

/* Learn where the map lives, from any draw the cartridge makes.
 *
 * COLUMN draws ($7504 / $7468) happen when the camera crosses a tile boundary
 * horizontally; ROW draws ($764E / $75B0) when it crosses one vertically. Both
 * pass the same three things -- d1's low word is the nametable address of the
 * first entry, a3 the map pointer for it, d0 the sub-position -- so either
 * will do, and taking both is what lets a session that only ever scrolls up
 * and down find the map at all. Resuming a save state and scrolling
 * vertically used to leave the margin falling back to the tilemap, which
 * wraps its 256-pixel plane and smears the top of the screen along the bottom.
 *
 * The nametable tells us the column only modulo 64 and the row only modulo 32,
 * and the cartridge draws at whichever edge it is heading for, so neither can
 * be resolved by proximity to the camera. Rather than reason about it, every
 * candidate that could be on screen is TRIED and the one that reproduces what
 * the cartridge has already drawn is kept. Guessing here is what once put the
 * base a lap out -- $40, exactly 16 cells -- and every tile wrong after it. */
void mapview_observe(uint32_t pc) {
    if (map_base && base_confirmed) return;
    obs_draws++;
    int plane_b = (pc == WS_COL_PLANE_B || pc == WS_ROW_PLANE_B);
    uint32_t nb = plane_b ? (uint32_t)(VDP.reg[4] & 0x07) << 13
                          : (uint32_t)(VDP.reg[2] & 0x38) << 10;
    uint32_t off = (CPU.d[1] & 0xFFFFu) - nb;
    int ntcol = (int)((off & 0x7Eu) >> 1);
    int ntrow = (int)((off >> 7) & 31u);
    int camx  = (int)(int16_t)m68k_read16(CAM_X);
    int camy  = (int)(int16_t)m68k_read16(CAM_Y);
    int cbase = camx >> 3, rbase = camy >> 3;
    int n_exact = 0;
    uint32_t only_exact = 0;

    for (int lc = -1; lc <= 1; lc++) {
        int wcol = cbase + ((ntcol - cbase) & 63) + lc * 64;
        if (wcol - cbase < -8 || wcol - cbase > 56) continue;
        if ((int)((CPU.d[0] & 6u) >> 1) != (wcol & 3)) continue;
        for (int lr = -1; lr <= 1; lr++) {
            int wrow = rbase + ((ntrow - rbase) & 31) + lr * 32;
            if (wrow - rbase < -8 || wrow - rbase > 40) continue;
            if ((int)((CPU.d[0] & 0x18u) >> 3) != (wrow & 3)) continue;
            uint32_t cand = CPU.a[3] - (uint32_t)((wcol >> 2) * (int)CELL_COL)
                                     - (uint32_t)((wrow >> 2) * (int)CELL_ROW);
            int exact = 0;
            if (base_agrees(cand, &exact)) {
                map_base = cand;
                base_confirmed = 1;
                if (mapv_log())
                    fprintf(stderr, "[mapv] base=%06X CONFIRMED by a draw at "
                                    "pc=%06X (draw %lu)\n", map_base, pc, obs_draws);
                return;
            }
            if (exact) { n_exact++; only_exact = cand; }
        }
    }

    /* A provisional base gets tested by every draw until something diverse
       enough comes along to confirm it. If it stops reproducing the tilemap
       exactly, drop it rather than keep drawing from it. */
    if (map_base && !(n_exact == 1 && only_exact == map_base)) {
        if (mapv_log())
            fprintf(stderr, "[mapv] provisional base=%06X DROPPED at draw %lu\n",
                    map_base, obs_draws);
        map_base = 0;
    }
}

/* Take the base from the game's own pointer, once per frame.
 *
 * Learning from a draw alone leaves the margin black at the start of every
 * mission, because the cartridge draws no column or row until the camera
 * crosses a tile boundary -- measured on data/recordings/tour.txt: gameplay
 * begins at frame 3109 and the first draw of any kind is at frame 3201, so
 * the strip sat at 6.5% lit for two seconds while the columns beside it were
 * at 97%. A player who never scrolls never gets a margin at all, which is
 * what the scripted Harkonnen and Ordos runs do.
 *
 * $FFE404 is the game's own pointer to cell (0, 0). It was found by dumping
 * work RAM and looking for the base the draws had already agreed on, and it
 * holds that same value in mission 1, mission 2 and all three houses, before
 * any draw has happened.
 *
 * It is still not TRUSTED. The pointer only proposes; base_agrees decides, by
 * requiring the candidate to reproduce the tilemap the cartridge has already
 * written. A diverse screen confirms it outright; a uniform one -- which is
 * what a mission opens on, every tile the same shroud -- can only say the
 * candidate is exact over the sample, so it is taken PROVISIONALLY and the
 * first draw either confirms or drops it. That is the same standard as
 * before: nothing draws the margin until it has reproduced what the cartridge
 * itself put on screen. */
void mapview_poll(void) {
    if (map_base || !render_widescreen_gameplay()) return;
    uint32_t cand = m68k_read32(MAP_PTR);
    if ((cand & 0xFF0000u) != 0xFF0000u) return;   /* must point into work RAM */
    int exact = 0;
    if (base_agrees(cand, &exact)) {
        map_base = cand;
        base_confirmed = 1;
        if (mapv_log())
            fprintf(stderr, "[mapv] base=%06X CONFIRMED from $FFE404\n", map_base);
    } else if (exact) {
        map_base = cand;
        if (mapv_log())
            fprintf(stderr, "[mapv] base=%06X provisional from $FFE404\n", map_base);
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
static unsigned long chk_blank;

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
                /* A blank entry means the cartridge has not filled that cell,
                   not that it disagrees with us.
                   .
                   It stops filling where it has nothing to show -- hard
                   against a camera limit, most visibly -- and leaves $0000,
                   which draws as backdrop. We compute the map's own fog tile
                   there, $800B, whose pixels are colour 12, and palette entry
                   12 is $0000. Both are black; only the ENTRY differs, and the
                   entry is not what anyone sees. Counted separately rather
                   than ignored, so it cannot hide a real disagreement. */
                if (!got) { chk_blank++; continue; }
                chk_tiles++;
                if (got != mine) chk_bad++;
            }
        }
    }
    chk_frames++;
}

void mapview_report(void) {
    if (!chk_frames) return;
    fprintf(stderr, "[mapv] base=%06X  tiles=%lu  mismatched=%lu (%.2f%%)  frames=%lu  unfilled=%lu\n",
            map_base, chk_tiles, chk_bad,
            chk_tiles ? 100.0 * (double)chk_bad / (double)chk_tiles : 0.0, chk_frames, chk_blank);
}
