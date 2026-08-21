/* Mega Drive VDP: address/code state machine, memory writes and DMA.
 *
 * Rendering is not here yet -- this stage exists so we can see what the ROM
 * uploads (tiles, palette, scroll, register setup) and verify it is sane
 * before drawing anything.
 *
 * Control port protocol. A write with bits 15-14 == 10 is a register write:
 * register (v >> 8) & 0x1F, value v & 0xFF. Anything else is half of a
 * two-word address/code pair:
 *     word 1:  CD1-CD0 in bits 15-14, A13-A0 in bits 13-0
 *     word 2:  CD5-CD2 in bits 7-4, A15-A14 in bits 1-0
 */
#include "vdp.h"
#include "m68k.h"
#include "invariant.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int dma_log = -1;
static int dma_logging(void) {
    if (dma_log < 0) dma_log = getenv("DB4A_LOG_DMA") ? 1 : 0;
    return dma_log;
}

vdp_t VDP;
unsigned long vdp_fills;
unsigned long dma_dest[10];
unsigned long dma_code[64];

/* Code field values, after assembling both control words. */
#define CD_VRAM_READ   0x00
#define CD_VRAM_WRITE  0x01
#define CD_CRAM_WRITE  0x03
#define CD_VSRAM_READ  0x04
#define CD_VSRAM_WRITE 0x05
#define CD_CRAM_READ   0x08
#define CD_DMA         0x20

void vdp_reset(void) {
    memset(&VDP, 0, sizeof VDP);
    VDP.reg[1]  = 0x04;      /* Mega Drive mode, display off */
    VDP.reg[10] = 0xFF;      /* HInt counter */
    VDP.reg[15] = 2;         /* auto-increment */
}

static void vdp_dma_run(void);

void vdp_write_control(uint16_t v) {
    if (!VDP.pending && (v & 0xC000) == 0x8000) {      /* register write */
        unsigned r = (v >> 8) & 0x1F;
        if (r < 24) { VDP.reg[r] = (uint8_t)(v & 0xFF); VDP.reg_writes++; }
        return;
    }
    if (!VDP.pending) {
        VDP.first = v;
        VDP.pending = 1;
        /* The low CD bits are usable immediately, before the second word. */
        VDP.addr = (VDP.addr & 0xC000) | (v & 0x3FFF);
        VDP.code = (uint8_t)((VDP.code & 0x3C) | ((v >> 14) & 3));
        return;
    }
    VDP.pending = 0;
    VDP.addr = (uint32_t)((VDP.first & 0x3FFF) | ((v & 3) << 14));
    VDP.code = (uint8_t)(((VDP.first >> 14) & 3) | ((v >> 2) & 0x3C));

    if (VDP.code & CD_DMA) {
        /* A VRAM-fill DMA waits for the fill value to arrive on the data
           port; the other kinds run immediately. */
        unsigned mode = (VDP.reg[23] >> 6) & 3;
        if (mode == 2) {
            if (dma_logging())
                fprintf(stderr, "[dma] ARM fill: reg23=%02X addr=%04X code=%02X (vram writes so far %lu)\n",
                        VDP.reg[23], VDP.addr, VDP.code, VDP.vram_writes);
            VDP.dma_fill_pending = 1;
        } else {
            vdp_dma_run();
        }
    }
}

static void write_vram_byte(uint32_t a, uint8_t b) { VDP.vram[a & 0xFFFF] = b; }

