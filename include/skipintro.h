#ifndef SKIPINTRO_H
#define SKIPINTRO_H
#include <stdint.h>

/* Start skips the opening sequence and lands on the title menu.
 *
 * The cartridge takes about 44 seconds to get from reset to START GAME, and
 * pressing anything during it does nothing at all -- there is no skip on the
 * hardware. This adds one.
 *
 * It is a FAST-FORWARD, not a jump: the skipped frames are really simulated,
 * just without rendering, audio or pacing, so the machine arrives in exactly
 * the state it would have reached by waiting. Nothing is poked into RAM and
 * no cartridge code is bypassed, which is why an input recording made across
 * a skip still replays frame for frame in the headless harness.
 *
 * Turn it off with `skipintro = 0` in db4a.conf, or DB4A_SKIPINTRO=0.
 */

/* Claim the probe slot that spots the title menu. Call once at startup. */
void skipintro_enable(int on);

/* True while a skip would do something: enabled, and the title menu has not
 * been reached yet. The frontend uses this to decide whether Start belongs to
 * the skip or to the game. */
int  skipintro_armed(void);

/* Start was pressed. The skip runs at the next skipintro_step(). */
void skipintro_request(void);

/* Call once per frame, BEFORE stepping the machine. Consumes the title-menu
 * probe -- so nothing else may -- and, if a skip is pending, runs frames until
 * the menu is up. Returns the new PC and advances *frames by however many were
 * simulated. */
uint32_t skipintro_step(uint32_t pc, unsigned *frames);

#endif
