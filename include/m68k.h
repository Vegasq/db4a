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
    bool     trace;         /* trace mode, SR bit 15 -- MOVE from SR returns it */
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

/* ---- extended arithmetic (ADDX/SUBX/NEGX) ----
 * Z is STICKY on these: it is only ever cleared, never set. That is what makes
 * multi-precision arithmetic work -- a zero partial result must not resurrect
 * Z after a previous non-zero limb cleared it.
 */
#define EXTOPS(W, T, U)                                                       \
static inline T addx##W(T a, T b) {                                           \
    U r = (U)a + b + CPU.x;                                                   \
    CPU.c = CPU.x = (uint8_t)(r >> W); CPU.n = (uint8_t)((r >> (W-1)) & 1);   \
    if ((T)r) CPU.z = 0;                                                      \
    CPU.v = (~(a ^ b) & (a ^ (T)r) & ((T)1 << (W-1))) != 0;                   \
    return (T)r; }                                                            \
static inline T subx##W(T a, T b) {                                           \
    U r = (U)a - b - CPU.x;                                                   \
    CPU.c = CPU.x = (uint8_t)((r >> W) & 1); CPU.n = (uint8_t)((r >> (W-1)) & 1); \
    if ((T)r) CPU.z = 0;                                                      \
    CPU.v = ((a ^ b) & (a ^ (T)r) & ((T)1 << (W-1))) != 0;                    \
    return (T)r; }

EXTOPS(8,  uint8_t,  uint16_t)
EXTOPS(16, uint16_t, uint32_t)
EXTOPS(32, uint32_t, uint64_t)
#undef EXTOPS

/* Raised by DIVU/DIVS with a zero divisor, and by ILLEGAL. */
void m68k_div_by_zero(void);
void m68k_illegal(uint32_t pc);
void m68k_unimplemented(uint32_t pc);

/* ---- shifts and rotates ----
 * 68000 rules encoded here, per size:
 *   LSL/LSR : X=C=last bit shifted out; V=0; count 0 leaves X, clears C
 *   ASL     : V set if the sign bit changed at ANY point during the shift
 *   ASR     : sign-preserving; V=0
 *   ROL/ROR : C=last bit rotated; X untouched; count 0 clears C
 *   ROXL/ROXR: rotate through X over (size+1) bits; count 0 gives C=X
 */
