#ifndef CURSOR_H
#define CURSOR_H

/* The game's cursor edge-scrolling, reimplemented in C. See src/cursor.c. */

/* Pixels from each screen edge where pointing scrolls the map. The cartridge's
 * own dead zone is far larger than this -- X 120..200 of 320 -- so the band it
 * implies is 120 pixels deep. DB4A_MOUSE_EDGE overrides; default 24. Only
 * consulted when mouse control is on. */
int cursor_scroll_band(void);


/* How far west of the cartridge's 320-pixel screen the cursor may travel,
 * clamped to the map that actually exists. 0 unless widescreen gameplay. */
int cursor_west_extension(void);

#endif
