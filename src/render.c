/* Software renderer for the VDP's scroll planes and sprites.
 *
 * Output is a 320x224 RGB framebuffer. This is deliberately a whole-frame
 * renderer rather than per-scanline: this ROM has no HBlank handler, so no
 * mid-frame register changes need to be honoured, and a frame renderer is far
 * easier to verify against a reference.
 */
#include "vdp.h"
#include "render.h"
#include <string.h>

uint8_t FB[FB_H][FB_W][3];

/* 9-bit BGR -> 8-bit RGB. Each component is 3 bits, so scale by 255/7. */
static void cram_rgb(uint16_t c, uint8_t out[3]) {
    static const uint8_t lvl[8] = {0, 36, 73, 109, 146, 182, 219, 255};
    out[0] = lvl[(c >> 1) & 7];
    out[1] = lvl[(c >> 5) & 7];
    out[2] = lvl[(c >> 9) & 7];
}

/* 4bpp 8x8 tile: 32 bytes, one nibble per pixel, high nibble leftmost. */
static uint8_t tile_pixel(unsigned tile, unsigned x, unsigned y) {
    uint32_t base = tile * 32u + y * 4u + (x >> 1);
    uint8_t b = VDP.vram[base & 0xFFFF];
    return (x & 1) ? (b & 0x0F) : (b >> 4);
}

static unsigned plane_w(void) {
    switch (VDP.reg[16] & 3) { case 0: return 32; case 1: return 64; default: return 128; }
}
static unsigned plane_h(void) {
    switch ((VDP.reg[16] >> 4) & 3) { case 0: return 32; case 1: return 64; default: return 128; }
}

/* Returns colour index 0-63, or 0 for transparent. */
static unsigned sample_plane(uint32_t nt_base, int px, int py, int *prio) {
    unsigned pw = plane_w(), ph = plane_h();
    unsigned tx = ((unsigned)px >> 3) % pw;
    unsigned ty = ((unsigned)py >> 3) % ph;
    uint32_t e = nt_base + (ty * pw + tx) * 2u;
    uint16_t ent = (uint16_t)((VDP.vram[e & 0xFFFF] << 8) | VDP.vram[(e + 1) & 0xFFFF]);

    unsigned tile = ent & 0x7FF;
    unsigned fx = (ent >> 11) & 1, fy = (ent >> 12) & 1;
    unsigned pal = (ent >> 13) & 3;
    *prio = (ent >> 15) & 1;

    unsigned ix = (unsigned)px & 7, iy = (unsigned)py & 7;
    if (fx) ix = 7 - ix;
    if (fy) iy = 7 - iy;
    unsigned c = tile_pixel(tile, ix, iy);
    return c ? (pal * 16 + c) : 0;
}

static int16_t hscroll_for(unsigned line, int plane_b) {
    uint32_t base = (uint32_t)(VDP.reg[13] & 0x3F) << 10;
    unsigned mode = VDP.reg[11] & 3;
    unsigned row = (mode == 0) ? 0 : (mode == 2) ? (line & ~7u) : line;
    uint32_t a = base + row * 4u + (plane_b ? 2u : 0u);
    return (int16_t)((VDP.vram[a & 0xFFFF] << 8) | VDP.vram[(a + 1) & 0xFFFF]);
}

static int16_t vscroll_for(unsigned col, int plane_b) {
    unsigned mode = (VDP.reg[11] >> 2) & 1;   /* 0 = whole screen, 1 = per 2 cells */
    unsigned idx = mode ? ((col >> 4) * 2u) : 0u;
    idx += plane_b ? 1u : 0u;
    return (int16_t)VDP.vsram[idx % VSRAM_SIZE];
}

void render_frame(void) {
    uint32_t nt_a = (uint32_t)(VDP.reg[2] & 0x38) << 10;
    uint32_t nt_b = (uint32_t)(VDP.reg[4] & 0x07) << 13;
    uint8_t backdrop[3];
    cram_rgb(VDP.cram[VDP.reg[7] & 0x3F], backdrop);

    if (!vdp_display_enabled()) {
        for (int y = 0; y < FB_H; y++)
            for (int x = 0; x < FB_W; x++)
                memset(FB[y][x], 0, 3);
        return;
    }

    for (int y = 0; y < FB_H; y++) {
        int16_t hs_a = hscroll_for((unsigned)y, 0);
        int16_t hs_b = hscroll_for((unsigned)y, 1);
        for (int x = 0; x < FB_W; x++) {
            int16_t vs_a = vscroll_for((unsigned)x, 0);
            int16_t vs_b = vscroll_for((unsigned)x, 1);
            int pa, pb;
            unsigned ca = sample_plane(nt_a, x - hs_a, y + vs_a, &pa);
            unsigned cb = sample_plane(nt_b, x - hs_b, y + vs_b, &pb);

            unsigned pick = 0;
            if      (pa && ca) pick = ca;      /* A, high priority */
            else if (pb && cb) pick = cb;      /* B, high priority */
            else if (ca)       pick = ca;
            else if (cb)       pick = cb;

            if (pick) cram_rgb(VDP.cram[pick & 0x3F], FB[y][x]);
            else      memcpy(FB[y][x], backdrop, 3);
        }
    }
    render_sprites();
}

/* Sprite attribute table: 8 bytes per entry, linked list via the `next` field.
 * Word 0 = Y, byte 2 = size, byte 3 = link, word 4 = attributes, word 6 = X. */
void render_sprites(void) {
    uint32_t sat = (uint32_t)(VDP.reg[5] & 0x7F) << 9;
    unsigned idx = 0;
    for (unsigned n = 0; n < 80; n++) {
        uint32_t e = sat + idx * 8u;
        const uint8_t *p = &VDP.vram[e & 0xFFFF];
        int      sy   = (int)(((p[0] << 8) | p[1]) & 0x3FF) - 128;
        unsigned hw   = ((p[2] >> 2) & 3) + 1;      /* width  in cells */
        unsigned hh   = (p[2] & 3) + 1;             /* height in cells */
        unsigned link = p[3] & 0x7F;
        uint16_t att  = (uint16_t)((p[4] << 8) | p[5]);
        int      sx   = (int)(((p[6] << 8) | p[7]) & 0x1FF) - 128;

        unsigned tile = att & 0x7FF;
        unsigned fx = (att >> 11) & 1, fy = (att >> 12) & 1;
        unsigned pal = (att >> 13) & 3;

        for (unsigned cx = 0; cx < hw; cx++) {
            for (unsigned cy = 0; cy < hh; cy++) {
                unsigned tc = tile + (fx ? (hw - 1 - cx) : cx) * hh
                                   + (fy ? (hh - 1 - cy) : cy);
                for (unsigned py = 0; py < 8; py++) {
                    for (unsigned px = 0; px < 8; px++) {
                        int X = sx + (int)(cx * 8 + px);
                        int Y = sy + (int)(cy * 8 + py);
                        if (X < 0 || X >= FB_W || Y < 0 || Y >= FB_H) continue;
                        unsigned ix = fx ? 7 - px : px, iy = fy ? 7 - py : py;
                        unsigned c = tile_pixel(tc, ix, iy);
                        if (c) cram_rgb(VDP.cram[(pal * 16 + c) & 0x3F], FB[Y][X]);
                    }
                }
            }
        }
        if (!link) break;
        idx = link;
    }
}

int render_write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    fwrite(FB, 1, sizeof FB, f);
    fclose(f);
    return 0;
}
