/* Mouse control for the game's cursor -- and the first piece of game logic
 * this project owns in C rather than running out of the cartridge.
 *
 * ---------------------------------------------------------------------------
 * What the ROM does
 * ---------------------------------------------------------------------------
 * Found by watching writes to $FFBF12 (DB4A_WATCH=FFBF12) and disassembling
 * the block that made them. The cursor update lives at $6DF8-$7060 and works
 * like this:
 *
 *   $FFBF12  cursor X, screen pixels     $FFBF14  cursor Y
 *   $FFBF1A  min X    $FFBF1C  min Y     ) clamp box, written by the routine
 *   $FFBF1E  max X    $FFBF20  max Y     ) at $4DA8 -- 24, 24, 296, 200
 *   $FFBF3C  X sub-pixel accumulator     $FFBF3E  Y sub-pixel accumulator
 *   $FFBF40  current speed, 16.16 fixed point
 *   $FFBF34  X overflow past the clamp   $FFBF36  Y overflow -- these scroll
 *
 * Each frame the routine adds $1000 (1/16 px) to the speed and caps it at
 * $30000, so the cursor accelerates from 1 to 3 pixels per frame and no
 * further. Releasing the d-pad resets the speed to $10000. Movement is added
 * in 16.16 fixed point through the sub-pixel accumulators, then clamped to the
 * box; whatever did not fit is written to $FFBF34/36 and scrolls the map.
 *
 * Three pixels per frame at 50 Hz is 150 px/s, so crossing the 320-pixel
 * screen takes just over two seconds. A mouse crosses it instantly, which is
 * why chasing the pointer with synthesised d-pad presses always trailed behind
 * it however the steering was tuned. The cursor was not being steered badly;
 * it was being asked to go somewhere it cannot travel fast enough to reach.
 *
 * ---------------------------------------------------------------------------
 * What we do instead
 * ---------------------------------------------------------------------------
 * Inside the clamp box we write the cursor position directly, so it is exactly
 * under the pointer on the same frame -- no acceleration curve, no chase, no
 * deadzone. This is safe because the ROM recomputes everything downstream (the
 * map cell under the cursor, the sprite, the selection) from the position
 * every frame, and because with no direction held the routine takes the
 * $6EB8 branch, which resets the speed and jumps straight to that recompute
 * without touching the position. So our value survives the frame intact.
 *
 * The clamp box is also ours now. The ROM keeps the cursor 24 pixels clear of
 * every edge, which with a mouse just means the outermost 24 pixels of the map
 * cannot be pointed at. We rewrite the box each frame with a much smaller
 * margin so the cursor can reach the screen edge -- and, importantly, so it can
 * get far enough into the scroll band for src/cursor.c to generate a speed.
 *
 * Scrolling needs no help from here. src/cursor.c owns the edge-scroll routine
 * and puts the band at the screen edge, so simply holding the pointer there
 * scrolls the map. This used to synthesise d-pad presses to provoke the ROM
 * into scrolling; it no longer touches the d-pad at all, which removes the
 * whole class of bug where a press was left held.
 *
 * Nothing here runs outside gameplay, and nothing here runs while the keyboard
 * or pad is being used -- see mouse_steer()'s contract in mouse.h.
 */
#include "mouse.h"
#include "render.h"
#include "config.h"
#include "input.h"
#include "hal.h"
#include "m68k.h"
#include "cursor.h"
#include <stdlib.h>

#define CURSOR_X_ADDR   0xBF12
#define CURSOR_Y_ADDR   0xBF14
#define BOUND_MIN_X     0xBF1A
#define BOUND_MIN_Y     0xBF1C
#define BOUND_MAX_X     0xBF1E
#define BOUND_MAX_Y     0xBF20

#define RAM_BASE        0xFF0000u
#define SCREEN_W        320
#define SCREEN_H        224

/* The ROM's margin, kept as the value we are deliberately departing from. */
#define ROM_CLAMP_MARGIN 24
#define DEFAULT_CLAMP    4

static int enabled;
static int clamp_margin = -1;    /* resolved from DB4A_MOUSE_CLAMP on first use */
static int have_last, last_px, last_py;

void mouse_enable(int on) { enabled = on ? 1 : 0; }
int  mouse_enabled(void)  { return enabled; }

static int read_word(unsigned addr) {
    size_t len = 0;
    const uint8_t *ram = hal_ram_ptr(&len);
    if (!ram || len < 0x10000) return -1;
    return (ram[addr] << 8) | ram[addr + 1];
}

static void write_word(unsigned addr, int v) {
    m68k_write16(RAM_BASE + addr, (uint16_t)v);
}

