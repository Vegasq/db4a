/* Mouse control on the pre-mission screens.
 *
 * Two screens, found by surveying everything between boot and gameplay
 * (docs/menus.md). Both work the same way and neither stores an index: the
 * game keeps the highlight's *position* and reads the choice off it.
 *
 * ---------------------------------------------------------------------------
 * House selection
 * ---------------------------------------------------------------------------
 *   $FFBEF8  highlight X: 32 Atreides, 120 Ordos, 208 Harkonnen
 *   $FFBF02  pixels left to slide; a direction is ignored unless this is 0
 *   $4808    the per-frame handler, sliding 4 px and clicking on arrival
 *
 * The shields and labels occupy x 32..111, 120..199 and 208..287 over
 * y 33..151, so the highlight X values are exactly their left edges.
 *
 * The confirmation at $4986 reads the house straight back out of $FFBEF8,
 * comparing against 32, 120 and 208 -- so the position *is* the choice, and
 * arriving late is the same as arriving early.
 *
 * ---------------------------------------------------------------------------
 * The mentat's YES/NO
 * ---------------------------------------------------------------------------
 *   $FFA62C  selector sprite Y: $128 = YES, $140 = NO (sprite Y is +128)
 *   $25CAE   the loop head, reached once per frame while the screen is up
 *
 *   YES  x 195..270, y 170..181        NO  x 193..271, y 192..205
 *
 * This screen is not a dispatched handler at all: $25C82 runs a loop that
 * waits for vblank itself ($FD4), reads the pad, and slides the selector 2 px
 * a frame for 12 frames with more vblank waits inside. So the probe goes on
 * the loop head rather than on the code that moves the sprites -- $25CF4 only
 * executes while the selector is actually travelling, which is exactly when a
 * detector must NOT be silent. Mid-slide the sprite Y is between the two
 * values, and steering waits rather than guessing.
 *
 * The buttons appear only after the question has been on screen for a while.
 * Sampling the question at 25-frame intervals shows no buttons at all, which
 * is how the first pass through this concluded -- wrongly -- that the whole
 * mentat sequence had nothing to select.
 *
 * ---------------------------------------------------------------------------
 * Detection
 * ---------------------------------------------------------------------------
 * Neither screen can be identified from the scene pointer. House selection
 * shares $00004500 with the loading transitions between gameplay segments and
 * appears twice before the first mission alone. Both are therefore detected by
 * their handler running -- see include/probe.h.
 *
 * Like the build console, this presses buttons rather than writing the
 * position: the handlers animate the highlight and play the click, and there
 * are at most two steps to make.
 */
#include "menus.h"
#include "buildmenu.h"
#include "probe.h"
#include "input.h"
#include "hal.h"
#include <stdlib.h>
#include <stdio.h>

#define HOUSE_HANDLER  0x4808u
#define MENTAT_HANDLER 0x25CAEu

#define HOUSE_X      0xBEF8      /* word */
#define HOUSE_SLIDE  0xBF02      /* word */
#define ANSWER_Y     0xA62C      /* word */

/* Sprite Y, which on the Mega Drive carries a +128 offset: $128 is screen
 * y 168 and $140 is y 192, matching the two plates measured on screen. */
#define ANSWER_YES_Y 0x128
#define ANSWER_NO_Y  0x140

static const int house_x[3] = { 32, 120, 208 };

static int enabled;
static int house_up, mentat_up;
static int releasing;

void menus_enable(int on) {
    enabled = menu_mouse_wanted() && on;
    probe_watch(PROBE_HOUSE_SELECT, enabled ? HOUSE_HANDLER  : 0);
    probe_watch(PROBE_MENTAT_ASK,   enabled ? MENTAT_HANDLER : 0);
}

static const uint8_t *ram_or_null(void) {
    size_t len = 0;
    const uint8_t *ram = hal_ram_ptr(&len);
    return (ram && len >= 0x10000) ? ram : NULL;
}

