/* cfg_bool: the settings file must be able to say NO.
 *
 * This exists because the presence test it replaces could not. Every boolean
 * setting was `if (cfg("DB4A_X"))`, which asks whether the key is present --
 * and `mute = 0` in db4a.conf is present. So the one line a player would write
 * to keep the sound on silenced it instead, and nothing in the build noticed.
 * The cases below are the ones that go wrong quietly. This branch adds five
 * more booleans and one that inverts, so it carries the extra cases too. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void chk(const char *what, int got, int want) {
    if (got != want) { printf("  FAIL %-34s got %d want %d\n", what, got, want); fails++; }
    else               printf("  ok   %-34s %d\n", what, got);
}

/* Each case sets the env var, since the environment and the file go through
   the same cfg() lookup and the environment is the half a test can control. */
static int with(const char *val, int dflt) {
    if (val) setenv("DB4A_TESTBOOL", val, 1); else unsetenv("DB4A_TESTBOOL");
    return cfg_bool("DB4A_TESTBOOL", dflt);
}

int main(void) {
    printf("cfg_bool\n");

    chk("unset keeps default off",   with(NULL, 0), 0);
    chk("unset keeps default on",    with(NULL, 1), 1);

    /* The case that was broken: an explicit no must be read as no. */
    chk("0 is off, not present",     with("0", 0), 0);
    chk("0 beats a default of on",   with("0", 1), 0);
    chk("no",                        with("no", 1), 0);
    chk("off",                       with("off", 1), 0);
    chk("false",                     with("false", 1), 0);
    chk("NO is case-insensitive",    with("NO", 1), 0);
    chk("Off is case-insensitive",   with("Off", 1), 0);

    chk("1 turns a default-off on",  with("1", 0), 1);
    chk("yes",                       with("yes", 0), 1);
    chk("on",                        with("on", 0), 1);
    chk("true",                      with("true", 0), 1);
    chk("TRUE is case-insensitive",  with("TRUE", 0), 1);

    /* `DB4A_MUTE= make play` is how a shell says no. */
    chk("empty is off, not present", with("", 1), 0);

    /* A typo must not read as yes. It warns on stderr and falls back. */
    printf("  (a warning on stderr is expected next)\n");
    chk("junk falls back to on",     with("yse", 1), 1);
    chk("junk falls back to off",    with("yse", 0), 0);

    /* placement_override_active() is the inverse of its setting, and this
       branch is where that setting exists. The name describes the CARTRIDGE's
       behaviour, so place_scroll=1 puts the re-centring back and takes the
       override OFF, and unset follows mouse control. Modelled here rather than
       called, because getting the inversion backwards is silent. */
    printf("place_scroll inversion\n");
    for (int mouse = 0; mouse <= 1; mouse++) {
        char name[64];
        snprintf(name, sizeof name, "unset follows mouse=%d", mouse);
        chk(name, !with(NULL, !mouse), mouse);
        snprintf(name, sizeof name, "=1 cartridge back, mouse=%d", mouse);
        chk(name, !with("1", !mouse), 0);
        snprintf(name, sizeof name, "=0 override on, mouse=%d", mouse);
        chk(name, !with("0", !mouse), 1);
    }

    if (fails) { printf("\n%d FAILED\n", fails); return 1; }
    printf("\nall cfg_bool cases correct\n");
    return 0;
}