int mouse_clamp_margin(void) {
    if (clamp_margin < 0) {
        const char *e = cfg("DB4A_MOUSE_CLAMP");
        clamp_margin = e ? atoi(e) : DEFAULT_CLAMP;
        if (clamp_margin < 0)  clamp_margin = 0;
        if (clamp_margin > 64) clamp_margin = 64;  /* past this the box inverts */
    }
    return clamp_margin;
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

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Ownership of the cursor's clamp box, split out from mouse_steer().
 *
 * ---------------------------------------------------------------------------
 * Why this is not part of the steering
 * ---------------------------------------------------------------------------
 * It was, and that was task #26: "with mouse control on the arrow keys no
 * longer scroll the map". Reproduced headlessly from a mission save state,
 * holding RIGHT for 200 frames:
 *
 *     no mouse           camera 699 -> 1194   (scrolled 495 px)
 *     mouse control on   camera 699 ->  699   (scrolled   0 px)
 *
 * The cursor walked to x=296 and stopped there for the remaining 190 frames.
 *
 * 296 is not a coincidence. The frontend suppresses steering while a keyboard
 * direction is held -- correctly, so the keys win -- and mouse_steer() was the
 * only thing that ever replaced the cartridge's own clamp box. So on exactly
 * the frames the player is using the arrows, the box reverts to the ROM's
 * 24..296, while cursor_scroll_band() still puts the scroll threshold at
 * 320-24 = 296. The cursor may reach the threshold and may never pass it, the
 * distance past it is therefore always zero, and a zero distance is a zero
 * velocity. db4a.conf.example already warned about this shape of fault under
 * `mouse_clamp`: "if the two meet there is no depth to measure and the map
 * will not scroll at all". It did not say that the ROM's box could come back
 * and make them meet.
 *
 * The box is a property of MOUSE MODE, not of the pointer having moved, so it
 * belongs somewhere that runs every gameplay frame regardless of who is
 * driving. src/cursor.c's override of $706C is exactly that, and already owns
 * the camera limits on the same argument. */
void mouse_own_clamp_box(void) {
    if (!enabled || !in_gameplay()) return;
    int c    = mouse_clamp_margin();
    int ext  = cursor_west_extension();
    write_word(BOUND_MIN_X, c - ext);
    write_word(BOUND_MIN_Y, c);
    write_word(BOUND_MAX_X, SCREEN_W - c);
    write_word(BOUND_MAX_Y, SCREEN_H - c);
}

int mouse_steer(int target_x, int target_y) {
    if (!enabled) return 0;

    /* Every path that declines to steer MUST release the pad first. Returning
       early without doing so leaves whatever was pressed held down, which is
       how this first shipped: the d-pad stuck on entering the mentat screen.
       Steering no longer presses anything, but the release stays -- it is the
       cheap half of the invariant and it costs nothing to keep. */
    if (!in_gameplay()) { release_dpad(); have_last = 0; return 0; }

    /* How far WEST of the cartridge's own screen the cursor may travel.
     *
     * Widescreen draws a strip of real map west of the 320 the cartridge
     * believes in. Its cursor lives in that 320-wide field, so without this
     * the pointer can see the strip but never reach it: the cursor stopped 84
     * pixels short of the window edge, and the left scroll band -- 24 pixels
     * inside the CURSOR's field -- began 104 pixels inside the WINDOW, while
     * the other three edges began at 24.
     *
     * The field can be extended because the cartridge's own cursor-to-map-cell
     * conversion at $5518 is linear and unclamped:
     *
     *     d0 = $E3EC - $FFBF54 + CUR_X    cursor to world pixels
     *     d0 &= $7F0 ; d0 >>= 4           to a map cell
     *
     * There is no sign test and no saturation -- the mask wraps rather than
     * clamps -- so a negative cursor X keeps picking the correct cell for as
     * long as the base keeps the sum positive, which it does everywhere except
     * hard against the map's western edge, where there is no map to point at.
     *
     * Verified that the cartridge never itself writes a negative cursor X
     * (0 of 3000 writes across a mission), so nothing treats it as a sentinel;
     * the guard below was ours and defensive. */
    int ext = cursor_west_extension();

    if (read_word(CURSOR_X_ADDR) < -ext) { release_dpad(); return 0; }

    int c    = mouse_clamp_margin();
    int minx = c - ext, miny = c, maxx = SCREEN_W - c, maxy = SCREEN_H - c;

    /* Take ownership of the clamp box. The ROM writes it once per mission from
       $4DA8, so setting it every frame is enough to keep it ours. Done here as
       well as from src/cursor.c so the box is already ours on the same frame
       the pointer moves, rather than from the next one. */
    mouse_own_clamp_box();

    int px = clampi(target_x, -ext, SCREEN_W - 1);
    int py = clampi(target_y, 0, SCREEN_H - 1);

    /* Last input wins: if the pointer has not moved, leave the cursor alone so
       the keyboard and pad can move it without the mouse snatching it back.
       The exception is the scroll band, where the cursor must be held in place
       against src/cursor.c pulling it out -- otherwise parking the pointer at
       the edge would scroll for three frames and stop. */
    int b       = cursor_scroll_band();
    /* The left band is measured from the TRUE screen edge, not from the
       cartridge's. Leaving it at `px < b` would keep scrolling starting a
       whole strip-width further in than the other three sides. */
    int in_band = px < b - ext || px >= SCREEN_W - b || py < b || py >= SCREEN_H - b;
    /* Anywhere west of the cartridge's own screen the cursor must also be
       held, or the scroll routine drags it back out of the strip on the very
       next frame and the pointer can never reach the map drawn there. */
    if (px < 0) in_band = 1;
    int moved   = !have_last || px != last_px || py != last_py;
    last_px = px; last_py = py; have_last = 1;

    if (moved || in_band) {
        write_word(CURSOR_X_ADDR, clampi(px, minx, maxx));
        write_word(CURSOR_Y_ADDR, clampi(py, miny, maxy));
    }
    return moved || in_band;
}
