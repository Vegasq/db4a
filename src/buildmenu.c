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
#include "m68k.h"
#include <stdlib.h>
#include <stdio.h>

/* Two screens share this code, because they are the same widget with
 * different data behind it. The Starport is the Construction Yard's console
 * with vehicles instead of buildings: same chrome, same 32x24 cells at the
 * same origin, same EXIT/FIX/STOP across the top, same handler shape reading
 * a (column,row) pair and refusing to move onto an empty cell.
 *
 * What differs is only what is in the table below -- and two values in it
 * would be easy to assume and wrong. The Starport's grid is FOUR rows, not
 * six (`cmpi.w #$4,d1` at $91CA against `#$6` at $84C0), and its empty marker
 * is $FF, not $80 (`cmpi.b #$ff,d3` at $91EA against `#$80` at $84E0).
 *
 * Grid geometry is shared and was measured, not assumed: driving the highlight
 * one cell right moves the changed pixels from x 32..63 to 64..95, and one
 * cell down from y 48..71 to 72..95, on both screens.
 */
struct console {
    uint32_t handler;      /* d-pad handler; its running IS the screen being up */
    unsigned probe;
    unsigned sel_col, sel_row, cells;
    uint8_t  empty;
    int      rows;
};

static const struct console CONSOLES[] = {
    /* build console */
    { 0x8462u, PROBE_BUILD_CONSOLE, 0xBF8A, 0xBF8C, 0xBF8E, 0x80, 6 },
    /* Starport */
    { 0x916Cu, PROBE_STARPORT,      0xBFC8, 0xBFCA, 0xBFCC, 0xFF, 4 },
};
#define NCONSOLES (sizeof CONSOLES / sizeof CONSOLES[0])

#define COLS   3
#define GRID_X 32
#define GRID_Y 48
#define CELL_W 32
#define CELL_H 24

static int enabled;
static const struct console *open_console;   /* NULL when none is up */
static int releasing;             /* the handler is edge-triggered: pulse */

void menu_enable(int on) {
    enabled = menu_mouse_wanted() && on;
    for (unsigned i = 0; i < NCONSOLES; i++)
        probe_watch(CONSOLES[i].probe, enabled ? CONSOLES[i].handler : 0);
}

/* One switch for every menu screen the pointer can drive. */
int menu_mouse_wanted(void) {
    const char *e = getenv("DB4A_MENU_MOUSE");
    return !(e && *e == '0');
}

int buildmenu_open(void) { return enabled && open_console != NULL; }

static const uint8_t *ram_or_null(void) {
    size_t len = 0;
    const uint8_t *ram = hal_ram_ptr(&len);
    return (ram && len >= 0x10000) ? ram : NULL;
}

static int rw(const uint8_t *ram, unsigned a) { return (ram[a] << 8) | ram[a + 1]; }

static int cell_filled(const struct console *k, const uint8_t *ram, int r, int c) {
    if (r < 0 || r >= k->rows || c < 0 || c >= COLS) return 0;
    return ram[k->cells + r * COLS + c] != k->empty;
}

void buildmenu_selection(int *row, int *col) {
    const uint8_t *ram = ram_or_null();
    const struct console *k = open_console;
    if (!ram || !k) { if (row) *row = -1; if (col) *col = -1; return; }
    if (col) *col = rw(ram, k->sel_col);
    if (row) *row = rw(ram, k->sel_row);
}

void buildmenu_cell_at(int px, int py, int *row, int *col) {
    const uint8_t *ram = ram_or_null();
    const struct console *k = open_console;
    int c = (px - GRID_X) / CELL_W;
    int r = (py - GRID_Y) / CELL_H;
    if (px < GRID_X || py < GRID_Y || !ram || !k || !cell_filled(k, ram, r, c)) { r = -1; c = -1; }
    if (row) *row = r;
    if (col) *col = c;
}

static void release(void) {
    pad_set(PAD_LEFT, 0);  pad_set(PAD_RIGHT, 0);
    pad_set(PAD_UP,   0);  pad_set(PAD_DOWN,  0);
}

