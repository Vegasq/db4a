#ifndef VDP_H
#define VDP_H
#include <stdint.h>

#define VRAM_SIZE   0x10000
#define CRAM_SIZE   64        /* 64 colours, 9-bit BGR */
#define VSRAM_SIZE  40

typedef struct {
    uint8_t  vram[VRAM_SIZE];
    uint16_t cram[CRAM_SIZE];
    uint16_t vsram[VSRAM_SIZE];
    uint8_t  reg[24];

    /* Control port is a two-write protocol; `pending` tracks the half-state. */
    uint16_t first;
    int      pending;
    uint32_t addr;
    uint8_t  code;

    int      dma_fill_pending;
    uint64_t dma_busy_until;   /* CPU cycle at which the current DMA completes */

    /* Counters, so we can see what the ROM actually did. */
    unsigned long vram_writes, cram_writes, vsram_writes, reg_writes;
    unsigned long dma_transfers, dma_words;
} vdp_t;

extern vdp_t VDP;

void     vdp_reset(void);
void     vdp_write_control(uint16_t v);
void     vdp_write_data(uint16_t v);
uint16_t vdp_read_data(void);
uint16_t vdp_read_status(void);
void     vdp_dump(void);

/* Register accessors used by the renderer. */
extern uint64_t vdp_frame_start;   /* CPU cycle at line 0 of this frame */

static inline int vdp_display_enabled(void) { return (VDP.reg[1] >> 6) & 1; }
static inline int vdp_vint_enabled(void)    { return (VDP.reg[1] >> 5) & 1; }
static inline int vdp_dma_enabled(void)     { return (VDP.reg[1] >> 4) & 1; }
static inline int vdp_h40(void)             { return (VDP.reg[12] >> 7) & 1; }
static inline unsigned vdp_autoinc(void)    { return VDP.reg[15]; }
void vdp_nt_report(void);
#endif
