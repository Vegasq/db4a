/* Minimal memory + trap stubs: enough to execute the generated blocks before
   the real HAL exists. Hardware registers are logged, not emulated, so we can
   see what the ROM asks for. Replaced by hal_mem.c / hal_vdp.c at M2. */
#include "m68k.h"
#include "hal.h"
#include "vdp.h"
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

m68k_t CPU;

static const uint8_t *rom;
static size_t rom_len;
static uint8_t ram[0x10000];        /* $FF0000-$FFFFFF, mirrored */
uint8_t hal_z80_read68k(uint32_t a);
void    hal_z80_write68k(uint32_t a, uint8_t v);

static int is_z80(uint32_t a) {
    return (a & 0xFF0000) == 0xA00000
        || (a & 0xFFFF00) == 0xA11100
        || (a & 0xFFFF00) == 0xA11200;
}

unsigned long hal_io_reads, hal_io_writes;
extern uint32_t m68k_cur_block;
int hal_log_io;

void hal_set_rom(const uint8_t *d, size_t n) { rom = d; rom_len = n; }
void hal_reset_ram(void) { memset(ram, 0, sizeof ram); }
const uint8_t *hal_ram_ptr(size_t *len) { if (len) *len = sizeof ram; return ram; }

/* Hardware reads.
 *
 * $A10001  version register. Bit 7 export (non-Japanese), bit 6 PAL,
 *          bit 5 no Mega CD attached, bits 3-0 hardware revision.
 *          This matters: the boot code at $2840 tests bit 6 and only the
 *          PAL branch reaches `move.w #$2000, sr` at $285A, which is the
 *          sole instruction that unmasks interrupts. Reporting NTSC here
 *          leaves VBlank masked forever and the game never starts.
 *          This is a region E (PAL Europe) cartridge, so report a PAL
 *          export console.
 * $A11100  Z80 bus request. Bit 0 set means the Z80 still holds the bus;
 *          report it free so the ROM's wait loops terminate.
 * $C00004  VDP status. Report "not busy, not in DMA".
 */
#define VERSION_REG 0xE0

/* Histogram of hardware addresses touched, so we can see what the ROM polls. */
#define IOHIST 32
static struct { uint32_t addr; unsigned long n; } io_hist[IOHIST];
static void io_tally(uint32_t a, int write) {
    uint32_t key = a | (write ? 0x1000000u : 0u);
    for (int i = 0; i < IOHIST; i++) {
        if (io_hist[i].n == 0) { io_hist[i].addr = key; io_hist[i].n = 1; return; }
        if (io_hist[i].addr == key) { io_hist[i].n++; return; }
    }
}
void hal_io_report(void) {
    printf("\n--- hardware access histogram ---\n");
    for (int i = 0; i < IOHIST && io_hist[i].n; i++)
        printf("  %s %06X : %lu\n", (io_hist[i].addr & 0x1000000) ? "W" : "R",
               io_hist[i].addr & 0xFFFFFF, io_hist[i].n);
}

static uint8_t io_read8(uint32_t a) {
    hal_io_reads++;
    io_tally(a, 0);
    if (is_z80(a)) return hal_z80_read68k(a);
    uint8_t v = 0;
    switch (a) {
    case 0xA10001: v = VERSION_REG; break;

    case 0xA10003: v = pad_read_data(0); break;
    case 0xA10005: v = pad_read_data(1); break;
    case 0xC00004: v = (uint8_t)(vdp_read_status() >> 8); break;
    case 0xC00005: v = (uint8_t)(vdp_read_status() & 0xFF); break;
    case 0xC00006: v = (uint8_t)(vdp_read_status() >> 8); break;
    case 0xC00007: v = (uint8_t)(vdp_read_status() & 0xFF); break;
    /* Byte access to the data port is rare; read the word once and pick a half. */
    case 0xC00000: case 0xC00002: v = (uint8_t)(vdp_read_data() >> 8); break;
    case 0xC00001: case 0xC00003: v = (uint8_t)(vdp_read_data() & 0xFF); break;
    default:       v = 0; break;
    }
    if (hal_log_io)
        fprintf(stderr, "[io ] blk %06X  read  %06X -> %02X\n", m68k_cur_block, a, v);
    return v;
}

uint8_t m68k_read8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < rom_len)   return rom[a];
    if (a >= 0xFF0000) return ram[a & 0xFFFF];
    if (a >= 0xA00000) return io_read8(a);
    return 0;
}
uint16_t m68k_read16(uint32_t a) {
    a &= 0xFFFFFF;
    if (a + 1 < rom_len) return (uint16_t)((rom[a] << 8) | rom[a + 1]);
    /* The VDP data port is a 16-bit register with a side effect: every read
       auto-increments the address. Servicing a word read as two byte reads
       consumes TWO entries and splices halves of different ones together, so
       it must be a single access. */
    if ((a & 0xFFFFE0) == 0xC00000 && (a & 0x1F) < 4) {
        hal_io_reads++;
        return vdp_read_data();
    }
    if (is_z80(a)) return (uint16_t)((hal_z80_read68k(a) << 8) | hal_z80_read68k(a + 1));
    if (a >= 0xFF0000)   return (uint16_t)((ram[a & 0xFFFF] << 8) | ram[(a + 1) & 0xFFFF]);
    if (a >= 0xA00000)   return (uint16_t)((io_read8(a) << 8) | io_read8(a + 1));
    return 0;
}
uint32_t m68k_read32(uint32_t a) {
    return ((uint32_t)m68k_read16(a) << 16) | m68k_read16(a + 2);
}

void m68k_write8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xFF0000) { ram[a & 0xFFFF] = v; return; }
    if ((a & 0xFFFFE0) == 0xC00000) {
        /* A byte write to a VDP port duplicates the byte into both halves. */
        m68k_write16(a & ~1u, (uint16_t)((v << 8) | v));
        return;
    }
    if (is_z80(a)) { hal_z80_write68k(a, v); return; }
    switch (a) {
    case 0xA10003: pad_write_data(0, v); hal_io_writes++; io_tally(a,1); return;
    case 0xA10005: pad_write_data(1, v); hal_io_writes++; return;
    case 0xA10009: pad_write_ctrl(0, v); hal_io_writes++; io_tally(a,1); return;
    case 0xA1000B: pad_write_ctrl(1, v); hal_io_writes++; return;
    default: break;
    }
    if (a >= 0xA00000) { hal_io_writes++; return; }
}
void m68k_write16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xFF0000) { ram[a & 0xFFFF] = (uint8_t)(v >> 8);
                         ram[(a + 1) & 0xFFFF] = (uint8_t)v; return; }
    /* BUSREQ and RESET are written as words (move.w #$0100,$A11100), so they
       must be routed here as well as in the byte path. */
    if (is_z80(a)) {
        hal_z80_write68k(a,     (uint8_t)(v >> 8));
        hal_z80_write68k(a + 1, (uint8_t)v);
        return;
    }
    if ((a & 0xFFFFE0) == 0xC00000) {
        hal_io_writes++;
        uint32_t p = a & 0x1F;
        if      (p < 4) vdp_write_data(v);
        else if (p < 8) vdp_write_control(v);
        return;
    }
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
