#include "system.h"
#include "m68k.h"
#include "hal.h"
#include "vdp.h"
#include "psg.h"
#include "ym2612.h"
#include "z80.h"
#include "invariant.h"

void hal_z80_init(void);

uint32_t system_reset(const uint8_t *rom, size_t len) {
    hal_set_rom(rom, len);
    hal_reset_ram();
    vdp_reset();
    psg_reset();
    ym_reset();
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
    /* Assert the Z80 INT line for one scanline, the way the VDP does, rather
       than delivering a single edge that is lost if the driver happens to have
       interrupts disabled at this instant. */
    if (hal_z80_running()) {
        uint64_t line = (uint64_t)PAL_FRAME_CYCLES / PAL_LINES;
        z80_irq_assert(Z80.cycles + line * Z80_NUM / Z80_DEN);
    }
    return m68k_interrupt(pc, 6);         /* VBlank */
}