static void vdp_store(uint16_t v) {
    switch (VDP.code & 0x0F) {
    case CD_VRAM_WRITE:
        /* An odd address swaps the byte order -- a real VDP quirk. */
        if (VDP.addr & 1) {
            write_vram_byte(VDP.addr - 1, (uint8_t)v);
            write_vram_byte(VDP.addr,     (uint8_t)(v >> 8));
        } else {
            write_vram_byte(VDP.addr,     (uint8_t)(v >> 8));
            write_vram_byte(VDP.addr + 1, (uint8_t)v);
        }
        VDP.vram_writes++;
        break;
    case CD_CRAM_WRITE:
        VDP.cram[(VDP.addr >> 1) & (CRAM_SIZE - 1)] = v & 0x0EEE;
        VDP.cram_writes++;
        break;
    case CD_VSRAM_WRITE:
        if (((VDP.addr >> 1) & 0x3F) < VSRAM_SIZE)
            VDP.vsram[(VDP.addr >> 1) & 0x3F] = v & 0x07FF;
        VDP.vsram_writes++;
        break;
    default:
        break;
    }
    VDP.addr = (VDP.addr + vdp_autoinc()) & 0xFFFF;
}

void vdp_write_data(uint16_t v) {
    if (VDP.dma_fill_pending) {
        VDP.dma_fill_pending = 0;
        uint16_t len = (uint16_t)((VDP.reg[20] << 8) | VDP.reg[19]);
        if (!len) len = 0xFFFF;
        vdp_fills++;
        if (dma_logging())
            fprintf(stderr, "[dma] FILL #%lu len=%u addr=%04X val=%04X inc=%u (after %lu vram writes)\n",
                    vdp_fills, len, VDP.addr, v, vdp_autoinc(), VDP.vram_writes);
        for (uint16_t i = 0; i < len; i++) {
            write_vram_byte(VDP.addr, (uint8_t)(v >> 8));
            VDP.addr = (VDP.addr + vdp_autoinc()) & 0xFFFF;
        }
        VDP.dma_transfers++; VDP.dma_words += len;
        return;
    }
    vdp_store(v);
}

uint16_t vdp_read_data(void) {
    uint16_t r = 0;
    switch (VDP.code & 0x0F) {
    case CD_VRAM_READ:
        r = (uint16_t)((VDP.vram[VDP.addr & 0xFFFF] << 8)
                       | VDP.vram[(VDP.addr + 1) & 0xFFFF]);
        break;
    case CD_CRAM_READ:  r = VDP.cram[(VDP.addr >> 1) & (CRAM_SIZE - 1)]; break;
    case CD_VSRAM_READ: r = VDP.vsram[(VDP.addr >> 1) % VSRAM_SIZE];     break;
    default: break;
    }
    VDP.addr = (VDP.addr + vdp_autoinc()) & 0xFFFF;
    return r;
}

/* Status: report "no DMA in progress, FIFO empty" plus the PAL bit.
   Bit 1 = FIFO empty, bit 9 = FIFO full(0), bit 0 = PAL. */
uint16_t vdp_read_status(void) {
    return 0x3400 | 0x0200 | 0x0001;
}

