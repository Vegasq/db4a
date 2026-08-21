/* PC -> block dispatch.
 *
 * Generated blocks return the next PC, so execution is a flat loop rather than
 * nested calls. A PC with no recompiled block is where the fallback interpreter
 * will go; until it exists we stop and report, so gaps are loud rather than
 * silently wrong. */
#include "m68k.h"
#include "hal.h"
#include <stdio.h>
#include <stdlib.h>

unsigned long m68k_blocks_run;
uint32_t m68k_cur_block;   /* block currently executing, for I/O attribution */
unsigned long m68k_irq_taken, m68k_irq_masked;
uint32_t m68k_last_unknown;

/* Optional per-block execution profile, for finding spin loops. */
unsigned long *m68k_profile;
int m68k_profiling;

void m68k_profile_enable(void) {
    m68k_profile = calloc(BLOCK_COUNT, sizeof *m68k_profile);
    m68k_profiling = m68k_profile != NULL;
}

/* 68000 interrupt entry. Level 6 (VBlank) is the only one this ROM uses.
 *
 * Sequence per the 68000 manual: latch SR, force supervisor mode (switching
 * to the supervisor stack if we were in user mode), raise the interrupt mask
 * to the level being serviced, push PC then SR, and vector. RTE in the
 * handler undoes all of it. */
uint32_t m68k_interrupt(uint32_t pc, int level) {
    if (level <= (int)CPU.imask && level < 7) { m68k_irq_masked++; return pc; }

    uint16_t sr = get_sr();
    if (!CPU.super) {                 /* swap to the supervisor stack */
        CPU.usp = CPU.a[7];
        CPU.a[7] = CPU.ssp;
        CPU.super = true;
    }
    CPU.imask = (uint8_t)level;

    CPU.a[7] -= 4; m68k_write32(CPU.a[7], pc);
    CPU.a[7] -= 2; m68k_write16(CPU.a[7], sr);

    m68k_irq_taken++;
    return m68k_read32(0x60 + (uint32_t)level * 4);   /* autovector 24+level */
}

static int find_block(uint32_t pc) {
    unsigned lo = 0, hi = BLOCK_COUNT;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (BLOCK_ADDR[mid] == pc) return (int)mid;
        if (BLOCK_ADDR[mid] < pc) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* Run until CPU.cycles reaches `deadline`, or a PC has no block. */
uint32_t m68k_run_until(uint32_t pc, uint64_t deadline) {
    m68k_last_unknown = 0;
    while (CPU.cycles < deadline) {
        int i = find_block(pc);
        if (i < 0) { m68k_last_unknown = pc; return pc; }
        if (m68k_profiling) m68k_profile[i]++;
        m68k_cur_block = pc;
        pc = BLOCK_FN[i]();
        m68k_blocks_run++;
    }
    return pc;
}

uint32_t m68k_run(uint32_t pc, unsigned long max_blocks) {
    m68k_blocks_run = 0;
    m68k_last_unknown = 0;
    while (m68k_blocks_run < max_blocks) {
        int i = find_block(pc);
        if (i < 0) {                       /* interpreter fallback goes here */
            m68k_last_unknown = pc;
            return pc;
        }
        if (m68k_profiling) m68k_profile[i]++;
        m68k_cur_block = pc;
        pc = BLOCK_FN[i]();
        m68k_blocks_run++;
    }
    return pc;
}
