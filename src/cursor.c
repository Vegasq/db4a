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
#include "config.h"
#include "m68k.h"
#include "mouse.h"
#include "cursor.h"
#include "render.h"
#include "widescreen.h"
#include <stdlib.h>

#define CUR_X    0xFFBF12u
#define CUR_Y    0xFFBF14u
#define SUB_X    0xFFBF3Cu
#define SUB_Y    0xFFBF3Eu
#define OUT_X    0xFFBF34u
#define OUT_Y    0xFFBF36u
#define CAM_X    0xFFE3BEu
#define CAM_Y    0xFFE3C0u
#define BOUND_MIN_X 0xFFBF1Au
#define BOUND_MIN_Y 0xFFBF1Cu
#define BOUND_MAX_X 0xFFBF1Eu
#define BOUND_MAX_Y 0xFFBF20u
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

/* How far west of the cartridge's own screen the cursor may travel.
 *
 * The widened strip shows real map, and the cursor-to-map-cell conversion at
 * $5518 tolerates a negative cursor X -- it is linear and unclamped. But it
 * MASKS rather than saturates (andi.w #$7f0), so once the sum goes negative it
 * wraps to the far side of the map and picks a cell nowhere near the pointer.
 * Measured: at 40 pixels west the cell was right, at 70 it jumped from 249 to
 * 363.
 *
 * That only happens where there is no map left to point at, and the game
 * already knows where that is: the camera cannot scroll past CAM_XMIN, so the
 * map still available to the west is exactly CAM_X - CAM_XMIN. Clamping the
 * extension to it means the cursor stops at the map's edge instead of wrapping
 * around behind it.
 *
 * Returns 0 unless widescreen gameplay is on, so the faithful path never sees
 * a negative cursor position and check-native keeps verifying these overrides. */
/* The cartridge's own western camera limit, before we move it.
 *
 * CAM_XMIN is set once per mission (block $59DA writes $200) and is the
 * pixel column the camera may not scroll past. Widescreen draws a strip WEST
 * of the camera, so with the limit left alone the camera can walk until that
 * strip hangs off the map and goes black -- "we display a row to the left of
 * the map; we need to stop scrolling when we reach the end of the map".
 *
 * So we take the limit over, the same way mouse.c takes over the cursor's
 * clamp box: remember whatever the cartridge last set, and publish that plus
 * the strip width. The camera then stops a strip-width early and the strip is
 * always looking at real map.
 *
 * The remembered value is also the honest answer to "how much map is there to
 * the west", which is what the cursor extension must be measured against --
 * measuring against the limit we ourselves moved would collapse it to zero at
 * exactly the point it is needed. */
/* The same argument applies BELOW the cartridge's own 224 lines, because a
 * taller view grows downwards. CAM_YMAX bounds the camera so that
 * camera + 224 lands exactly on the map's southern edge; with (fb_height-224)
 * extra lines below that, the camera has to stop that much earlier or the
 * extra lines hang off the map. Measured at the southern limit with a 256-line
 * view: CAM_Y pinned at 1311 either way, so lines 224-255 read cell row 48,
 * the all-zero ring outside the 32x32 playfield, and rendered as backdrop.
 *
 * Both limits are handled the same way and for the same reason, so they share
 * one function. */
static int cam_xmin_rom = -1, cam_ymax_rom = -1;

/* The limit we publish is clamped against the OTHER end of the camera's range,
 * because a view as wide as the map leaves it nowhere to go. Mission 1 and
 * mission 2 both give X 512..1216, which with the 320 the camera frames is a
 * world exactly 1024 pixels across; at fb_width 1024 the extension is 704 and
 * the two limits meet exactly. Meeting is fine -- the camera is simply pinned
 * and the whole map is on screen. CROSSING is not: min above max makes the two
 * clamps in scroll_axis pull opposite ways every frame. A map smaller than
 * this one would cross, so the clamp is here rather than assumed away.
 * With widescreen off both adjustments are 0 and this republishes the
 * cartridge's own values untouched, which is why check-native still passes. */
static void own_camera_limit(int ext, int extra_lines) {
    {   int cur = (int)(int16_t)rw(CAM_XMIN);
        static int published = -1;
        if (cur != published) cam_xmin_rom = cur;   /* the cartridge set it */
        int want = cam_xmin_rom + ext;
        int lim  = (int)(int16_t)rw(CAM_XMAX);
        if (want > lim) want = lim;
        if (want != cur) ww(CAM_XMIN, (int16_t)want);
        published = want;
    }
    {   int cur = (int)(int16_t)rw(CAM_YMAX);
        static int published = -1;
        if (cur != published) cam_ymax_rom = cur;
        int want = cam_ymax_rom - extra_lines;
        int lim  = (int)(int16_t)rw(CAM_YMIN);
        if (want < lim) want = lim;
        if (want != cur) ww(CAM_YMAX, (int16_t)want);
        published = want;
    }
}

