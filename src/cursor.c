/* Cursor edge-scrolling -- the first game routine this project owns in C.
 *
 * ---------------------------------------------------------------------------
 * The routine
 * ---------------------------------------------------------------------------
 * $706C in the cartridge, reached once per gameplay frame. It decides whether
 * the cursor is far enough from the middle of the screen to scroll the map,
 * and if so moves the map instead of the cursor. Found by pointing the RAM
 * watchpoint at the cursor variable (DB4A_WATCH=FFBF12) and disassembling
 * whatever block kept writing it.
 *
 *   $FFBF12  cursor X, screen pixels    $FFBF14  cursor Y
 *   $FFBF3C  X sub-pixel accumulator    $FFBF3E  Y sub-pixel
 *   $FFBF34  X scroll output            $FFBF36  Y scroll output
 *   $FFE3BE  camera X                   $FFE3C0  camera Y
 *   $FFE3D2  camera X min               $FFE3D4  camera X max
 *   $FFE3CE  camera Y min               $FFE3D0  camera Y max
 *
 * Per axis: if the cursor sits inside a dead zone the scroll output is zero.
 * Outside it, the distance past the threshold becomes a 16.16 velocity of
 * distance/16 pixels per frame, forced to at least 1 and capped at 3. That
 * velocity is subtracted from the cursor -- pulling it back towards the dead
 * zone -- and however far the cursor actually moved is written to the scroll
 * output, so the map moves under a cursor that stays over the same world
 * point. Finally the scroll is clamped so the camera cannot leave the map, and
 * anything trimmed off is handed back to the cursor.
 *
 * The ROM's dead zone is X 120..200 and Y 82..142. On a 320x224 screen that
 * means the map is scrolling unless the cursor is in a box a quarter of the
 * screen wide -- which is a sensible design for a cursor that crawls at three
 * pixels a frame, and completely wrong for a mouse. Pointing anywhere but the
 * middle of the screen scrolled the map and dragged the cursor away from the
 * pointer; that, not the steering, was what made the pointer impossible to
 * follow.
 *
 * ---------------------------------------------------------------------------
 * Why this one first
 * ---------------------------------------------------------------------------
 * Both changes wanted here -- a cursor that can keep up with a mouse, and
 * scrolling that starts at the screen edge rather than a quarter of the way in
 * -- are governed by constants baked into this routine as instruction
 * immediates: two thresholds per axis, a shift, and two speed caps. Patching a
 * dozen immediates in the ROM image would work and would be unreadable. Once
 * the routine is C, they are variables.
 *
 * It is also a good first candidate on its own merits: one entry, one exit, no
 * subroutine calls, all state in named RAM locations, and it runs every frame
 * of every mission so the existing recorded-mission replay exercises it
 * thoroughly.
 *
 * ---------------------------------------------------------------------------
 * Faithful by default
 * ---------------------------------------------------------------------------
 * With DB4A_MOUSE unset this reproduces the ROM exactly, including its cycle
 * counts and its quirks: the 2.75-snaps-to-3.0 speed cap, the off-by-one on
 * the camera maximum, and the fact that a velocity below one pixel is rounded
 * away from zero rather than to it. `make check-native` replays a full
 * recorded mission with the override on and off and requires identical frames.
 */
#include "native.h"
#include "m68k.h"
#include "mouse.h"
#include "cursor.h"
#include <stdlib.h>

#define CUR_X    0xFFBF12u
#define CUR_Y    0xFFBF14u
#define SUB_X    0xFFBF3Cu
#define SUB_Y    0xFFBF3Eu
#define OUT_X    0xFFBF34u
#define OUT_Y    0xFFBF36u
#define CAM_X    0xFFE3BEu
#define CAM_Y    0xFFE3C0u
#define CAM_XMIN 0xFFE3D2u
#define CAM_XMAX 0xFFE3D4u
#define CAM_YMIN 0xFFE3CEu
#define CAM_YMAX 0xFFE3D0u

#define SCREEN_W 320
#define SCREEN_H 224

/* The cartridge's own constants, kept as named values because they are what
 * the faithful path must reproduce and what the modern path departs from. */