#define SHIFTS(W, T)                                                          \
static inline void flags_nz##W(T r) { CPU.n = (T)(r >> (W-1)) & 1; CPU.z = !r; } \
static inline T lsl##W(T v, uint32_t cnt) {                                   \
    if (!cnt) { CPU.c = 0; CPU.v = 0; flags_nz##W(v); return v; }             \
    CPU.c = CPU.x = (cnt <= W) ? ((v >> (W - cnt)) & 1) : 0;                   \
    T r = (cnt >= W) ? 0 : (T)(v << cnt);                                     \
    CPU.v = 0; flags_nz##W(r); return r; }                                    \
static inline T lsr##W(T v, uint32_t cnt) {                                   \
    if (!cnt) { CPU.c = 0; CPU.v = 0; flags_nz##W(v); return v; }             \
    CPU.c = CPU.x = (cnt <= W) ? ((v >> (cnt - 1)) & 1) : 0;                   \
    T r = (cnt >= W) ? 0 : (T)(v >> cnt);                                     \
    CPU.v = 0; flags_nz##W(r); return r; }                                    \
static inline T asl##W(T v, uint32_t cnt) {                                   \
    if (!cnt) { CPU.c = 0; CPU.v = 0; flags_nz##W(v); return v; }             \
    CPU.c = CPU.x = (cnt <= W) ? ((v >> (W - cnt)) & 1) : 0;                   \
    T r = (cnt >= W) ? 0 : (T)(v << cnt);                                     \
    /* V: did the sign bit differ at any point? */                            \
    T mask = (cnt >= W) ? (T)0xFFFFFFFFu                                      \
                        : (T)(0xFFFFFFFFu << (W - cnt - 1));                  \
    T top  = (T)(v & mask);                                                   \
    CPU.v = !(top == 0 || top == mask);                                       \
    flags_nz##W(r); return r; }                                               \
static inline T asr##W(T v, uint32_t cnt) {                                   \
    if (!cnt) { CPU.c = 0; CPU.v = 0; flags_nz##W(v); return v; }             \
    T sign = (T)(v >> (W-1)) & 1;                                             \
    CPU.c = CPU.x = (cnt <= W) ? ((v >> (cnt - 1)) & 1) : sign;                \
    T r = (cnt >= W) ? (T)(sign ? 0xFFFFFFFFu : 0u)                           \
                     : (T)((v >> cnt)                                         \
                           | (sign ? (T)(0xFFFFFFFFu << (W - cnt)) : 0u));    \
    CPU.v = 0; flags_nz##W(r); return r; }                                    \
static inline T rol##W(T v, uint32_t cnt) {                                   \
    uint32_t c = cnt % W;                                                     \
    if (!cnt) { CPU.c = 0; CPU.v = 0; flags_nz##W(v); return v; }             \
    T r = c ? (T)((v << c) | (v >> (W - c))) : v;                             \
    CPU.c = r & 1; CPU.v = 0; flags_nz##W(r); return r; }                     \
static inline T ror##W(T v, uint32_t cnt) {                                   \
    uint32_t c = cnt % W;                                                     \
    if (!cnt) { CPU.c = 0; CPU.v = 0; flags_nz##W(v); return v; }             \
    T r = c ? (T)((v >> c) | (v << (W - c))) : v;                             \
    CPU.c = (T)(r >> (W-1)) & 1; CPU.v = 0; flags_nz##W(r); return r; }       \
static inline T roxl##W(T v, uint32_t cnt) {                                  \
    CPU.v = 0;                                                                \
    if (!cnt) { CPU.c = CPU.x; flags_nz##W(v); return v; }                     \
    for (uint32_t i = 0; i < cnt; i++) {                                      \
        uint8_t nx = (T)(v >> (W-1)) & 1;                                     \
        v = (T)((v << 1) | CPU.x); CPU.x = nx; }                              \
    CPU.c = CPU.x; flags_nz##W(v); return v; }                                \
static inline T roxr##W(T v, uint32_t cnt) {                                  \
    CPU.v = 0;                                                                \
    if (!cnt) { CPU.c = CPU.x; flags_nz##W(v); return v; }                     \
    for (uint32_t i = 0; i < cnt; i++) {                                      \
        uint8_t nx = v & 1;                                                   \
        v = (T)((v >> 1) | ((T)CPU.x << (W-1))); CPU.x = nx; }                 \
    CPU.c = CPU.x; flags_nz##W(v); return v; }

SHIFTS(8,  uint8_t)
SHIFTS(16, uint16_t)
SHIFTS(32, uint32_t)
#undef SHIFTS

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
    return (uint16_t)((CPU.trace << 15) | (CPU.super << 13) | (CPU.imask << 8) |
                      (CPU.x << 4) | (CPU.n << 3) | (CPU.z << 2) |
                      (CPU.v << 1) | CPU.c);
}
void set_sr(uint16_t v);   /* may swap USP/SSP, so not inline */

/* CCR is the low byte of SR: X N Z V C. Writing it never changes the
   interrupt mask or supervisor state, so unlike set_sr it is inline. */
static inline void set_ccr(uint8_t v) {
    CPU.c = v & 1; CPU.v = (v >> 1) & 1; CPU.z = (v >> 2) & 1;
    CPU.n = (v >> 3) & 1; CPU.x = (v >> 4) & 1;
}

/* ---- dispatch (see src/dispatch.c) ---- */
typedef void (*block_fn)(void);
void m68k_call(uint32_t addr);   /* jsr/bsr to a possibly-unknown target */
void m68k_jump(uint32_t addr);   /* jmp to a possibly-unknown target */

#endif /* M68K_H */
