#ifndef PROBE_H
#define PROBE_H
#include <stdint.h>

/* Block probes: "did this routine run during the last frame?"
 *
 * Several screens cannot be identified from RAM or from the scene pointer at
 * $FFFFE002. The build console is a sub-mode of gameplay and leaves its state
 * behind after closing; house selection shares scene $00004500 with the
 * loading transitions between gameplay segments. In both cases the reliable
 * signal is execution rather than state -- the screen is up exactly when its
 * input handler runs -- and the dispatcher sees every block entry anyway.
 *
 * Slots are claimed at startup and only when the feature that wants them is
 * enabled, so an unused build compares against nothing.
 */
#define PROBE_SLOTS 4

#define PROBE_BUILD_CONSOLE 0   /* $8462  the console's d-pad handler   */
#define PROBE_HOUSE_SELECT  1   /* $4808  the shield highlight's slide  */
#define PROBE_MENTAT_ASK    2   /* $25CAE the YES/NO loop head          */
#define PROBE_STARPORT      3   /* $916C  the Starport's d-pad handler  */

extern uint32_t probe_pc[PROBE_SLOTS];
extern uint8_t  probe_hit[PROBE_SLOTS];
extern int      probe_any;              /* 0 = no slot claimed, skip the scan */

/* Claim a slot. pc == 0 releases it. */
void probe_watch(unsigned slot, uint32_t pc);

/* Did the watched block run since the last call? Clears as it reads, so call
 * exactly once per frame per slot. */
int  probe_take(unsigned slot);

#endif
