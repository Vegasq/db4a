/* Instruction execution, included by z80.c. Split out only for file size. */

static void daa(void) {
    uint8_t a = Z80.a, adj = 0, f = Z80.f;
    if ((f & ZF_H) || (a & 0x0F) > 9) adj |= 0x06;
    if ((f & ZF_C) || a > 0x99)       adj |= 0x60;
    uint8_t r = (f & ZF_N) ? (uint8_t)(a - adj) : (uint8_t)(a + adj);
    Z80.f = (uint8_t)(szyx(r) | parity(r) | (f & ZF_N)
          | ((a > 0x99 || (f & ZF_C)) ? ZF_C : 0)
          | (((f & ZF_N) ? ((f & ZF_H) && (a & 0x0F) < 6)
                         : ((a & 0x0F) > 9)) ? ZF_H : 0));
    Z80.a = r;
}

/* Active HL: either the real HL, or IX/IY under a DD/FD prefix. */
/* The "active HL": plain HL, or IX/IY when a DD/FD prefix is in effect.
   `reg` points at the real index register so that instructions which MODIFY it
   (INC IX, ADD IX,rp, LD IX,nn, POP IX, EX (SP),IX) write back rather than
   silently discarding the result. */
typedef struct {
    int      indexed;   /* a DD/FD prefix is in effect */
    int      halves;    /* ...and it substitutes IXH/IXL for H/L (see below) */
    int8_t   disp;
    uint16_t *reg;
} hlctx;

/* Always read and write the LIVE register.
 *
 * An earlier version cached HL in the context at instruction entry and wrote
 * it back at exit. That silently discarded every 8-bit write to H or L
 * (LD H,n / INC L / LD H,B), because those go through REG8 and never touched
 * the cache -- and under a DD/FD prefix it wrote HL into the index register.
 * Caching a register that other paths can modify is the bug; not caching it
 * removes the whole class. */
static inline uint16_t get_hl(void);
static inline void     set_hl(uint16_t v);
static inline uint16_t xget(hlctx *x) { return x->indexed ? *x->reg : get_hl(); }
static inline void     xset(hlctx *x, uint16_t v) {
    if (x->indexed) *x->reg = v; else set_hl(v);
}
static inline uint16_t xaddr(hlctx *x) {
    return x->indexed ? (uint16_t)(*x->reg + x->disp) : get_hl();
}

/* Undocumented but real: a DD/FD prefix on an instruction that does NOT use
   (HL) redirects the H and L operands to the high and low halves of the index
   register. On an instruction that does use (HL), the prefix means (IX+d) and
   H/L keep their normal meaning -- so the two readings never collide. */
static inline uint8_t xhalf_get(int r, hlctx *x) {
    uint16_t v = *x->reg;
    return (r == 4) ? (uint8_t)(v >> 8) : (uint8_t)v;
}
static inline void xhalf_set(int r, hlctx *x, uint8_t b) {
    uint16_t v = *x->reg;
    *x->reg = (r == 4) ? (uint16_t)((v & 0x00FFu) | ((uint16_t)b << 8))
                       : (uint16_t)((v & 0xFF00u) | b);
}
static inline int is_half(int r, hlctx *x) {
    return x->halves && (r == 4 || r == 5);
}

static inline uint16_t get_hl(void) { return (uint16_t)((Z80.h << 8) | Z80.l); }
static inline void set_hl(uint16_t v) { Z80.h = (uint8_t)(v >> 8); Z80.l = (uint8_t)v; }

/* Read/write register index z, resolving (HL) / (IX+d) / (IY+d). */
static uint8_t rd_r(int z, hlctx *x) {
    if (is_half(z, x)) return xhalf_get(z, x);
    if (z != 6) return *REG8[z];
    return z80_read(xaddr(x));
}
static void wr_r(int z, hlctx *x, uint8_t v) {
    if (is_half(z, x)) { xhalf_set(z, x, v); return; }
    if (z != 6) { *REG8[z] = v; return; }
    z80_write(xaddr(x), v);
}

