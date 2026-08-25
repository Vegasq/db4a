/* The game's object list, read directly. See include/objects.h. */
#include "objects.h"
#include "vdp.h"
#include "m68k.h"
#include <stdlib.h>
#include <stdio.h>

#define OBJ_HEAD    0x00FFF3ACu
#define SAT_SHADOW  0x00FFE428u
#define SAT_MAX     0x50
#define BANDS       0x00FFF700u     /* a5 in the emitter: 8 per-band counters */
#define ADJ_TABLE   0x00001164u     /* the eight (dx, dy) pairs at $1164     */
#define CAM_X       0x00FFE3BEu
#define CAM_Y       0x00FFE3C0u

static struct obj_entry pred[SAT_MAX];
/* Provenance, for diagnosing a mismatch: which object and piece produced it. */
static uint32_t pred_obj[SAT_MAX], pred_piece[SAT_MAX];
static int      pred_ox[SAT_MAX], pred_screen[SAT_MAX];
static unsigned npred;

static unsigned long chk_frames, chk_entries, chk_bad, chk_count_bad;

/* A private copy of the band counters, so predicting never touches the
   cartridge's. The emitter clears them at $1090..$10A0 before it starts. */
static uint8_t band_n[9], band_w[9];   /* index band+1, since band can be -1 */

/* Should this object be drawn at all?  $10BA..$1120, read-only.
 *
 * The blink logic mutates -- it decrements $4(a2) and toggles bit 4 of the
 * flags. We must not, so the counter is read and the SAME decision is derived
 * without writing it back. That is only sound because this runs BEFORE the
 * cartridge's own pass, on state it has not yet advanced. */
static int obj_visible(uint32_t o) {
    unsigned f = m68k_read8(o + 7);
    if (f & 0x80u) return 0;               /* $10BE  hidden outright        */
    if (!(f & 0x20u)) return 1;            /* $10C8  no blink, always drawn */
    unsigned ctr = m68k_read8(o + 4);
    if (f & 0x10u) {                       /* $10D6  currently blinked out  */
        return ctr ? 0 : 1;                /* expires -> drawn again        */
    }
    return ctr ? 1 : 0;                    /* expires -> skipped one frame  */
}

/* Object to screen, $1124..$1188. */
static void obj_screen(uint32_t o, int *ox, int *oy) {
    unsigned f = m68k_read8(o + 7);
    uint32_t pos = m68k_read32(o);
    int d4 = (int16_t)m68k_read16(pos);
    int d5 = (int16_t)m68k_read16(pos + 2);
    if (!(f & 0x40u)) {                              /* $112A world-space   */
        int t = d4; d4 = d5; d5 = t;                 /* $1132 exg           */
        d4 = (int16_t)(((uint16_t)d4) >> 3);
        d5 = (int16_t)(((uint16_t)d5) >> 3);
        d4 -= (int)(int16_t)m68k_read16(CAM_X);
        d5 -= (int)(int16_t)m68k_read16(CAM_Y);
        if (f & 0x04u) {                             /* $1140 adjust        */
            /* $1148 does `lea -$c(a0), a0`, but a0 is already pos+2 -- the
               post-increment at $1126 moved it -- so the index byte is at
               pos + 2 - 12 + $72 = pos + $68. Reading pos + $66 instead picks
               up the neighbouring byte, which differs often enough to shift
               one sprite by one pixel and no more: 1.7% of entries wrong, all
               of them x off by exactly 1. */
            unsigned k = m68k_read8(pos + 0x68u) & 7u;
            d4 += (int16_t)m68k_read16(ADJ_TABLE + k * 4u);
            d5 += (int16_t)m68k_read16(ADJ_TABLE + k * 4u + 2u);
        }
    }
    *ox = d4 + 0x80;                                 /* $1184               */
    *oy = d5 + 0x80;
}

unsigned long obj_calls, obj_dmas;

