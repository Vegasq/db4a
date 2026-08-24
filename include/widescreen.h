#ifndef WIDESCREEN_H
#define WIDESCREEN_H
#include <stdint.h>

/* Drawing the map columns the cartridge does not draw.
 *
 * Dune keeps only the columns it believes are visible. It scrolls by rewriting
 * the tilemap -- horizontal scroll moves within a 512-pixel ring, and as the
 * camera crosses a tile boundary it draws the one or two new columns arriving
 * at the edge. Nothing else in the ring is maintained.
 *
 * That is why the widescreen extension goes black when you scroll WEST: the
 * extra columns sit west of the view, so they are behind the camera going east
 * (drawn a moment ago, and still correct) but ahead of it going west, where
 * the game has not drawn them yet. Measured in docs/widescreen.md.
 *
 * So we draw them. widescreen_note_column() is called when the cartridge's own
 * column-draw routine is entered; it reads that routine's parameters and draws
 * the same kind of column, from the same map data and the same tile table, for
 * the columns further west that the extension needs.
 *
 * This is an OBSERVER, not a native override: it does not replace the block,
 * consume cycles, or alter any register. It writes tilemap entries the game
 * will not read at 320 and would have written itself had it believed the
 * screen were wider. */
void widescreen_note_column(uint32_t pc);

/* Called once per frame, before the frame is drawn: fills the extension from
 * the remembered anchor, so a stationary camera does not leave it blank. */
void widescreen_extend(void);
void widescreen_check_report(void);

/* Append the strip's units to the sprite list AFTER the cartridge's emitter
 * has finished, so its own run is untouched. Hooked on $6716, the DMA that
 * copies the shadow to VRAM. */
void widescreen_append_sprites(void);
#define WS_SAT_DMA 0x00006716u

/* Native override of $11B4, the cartridge's left-edge sprite cull. Widened by
 * the extension width so units standing in it are not dropped. Identical to
 * the cartridge when widescreen is off. */
uint32_t native_sprite_left_cull(void);
#define WS_SPRITE_LEFT_CULL 0x0011B4u

/* Native override of $11DC, where a surviving sprite is charged against its
 * band's budget. Extension sprites are drawn but not charged, so they cannot
 * change which other sprites the cartridge decides to suppress. */
uint32_t native_sprite_band_count(void);
#define WS_SPRITE_BAND_COUNT 0x0011DCu

/* The two cartridge routines this hooks. Both draw a column downward with the
 * VDP autoincrement set to one nametable row; they differ in how they index
 * the shared tile table at $4ADE8. */
#define WS_COL_PLANE_A 0x007504u
#define WS_COL_PLANE_B 0x007468u

#endif
