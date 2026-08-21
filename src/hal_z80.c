/* Z80 sub-system: RAM, bus arbitration and the Z80's view of the bus.
 *
 * From the 68000 side:
 *   $A00000-$A0FFFF  Z80 RAM (8 KiB, mirrored)
 *   $A11100          BUSREQ  -- write 0x0100 to request, 0 to release;
 *                              reading bit 0 returns 0 once the bus is granted
 *   $A11200          RESET   -- write 0 to assert reset, 0x0100 to release
 *
 * From the Z80 side:
 *   $0000-$1FFF  RAM        $2000-$3FFF  mirror
 *   $4000-$4003  YM2612     $6000  bank register    $7F00-$7FFF  VDP
 *   $8000-$FFFF  32 KiB window into the 68000 address space, selected by the
 *                bank register one bit at a time (9 writes to set a bank)
 */
#include "z80.h"
#include "m68k.h"
#include <stdio.h>
#include <string.h>

static uint8_t  z80ram[0x2000];
static uint16_t bank;              /* 9-bit, shifted in one bit per write */
static int      bus_granted = 1;   /* 68000 holds the bus until the Z80 runs */
static int      z80_reset_held = 1;

unsigned long hal_z80_writes, hal_z80_cycles;

void hal_z80_init(void) {
    memset(z80ram, 0, sizeof z80ram);
    bank = 0; bus_granted = 1; z80_reset_held = 1;
    z80_reset();
}

int  hal_z80_running(void) { return !bus_granted && !z80_reset_held; }
void hal_dump_z80(const char *path);

/* --- 68000 side --- */
uint8_t hal_z80_read68k(uint32_t a) {
    if ((a & 0xFF0000) == 0xA00000) return z80ram[a & 0x1FFF];
    if ((a & 0xFFFF00) == 0xA11100) return (uint8_t)(bus_granted ? 0x00 : 0x01);
    return 0;
}
void hal_z80_write68k(uint32_t a, uint8_t v) {
    if ((a & 0xFF0000) == 0xA00000) { z80ram[a & 0x1FFF] = v; hal_z80_writes++; return; }
    /* BUSREQ and RESET are word registers whose only meaningful bit is bit 8,
       i.e. bit 0 of the EVEN byte. The ROM writes them with move.w #$0100,
       which decomposes into 0x01 at the even address and 0x00 at the odd one;
       acting on the odd half immediately undoes the request. */
    if (a & 1) return;
    if ((a & 0xFFFF00) == 0xA11100) { bus_granted = (v & 1) ? 1 : 0; return; }
    if ((a & 0xFFFF00) == 0xA11200) {
        z80_reset_held = (v & 1) ? 0 : 1;
        if (z80_reset_held) {
            z80_reset();
        }
        return;
    }
}

/* --- Z80 side --- */
uint8_t z80_read(uint16_t a) {
    if (a < 0x4000) return z80ram[a & 0x1FFF];
    if (a < 0x6000) return 0;                       /* YM2612 status: not busy */
    if (a < 0x8000) return 0;
    /* Banked window into the 68000 bus. */
    uint32_t phys = ((uint32_t)bank << 15) | (a & 0x7FFF);
    return m68k_read8(phys);
}

void z80_write(uint16_t a, uint8_t v) {
    if (a < 0x4000) { z80ram[a & 0x1FFF] = v; return; }
    if (a < 0x6000) return;                          /* YM2612 -- silent for now */
    if (a < 0x6100) {                                /* bank register */
        bank = (uint16_t)(((bank >> 1) | ((v & 1) << 8)) & 0x1FF);
        return;
    }
    if (a < 0x8000) return;                          /* PSG */
    uint32_t phys = ((uint32_t)bank << 15) | (a & 0x7FFF);
    m68k_write8(phys, v);
}

uint8_t z80_in(uint16_t port)            { (void)port; return 0xFF; }
void    z80_out(uint16_t port, uint8_t v){ (void)port; (void)v; }

void hal_dump_z80(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(z80ram, 1, sizeof z80ram, f);
    fclose(f);
}