int buildmenu_steer(int px, int py) {
    /* Consume every console's probe each frame, whichever is up: a slot read
       late reports a stale screen. Only one can be open at a time.
     *
     * The handler does NOT run on every frame -- the Starport's alternates,
     * giving starport/none/starport/none -- so a console is held open for a
     * few frames after its handler was last seen rather than being dropped the
     * first frame it is missing. Without this the steering sees a closed
     * screen every other frame, never completes its press/release pulse, and
     * the highlight does not move at all, which is exactly how this first
     * behaved. The grace is short so that leaving the screen still stops the
     * steering promptly. */
    const struct console *was = open_console;
    static int grace;
    int seen = 0;
    for (unsigned i = 0; i < NCONSOLES; i++)
        if (probe_take(CONSOLES[i].probe)) { open_console = &CONSOLES[i]; seen = 1; }
    if (seen) grace = 4;
    else if (grace > 0 && --grace == 0) open_console = NULL;
    if (getenv("DB4A_LOG_CONSOLE")) {
        static const struct console *last = (const struct console *)1;
        if (open_console != last) {
            last = open_console;
            fprintf(stderr, "[console] %s\n",
                    open_console ? (open_console->probe == PROBE_STARPORT
                                    ? "starport" : "build console") : "none");
        }
    }
    if (!enabled || !was) { releasing = 0; return 0; }
    const struct console *k = was;

    const uint8_t *ram = ram_or_null();
    if (!ram) { release(); return 1; }

    /* Edge-triggered: a held direction moves one cell and then does nothing,
       so every press frame must be followed by a release frame. */
    if (releasing) { release(); releasing = 0; return 1; }

    int tr, tc;
    buildmenu_cell_at(px, py, &tr, &tc);
    if (tr < 0) { release(); return 1; }          /* not over a live cell */

    int cr = rw(ram, k->sel_row), cc = rw(ram, k->sel_col);
    int dr = tr - cr, dc = tc - cc;
    if (!dr && !dc) { release(); return 1; }

    int sr = dr > 0 ? 1 : (dr < 0 ? -1 : 0);
    int sc = dc > 0 ? 1 : (dc < 0 ? -1 : 0);

    /* Arrive in ONE step, by placing the selection on a neighbour of the
       target and pressing towards it.
     *
     * Stepping cell by cell means up to seven presses, and each press needs a
     * release frame after it because the handler is edge-triggered -- so the
     * highlight visibly crawls after the pointer. Writing the target straight
     * into the row and column words is no good either: the game moves its
     * internal state but nothing redraws, leaving the highlight painted on the
     * old cell and the preview panel blank. (Measured: warping to a vehicle
     * leaves the highlight on EXIT with empty price bars, 5951 pixels away
     * from where stepping lands.)
     *
     * Doing both fixes both. The write is invisible because it is immediately
     * followed by a real press, and the press runs the game's own handler,
     * which redraws the panel, the price and the highlight exactly as it does
     * for a d-pad move.
     *
     * The neighbour must be a cell that exists, or the handler refuses the
     * move and nothing happens. Horizontal is tried first: rows are only three
     * wide and the top row of buttons is always full, so a horizontal
     * neighbour almost always exists. */
    static const struct { int dr, dc; int pad; } APPROACH[] = {
        {  0, -1, PAD_RIGHT }, {  0, +1, PAD_LEFT },
        { -1,  0, PAD_DOWN  }, { +1,  0, PAD_UP   },
    };
    for (unsigned i = 0; i < sizeof APPROACH / sizeof APPROACH[0]; i++) {
        int nr = tr + APPROACH[i].dr, nc = tc + APPROACH[i].dc;
        if (!cell_filled(k, ram, nr, nc)) continue;
        if (nr == cr && nc == cc) {               /* already adjacent */
            pad_set(APPROACH[i].pad, 1);
            releasing = 1;
            return 1;
        }
        m68k_write16(0xFF0000u + k->sel_col, (uint16_t)nc);
        m68k_write16(0xFF0000u + k->sel_row, (uint16_t)nr);
        pad_set(APPROACH[i].pad, 1);
        releasing = 1;
        return 1;
    }

    /* An isolated cell with no filled neighbour: fall back to stepping, which
       cannot reach it either, but at least does nothing surprising. */
    int col_first = (dc < 0 ? -dc : dc) >= (dr < 0 ? -dr : dr);
    int did = 0;
    for (int attempt = 0; attempt < 2 && !did; attempt++) {
        int try_col = col_first ? (attempt == 0) : (attempt == 1);
        if (try_col && sc && cell_filled(k, ram, cr, cc + sc)) {
            pad_set(sc > 0 ? PAD_RIGHT : PAD_LEFT, 1);
            did = 1;
        } else if (!try_col && sr && cell_filled(k, ram, cr + sr, cc)) {
            pad_set(sr > 0 ? PAD_DOWN : PAD_UP, 1);
            did = 1;
        }
    }
    if (!did) { release(); return 1; }            /* boxed in; leave it alone */
    releasing = 1;
    return 1;
}
