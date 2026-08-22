/* Z80 instruction timing.
 *
 * zexdoc proves the core computes the right RESULTS; it says nothing about how
 * long each instruction takes. That gap mattered: every non-indexed CB op was
 * 4 T-states heavy (bit 0,(hl) cost 16 instead of 12), and Dune's sound driver
 * is CPU-bound on the Z80 and spins on exactly that instruction, so the error
 * slowed the music. Correct results, wrong tempo, and nothing in the test
 * suite could see it.
 */
#include "z80.h"
#include <stdio.h>
#include <string.h>
static uint8_t MEM[0x10000];
uint8_t z80_read(uint16_t a) { return MEM[a]; }
void z80_write(uint16_t a, uint8_t v) { MEM[a] = v; }
uint8_t z80_in(uint16_t p) { (void)p; return 0xFF; }
void z80_out(uint16_t p, uint8_t v) { (void)p; (void)v; }
static unsigned one(const uint8_t *code, unsigned n, const char *name, unsigned want) {
    memset(MEM, 0, sizeof MEM);
    memcpy(MEM + 0x100, code, n);
    z80_reset();
    Z80.pc = 0x100; Z80.cycles = 0;
    Z80.h = 0x20; Z80.l = 0x00;
    unsigned c = z80_step();
    printf("   %-16s got %2u  want %2u  %s\n", name, c, want, c == want ? "ok" : "MISMATCH");
    return c == want;
}
int main(void) {
    printf("-- Z80 instruction timing (T-states)\n");
    int ok = 1;
    ok &= one((const uint8_t[]){0xCB,0x46}, 2, "bit 0,(hl)", 12);
    ok &= one((const uint8_t[]){0xCA,0x00,0x01}, 3, "jp z,nn", 10);
    ok &= one((const uint8_t[]){0xC3,0x00,0x01}, 3, "jp nn", 10);
    ok &= one((const uint8_t[]){0x36,0x27}, 2, "ld (hl),n", 10);
    ok &= one((const uint8_t[]){0x7E}, 1, "ld a,(hl)", 7);
    ok &= one((const uint8_t[]){0x00}, 1, "nop", 4);
    ok &= one((const uint8_t[]){0x3A,0x00,0x20}, 3, "ld a,(nn)", 13);
    ok &= one((const uint8_t[]){0xCB,0x40}, 2, "bit 0,b", 8);
    ok &= one((const uint8_t[]){0x18,0x00}, 2, "jr d", 12);
    ok &= one((const uint8_t[]){0xD9}, 1, "exx", 4);
    ok &= one((const uint8_t[]){0x08}, 1, "ex af,af'", 4);
    printf(ok ? "\nall timings correct\n" : "\nTIMING MISMATCHES\n");
    return !ok;
}