int cursor_west_extension(void) {
    if (!mouse_enabled() || !render_widescreen_gameplay()) return 0;
    int want = render_world_offset();
    int floor_ = (cam_xmin_rom >= 0) ? cam_xmin_rom : (int)(int16_t)rw(CAM_XMIN);
    int have = (int)(int16_t)rw(CAM_X) - floor_;
    if (have < 0) have = 0;
    return want < have ? want : have;
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
        const char *e = cfg("DB4A_MOUSE_EDGE");
        v = e ? atoi(e) : 24;
        if (v < 2)  v = 2;
        if (v > 96) v = 96;
    }
    return v;
}

static int32_t max_speed(void) {
    static int32_t v = -1;
    if (v < 0) {
        const char *e = cfg("DB4A_SCROLL_MAX");
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

/* The modern scroll, shared by normal play and by placing a building.
 *
 * Both the cartridge's routines -- $706C in play, $64D2 while placing -- carry
 * their own copy of the same edge-scroll arithmetic, so the replacement is
 * shared the same way rather than written twice. */
static void modern_band_scroll(void) {
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

    /* Widescreen lets the cursor travel WEST of the cartridge's own screen,
       into the strip of map drawn there. The scroll band has to move with it:
       measured from the cartridge's edge it would start a whole strip-width
       further in than the other three sides, which is what "the left scrolls
       when you are far from the edge" was. ext is 0 unless widescreen
       gameplay is on, so the faithful path is untouched. */
    /* Still needed for the camera limits below; the scroll band no longer
       uses it, because it now reads the clamp bound actually in force. */
    int ext = cursor_west_extension();
    (void)ext;

    /* Keep the camera off the part of the map the strip would hang over, west
       and south alike. Done before the scroll below, so the clamp it applies
       is already ours. With widescreen off both are 0 and this republishes the
       cartridge's own values, leaving RAM identical -- which is why
       check-native still passes. */
    int wide = modern && render_widescreen_gameplay();
    own_camera_limit(wide ? render_world_offset() : 0,
                     wide ? fb_height - SCREEN_H : 0);


    struct axis x = { CUR_X, SUB_X, OUT_X, CAM_X, CAM_XMIN, CAM_XMAX,
                      /* Measure the band from where the cursor can actually
                         REACH, not from a fixed screen coordinate.
                         .
                         The clamp box is not reliably ours: $4DA8 is the
                         cartridge's own setter for it and runs every frame,
                         after we have written ours. That never showed while
                         the pointer was driving, because mouse_steer writes
                         the cursor position directly and the box is beside the
                         point. With the ARROW keys it decides everything: the
                         ROM's box pins the cursor at 24 while the threshold
                         sat at m - ext = -56, so it could never be reached and
                         the map would not scroll. Reading the bound actually
                         in force makes the band right whoever last wrote it. */
                      modern ? (int16_t)((int)(int16_t)rw(BOUND_MIN_X) + m) : ROM_X_LO,
                      modern ? (int16_t)((int)(int16_t)rw(BOUND_MAX_X) - m) : ROM_X_HI,
                      snap, cap, shift, 58, 72, 30, 20 };
    struct axis y = { CUR_Y, SUB_Y, OUT_Y, CAM_Y, CAM_YMIN, CAM_YMAX,
                      modern ? (int16_t)((int)(int16_t)rw(BOUND_MIN_Y) + m) : ROM_Y_LO,
                      modern ? (int16_t)((int)(int16_t)rw(BOUND_MAX_Y) - m) : ROM_Y_HI,
                      snap, cap, shift, 42, 80, 34, 24 };
    scroll_axis(&x);
    scroll_axis(&y);
}

uint32_t native_cursor_scroll(void) {
    CPU.a[0] = CUR_X;                          /* the lea the block opens with */
    modern_band_scroll();
    /* $71E0 reloads both scroll outputs from RAM and applies them to the
       camera, so nothing needs to be left in registers beyond what the blocks
       we replaced would have left. */
    return 0x71E0u;
}

/* Placement mode runs its OWN copy of the edge-scroll routine, at $64D2 --
 * same 16.16 velocity, same 2.75-snaps-to-3.0 cap, same $FFBF3C accumulator,
 * same camera clamp as $706C. That is why the override above never affected
 * it, and why the view still lurched when a building became ready to place.
 *
 * Its dead zone is computed rather than fixed: d4 starts as the building's
 * extent from the table at $64F2, and $651E/$6522/$6528 turn that into
 * [120 - extent, 200 - extent] -- the usual 80-pixel window, shifted so the
 * building's centre sits inside it. That shift IS the auto-centring: with a
 * d-pad it helpfully brings the outline into view, and with a mouse it drags
 * the map out from under a pointer that is already where the player is
 * looking.
 *
 * This used to take the outcome the routine produces when the cursor is
 * already inside the dead zone -- NO SCROLL AT ALL -- and rejoin the ROM at
 * $66F4. That killed the auto-centring, which was the point, and with it every
 * other reason the view moves: while a building was waiting to be placed the
 * map could not be scrolled by any means, mouse or arrows, so a site off the
 * current screen could not be reached. Measured by holding left through a
 * placement in data/recordings/tour.txt: CAM_X pinned at 1695 for the whole
 * hold, against 1283 -> 1060 with the cartridge's own routine back.
 *
 * So it now runs the same modern band scroll as normal play and writes its
 * result to the two scroll outputs, rejoining the ROM at $66F4, which reads
 * them and applies them the way it always does. The auto-centring is still
 * gone -- it lived in the dead zone's extent shift, which this does not
 * reproduce -- but the player can scroll again.
 *
 * Skipping the routine outright is not an option: all twelve callers reach
 * $64D2 by bra/beq/jmp rather than bsr/jsr, so it has no return address of its
 * own on the stack to emulate an rts with. Rejoining its tail sidesteps that
 * entirely.
 */
/* DB4A_PLACE_SCROLL: unset follows mouse control, 0 forces this override on,
 * 1 forces the cartridge behaviour back. */
int placement_override_active(void) {
    /* The setting names the CARTRIDGE's behaviour, so the override is its
       inverse: place_scroll=1 puts the re-centring back and takes the override
       off. Unset, it follows mouse control. */
    return !cfg_bool("DB4A_PLACE_SCROLL", !mouse_enabled());
}

uint32_t native_placement_scroll(void) {
    CPU.a[0] = CUR_X;                      /* the lea the block opens with */
    ww(OUT_X, 0);
    ww(OUT_Y, 0);
    modern_band_scroll();
    /* What the routine's own no-scroll path costs, summed from the blocks it
       replaces: $64D2 110, $6516 58, $6528 26, $6530 18, $659E 46, $65B6 58,
       $6600 50, $6610 26, $6618 18, $668A 46, $66A4 58. Approximate now that
       the shared scroll runs and bills its own blocks on top: this override is
       a deliberate behaviour change, so no checker holds it to the cartridge's
       cycle count. */
    CPU.cycles += 514;
    return 0x66F4u;
}

/* `faithful` says whether the override reproduces the cartridge exactly.
 * make check-native verifies only those -- comparing a deliberate behaviour
 * change against the code it deliberately differs from would fail by
 * construction, and a checker that is expected to fail is worth nothing.
 * A modern override is also not registered at all unless it is switched on,
 * so a faithful run never takes it. */
static const struct { uint32_t pc; native_fn fn; int faithful; } TABLE[] = {
    { 0x706Cu, native_cursor_scroll,    1 },
    { 0x64D2u, native_placement_scroll, 0 },
};

/* When the override may run.
 *
 * Always, unless DB4A_NATIVE=0. Faithful overrides are free: replacing the
 * cartridge's blocks with C changes nothing observable.
 *
 * This used to be gated on mouse control, on the belief that an override
 * costs fidelity. The reasoning was that m68k_run_until checks the slice
 * deadline BETWEEN blocks, so collapsing an eight-block, ~1000-cycle routine
 * into one indivisible step moves where the 68000 yields to the Z80. A figure
 * of 0.62% of pixels was recorded against it, and task #23 -- interleave on
 * absolute cycle position -- was raised to fix it before migrating more
 * routines.
 *
 * That measurement was wrong. Re-measured 2026-08-23 across the whole recorded
 * mission, DB4A_NATIVE=0 against =1, at frames 2000/6000/10000/14000/18000:
 * zero differing pixels and zero differing RAM bytes at every one, with the
 * checker confirming the override really did execute (9093 calls, 0
 * mismatched). The original figure was almost certainly taken before the
 * register and flag fixes landed, and read as still-diverging from stale
 * output.
 *
 * The theory was plausible enough to survive unexamined for a while, which is
 * the argument for re-measuring a number before building on it rather than
 * after.
 */
int placement_override_active(void);

int native_active(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DB4A_NATIVE");
        if (!e)                 v = 1;           /* on by default */
        else if (*e == '0')     v = 0;
        else                    v = 1;
    }
    /* Overrides are on by default, so a modern one switched on by itself --
       DB4A_PLACE_SCROLL=0 without DB4A_MOUSE -- is reached without any special
       case here. */
    return v;
}

int native_checking(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DB4A_NATIVE"); v = (e && *e == 'c') ? 1 : 0; }
    return v;
}

native_fn native_lookup(uint32_t pc) {
    for (unsigned i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (TABLE[i].pc != pc) continue;
        /* A modern override is not registered unless it is switched on, so a
           faithful run never takes it and check-native never sees it. */
        if (!TABLE[i].faithful && !placement_override_active()) return NULL;
        return TABLE[i].fn;
    }
    return NULL;
}

int native_faithful_only(uint32_t pc) {
    for (unsigned i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++)
        if (TABLE[i].pc == pc) return TABLE[i].faithful;
    return 1;
}
