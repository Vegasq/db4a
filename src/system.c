#include "system.h"
#include "mouse.h"
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
    /* Mouse mode owns the cursor's clamp box, and this is the only moment in
     * the frame at which owning it works.
     *
     * MEASURED with DB4A_WATCH=FFBF1E, one gameplay frame:
     *
     *     FFBF1E <- 013C  from block 00706C     ours   (316)
     *     FFBF1E <- 0128  from block 004DA8     the cartridge's (296)
     *
     * so $4DA8 rewrites the box EVERY FRAME, not once per mission as
     * src/mouse.c used to claim, and it runs after the cursor routine at
     * $6DF8 and after the scroll at $706C. Anything written from inside the
     * frame is therefore stale by the time $6DF8 next reads it. Written here,
     * before the VBlank handler that contains all three, it is the value
     * $6DF8 actually sees.
     *
     * That mattered: with the ROM's box in force the cursor stops at x=296,
     * which is exactly where cursor_scroll_band() puts the scroll threshold,
     * so a held arrow key could never produce a non-zero distance past the
     * threshold and the map never scrolled. Task #26. Putting the write here
     * rather than in the frontends also means every path gets it -- SDL,
     * the headless harness, and replay of a recorded session alike.
     *
     * Does nothing unless mouse control is on and the game is in a scene with
     * a cursor, so the faithful path is untouched. */
    mouse_own_clamp_box();
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
