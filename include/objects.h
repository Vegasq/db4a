#ifndef OBJECTS_H
#define OBJECTS_H
#include <stdint.h>

/* The game's object list, read directly.
 *
 * The cartridge builds its sprite table at $1088: it walks a linked list of
 * objects, converts each to screen space, expands it into pieces, culls what
 * falls outside its 320x224, charges what survives against a per-band budget,
 * and writes at most 80 entries into a shadow at $FFE428 which $6716 DMAs to
 * the hardware.
 *
 * Every one of those steps exists because the hardware has 80 sprites and a
 * per-scanline limit. None of them is something we need. Reading the objects
 * ourselves means a unit is drawn where it is, at any resolution, with no
 * table and no budget -- the same move mapview.c made for the ground.
 *
 * BEFORE ANY OF THAT, this predicts what the cartridge WILL emit and compares
 * against what it did. A predictor that cannot reproduce the shadow has no
 * business painting pixels, and this project has repeatedly been wrong in ways
 * that only a comparison against the cartridge caught.
 *
 * The prediction runs at the emitter's ENTRY, because the blink logic at
 * $10CE..$111E decrements a counter inside each object and toggles a flag as
 * it goes. Predicting afterwards would read state the cartridge has already
 * advanced, and could not tell which way it went.
 */

/* One sprite table entry, in the cartridge's own terms. */
struct obj_entry {
    int      y;          /* biased by $80, as the hardware wants   */
    int      x;
    uint16_t size_link;  /* size in the high bits; link is ours     */
    uint16_t attr;
};

/* One piece to draw, in the cartridge's own terms: x and y biased by $80,
 * size as the hardware encodes it, attributes carrying tile, palette, flip
 * and priority. */
struct obj_piece {
    int      x, y;
    uint8_t  size;
    uint16_t attr;
};

/* Collect the pieces that fall OUTSIDE the cartridge's 320x224 but inside a
 * margin `ext` pixels wide to the west and `exth` tall to the south -- exactly
 * the ones its culls throw away. Returns how many were written.
 *
 * This is the whole point of reading the object list: no 80-entry table, no
 * per-band budget, no culling to a screen we are not using. The cartridge's
 * own run is untouched, so nothing here can change what the game does. */
unsigned objects_margin(struct obj_piece *out, unsigned max, int ext, int exth);

/* Snapshot the prediction. Call on entry to $1088. */
void objects_predict(void);

/* Compare the prediction against the shadow. Call on entry to $6716. */
void objects_verify(void);

void objects_report(void);

#define OBJ_EMITTER 0x00001088u
#define OBJ_SAT_DMA 0x00006716u

#endif
