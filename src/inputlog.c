#include "inputlog.h"
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *BTN[PAD_COUNT] = {
    "up", "down", "left", "right", "a", "b", "c", "start"
};

static int name_to_button(const char *s) {
    for (int i = 0; i < PAD_COUNT; i++)
        if (!strcmp(s, BTN[i])) return i;
    return -1;
}

/* ---- recording ---- */
static FILE *rec;

void inputlog_record_open(const char *path) {
    rec = fopen(path, "w");
    if (!rec) { perror(path); return; }
    fprintf(rec, "# db4a input recording\n# frame button down\n"
                 "# frame mouse x y   (game pixels)\n");
    printf("recording input to %s\n", path);
}

void inputlog_record(unsigned frame, int button, int down) {
    if (!rec || button < 0 || button >= PAD_COUNT) return;
    fprintf(rec, "%u %s %d\n", frame, BTN[button], down ? 1 : 0);
}

/* Only when it changes: a held pointer would otherwise write a line a frame. */
void inputlog_record_mouse(unsigned frame, int x, int y) {
    static int lx = -99999, ly = -99999;
    if (!rec || (x == lx && y == ly)) return;
    lx = x; ly = y;
    fprintf(rec, "%u mouse %d %d\n", frame, x, y);
}

void inputlog_record_close(void) {
    if (rec) { fclose(rec); rec = NULL; }
}

/* ---- replay ---- */
typedef struct { unsigned frame; int button; int down; } ev_t;
static ev_t *evs;
static unsigned nev, next_ev;

typedef struct { unsigned frame; int x, y; } mev_t;
static mev_t *mevs;
static unsigned nmev, next_mev;
static int have_mouse_x, have_mouse_y, have_mouse;

int inputlog_replay_has_mouse(void) { return nmev > 0; }

/* The pointer HOLDS its position between events, so replay reports the most
   recent one rather than only the frames that carry a line. */
int inputlog_replay_mouse(unsigned frame, int *x, int *y) {
    while (next_mev < nmev && mevs[next_mev].frame <= frame) {
        have_mouse_x = mevs[next_mev].x;
        have_mouse_y = mevs[next_mev].y;
        have_mouse = 1;
        next_mev++;
    }
    if (!have_mouse) return 0;
    *x = have_mouse_x; *y = have_mouse_y;
    return 1;
}

int inputlog_replay_open(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 0; }
    unsigned cap = 256;
    evs = malloc(cap * sizeof *evs);
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned fr; char name[32]; int down;
        { unsigned mf; int mx, my;
          if (sscanf(line, "%u mouse %d %d", &mf, &mx, &my) == 3) {
              static unsigned mcap;
              if (nmev == mcap) { mcap = mcap ? mcap * 2 : 256;
                                  mevs = realloc(mevs, mcap * sizeof *mevs); }
              mevs[nmev].frame = mf; mevs[nmev].x = mx; mevs[nmev].y = my;
              nmev++;
              continue;
          } }
        if (sscanf(line, "%u %31s %d", &fr, name, &down) != 3) continue;
        int b = name_to_button(name);
        if (b < 0) { fprintf(stderr, "replay: unknown button '%s'\n", name); continue; }
        if (nev == cap) { cap *= 2; evs = realloc(evs, cap * sizeof *evs); }
        evs[nev].frame = fr; evs[nev].button = b; evs[nev].down = down;
        nev++;
    }
    fclose(f);
    printf("replaying %u input events from %s (last at frame %u)\n",
           nev, path, nev ? evs[nev-1].frame : 0);
    if (nmev) printf("  plus %u mouse positions (last at frame %u)\n",
                     nmev, mevs[nmev-1].frame);
    return nev > 0 || nmev > 0;
}

void inputlog_replay_frame(unsigned frame) {
    /* Events are in file order, which is frame order for a recording. */
    while (next_ev < nev && evs[next_ev].frame <= frame) {
        pad_set(evs[next_ev].button, evs[next_ev].down);
        next_ev++;
    }
}

unsigned inputlog_replay_last_frame(void) {
    unsigned a = nev ? evs[nev-1].frame : 0;
    unsigned b = nmev ? mevs[nmev-1].frame : 0;
    return a > b ? a : b;
}
