/* Z80 CPU core.
 *
 * Decoded structurally rather than as a 256-way switch. Every opcode splits as
 *
 *     op = x x y y y z z z      x = op>>6   y = (op>>3)&7   z = op&7
 *                               p = y>>1    q = y&1
 *
 * which mirrors how the instruction set is actually laid out, so one branch per
 * (x,z) pair covers whole families at once. That matters here: the captured
 * sound driver uses 133 distinct opcodes with no small hot set, so a partial
 * implementation would have been a long tail of crashes.
 *
 * IX/IY are handled by pointing `hlp` at the active index register and adding a
 * displacement to (HL) accesses -- DD and FD are the two most common opcodes in
 * this driver, so they are a first-class path, not an afterthought.
 */
#include "z80.h"
#include <stddef.h>

z80_t Z80;

/* Optional execution profile, for finding where the driver actually sits. */
unsigned long z80_pc_hits[0x2000];
int z80_profiling;

static const uint8_t PARITY[256] = {
#define P2(n) n, n^1, n^1, n
#define P4(n) P2(n), P2(n^1), P2(n^1), P2(n)
#define P6(n) P4(n), P4(n^1), P4(n^1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
#undef P6
#undef P4
#undef P2
};
static inline uint8_t parity(uint8_t v) { return PARITY[v] ? ZF_P : 0; }
static inline uint8_t szyx(uint8_t v) {
    return (uint8_t)((v & (ZF_S | ZF_Y | ZF_X)) | (v ? 0 : ZF_Z));
}

void z80_reset(void) {
    Z80.pc = 0; Z80.sp = 0xFFFF; Z80.i = Z80.r = 0;
    Z80.iff1 = Z80.iff2 = false; Z80.im = 0; Z80.halted = false;
    Z80.a = Z80.f = 0xFF;
}

/* --- register file access by index: B C D E H L (HL) A --- */
static uint8_t *const REG8[8] = { &Z80.b, &Z80.c, &Z80.d, &Z80.e,
                                  &Z80.h, &Z80.l, NULL, &Z80.a };

static inline uint16_t rp(int p, uint16_t hl) {
    switch (p) {
    case 0: return (uint16_t)((Z80.b << 8) | Z80.c);
    case 1: return (uint16_t)((Z80.d << 8) | Z80.e);
    case 2: return hl;
    default: return Z80.sp;
    }
}
static inline void set_rp(int p, uint16_t v, uint16_t *hl) {
    switch (p) {
    case 0: Z80.b = (uint8_t)(v >> 8); Z80.c = (uint8_t)v; break;
    case 1: Z80.d = (uint8_t)(v >> 8); Z80.e = (uint8_t)v; break;
    case 2: *hl = v; break;
    default: Z80.sp = v; break;
    }
}
static inline uint16_t rp2(int p, uint16_t hl) {
    return (p == 3) ? (uint16_t)((Z80.a << 8) | Z80.f) : rp(p, hl);
}
static inline void set_rp2(int p, uint16_t v, uint16_t *hl) {
    if (p == 3) { Z80.a = (uint8_t)(v >> 8); Z80.f = (uint8_t)v; }
    else set_rp(p, v, hl);
}

static inline uint8_t fetch(void)  { return z80_read(Z80.pc++); }
static inline uint16_t fetch16(void) {
    uint8_t lo = fetch(); uint8_t hi = fetch();
    return (uint16_t)(lo | (hi << 8));
}
static inline void push16(uint16_t v) {
    z80_write(--Z80.sp, (uint8_t)(v >> 8));
    z80_write(--Z80.sp, (uint8_t)v);
}
static inline uint16_t pop16(void) {
    uint8_t lo = z80_read(Z80.sp++);
    uint8_t hi = z80_read(Z80.sp++);
    return (uint16_t)(lo | (hi << 8));
}