#define ROM_X_LO   0x78      /* 120 */
#define ROM_X_HI   0xC8      /* 200 */
#define ROM_Y_LO   0x52      /*  82 */
#define ROM_Y_HI   0x8E      /* 142 */
#define ROM_SHIFT  4         /* distance/16, in 16.16                        */
#define ROM_SNAP   0x2C000   /* anything faster than 2.75 px/frame becomes 3 */
#define ROM_CAP    0x30000   /* 3.0 px/frame                                 */
#define ONE_PX     0x10000

static int16_t rw(uint32_t a)            { return (int16_t)m68k_read16(a); }
static void    ww(uint32_t a, int16_t v) { m68k_write16(a, (uint16_t)v); }

/* Registers and flags are part of the contract, not an implementation detail.
 *
 * An override is spliced into the middle of the cartridge's execution, so it
 * must leave the CPU exactly as the blocks it replaced would have. The first
 * version of this file computed the right RAM and the right cycle count and
 * still diverged after a few thousand frames, because it left d0-d2 and the X
 * flag holding the caller's values. The equivalence checker missed it too --
 * it was comparing RAM, cycles and the exit PC and nothing else.
 *
 * So the arithmetic below goes through the same add16/sub16/cmp16 helpers the
 * generated code uses. That is not decoration: it makes the flags correct by
 * construction rather than by a second reading of the 68000 manual. Only X
 * actually survives to be read -- $71E0 opens with a move.w, which rewrites
 * N, Z, V and C -- but getting X right means knowing which operation ran last
 * on every path, and calling the helper everywhere is cheaper than that case
 * analysis and cannot be got wrong.
 */
#define LO(x)        ((uint16_t)(x))
#define SET_LO(x, v) ((x) = ((x) & 0xFFFF0000u) | (uint16_t)(v))

struct axis {
    uint32_t pos, sub, out, cam, cmin, cmax;
    int16_t  lo, hi;
    int32_t  snap, cap;
    int      shift;      /* distance -> velocity: 16.16 gain of 1 << (16-shift) */
    /* The four cycle counts that differ between the two axes. */
    int c_entry, c_move, c_min_apply, c_max_apply;
};

/* $7092-$70D4: distance past the dead zone -> 16.16 pixels per frame.
 * Identical code and identical cycle counts on both axes. */
static uint32_t velocity(uint32_t d0, const struct axis *a) {
    d0 = (d0 >> 16) | (d0 << 16);            /* swap d0  */
    flags_logic32(d0);
    d0 = asr32(d0, (uint32_t)a->shift);      /* asr.l #4 */
    CPU.cycles += 52;
    cmp32(d0, ONE_PX);
    if (!cond_ge()) {
        CPU.cycles += 32;                                        /* 709E */
        cmp32(d0, (uint32_t)-ONE_PX);
        if (!cond_le()) {
            CPU.cycles += 14;                                    /* 70A8 */
            flags_logic32(d0);                                   /* tst.l */
            if (!cond_eq()) {
                CPU.cycles += 10;                                /* 70AE */
                /* Below a pixel a frame the cursor would never move at all,
                   so this rounds away from zero rather than towards it. */
                if (cond_mi()) { CPU.cycles += 12; d0 = (uint32_t)-ONE_PX; }
                else           { CPU.cycles += 22; d0 = ONE_PX; }
            }
        }
    }
    CPU.cycles += 32;                                            /* 70BE */
    cmp32(d0, (uint32_t)a->snap);
    if (!cond_le()) { CPU.cycles += 12; d0 = (uint32_t)a->cap; }
    CPU.cycles += 32;                                            /* 70CC */
    cmp32(d0, (uint32_t)-a->snap);
    if (!cond_ge()) { CPU.cycles += 12; d0 = (uint32_t)-a->cap; }
    return d0;
}

