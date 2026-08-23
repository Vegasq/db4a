#ifndef YM2612_H
#define YM2612_H
#include <stdint.h>
#include <stddef.h>

/* YM2612 (OPN2): six four-operator FM channels, channel 6 switchable to an
 * 8-bit DAC. Registers are reached through two banks of address/data pairs. */

void ym_reset(void);
void ym_write(unsigned port, uint8_t v);   /* port 0-3 = A0/D0/A1/D1 */
uint8_t ym_read_status(void);

void   ym_run(uint64_t cycles);            /* advance to a 68000 cycle count */
size_t ym_read_samples(int16_t *out, size_t max);   /* interleaved stereo */
size_t ym_available(void);                 /* stereo frames pending */
void   ym_report(void);

extern unsigned long ym_writes, ym_keyons;
unsigned ym_timer_ctrl(void);
unsigned ym_timer_a_period(void);

#endif
