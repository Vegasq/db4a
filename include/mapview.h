#ifndef MAPVIEW_H
#define MAPVIEW_H
#include <stdint.h>

/* Drawing the map from the GAME'S OWN MAP DATA, not from the tilemap.
 *
 * src/render.c is a VDP: it paints whatever is in the nametables. That is
 * right for everything the cartridge draws, and useless for anything it does
 * not -- and the cartridge only ever fills the tilemap for the 320 pixels it
 * believes are visible. Every widescreen fault so far has come from trying to
 * reconstruct the missing columns by watching the cartridge draw the ones it
 * does maintain, then extrapolating: it depends on which direction you last
 * scrolled, it dies on a vertical scroll, and it has to guess which lap of the
 * 64-column ring a column belongs to.
 *
 * This reads the map instead. The map is a grid of CELLS in work RAM, each
 * four tiles square, and the cartridge expands one cell byte into sixteen tile
 * entries through a table in ROM:
 *
 *     cell (cx, cy) is at   MAP + cy * 0x100 + cx * 4
 *     plane A takes the byte at that address, & $FE, and looks up
 *                           TILES + val * 16 + (subrow << 3 | subcol << 1)
 *     plane B takes the word, & $1FF, and looks up
 *                           TILES + val * 32 + (subrow << 3 | subcol << 1)
 *
 * both reading a WORD, which is a finished nametable entry. Verified against
 * the running game: the sub-position the cartridge passes in d0 always equals
 * the world column mod 4, and (a3 - cellcolumn * 4) is constant for a given
 * camera row.
 *
 * The address is a pure function of the CELL COORDINATE, which comes straight
 * from the camera. There is no ring, no lap, no direction and no history -- so
 * none of the failures above can occur. What it cannot do is invent map: where
 * the game has drawn nothing, or the player has explored nothing, this reports
 * the same nothing the cartridge would.
 */

/* Where the map lives. Derived once by watching a column draw, because it is
 * per-mission; 0 until then. */
uint32_t mapview_base(void);
int      mapview_ready(void);

/* Called on the cartridge's column-draw entry to learn, confirm or drop the
 * base. */
void mapview_observe(uint32_t pc);

/* Called once per rendered frame. Proposes the base from the game's own
 * pointer at $FFE404, so the margin works before the first scroll -- and
 * before any draw exists to learn from. Validated, never trusted: see the
 * comment on the definition. */
void mapview_poll(void);


/* The nametable entry for one tile of the world, straight from the map.
 * col/row are WORLD tile coordinates (camera pixels / 8). */
uint16_t mapview_entry(int plane_b, int col, int row);

/* Compare what mapview computes against what the cartridge actually wrote,
 * over the whole visible screen. DB4A_MAPCHECK=1. */
/* The colour index for one WORLD pixel, straight from the map: 0 if the tile
 * is transparent there. *prio receives the entry's priority bit. */
unsigned mapview_pixel(int plane_b, int px, int py, int *prio);

void mapview_check(void);
void mapview_report(void);

#endif