static void scroll_axis(const struct axis *a) {
    CPU.cycles += a->c_entry;
    uint32_t d0, d1 = CPU.d[1], d2 = CPU.d[2];

    d0 = (uint16_t)rw(a->pos);                    /* moveq #0,d0 ; move.w   */
    flags_logic16(LO(d0));
    cmp16(LO(d0), (uint16_t)a->lo);
    if (cond_lt()) {
        CPU.cycles += 22;                                        /* 7088 */
        SET_LO(d0, sub16(LO(d0), (uint16_t)a->lo));
    } else {
        CPU.cycles += 22;                                        /* 707C */
        cmp16(LO(d0), (uint16_t)a->hi);
        if (!cond_gt()) {
            CPU.cycles += 18;                                    /* 7082 */
            d2 = 0; flags_logic32(0);                            /* moveq  */
            goto apply;
        }
        CPU.cycles += 12;                                        /* 708E */
        SET_LO(d0, sub16(LO(d0), (uint16_t)a->hi));
    }

    d0 = velocity(d0, a);

    /* $70DA: position and sub-pixel are one 16.16 value straddling two words,
       so the move is a single 32-bit subtract across both. */
    CPU.cycles += a->c_move;
    SET_LO(d1, (uint16_t)rw(a->pos));
    SET_LO(d2, LO(d1));                           /* remember where we were */
    flags_logic16(LO(d1));
    d1 = (d1 >> 16) | (d1 << 16);                 /* swap: position on top  */
    flags_logic32(d1);
    SET_LO(d1, (uint16_t)rw(a->sub));
    flags_logic16(LO(d1));
    d1 = sub32(d1, d0);
    ww(a->sub, (int16_t)LO(d1));
    flags_logic16(LO(d1));
    d1 = (d1 >> 16) | (d1 << 16);
    flags_logic32(d1);
    ww(a->pos, (int16_t)LO(d1));
    flags_logic16(LO(d1));
    SET_LO(d2, sub16(LO(d2), LO(d1)));            /* how far it actually moved */

apply:
    /* Whatever the cursor gave up becomes the map's scroll for this frame,
       trimmed so the camera cannot leave the map -- and anything trimmed is
       handed back to the cursor, which is what pins it at the map edge. */
    CPU.cycles += 46;                                            /* 70F4 */
    ww(a->out, (int16_t)LO(d2));
    flags_logic16(LO(d2));
    SET_LO(d2, add16(LO(d2), (uint16_t)rw(a->cam)));
    SET_LO(d2, sub16(LO(d2), (uint16_t)rw(a->cmin)));
    if (!cond_gt()) {
        CPU.cycles += a->c_min_apply;                            /* 7102 */
        ww(a->out, (int16_t)sub16((uint16_t)rw(a->out), LO(d2)));
        ww(a->pos, (int16_t)add16((uint16_t)rw(a->pos), LO(d2)));
        goto done;
    }
    CPU.cycles += 58;                                            /* 710C */
    SET_LO(d2, (uint16_t)rw(a->out));
    flags_logic16(LO(d2));
    SET_LO(d2, add16(LO(d2), (uint16_t)rw(a->cam)));
    SET_LO(d0, (uint16_t)rw(a->cmax));
    flags_logic16(LO(d0));
    /* The far limit is exclusive, hence the subq; the map does stop one pixel
       short of the camera maximum because of it. */
    SET_LO(d0, sub16(LO(d0), 1));
    SET_LO(d2, sub16(LO(d2), LO(d0)));
    if (!cond_lt()) {
        CPU.cycles += a->c_max_apply;                            /* 711E */
        ww(a->out, (int16_t)sub16((uint16_t)rw(a->out), LO(d2)));
        ww(a->pos, (int16_t)add16((uint16_t)rw(a->pos), LO(d2)));
    }
done:
    CPU.d[0] = d0; CPU.d[1] = d1; CPU.d[2] = d2;
}

/* Modern mode: scroll only near the screen edge, and ramp the speed with how
 * far into the margin the pointer is, so easing into the edge creeps and
 * shoving into it runs. The band has to be wider than the cursor's own clamp
 * box or there is no room left to generate a distance at all -- see
 * mouse_clamp_margin(). */
int cursor_scroll_band(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DB4A_MOUSE_EDGE");
        v = e ? atoi(e) : 24;
        if (v < 2)  v = 2;
        if (v > 96) v = 96;
    }
    return v;
}

