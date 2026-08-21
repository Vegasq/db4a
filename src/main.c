/* Boot harness: load the ROM, reset the CPU, execute, report where it got to. */
#include "m68k.h"
#include "hal.h"
#include "vdp.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>

extern unsigned long m68k_blocks_run;
extern uint32_t m68k_last_unknown;
extern unsigned long *m68k_profile;
extern unsigned long m68k_irq_taken, m68k_irq_masked;
void m68k_profile_enable(void);

static int cmp_hot(const void *a, const void *b) {
    unsigned ia = *(const unsigned *)a, ib = *(const unsigned *)b;
    if (m68k_profile[ia] < m68k_profile[ib]) return 1;
    if (m68k_profile[ia] > m68k_profile[ib]) return -1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <rom> [frames]\n", argv[0]); return 2; }
    /* Pacing is cycle-based now, so the budget is a frame count: at PAL
       50 Hz, 1 frame = 152009 cycles = 20 ms of game time. */
    unsigned max_frames = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 600;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc((size_t)n);
    if (fread(rom, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    hal_set_rom(rom, (size_t)n);
    hal_reset_ram();
    vdp_reset();

    /* Reset: SSP from $000000, PC from $000004, supervisor, interrupts masked */
    CPU.a[7]   = m68k_read32(0);
    CPU.ssp    = CPU.a[7];
    CPU.super  = true;
    CPU.imask  = 7;
    uint32_t pc = m68k_read32(4);

    printf("recompiled blocks : %u\n", BLOCK_COUNT);
    printf("reset SSP         : %08X\n", CPU.a[7]);
    printf("reset PC          : %06X\n", pc);
    printf("running %u frames (%.1f s of game time)...\n\n", max_frames, max_frames/50.0);
    m68k_profile_enable();
    { extern int hal_log_sr, hal_log_io;
      hal_log_sr = getenv("DB4A_LOG_SR") != NULL;
      hal_log_io = getenv("DB4A_LOG_IO") != NULL; }

    /* Run in frame-sized slices, firing VBlank between them. The ROM boots
       into an idle loop and does all its work from the level 6 handler, so
       without this nothing past initialisation ever executes. */
    uint32_t end = pc;
    unsigned frames = 0;
    for (frames = 0; frames < max_frames; frames++) {
        end = m68k_run_until(end, CPU.cycles + PAL_FRAME_CYCLES);
        if (m68k_last_unknown) break;
        end = m68k_interrupt(end, 6);          /* VBlank at end of frame */
    }
    printf("frames simulated  : %u  (%.2f s of game time)\n",
           frames, frames / 50.0);
    printf("cycles emulated   : %llu\n", (unsigned long long)CPU.cycles);

    printf("\nblocks executed   : %lu\n", m68k_blocks_run);
    printf("stopped at PC     : %06X\n", end);
    if (m68k_last_unknown) {
        printf("reason            : no block for PC %06X (unknown target)\n",
               m68k_last_unknown);
        const char *sf = getenv("DB4A_SEEDS");
        if (sf) {
            FILE *s = fopen(sf, "a");
            if (s) { fprintf(s, "%06X\n", m68k_last_unknown); fclose(s); }
        }
    }
    else
        printf("reason            : block budget exhausted\n");
    printf("I/O reads / writes: %lu / %lu\n", hal_io_reads, hal_io_writes);
    printf("IRQ taken / masked: %lu / %lu\n", m68k_irq_taken, m68k_irq_masked);
    { extern unsigned long hal_sr_writes;
      printf("SR writes         : %lu\n", hal_sr_writes); }
    printf("SR state          : imask=%u super=%d\n", CPU.imask, (int)CPU.super);
    printf("RAM $FFFFE002     : %08X  (main-loop handler pointer)\n", m68k_read32(0xFFFFE002));
    printf("D0-D7 %08X %08X %08X %08X %08X %08X %08X %08X\n",
           CPU.d[0],CPU.d[1],CPU.d[2],CPU.d[3],CPU.d[4],CPU.d[5],CPU.d[6],CPU.d[7]);
    printf("A0-A7 %08X %08X %08X %08X %08X %08X %08X %08X\n",
           CPU.a[0],CPU.a[1],CPU.a[2],CPU.a[3],CPU.a[4],CPU.a[5],CPU.a[6],CPU.a[7]);

    vdp_dump();
    render_frame();
    { const char *out = getenv("DB4A_PPM");
      if (out && render_write_ppm(out) == 0) printf("wrote framebuffer to %s\n", out); }
    /* nametable occupancy: are the planes actually populated? */
    { uint32_t na = (uint32_t)(VDP.reg[2] & 0x38) << 10;
      uint32_t nb = (uint32_t)(VDP.reg[4] & 0x07) << 13;
      uint32_t sa = (uint32_t)(VDP.reg[5] & 0x7F) << 9;
      unsigned ca=0, cb=0, cs=0;
      for (unsigned i=0;i<4096;i++){
        if (VDP.vram[(na+i*2)&0xFFFF] || VDP.vram[(na+i*2+1)&0xFFFF]) ca++;
        if (VDP.vram[(nb+i*2)&0xFFFF] || VDP.vram[(nb+i*2+1)&0xFFFF]) cb++;
      }
      for (unsigned i=0;i<640;i++) if (VDP.vram[(sa+i)&0xFFFF]) cs++;
      printf("nametable A @%04X: %u/4096 non-zero\n", na, ca);
      printf("nametable B @%04X: %u/4096 non-zero\n", nb, cb);
      printf("sprite tbl  @%04X: %u/640 bytes non-zero\n", sa, cs);
      printf("tile area 0000-B000: ");
      { unsigned nz=0; for(unsigned i=0;i<0xB000;i++) if(VDP.vram[i]) nz++;
        printf("%u/%u non-zero (%.1f%%)\n", nz, 0xB000, 100.0*nz/0xB000); } }

    if (m68k_profile) {
        unsigned *idx = malloc(BLOCK_COUNT * sizeof *idx);
        unsigned live = 0;
        for (unsigned i = 0; i < BLOCK_COUNT; i++) {
            if (m68k_profile[i]) idx[live++] = i;
        }
        qsort(idx, live, sizeof *idx, cmp_hot);
        printf("\ndistinct blocks executed: %u of %u\n", live, BLOCK_COUNT);
        printf("hottest blocks:\n");
        for (unsigned i = 0; i < live && i < 12; i++)
            printf("   %06X  %12lu  %5.1f%%\n", BLOCK_ADDR[idx[i]],
                   m68k_profile[idx[i]],
                   100.0 * m68k_profile[idx[i]] / (double)m68k_blocks_run);
    }
    return 0;
}