static void vdp_dma_run(void) {
    unsigned mode = (VDP.reg[23] >> 6) & 3;
    uint32_t len = (uint32_t)((VDP.reg[20] << 8) | VDP.reg[19]);
    if (!len) len = 0x10000;
    /* A transfer larger than VRAM itself is either a decode error or a
       runaway; either way it will wipe the display. */
    INV_CHECK(INV_VDP_DMA_LEN, len <= 0x10000, "DMA length exceeds VRAM", len, VDP.addr);
    if (dma_logging() && VDP.dma_transfers < 40)
        fprintf(stderr, "[dma] #%lu mode=%u len=%u code=%02X addr=%04X src=%06X\n",
                VDP.dma_transfers, mode, len, VDP.code, VDP.addr,
                (unsigned)(((VDP.reg[23] & 0x7F) << 17) | (VDP.reg[22] << 9) | (VDP.reg[21] << 1)));
    uint32_t src = (uint32_t)(((VDP.reg[23] & 0x7F) << 17)
                            | (VDP.reg[22] << 9) | (VDP.reg[21] << 1));
    /* Tally DMA destinations by target region so a missing tilemap upload is
       visible without wading through thousands of log lines. */
    dma_code[VDP.code & 0x3F] += len;
    {
      /* Bucket by target. VRAM buckets are 8 KiB each; note 0xE000+ is bucket
         7, which is a VRAM region and NOT VSRAM -- conflating the two sent an
         earlier investigation off after a non-existent misclassification. */
      unsigned bucket = (VDP.code & 0x0F) == 0x03 ? 8         /* CRAM  */
                      : (VDP.code & 0x0F) == 0x05 ? 9         /* VSRAM */
                      : (VDP.addr >> 13) & 7;                 /* VRAM 8K bucket */
      dma_dest[bucket] += len;
    }
    if (dma_logging() && VDP.addr >= 0xE000 && (VDP.code & 0x0F) == 1) {
        static unsigned shown;
        if (shown++ < 6) {
            uint32_t s0 = m68k_read32(src), s1 = m68k_read32(src + 4);
            fprintf(stderr, "[dma] ->nametable addr=%04X len=%u src=%06X first8=%08X %08X\n",
                    VDP.addr, len, src, s0, s1);
        }
    }
    if (mode == 3) {                       /* VRAM -> VRAM copy */
        for (uint32_t i = 0; i < len; i++) {
            VDP.vram[VDP.addr & 0xFFFF] = VDP.vram[(src + i) & 0xFFFF];
            VDP.addr = (VDP.addr + vdp_autoinc()) & 0xFFFF;
        }
    } else {                               /* 68000 bus -> VDP */
        for (uint32_t i = 0; i < len; i++) {
            vdp_store(m68k_read16(src + i * 2));
        }
    }
    VDP.dma_transfers++; VDP.dma_words += len;
}

void vdp_dump(void) {
    unsigned nonzero_vram = 0;
    for (unsigned i = 0; i < VRAM_SIZE; i++) if (VDP.vram[i]) nonzero_vram++;
    unsigned nonzero_cram = 0;
    for (unsigned i = 0; i < CRAM_SIZE; i++) if (VDP.cram[i]) nonzero_cram++;

    printf("\n--- VDP ---\n");
    printf("writes  vram=%lu cram=%lu vsram=%lu reg=%lu\n",
           VDP.vram_writes, VDP.cram_writes, VDP.vsram_writes, VDP.reg_writes);
    printf("dma     transfers=%lu words=%lu\n", VDP.dma_transfers, VDP.dma_words);
    printf("vram    %u of %d bytes non-zero (%.1f%%)\n",
           nonzero_vram, VRAM_SIZE, 100.0 * nonzero_vram / VRAM_SIZE);
    printf("cram    %u of %d entries non-zero\n", nonzero_cram, CRAM_SIZE);
    printf("display %s   vint %s   dma %s   %s\n",
           vdp_display_enabled() ? "ON" : "off",
           vdp_vint_enabled() ? "on" : "off",
           vdp_dma_enabled() ? "on" : "off",
           vdp_h40() ? "H40 (320px)" : "H32 (256px)");
    printf("planes  A=%04X B=%04X window=%04X sprites=%04X hscroll=%04X\n",
           (VDP.reg[2] & 0x38) << 10, (VDP.reg[4] & 0x07) << 13,
           (VDP.reg[3] & 0x3E) << 10, (VDP.reg[5] & 0x7F) << 9,
           (VDP.reg[13] & 0x3F) << 10);
    printf("dma by CD code (words):\n");
    for (int i = 0; i < 64; i++)
        if (dma_code[i]) printf("   code %02X : %lu\n", i, dma_code[i]);
    printf("dma dest by region (words):\n");
    for (int i = 0; i < 8; i++)
        if (dma_dest[i]) printf("   VRAM %04X-%04X : %lu\n", i*0x2000, i*0x2000+0x1FFF, dma_dest[i]);
    if (dma_dest[8]) printf("   CRAM          : %lu\n", dma_dest[8]);
    if (dma_dest[9]) printf("   VSRAM         : %lu\n", dma_dest[9]);
    printf("regs   ");
    for (int i = 0; i < 24; i++) printf(" %02X", VDP.reg[i]);
    printf("\n");
}
