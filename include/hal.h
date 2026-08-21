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

#define PAL_FRAME_CYCLES 152009u
#endif
