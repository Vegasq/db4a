#include "system.h"
#include "m68k.h"
#include "hal.h"
#include "vdp.h"
#include "z80.h"
#include "invariant.h"

void hal_z80_init(void);

uint32_t system_reset(const uint8_t *rom, size_t len) {
    hal_set_rom(rom, len);
    hal_reset_ram();
    vdp_reset();
    hal_z80_init();
    invariant_init();

    CPU.a[7]  = m68k_read32(0);
    CPU.ssp   = CPU.a[7];
    CPU.super = true;
    CPU.imask = 7;
    return m68k_read32(4);
}

uint32_t system_frame(uint32_t pc) {
    pc = m68k_run_frame(pc);              /* interleaves the Z80 */
    if (m68k_last_unknown) return pc;
    if (hal_z80_running()) z80_irq();
    return m68k_interrupt(pc, 6);         /* VBlank */
}
