/* Z80 core validation against the Frank Cringle exerciser suite.
 *
 * Verification layer 4: ground truth for the Z80 with no Mega Drive, no sound
 * driver and no 68000 involved. The exercisers run each instruction over a
 * large set of operand/flag permutations and CRC the results, so a failure
 * names the exact instruction group.
 *
 * The programs are CP/M .COM images: loaded at 0x0100, they call BDOS at
 * address 0x0005 for console output and warm-boot to 0x0000 when finished.
 * We emulate only BDOS functions 2 (print char) and 9 (print $-string).
 *
 * src/z80.c takes its bus through externs, so this harness supplies a flat
 * 64 KiB space in place of the Mega Drive memory map -- the CPU core under
 * test is byte-for-byte the one that ships.
 */
#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t MEM[0x10000];

uint8_t z80_read(uint16_t a)             { return MEM[a]; }
void    z80_write(uint16_t a, uint8_t v) { MEM[a] = v; }
uint8_t z80_in(uint16_t port)            { (void)port; return 0xFF; }
void    z80_out(uint16_t port, uint8_t v){ (void)port; (void)v; }

static int bdos(void) {
    if (Z80.c == 2) {
        putchar(Z80.e);
    } else if (Z80.c == 9) {
        uint16_t p = (uint16_t)((Z80.d << 8) | Z80.e);
        for (int guard = 0; guard < 0x10000; guard++) {
            char ch = (char)MEM[p++];
            if (ch == '$') break;
            putchar(ch);
        }
    }
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <image.com> [max_Mcycles]\n", argv[0]); return 2; }
    unsigned long budget = (argc > 2) ? strtoul(argv[2], NULL, 0) : 8000;   /* millions */

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    size_t n = fread(&MEM[0x0100], 1, 0x10000 - 0x0100, f);
    fclose(f);
    printf("loaded %s (%zu bytes) at 0x0100\n", argv[1], n);

    z80_reset();
    Z80.pc = 0x0100;
    Z80.sp = 0xF000;
    /* Warm boot and BDOS entry both just return; we intercept before execution. */
    MEM[0x0000] = 0xC9;
    MEM[0x0005] = 0xC9;

    uint64_t limit = (uint64_t)budget * 1000000ull;
    int done = 0;

    /* Ring buffer of recent PCs. If execution returns to the entry point the
       program has derailed -- the exerciser never restarts itself -- and the
       trail shows what led there. */
    enum { TRAIL = 32 };
    static uint16_t trail[TRAIL];
    unsigned tn = 0;
    int started = 0;

    while (!done && Z80.cycles < limit) {
        if (Z80.pc == 0x0005) { bdos(); }
        else if (Z80.pc == 0x0000) { done = 1; break; }
        else if (Z80.pc == 0x0100 && started) {
            printf("\n\n*** DERAILED: execution returned to the entry point\n");
            printf("AF=%02X%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X\n",
                   Z80.a, Z80.f, Z80.b, Z80.c, Z80.d, Z80.e, Z80.h, Z80.l);
            printf("IX=%04X IY=%04X SP=%04X  iff1=%d im=%d\n",
                   Z80.ix, Z80.iy, Z80.sp, (int)Z80.iff1, Z80.im);
            printf("stack: ");
            for (int i = 0; i < 6; i++)
                printf("%04X ", (uint16_t)(MEM[Z80.sp + i*2] | (MEM[Z80.sp + i*2 + 1] << 8)));
            printf("\nlast %u PCs (most recent last):\n  ", TRAIL);
            unsigned n = tn < TRAIL ? tn : TRAIL;
            for (unsigned i = 0; i < n; i++)
                printf("%04X ", trail[(tn - n + i) % TRAIL]);
            printf("\nopcodes at those PCs:\n  ");
            for (unsigned i = 0; i < n; i++) {
                uint16_t a = trail[(tn - n + i) % TRAIL];
                printf("%04X:%02X%02X ", a, MEM[a], MEM[(uint16_t)(a+1)]);
            }
            printf("\n");
            return 2;
        }
        if (Z80.pc > 0x0100) started = 1;
        trail[tn++ % TRAIL] = Z80.pc;
        z80_step();
    }

    printf("\n%s after %llu cycles\n",
           done ? "completed" : "CYCLE BUDGET EXHAUSTED",
           (unsigned long long)Z80.cycles);
    return done ? 0 : 1;
}
