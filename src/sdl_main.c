/* Interactive frontend: SDL2 window, input, 49.70 Hz PAL pacing.
 *
 * The headless harness in main.c stays as the batch/diff tool; this is the
 * playable build.
 */
#include "m68k.h"
#include "hal.h"
#include "vdp.h"
#include "render.h"
#include "psg.h"
#include "ym2612.h"
#include "savestate.h"

/* The FM mix peaks well below full scale; lift it to a usable level.
   DB4A_GAIN overrides for anyone who wants it louder or quieter. */
#define AUDIO_GAIN audio_gain
static int audio_gain = 4;
#include "input.h"
#include "system.h"
#include "invariant.h"
#include "inputlog.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned long m68k_blocks_run;
extern uint32_t m68k_last_unknown;

/* Default bindings. Kept as a table so remapping is a data change, not a code
   change -- the arrow keys and Z/X/C are only defaults. */
/* Matched on the layout-dependent keycode OR the physical scancode. A keycode
   alone is what the CURRENT layout produces, so on a non-US layout the letter
   keys move and a binding silently stops working -- Z/X/C are exactly the keys
   that shift on QWERTZ and AZERTY. Accepting either means the key labelled Z
   works and so does the key in the US-layout Z position. */
typedef struct {
    SDL_Keycode  key;
    SDL_Scancode scan;
    int          pad;
    const char  *name;
} binding_t;
static binding_t BINDINGS[] = {
    { SDLK_UP,     SDL_SCANCODE_UP,     PAD_UP,    "Up"    },
    { SDLK_DOWN,   SDL_SCANCODE_DOWN,   PAD_DOWN,  "Down"  },
    { SDLK_LEFT,   SDL_SCANCODE_LEFT,   PAD_LEFT,  "Left"  },
    { SDLK_RIGHT,  SDL_SCANCODE_RIGHT,  PAD_RIGHT, "Right" },
    { SDLK_q,      SDL_SCANCODE_Q,      PAD_A,     "A"     },
    { SDLK_w,      SDL_SCANCODE_W,      PAD_B,     "B"     },
    { SDLK_e,      SDL_SCANCODE_E,      PAD_C,     "C"     },
    { SDLK_RETURN, SDL_SCANCODE_RETURN, PAD_START, "Start" },
    /* Alternates. Several, deliberately: Z and C went unrecognised on one
       machine while X on the same row worked, and the cause was never pinned
       down, so no single key is allowed to be the only way to press a button. */
    { SDLK_z,      SDL_SCANCODE_Z,      PAD_A,     "A"     },
    { SDLK_x,      SDL_SCANCODE_X,      PAD_B,     "B"     },
    { SDLK_c,      SDL_SCANCODE_C,      PAD_C,     "C"     },
    { SDLK_SPACE,  SDL_SCANCODE_SPACE,  PAD_A,     "A"     },
    { SDLK_LALT,   SDL_SCANCODE_LALT,   PAD_B,     "B"     },
    { SDLK_LSHIFT, SDL_SCANCODE_LSHIFT, PAD_C,     "C"     },
    { SDLK_TAB,    SDL_SCANCODE_TAB,    PAD_START, "Start" },
};
#define NBINDINGS ((int)(sizeof BINDINGS / sizeof BINDINGS[0]))

static const char *PADNAME[PAD_COUNT] = {
    "up", "down", "left", "right", "a", "b", "c", "start"
};

/* DB4A_KEYS="a=q,b=w,start=space" replaces the default key for those buttons.
   Any button named here drops its built-in bindings entirely, so a key that
   the machine will not deliver can be routed around without a rebuild. */
