/* SN76489 checks.
 *
 * The chip is write-only and the game gives no feedback, so without these the
 * only test of the PSG would be "does it sound plausible", which is not a test.
 * Each case drives the documented register protocol and measures the output. */
#include "psg.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

m68k_t CPU;
static int fails;

static void check(const char *what, double got, double want, double tol) {
    double err = fabs(got - want);
    int ok = err <= tol;
    if (!ok) fails++;
    printf("   %-42s got %9.2f want %9.2f  %s\n", what, got, want, ok ? "ok" : "FAIL");
}

/* Run the chip for `ms` and report the fundamental by counting sign changes. */
static double measure_hz(unsigned ms) {
    uint64_t start = CPU.cycles;
    uint64_t end = start + (uint64_t)(7600489.0 * ms / 1000.0);
    int16_t buf[8192];
    int prev = 0, crossings = 0;
    size_t total = 0;
    while (CPU.cycles < end) {
        CPU.cycles += 5000;
        psg_run(CPU.cycles);
        size_t n;
        while ((n = psg_read_samples(buf, 8192)) > 0) {
            for (size_t i = 0; i < n; i++) {
                int cur = buf[i] > 0 ? 1 : (buf[i] < 0 ? -1 : prev);
                if (prev && cur && cur != prev) crossings++;
                prev = cur;
            }
            total += n;
        }
    }
    if (!total) return 0.0;
    double secs = (double)total / PSG_RATE;
    return crossings / 2.0 / secs;
}

static void set_tone(int ch, unsigned period, unsigned att) {
    psg_write((uint8_t)(0x80 | (ch << 5) | (period & 0x0F)));   /* latch + low 4 */
    psg_write((uint8_t)((period >> 4) & 0x3F));                 /* data, upper 6 */
    psg_write((uint8_t)(0x90 | (ch << 5) | (att & 0x0F)));      /* volume */
}

int main(void) {
    printf("-- PSG\n");

    /* A tone channel outputs clock/(32*period); the PSG clock is master/15. */
    const double PSG_CLK = 53203424.0 / 15.0;
    for (unsigned period = 0x40; period <= 0x100; period <<= 1) {
        psg_reset();
        CPU.cycles = 0;
        set_tone(0, period, 0);
        char label[64];
        snprintf(label, sizeof label, "tone period %u", period);
        /* Zero-crossing counting is coarse at 44.1 kHz, so allow 2%. */
        check(label, measure_hz(300), PSG_CLK / (32.0 * period), PSG_CLK / (32.0 * period) * 0.02);
    }

    /* Attenuation 15 is silence, and it must be actual silence, not quiet. */
    psg_reset(); CPU.cycles = 0;
    set_tone(0, 0x80, 15);
    CPU.cycles += 7600489 / 10;
    psg_run(CPU.cycles);
    {
        int16_t buf[8192]; size_t n; long peak = 0;
        while ((n = psg_read_samples(buf, 8192)) > 0)
            for (size_t i = 0; i < n; i++) if (labs(buf[i]) > peak) peak = labs(buf[i]);
        check("attenuation 15 is silent", (double)peak, 0.0, 0.0);
    }

    /* Attenuation steps are 2 dB, so each step is a factor of about 0.794. */
    psg_reset(); CPU.cycles = 0;
    double amp[3];
    for (int a = 0; a < 3; a++) {
        psg_reset(); CPU.cycles = 0;
        set_tone(0, 0x80, (unsigned)a);
        CPU.cycles += 7600489 / 20;
        psg_run(CPU.cycles);
        int16_t buf[8192]; size_t n; long peak = 0;
        while ((n = psg_read_samples(buf, 8192)) > 0)
            for (size_t i = 0; i < n; i++) if (labs(buf[i]) > peak) peak = labs(buf[i]);
        amp[a] = (double)peak;
    }
    check("attenuation step 0->1 ratio", amp[1] / amp[0], 0.794, 0.02);
    check("attenuation step 1->2 ratio", amp[2] / amp[1], 0.794, 0.02);

    /* A silent chip must sit at zero, not at some DC offset: four channels all
       muted should sum to exactly nothing. */
    psg_reset(); CPU.cycles = 0;
    CPU.cycles += 7600489 / 10;
    psg_run(CPU.cycles);
    {
        int16_t buf[8192]; size_t n; long peak = 0;
        while ((n = psg_read_samples(buf, 8192)) > 0)
            for (size_t i = 0; i < n; i++) if (labs(buf[i]) > peak) peak = labs(buf[i]);
        check("reset state is silent", (double)peak, 0.0, 0.0);
    }

    printf(fails ? "\n%d PSG test(s) FAILED\n" : "\nall PSG tests pass\n", fails);
    return fails != 0;
}
