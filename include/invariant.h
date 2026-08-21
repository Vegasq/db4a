#ifndef INVARIANT_H
#define INVARIANT_H
#include <stdint.h>

/* Machine invariants: cheap tripwires that fire at the MOMENT state goes bad,
 * rather than thousands of blocks later when the damage finally surfaces.
 *
 * The LINK sign-extension bug walked the stack pointer out of RAM and only
 * became visible much later as a jump to a garbage PC. "A7 left RAM" would
 * have caught it on the first bad frame, pointing directly at the culprit.
 *
 * These are permanent and flag-gated, not throwaway probes. Disabled with
 * DB4A_NO_INVARIANTS=1 for a speed run; a violation reports once per site so a
 * repeating fault cannot flood the log.
 */

typedef enum {
    INV_SP_RANGE,        /* A7 outside RAM */
    INV_PC_RANGE,        /* PC outside ROM */
    INV_SP_ALIGN,        /* A7 odd -- the 68000 cannot push to an odd address */
    INV_VDP_DMA_LEN,     /* implausible DMA length */
    INV_VDP_ADDR,        /* VDP address beyond its target memory */
    INV_Z80_PC,          /* Z80 PC outside its 8 KiB RAM */
    INV_COUNT
} inv_id;

void invariant_init(void);
void invariant_fail(inv_id id, const char *what, uint32_t got, uint32_t ctx);
int  invariant_enabled(void);
unsigned long invariant_violations(void);
void invariant_report(void);

#define INV_CHECK(id, cond, what, got, ctx) \
    do { if (invariant_enabled() && !(cond)) invariant_fail((id), (what), (got), (ctx)); } while (0)

/* Mega Drive memory geography */
#define IS_ROM_ADDR(a)  (((a) & 0xFFFFFF) < 0x400000u)
#define IS_RAM_ADDR(a)  ((((a) & 0xFFFFFF) & 0xFF0000u) == 0xFF0000u)
#endif
