/* YM2612 (OPN2).
 *
 * Six FM channels of four operators each, plus an 8-bit DAC that replaces
 * channel 6 when enabled. Dune drives all of its music and effects through
 * this chip -- the PSG is muted at init and never used -- so this is what
 * makes the game audible.
 *
 * Synthesis works in the log domain, as the hardware does. An operator's
 * output is a sine looked up as attenuation, summed with the envelope's
 * attenuation, and converted back to linear once at the end. That keeps the
 * multiply out of the inner loop and matches the chip's own quantisation.
 *
 * Clocking: the YM2612 and the 68000 both run at master/7, so a chip clock IS
 * a 68000 cycle. The chip produces one sample every 144 of them, giving
 * 7600489/144 = 52781 Hz, which is then resampled to PSG_RATE by averaging.
 */
#include "ym2612.h"
#include "psg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define FM_CLOCK_DIV 144u
#define M68K_HZ      7600489u

unsigned long ym_writes, ym_keyons, dac_writes;

/* ---------------------------------------------------------------- tables */

/* Attenuation is carried in units of 1/256 of a halving, so a value of 256
 * means "half as loud". logsin holds -log2(sin) in those units for a quarter
 * of a 1024-step sine; the other three quadrants come from mirroring. */
static uint16_t logsin[256];
/* 2^(-i/256) scaled so unity is 8192: the chip's operators are 14-bit
 * signed, and the modulation depth below depends on that scale. */
static uint16_t powtab[256];
static int tables_done;

static void init_tables(void) {
    if (tables_done) return;
    for (int i = 0; i < 256; i++) {
        double s = sin((i + 0.5) * M_PI / 512.0);
        double v = -log(s) / log(2.0) * 256.0;
        logsin[i] = (uint16_t)(v + 0.5);
        powtab[i] = (uint16_t)(pow(2.0, -i / 256.0) * 8192.0 + 0.5);
    }
    tables_done = 1;
}

/* Attenuation (1/256 halvings) to a signed linear amplitude, peak 8192. */
static int32_t att_to_lin(uint32_t att) {
    if (att >= 0x1000) return 0;           /* 16 halvings is silence */
    return powtab[att & 0xFF] >> (att >> 8);
}

/* Detune, indexed by the 5-bit key code. Values are in phase-increment units. */
static const uint8_t dt_tab[4][32] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,8,8},
    {1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16},
    {2,2,2,2,4,4,4,4,4,6,6,6,8,8,8,10,10,12,12,14,16,16,18,20,22,24,26,28,32,32,32,32}
};

/* Envelope increment patterns. Which of the eight applies is chosen by the low
 * two bits of the rate, and how often by the high bits. */
static const uint8_t inc_pat[4][8] = {
    {0,1,0,1,0,1,0,1},
    {0,1,0,1,1,1,0,1},
    {0,1,1,1,0,1,1,1},
    {0,1,1,1,1,1,1,1},
};

/* LFO. One step every N FM samples, giving the eight documented rates from
 * about 3.98 Hz up to 72.2 Hz. */
static const uint8_t LFO_STEP[8] = {108, 77, 71, 67, 62, 44, 8, 5};

/* Amplitude modulation depth: the LFO's 0..126 triangle is shifted right by
 * this before being added to an operator's attenuation. 8 shifts it away
 * entirely, which is the "no AM" case. */
static const uint8_t AMS_SHIFT[4] = {8, 3, 1, 0};

/* Frequency modulation depth, as a scale on the phase increment. The hardware
 * depths are 0, 3.4, 6.7, 10, 14, 20, 40 and 80 cents; 80 cents is a ratio of
 * 1.047, so the largest is about 4.7% and the rest are proportional. */
static const uint16_t FMS_SCALE[8] = {0, 65, 129, 193, 270, 385, 770, 1540};

/* Key-scaling: how much of the key code feeds the envelope rate. */
static const uint8_t ks_shift[4] = {3, 2, 1, 0};

/* ------------------------------------------------------------------ state */

enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE, EG_OFF };

typedef struct {
    /* register-backed */
    uint8_t  dt, mul, tl, ks, ar, am, dr, sr, sl, rr, ssg, ams;
    /* running */
    uint32_t phase;
    uint32_t inc;
    int      state;
    int32_t  env;        /* 0 = loudest, 1023 = silent */
    int32_t  out, prev;  /* last two outputs, for feedback */
} op_t;