void objects_predict(void) {
    static int on = -1;
    if (on < 0) on = getenv("DB4A_OBJCHECK") ? 1 : 0;
    if (!on) return;

    obj_calls++;
    npred = 0;
    for (int i = 0; i < 9; i++) { band_n[i] = 0; band_w[i] = 0; }

    /* The list head is the PARAMETER, not a fixed variable.
     *
     * $1088 has twelve call sites, and during gameplay alone two different
     * lists reach it -- $FFFFE6A8 on 368 calls and $FFFFF3B0 on 27. Reading a
     * fixed $FFF3AC gets the common one right and the rest wrong, which is
     * what left 1.78% of entries mismatched and every entry after the first
     * divergence misaligned. a0 holds whichever list this call is for. */
    uint32_t o = CPU.a[0];
    for (int guard = 0; guard < 512 && (o & 0xFFFFu); guard++) {
        if (!obj_visible(o)) goto next;
        {
        int ox, oy;
        obj_screen(o, &ox, &oy);

        uint32_t a4 = m68k_read32(o + 8);
        int pieces = (int16_t)m68k_read16(a4);
        a4 += 2;
        for (int k = 0; k <= pieces && npred < SAT_MAX; k++, a4 += 0xC) {
            int w = (int16_t)m68k_read16(a4);          /* +0  width   */
            int h = (int16_t)m68k_read16(a4 + 2);      /* +2  height  */
            int y = oy + (int16_t)m68k_read16(a4 + 4); /* +4  y off   */
            int x = ox + (int16_t)m68k_read16(a4 + 0xA); /* +$A x off */

            if (x > 0x1BF) continue;                   /* $11A4 */
            if (y > 0x15F) continue;                   /* $11AC */
            if (x + w < 0x80) continue;                /* $11B4 */
            if (y + h < 0x80) continue;                /* $11C0 */

            /* $11C8..$123A, the per-band budget.
             *
             * Two traps here, both of which cost a wrong first attempt. An
             * out-of-range band EMITS rather than drops -- both `blt $123c`
             * and `bge $123c` jump to the write, not the skip. And the band
             * index is a SIGNED arithmetic shift, so a piece just above the
             * screen gives -1, which the cartridge happily uses as an offset
             * one byte below the first counter. Both are legal and both are
             * common. */
            int rel = y - 0x80;
            if (rel < -31) goto emit;                  /* $11CE */
            int band = rel >> 5;                       /* $11D4 asr, signed */
            if (band >= 7) goto emit;                  /* $11D6 */

            int bi = band + 1;                         /* -1..6 -> 0..7 */
            band_n[bi]++;                              /* $11DC */
            band_w[bi] = (uint8_t)(band_w[bi] + (unsigned)((uint16_t)w >> 3));

            {
                unsigned f = m68k_read8(o + 7);
                if (!(f & 0x02u)) goto emit;                    /* $11E8 */
                uint8_t sel = m68k_read8(BANDS + 0x9u + (uint32_t)(int32_t)band);
                if (!sel) goto emit;                            /* $11F0 */
                uint8_t cap_n = m68k_read8(BANDS + 0x21u + (uint32_t)(int32_t)band);
                uint8_t cap_w = m68k_read8(BANDS + 0x19u + (uint32_t)(int32_t)band);
                if (!(sel & 0x80u)) {                           /* $11F6 bpl */
                    if (band_n[bi] <= cap_n) continue;          /* $1200 drop */
                    if (band_w[bi] <= cap_w) continue;          /* $120C drop */
                    goto emit;                                  /* $120E      */
                }
                if (band_n[bi] <= cap_n) goto emit;             /* $1218 */
                if (band_w[bi] <= cap_w) goto emit;             /* $1222 */
                if ((uint8_t)(cap_n + cap_n) >= band_n[bi]) continue;  /* $122E */
                if ((uint8_t)(cap_w + cap_w) >= band_w[bi]) continue;  /* $123A */
            }
        emit:
            pred_obj[npred]    = o;
            pred_piece[npred]  = a4;
            pred_ox[npred]     = ox;
            pred_screen[npred] = (m68k_read8(o + 7) & 0x40u) ? 1 : 0;
            pred[npred].y = y;
            pred[npred].x = x;
            pred[npred].size_link = m68k_read16(a4 + 6);
            {
                uint16_t at = m68k_read16(a4 + 8);
                uint16_t v = (uint16_t)(at & 0x9FFFu);
                v ^= (uint16_t)(m68k_read16(o + 6) & 0xF800u);
                if (at & 0x6000u) { v = (uint16_t)(v & 0x9FFFu); v |= at; }
                else              { v |= (uint16_t)(at & 0x6000u); }
                pred[npred].attr = v;
            }
            npred++;
        }
        }
    next:
        o = 0xFFFF0000u | m68k_read16(o + 0xE);
    }
}

void objects_verify(void) {
    static int on = -1;
    if (on < 0) on = getenv("DB4A_OBJCHECK") ? 1 : 0;
    if (!on) return;

    /* Walk the shadow's link chain, the way the hardware would. */
    unsigned idx = 0, n = 0;
    int bad = 0;
    for (; n < SAT_MAX; n++) {
        uint32_t e = SAT_SHADOW + idx * 8u;
        int y = (int)(m68k_read16(e) & 0x3FFu);
        int x = (int)(m68k_read16(e + 6) & 0x1FFu);
        uint16_t attr = m68k_read16(e + 4);
        if (n < npred) {
            chk_entries++;
            if (y != (pred[n].y & 0x3FF) || x != (pred[n].x & 0x1FF)
                || attr != pred[n].attr) {
                chk_bad++; bad = 1;
                if (getenv("DB4A_OBJDBG")) {
                    static int shown = 0;
                    if (shown < 10) { shown++;
                        uint32_t oo = pred_obj[n], pp = pred_piece[n];
                        uint32_t pos = m68k_read32(oo);
                        fprintf(stderr, "[obj] slot %2u got x=%4d pred x=%4d | obj=%06X flags=%02X screen=%d "
                                        "pos=%06X w0=%04X w1=%04X ox=%d xoff=%d camx=%d\n",
                                n, x, pred[n].x & 0x1FF, oo, m68k_read8(oo + 7), pred_screen[n],
                                pos, m68k_read16(pos), m68k_read16(pos + 2), pred_ox[n],
                                (int16_t)m68k_read16(pp + 0xA),
                                (int)(int16_t)m68k_read16(CAM_X)); }
                }
            }
        }
        unsigned link = m68k_read8(e + 3) & 0x7Fu;
        if (!link) break;
        idx = link;
    }
    n++;
    if (n != npred) chk_count_bad++;
    (void)bad;
    obj_dmas++;
    chk_frames++;
}

void objects_report(void) {
    if (!chk_frames) return;
    fprintf(stderr, "[obj] emitter calls=%lu dmas=%lu\n", obj_calls, obj_dmas);
    fprintf(stderr, "[obj] frames=%lu entries=%lu mismatched=%lu (%.2f%%)  wrong-count frames=%lu\n",
            chk_frames, chk_entries, chk_bad,
            chk_entries ? 100.0 * (double)chk_bad / (double)chk_entries : 0.0,
            chk_count_bad);
}
