/* Interactive frontend: SDL2 window, input, 50 Hz PAL pacing.
 *
 * The headless harness in main.c stays as the batch/diff tool; this is the
 * playable build.
 */
#include "m68k.h"
#include "hal.h"
#include "vdp.h"
#include "render.h"
#include "input.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned long m68k_blocks_run;
extern uint32_t m68k_last_unknown;

/* Default bindings. Kept as a table so remapping is a data change, not a code
   change -- the arrow keys and Z/X/C are only defaults. */
typedef struct { SDL_Keycode key; int pad; const char *name; } binding_t;
static binding_t BINDINGS[] = {
    { SDLK_UP,     PAD_UP,    "Up"    },
    { SDLK_DOWN,   PAD_DOWN,  "Down"  },
    { SDLK_LEFT,   PAD_LEFT,  "Left"  },
    { SDLK_RIGHT,  PAD_RIGHT, "Right" },
    { SDLK_z,      PAD_A,     "A"     },
    { SDLK_x,      PAD_B,     "B"     },
    { SDLK_c,      PAD_C,     "C"     },
    { SDLK_RETURN, PAD_START, "Start" },
};
#define NBINDINGS ((int)(sizeof BINDINGS / sizeof BINDINGS[0]))

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

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
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

    hal_set_rom(rom, romlen);
    hal_reset_ram();
    vdp_reset();
    CPU.a[7] = m68k_read32(0); CPU.ssp = CPU.a[7];
    CPU.super = true; CPU.imask = 7;
    uint32_t pc = m68k_read32(4);

    printf("controls: arrows = D-pad, Z/X/C = A/B/C, Enter = Start, Esc = quit\n");

    int running = 1;
    unsigned frames = 0;
    Uint64 t0 = SDL_GetTicks64();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                int down = (e.type == SDL_KEYDOWN);
                if (e.key.keysym.sym == SDLK_ESCAPE) running = 0;
                for (int i = 0; i < NBINDINGS; i++)
                    if (BINDINGS[i].key == e.key.keysym.sym)
                        pad_set(BINDINGS[i].pad, down);
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
                int down = (e.type == SDL_CONTROLLERBUTTONDOWN);
                for (int i = 0; i < NGC; i++)
                    if (GC_MAP[i].b == e.cbutton.button) pad_set(GC_MAP[i].pad, down);
            }
        }

        pc = m68k_run_until(pc, CPU.cycles + PAL_FRAME_CYCLES);
        if (m68k_last_unknown) {
            fprintf(stderr, "no block for PC %06X -- stopping\n", m68k_last_unknown);
            running = 0;
        }
        pc = m68k_interrupt(pc, 6);

        render_frame();
        SDL_UpdateTexture(tex, NULL, FB, FB_W * 3);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        frames++;

        /* Pace to 50 Hz when vsync is not doing it for us. */
        Uint64 target = t0 + (Uint64)(frames * 1000.0 / 50.0);
        Uint64 now = SDL_GetTicks64();
        if (now < target) SDL_Delay((Uint32)(target - now));
    }

    printf("ran %u frames, %lu blocks\n", frames, m68k_blocks_run);
    pad_report();
    SDL_DestroyTexture(tex); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
