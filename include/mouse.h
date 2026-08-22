#ifndef MOUSE_H
#define MOUSE_H
#include <stdint.h>

/* Mouse control: point at the map and the game's cursor comes to you.
 *
 * The game has no mouse. Its cursor is moved a step at a time by the d-pad, so
 * this reads where the cursor currently is and synthesises the d-pad presses
 * that walk it towards the pointer -- the game's own movement code does the
 * work, and nothing about its logic is modified.
 */
void mouse_enable(int on);
int  mouse_enabled(void);

/* Call once per frame with the pointer in GAME pixels. Returns 1 if it drove
 * the d-pad this frame. */
int  mouse_steer(int target_x, int target_y);

/* Where the game thinks its cursor is, for diagnostics. */
/* True when steering is on AND the game is in a scene that has a cursor. */
int  mouse_steering_active(void);

void mouse_cursor_pos(int *x, int *y);

#endif