static int rw(const uint8_t *ram, unsigned a) { return (ram[a] << 8) | ram[a + 1]; }

static void release(void) {
    pad_set(PAD_LEFT, 0);  pad_set(PAD_RIGHT, 0);
    pad_set(PAD_UP,   0);  pad_set(PAD_DOWN,  0);
}

/* Which shield the highlight is on, by position. Mid-slide it is on none. */
int menus_house_selected(void) {
    const uint8_t *ram = ram_or_null();
    if (!ram || !house_up) return -1;
    int x = rw(ram, HOUSE_X);
    for (int i = 0; i < 3; i++) if (x == house_x[i]) return i;
    return -1;
}

int menus_answer_selected(void) {
    const uint8_t *ram = ram_or_null();
    if (!ram || !mentat_up) return -1;
    int y = rw(ram, ANSWER_Y);
    if (y == ANSWER_YES_Y) return 0;
    if (y == ANSWER_NO_Y)  return 1;
    return -1;
}

/* x 32..111 / 120..199 / 208..287 over y 33..151, else -1. */
static int house_at(int px, int py) {
    if (py < 33 || py > 151) return -1;
    for (int i = 0; i < 3; i++)
        if (px >= house_x[i] && px < house_x[i] + 80) return i;
    return -1;
}

static int answer_at(int px, int py) {
    if (px >= 193 && px <= 271 && py >= 170 && py <= 181) return 0;   /* YES */
    if (px >= 193 && px <= 271 && py >= 192 && py <= 205) return 1;   /* NO  */
    return -1;
}

int menus_steer(int px, int py) {
    /* Consume both probes every frame, whatever else happens: a slot read late
       reports a stale screen. */
    int h = probe_take(PROBE_HOUSE_SELECT);
    int m = probe_take(PROBE_MENTAT_ASK);
    int was_house = house_up, was_mentat = mentat_up;
    house_up = h; mentat_up = m;

    /* DB4A_LOG_MENUS=1 reports which screen the probes think is up. Worth
       keeping: the first mentat probe was put on the block that moves the
       selector's sprites, which only runs while the selector is travelling,
       so it read "no screen" precisely when the screen was idle and waiting
       for input. This is what showed that. */
    if (getenv("DB4A_LOG_MENUS")) {
        static int last = -1;
        int now = h ? 1 : (m ? 2 : 0);
        if (now != last) {
            fprintf(stderr, "[menus] screen: %s\n",
                    now == 1 ? "house selection" : now == 2 ? "mentat yes/no" : "none");
            last = now;
        }
    }
    if (!enabled || (!was_house && !was_mentat)) { releasing = 0; return 0; }

    const uint8_t *ram = ram_or_null();
    if (!ram) { release(); return 1; }

    /* Both handlers are edge-triggered, so a press frame needs a release
       frame after it or the direction is only acted on once anyway. */
    if (releasing) { release(); releasing = 0; return 1; }

    if (was_house) {
        /* A direction sent mid-slide is discarded by the handler, so wait for
           the animation rather than throwing presses away. */
        if (rw(ram, HOUSE_SLIDE) != 0) { release(); return 1; }
        int target = house_at(px, py);
        int x = rw(ram, HOUSE_X);
        int cur = -1;
        for (int i = 0; i < 3; i++) if (x == house_x[i]) cur = i;
        if (target < 0 || cur < 0 || target == cur) { release(); return 1; }
        pad_set(target > cur ? PAD_RIGHT : PAD_LEFT, 1);
        releasing = 1;
        return 1;
    }

    /* Mentat YES/NO. */
    int target = answer_at(px, py);
    int y = rw(ram, ANSWER_Y);
    int cur = (y == ANSWER_YES_Y) ? 0 : (y == ANSWER_NO_Y ? 1 : -1);
    if (target < 0 || cur < 0 || target == cur) { release(); return 1; }
    pad_set(target > cur ? PAD_DOWN : PAD_UP, 1);
    releasing = 1;
    return 1;
}