static void do_cb(hlctx *x, unsigned *cyc) {
    /* Under DD/FD the displacement precedes the opcode byte. */
    if (x->indexed) x->disp = (int8_t)fetch();
    uint8_t op = fetch();
    int xx = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v = rd_r(z, x);
    /* An indexed CB always addresses memory, even when z names a register. */
    if (x->indexed) v = z80_read(xaddr(x));

    /* Cycle costs here EXCLUDE the fetch z80_step already charged, and for an
     * indexed form the extra 4 the DD/FD loop charged as well. Adding the full
     * documented totals on top of those made every non-indexed CB op 4 cycles
     * heavy -- bit 0,(hl) came to 16 rather than 12. That matters more than it
     * looks: Dune's sound driver is CPU-bound on the Z80 and spins on exactly
     * that instruction, so the error slowed the music directly.
     *
     *   bit b,r  8    bit b,(hl)  12    bit b,(ix+d)  20
     *   rot r    8    rot (hl)    15    rot (ix+d)    23   (res/set likewise)
     */
    if (xx == 1) {                              /* BIT */
        uint8_t r = (uint8_t)(v & (1u << y));
        Z80.f = (uint8_t)((r ? (r & ZF_S) : (ZF_Z | ZF_P)) | ZF_H | (Z80.f & ZF_C)
              | ((z == 6 || x->indexed) ? 0 : (v & (ZF_Y | ZF_X))));
        *cyc += x->indexed ? 12 : (z == 6 ? 8 : 4);
    } else {
        uint8_t r = (xx == 0) ? rot(y, v)
                  : (xx == 2) ? (uint8_t)(v & ~(1u << y))
                              : (uint8_t)(v | (1u << y));
        if (x->indexed) { z80_write(xaddr(x), r); if (z != 6) *REG8[z] = r; }
        else wr_r(z, x, r);
        *cyc += x->indexed ? 15 : (z == 6 ? 11 : 4);
    }
}

