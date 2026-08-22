/* SN76489 PSG.
 *
 * Three tone channels and one noise channel. Each holds a 10-bit period and a
 * 4-bit attenuation, written through one port: a byte with bit 7 set latches a
 * channel and register and carries the low 4 bits, and a following byte with
 * bit 7 clear supplies the upper 6.
 *
 * Clocking: the chip runs at master/15 and divides by a further 16 for the
 * tone counters, so a channel's output frequency is clock / (32 * period).
 * Everything here is driven from 68000 cycles, since that is the clock the
 * rest of the emulator measures time in.
 */
#include "psg.h"
#include "hal.h"
#include <string.h>
#include <stdio.h>

/* PAL: master 53203424 Hz, PSG = master/15, 68000 = master/7. Working in
 * 68000 cycles, one PSG tick (clock/16) takes 15*16/7 = 34.29 of them. */
#define PSG_TICK_NUM 240u        /* 15 * 16 */
#define PSG_TICK_DEN 7u
#define M68K_HZ      7600489u   /* PAL master / 7 */

/* Attenuation is 2 dB per step, 0 loudest and 15 silent. Four channels sum, so
 * each is scaled to leave headroom rather than clipping on a full chord. */
static const int16_t VOL[16] = {
    8191, 6507, 5168, 4105, 3261, 2590, 2057, 1634,
    1298, 1031,  819,  650,  517,  410,  326,    0
};

static struct {
    uint16_t period[4];      /* ch3 holds the noise control bits */
    uint8_t  vol[4];         /* attenuation, 0 = loudest */
    int32_t  counter[4];
    uint8_t  flip[4];        /* tone output flip-flop */
    uint16_t lfsr;
    uint8_t  latch_ch, latch_vol;
    uint64_t cycles;         /* 68000 cycles consumed */
    uint64_t tick_acc;       /* fractional cycles owed to the next chip tick */
    uint64_t samp_acc;       /* fractional cycles owed to the next sample */
} P;

#define RING 16384
static int16_t ring[RING];
static size_t  ring_w, ring_r;

void psg_reset(void) {
    memset(&P, 0, sizeof P);
    for (int i = 0; i < 4; i++) { P.vol[i] = 15; P.period[i] = 1; P.counter[i] = 1; }
    P.lfsr = 0x8000;
    ring_w = ring_r = 0;
}

unsigned long psg_writes;

void psg_write(uint8_t v) {
    psg_writes++;
    if (v & 0x80) {                       /* latch: channel, type, low bits */
        P.latch_ch  = (v >> 5) & 3;
        P.latch_vol = (v >> 4) & 1;
        if (P.latch_vol) {
            P.vol[P.latch_ch] = v & 0x0F;
        } else if (P.latch_ch == 3) {
            P.period[3] = v & 0x07;       /* noise control is 3 bits */
            P.lfsr = 0x8000;              /* writing the control resets it */
        } else {
            P.period[P.latch_ch] = (uint16_t)((P.period[P.latch_ch] & 0x3F0) | (v & 0x0F));
        }
    } else {                              /* data: upper 6 bits */
        if (P.latch_vol) {
            P.vol[P.latch_ch] = v & 0x0F;
        } else if (P.latch_ch == 3) {
            P.period[3] = v & 0x07;
            P.lfsr = 0x8000;
        } else {
            P.period[P.latch_ch] =
                (uint16_t)((P.period[P.latch_ch] & 0x00F) | ((v & 0x3F) << 4));
        }
    }
}

/* Noise shift rate: 0/1/2 divide the clock, 3 takes channel 2's period. */
static int32_t noise_reload(void) {
    switch (P.period[3] & 3) {
    case 0:  return 0x10;
    case 1:  return 0x20;
    case 2:  return 0x40;
    default: return P.period[2] ? P.period[2] : 1;
    }
}

static void tick(void) {
    for (int c = 0; c < 3; c++) {
        if (--P.counter[c] <= 0) {
            /* A period below 2 sits above the audio band; real hardware holds
               the output steady rather than emitting a screech. */
            P.counter[c] = P.period[c] ? P.period[c] : 1;
            if (P.period[c] > 1) P.flip[c] ^= 1;
            else                 P.flip[c] = 1;
        }
    }
    if (--P.counter[3] <= 0) {
        P.counter[3] = noise_reload();
        /* White noise taps bits 0 and 3; periodic noise just rotates. */
        uint16_t bit = (P.period[3] & 4)
                     ? ((P.lfsr & 1) ^ ((P.lfsr >> 3) & 1))
                     : (P.lfsr & 1);
        P.lfsr = (uint16_t)((P.lfsr >> 1) | (bit << 15));
        P.flip[3] = P.lfsr & 1;
    }
}

static int16_t mix(void) {
    int32_t s = 0;
    for (int c = 0; c < 4; c++)
        s += P.flip[c] ? VOL[P.vol[c]] : -VOL[P.vol[c]];
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

void psg_run(uint64_t cycles) {
    /* Walk forward one SAMPLE at a time, ticking the chip along the way.
     *
     * Doing all the ticks first and all the samples afterwards -- which an
     * earlier version did -- makes every sample in a call read the state at the
     * END of that call, so the waveform between calls is thrown away. Low tones
     * survive it because they barely move over one call; anything high aliases
     * into nonsense. The unit test caught this at period 64, reading 211 Hz for
     * a 1732 Hz tone.
     *
     *   one PSG tick = PSG_TICK_NUM / PSG_TICK_DEN 68000 cycles
     *   one sample   = M68K_HZ / PSG_RATE          68000 cycles
     */
    while (P.cycles < cycles) {
        uint64_t need = (M68K_HZ - P.samp_acc + PSG_RATE - 1) / PSG_RATE;
        if (need == 0) need = 1;
        uint64_t step = cycles - P.cycles;
        if (step > need) step = need;

        P.tick_acc += step * PSG_TICK_DEN;
        while (P.tick_acc >= PSG_TICK_NUM) {
            P.tick_acc -= PSG_TICK_NUM;
            tick();
        }
        P.cycles   += step;
        P.samp_acc += step * PSG_RATE;

        if (P.samp_acc >= M68K_HZ) {
            P.samp_acc -= M68K_HZ;
            size_t nw = (ring_w + 1) % RING;
            if (nw != ring_r) { ring[ring_w] = mix(); ring_w = nw; }
        }
    }
}

size_t psg_available(void) {
    return (ring_w + RING - ring_r) % RING;
}

size_t psg_read_samples(int16_t *out, size_t max) {
    size_t n = 0;
    while (n < max && ring_r != ring_w) {
        out[n++] = ring[ring_r];
        ring_r = (ring_r + 1) % RING;
    }
    return n;
}

void psg_report(void) {
    printf("psg     writes=%lu  samples queued=%zu  channels:",
           psg_writes, psg_available());
    for (int c = 0; c < 4; c++)
        printf(" [%d per=%u att=%u]", c, P.period[c], P.vol[c]);
    printf("\n");
}