typedef struct {
    op_t     op[4];
    uint16_t fnum;
    uint8_t  block, alg, fb, pan, ams, fms;
    uint8_t  keyed;
    int32_t  mem;        /* algorithm scratch */
} ch_t;

static struct {
    ch_t     ch[6];
    uint8_t  reg[2][256];
    uint8_t  addr[2];
    uint8_t  dac_on;
    int16_t  dac;
    uint32_t eg_cnt;
    uint64_t cycles;
    uint64_t fm_acc;     /* 68000 cycles owed to the next FM sample */
    uint64_t out_acc;    /* and to the next output sample */
    int32_t  sum_l, sum_r;
    uint32_t sum_n;
    uint8_t  timer_ctrl;
    uint8_t  lfo_on, lfo_rate;
    uint8_t  lfo_cnt;        /* 0..127, the triangle position */
    uint8_t  lfo_sub;        /* FM samples until the next LFO step */
    uint8_t  lfo_am;         /* 0..126 */
    int8_t   lfo_pm;         /* -32..31 */
    int32_t  dc_x_l, dc_y_l, dc_x_r, dc_y_r;   /* DC blocker history */
    /* Timers. A sound driver paces itself on these overflow flags, so leaving
       them at zero makes the music play at whatever slower rate the driver
       falls back to. Both count at the FM sample rate (chip clock / 144):
       Timer A overflows after 1024 - NA ticks, Timer B after 16*(256 - NB). */
    uint16_t ta_period, ta_count;
    uint8_t  tb_period;
    uint16_t tb_count;
    uint16_t tb_div;
    uint8_t  ta_run, tb_run, ta_flag, tb_flag;
} Y;

#define RING 16384
static int16_t ring[RING * 2];
static size_t  ring_w, ring_r;

/* ------------------------------------------------------------- operators */

/* Key code: block plus the top bits of the F-number, as the chip derives it. */
static unsigned keycode(const ch_t *c) {
    unsigned f11 = (c->fnum >> 10) & 1, f10 = (c->fnum >> 9) & 1;
    unsigned f9 = (c->fnum >> 8) & 1, f8 = (c->fnum >> 7) & 1;
    unsigned n = f11 & (f10 | f9 | f8);
    if (!f11) n = f11 | (f10 & f9 & f8);
    return (c->block << 2) | (f11 << 1) | n;
}

static void refresh_inc(ch_t *c, op_t *o) {
    uint32_t base = ((uint32_t)c->fnum << c->block) >> 1;
    unsigned kc = keycode(c);
    int32_t  dt = dt_tab[o->dt & 3][kc & 31];
    if (o->dt & 4) dt = -dt;
    int32_t inc = (int32_t)base + dt;
    if (inc < 0) inc = 0;
    inc = o->mul ? inc * o->mul : inc >> 1;
    o->inc = (uint32_t)inc & 0x1FFFF;
}

static void refresh_channel(ch_t *c) {
    for (int i = 0; i < 4; i++) refresh_inc(c, &c->op[i]);
}

static void key_on(ch_t *c, int i) {
    op_t *o = &c->op[i];
    if (o->state == EG_OFF || o->state == EG_RELEASE) {
        o->phase = 0;
        o->env   = 1023;
    }
    o->state = EG_ATTACK;
}

static void key_off(ch_t *c, int i) {
    op_t *o = &c->op[i];
    if (o->state != EG_OFF) o->state = EG_RELEASE;
}

