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

static uint8_t held[PAD_COUNT];
static uint8_t ctrl[2];      /* direction bits, 1 = output */
static uint8_t th[2] = {1, 1};

void pad_set(int b, int pressed) {
    if (b >= 0 && b < PAD_COUNT) held[b] = pressed ? 1 : 0;
}

void pad_write_ctrl(int port, uint8_t v) { if (port < 2) ctrl[port] = v; }

void pad_write_data(int port, uint8_t v) {
    if (port >= 2) return;
    /* TH only changes when the console has configured it as an output. */
    if (ctrl[port] & 0x40) th[port] = (v >> 6) & 1;
}

uint8_t pad_read_data(int port) {
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
    return b;
}
