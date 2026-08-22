/* Software renderer for the VDP's scroll planes and sprites.
 *
 * Output is a 320x224 RGB framebuffer. This is deliberately a whole-frame
 * renderer rather than per-scanline: this ROM has no HBlank handler, so no
 * mid-frame register changes need to be honoured, and a frame renderer is far
 * easier to verify against a reference.
 */
#include <stdlib.h>
#include <stdio.h>
#include "vdp.h"
#include "render.h"
#include "hal.h"
#include <string.h>

uint8_t FB[FB_H][FB_W][3];
int fb_width = 320;

/* Set by the plane pass: 1 where the visible plane pixel came from a
 * high-priority pattern. Read by the sprite pass, which is why it is file
 * scope rather than a local. */
static uint8_t plane_hi[FB_H][FB_W];

/* Sprite pixels a high-priority plane pattern hid. Zero means the priority
 * rule made no difference, which is the only honest way to tell whether a
 * frame exercises it at all -- DB4A_LOG_OCCLUDE reports it.
 *
 * Note the headless build only renders when a screenshot is requested, so
 * these counters stay at zero on a run with no DB4A_SHOTS. That is not the
 * same as "the rule never fires", and reading it that way cost a wrong
 * conclusion here twice. */
unsigned long render_occluded;
unsigned long render_planehi;   /* high-priority plane pixels drawn */

/* 9-bit BGR -> 8-bit RGB.
 *
 * A 3-bit component does NOT map linearly onto 0-255. The VDP shares one DAC
 * range across shadow, normal and highlight modes, so normal mode occupies
 * only the middle of the range: the component behaves as a 4-bit value of
 * (c << 1) out of 15, and highlight adds 7 on top to reach full scale.
 * Full intensity in normal mode is therefore 14/15 = 238, not 255.
 *
 * Using a naive c*255/7 ramp made every non-black pixel disagree with the
 * reference emulator while the image structure matched exactly.
 */
#define LVL(n)  ((uint8_t)(((n) * 255 + 7) / 15))

__attribute__((unused)) static const uint8_t LVL_SHADOW[8]    = { LVL(0), LVL(1), LVL(2),  LVL(3),
                                          LVL(4), LVL(5), LVL(6),  LVL(7)  };
static const uint8_t LVL_NORMAL[8]    = { LVL(0), LVL(2), LVL(4),  LVL(6),
                                          LVL(8), LVL(10),LVL(12), LVL(14) };
__attribute__((unused)) static const uint8_t LVL_HIGHLIGHT[8] = { LVL(7), LVL(8), LVL(9),  LVL(10),
                                          LVL(11),LVL(12),LVL(13), LVL(15) };

static void cram_rgb_mode(uint16_t c, uint8_t out[3], const uint8_t lvl[8]) {
    out[0] = lvl[(c >> 1) & 7];
    out[1] = lvl[(c >> 5) & 7];
    out[2] = lvl[(c >> 9) & 7];
}
static void cram_rgb(uint16_t c, uint8_t out[3]) {
    cram_rgb_mode(c, out, LVL_NORMAL);
}

/* 4bpp 8x8 tile: 32 bytes, one nibble per pixel, high nibble leftmost. */
static uint8_t tile_pixel(unsigned tile, unsigned x, unsigned y) {
    uint32_t base = tile * 32u + y * 4u + (x >> 1);
    uint8_t b = VDP.vram[base & 0xFFFF];
    return (x & 1) ? (b & 0x0F) : (b >> 4);
}

/* The window is a third tilemap layer that REPLACES plane A over a
 * rectangular region, and does not scroll. Its extent comes from two
 * registers:
 *
 *   reg17  bits 0-4 = horizontal position in 2-cell units, bit 7 = RIGT
 *          RIGT=0 -> window spans columns [0, pos*2)
 *          RIGT=1 -> window spans columns [pos*2, right edge)
 *   reg18  bits 0-4 = vertical position in cell units, bit 7 = DOWN
 *          DOWN=0 -> window spans rows [0, pos)
 *          DOWN=1 -> window spans rows [pos, bottom)
 *
 * Its nametable rows are always 64 entries wide in H40 and 32 in H32,
 * independent of the plane-size register.
 *
 * Missing this made the house-select screen render as a handful of stray
 * tiles: the game had drawn the entire screen to the window, so plane A was
 * legitimately almost empty and nothing was wrong except that we never looked
 * at the layer holding the picture.
 */
