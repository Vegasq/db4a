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
#include "vdp.h"
#include "psg.h"
#include "ym2612.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>

unsigned long m68k_blocks_run;
unsigned long z80_slices, z80_slices_off;

/* Execution trace for the differential oracle (layer 3).
 *
 * One 8-byte record per BLOCK entry: PC plus a 32-bit FNV-1a hash of
 * D0-D7/A0-A7, using byte-for-byte the same hash as the patched reference
 * core so the two traces are directly comparable. The reference logs every
 * instruction, so its trace is filtered to block-start PCs before diffing. */
static FILE *trace_fp;
static int   trace_init;

static void trace_block(uint32_t pc) {
    if (!trace_init) {
        const char *path = getenv("DB4A_TRACE");
        trace_fp = path ? fopen(path, "wb") : NULL;
        trace_init = 1;
    }
    if (!trace_fp) return;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; i++) {
        uint32_t v = (i < 8) ? CPU.d[i] : CPU.a[i - 8];
        for (int b = 0; b < 4; b++) { h ^= (v >> (b * 8)) & 0xFF; h *= 16777619u; }
    }
    uint32_t rec[2] = { pc, h };
    fwrite(rec, 4, 2, trace_fp);
}
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

    /* Record the address so the next `make analyse` picks it up. Discovery
     * reaches everything the game has been played through, but the RAM
     * dispatch at $FFFFE002 means a state nobody has entered yet can always
     * produce a PC no static pass predicted. Appending it here turns that from
     * "parse the crash by hand" into one rebuild. DB4A_SEEDS overrides the
     * path; trace.py reads build/seeds.txt as scratch alongside the tracked
     * data/seeds.txt. */
    {
        const char *sp = getenv("DB4A_SEEDS");
        if (!sp) sp = "build/seeds.txt";
        FILE *sf = fopen(sp, "a");
        if (sf) {
            fprintf(sf, "%06X   # unknown PC, reached from %06X\n",
                    pc, trail_n ? trail[(trail_n - 1) % TRAIL] : 0);
            fclose(sf);
            fprintf(stderr, "\nrecorded %06X in %s -- `make analyse && make` will cover it\n",
                    pc, sp);
        }
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
        trace_block(pc);
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

/* Who is waiting?  The vsync wait at $FD4 pushes D0 then spins at $FDA, so the
   caller's return address sits at 4(A7). Sampling it identifies the sequencer
   function that is blocked, which a PC profile alone cannot show. */
unsigned long waiter_hits[64];
uint32_t      waiter_addr[64];
unsigned      waiter_n;
int           waiter_enable;

/* DB4A_LOG_SFX: $2DDAE is the game's "play sound N" entry -- the callers push
 * the id as a long and jsr there. Logging the id at the call identifies which
 * sound any given effect is, which is otherwise guesswork. */
static void sfx_log(uint32_t pc) {
    if (pc != 0x001664u && pc != 0x00167Au) return;
    static FILE *fp; static int init;
    if (!init) { const char *p = getenv("DB4A_LOG_SFX"); fp = p ? fopen(p, "w") : NULL; init = 1; }
    if (fp) fprintf(fp, "%06X %u %u\n", pc,
                    (unsigned)m68k_read32(CPU.a[7] + 4), (unsigned)CPU.cycles);
}

static void sample_waiter(uint32_t pc) {
    if (!waiter_enable || pc != 0x000FDAu) return;
    uint32_t ret = m68k_read32(CPU.a[7] + 4);
    for (unsigned i = 0; i < waiter_n; i++)
        if (waiter_addr[i] == ret) { waiter_hits[i]++; return; }
    if (waiter_n < 64) { waiter_addr[waiter_n] = ret; waiter_hits[waiter_n] = 1; waiter_n++; }
}

uint32_t m68k_run_frame(uint32_t pc) {
    const uint64_t start = CPU.cycles;
    uint64_t deadline = start + PAL_FRAME_CYCLES;
    int z80_on = hal_z80_running();
    /* Cycle 0 of a frame is the start of active display and the VBlank
       interrupt is raised once this returns, so line L begins at
       L * PAL_FRAME_CYCLES / PAL_LINES cycles in. Scroll state is latched
       there because the game rewrites vertical scroll DURING the frame and a
       real VDP draws each line with the value in effect at that line. */
    unsigned line = 0;
    vdp_frame_start = start;
    render_frame_begin();
    while (CPU.cycles < deadline) {
        while (line < PAL_LINES) {
            uint64_t at = start + (uint64_t)line * PAL_FRAME_CYCLES / PAL_LINES;
            if (at > CPU.cycles) break;
            render_line_latch(line);
            line++;
        }
        uint64_t chunk = CPU.cycles + SLICE_CYCLES;
        /* Stop at the next line boundary so a latch is never overshot. */
        if (line < PAL_LINES) {
            uint64_t next = start + (uint64_t)line * PAL_FRAME_CYCLES / PAL_LINES;
            if (next < chunk) chunk = next;
        }
        if (chunk > deadline) chunk = deadline;
        if (chunk <= CPU.cycles) chunk = CPU.cycles + 1;   /* always advance */
        sample_waiter(pc);
        sfx_log(pc);
        /* Checked per slice rather than per block: a corrupted SP or PC stays
           corrupted, so slice granularity still catches it within 500 cycles
           while costing nothing measurable. */
        INV_CHECK(INV_SP_RANGE, IS_RAM_ADDR(CPU.a[7]), "stack pointer left RAM", CPU.a[7], pc);
        INV_CHECK(INV_SP_ALIGN, (CPU.a[7] & 1) == 0,   "stack pointer is odd",   CPU.a[7], pc);
        INV_CHECK(INV_PC_RANGE, IS_ROM_ADDR(pc),       "PC left ROM",            pc,       CPU.a[7]);
        pc = m68k_run_until(pc, chunk);
        if (m68k_last_unknown) return pc;
        /* Advance the sound chips BEFORE the Z80 runs, so it polls a current
         * chip rather than a frozen one.
         *
         * These used to be called once at the end of the frame. The Dune sound
         * driver paces itself by writing 0x15 to YM2612 register 0x27 -- start
         * Timer A, enable its flag, clear it -- and then spinning on
         * `bit 0,(hl)` until the overflow appears. With the timers only
         * advancing at a frame boundary that flag could not change while the
         * Z80 was running, so the driver burned its entire slice in the spin
         * loop and received one timer event per frame instead of the ~10000 it
         * expects. That is what made the music crawl and then stop entirely.
         */
        psg_run(CPU.cycles);
        /* NOT ym_run(CPU.cycles) here.
         *
         * The Z80 is about to execute the span that ends at CPU.cycles, so
         * running the chip to CPU.cycles first puts it AHEAD of the Z80 for
         * the whole slice. Every register write the driver makes then lands at
         * the end of the slice instead of where the Z80 actually is -- and a
         * Timer A restart that lands late overflows late. At a 500-cycle slice
         * against a ~95us timer period that is most of a period of delay per
         * restart, and the driver restarts the timer constantly.
         *
         * The chip is advanced on demand instead: z80_read and z80_write both
         * bring it to the Z80's position before answering, and the end of the
         * frame catches it up. That keeps the flag current for the driver's
         * poll -- which is what the eager call was for -- without ever running
         * the chip past the code that is driving it. */
        z80_on = hal_z80_running();
        z80_slices++; if (!z80_on) z80_slices_off++;
        if (z80_on) {
            uint64_t target = (CPU.cycles * Z80_NUM) / Z80_DEN;
            if (Z80.cycles < target) z80_run(target);
        } else {
            /* Bus held by the 68000: the Z80 is stopped, so keep its clock
               aligned rather than letting it owe a huge catch-up later. */
            Z80.cycles = (CPU.cycles * Z80_NUM) / Z80_DEN;
        }
    }
    psg_run(CPU.cycles);
    ym_run(CPU.cycles);
    /* A frame cut short (unknown PC, or a slice that ran long) can leave later
       lines unlatched; fill them so the renderer never mixes latched and live
       state within one frame. */
    while (line < PAL_LINES) { render_line_latch(line); line++; }
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
        trace_block(pc);
        pc = BLOCK_FN[i]();
        m68k_blocks_run++;
    }
    return pc;
}