static void apply_key_overrides(void) {
    const char *spec = getenv("DB4A_KEYS");
    if (!spec) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) { fprintf(stderr, "DB4A_KEYS: ignoring '%s'\n", tok); continue; }
        *eq = 0;
        while (*tok == ' ') tok++;
        int pad = -1;
        for (int p = 0; p < PAD_COUNT; p++)
            if (SDL_strcasecmp(tok, PADNAME[p]) == 0) pad = p;
        if (pad < 0) { fprintf(stderr, "DB4A_KEYS: unknown button '%s'\n", tok); continue; }
        SDL_Keycode k = SDL_GetKeyFromName(eq + 1);
        if (k == SDLK_UNKNOWN) {
            fprintf(stderr, "DB4A_KEYS: unknown key '%s'\n", eq + 1);
            continue;
        }
        for (int i = 0; i < NBINDINGS; i++)
            if (BINDINGS[i].pad == pad) {          /* clear the defaults */
                BINDINGS[i].key  = SDLK_UNKNOWN;
                BINDINGS[i].scan = SDL_SCANCODE_UNKNOWN;
            }
        for (int i = 0; i < NBINDINGS; i++)
            if (BINDINGS[i].pad == pad) {
                BINDINGS[i].key  = k;
                BINDINGS[i].scan = SDL_GetScancodeFromKey(k);
                break;
            }
        printf("binding %s -> %s\n", PADNAME[pad], SDL_GetKeyName(k));
    }
}

static const struct { SDL_GameControllerButton b; int pad; } GC_MAP[] = {
    { SDL_CONTROLLER_BUTTON_DPAD_UP,    PAD_UP    },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN,  PAD_DOWN  },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT,  PAD_LEFT  },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT },
    { SDL_CONTROLLER_BUTTON_A,          PAD_A     },
    { SDL_CONTROLLER_BUTTON_B,          PAD_B     },
    { SDL_CONTROLLER_BUTTON_X,          PAD_C     },
    { SDL_CONTROLLER_BUTTON_START,      PAD_START },
};
#define NGC ((int)(sizeof GC_MAP / sizeof GC_MAP[0]))

