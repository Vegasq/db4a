#ifndef Z80_H
#define Z80_H
#include <stdint.h>
#include <stdbool.h>

/* Flag bits in F. Y and X are the undocumented copies of result bits 5 and 3;
   the sound driver is unlikely to depend on them, but they are cheap to keep
   correct and expensive to retrofit. */
#define ZF_S 0x80
#define ZF_Z 0x40
#define ZF_Y 0x20
#define ZF_H 0x10
#define ZF_X 0x08
#define ZF_P 0x04
#define ZF_N 0x02
#define ZF_C 0x01

typedef struct {
    uint8_t  a, f, b, c, d, e, h, l;
    uint8_t  a_, f_, b_, c_, d_, e_, h_, l_;   /* shadow set */
    uint16_t ix, iy, sp, pc;
    uint8_t  i, r;
    bool     iff1, iff2, halted;
    uint8_t  im;
    uint64_t cycles;
} z80_t;

extern z80_t Z80;

void     z80_reset(void);
unsigned z80_step(void);              /* execute one instruction, return cycles */
void     z80_run(uint64_t until);     /* run until Z80.cycles >= until */
void     z80_irq(void);               /* raise INT (the VDP drives this at VBlank) */

/* Bus, implemented in hal_z80.c */
uint8_t  z80_read(uint16_t addr);
void     z80_write(uint16_t addr, uint8_t v);
uint8_t  z80_in(uint16_t port);
void     z80_out(uint16_t port, uint8_t v);
#endif
