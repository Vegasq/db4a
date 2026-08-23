/* Block probes -- see include/probe.h for why screens are identified by
 * execution rather than by state. */
#include "probe.h"

uint32_t probe_pc[PROBE_SLOTS];
uint8_t  probe_hit[PROBE_SLOTS];
int      probe_any;

void probe_watch(unsigned slot, uint32_t pc) {
    if (slot >= PROBE_SLOTS) return;
    probe_pc[slot]  = pc;
    probe_hit[slot] = 0;
    probe_any = 0;
    for (unsigned i = 0; i < PROBE_SLOTS; i++)
        if (probe_pc[i]) probe_any = 1;
}

int probe_take(unsigned slot) {
    if (slot >= PROBE_SLOTS) return 0;
    int hit = probe_hit[slot];
    probe_hit[slot] = 0;
    return hit;
}
