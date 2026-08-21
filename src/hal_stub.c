/* Minimal memory + trap stubs: enough to execute the generated blocks before
   the real HAL exists. Hardware registers are logged, not emulated, so we can
   see what the ROM asks for. Replaced by hal_mem.c / hal_vdp.c at M2. */
#include "m68k.h"
#include "hal.h"
#include <stdio.h>
#include <string.h>

m68k_t CPU;

static const uint8_t *rom;
static size_t rom_len;
static uint8_t ram[0x10000];        /* $FF0000-$FFFFFF, mirrored */

unsigned long hal_io_reads, hal_io_writes;

void hal_set_rom(const uint8_t *d, size_t n) { rom = d; rom_len = n; }
void hal_reset_ram(void) { memset(ram, 0, sizeof ram); }

/* $A00000-$A1FFFF  Z80 / controller / version registers
 * $C00000-$C0001F  VDP data, control and HV counter
 * Reads return 0 for now, except where a zero would deadlock the ROM. */
static uint16_t io_read16(uint32_t a) {
    hal_io_reads++;
    if ((a & 0xFFFFFF) == 0xC00004 || (a & 0xFFFFFF) == 0xC00006) {
        /* VDP status: report "not busy, not in DMA" so the ROM's wait loops
           terminate instead of spinning forever against a constant 0. */
        return 0x3400;
    }
    return 0;
}

uint8_t m68k_read8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < rom_len)   return rom[a];
    if (a >= 0xFF0000) return ram[a & 0xFFFF];
    if (a >= 0xA00000) return (uint8_t)(io_read16(a) >> ((a & 1) ? 0 : 8));
    return 0;
}
uint16_t m68k_read16(uint32_t a) {
    a &= 0xFFFFFF;
    if (a + 1 < rom_len) return (uint16_t)((rom[a] << 8) | rom[a + 1]);
    if (a >= 0xFF0000)   return (uint16_t)((ram[a & 0xFFFF] << 8) | ram[(a + 1) & 0xFFFF]);
    if (a >= 0xA00000)   return io_read16(a);
    return 0;
}
uint32_t m68k_read32(uint32_t a) {
    return ((uint32_t)m68k_read16(a) << 16) | m68k_read16(a + 2);
}

void m68k_write8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xFF0000) { ram[a & 0xFFFF] = v; return; }
    if (a >= 0xA00000) { hal_io_writes++; return; }
}
void m68k_write16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xFF0000) { ram[a & 0xFFFF] = (uint8_t)(v >> 8);
                         ram[(a + 1) & 0xFFFF] = (uint8_t)v; return; }
    if (a >= 0xA00000) { hal_io_writes++; return; }
}
void m68k_write32(uint32_t a, uint32_t v) {
    m68k_write16(a, (uint16_t)(v >> 16));
    m68k_write16(a + 2, (uint16_t)v);
}

unsigned long hal_sr_writes;
int hal_log_sr;
void set_sr(uint16_t v) {
    if (hal_log_sr && hal_sr_writes < 40)
        fprintf(stderr, "[sr] #%lu  <- %04X  (imask %u -> %u)\n",
                hal_sr_writes, v, CPU.imask, (v >> 8) & 7);
    hal_sr_writes++;
    set_ccr((uint8_t)(v & 0xFF));
    CPU.imask = (v >> 8) & 7;
    bool sup = (v >> 13) & 1;
    if (sup != CPU.super) {          /* stack pointers swap on a mode change */
        if (CPU.super) { CPU.ssp = CPU.a[7]; CPU.a[7] = CPU.usp; }
        else           { CPU.usp = CPU.a[7]; CPU.a[7] = CPU.ssp; }
        CPU.super = sup;
    }
}

void m68k_div_by_zero(void)          { fprintf(stderr, "[trap] divide by zero\n"); }
void m68k_illegal(uint32_t pc)       { fprintf(stderr, "[trap] illegal @ %06X\n", pc); }
void m68k_unimplemented(uint32_t pc) { fprintf(stderr, "[trap] unimplemented @ %06X\n", pc); }
