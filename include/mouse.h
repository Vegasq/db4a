#ifndef MOUSE_H
#define MOUSE_H
#include <stdint.h>

/* Mouse control: point at the map and the game's cursor comes to you.
 *
 * The game has no mouse, and its own cursor cannot travel faster than 3 pixels
 * per frame, so it can never catch a pointer by being steered. Inside the map
 * this writes the cursor position directly; in the margin at the screen edge it
 * holds the d-pad and lets the ROM scroll. See src/mouse.c for the RAM layout
 * and why writing the position directly is safe.
 */
void mouse_enable(int on);
int  mouse_enabled(void);

/* Call once per frame with the pointer in GAME pixels, and ONLY when no
 * keyboard or pad direction is held -- those win the d-pad. Returns 1 if it
 * moved the cursor or drove a scroll this frame. */
int  mouse_steer(int target_x, int target_y);

/* How close to each screen edge the cursor is allowed to get. Must stay well
 * inside cursor_scroll_band() or there is no depth left to scroll with.
 * DB4A_MOUSE_CLAMP overrides; the ROM's own value is 24. */
int  mouse_clamp_margin(void);

/* Where the game thinks its cursor is, for diagnostics. */
/* True when steering is on AND the game is in a scene that has a cursor. */
int  mouse_steering_active(void);

void mouse_cursor_pos(int *x, int *y);

#endif
