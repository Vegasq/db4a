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
    fprintf(rec, "# db4a input recording\n# frame button down\n");
    printf("recording input to %s\n", path);
}

void inputlog_record(unsigned frame, int button, int down) {
    if (!rec || button < 0 || button >= PAD_COUNT) return;
    fprintf(rec, "%u %s %d\n", frame, BTN[button], down ? 1 : 0);
}

void inputlog_record_close(void) {
    if (rec) { fclose(rec); rec = NULL; }
}

/* ---- replay ---- */
typedef struct { unsigned frame; int button; int down; } ev_t;
static ev_t *evs;
static unsigned nev, next_ev;

int inputlog_replay_open(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 0; }
    unsigned cap = 256;
    evs = malloc(cap * sizeof *evs);
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned fr; char name[32]; int down;
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
    return nev > 0;
}

void inputlog_replay_frame(unsigned frame) {
    /* Events are in file order, which is frame order for a recording. */
    while (next_ev < nev && evs[next_ev].frame <= frame) {
        pad_set(evs[next_ev].button, evs[next_ev].down);
        next_ev++;
    }
}

unsigned inputlog_replay_last_frame(void) {
    return nev ? evs[nev-1].frame : 0;
}
