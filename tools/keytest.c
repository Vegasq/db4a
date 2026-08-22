/* Standalone SDL keyboard probe.
 *
 * Answers one question: what does SDL report for a given physical key on THIS
 * machine, independent of db4a? Prints both what arrives as an event and what
 * SDL_GetKeyboardState reports as held, because those can disagree -- a
 * compositor that grabs a key, or a layout that remaps it, shows up as one
 * being present and the other not.
 *
 *   cc -o build/keytest tools/keytest.c $(sdl2-config --cflags --libs)
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("db4a key probe - press keys, Esc to quit",
                                     SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     480, 120, SDL_WINDOW_SHOWN);
    if (!w) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return 1; }

    printf("video driver: %s\n", SDL_GetCurrentVideoDriver());
    printf("press Z, X and C (and anything else). Esc quits.\n");
    printf("EVENT lines come from the event queue; HELD lines from "
           "SDL_GetKeyboardState.\n\n");

    int running = 1;
    Uint8 prev[SDL_NUM_SCANCODES];
    memset(prev, 0, sizeof prev);

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                if (e.key.keysym.sym == SDLK_ESCAPE) running = 0;
                printf("EVENT %-4s sym=%-12s scancode=%-12s repeat=%d\n",
                       e.type == SDL_KEYDOWN ? "DOWN" : "UP",
                       SDL_GetKeyName(e.key.keysym.sym),
                       SDL_GetScancodeName(e.key.keysym.scancode),
                       e.key.repeat);
                fflush(stdout);
            }
        }
        int n = 0;
        const Uint8 *ks = SDL_GetKeyboardState(&n);
        for (int i = 0; i < n && i < SDL_NUM_SCANCODES; i++)
            if (ks[i] != prev[i]) {
                prev[i] = ks[i];
                printf("HELD  %-4s scancode=%-12s (index %d)\n",
                       ks[i] ? "down" : "up", SDL_GetScancodeName((SDL_Scancode)i), i);
                fflush(stdout);
            }
        SDL_Delay(16);
    }
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
