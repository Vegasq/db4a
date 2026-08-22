#ifndef HAL_H
#define HAL_H
#include <stdint.h>
#include <stddef.h>

void hal_set_rom(const uint8_t *data, size_t len);
void hal_reset_ram(void);
extern unsigned long hal_io_reads, hal_io_writes;

/* Emitted by tools/recomp.py into src/gen/blocks.c */
extern const uint32_t BLOCK_ADDR[];
extern uint32_t (*const BLOCK_FN[])(void);
extern const unsigned BLOCK_COUNT;

uint32_t m68k_run(uint32_t pc, unsigned long max_blocks);
uint32_t m68k_interrupt(uint32_t pc, int level);
uint32_t m68k_run_until(uint32_t pc, uint64_t deadline);
uint32_t m68k_run_frame(uint32_t pc);
int hal_z80_running(void);
extern uint32_t m68k_last_unknown;
const uint8_t *hal_ram_ptr(size_t *len);

/* 68000 cycles in one PAL frame.
 *
 * 313 lines * 3420 master clocks / 7 = 152922.86. The PAL Mega Drive runs at
 * 49.70 Hz, NOT 50: taking the 68000 clock and dividing by 50 gives 152009 and
 * starves the CPU of 914 cycles every frame, about 0.6% less work than real
 * hardware does. Genesis-Plus-GX reports 49.70 fps for this ROM. */
#define PAL_FRAME_CYCLES 152923u
#define PAL_LINES        313u   /* total scanlines, 224 of them visible */
#endif