static void do_ed(unsigned *cyc) {
    uint8_t op = fetch();
    int xx = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    uint16_t hl = get_hl();
    *cyc += 8;

    if (xx == 1) {
        switch (z) {
        case 0: {                                  /* IN r[y],(C) */
            uint8_t v = z80_in((uint16_t)((Z80.b << 8) | Z80.c));
            if (y != 6) *REG8[y] = v;
            Z80.f = (uint8_t)(szyx(v) | parity(v) | (Z80.f & ZF_C)); *cyc += 4; break; }
        case 1:                                    /* OUT (C),r[y] */
            z80_out((uint16_t)((Z80.b << 8) | Z80.c), (y == 6) ? 0 : *REG8[y]);
            *cyc += 4; break;
        case 2: {                                  /* SBC/ADC HL,rp[p] */
            uint16_t v = rp(p, hl); unsigned r;
            if (q) { r = hl + v + ((Z80.f & ZF_C) ? 1 : 0);
                     Z80.f = (uint8_t)((((r >> 8) & (ZF_S|ZF_Y|ZF_X)))
                           | (((uint16_t)r) ? 0 : ZF_Z)
                           | (((hl ^ v ^ r) & 0x1000) ? ZF_H : 0)
                           | ((((hl ^ r) & (v ^ r)) & 0x8000) ? ZF_P : 0)
                           | ((r > 0xFFFF) ? ZF_C : 0)); }
            else   { r = hl - v - ((Z80.f & ZF_C) ? 1 : 0);
                     Z80.f = (uint8_t)((((r >> 8) & (ZF_S|ZF_Y|ZF_X)))
                           | (((uint16_t)r) ? 0 : ZF_Z)
                           | (((hl ^ v ^ r) & 0x1000) ? ZF_H : 0)
                           | ((((hl ^ v) & (hl ^ r)) & 0x8000) ? ZF_P : 0)
                           | ZF_N | ((r > 0xFFFF) ? ZF_C : 0)); }
            set_hl((uint16_t)r); *cyc += 7; break; }
        case 3: {                                  /* LD (nn),rp / LD rp,(nn) */
            uint16_t nn = fetch16();
            if (q) { uint16_t v = (uint16_t)(z80_read(nn) | (z80_read((uint16_t)(nn+1)) << 8));
                     uint16_t t = hl; set_rp(p, v, &t); if (p == 2) set_hl(t); }
            else   { uint16_t v = rp(p, hl);
                     z80_write(nn, (uint8_t)v);
                     z80_write((uint16_t)(nn+1), (uint8_t)(v >> 8)); }
            *cyc += 12; break; }
        case 4: {                                  /* NEG */
            uint8_t a = Z80.a; unsigned r = 0u - a;
            Z80.f = (uint8_t)(szyx((uint8_t)r) | ZF_N
                  | ((a & 0x0F) ? ZF_H : 0) | ((a == 0x80) ? ZF_P : 0)
                  | (a ? ZF_C : 0));
            Z80.a = (uint8_t)r; break; }
        case 5: Z80.iff1 = Z80.iff2; Z80.pc = pop16(); *cyc += 6; break;  /* RETN/RETI */
        case 6: Z80.im = (uint8_t)((y & 3) ? ((y & 3) - 1) : 0); break;   /* IM */
        default:
            switch (y) {
            case 0: Z80.i = Z80.a; *cyc += 1; break;
            case 1: Z80.r = Z80.a; *cyc += 1; break;
            case 2: Z80.a = Z80.i;
                    Z80.f = (uint8_t)(szyx(Z80.a) | (Z80.iff2 ? ZF_P : 0) | (Z80.f & ZF_C));
                    *cyc += 1; break;
            case 3: Z80.a = Z80.r;
                    Z80.f = (uint8_t)(szyx(Z80.a) | (Z80.iff2 ? ZF_P : 0) | (Z80.f & ZF_C));
                    *cyc += 1; break;
            case 4: { uint8_t m = z80_read(hl);      /* RRD */
                      z80_write(hl, (uint8_t)((m >> 4) | (Z80.a << 4)));
                      Z80.a = (uint8_t)((Z80.a & 0xF0) | (m & 0x0F));
                      Z80.f = (uint8_t)(szyx(Z80.a) | parity(Z80.a) | (Z80.f & ZF_C));
                      *cyc += 10; break; }
            case 5: { uint8_t m = z80_read(hl);      /* RLD */
                      z80_write(hl, (uint8_t)((m << 4) | (Z80.a & 0x0F)));
                      Z80.a = (uint8_t)((Z80.a & 0xF0) | (m >> 4));
                      Z80.f = (uint8_t)(szyx(Z80.a) | parity(Z80.a) | (Z80.f & ZF_C));
                      *cyc += 10; break; }
            default: break;
            }
        }
        return;
    }
    if (xx == 2 && z <= 3 && y >= 4) {             /* block transfer / search */
        int inc = (y & 1) ? -1 : 1;
        int rep = (y & 2) != 0;
        uint16_t bc = (uint16_t)((Z80.b << 8) | Z80.c);
        if (z == 0) {                               /* LDI/LDD/LDIR/LDDR */
            uint8_t v = z80_read(hl);
            uint16_t de = (uint16_t)((Z80.d << 8) | Z80.e);
            z80_write(de, v);
            de = (uint16_t)(de + inc); hl = (uint16_t)(hl + inc); bc--;
            Z80.d = (uint8_t)(de >> 8); Z80.e = (uint8_t)de; set_hl(hl);
            Z80.b = (uint8_t)(bc >> 8); Z80.c = (uint8_t)bc;
            uint8_t n = (uint8_t)(Z80.a + v);
            Z80.f = (uint8_t)((Z80.f & (ZF_S | ZF_Z | ZF_C))
                  | (bc ? ZF_P : 0) | ((n & 0x02) ? ZF_Y : 0) | ((n & 0x08) ? ZF_X : 0));
            *cyc += 8;
            if (rep && bc) { Z80.pc -= 2; *cyc += 5; }
        } else if (z == 1) {                        /* CPI/CPD/CPIR/CPDR */
            uint8_t v = z80_read(hl);
            uint8_t r = (uint8_t)(Z80.a - v);
            hl = (uint16_t)(hl + inc); bc--; set_hl(hl);
            Z80.b = (uint8_t)(bc >> 8); Z80.c = (uint8_t)bc;
            uint8_t hf = (uint8_t)(((Z80.a ^ v ^ r) & 0x10) ? ZF_H : 0);
            uint8_t n = (uint8_t)(r - (hf ? 1 : 0));
            Z80.f = (uint8_t)((r & ZF_S) | (r ? 0 : ZF_Z) | hf | ZF_N
                  | (bc ? ZF_P : 0) | (Z80.f & ZF_C)
                  | ((n & 0x02) ? ZF_Y : 0) | ((n & 0x08) ? ZF_X : 0));
            *cyc += 8;
            if (rep && bc && r) { Z80.pc -= 2; *cyc += 5; }
        } else {                                    /* INI/OUTI families */
            if (z == 2) { uint8_t v = z80_in((uint16_t)((Z80.b << 8) | Z80.c));
                          z80_write(hl, v); }
            else        { z80_out((uint16_t)((Z80.b << 8) | Z80.c), z80_read(hl)); }
            hl = (uint16_t)(hl + inc); set_hl(hl);
            Z80.b--;
            Z80.f = (uint8_t)(szyx(Z80.b) | ZF_N);
            *cyc += 8;
            if (rep && Z80.b) { Z80.pc -= 2; *cyc += 5; }
        }
        return;
    }
    /* everything else in the ED page behaves as NOP */
}