static int32_t max_speed(void) {
    static int32_t v = -1;
    if (v < 0) {
        const char *e = getenv("DB4A_SCROLL_MAX");
        /* The cartridge caps at 3 px/frame, which suits a cursor nudged by a
           d-pad. Mouse control is already a modern mode, and 6 crosses a
           screen of map in about a second, which is what an RTS feels like. */
        int px = e ? atoi(e) : 6;
        if (px < 1)  px = 1;
        if (px > 15) px = 15;
        v = (int32_t)px << 16;
    }
    return v;
}

uint32_t native_cursor_scroll(void) {
    CPU.a[0] = CUR_X;                          /* the lea the block opens with */

    int     modern = mouse_enabled();
    int     m      = cursor_scroll_band();
    int32_t cap    = modern ? max_speed() : ROM_CAP;
    int32_t snap   = modern ? cap : ROM_SNAP;
    int     shift  = ROM_SHIFT;

    if (modern) {
        /* Pick the gain so that shoving the pointer right into the corner
           reaches the speed cap, given how deep the cursor is allowed to get.
           A fixed gain would either crawl with a narrow band or slam with a
           wide one. */
        int reach = m - mouse_clamp_margin();
        if (reach < 1) reach = 1;
        while (shift > 0 && ((int64_t)reach << (16 - shift)) < cap) shift--;
    }

    struct axis x = { CUR_X, SUB_X, OUT_X, CAM_X, CAM_XMIN, CAM_XMAX,
                      modern ? (int16_t)m : ROM_X_LO,
                      modern ? (int16_t)(SCREEN_W - m) : ROM_X_HI,
                      snap, cap, shift, 58, 72, 30, 20 };
    struct axis y = { CUR_Y, SUB_Y, OUT_Y, CAM_Y, CAM_YMIN, CAM_YMAX,
                      modern ? (int16_t)m : ROM_Y_LO,
                      modern ? (int16_t)(SCREEN_H - m) : ROM_Y_HI,
                      snap, cap, shift, 42, 80, 34, 24 };
    scroll_axis(&x);
    scroll_axis(&y);

    /* $71E0 reloads both scroll outputs from RAM and applies them to the
       camera, so nothing needs to be left in registers beyond what the blocks
       we replaced would have left. */
    return 0x71E0u;
}

static const struct { uint32_t pc; native_fn fn; } TABLE[] = {
    { 0x706Cu, native_cursor_scroll },
};

/* When the override may run.
 *
 * Not always -- and the reason is worth stating, because it is the one real
 * constraint the override mechanism has.
 *
 * m68k_run_until checks the slice deadline BETWEEN blocks. The routine this
 * replaces is eight blocks and about a thousand cycles, and the slice is five
 * hundred, so the cartridge always yields to the Z80 somewhere in the middle
 * of it. An override is one indivisible step, so it cannot yield there: the
 * 68000 runs on to the end and the Z80 gets its slice later. Nothing is
 * computed differently -- the equivalence checker confirms RAM, registers,
 * flags, cycles and exit PC match on every call -- but the two processors
 * interleave at different points, and over a mission that moves sprites by a
 * pixel or two. Measured at 0.62% of pixels at frame 6000, which is the same
 * order as the residual already tracked as task #21.
 *
 * So: faithful runs do not take the override at all, and stay bit-exact. It
 * runs when mouse control is on, where the behaviour is deliberately different
 * anyway. Making it the default would mean either registering one C function
 * per original block -- preserving the yield points but giving up the readable
 * structure that is the whole point -- or a scheduler that interleaves on
 * absolute cycle position rather than block boundaries. The second is the real
 * fix and is worth doing before more routines migrate.
 *
 *   unset  follow mouse control      0  never
 *   1      always                    check  always, plus the differential check
 */
int native_active(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DB4A_NATIVE");
        if (!e)                 v = -2;          /* decided per call, below */
        else if (*e == '0')     v = 0;
        else                    v = 1;
    }
    return v == -2 ? mouse_enabled() : v;
}

int native_checking(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DB4A_NATIVE"); v = (e && *e == 'c') ? 1 : 0; }
    return v;
}

native_fn native_lookup(uint32_t pc) {
    for (unsigned i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++)
        if (TABLE[i].pc == pc) return TABLE[i].fn;
    return NULL;
}
