#ifndef NATIVE_H
#define NATIVE_H
#include <stdint.h>

/* Native overrides -- game logic this project implements in C instead of
 * running the recompiled cartridge code.
 *
 * The recompiler turns 68000 basic blocks into C that manipulates a CPU state
 * struct. That is faithful, but it is not *ours*: the blocks are generated,
 * unreadable, and the only way to change what the game does is to patch ROM
 * immediates. An override replaces one block entry with a hand-written C
 * function that does the same job against the same RAM, and returns the PC the
 * ROM would have continued from.
 *
 * The rules that make this safe:
 *
 *   1. Override only at an address that is already a block entry. The
 *      dispatcher looks up whole blocks, so a PC in the middle of one is never
 *      consulted and an override there would silently never run.
 *   2. Account for cycles. The 68000 cycle count drives frame pacing and the
 *      Z80 interleave, so a function that does the same work in zero cycles
 *      changes the game's timing. Each override adds the same counts the
 *      blocks it replaced would have.
 *   3. Be exactly equivalent by default. New behaviour goes behind a flag; with
 *      the flag off, `make check-native` requires a full recorded mission to
 *      replay frame-for-frame identically with the override on and off.
 *
 * DB4A_NATIVE=0 disables every override, which is what the equivalence test
 * compares against.
 */
typedef uint32_t (*native_fn)(void);

/* The C implementation registered for `pc`, or NULL. */
native_fn native_lookup(uint32_t pc);

/* Non-zero unless DB4A_NATIVE=0. Resolved once. */
int native_active(void);

/* DB4A_NATIVE=check: run both implementations on every call and diff the whole
 * of RAM, the cycle count and the exit PC. The cartridge's result is the one
 * kept, so a checked run is still a faithful run. */
int native_checking(void);

/* 0 for an override that deliberately differs from the cartridge, so the
 * equivalence checker skips it. */
int native_faithful_only(uint32_t pc);
extern unsigned long native_mismatches;

#endif
