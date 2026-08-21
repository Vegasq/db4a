/* Minimal memory + trap stubs, enough to compile and link the generated
   blocks before the real HAL exists. Replaced by hal_mem.c / hal_vdp.c. */
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>

m68k_t CPU;
static uint8_t RAM[0x10000];
extern const unsigned char *ROM_DATA;
static const unsigned char *rom;
static size_t rom_len;

void hal_set_rom(const unsigned char *d, size_t n) { rom = d; rom_len = n; }

uint8_t m68k_read8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < rom_len && rom) return rom[a];
    if (a >= 0xFF0000)      return RAM[a & 0xFFFF];
    return 0;
}
uint16_t m68k_read16(uint32_t a){ return (uint16_t)((m68k_read8(a)<<8)|m68k_read8(a+1)); }
uint32_t m68k_read32(uint32_t a){ return ((uint32_t)m68k_read16(a)<<16)|m68k_read16(a+2); }
void m68k_write8 (uint32_t a, uint8_t v){ a&=0xFFFFFF; if(a>=0xFF0000) RAM[a&0xFFFF]=v; }
void m68k_write16(uint32_t a, uint16_t v){ m68k_write8(a,(uint8_t)(v>>8)); m68k_write8(a+1,(uint8_t)v); }
void m68k_write32(uint32_t a, uint32_t v){ m68k_write16(a,(uint16_t)(v>>16)); m68k_write16(a+2,(uint16_t)v); }

void set_sr(uint16_t v) {
    set_ccr((uint8_t)(v & 0xFF));
    CPU.imask = (v >> 8) & 7;
    bool sup = (v >> 13) & 1;
    if (sup != CPU.super) {            /* swap stack pointers on mode change */
        if (CPU.super) { CPU.ssp = CPU.a[7]; CPU.a[7] = CPU.usp; }
        else           { CPU.usp = CPU.a[7]; CPU.a[7] = CPU.ssp; }
        CPU.super = sup;
    }
}
void m68k_div_by_zero(void){ fprintf(stderr, "division by zero\n"); }
void m68k_illegal(uint32_t pc){ fprintf(stderr, "illegal instruction at %06X\n", pc); }
void m68k_unimplemented(uint32_t pc){ fprintf(stderr, "unimplemented at %06X\n", pc); }
