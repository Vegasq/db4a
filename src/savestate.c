/* Whole-machine save states.
 *
 * Every component already keeps its state in one contiguous block, so this is
 * a header plus those blocks written back to back. Each block records its size
 * in the file, and loading checks them: a state written by a build whose
 * structs differ is refused rather than loaded into the wrong layout, which
 * would corrupt the machine in ways that look like emulation bugs.
 *
 * The 68000 PC is passed in and out separately because it lives in the dispatch
 * loop's local variable rather than in the CPU struct.
 */
#include "savestate.h"
#include "m68k.h"
#include "vdp.h"
#include "z80.h"
#include "psg.h"
#include "ym2612.h"
#include <stdio.h>
#include <string.h>

void *hal_ram_state(size_t *len);
void *hal_z80_ram_state(size_t *len);
size_t hal_z80_bus_size(void);
void hal_z80_bus_save(void *p);
void hal_z80_bus_load(const void *p);
void *psg_state(size_t *len);
void *ym_state(size_t *len);

#define SS_MAGIC   0x41344244u        /* "DB4A" little-endian */
#define SS_VERSION 2u

typedef struct {
    uint32_t magic, version, pc;
    uint32_t frame;        /* when it was taken: a resumed run must line up
                              with input replay and anything else counting
                              frames, or it silently diverges */
    uint32_t n_blocks;
} ss_header;

/* Anything whose size differs from the running build means the state was
   written by a different binary, so sizes are the compatibility check. */
static int put(FILE *f, const void *p, size_t n) {
    uint32_t sz = (uint32_t)n;
    return fwrite(&sz, 4, 1, f) == 1 && fwrite(p, 1, n, f) == n;
}

static int get(FILE *f, void *p, size_t n) {
    uint32_t sz = 0;
    if (fread(&sz, 4, 1, f) != 1 || sz != (uint32_t)n) return 0;
    return fread(p, 1, n, f) == n;
}

int savestate_write(const char *path, uint32_t pc, uint32_t frame) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    ss_header h = { SS_MAGIC, SS_VERSION, pc, frame, 7 };
    int ok = fwrite(&h, sizeof h, 1, f) == 1;

    size_t n;
    void *p;
    uint8_t bus[64];

    ok &= put(f, &CPU, sizeof CPU);
    p = hal_ram_state(&n);      ok &= put(f, p, n);
    ok &= put(f, &VDP, sizeof VDP);
    ok &= put(f, &Z80, sizeof Z80);
    p = hal_z80_ram_state(&n);  ok &= put(f, p, n);
    hal_z80_bus_save(bus);      ok &= put(f, bus, hal_z80_bus_size());
    p = psg_state(&n);          ok &= put(f, p, n);
    p = ym_state(&n);           ok &= put(f, p, n);

    fclose(f);
    if (!ok) { remove(path); return -1; }
    return 0;
}

int savestate_read(const char *path, uint32_t *pc, uint32_t *frame) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    ss_header h;
    if (fread(&h, sizeof h, 1, f) != 1 ||
        h.magic != SS_MAGIC || h.version != SS_VERSION) {
        fclose(f);
        return -2;                       /* not ours, or a different version */
    }

    size_t n;
    void *p;
    uint8_t bus[64];
    int ok = 1;

    ok &= get(f, &CPU, sizeof CPU);
    p = hal_ram_state(&n);      ok &= get(f, p, n);
    ok &= get(f, &VDP, sizeof VDP);
    ok &= get(f, &Z80, sizeof Z80);
    p = hal_z80_ram_state(&n);  ok &= get(f, p, n);
    ok &= get(f, bus, hal_z80_bus_size());
    if (ok) hal_z80_bus_load(bus);
    p = psg_state(&n);          ok &= get(f, p, n);
    p = ym_state(&n);           ok &= get(f, p, n);

    fclose(f);
    if (!ok) return -3;                  /* truncated, or a layout mismatch */
    *pc = h.pc;
    if (frame) *frame = h.frame;
    return 0;
}