/* One envelope step for one operator. */
static void eg_step(const ch_t *c, op_t *o) {
    if (o->state == EG_OFF) return;
    unsigned kc   = keycode(c);
    unsigned rate_base;
    switch (o->state) {
    case EG_ATTACK:  rate_base = o->ar; break;
    case EG_DECAY:   rate_base = o->dr; break;
    case EG_SUSTAIN: rate_base = o->sr; break;
    default:         rate_base = o->rr; break;
    }
    if (rate_base == 0) return;                    /* rate 0 never advances */

    unsigned ksr  = kc >> ks_shift[o->ks & 3];
    unsigned rate = rate_base * 2 + ksr;
    if (rate > 63) rate = 63;

    unsigned shift = (rate < 48) ? (11 - (rate >> 2)) : 0;
    if (Y.eg_cnt & ((1u << shift) - 1u)) return;

    unsigned inc = inc_pat[rate & 3][(Y.eg_cnt >> shift) & 7];
    if (rate >= 48) {                              /* the fastest rates scale up */
        unsigned boost = (rate - 48) >> 2;
        inc <<= boost;
        if (inc > 8) inc = 8;
    }
    if (!inc) return;

    switch (o->state) {
    case EG_ATTACK:
        /* Attack approaches zero attenuation proportionally, which is what
           gives the chip its characteristic curve rather than a ramp. */
        o->env += (int32_t)((~o->env * (int32_t)inc) >> 4);
        if (o->env <= 0) { o->env = 0; o->state = EG_DECAY; }
        break;
    case EG_DECAY:
        o->env += (int32_t)inc;
        if (o->env >= (int32_t)(o->sl == 15 ? 1023 : o->sl * 32)) o->state = EG_SUSTAIN;
        break;
    case EG_SUSTAIN:
    case EG_RELEASE:
        o->env += (int32_t)inc;
        if (o->env >= 1023) { o->env = 1023; if (o->state == EG_RELEASE) o->state = EG_OFF; }
        break;
    }
    if (o->env > 1023) o->env = 1023;
}

/* Operator output. `mod` is the RAW output of whatever feeds this operator,
 * not a pre-scaled phase offset.
 *
 * The hardware adds half the modulator's 14-bit output to the 10-bit phase
 * index, so a full-scale modulator sweeps about four complete cycles. Passing
 * a pre-halved value against a 12-bit operator scale, as this did, made
 * modulation four times too shallow -- which flattens timbre and throws the
 * relative loudness of different patches out, since how bright an FM voice is
 * depends entirely on this depth. */
static int32_t op_out(op_t *o, int32_t mod) {
    uint32_t p = ((o->phase >> 10) + (uint32_t)(mod >> 1)) & 0x3FF;
    uint32_t i = p & 0xFF;
    if (p & 0x100) i ^= 0xFF;
    /* Both attenuations are converted into the log scale used here, which is
       1/256 of a halving (6.02 dB). The envelope is 10 bits over 96 dB, so one
       unit is 4. Total level is 7 bits at 0.75 dB per step, so one unit is 32
       -- it was 16, which made every operator twice as loud as it should be
       and skewed the balance between carriers and modulators. */
    uint32_t env = (uint32_t)o->env;
    if (o->am && o->ams < 4)                  /* tremolo */
        env += (uint32_t)(Y.lfo_am >> AMS_SHIFT[o->ams]);
    uint32_t att = logsin[i] + env * 4 + (uint32_t)(o->tl * 32);
    int32_t  v   = att_to_lin(att);
    return (p & 0x200) ? -v : v;
}

/* ------------------------------------------------------------- one sample */

static void fm_sample(int32_t *L, int32_t *R) {
    int32_t l = 0, r = 0;
    for (int ci = 0; ci < 6; ci++) {
        ch_t *c = &Y.ch[ci];
        int32_t out;

        if (ci == 5 && Y.dac_on) {
            out = Y.dac * 32;   /* 8-bit sample against 14-bit operators */
        } else {
            op_t *o0 = &c->op[0], *o1 = &c->op[1], *o2 = &c->op[2], *o3 = &c->op[3];

            /* Operator 1 is modulated by its own two previous outputs. */
            int32_t fb = 0;
            if (c->fb) fb = (o0->prev + o0->out) >> (10 - c->fb);
            int32_t m1 = op_out(o0, fb);
            o0->prev = o0->out; o0->out = m1;

            int32_t a, b, cc;
            switch (c->alg) {
            case 0:  a = op_out(o1, m1); b = op_out(o2, a);      out = op_out(o3, b); break;
            case 1:  a = op_out(o1, 0);  b = op_out(o2, m1 + a); out = op_out(o3, b); break;
            case 2:  a = op_out(o1, 0);  b = op_out(o2, a);      out = op_out(o3, m1 + b); break;
            case 3:  a = op_out(o1, m1); b = op_out(o2, 0);      out = op_out(o3, a + b); break;
            case 4:  a = op_out(o1, m1); b = op_out(o2, 0);      cc = op_out(o3, b); out = a + cc; break;
            case 5:  a = op_out(o1, m1); b = op_out(o2, m1);     cc = op_out(o3, m1); out = a + b + cc; break;
            case 6:  a = op_out(o1, m1); b = op_out(o2, 0);      cc = op_out(o3, 0); out = a + b + cc; break;
            default: a = op_out(o1, 0);  b = op_out(o2, 0);      cc = op_out(o3, 0); out = m1 + a + b + cc; break;
            }
        }

        if (c->pan & 0x80) l += out;
        if (c->pan & 0x40) r += out;
    }
    *L = l; *R = r;
}

