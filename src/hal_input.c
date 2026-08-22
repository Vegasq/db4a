/* Mega Drive controller ports.
 *
 * A 3-button pad multiplexes on the TH line (bit 6 of the data port). The
 * console drives TH as an output and reads back six bits, getting a different
 * button set depending on its level:
 *
 *   TH = 1:  D0 Up  D1 Down  D2 Left  D3 Right  D4 B      D5 C
 *   TH = 0:  D0 Up  D1 Down  D2 0     D3 0      D4 A      D5 Start
 *
 * Buttons are ACTIVE LOW -- a 0 bit means pressed. Getting that inverted
 * reads as "every button held", which usually looks like a hung game rather
 * than an input bug.
 */
#include "input.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Input tracing. DB4A_LOG_PAD=1 reports every key press we inject and every
   read the game performs, so "the game ignores input" and "the game never
   asks for input" can be told apart. */
static int log_pad = -1;
/* 0 = off, 1 = presses and any read taken while a button is held, 2 = also the
   TH strobe. Level 1 is the useful one: the strobe happens every frame forever
   and buries the handful of reads that actually matter. */
static int logging(void) {
    if (log_pad < 0) {
        const char *v = getenv("DB4A_LOG_PAD");
        log_pad = !v ? 0 : (v[0] == 'a' || v[0] == '2') ? 2 : 1;
    }
    return log_pad;
}
unsigned long pad_reads, pad_ctrl_writes, pad_data_writes;
/* Log a window of reads rather than the first few: the interesting ones happen
   deep into gameplay, tens of thousands of reads in. */
unsigned long pad_log_from = 0;
static const char *BNAME[PAD_COUNT] = {"Up","Down","Left","Right","A","B","C","Start"};

static uint8_t held[PAD_COUNT];
static uint8_t ctrl[2];      /* direction bits, 1 = output */
static uint8_t th[2] = {1, 1};

void pad_set(int b, int pressed) {
    if (b < 0 || b >= PAD_COUNT) return;
    if (logging() && held[b] != (pressed ? 1 : 0))
        fprintf(stderr, "[pad] %-5s %s\n", BNAME[b], pressed ? "DOWN" : "up");
    held[b] = pressed ? 1 : 0;
}

void pad_write_ctrl(int port, uint8_t v) {
    pad_ctrl_writes++;
    if (logging() >= 2) fprintf(stderr, "[pad] port%d CTRL <- %02X\n", port, v);
    if (port < 2) ctrl[port] = v;
}

void pad_write_data(int port, uint8_t v) {
    pad_data_writes++;
    if (logging() >= 2) fprintf(stderr, "[pad] port%d DATA <- %02X (TH=%d)\n",
                                port, v, (v >> 6) & 1);
    if (port >= 2) return;
    /* TH only changes when the console has configured it as an output. */
    if (ctrl[port] & 0x40) th[port] = (v >> 6) & 1;
}

/* Is anything currently asking for a direction? Used to let scripted or
   keyboard input take precedence over mouse steering. */
int pad_dir_held(void) {
    return held[PAD_UP] || held[PAD_DOWN] || held[PAD_LEFT] || held[PAD_RIGHT];
}

uint8_t pad_read_data(int port) {
    pad_reads++;
    if (port != 0) return 0x7F;            /* no pad in port 2 */
    uint8_t b;
    if (th[0]) {
        b = (uint8_t)((held[PAD_UP]    ? 0 : 1) << 0 |
                      (held[PAD_DOWN]  ? 0 : 1) << 1 |
                      (held[PAD_LEFT]  ? 0 : 1) << 2 |
                      (held[PAD_RIGHT] ? 0 : 1) << 3 |
                      (held[PAD_B]     ? 0 : 1) << 4 |
                      (held[PAD_C]     ? 0 : 1) << 5);
        b |= 0x40;
    } else {
        b = (uint8_t)((held[PAD_UP]    ? 0 : 1) << 0 |
                      (held[PAD_DOWN]  ? 0 : 1) << 1 |
                      /* left/right read as 0 when TH is low */
                      (held[PAD_A]     ? 0 : 1) << 4 |
                      (held[PAD_START] ? 0 : 1) << 5);
    }
    /* Log reads only while something is held: that is the exact moment the
       question "does the game see this button" is answered, and it keeps the
       output to a few lines per press instead of thousands per second. */
    int any = 0;
    for (int i = 0; i < PAD_COUNT; i++) any |= held[i];
    if (logging() && any) {
        static unsigned long shown;
        if (shown < 400) {
            shown++;
            fprintf(stderr, "[pad] port%d READ -> %02X (TH=%d)  held:", port, b, th[port]);
            for (int i = 0; i < PAD_COUNT; i++)
                if (held[i]) fprintf(stderr, " %s", BNAME[i]);
            fprintf(stderr, "\n");
        }
    }
    return b;
}

void pad_report(void) {
    printf("pad reads=%lu ctrl-writes=%lu data-writes=%lu\n",
           pad_reads, pad_ctrl_writes, pad_data_writes);
}
