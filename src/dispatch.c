/* PC -> block dispatch.
 *
 * Generated blocks return the next PC, so execution is a flat loop rather than
 * nested calls. A PC with no recompiled block is where the fallback interpreter
 * will go; until it exists we stop and report, so gaps are loud rather than
 * silently wrong. */
#include "m68k.h"
#include "hal.h"
#include "z80.h"
#include "invariant.h"
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

/* Ring buffer of recently executed blocks. When execution reaches a PC with no
   block -- almost always a jump through a corrupted pointer -- the trail of
   blocks that led there is far more useful than the faulting address alone. */
#define TRAIL 64
static uint32_t trail[TRAIL];
static unsigned trail_n;

static void trail_push(uint32_t pc) { trail[trail_n++ % TRAIL] = pc; }

void m68k_dump_crash(uint32_t pc) {
    fprintf(stderr, "\n=== BAD PC %08X ===\n", pc);
    fprintf(stderr, "D0-D7 %08X %08X %08X %08X %08X %08X %08X %08X\n",
            CPU.d[0],CPU.d[1],CPU.d[2],CPU.d[3],CPU.d[4],CPU.d[5],CPU.d[6],CPU.d[7]);
    fprintf(stderr, "A0-A7 %08X %08X %08X %08X %08X %08X %08X %08X\n",
            CPU.a[0],CPU.a[1],CPU.a[2],CPU.a[3],CPU.a[4],CPU.a[5],CPU.a[6],CPU.a[7]);
    fprintf(stderr, "SR imask=%u super=%d  N=%u Z=%u V=%u C=%u X=%u\n",
            CPU.imask, (int)CPU.super, CPU.n, CPU.z, CPU.v, CPU.c, CPU.x);

    fprintf(stderr, "\nstack at A7=%08X:\n", CPU.a[7]);
    for (int i = 0; i < 8; i++) {
        uint32_t a = CPU.a[7] + (uint32_t)(i * 4);
        fprintf(stderr, "   %08X: %08X\n", a, m68k_read32(a));
    }

    unsigned n = trail_n < TRAIL ? trail_n : TRAIL;
    fprintf(stderr, "\nlast %u blocks executed (most recent last):\n", n);
    for (unsigned i = 0; i < n; i++) {
        unsigned idx = (trail_n - n + i) % TRAIL;
        fprintf(stderr, "   %2u: %06X\n", n - i - 1, trail[idx]);
    }
    fflush(stderr);
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
        if (i < 0) {
            m68k_last_unknown = pc;
            m68k_dump_crash(pc);
            return pc;
        }
        if (m68k_profiling) m68k_profile[i]++;
        m68k_cur_block = pc;
        trail_push(pc);
        pc = BLOCK_FN[i]();
        m68k_blocks_run++;
    }
    return pc;
}

/* Run one PAL frame, interleaving the 68000 and Z80 in small slices.
 *
 * Frame-granular scheduling is not good enough: the ROM writes a command into
 * Z80 RAM and then polls for the reply in a tight loop. If the Z80 only gets
 * to run once the 68000's whole frame is done, that poll spins for the entire
 * frame and the handshake can never complete inside it. Slicing keeps the two
 * processors in step at roughly the granularity real hardware provides.
 */
#define SLICE_CYCLES 500
/* Z80 3546893 Hz vs 68000 7600489 Hz on PAL. */
#define Z80_NUM 3546893ull
#define Z80_DEN 7600489ull

/* Who is waiting?  The vsync wait at $FD4 pushes D0 then spins at $FDA, so the
   caller's return address sits at 4(A7). Sampling it identifies the sequencer
   function that is blocked, which a PC profile alone cannot show. */
unsigned long waiter_hits[64];
uint32_t      waiter_addr[64];
unsigned      waiter_n;
int           waiter_enable;

static void sample_waiter(uint32_t pc) {
    if (!waiter_enable || pc != 0x000FDAu) return;
    uint32_t ret = m68k_read32(CPU.a[7] + 4);
    for (unsigned i = 0; i < waiter_n; i++)
        if (waiter_addr[i] == ret) { waiter_hits[i]++; return; }
    if (waiter_n < 64) { waiter_addr[waiter_n] = ret; waiter_hits[waiter_n] = 1; waiter_n++; }
}

uint32_t m68k_run_frame(uint32_t pc) {
    uint64_t deadline = CPU.cycles + PAL_FRAME_CYCLES;
    int z80_on = hal_z80_running();
    while (CPU.cycles < deadline) {
        uint64_t chunk = CPU.cycles + SLICE_CYCLES;
        if (chunk > deadline) chunk = deadline;
        sample_waiter(pc);
        /* Checked per slice rather than per block: a corrupted SP or PC stays
           corrupted, so slice granularity still catches it within 500 cycles
           while costing nothing measurable. */
        INV_CHECK(INV_SP_RANGE, IS_RAM_ADDR(CPU.a[7]), "stack pointer left RAM", CPU.a[7], pc);
        INV_CHECK(INV_SP_ALIGN, (CPU.a[7] & 1) == 0,   "stack pointer is odd",   CPU.a[7], pc);
        INV_CHECK(INV_PC_RANGE, IS_ROM_ADDR(pc),       "PC left ROM",            pc,       CPU.a[7]);
        pc = m68k_run_until(pc, chunk);
        if (m68k_last_unknown) return pc;
        z80_on = hal_z80_running();
        if (z80_on) {
            uint64_t target = (CPU.cycles * Z80_NUM) / Z80_DEN;
            if (Z80.cycles < target) z80_run(target);
        } else {
            /* Bus held by the 68000: the Z80 is stopped, so keep its clock
               aligned rather than letting it owe a huge catch-up later. */
            Z80.cycles = (CPU.cycles * Z80_NUM) / Z80_DEN;
        }
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
            m68k_dump_crash(pc);
            return pc;
        }
        if (m68k_profiling) m68k_profile[i]++;
        m68k_cur_block = pc;
        trail_push(pc);
        pc = BLOCK_FN[i]();
        m68k_blocks_run++;
    }
    return pc;
}