static void timers_tick(void) {
    if (Y.ta_run) {
        if (++Y.ta_count >= 1024u) {
            Y.ta_count = Y.ta_period;
            if (Y.timer_ctrl & 0x04) Y.ta_flag = 1;
        }
    }
    if (Y.tb_run) {
        if (++Y.tb_div >= 16u) {
            Y.tb_div = 0;
            if (++Y.tb_count >= 256u) {
                Y.tb_count = Y.tb_period;
                if (Y.timer_ctrl & 0x08) Y.tb_flag = 1;
            }
        }
    }
}

static void lfo_tick(void) {
    if (!Y.lfo_on) { Y.lfo_am = 0; Y.lfo_pm = 0; return; }
    if (Y.lfo_sub) { Y.lfo_sub--; return; }
    Y.lfo_sub = LFO_STEP[Y.lfo_rate & 7];
    Y.lfo_cnt = (uint8_t)((Y.lfo_cnt + 1) & 127);
    /* Triangle up then down, so amplitude modulation has no discontinuity. */
    Y.lfo_am = (uint8_t)((Y.lfo_cnt & 64) ? (127 - Y.lfo_cnt) * 2 : Y.lfo_cnt * 2);
    Y.lfo_pm = (int8_t)((int)(Y.lfo_cnt & 63) - 32);
}

static void advance_chip(void) {
    /* One FM sample: run every operator's phase and envelope. */
    Y.eg_cnt++;
    timers_tick();
    lfo_tick();
    for (int ci = 0; ci < 6; ci++) {
        ch_t *c = &Y.ch[ci];
        for (int i = 0; i < 4; i++) {
            uint32_t step = c->op[i].inc;
            if (c->fms)                       /* vibrato */
                step = (uint32_t)((int32_t)step +
                       (((int32_t)step * Y.lfo_pm * FMS_SCALE[c->fms & 7]) >> 20));
            c->op[i].phase = (c->op[i].phase + step) & 0xFFFFF;
            eg_step(c, &c->op[i]);
        }
    }
}

void ym_run(uint64_t cycles) {
    init_tables();
    if (cycles <= Y.cycles) return;
    uint64_t delta = cycles - Y.cycles;
    Y.cycles = cycles;

    /* Walk in FM-sample steps, emitting an output sample whenever enough
       cycles have passed. The FM rate (52781 Hz) is higher than the output
       rate, so several FM samples are averaged into each one, which is a cheap
       box filter and keeps the top octave from aliasing. */
    Y.fm_acc += delta;
    while (Y.fm_acc >= FM_CLOCK_DIV) {
        Y.fm_acc -= FM_CLOCK_DIV;
        advance_chip();
        int32_t l, r;
        fm_sample(&l, &r);
        Y.sum_l += l; Y.sum_r += r; Y.sum_n++;

        Y.out_acc += (uint64_t)PSG_RATE * FM_CLOCK_DIV;
        while (Y.out_acc >= M68K_HZ) {
            Y.out_acc -= M68K_HZ;
            /* Operators are 14-bit now, so the mix already uses the range. */
            int32_t ol = Y.sum_n ? Y.sum_l / (int32_t)Y.sum_n : 0;
            int32_t or_ = Y.sum_n ? Y.sum_r / (int32_t)Y.sum_n : 0;
            /* Start the next averaging window. Losing this line makes sum_n
               grow without bound, so each output sample averages a longer and
               longer stretch of the waveform and the level decays as 1/n --
               which looks exactly like an envelope fault and is not one. */
            Y.sum_l = Y.sum_r = 0; Y.sum_n = 0;

            /* Block DC, as the console's AC-coupled output stage does.
             *
             * The DAC holds its last sample indefinitely once the driver stops
             * feeding it. On hardware that is harmless because nothing
             * downstream passes DC. Without this the held value becomes a
             * constant offset -- measured at -708 after the intro cinematic,
             * against the reference's 0 -- and everything else rides on it and
             * clips, which is why a sound effect seemed to hang and the mix
             * afterwards was a mess.
             *
             * One-pole high pass: y[n] = x[n] - x[n-1] + k*y[n-1] with
             * k = 1020/1024, a corner near 27 Hz. Measured in isolation that
             * passes a 240 Hz tone at 0.996 and removes a DC step completely
             * within a tenth of a second. */
            {
                int32_t yl = ol - Y.dc_x_l + (Y.dc_y_l * 1020) / 1024;
                Y.dc_x_l = ol; Y.dc_y_l = yl; ol = yl;
                int32_t yr = or_ - Y.dc_x_r + (Y.dc_y_r * 1020) / 1024;
                Y.dc_x_r = or_; Y.dc_y_r = yr; or_ = yr;
            }

            if (ol  >  32767) ol  =  32767;
            if (ol  < -32768) ol  = -32768;
            if (or_ >  32767) or_ =  32767;
            if (or_ < -32768) or_ = -32768;
            size_t nw = (ring_w + 1) % RING;
            if (nw != ring_r) {
                ring[ring_w * 2] = (int16_t)ol;
                ring[ring_w * 2 + 1] = (int16_t)or_;
                ring_w = nw;
            }
        }
    }
}

