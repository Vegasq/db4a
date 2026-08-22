/* YM2612 envelope checks.
 *
 * A note that never releases is audible as a stuck tone forever, and it is not
 * visible in any register dump -- the register says "key off" while the
 * envelope quietly refuses to fall. These drive the chip through the same
 * register writes a sound driver makes and then look at what the envelope
 * actually did.
 */
#include "ym2612.h"
#include "psg.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>

m68k_t CPU;
static int fails;

static void ok(const char *what, int cond, const char *detail) {
    if (!cond) fails++;
    printf("   %-46s %s%s%s\n", what, cond ? "ok" : "FAIL",
           detail && !cond ? "  " : "", detail && !cond ? detail : "");
}

static void w(unsigned port, uint8_t a, uint8_t v) { ym_write(port, a); ym_write(port | 1, v); }

/* Discard whatever the chip has already produced. Without this a measurement
   taken right after a key-off reads the ring buffer's stale pre-key-off audio
   and reports a note that never released -- which is exactly the bug being
   looked for, so the test would lie in the most misleading direction. */
static void drain(void) {
    int16_t buf[4096];
    while (ym_read_samples(buf, 4096) > 0) { }
}

/* Peak amplitude over `ms` of output, ignoring anything already buffered. */
static long run_ms(unsigned ms) {
    drain();
    uint64_t end = CPU.cycles + (uint64_t)(7600489.0 * ms / 1000.0);
    long peak = 0;
    int16_t buf[4096];
    while (CPU.cycles < end) {
        CPU.cycles += 2000;
        ym_run(CPU.cycles);
        size_t n;
        while ((n = ym_read_samples(buf, 4096)) > 0)
            for (size_t i = 0; i < n; i++) { long v = buf[i] < 0 ? -buf[i] : buf[i]; if (v > peak) peak = v; }
    }
    return peak;
}

/* A plain audible patch on channel 0: algorithm 7 (all four operators are
   carriers), everything at full level, fast attack. */
static void patch(uint8_t rr) {
    ym_reset();
    CPU.cycles = 0;
    for (int op = 0; op < 4; op++) {
        uint8_t base = (uint8_t)(0x30 + op * 4);
        w(0, base,          0x01);      /* DT 0, MUL 1 */
        w(0, base + 0x10,   0x00);      /* TL 0, full volume */
        w(0, base + 0x20,   0x1F);      /* KS 0, AR 31 -- instant attack */
        w(0, base + 0x30,   0x00);      /* DR 0 -- no decay */
        w(0, base + 0x40,   0x00);      /* SR 0 -- hold */
        w(0, base + 0x50,   (uint8_t)(0x00 | rr));  /* SL 0, RR */
    }
    w(0, 0xB0, 0x07);                   /* feedback 0, algorithm 7 */
    w(0, 0xB4, 0xC0);                   /* both speakers */
    w(0, 0xA4, 0x22);                   /* block 4, fnum high */
    w(0, 0xA0, 0x69);                   /* fnum low */
}

int main(void) {
    printf("-- YM2612\n");

    /* Key on must produce sound. */
    patch(15);
    w(0, 0x28, 0xF0);
    long on = run_ms(50);
    ok("key on produces output", on > 500, NULL);

    /* Key off, let the release finish, THEN measure. Measuring across the
       release instead reports its loud first milliseconds and looks like a
       stuck note whatever the chip does. */
    char d[80];
    w(0, 0x28, 0x00);
    run_ms(200);                            /* settle */
    long after = run_ms(50);
    snprintf(d, sizeof d, "(peak still %ld)", after);
    ok("key off at RR 15 is silent 200 ms later", after < 50, d);

    /* A slow release must still be FALLING. RR 1 is rate 3, one envelope step
       per ~19 ms over 1023 steps, so silence is about 20 s away -- the test is
       that it decays, not that it has finished. */
    patch(1);
    w(0, 0x28, 0xF0);
    run_ms(50);
    w(0, 0x28, 0x00);
    run_ms(1000);
    long early = run_ms(50);
    run_ms(8000);
    long late = run_ms(50);
    snprintf(d, sizeof d, "(1 s: %ld, 9 s: %ld)", early, late);
    ok("slow release keeps decaying", late < early / 2, d);

    /* A held note with SR 0 must NOT decay -- that is the hardware behaviour
       and the reason a stuck note is hard to tell from a correct one. */
    patch(15);
    w(0, 0x28, 0xF0);
    run_ms(100);
    long held1 = run_ms(50);
    run_ms(500);
    long held2 = run_ms(50);
    snprintf(d, sizeof d, "(%ld then %ld)", held1, held2);
    ok("held note with SR 0 sustains", held1 > 500 && held2 > held1 / 2, d);

    /* Re-keying a released note must restart it. */
    patch(15);
    w(0, 0x28, 0xF0); run_ms(30);
    w(0, 0x28, 0x00); run_ms(300);
    w(0, 0x28, 0xF0);
    long re = run_ms(50);
    ok("re-keying after release sounds again", re > 500, NULL);

    printf(fails ? "\n%d YM2612 test(s) FAILED\n" : "\nall YM2612 tests pass\n", fails);
    return fails != 0;
}