unsigned z80_step(void) {
    unsigned cyc = 4;
    hlctx x = { 0, 0, 0, NULL };

    if (Z80.halted) { Z80.cycles += 4; return 4; }

    if (z80_profiling && Z80.pc < 0x2000) z80_pc_hits[Z80.pc]++;
    uint8_t op = fetch();
    Z80.r = (uint8_t)((Z80.r & 0x80) | ((Z80.r + 1) & 0x7F));

    /* DD/FD select an index register; a chain of them keeps the last. */
    while (op == 0xDD || op == 0xFD) {
        x.indexed = 1;
        x.reg = (op == 0xDD) ? &Z80.ix : &Z80.iy;
        op = fetch();
        cyc += 4;
        Z80.r = (uint8_t)((Z80.r & 0x80) | ((Z80.r + 1) & 0x7F));
    }
    if (op == 0xCB) { do_cb(&x, &cyc); goto out; }
    if (op == 0xED) { do_ed(&cyc);     goto out; }

    {
    int xx = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    /* Does this instruction reference (HL)? If so the prefix supplies a
       displacement and H/L keep their meaning; if not, H/L become the index
       register's halves. */
    int uses_mem = (xx == 1 && (z == 6 || y == 6))
                || (xx == 0 && (z == 4 || z == 5 || z == 6) && y == 6)
                || (xx == 2 && z == 6);
    if (x.indexed && uses_mem)
        x.disp = (int8_t)fetch();
    x.halves = x.indexed && !uses_mem;

    switch (xx) {
    case 0:
        switch (z) {
        case 0:
            if (y == 0) { }
            else if (y == 1) { uint8_t t;
                t = Z80.a; Z80.a = Z80.a_; Z80.a_ = t;
                t = Z80.f; Z80.f = Z80.f_; Z80.f_ = t; }
            else if (y == 2) { int8_t d = (int8_t)fetch();
                if (--Z80.b) { Z80.pc = (uint16_t)(Z80.pc + d); cyc += 9; } else cyc += 4; }
            else if (y == 3) { int8_t d = (int8_t)fetch();
                Z80.pc = (uint16_t)(Z80.pc + d); cyc += 8; }
            else { int8_t d = (int8_t)fetch();
                if (cc(y - 4)) { Z80.pc = (uint16_t)(Z80.pc + d); cyc += 8; } else cyc += 3; }
            break;
        case 1:
            if (q) { xset(&x, add16(xget(&x), rp(p, xget(&x)))); cyc += 7; }
            else   { uint16_t nn = fetch16(); uint16_t h = xget(&x);
                     set_rp(p, nn, &h); xset(&x, h); cyc += 6; }
            break;
        case 2:
            if (!q) switch (p) {
                case 0: z80_write((uint16_t)((Z80.b<<8)|Z80.c), Z80.a); cyc += 3; break;
                case 1: z80_write((uint16_t)((Z80.d<<8)|Z80.e), Z80.a); cyc += 3; break;
                case 2: { uint16_t nn = fetch16(); uint16_t h = xget(&x);
                          z80_write(nn, (uint8_t)h);
                          z80_write((uint16_t)(nn+1), (uint8_t)(h >> 8)); cyc += 12; break; }
                default:{ uint16_t nn = fetch16(); z80_write(nn, Z80.a); cyc += 9; break; }
            } else switch (p) {
                case 0: Z80.a = z80_read((uint16_t)((Z80.b<<8)|Z80.c)); cyc += 3; break;
                case 1: Z80.a = z80_read((uint16_t)((Z80.d<<8)|Z80.e)); cyc += 3; break;
                case 2: { uint16_t nn = fetch16();
                          xset(&x, (uint16_t)(z80_read(nn) | (z80_read((uint16_t)(nn+1)) << 8)));
                          cyc += 12; break; }
                default:{ uint16_t nn = fetch16(); Z80.a = z80_read(nn); cyc += 9; break; }
            }
            break;
        case 3: { uint16_t h = xget(&x); uint16_t v = rp(p, h);
                  set_rp(p, (uint16_t)(v + (q ? -1 : 1)), &h);
                  xset(&x, h); cyc += 2; break; }
        case 4: wr_r(y, &x, inc8(rd_r(y, &x))); if (y == 6) cyc += 7; break;
        case 5: wr_r(y, &x, dec8(rd_r(y, &x))); if (y == 6) cyc += 7; break;
        /* LD r,n is 7 and LD (HL),n is 10, both on top of the 4 already
           charged; the indexed LD (IX+d),n is 19 total. */
        case 6: { uint8_t n = fetch(); wr_r(y, &x, n);
                  cyc += (y == 6) ? (x.indexed ? 11 : 6) : 3; break; }
        default:
            switch (y) {
            case 0: case 1: case 2: case 3: {
                /* RLCA/RRCA/RLA/RRA differ from their CB counterparts: S, Z
                   and P/V are PRESERVED, H and N are cleared, C comes from the
                   rotated bit and Y/X from the result. rot() computes the CB
                   flags, so keep the preserved bits and take only C from it. */
                uint8_t keep = (uint8_t)(Z80.f & (ZF_S | ZF_Z | ZF_P));
                uint8_t r = rot(y, Z80.a);
                Z80.f = (uint8_t)(keep | (Z80.f & ZF_C) | (r & (ZF_Y | ZF_X)));
                Z80.a = r; break; }
            case 4: daa(); break;
            case 5: Z80.a = (uint8_t)~Z80.a;
                    Z80.f = (uint8_t)((Z80.f & (ZF_S|ZF_Z|ZF_P|ZF_C)) | ZF_H | ZF_N
                          | (Z80.a & (ZF_Y|ZF_X))); break;
            case 6: Z80.f = (uint8_t)((Z80.f & (ZF_S|ZF_Z|ZF_P)) | ZF_C
                          | (Z80.a & (ZF_Y|ZF_X))); break;
            default:Z80.f = (uint8_t)((Z80.f & (ZF_S|ZF_Z|ZF_P))
                          | ((Z80.f & ZF_C) ? ZF_H : ZF_C)
                          | (Z80.a & (ZF_Y|ZF_X))); break;
            }
        }
        break;

    case 1:
        if (y == 6 && z == 6) { Z80.halted = true; }
        else {
            uint8_t v = rd_r(z, &x);
            wr_r(y, &x, v);
            if (y == 6 || z == 6) cyc += 3;
        }
        break;

    case 2: alu(y, rd_r(z, &x)); if (z == 6) cyc += 3; break;

    default:
        switch (z) {
        case 0: if (cc(y)) { Z80.pc = pop16(); cyc += 7; } else cyc += 1; break;
        case 1:
            if (q) switch (p) {
                case 0: Z80.pc = pop16(); cyc += 6; break;
                case 1: { uint8_t t;
                    t=Z80.b;Z80.b=Z80.b_;Z80.b_=t; t=Z80.c;Z80.c=Z80.c_;Z80.c_=t;
                    t=Z80.d;Z80.d=Z80.d_;Z80.d_=t; t=Z80.e;Z80.e=Z80.e_;Z80.e_=t;
                    t=Z80.h;Z80.h=Z80.h_;Z80.h_=t; t=Z80.l;Z80.l=Z80.l_;Z80.l_=t;
                    break; }   /* EXX never touches IX/IY */
                case 2: Z80.pc = xget(&x); break;
                default: Z80.sp = xget(&x); cyc += 2; break;
            } else { uint16_t v = pop16(); uint16_t h = xget(&x);
                     set_rp2(p, v, &h); xset(&x, h); cyc += 6; }
            break;
        case 2: { uint16_t nn = fetch16(); if (cc(y)) Z80.pc = nn; cyc += 6; break; }
        case 3:
            switch (y) {
            case 0: Z80.pc = fetch16(); cyc += 6; break;
            case 2: { uint8_t n = fetch();
                      z80_out((uint16_t)((Z80.a << 8) | n), Z80.a); cyc += 7; break; }
            case 3: { uint8_t n = fetch();
                      Z80.a = z80_in((uint16_t)((Z80.a << 8) | n)); cyc += 7; break; }
            case 4: { uint16_t t = (uint16_t)(z80_read(Z80.sp) | (z80_read((uint16_t)(Z80.sp+1)) << 8));
                      uint16_t h = xget(&x);
                      z80_write(Z80.sp, (uint8_t)h);
                      z80_write((uint16_t)(Z80.sp+1), (uint8_t)(h >> 8));
                      xset(&x, t); cyc += 15; break; }
            case 5: {   /* EX DE,HL always swaps the REAL HL, never IX/IY */
                      uint8_t t;
                      t = Z80.d; Z80.d = Z80.h; Z80.h = t;
                      t = Z80.e; Z80.e = Z80.l; Z80.l = t;
                      break; }
            case 6: Z80.iff1 = Z80.iff2 = false; break;
            default:Z80.iff1 = Z80.iff2 = true;  break;
            }
            break;
        case 4: { uint16_t nn = fetch16();
                  if (cc(y)) { push16(Z80.pc); Z80.pc = nn; cyc += 13; } else cyc += 6;
                  break; }
        case 5:
            if (q) { uint16_t nn = fetch16(); push16(Z80.pc); Z80.pc = nn; cyc += 13; }
            else   { push16(rp2(p, xget(&x))); cyc += 7; }
            break;
        case 6: alu(y, fetch()); cyc += 3; break;
        default: push16(Z80.pc); Z80.pc = (uint16_t)(y * 8); cyc += 7; break;
        }
    }

    }
out:
    Z80.cycles += cyc;
    return cyc;
}

unsigned long z80_instructions;   /* for the audio work: instructions retired */
unsigned long z80_pchist[65536];  /* full PC, to find the exact loop */

void z80_run(uint64_t until) {
    while (Z80.cycles < until) {
        z80_pchist[Z80.pc & 0xFFFF]++;
        z80_step();
        z80_instructions++;
    }
}

void z80_irq(void) {
    if (!Z80.iff1) return;
    Z80.halted = false;
    Z80.iff1 = Z80.iff2 = false;
    push16(Z80.pc);
    Z80.pc = 0x0038;              /* IM 1 is what Mega Drive drivers use */
    Z80.cycles += 13;
}