/* -------------------------------------------------------------- register */

static op_t *op_for(unsigned bank, unsigned reg, ch_t **chp) {
    unsigned ci = (reg & 3);
    if (ci == 3) return NULL;                    /* no channel 4 in a bank */
    ch_t *c = &Y.ch[ci + bank * 3];
    *chp = c;
    /* Operators appear in the register map in the order 1,3,2,4. */
    static const uint8_t slot[4] = {0, 2, 1, 3};
    return &c->op[slot[(reg >> 2) & 3]];
}

void ym_write(unsigned port, uint8_t v) {
    ym_writes++;
    init_tables();
    unsigned bank = (port >> 1) & 1;
    if ((port & 1) == 0) { Y.addr[bank] = v; return; }

    unsigned reg = Y.addr[bank];
    Y.reg[bank][reg] = v;

    if (bank == 0 && reg < 0x30) {
        switch (reg) {
        case 0x24: Y.ta_period = (uint16_t)((Y.ta_period & 3) | (v << 2)); break;
        case 0x25: Y.ta_period = (uint16_t)((Y.ta_period & 0x3FC) | (v & 3)); break;
        case 0x26: Y.tb_period = v; break;
        case 0x27:
            if (getenv("DB4A_TIMERLOG")) {
                static unsigned long n;
                if (n++ < 30)
                    fprintf(stderr, "[t] 27 <- %02X  loadA=%d enA=%d rstA=%d (period %u)\n",
                            v, v & 1, (v >> 2) & 1, (v >> 4) & 1, Y.ta_period);
            }
            Y.timer_ctrl = v;
            if (v & 0x10) Y.ta_flag = 0;          /* reset the overflow flags */
            if (v & 0x20) Y.tb_flag = 0;
            if ((v & 1) && !Y.ta_run) Y.ta_count = Y.ta_period;
            if ((v & 2) && !Y.tb_run) { Y.tb_count = Y.tb_period; Y.tb_div = 0; }
            Y.ta_run = v & 1;
            Y.tb_run = (v >> 1) & 1;
            break;
        case 0x28: {                              /* key on/off */
            if (getenv("DB4A_KEYLOG")) {
                static unsigned long shown;
                if (shown++ < 60) fprintf(stderr, "[key] 28 <- %02X (ch=%u bits=%X)\n",
                                          v, (unsigned)((v & 3) + ((v & 4) ? 3 : 0)),
                                          (v >> 4) & 15);
            }
            unsigned ci = v & 3;
            if (ci == 3) break;
            if (v & 4) ci += 3;
            ch_t *c = &Y.ch[ci];
            static const uint8_t slot[4] = {0, 2, 1, 3};
            for (int i = 0; i < 4; i++) {
                if (v & (0x10 << i)) { key_on(c, slot[i]); ym_keyons++; }
                else                   key_off(c, slot[i]);
            }
            c->keyed = (v >> 4) & 15;
            break;
        }
        case 0x22:
            Y.lfo_on = (v >> 3) & 1;
            Y.lfo_rate = v & 7;
            if (!Y.lfo_on) { Y.lfo_cnt = 0; Y.lfo_am = 0; Y.lfo_pm = 0; }
            break;
        case 0x2A: Y.dac = (int16_t)((int)v - 128); dac_writes++; break;
        case 0x2B: Y.dac_on = v >> 7; break;
        default: break;
        }
        return;
    }

    ch_t *c = NULL;
    if (reg >= 0x30 && reg < 0xA0) {
        op_t *o = op_for(bank, reg, &c);
        if (!o) return;
        switch (reg & 0xF0) {
        case 0x30: o->dt = (v >> 4) & 7; o->mul = v & 15; refresh_inc(c, o); break;
        case 0x40: o->tl = v & 0x7F; break;
        case 0x50: o->ks = v >> 6; o->ar = v & 0x1F; break;
        case 0x60: o->am = v >> 7; o->dr = v & 0x1F; break;
        case 0x70: o->sr = v & 0x1F; break;
        case 0x80: o->sl = v >> 4; o->rr = (v & 15) * 2 + 1; break;
        case 0x90: o->ssg = v & 15; break;
        default: break;
        }
        return;
    }

    unsigned ci = reg & 3;
    if (ci == 3) return;
    c = &Y.ch[ci + bank * 3];
    switch (reg & 0xFC) {
    case 0xA0: c->fnum = (uint16_t)((c->fnum & 0x700) | v); refresh_channel(c); break;
    case 0xA4: c->fnum = (uint16_t)((c->fnum & 0xFF) | ((v & 7) << 8));
               c->block = (v >> 3) & 7; refresh_channel(c); break;
    case 0xB0: c->fb = (v >> 3) & 7; c->alg = v & 7; break;
    case 0xB4:
        c->pan = v & 0xC0;
        c->ams = (v >> 4) & 3;
        c->fms = v & 7;
        for (int i = 0; i < 4; i++) c->op[i].ams = c->ams;
        break;
    default: break;
    }
}

