#ifndef CONFIG_H
#define CONFIG_H

/* Settings from a file, so a player can tinker without a shell.
 *
 * db4a.conf next to the binary, or wherever DB4A_CONF points. Format is
 * `key = value`, one per line, # starts a comment, blank lines ignored. Keys
 * are the environment names without the DB4A_ prefix, lowercased:
 *
 *     gain = 8          # same as DB4A_GAIN=8
 *
 * **An environment variable always wins over the file.** That is what keeps
 * the test suite working: every test drives the binary through env vars, and
 * a stray line in someone's config must not be able to change what a test
 * does.
 *
 * Only PLAYER settings are read this way. The diagnostic and harness controls
 * -- DB4A_REPLAY, DB4A_SHOTS, DB4A_WATCH, DB4A_NATIVE, the DB4A_LOG_* family
 * and so on -- stay environment-only on purpose. They exist to make one run
 * behave unusually; putting them in a file that persists across runs is how
 * you end up debugging your own configuration.
 */
void        config_load(void);              /* call once at startup */
const char *cfg(const char *env_name);      /* env first, then the file, else NULL */

#endif
