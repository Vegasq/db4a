#ifndef SYSTEM_H
#define SYSTEM_H
#include <stdint.h>
#include <stddef.h>

/* One place that knows how to reset and step the machine.
 *
 * This exists because the headless and SDL frontends drifted: Z80 support was
 * added to one and not the other, so the interactive build ran the 68000 with
 * a dead Z80 and crashed where the batch build did not. Both frontends now
 * call the same two functions, so that divergence cannot recur. */

uint32_t system_reset(const uint8_t *rom, size_t len);
uint32_t system_frame(uint32_t pc);      /* one PAL frame + VBlank */
#endif
