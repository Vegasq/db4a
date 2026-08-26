/* Boot harness: load the ROM, reset the CPU, execute, report where it got to. */
#include "m68k.h"
#include "hal.h"
#include "vdp.h"
#include "psg.h"
#include "ym2612.h"
#include "savestate.h"
#include "mouse.h"
#include "buildmenu.h"
#include "menus.h"
#include "render.h"
#include "widescreen.h"
#include "mapview.h"
#include "objects.h"
#include "splash.h"
#include "input.h"
#include "z80.h"
#include "system.h"
#include "invariant.h"
#include "inputlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
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


/* --- tempo instrumentation, see DB4A_LOG_TEMPO ------------------------- */
static unsigned long tempo_n, tempo_lo = ~0UL, tempo_hi;
static double        tempo_sum, tempo_sumsq;

static void tempo_report(void) {
    if (tempo_n < 2) return;
    double mean = tempo_sum / tempo_n;
    double var  = tempo_sumsq / tempo_n - mean * mean;
    printf("  [tempo] %lu frames: timerA/frame mean=%.2f sd=%.2f min=%lu max=%lu\n",
           tempo_n, mean, var > 0 ? sqrt(var) : 0.0, tempo_lo, tempo_hi);
}

static void tempo_sample(unsigned frames) {
    extern unsigned long ta_overflows;
    static unsigned long prev_ov, prev_z;
    static int armed;
    unsigned long ov = ta_overflows - prev_ov;
    unsigned long zc = (unsigned long)(Z80.cycles - prev_z);
    prev_ov = ta_overflows; prev_z = Z80.cycles;
    if (frames <= 400) return;              /* boot, before the music starts */
    if (!armed) { armed = 1; atexit(tempo_report); }
    /* Only meaningful while Timer A is actually running: the driver stops and
       reprograms it constantly, and counting the stopped frames buries the
       signal in zeroes. */
    if (!(ym_timer_ctrl() & 0x01)) return;
    tempo_n++; tempo_sum += ov; tempo_sumsq += (double)ov * ov;
    if (ov < tempo_lo) tempo_lo = ov;
    if (ov > tempo_hi) tempo_hi = ov;
    { const char *e = getenv("DB4A_LOG_TEMPO");
      if (e && e[0] == '2' && tempo_n < 30)
          printf("  [tempo] frame %5u  timerA=%3lu  z80cycles=%6lu  ctrl=%02X period=%u\n",
                 frames, ov, zc, ym_timer_ctrl(), ym_timer_a_period()); }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <rom> [frames]\n", argv[0]); return 2; }
    /* DB4A_SPLASH_PREVIEW=out.ppm draws the start-up notice and exits, so its
       layout can be checked without launching a window. The game's own font is
       no use here: its text is tiles that the cartridge uploads to VRAM once it
       boots, and this runs before that -- VRAM is still entirely zero. */
    { const char *sp = getenv("DB4A_SPLASH_PREVIEW");
      if (sp) { splash_draw(30); render_write_ppm(sp); return 0; } }
    /* Pacing is cycle-based now, so the budget is a frame count: at PAL
       49.70 Hz, 1 frame = 152923 cycles = 20.12 ms of game time. */
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
    /* DB4A_WAV=out.wav captures the PSG for the whole run. The header is
       written with a placeholder length and patched at the end, since the
       sample count is not known until the run finishes. */
    FILE *wav = NULL;
    unsigned long wav_samples = 0;
    {
        const char *wp = getenv("DB4A_WAV");
        if (wp && (wav = fopen(wp, "wb"))) {
            const uint32_t rate = PSG_RATE;
            uint8_t hdr[44] = {0};
            memcpy(hdr, "RIFF", 4); memcpy(hdr + 8, "WAVEfmt ", 8);
            hdr[16] = 16;                     /* fmt chunk size */
            hdr[20] = 1;  hdr[22] = 2;        /* PCM, stereo */
            hdr[24] = (uint8_t)rate; hdr[25] = (uint8_t)(rate >> 8);
            hdr[26] = (uint8_t)(rate >> 16);  hdr[27] = (uint8_t)(rate >> 24);
            uint32_t bps = rate * 4;
            hdr[28] = (uint8_t)bps; hdr[29] = (uint8_t)(bps >> 8);
            hdr[30] = (uint8_t)(bps >> 16); hdr[31] = (uint8_t)(bps >> 24);
            hdr[32] = 4;                      /* block align */
            hdr[34] = 16;                     /* bits per sample */
            memcpy(hdr + 36, "data", 4);
            fwrite(hdr, 1, 44, wav);
        }
    }

    { const char *t = getenv("DB4A_TALL");
      if (t) { int v = atoi(t); if (v >= 224 && v <= FB_H) fb_height = v; } }
    { const char *w = getenv("DB4A_WIDE");
      if (w) { int v = atoi(w);
               if (v >= 320 && v <= FB_W) { fb_width = v;
                   printf("widescreen: %dx%d\n", fb_width, fb_height); } } }

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
    /* A recording replaces the scripted press list entirely: it carries exact
       press and release frames, so nothing has to be inferred. */
    int replaying = 0;
    { const char *rp = getenv("DB4A_REPLAY");
      if (rp && inputlog_replay_open(rp)) {
          replaying = 1;
          unsigned need = inputlog_replay_last_frame() + 600;
          /* Also run past the furthest requested capture, or a shot scheduled
             after the last input silently never happens. */
          for (unsigned k = 0; k < nshots; k++)
              if (shot_at[k] + 60 > need) need = shot_at[k] + 60;
          if (max_frames < need) {
              /* Only extend when no explicit length was asked for. Extending
                 regardless made every "run N frames" measurement silently
                 simulate the whole recording, so a series of runs at different
                 lengths all returned identical numbers. */
              if (argc > 2) {
                  printf("recording runs to frame %u; keeping the requested %u\n",
                         inputlog_replay_last_frame(), max_frames);
              } else {
                  max_frames = need;
                  printf("extending run to %u frames to cover the recording\n", need);
              }
          }
      } }

    /* DB4A_SAVE_AT=frame:path writes a state mid-run, DB4A_LOAD=path resumes
       from one before the run starts. Together they let a round trip be tested
       headlessly: save, keep going, then reload and check the same frame comes
       out identical. */
    unsigned save_at = 0; const char *save_path = NULL; unsigned resume_frame = 0;
    { const char *sa = getenv("DB4A_SAVE_AT");
      if (sa) { char buf[256];
                if (sscanf(sa, "%u:%255s", &save_at, buf) == 2) {
                    static char keep[256]; snprintf(keep, sizeof keep, "%s", buf);
                    save_path = keep; } } }
    { const char *lp = getenv("DB4A_LOAD");
      if (lp) { uint32_t npc = 0;
                uint32_t nf = 0;
                int r = savestate_read(lp, &npc, &nf);
                if (r == 0) { end = npc; resume_frame = nf;
                              printf("resumed from %s at frame %u\n", lp, nf); }
                else printf("could not load %s (code %d)\n", lp, r); } }

    /* Continue the frame count from the state, so input replay and captures
       line up with the original run rather than starting over. */
    for (frames = resume_frame; frames < resume_frame + max_frames; frames++) {
        if (replaying) inputlog_replay_frame(frames);
        /* A recording made with the pointer carries its motion; feed it through
           the same steering the frontend uses, so headless analysis sees the
           session the player actually had. */
        if (replaying && inputlog_replay_has_mouse()) {
            static int armed = 0;
            if (!armed) { mouse_enable(1); menu_enable(1); menus_enable(1); armed = 1; }
            /* A recorded session that used BOTH pointer and keys must not
               have the pointer win after the player let go of it.
               .
               A pointer position HOLDS between events -- that is what a
               pointer does -- so once the mouse stops moving we keep steering
               to its last position forever. The player, meanwhile, moved on to
               the arrow keys, and steering pins the cursor mid-screen where it
               can never reach the scroll band. The session then replays with
               the camera stuck: artifacts_new_render.txt records a westward
               scroll driven by LEFT held from frame 2911, and the replay went
               east instead.
               .
               The frontend already suppresses steering while a key is held.
               Replay has to do the same against the REPLAYED pad, or a mixed
               session cannot be reproduced at all. */
            extern int pad_dir_held(void);
            int dir_held = pad_dir_held();
            int mx, my;
            if (!dir_held && inputlog_replay_mouse(frames, &mx, &my)) {
                int on_console = buildmenu_steer(mx, my);
                int on_menu    = menus_steer(mx, my);
                if (!on_console && !on_menu) mouse_steer(mx, my);
            }
        }
        /* Hold length matters: a menu that advances on each press can consume
           one long hold twice. DB4A_HOLD tunes it. */
        static uint8_t script_held[PAD_COUNT];
        for (unsigned k = 0; k < nscript; k++) {
            if (frames == script[k].at)        { pad_set(script[k].pad, 1); script_held[script[k].pad] = 1; }
            if (frames == script[k].at + hold) { pad_set(script[k].pad, 0); script_held[script[k].pad] = 0; }
        }
        /* What the SCRIPT is holding, which is this harness's stand-in for the
           keyboard state the SDL frontend reads. Steering is suppressed while a
           direction is held, exactly as the frontend suppresses it, so the two
           agree about who owns the d-pad -- and so a fault that only appears
           when the player uses the arrows with the pointer parked is
           reproducible without a display. That is task #26.
           .
           The pad state itself is deliberately NOT the signal: steering used to
           press directions, so gating on the pad self-locked. This tracks only
           what the script asked for, which steering cannot influence. */
        int script_dir = script_held[PAD_UP] || script_held[PAD_DOWN]
                      || script_held[PAD_LEFT] || script_held[PAD_RIGHT];
        if (getenv("DB4A_LOG_OCCLUDE")) {
            extern unsigned long render_occluded, render_planehi;
            static unsigned long prev_occ = 0;
            (void)render_planehi;
            if (render_occluded != prev_occ) {
                printf("  [prio] frame %5u  %lu sprite pixels hidden by plane priority\n",
                       frames, render_occluded - prev_occ);
                prev_occ = render_occluded;
            }
        }
        /* DB4A_LOG_TEMPO: the sound driver paces itself on YM2612 Timer A, so
           the number of overflows per frame IS the tempo. A steady figure means
           steady music; spread means the wobble is in our scheduling rather
           than in the driver's. DB4A_LOG_TEMPO=2 also lists the first frames. */
        if (getenv("DB4A_LOG_TEMPO")) tempo_sample(frames);
        { static int mt = -1; static int tx, ty;
          if (mt < 0) { const char *e = getenv("DB4A_MOUSE_TARGET");
                        mt = (e && sscanf(e, "%d,%d", &tx, &ty) == 2) ? 1 : 0;
                        if (mt) { mouse_enable(1); menu_enable(1); menus_enable(1); } }
          if (mt) {
              /* Steering runs unconditionally here.
               *
               * The SDL frontend gates it on the KEYBOARD state, which is the
               * correct signal: it distinguishes the player asking for a
               * direction from steering's own output. Gating on the pad
               * instead -- as this harness first did -- self-locks, because
               * steering presses a direction, then sees a direction held and
               * declines to run, so it never releases it and the pad sticks
               * for the rest of the session. */
              {
                  /* DB4A_MOUSE_TARGET is given in LOGICAL coordinates, the
                     same space SDL hands the frontend, and converted here the
                     same way. Taking it as game coordinates instead would make
                     this harness the one place that skips the conversion --
                     and a widescreen offset bug would then be invisible to
                     every test while being obvious in play, which is exactly
                     what happened. At 320 the offset is 0 and nothing moves. */
                  int px = tx - render_world_offset(), py = ty;
                  int on_console = 0, on_menu = 0;
                  if (!script_dir) {
                      on_console = buildmenu_steer(px, py);
                      on_menu    = menus_steer(px, py);
                      if (!on_console && !on_menu) mouse_steer(px, py);
                  }

                  /* On a screen nothing claims, steering must leave the pad
                     completely alone or that screen becomes unusable. It may
                     hold a direction during gameplay, and on the screens the
                     console and menu steering own -- driving the d-pad there
                     is the whole point of them. */
                  extern int pad_dir_held(void);
                  uint32_t sc = ((uint32_t)hal_ram_ptr(0)[0xE002] << 24)
                              | ((uint32_t)hal_ram_ptr(0)[0xE003] << 16)
                              | ((uint32_t)hal_ram_ptr(0)[0xE004] << 8)
                              |  hal_ram_ptr(0)[0xE005];
                  int gameplay = (sc == 0x006D0Cu || sc == 0x00608Eu || sc == 0x00B540u);
                  if (!script_dir && !gameplay && !on_console && !on_menu && pad_dir_held()) {
                      printf("  FAIL frame %u: steering held the d-pad in scene %06X\n",
                             frames, sc);
                  }
              }
              if ((frames % 20) == 0) {
                  int cx, cy; mouse_cursor_pos(&cx, &cy);
                  printf("  frame %5u cursor=(%3d,%3d) scene=%06X\n", frames, cx, cy,
                         (unsigned)((hal_ram_ptr(0)[0xE002]<<24)|(hal_ram_ptr(0)[0xE003]<<16)
                                   |(hal_ram_ptr(0)[0xE004]<<8)|hal_ram_ptr(0)[0xE005]));
              }
          } }
        /* DB4A_LOG_SCENE=1 prints the scene pointer whenever it changes.
           The main loop dispatches through the function pointer at $FFFFE002,
           so that value identifies the screen you are looking at -- which is
           how mouse control decides whether it may touch anything. */
        if (getenv("DB4A_LOG_SCENE")) {
            static uint32_t last_scene = 0xFFFFFFFFu;
            const uint8_t *r = hal_ram_ptr(0);
            uint32_t sc = ((uint32_t)r[0xE002] << 24) | ((uint32_t)r[0xE003] << 16)
                        | ((uint32_t)r[0xE004] << 8)  |  r[0xE005];
            if (sc != last_scene) {
                printf("  [scene] frame %5u  %08X\n", frames, sc);
                last_scene = sc;
            }
        }
        end = system_frame(end);
        if (m68k_last_unknown) break;

        static int scene_log = -1;
        if (scene_log < 0) scene_log = getenv("DB4A_SCENE") ? 1 : 0;
        if (scene_log) {
            extern const uint8_t *hal_ram_ptr(size_t *);
            size_t rl; const uint8_t *rp = hal_ram_ptr(&rl);
            uint32_t sp_ = ((uint32_t)rp[0xE002] << 24) | ((uint32_t)rp[0xE003] << 16)
                         | ((uint32_t)rp[0xE004] << 8) | rp[0xE005];
            static uint32_t last = 0xFFFFFFFFu;
            if (sp_ != last) { last = sp_;
                printf("  frame %6u  $FFFFE002 = %06X\n", frames, sp_); }
        }
        if (wav) {
            /* Mix both chips. The YM2612 is stereo with per-channel panning;
               the PSG is mono and goes to both sides. Both produce at the same
               rate off the same cycle count, so a frame's worth from each lines
               up without any resampling here. */
            int16_t ybuf[8192], pbuf[4096];
            size_t yn = ym_read_samples(ybuf, 8192);
            size_t pn = psg_read_samples(pbuf, 4096);
            size_t frames_out = yn / 2;
            if (pn > frames_out) frames_out = pn;
            for (size_t i = 0; i < frames_out; i++) {
                int32_t l = (i * 2 + 1 < yn) ? ybuf[i * 2]     : 0;
                int32_t r = (i * 2 + 1 < yn) ? ybuf[i * 2 + 1] : 0;
                int32_t p = (i < pn) ? pbuf[i] : 0;
                l += p; r += p;
                if (l >  32767) l =  32767;
                if (l < -32768) l = -32768;
                if (r >  32767) r =  32767;
                if (r < -32768) r = -32768;
                int16_t st[2] = { (int16_t)l, (int16_t)r };
                fwrite(st, sizeof st[0], 2, wav);
                wav_samples++;
            }
        }

        if (save_path && frames == save_at) {
            printf(savestate_write(save_path, end, frames + 1) == 0
                   ? "  [frame %5u] state saved to %s\n"
                   : "  [frame %5u] could not save to %s\n", frames, save_path);
        }

        /* Headless renders ONLY at the DB4A_SHOTS frames, which silently
           starves any per-frame diagnostic inside the renderer -- measured
           counters come back as zero and read as "the thing never happens".
           That has cost real debugging time three times. DB4A_RENDER_ALL
           forces a render every frame so those counters mean something. */
        { static int render_all = -1;
          if (render_all < 0) render_all = getenv("DB4A_RENDER_ALL") ? 1 : 0;
          if (render_all) render_frame();
          static int logw = -1;
          if (logw < 0) logw = getenv("DB4A_LOG_WIDE") ? 1 : 0;
          if (render_all && logw) {
              unsigned long g, e; int ha, hb;
              render_wide_stats(&g, &e, &ha, &hb);
              fprintf(stderr, "[wide] frame %5u hs_a=%5d hs_b=%5d guard_px=%lu ext_px=%lu\n",
                      frames, ha, hb, g, e);
          } }

        /* DB4A_LOG_JUMP: acceptance criterion D4 turned into a measurement.
         *
         * "The picture must not jump when a menu or console opens" means the
         * horizontal offset the renderer picks must never change on a frame
         * the player can SEE. It is not enough to look at where the offset
         * changes, because the game fades to black across most of its own
         * screen changes and a swap behind a black screen is invisible. So
         * report each change together with the brightest pixel in the frame
         * before and after it, and call it VISIBLE only when either side is
         * lit. tests/nojump.sh counts the VISIBLE ones.
         *
         * =2 additionally prints offset, peak and scene for EVERY frame,
         * which is how the classification was surveyed in the first place.
         *
         * Needs DB4A_RENDER_ALL=1: headless renders only at the DB4A_SHOTS
         * frames, so without it FB holds whatever the last shot left and every
         * peak reads as that frame's. */
        { static int logj = -1;
          if (logj < 0) { const char *e = getenv("DB4A_LOG_JUMP");
                          logj = e ? atoi(e) : 0;
                          if (e && !logj) logj = 1; }
          if (logj) {
              int peak = 0;
              for (int y = 0; y < fb_height; y++)
                  for (int x = 0; x < fb_width; x++)
                      for (int c = 0; c < 3; c++)
                          if (FB[y][x][c] > peak) peak = FB[y][x][c];
              const uint8_t *rr = hal_ram_ptr(0);
              uint32_t sc = ((uint32_t)rr[0xE002] << 24) | ((uint32_t)rr[0xE003] << 16)
                          | ((uint32_t)rr[0xE004] << 8)  |  rr[0xE005];
              static int last_off = -1, last_peak = 0;
              int off = render_world_offset();
              if (logj > 1)
                  printf("  [jump] frame %5u offset %d peak %3d scene %06X\n",
                         frames, off, peak, sc);
              else if (last_off >= 0 && off != last_off)
                  printf("  [jump] frame %5u offset %d -> %d  peak %d -> %d  scene %06X  %s\n",
                         frames, last_off, off, last_peak, peak, sc,
                         (last_peak || peak) ? "VISIBLE" : "covered-by-black");
              last_off = off; last_peak = peak;
          } }

        for (unsigned k = 0; k < nshots; k++) {
            if (frames == shot_at[k] && shot_prefix) {
                char path[512];
                snprintf(path, sizeof path, "%s.%u.ppm", shot_prefix, frames);
                render_frame();
                if (render_write_ppm(path) == 0)
                    printf("  [frame %5u] captured %s\n", frames, path);
                /* Work RAM alongside the frame, matching refhost's
                   DB4A_RAMDUMP, so a screen difference can be attributed to
                   the renderer or to game logic instead of guessed at. */
                if (getenv("DB4A_RAMDUMP")) {
                    size_t rlen = 0;
                    const uint8_t *rp = hal_ram_ptr(&rlen);
                    snprintf(path, sizeof path, "%s.%u.ram", shot_prefix, frames);
                    FILE *rf = fopen(path, "wb");
                    if (rf) {
                        fwrite(rp, 1, rlen, rf);
                        fclose(rf);
                        printf("  [frame %5u] wrote %s (%zu bytes)\n", frames, path, rlen);
                    }
                }
            }
        }
    }
    printf("frames simulated  : %u  (%.2f s of game time)\n",
           frames, frames / PAL_HZ);
    printf("cycles emulated   : %llu\n", (unsigned long long)CPU.cycles);

    printf("\nblocks executed   : %lu\n", m68k_blocks_run);
    { extern unsigned long z80_irq_taken, z80_irq_dropped;
      printf("z80 IRQ taken/dropped: %lu / %lu  (%.1f%% dropped)\n",
             z80_irq_taken, z80_irq_dropped,
             100.0 * z80_irq_dropped / (double)(z80_irq_taken + z80_irq_dropped + 1)); }
    { extern unsigned long z80_instructions, z80_pchist[65536];
      printf("z80 instructions  : %lu\n", z80_instructions);
      if (getenv("DB4A_Z80HIST")) {
          for (unsigned n = 0; n < 14; n++) {
              unsigned best = 0;
              for (unsigned i = 1; i < 65536; i++) if (z80_pchist[i] > z80_pchist[best]) best = i;
              if (!z80_pchist[best]) break;
              printf("  z80 pc %04X : %10lu  %5.2f%%\n", best, z80_pchist[best],
                     100.0 * z80_pchist[best] / (double)z80_instructions);
              z80_pchist[best] = 0;
          }
      } }
    { extern unsigned long z80_slices, z80_slices_off;
      printf("z80 slices        : %lu, of which bus held: %lu (%.1f%%)\n",
             z80_slices, z80_slices_off,
             z80_slices ? 100.0 * z80_slices_off / z80_slices : 0.0); }
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
    objects_report();
    mapview_report();
    widescreen_check_report();
    vdp_dump();
    psg_report();
    ym_report();
    if (wav) {
        uint32_t data = (uint32_t)(wav_samples * 4), riff = data + 36;
        uint8_t v[4];
        v[0]=(uint8_t)riff; v[1]=(uint8_t)(riff>>8); v[2]=(uint8_t)(riff>>16); v[3]=(uint8_t)(riff>>24);
        fseek(wav, 4, SEEK_SET);  fwrite(v, 1, 4, wav);
        v[0]=(uint8_t)data; v[1]=(uint8_t)(data>>8); v[2]=(uint8_t)(data>>16); v[3]=(uint8_t)(data>>24);
        fseek(wav, 40, SEEK_SET); fwrite(v, 1, 4, wav);
        fclose(wav);
        printf("wrote %lu samples (%.2f s of audio)\n", wav_samples, wav_samples / (double)PSG_RATE);
    }
    vdp_nt_report();
    /* DB4A_VRAM=file writes VRAM followed by CRAM, for offline inspection of
       what the game actually put in the tilemaps. */
    { const char *vp = getenv("DB4A_VRAM");
      if (vp) { FILE *vf = fopen(vp, "wb");
                if (vf) { fwrite(VDP.vram, 1, sizeof VDP.vram, vf);
                          fwrite(VDP.cram, 2, CRAM_SIZE, vf);
                          fclose(vf);
                          printf("dumped VRAM+CRAM to %s\n", vp); } } }
    { extern unsigned long ws_band_calls, ws_band_ext;
      if (ws_band_calls && getenv("DB4A_LOG_WIDE"))
          fprintf(stderr, "[wsb] band-count calls %lu, of which extension-only %lu\n",
                  ws_band_calls, ws_band_ext); }
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