uint8_t ym_read_status(void) {
    return (uint8_t)((Y.ta_flag ? 1 : 0) | (Y.tb_flag ? 2 : 0));
}

void ym_reset(void) {
    init_tables();
    memset(&Y, 0, sizeof Y);
    for (int ci = 0; ci < 6; ci++) {
        Y.ch[ci].pan = 0xC0;                     /* both speakers by default */
        for (int i = 0; i < 4; i++) {
            Y.ch[ci].op[i].state = EG_OFF;
            Y.ch[ci].op[i].env   = 1023;
        }
    }
    ring_w = ring_r = 0;
}

size_t ym_available(void) { return (ring_w + RING - ring_r) % RING; }

size_t ym_read_samples(int16_t *out, size_t max) {
    size_t n = 0;
    while (n + 1 < max && ring_r != ring_w) {
        out[n++] = ring[ring_r * 2];
        out[n++] = ring[ring_r * 2 + 1];
        ring_r = (ring_r + 1) % RING;
    }
    return n;
}

void ym_report(void) {
    static const char *SN[5] = {"att","dec","sus","rel","off"};
    int cnt[5] = {0,0,0,0,0};
    for (int c = 0; c < 6; c++)
        for (int i = 0; i < 4; i++) cnt[Y.ch[c].op[i].state]++;
    printf("ym eg states:");
    for (int i = 0; i < 5; i++) printf(" %s=%d", SN[i], cnt[i]);
    printf("   keyed:");
    for (int c = 0; c < 6; c++) printf(" ch%d=%X", c, Y.ch[c].keyed);
    printf("\n");

    printf("ym2612  writes=%lu keyons=%lu dacw=%lu dac=%s lfo=%u/%u  timerA=%s(%u) timerB=%s(%u)  frames=%zu\n",
           ym_writes, ym_keyons, dac_writes, Y.dac_on ? "on" : "off",
           Y.lfo_on, Y.lfo_rate,
           Y.ta_run ? "run" : "off", Y.ta_period,
           Y.tb_run ? "run" : "off", Y.tb_period, ym_available());
}

