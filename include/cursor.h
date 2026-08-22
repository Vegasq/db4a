#ifndef CURSOR_H
#define CURSOR_H

/* The game's cursor edge-scrolling, reimplemented in C. See src/cursor.c. */

/* Pixels from each screen edge where pointing scrolls the map. The cartridge's
 * own dead zone is far larger than this -- X 120..200 of 320 -- so the band it
 * implies is 120 pixels deep. DB4A_MOUSE_EDGE overrides; default 24. Only
 * consulted when mouse control is on. */
int cursor_scroll_band(void);

#endif
