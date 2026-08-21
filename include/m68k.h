/* 68000 CPU state and runtime support for the recompiled Dune ROM.
 *
 * Flags are kept as discrete bytes rather than a packed CCR. The 68000's
 * flag semantics differ per instruction, so each one is set explicitly by
 * the helpers below; packing/unpacking only happens for the rare code that
 * touches SR directly.
 */
#ifndef M68K_H
#define M68K_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t d[8];
    uint32_t a[8];          /* a[7] is the active stack pointer */
    uint32_t pc;
    uint32_t usp, ssp;
    uint8_t  n, z, v, c, x; /* condition codes, 0 or 1 */
    uint8_t  imask;         /* interrupt mask, SR bits 8-10 */
    bool     super;         /* supervisor mode, SR bit 13 */
    bool     stopped;
    uint64_t cycles;
} m68k_t;

extern m68k_t CPU;

/* ---- memory interface (see src/hal_mem.c) ---- */
uint8_t  m68k_read8  (uint32_t addr);
uint16_t m68k_read16 (uint32_t addr);
uint32_t m68k_read32 (uint32_t addr);
void     m68k_write8 (uint32_t addr, uint8_t  v);
void     m68k_write16(uint32_t addr, uint16_t v);
void     m68k_write32(uint32_t addr, uint32_t v);

/* ---- sign/zero extension ---- */
static inline uint32_t sx8 (uint8_t  v) { return (uint32_t)(int32_t)(int8_t) v; }
static inline uint32_t sx16(uint16_t v) { return (uint32_t)(int32_t)(int16_t)v; }

/* ---- flag helpers ---- */
/* Logic ops and MOVE/TST: N,Z from result; V,C cleared; X untouched. */
static inline void flags_logic8 (uint8_t  r) { CPU.n = r >> 7;  CPU.z = !r; CPU.v = CPU.c = 0; }
static inline void flags_logic16(uint16_t r) { CPU.n = r >> 15; CPU.z = !r; CPU.v = CPU.c = 0; }
static inline void flags_logic32(uint32_t r) { CPU.n = r >> 31; CPU.z = !r; CPU.v = CPU.c = 0; }

/* ADD: V on signed overflow, C on unsigned carry, X mirrors C. */
static inline uint8_t add8(uint8_t a, uint8_t b) {
    uint16_t r = (uint16_t)a + b;
    CPU.c = CPU.x = r >> 8;  CPU.n = (r >> 7) & 1;  CPU.z = !(uint8_t)r;
    CPU.v = (~(a ^ b) & (a ^ r) & 0x80) != 0;
    return (uint8_t)r;
}
static inline uint16_t add16(uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a + b;
    CPU.c = CPU.x = r >> 16; CPU.n = (r >> 15) & 1; CPU.z = !(uint16_t)r;
    CPU.v = (~(a ^ b) & (a ^ r) & 0x8000) != 0;
    return (uint16_t)r;
}
static inline uint32_t add32(uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a + b;
    CPU.c = CPU.x = r >> 32; CPU.n = (r >> 31) & 1; CPU.z = !(uint32_t)r;
    CPU.v = (~(a ^ b) & (a ^ (uint32_t)r) & 0x80000000u) != 0;
    return (uint32_t)r;
}

/* SUB computes a - b. CMP uses the same result but must not disturb X. */
static inline uint8_t sub8(uint8_t a, uint8_t b) {
    uint16_t r = (uint16_t)a - b;
    CPU.c = CPU.x = (r >> 8) & 1; CPU.n = (r >> 7) & 1; CPU.z = !(uint8_t)r;
    CPU.v = ((a ^ b) & (a ^ r) & 0x80) != 0;
    return (uint8_t)r;
}
static inline uint16_t sub16(uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a - b;
    CPU.c = CPU.x = (r >> 16) & 1; CPU.n = (r >> 15) & 1; CPU.z = !(uint16_t)r;
    CPU.v = ((a ^ b) & (a ^ r) & 0x8000) != 0;
    return (uint16_t)r;
}
static inline uint32_t sub32(uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a - b;
    CPU.c = CPU.x = (r >> 32) & 1; CPU.n = (r >> 31) & 1; CPU.z = !(uint32_t)r;
    CPU.v = ((a ^ b) & (a ^ (uint32_t)r) & 0x80000000u) != 0;
    return (uint32_t)r;
}
#define CMP_BODY(W)                                       \
    static inline void cmp##W(uint##W##_t a, uint##W##_t b) { \
        uint8_t sx = CPU.x; sub##W(a, b); CPU.x = sx; }
CMP_BODY(8) CMP_BODY(16) CMP_BODY(32)
#undef CMP_BODY

/* ---- condition codes, in the encoding order used by Bcc/Scc/DBcc ---- */
static inline bool cond_t (void){ return true; }
static inline bool cond_f (void){ return false; }
static inline bool cond_hi(void){ return !CPU.c && !CPU.z; }
static inline bool cond_ls(void){ return CPU.c || CPU.z; }
static inline bool cond_cc(void){ return !CPU.c; }
static inline bool cond_cs(void){ return CPU.c; }
static inline bool cond_ne(void){ return !CPU.z; }
static inline bool cond_eq(void){ return CPU.z; }
static inline bool cond_vc(void){ return !CPU.v; }
static inline bool cond_vs(void){ return CPU.v; }
static inline bool cond_pl(void){ return !CPU.n; }
static inline bool cond_mi(void){ return CPU.n; }
static inline bool cond_ge(void){ return CPU.n == CPU.v; }
static inline bool cond_lt(void){ return CPU.n != CPU.v; }
static inline bool cond_gt(void){ return CPU.n == CPU.v && !CPU.z; }
static inline bool cond_le(void){ return CPU.n != CPU.v || CPU.z; }

/* ---- SR packing, for the code that reads/writes SR directly ---- */
static inline uint16_t get_sr(void) {
    return (uint16_t)((CPU.super << 13) | (CPU.imask << 8) |
                      (CPU.x << 4) | (CPU.n << 3) | (CPU.z << 2) |
                      (CPU.v << 1) | CPU.c);
}
void set_sr(uint16_t v);   /* may swap USP/SSP, so not inline */

/* ---- dispatch (see src/dispatch.c) ---- */
typedef void (*block_fn)(void);
void m68k_call(uint32_t addr);   /* jsr/bsr to a possibly-unknown target */
void m68k_jump(uint32_t addr);   /* jmp to a possibly-unknown target */

#endif /* M68K_H */
