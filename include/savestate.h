#ifndef SAVESTATE_H
#define SAVESTATE_H
#include <stdint.h>

/* Save and restore the whole machine.
 *
 * A modern convenience, not a fidelity feature: the cartridge has no SRAM, so
 * without this a mission has to be played in one sitting. Enabled by default in
 * the SDL frontend (F5 saves, F9 loads); the emulation is unaffected either way.
 */
int savestate_write(const char *path, uint32_t pc, uint32_t frame);
int savestate_read(const char *path, uint32_t *pc, uint32_t *frame);

#endif
