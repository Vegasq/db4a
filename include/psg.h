#ifndef PSG_H
#define PSG_H
#include <stdint.h>
#include <stddef.h>

/* SN76489 PSG: three square-wave tone channels and one noise channel, each
 * with 4-bit attenuation, driven through a single write-only port. */

void psg_reset(void);
void psg_write(uint8_t v);

/* Advance to `cycles` (68000 cycles, the clock everything else is measured in)
 * and append signed 16-bit mono samples at PSG_RATE. */
void psg_run(uint64_t cycles);

/* Drain generated samples. Returns how many were written. */
size_t psg_read_samples(int16_t *out, size_t max);
size_t psg_available(void);

void psg_report(void);
extern unsigned long psg_writes;

#define PSG_RATE 44100

#endif