/* --- 8-bit arithmetic --- */
static void alu(int op, uint8_t v) {
    uint8_t a = Z80.a; unsigned r;
    switch (op) {
    case 0: /* ADD */
    case 1: /* ADC */ {
        unsigned cy = (op == 1 && (Z80.f & ZF_C)) ? 1 : 0;
        r = a + v + cy;
        Z80.f = (uint8_t)(szyx((uint8_t)r)
              | (((a ^ v ^ (unsigned)r) & 0x10) ? ZF_H : 0)
              | ((((a ^ (unsigned)r) & (v ^ (unsigned)r)) & 0x80) ? ZF_P : 0)
              | ((r > 0xFF) ? ZF_C : 0));
        Z80.a = (uint8_t)r; break; }
    case 2: /* SUB */
    case 3: /* SBC */
    case 7: /* CP  */ {
        unsigned cy = (op == 3 && (Z80.f & ZF_C)) ? 1 : 0;
        r = a - v - cy;
        uint8_t res = (uint8_t)r;
        uint8_t fl = (uint8_t)(((op == 7) ? (uint8_t)((res & ZF_S) | (res ? 0 : ZF_Z))
                                          : szyx(res))
              | ZF_N
              | (((a ^ v ^ (unsigned)r) & 0x10) ? ZF_H : 0)
              | ((((a ^ v) & (a ^ (unsigned)r)) & 0x80) ? ZF_P : 0)
              | ((r > 0xFF) ? ZF_C : 0));
        /* CP takes the undocumented Y/X bits from the OPERAND, not the result */
        if (op == 7) fl |= (uint8_t)(v & (ZF_Y | ZF_X));
        Z80.f = fl;
        if (op != 7) Z80.a = res;
        break; }
    case 4: Z80.a &= v; Z80.f = (uint8_t)(szyx(Z80.a) | ZF_H | parity(Z80.a)); break;
    case 5: Z80.a ^= v; Z80.f = (uint8_t)(szyx(Z80.a) | parity(Z80.a));        break;
    default:Z80.a |= v; Z80.f = (uint8_t)(szyx(Z80.a) | parity(Z80.a));        break;
    }
}

static uint8_t inc8(uint8_t v) {
    uint8_t r = (uint8_t)(v + 1);
    Z80.f = (uint8_t)(szyx(r) | ((r & 0x0F) ? 0 : ZF_H)
          | ((r == 0x80) ? ZF_P : 0) | (Z80.f & ZF_C));
    return r;
}
static uint8_t dec8(uint8_t v) {
    uint8_t r = (uint8_t)(v - 1);
    Z80.f = (uint8_t)(szyx(r) | (((r & 0x0F) == 0x0F) ? ZF_H : 0)
          | ((r == 0x7F) ? ZF_P : 0) | ZF_N | (Z80.f & ZF_C));
    return r;
}

/* --- rotates and shifts (CB table and the accumulator forms) --- */
static uint8_t rot(int op, uint8_t v) {
    uint8_t c = Z80.f & ZF_C, r = 0, out = 0;
    switch (op) {
    case 0: out = v >> 7;      r = (uint8_t)((v << 1) | out);       break; /* RLC */
    case 1: out = v & 1;       r = (uint8_t)((v >> 1) | (out << 7));break; /* RRC */
    case 2: out = v >> 7;      r = (uint8_t)((v << 1) | c);         break; /* RL  */
    case 3: out = v & 1;       r = (uint8_t)((v >> 1) | (c << 7));  break; /* RR  */
    case 4: out = v >> 7;      r = (uint8_t)(v << 1);               break; /* SLA */
    case 5: out = v & 1;       r = (uint8_t)((v >> 1) | (v & 0x80));break; /* SRA */
    case 6: out = v >> 7;      r = (uint8_t)((v << 1) | 1);         break; /* SLL */
    default:out = v & 1;       r = (uint8_t)(v >> 1);               break; /* SRL */
    }
    Z80.f = (uint8_t)(szyx(r) | parity(r) | (out ? ZF_C : 0));
    return r;
}

static uint16_t add16(uint16_t a, uint16_t b) {
    unsigned r = a + b;
    Z80.f = (uint8_t)((Z80.f & (ZF_S | ZF_Z | ZF_P))
          | (((a ^ b ^ r) & 0x1000) ? ZF_H : 0)
          | ((r > 0xFFFF) ? ZF_C : 0)
          | (((r >> 8) & (ZF_Y | ZF_X))));
    return (uint16_t)r;
}

static bool cc(int y) {
    switch (y) {
    case 0: return !(Z80.f & ZF_Z);  case 1: return  (Z80.f & ZF_Z);
    case 2: return !(Z80.f & ZF_C);  case 3: return  (Z80.f & ZF_C);
    case 4: return !(Z80.f & ZF_P);  case 5: return  (Z80.f & ZF_P);
    case 6: return !(Z80.f & ZF_S);  default: return (Z80.f & ZF_S);
    }
}
#include "z80_exec.h"