static int window_covers(int cx, int cy) {
    unsigned r17 = VDP.reg[17], r18 = VDP.reg[18];
    unsigned hpos = (r17 & 0x1F) * 2;
    unsigned vpos = (r18 & 0x1F);
    int in_h = (r17 & 0x80) ? (cx >= (int)hpos) : (cx < (int)hpos);
    int in_v = (r18 & 0x80) ? (cy >= (int)vpos) : (cy < (int)vpos);
    return in_h && in_v;
}

static unsigned sample_window(int px, int py, int *prio) {
    uint32_t base = (uint32_t)(VDP.reg[3] & 0x3E) << 10;
    unsigned pitch = vdp_h40() ? 64u : 32u;
    unsigned tx = (unsigned)px >> 3, ty = (unsigned)py >> 3;
    uint32_t e = base + (ty * pitch + tx) * 2u;
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

/* One entry per visible scanline. hscroll is stored already resolved, since
   its table lives in VRAM and that VRAM can be overwritten later in the frame;
   vsram is copied wholesale because per-2-cell mode indexes up to 40 entries.
   reg11 comes along so the scroll MODE is read as it was at that line too. */
static struct {
    int16_t  hs_a, hs_b;
    uint16_t vsram[VSRAM_SIZE];
    uint8_t  reg11;
} LATCH[FB_H];
static unsigned latched;          /* how many lines this frame have been latched */

void render_frame_begin(void) { latched = 0; }

void render_line_latch(unsigned line) {
    if (line >= FB_H) return;
    LATCH[line].hs_a  = hscroll_for(line, 0);
    LATCH[line].hs_b  = hscroll_for(line, 1);
    LATCH[line].reg11 = VDP.reg[11];
    memcpy(LATCH[line].vsram, VDP.vsram, sizeof LATCH[line].vsram);
    if (line + 1 > latched) latched = line + 1;
}

/* vscroll for a column, out of a latched line rather than live VSRAM. */
static int16_t vscroll_latched(unsigned line, unsigned col, int plane_b) {
    unsigned mode = (LATCH[line].reg11 >> 2) & 1;
    unsigned idx = mode ? ((col >> 4) * 2u) : 0u;
    idx += plane_b ? 1u : 0u;
    return (int16_t)LATCH[line].vsram[idx % VSRAM_SIZE];
}

/* Widescreen anchoring.
 *
 * In GAMEPLAY the HUD is right-anchored and its backdrop lives in plane B: the
 * credits, portraits and minimap frame are sprites at x >= 240, and plane B has
 * a black rectangle punched behind the minimap. Shifting everything right by
 * the FULL extra width keeps the HUD flush against the edge with its backdrop
 * aligned underneath, because sprites and planes move together. The new view
 * then appears on the left, which is what a right-anchored HUD wants.
 *
 * MENUS are 320-wide compositions with nothing to anchor. Shifting them right
 * just leaves them lopsided, so they are centred and the surplus is left as
 * pillarbox, which reads as deliberate rather than broken.
 *
 * Telling the two apart: gameplay is exactly when the HUD is on screen, so look
 * for a high-priority sprite in the sidebar region. That is a direct
 * observation of the thing that matters rather than a guess from scroll values
 * or the window registers, which are identical on every screen in this game. */
/* Which scene are we in?
 *
 * The game's main loop dispatches through a function pointer at $FFFFE002, so
 * that pointer IS the scene identifier -- far more reliable than inferring the
 * scene from what happens to be on screen. Observed across a full playthrough
 * and all three houses:
 *
 *     006D0C  gameplay                 017C32  publisher logos
 *     00608E  gameplay, placing a building   024724  mentat / world map
 *     00B540  gameplay                 024812  house select
 *     000000  cutscenes and transitions      004500  transitions
 *
 * All three houses use 006D0C, so this is not Atreides-specific.
 *
 * DB4A_WIDE_SCENES overrides the gameplay set as comma-separated hex, for a
 * mission or a house that turns out to use a handler not listed here. */
static uint32_t scene_id(void) {
    size_t rl;
    const uint8_t *r = hal_ram_ptr(&rl);
    if (!r || rl < 0x10000) return 0;
    return ((uint32_t)r[0xE002] << 24) | ((uint32_t)r[0xE003] << 16)
         | ((uint32_t)r[0xE004] << 8)  |  (uint32_t)r[0xE005];
}

static int scene_is_gameplay(void) {
    static uint32_t list[16];
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("DB4A_WIDE_SCENES");
        n = 0;
        if (e) {
            char buf[128];
            snprintf(buf, sizeof buf, "%s", e);
            for (char *t = strtok(buf, ","); t && n < 16; t = strtok(NULL, ","))
                list[n++] = (uint32_t)strtoul(t, NULL, 16);
        }
        if (!n) { list[0] = 0x006D0Cu; list[1] = 0x00608Eu; list[2] = 0x00B540u; n = 3; }
    }
    uint32_t s = scene_id();
    for (int i = 0; i < n; i++) if (s == list[i]) return 1;
    return 0;
}

