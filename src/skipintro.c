/* Start skips the opening sequence -- see include/skipintro.h for the shape
 * of it and why it fast-forwards rather than jumping.
 *
 * Finding the landing point. The intro is not one scene: $FFFFE002 holds
 * $00017C32 from frame 351 to 1404 and then goes back to zero, because the
 * planet zoom, the DUNE title and the menu are one routine that waits for
 * vblank itself instead of returning to the dispatcher. So there is no scene
 * value that means "the menu is up", and the screen has to be identified by
 * its code running -- the same trick as house selection and the build console,
 * see include/probe.h.
 *
 * Two block traces, one cut before the menu appeared and one after, differ by
 * 67 blocks, all in $177D4-$17B56. $178C8 is the head of the loop that reads
 * the pad ($4D46 maps pad bits to letters, 'S' for Start) and runs the idle
 * countdown that eventually hands over to the attract demo. It runs once the
 * fade-in is finished and the menu is taking input, which is exactly the
 * moment to stop at.
 */
#include "skipintro.h"
#include "probe.h"
#include "config.h"
#include "system.h"
#include "psg.h"
#include "ym2612.h"
#include <stdio.h>
#include <stdlib.h>

#define TITLE_MENU_LOOP 0x178C8u

/* An unattended boot reaches the menu at frame 2164 and the attract demo takes
 * over at 4016. The cap sits between the two: if the probe somehow never
 * fires, the skip gives up on the title screen rather than winding the game
 * forward into the demo. */
#define SKIP_MAX_FRAMES 3600u

static int enabled;
static int reached;                     /* the menu loop has run at least once */
static int pending;                     /* Start was pressed, skip not run yet */

void skipintro_enable(int on) {
    const char *e = cfg("DB4A_SKIPINTRO");
    enabled = (e && *e == '0') ? 0 : (on ? 1 : 0);
    probe_watch(PROBE_TITLE_MENU, enabled ? TITLE_MENU_LOOP : 0);
}

int skipintro_armed(void) { return enabled && !reached; }

void skipintro_request(void) { if (skipintro_armed()) pending = 1; }

/* The chips keep producing while the skipped frames run. Throw those samples
 * away as they are made: leaving them would either play 44 seconds of intro
 * music after the fact or, more likely, fill the ring and have it drop
 * whichever samples happened to be last. */
static void discard_audio(void) {
    int16_t scratch[4096];
    while (ym_read_samples(scratch, sizeof scratch / sizeof scratch[0])) { }
    while (psg_read_samples(scratch, sizeof scratch / sizeof scratch[0])) { }
}

uint32_t skipintro_step(uint32_t pc, unsigned *frames) {
    if (!enabled) return pc;

    /* Consumed every frame whether or not a skip is pending: a probe slot read
     * late reports a stale screen, and this is the only reader. */
    if (probe_take(PROBE_TITLE_MENU)) reached = 1;

    if (!pending) return pc;
    pending = 0;
    if (reached) return pc;

    unsigned start = *frames;
    printf("skipping the intro...\n");
    fflush(stdout);
    for (unsigned n = 0; n < SKIP_MAX_FRAMES; n++) {
        pc = system_frame(pc);
        (*frames)++;
        discard_audio();
        if (probe_take(PROBE_TITLE_MENU)) { reached = 1; break; }
    }
    if (reached)
        printf("skipped %u frames; the title menu is up at frame %u\n",
               *frames - start, *frames);
    else
        printf("gave up after %u frames without reaching the title menu\n",
               *frames - start);
    return pc;
}
