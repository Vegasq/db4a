/* Steer the game's cursor with the mouse.
 *
 * Dune's cursor is moved by the d-pad, one step per press, and the game clamps
 * and scrolls as it sees fit. Rather than fight that, this reads the cursor's
 * current screen position out of RAM and presses whichever directions reduce
 * the distance to the pointer. The game's own movement, clamping and edge
 * scrolling all still apply, so nothing here can put the cursor somewhere the
 * game would not.
 *
 * The cursor position lives at $FFBF12 (x) and $FFBF14 (y), as screen pixels.
 * Found by moving the cursor and looking for a word that changes on left/right
 * but not up/down, and the reverse -- then confirmed by eye: the on-screen
 * bracket sits at exactly those coordinates.
 */
#include "mouse.h"
#include "input.h"
#include "hal.h"
#include <stdlib.h>

#define CURSOR_X_ADDR 0xBF12
#define CURSOR_Y_ADDR 0xBF14

/* One d-pad press moves the cursor about 7 pixels. Stopping inside that is
 * impossible, so anything closer counts as arrived -- otherwise the cursor
 * oscillates around the pointer forever. */
#define DEADZONE 6

static int enabled;

void mouse_enable(int on) { enabled = on ? 1 : 0; }
int  mouse_enabled(void)  { return enabled; }

static int read_word(unsigned addr) {
    size_t len = 0;
    const uint8_t *ram = hal_ram_ptr(&len);
    if (!ram || len < 0x10000) return -1;
    return (ram[addr] << 8) | ram[addr + 1];
}

void mouse_cursor_pos(int *x, int *y) {
    if (x) *x = read_word(CURSOR_X_ADDR);
    if (y) *y = read_word(CURSOR_Y_ADDR);
}

int mouse_steer(int target_x, int target_y) {
    if (!enabled) return 0;
    int cx = read_word(CURSOR_X_ADDR);
    int cy = read_word(CURSOR_Y_ADDR);
    if (cx < 0 || cy < 0) return 0;

    /* A cursor position far outside the screen means we are not in a scene
       that has one -- a menu or a cutscene -- so leave the pad alone. */
    if (cx < -64 || cx > 512 || cy < -64 || cy > 384) return 0;

    int dx = target_x - cx, dy = target_y - cy;
    int drove = 0;

    if (dx < -DEADZONE) { pad_set(PAD_LEFT, 1);  drove = 1; }
    else                  pad_set(PAD_LEFT, 0);
    if (dx > DEADZONE)  { pad_set(PAD_RIGHT, 1); drove = 1; }
    else                  pad_set(PAD_RIGHT, 0);
    if (dy < -DEADZONE) { pad_set(PAD_UP, 1);    drove = 1; }
    else                  pad_set(PAD_UP, 0);
    if (dy > DEADZONE)  { pad_set(PAD_DOWN, 1);  drove = 1; }
    else                  pad_set(PAD_DOWN, 0);

    return drove;
}
