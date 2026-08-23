#ifndef BUILDMENU_H
#define BUILDMENU_H
#include <stdint.h>

/* Mouse control inside the build console.
 *
 * The console is the screen you get from a Construction Yard: EXIT/FIX/STOP
 * along the top and the buildable items below. It is a 3x6 grid navigated with
 * the d-pad. docs/buildmenu.md has the full model and how it was measured.
 *
 * Off unless mouse control is on; DB4A_MENU_MOUSE=0 disables it separately,
 * leaving the rest of mouse control alone.
 */

/* Called by the dispatcher when the console's input handler executes. That is
 * the open/closed signal -- no RAM flag distinguishes the two states. */
void menu_handler_ran(void);

/* The PC to watch for, or 0 when the feature is off. Read once per block by
 * the dispatch loop, so it is a plain global rather than a call. */
extern uint32_t menu_probe_pc;

void menu_enable(int on);

/* True if the console took input on the frame just simulated. */
int  buildmenu_open(void);

/* Point at a cell and the highlight walks to it. Call once per frame with the
 * pointer in GAME pixels, before stepping the emulation.
 *
 * Returns 1 if the console is open, in which case the caller must NOT also run
 * mouse_steer() -- the two would fight over the d-pad and over the cursor
 * variables. Returns 0 when the console is closed and the map cursor should be
 * driven as usual. */
int  buildmenu_steer(int px, int py);

/* Which cell the pointer is over, -1 if none or if the cell is empty. For
 * tests and diagnostics. */
void buildmenu_cell_at(int px, int py, int *row, int *col);

/* Where the highlight currently is. */
void buildmenu_selection(int *row, int *col);

#endif