int render_widescreen_gameplay(void) { return fb_width > 320 && scene_is_gameplay(); }

int render_world_offset(void) {
    int extra = fb_width - 320;
    if (extra <= 0) return 0;
    return render_widescreen_gameplay() ? extra : extra / 2;
}

void render_frame(void) {
    uint32_t nt_a = (uint32_t)(VDP.reg[2] & 0x38) << 10;
    uint32_t nt_b = (uint32_t)(VDP.reg[4] & 0x07) << 13;
    uint8_t backdrop[3];
    cram_rgb(VDP.cram[VDP.reg[7] & 0x3F], backdrop);

    if (!vdp_display_enabled()) {
        for (int y = 0; y < FB_H; y++)
            for (int x = 0; x < fb_width; x++)
                memset(FB[y][x], 0, 3);
        return;
    }

    for (int y = 0; y < FB_H; y++) {
        /* Fall back to live state for any line that was never latched -- a
           frame rendered outside the normal loop, or before the first latch. */
        int have = ((unsigned)y < latched);
        int16_t hs_a = have ? LATCH[y].hs_a : hscroll_for((unsigned)y, 0);
        int16_t hs_b = have ? LATCH[y].hs_b : hscroll_for((unsigned)y, 1);
        int pillar = render_widescreen_gameplay() ? 0 : (fb_width - 320) / 2;
        for (int x = 0; x < fb_width; x++) {
            if (pillar && (x < pillar || x >= pillar + 320)) {
                memset(FB[y][x], 0, 3);      /* bars, not stretched content */
                continue;
            }
            int16_t vs_a = have ? vscroll_latched((unsigned)y, (unsigned)(x - render_world_offset()), 0)
                                : vscroll_for((unsigned)x, 0);
            int16_t vs_b = have ? vscroll_latched((unsigned)y, (unsigned)(x - render_world_offset()), 1)
                                : vscroll_for((unsigned)x, 1);
            int pa, pb;
            unsigned ca, cb;
            /* Inside the window region the window replaces plane A entirely,
               and is not scrolled. */
            int wx = x - render_world_offset();

            /* Do not let the widescreen extension WRAP around the plane.
             *
             * The planes are a 512-pixel ring. The game's own 320 view uses
             * that wrap legitimately, but the extra columns we add can fall off
             * the left of the map and reappear as the map's far side -- which
             * is the stray lump of terrain reported at the map edge. The game
             * shows plain backdrop there, because there is nothing to the west
             * of the map, and so should we.
             *
             * The test is whether this column sits in the same 512-block as the
             * leftmost column the game itself is drawing. */
            if (x < render_world_offset()) {
                int hs = ((hs_b % 512) + 512) % 512;
                if (x - render_world_offset() + hs < 0) {
                    memcpy(FB[y][x], backdrop, 3);
                    continue;
                }
            }
            if (window_covers(wx >> 3, y >> 3))
                ca = sample_window(wx, y, &pa);
            else
                ca = sample_plane(nt_a, wx - hs_a, y + vs_a, &pa);
            cb = sample_plane(nt_b, wx - hs_b, y + vs_b, &pb);

            unsigned pick = 0;
            int hi = 0;
            if      (pa && ca) { pick = ca; hi = 1; }   /* A, high priority */
            else if (pb && cb) { pick = cb; hi = 1; }   /* B, high priority */
            else if (ca)         pick = ca;
            else if (cb)         pick = cb;

            /* Remembered for the sprite pass: a LOW-priority sprite is drawn
               behind a high-priority plane pixel. This is what puts units
               under the fog of war. */
            plane_hi[y][x] = (uint8_t)hi;
            if (hi) render_planehi++;

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
    /* Sprite-to-sprite priority is by position in the link list: the FIRST
     * sprite with an opaque pixel at a position wins, and later sprites never
     * paint over it. Drawing in link order without this mask gets it exactly
     * backwards -- the last sprite wins. Dune stacks a button face (link 9)
     * on top of its red selection border (link 2) at the same coordinates, so
     * without the mask the border vanishes underneath the face. */
    static uint8_t taken[FB_H][FB_W];
    memset(taken, 0, sizeof taken);
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
        int      spr_hi = (att >> 15) & 1;

        for (unsigned cx = 0; cx < hw; cx++) {
            for (unsigned cy = 0; cy < hh; cy++) {
                unsigned tc = tile + (fx ? (hw - 1 - cx) : cx) * hh
                                   + (fy ? (hh - 1 - cy) : cy);
                for (unsigned py = 0; py < 8; py++) {
                    for (unsigned px = 0; px < 8; px++) {
                        int X = sx + (int)(cx * 8 + px) + render_world_offset();
                        int Y = sy + (int)(cy * 8 + py);
                        if (X < 0 || X >= fb_width || Y < 0 || Y >= FB_H) continue;
                        unsigned ix = fx ? 7 - px : px, iy = fy ? 7 - py : py;
                        unsigned c = tile_pixel(tc, ix, iy);
                        if (c && !taken[Y][X]) {
                            /* Sprite-vs-sprite is settled by link order alone,
                               independent of the priority bit, so this pixel
                               is spoken for either way -- a later sprite never
                               shows through, even a high-priority one over a
                               low-priority winner. */
                            taken[Y][X] = 1;
                            /* Sprite-vs-plane is settled by the priority bit:
                               front to back the hardware orders high sprites,
                               high A, high B, LOW SPRITES, low A, low B. */
                            if (spr_hi || !plane_hi[Y][X])
                                cram_rgb(VDP.cram[(pal * 16 + c) & 0x3F], FB[Y][X]);
                            else
                                render_occluded++;   /* hidden behind the plane */
                        }
                    }
                }
            }
        }
        if (!link) break;
        idx = link;
    }
    if (getenv("DB4A_LOG_PRIO")) {
        static int once = 0;
        if (!once++) {
            unsigned lo = 0, hi = 0;
            unsigned i2 = 0;
            for (unsigned n = 0; n < 80; n++) {
                const uint8_t *q = &VDP.vram[(sat + i2 * 8u) & 0xFFFF];
                uint16_t a2 = (uint16_t)((q[4] << 8) | q[5]);
                if ((a2 >> 15) & 1) hi++; else lo++;
                if (!(q[3] & 0x7F)) break;
                i2 = q[3] & 0x7F;
            }
            fprintf(stderr, "[prio] sprites: %u high, %u low\n", hi, lo);
        }
    }
}

int render_write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", fb_width, FB_H);
    /* Row by row: FB is allocated at FB_W but only fb_width is live. */
    for (int y = 0; y < FB_H; y++) fwrite(FB[y], 3, (size_t)fb_width, f);
    fclose(f);
    return 0;
}