static uint8_t *load_rom(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
    fclose(f); *len = (size_t)n; return d;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <rom> [scale]\n", argv[0]); return 2; }
    int scale = (argc > 2) ? atoi(argv[2]) : 3;
    if (scale < 1) scale = 1;

    size_t romlen;
    uint8_t *rom = load_rom(argv[1], &romlen);
    if (!rom) return 1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_Window *win = SDL_CreateWindow("Dune: The Battle for Arrakis",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        FB_W * scale, FB_H * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, FB_W, FB_H);        /* integer-ish scaling */
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);

    SDL_GameController *gc = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { gc = SDL_GameControllerOpen(i); break; }
    if (gc) printf("gamepad: %s\n", SDL_GameControllerName(gc));

    uint32_t pc = system_reset(rom, romlen);

    /* Audio is queued rather than pulled from a callback: the emulator already
       produces samples in frame-sized bursts, and queueing keeps all the state
       on this thread. DB4A_MUTE=1 skips opening the device entirely. */
    SDL_AudioDeviceID audio = 0;
    if (!getenv("DB4A_MUTE")) {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq     = PSG_RATE;
        want.format   = AUDIO_S16SYS;
        want.channels = 2;
        want.samples  = 1024;
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!audio)
            fprintf(stderr, "audio: %s (continuing silent)\n", SDL_GetError());
        else {
            SDL_PauseAudioDevice(audio, 0);
            printf("audio: %d Hz, %d channel(s)\n", have.freq, have.channels);
        }
    }

    { const char *g = getenv("DB4A_GAIN"); if (g) { int n = atoi(g); if (n > 0 && n <= 64) audio_gain = n; } }
    apply_key_overrides();
    printf("controls: arrows = D-pad, Q/W/E = A/B/C, Enter = Start, Esc = quit\n");
    printf("          also accepted: Z/X/C, Space = A, Alt = B, Shift = C, Tab = Start\n");
    printf("          remap with DB4A_KEYS=\"a=q,b=w,c=e\"\n");
    printf("          F5 saves a state, F9 loads it (DB4A_STATE sets the path)\n");
    printf("          P pauses, ` or F fast-forwards while held\n");
    printf("          DB4A_LOG_PAD=1 names every key SDL reports\n");

    /* DB4A_RECORD=<file> captures play; DB4A_REPLAY=<file> plays it back. */
    const char *recpath = getenv("DB4A_RECORD");
    const char *reppath = getenv("DB4A_REPLAY");
    if (recpath) inputlog_record_open(recpath);
    int replaying = reppath ? inputlog_replay_open(reppath) : 0;
    if (replaying)
        printf("replaying -- keyboard input is ignored\n");

    int running = 1;
    unsigned frames = 0;
    uint8_t key_state[PAD_COUNT] = {0};   /* what the keyboard asked for last frame */
    int paused = 0;
    static uint8_t held_scan[SDL_NUM_SCANCODES];   /* for the DB4A_LOG_PAD dump */
    Uint64 t0 = SDL_GetTicks64();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                /* Pad state is POLLED once per frame below, not derived from
                   these events. Auto-repeat emits a KEYUP/KEYDOWN pair per
                   repeat: the repeat flag marks the KEYDOWN but NOT the KEYUP,
                   so an event-driven binding released the button mid-hold and
                   never pressed it again -- "A" logged DOWN then immediately up
                   while the key was still held. Polling cannot see repeat at
                   all. Events are still used for quit and for diagnostics. */
                if (e.key.keysym.sym == SDLK_ESCAPE) running = 0;
                if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                    if (e.key.keysym.sym == SDLK_p) {
                        paused = !paused;
                        printf(paused ? "paused\n" : "resumed\n");
                        if (audio) SDL_ClearQueuedAudio(audio);
                    }
                }
                /* Save states. The cartridge has no SRAM, so without these a
                   mission has to be played in one sitting. Emulation is
                   unaffected either way -- this is convenience, not fidelity. */
                if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                    const char *sp = getenv("DB4A_STATE");
                    if (!sp) sp = "build/state.db4a";
                    if (e.key.keysym.sym == SDLK_F5) {
                        printf(savestate_write(sp, pc, frames) == 0
                               ? "state saved to %s\n" : "could not save to %s\n", sp);
                    } else if (e.key.keysym.sym == SDLK_F9) {
                        uint32_t npc = 0;
                        uint32_t nf = 0;
                        int r = savestate_read(sp, &npc, &nf);
                        if (r == 0) { pc = npc; frames = nf;
                                      printf("state loaded from %s (frame %u)\n", sp, nf); }
                        else printf("could not load %s (%s)\n", sp,
                                    r == -2 ? "not a db4a state, or a different version"
                                            : r == -3 ? "truncated or built by a different binary"
                                                      : "no such file");
                    }
                }
                if (e.type == SDL_KEYDOWN && !e.key.repeat && getenv("DB4A_LOG_PAD")) {
                    int known = 0;
                    for (int i = 0; i < NBINDINGS; i++)
                        if (BINDINGS[i].key  == e.key.keysym.sym ||
                            BINDINGS[i].scan == e.key.keysym.scancode) known = 1;
                    if (!known)
                        fprintf(stderr, "[key] unmapped: sym=%s scancode=%s\n",
                                SDL_GetKeyName(e.key.keysym.sym),
                                SDL_GetScancodeName(e.key.keysym.scancode));
                }
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
                int down = (e.type == SDL_CONTROLLERBUTTONDOWN);
                for (int i = 0; i < NGC; i++)
                    if (GC_MAP[i].b == e.cbutton.button && !replaying) {
                        pad_set(GC_MAP[i].pad, down);
                        inputlog_record(frames, GC_MAP[i].pad, down);
                    }
            }
        }

        /* Sample the real keyboard state for this frame. A key that went up and
           back down within one frame (exactly what auto-repeat does) reads as
           held, which is correct: the user never let go. */
        if (!replaying) {
            int nks = 0;
            const Uint8 *ks = SDL_GetKeyboardState(&nks);
            int want[PAD_COUNT];
            for (int p = 0; p < PAD_COUNT; p++) want[p] = 0;
            /* Walk the keys that are actually down and ask SDL what symbol each
               one produces, rather than guessing which scancode a symbol lives
               on. SDL_GetScancodeFromKey answers that second question and can
               disagree with what the key event reported, which is how a key
               that worked event-driven stopped matching when polled. Matching
               on either the produced symbol or the physical position covers
               every layout. */
            for (int i = 0; i < nks && i < SDL_NUM_SCANCODES; i++) {
                if (!ks[i]) continue;
                SDL_Keycode k = SDL_GetKeyFromScancode((SDL_Scancode)i);
                for (int j = 0; j < NBINDINGS; j++)
                    if (BINDINGS[j].scan == (SDL_Scancode)i || BINDINGS[j].key == k)
                        want[BINDINGS[j].pad] = 1;
            }
            if (getenv("DB4A_LOG_PAD"))
                for (int i = 0; i < nks && i < SDL_NUM_SCANCODES; i++) {
                    if (ks[i] == held_scan[i]) continue;
                    held_scan[i] = ks[i];
                    fprintf(stderr, "[key] %-4s scancode=%s sym=%s\n",
                            ks[i] ? "down" : "up",
                            SDL_GetScancodeName((SDL_Scancode)i),
                            SDL_GetKeyName(SDL_GetKeyFromScancode((SDL_Scancode)i)));
                }
            for (int p = 0; p < PAD_COUNT; p++)
                if (want[p] != key_state[p]) {
                    key_state[p] = (uint8_t)want[p];
                    pad_set(p, want[p]);
                    inputlog_record(frames, p, want[p]);
                }
        }

        /* Fast-forward while held: skip the frame delay and drop audio rather
           than queue minutes of it. Purely a frontend convenience -- the
           emulation runs exactly the same frames either way. */
        const Uint8 *ks_ff = SDL_GetKeyboardState(NULL);
        int fast = ks_ff[SDL_SCANCODE_GRAVE] || ks_ff[SDL_SCANCODE_F];

        if (paused) { SDL_Delay(16); continue; }

        if (replaying) inputlog_replay_frame(frames);
        pc = system_frame(pc);
        if (m68k_last_unknown) {
            fprintf(stderr, "no block for PC %06X -- stopping\n", m68k_last_unknown);
            running = 0;
        }

        /* Drain the chip into the device. If the queue runs long -- the
           window was dragged, or a frame took too long -- drop the backlog
           rather than let latency grow without bound. */
        if (audio) {
            /* Mix both chips, same as the headless WAV path. The YM2612 is
               stereo with per-channel panning; the PSG is mono and goes to
               both sides. Reading only the PSG -- which this did at first --
               plays silence, because Dune mutes it and drives everything
               through the FM chip. */
            int16_t ybuf[8192], pbuf[4096], mix[8192];
            size_t yn = ym_read_samples(ybuf, 8192);
            size_t pn = psg_read_samples(pbuf, 4096);
            size_t frames_out = yn / 2;
            if (pn > frames_out) frames_out = pn;
            if (frames_out > 4096) frames_out = 4096;
            for (size_t i = 0; i < frames_out; i++) {
                int32_t l = (i * 2 + 1 < yn) ? ybuf[i * 2]     : 0;
                int32_t r = (i * 2 + 1 < yn) ? ybuf[i * 2 + 1] : 0;
                int32_t p = (i < pn) ? pbuf[i] : 0;
                l = (l + p) * AUDIO_GAIN;
                r = (r + p) * AUDIO_GAIN;
                if (l >  32767) l =  32767;
                if (l < -32768) l = -32768;
                if (r >  32767) r =  32767;
                if (r < -32768) r = -32768;
                mix[i * 2]     = (int16_t)l;
                mix[i * 2 + 1] = (int16_t)r;
            }
            if (frames_out && !fast)
                SDL_QueueAudio(audio, mix, (Uint32)(frames_out * 2 * sizeof mix[0]));
            if (getenv("DB4A_LOG_AUDIO")) {
                static unsigned long tot, calls;
                tot += frames_out; calls++;
                if ((calls % 100) == 0)
                    fprintf(stderr, "[audio] %lu frames queued over %lu video frames, "
                            "queue=%u bytes, ym pending=%zu psg pending=%zu\n",
                            tot, calls, SDL_GetQueuedAudioSize(audio),
                            ym_available(), psg_available());
            }
            /* Drop a backlog rather than let latency grow without bound. */
            if (SDL_GetQueuedAudioSize(audio) > PSG_RATE * 2 * sizeof(int16_t) / 4)
                SDL_ClearQueuedAudio(audio);
        }

        render_frame();
        SDL_UpdateTexture(tex, NULL, FB, FB_W * 3);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        frames++;

        /* Pace to PAL rate when vsync is not doing it for us. */
        /* PAL is 49.7015 Hz, not 50: pacing at 50 runs the game 0.6% fast. */
        Uint64 target = fast ? 0 : t0 + (Uint64)(frames * 1000.0 / PAL_HZ);
        Uint64 now = SDL_GetTicks64();
        if (now < target) SDL_Delay((Uint32)(target - now));
    }

    inputlog_record_close();
    printf("ran %u frames, %lu blocks\n", frames, m68k_blocks_run);
    invariant_report();
    pad_report();
    SDL_DestroyTexture(tex); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
