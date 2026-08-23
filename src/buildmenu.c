/* Mouse control inside the build console.
 *
 * ---------------------------------------------------------------------------
 * The console
 * ---------------------------------------------------------------------------
 * A 3x6 grid: EXIT/FIX/STOP across the top row, buildable items below.
 *
 *   $FFBF8A  column, word, 0..2
 *   $FFBF8C  row,    word, 0..5
 *   $FFBF8E + row*3 + col   the item in that cell, byte; $80 means empty
 *
 * Cells are 32x24 pixels from origin (32,48), measured by driving the
 * highlight to a known cell and locating its outline. docs/buildmenu.md has
 * the disassembly of the handler and the reproduction commands.
 *
 * The cell table is the useful part: the game states which cells exist, so a
 * hit test never has to guess, and pointing at a gap does nothing rather than
 * doing something surprising.
 *
 * ---------------------------------------------------------------------------
 * Why this presses buttons instead of writing the selection
 * ---------------------------------------------------------------------------
 * The map cursor is warped by writing $FFBF12 directly, so the obvious move is
 * to write $FFBF8A/$FFBF8C the same way. It does not work. The map cursor
 * tolerates a direct write precisely because the ROM recomputes everything
 * downstream from it every frame; here the opposite is true. The code after
 * $84E8 also writes the item code to $FFBFA1, calls $8302 to redraw the
 * preview panel and price, and updates the list scroll. Writing the words
 * alone moves the highlight and leaves the panel describing the previous item.
 *
 * So this synthesises d-pad presses and lets the handler do all of it. The
 * objection that sank presses for the map cursor -- three pixels a frame
 * across a 320-pixel screen -- does not apply to a 3x6 grid: the worst case is
 * five steps. The handler is edge-triggered, so each step costs a press frame
 * and a release frame; ten frames, 200ms, worst case and usually two or three.
 *
 * ---------------------------------------------------------------------------
 * Knowing the console is open
 * ---------------------------------------------------------------------------
 * There is no RAM flag for it. $FFBF86, $FFBF32, $FFBFB4 and $FFC728 all read
 * the same open or closed, and the cell table keeps its contents after the
 * console closes, so it cannot be used as a proxy either. The signal is
 * execution rather than state: the console is open exactly when its input
 * handler at $8462 runs, which the dispatcher sees for free.
 */
#include "buildmenu.h"
#include "probe.h"
#include "input.h"
#include "hal.h"
#include <stdlib.h>

#define MENU_HANDLER  0x8462u    /* the console's d-pad handler */

#define SEL_COL   0xBF8A
#define SEL_ROW   0xBF8C
#define CELLS     0xBF8E
#define EMPTY     0x80

#define COLS   3
#define ROWS   6
#define GRID_X 32
#define GRID_Y 48
#define CELL_W 32
#define CELL_H 24

static int enabled;
static int ran_last_frame;
static int releasing;             /* the handler is edge-triggered: pulse */

void menu_enable(int on) {
    enabled = menu_mouse_wanted() && on;
    probe_watch(PROBE_BUILD_CONSOLE, enabled ? MENU_HANDLER : 0);
}

/* One switch for every menu screen the pointer can drive. */
int menu_mouse_wanted(void) {
    const char *e = getenv("DB4A_MENU_MOUSE");
    return !(e && *e == '0');
}

int buildmenu_open(void) { return enabled && ran_last_frame; }

static const uint8_t *ram_or_null(void) {
    size_t len = 0;
    const uint8_t *ram = hal_ram_ptr(&len);
    return (ram && len >= 0x10000) ? ram : NULL;
}

static int rw(const uint8_t *ram, unsigned a) { return (ram[a] << 8) | ram[a + 1]; }

static int cell_filled(const uint8_t *ram, int r, int c) {
    if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return 0;
    return ram[CELLS + r * COLS + c] != EMPTY;
}

void buildmenu_selection(int *row, int *col) {
    const uint8_t *ram = ram_or_null();
    if (!ram) { if (row) *row = -1; if (col) *col = -1; return; }
    if (col) *col = rw(ram, SEL_COL);
    if (row) *row = rw(ram, SEL_ROW);
}

void buildmenu_cell_at(int px, int py, int *row, int *col) {
    const uint8_t *ram = ram_or_null();
    int c = (px - GRID_X) / CELL_W;
    int r = (py - GRID_Y) / CELL_H;
    if (px < GRID_X || py < GRID_Y || !ram || !cell_filled(ram, r, c)) { r = -1; c = -1; }
    if (row) *row = r;
    if (col) *col = c;
}

static void release(void) {
    pad_set(PAD_LEFT, 0);  pad_set(PAD_RIGHT, 0);
    pad_set(PAD_UP,   0);  pad_set(PAD_DOWN,  0);
}

int buildmenu_steer(int px, int py) {
    int was_open = buildmenu_open();
    ran_last_frame = probe_take(PROBE_BUILD_CONSOLE);
    if (!enabled || !was_open) { releasing = 0; return 0; }

    const uint8_t *ram = ram_or_null();
    if (!ram) { release(); return 1; }

    /* Edge-triggered: a held direction moves one cell and then does nothing,
       so every press frame must be followed by a release frame. */
    if (releasing) { release(); releasing = 0; return 1; }

    int tr, tc;
    buildmenu_cell_at(px, py, &tr, &tc);
    if (tr < 0) { release(); return 1; }          /* not over a live cell */

    int cr = rw(ram, SEL_ROW), cc = rw(ram, SEL_COL);
    int dr = tr - cr, dc = tc - cc;
    if (!dr && !dc) { release(); return 1; }

    int sr = dr > 0 ? 1 : (dr < 0 ? -1 : 0);
    int sc = dc > 0 ? 1 : (dc < 0 ? -1 : 0);

    /* Step along the longer axis first, but only onto a cell that exists --
       the handler refuses to move onto an empty one, so a naive per-axis walk
       gets stuck the moment the grid is ragged, which it usually is. */
    int col_first = (dc < 0 ? -dc : dc) >= (dr < 0 ? -dr : dr);
    int did = 0;
    for (int attempt = 0; attempt < 2 && !did; attempt++) {
        int try_col = col_first ? (attempt == 0) : (attempt == 1);
        if (try_col && sc && cell_filled(ram, cr, cc + sc)) {
            pad_set(sc > 0 ? PAD_RIGHT : PAD_LEFT, 1);
            did = 1;
        } else if (!try_col && sr && cell_filled(ram, cr + sr, cc)) {
            pad_set(sr > 0 ? PAD_DOWN : PAD_UP, 1);
            did = 1;
        }
    }
    if (!did) { release(); return 1; }            /* boxed in; leave it alone */
    releasing = 1;
    return 1;
}
