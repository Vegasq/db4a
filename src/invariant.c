#include "invariant.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>

static int enabled = -1;
static unsigned long hits[INV_COUNT];
static unsigned long total;

void m68k_dump_crash(uint32_t pc);

int invariant_enabled(void) {
    if (enabled < 0) enabled = getenv("DB4A_NO_INVARIANTS") ? 0 : 1;
    return enabled;
}

void invariant_init(void) {
    for (int i = 0; i < INV_COUNT; i++) hits[i] = 0;
    total = 0;
}

void invariant_fail(inv_id id, const char *what, uint32_t got, uint32_t ctx) {
    total++;
    /* Report each site once: a corrupted machine violates the same invariant
       every block, and a flooded log hides the first occurrence. */
    if (hits[id]++ == 0) {
        fprintf(stderr, "\n*** INVARIANT VIOLATED: %s\n", what);
        fprintf(stderr, "    value=%08X context=%08X  (block %lu)\n",
                got, ctx, (unsigned long)CPU.cycles);
        m68k_dump_crash(CPU.pc);
    }
}

unsigned long invariant_violations(void) { return total; }

void invariant_report(void) {
    static const char *names[INV_COUNT] = {
        "SP outside RAM", "PC outside ROM", "SP odd",
        "DMA length implausible", "VDP address out of range", "Z80 PC out of RAM"
    };
    if (!total) { printf("invariants        : all clean\n"); return; }
    printf("invariants        : %lu violations\n", total);
    for (int i = 0; i < INV_COUNT; i++)
        if (hits[i]) printf("   %-28s %lu\n", names[i], hits[i]);
}
