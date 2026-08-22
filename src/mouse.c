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

/* Only steer during gameplay.
 *
 * The game's main loop dispatches through a function pointer at $FFFFE002, so
 * that pointer identifies the scene: 006D0C is gameplay, 00608E is gameplay
 * while placing a building, 00B540 is gameplay. Menus, briefings and cutscenes
 * are anything else.
 *
 * This matters more than it looks. Outside gameplay the cursor variables hold
 * (0,0), which is a perfectly plausible-looking coordinate, so a range check
 * passes and the steering happily drives towards the pointer -- holding right
 * and down forever and making the mentat screen impossible to get past. */
static int in_gameplay(void);

int mouse_steering_active(void) { return enabled && in_gameplay(); }

static int in_gameplay(void) {
    uint32_t s = ((uint32_t)read_word(0xE002) << 16) | (uint32_t)read_word(0xE004);
    return s == 0x006D0Cu || s == 0x00608Eu || s == 0x00B540u;
}

static void release_dpad(void) {
    pad_set(PAD_LEFT, 0);
    pad_set(PAD_RIGHT, 0);
    pad_set(PAD_UP, 0);
    pad_set(PAD_DOWN, 0);
}

void mouse_cursor_pos(int *x, int *y) {
    if (x) *x = read_word(CURSOR_X_ADDR);
    if (y) *y = read_word(CURSOR_Y_ADDR);
}

int mouse_steer(int target_x, int target_y) {
    if (!enabled) return 0;

    /* Every path that declines to steer MUST release the pad first. Returning
       early without doing so leaves whatever was pressed held down, which is
       how this first shipped: the d-pad stuck on entering the mentat screen. */
    if (!in_gameplay()) { release_dpad(); return 0; }

    int cx = read_word(CURSOR_X_ADDR);
    int cy = read_word(CURSOR_Y_ADDR);
    if (cx < 0 || cy < 0) { release_dpad(); return 0; }

    int dx = target_x - cx, dy = target_y - cy;
    int drove = 0;

    pad_set(PAD_LEFT,  dx < -DEADZONE);
    pad_set(PAD_RIGHT, dx >  DEADZONE);
    pad_set(PAD_UP,    dy < -DEADZONE);
    pad_set(PAD_DOWN,  dy >  DEADZONE);
    if (dx < -DEADZONE || dx > DEADZONE || dy < -DEADZONE || dy > DEADZONE) drove = 1;

    return drove;
}
