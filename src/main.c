/* Boot harness: load the ROM, reset the CPU, execute, report where it got to. */
#include "m68k.h"
#include "hal.h"
#include "vdp.h"
#include "render.h"
#include "input.h"
#include "z80.h"
#include "system.h"
#include "invariant.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    uint32_t pc = system_reset(rom, (size_t)n);
    { extern int z80_profiling; z80_profiling = 1; }
    { extern int waiter_enable; waiter_enable = 1; }


    printf("recompiled blocks : %u\n", BLOCK_COUNT);
    printf("reset SSP         : %08X\n", CPU.a[7]);
    printf("reset PC          : %06X\n", pc);
    printf("running %u frames (%.1f s of game time)...\n\n", max_frames, max_frames/50.0);

    /* Capture frames mid-run, not just at the end: a playthrough needs to see
       each screen it passes through, and the interesting one is rarely last. */
    unsigned shot_at[32]; unsigned nshots = 0;
    { const char *sp = getenv("DB4A_SHOTS");
      char buf[256];
      if (sp) {
        snprintf(buf, sizeof buf, "%s", sp);
        for (char *tok = strtok(buf, ","); tok && nshots < 32; tok = strtok(NULL, ","))
            shot_at[nshots++] = (unsigned)strtoul(tok, NULL, 0);
      } }
    const char *shot_prefix = getenv("DB4A_PPM");
    m68k_profile_enable();
    { extern int hal_log_sr, hal_log_io;
      hal_log_sr = getenv("DB4A_LOG_SR") != NULL;
      hal_log_io = getenv("DB4A_LOG_IO") != NULL;
      extern unsigned long pad_log_from;
      if (getenv("DB4A_LOG_PAD_FROM"))
          pad_log_from = strtoul(getenv("DB4A_LOG_PAD_FROM"), NULL, 0); }

    /* Run in frame-sized slices, firing VBlank between them. The ROM boots
       into an idle loop and does all its work from the level 6 handler, so
       without this nothing past initialisation ever executes. */
    uint32_t end = pc;
    unsigned frames = 0;
    /* Scripted input, so the headless harness can drive the game past menus
       without a display:  DB4A_PRESS="800:start,1000:c"  */
    /* Sized for real scenarios: the gameplay sweep is 82 inputs and its script
       string is ~700 bytes. The previous 16 entries / 256 byte buffer
       truncated both SILENTLY, so every press past the 16th was dropped and
       in-game input looked broken when it had simply never been parsed. */
    enum { MAX_SCRIPT = 256 };
    struct { unsigned at; int pad; } script[MAX_SCRIPT];
    unsigned nscript = 0;
    unsigned hold = getenv("DB4A_HOLD") ? (unsigned)strtoul(getenv("DB4A_HOLD"), NULL, 0) : 8;
    { const char *sp = getenv("DB4A_PRESS");
      char buf[8192];
      if (sp) {
        if (strlen(sp) >= sizeof buf)
            fprintf(stderr, "DB4A_PRESS truncated: %zu bytes, buffer %zu\n",
                    strlen(sp), sizeof buf);
        snprintf(buf, sizeof buf, "%s", sp);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            if (nscript >= MAX_SCRIPT) {
                fprintf(stderr, "DB4A_PRESS truncated at %u entries\n", nscript);
                break;
            }
            char name[32]; unsigned at;
            if (sscanf(tok, "%u:%31s", &at, name) != 2) continue;
            /* Table-driven so a missing name is impossible to overlook.
               left and right were absent from the previous if-chain and were
               dropped SILENTLY, so every horizontal input ever scripted was
               discarded -- including the gameplay sweep. */
            static const struct { const char *name; int pad; } NAMES[] = {
                { "up", PAD_UP }, { "down", PAD_DOWN },
                { "left", PAD_LEFT }, { "right", PAD_RIGHT },
                { "a", PAD_A }, { "b", PAD_B }, { "c", PAD_C },
                { "start", PAD_START },
            };
            int b = -1;
            for (unsigned n = 0; n < sizeof NAMES / sizeof NAMES[0]; n++)
                if (!strcmp(name, NAMES[n].name)) { b = NAMES[n].pad; break; }
            if (b < 0) {
                fprintf(stderr, "DB4A_PRESS: unknown button '%s' -- ignored\n", name);
                continue;
            }
            script[nscript].at = at; script[nscript].pad = b; nscript++;
        }
      } }

    if (nscript)
        fprintf(stderr, "parsed %u input events; last at frame %u\n",
                nscript, script[nscript-1].at);
    for (frames = 0; frames < max_frames; frames++) {
        /* Hold length matters: a menu that advances on each press can consume
           one long hold twice. DB4A_HOLD tunes it. */
        for (unsigned k = 0; k < nscript; k++) {
            if (frames == script[k].at)          pad_set(script[k].pad, 1);
            if (frames == script[k].at + hold)   pad_set(script[k].pad, 0);
        }
        end = system_frame(end);
        if (m68k_last_unknown) break;

        for (unsigned k = 0; k < nshots; k++) {
            if (frames == shot_at[k] && shot_prefix) {
                char path[512];
                snprintf(path, sizeof path, "%s.%u.ppm", shot_prefix, frames);
                render_frame();
                if (render_write_ppm(path) == 0)
                    printf("  [frame %5u] captured %s\n", frames, path);
            }
        }
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

    { extern void hal_io_report(void); hal_io_report(); }
    { extern unsigned long hal_z80_writes; extern int hal_z80_running(void);
      extern void hal_dump_z80(const char *);
      printf("Z80 RAM writes    : %lu\n", hal_z80_writes);
      printf("Z80 state         : pc=%04X sp=%04X cycles=%llu %s\n",
             Z80.pc, Z80.sp, (unsigned long long)Z80.cycles,
             hal_z80_running() ? "RUNNING" : "halted/bus-held");
      { extern unsigned long z80_pc_hits[0x2000], z80_writes_1b2x;
        unsigned live = 0; unsigned long tot = 0;
        for (unsigned i = 0; i < 0x2000; i++) { if (z80_pc_hits[i]) live++; tot += z80_pc_hits[i]; }
        printf("Z80 distinct PCs  : %u   (total %lu instructions)\n", live, tot);
        printf("Z80 writes to 1B2x: %lu\n", z80_writes_1b2x);
        printf("Z80 hottest PCs   :");
        for (int k = 0; k < 8; k++) {
          unsigned best = 0; for (unsigned i = 0; i < 0x2000; i++)
            if (z80_pc_hits[i] > z80_pc_hits[best]) best = i;
          if (!z80_pc_hits[best]) break;
          printf(" %04X(%lu)", best, z80_pc_hits[best]); z80_pc_hits[best] = 0; }
        printf("\n"); }
      const char *zp = getenv("DB4A_Z80DUMP");
      if (zp) { hal_dump_z80(zp); printf("dumped Z80 RAM to %s\n", zp); } }
    { extern unsigned long waiter_hits[64]; extern uint32_t waiter_addr[64];
      extern unsigned waiter_n;
      printf("\nvsync-wait callers (who is blocked):\n");
      for (unsigned i = 0; i < waiter_n; i++)
        printf("   return to %06X : %lu samples\n", waiter_addr[i], waiter_hits[i]); }
    invariant_report();
    pad_report();
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
      { uint32_t wn = (uint32_t)(VDP.reg[3] & 0x3E) << 10;
        unsigned cw = 0;
        for (unsigned i = 0; i < 4096; i++)
          if (VDP.vram[(wn+i*2)&0xFFFF] || VDP.vram[(wn+i*2+1)&0xFFFF]) cw++;
        printf("window nametable @%04X: %u/4096 non-zero  (regs 11=%02X 17=%02X 18=%02X)\n",
               wn, cw, VDP.reg[11], VDP.reg[17], VDP.reg[18]); }
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
