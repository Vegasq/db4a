/* See include/config.h. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_KEYS 64
static struct { char key[32]; char val[192]; } entries[MAX_KEYS];
static unsigned n_entries;
static int loaded;

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

void config_load(void) {
    if (loaded) return;
    loaded = 1;
    const char *path = getenv("DB4A_CONF");
    if (!path) path = "db4a.conf";
    FILE *f = fopen(path, "r");
    if (!f) return;                       /* no file is not an error */

    char line[256];
    unsigned lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) { trim(line); if (*line) fprintf(stderr,
                   "%s:%u: ignored, expected key = value\n", path, lineno); continue; }
        *eq = 0;
        char k[32], v[192];
        /* Explicit precision, so a long line truncates quietly rather than
           tripping -Wformat-truncation. */
        snprintf(k, sizeof k, "%.*s", (int)sizeof k - 1, line);   trim(k);
        snprintf(v, sizeof v, "%.*s", (int)sizeof v - 1, eq + 1); trim(v);
        if (!*k) continue;
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (n_entries == MAX_KEYS) { fprintf(stderr, "%s: too many settings\n", path); break; }
        snprintf(entries[n_entries].key, sizeof entries[0].key, "%s", k);
        snprintf(entries[n_entries].val, sizeof entries[0].val, "%s", v);
        n_entries++;
    }
    fclose(f);
    if (n_entries) printf("config: %u setting(s) from %s\n", n_entries, path);
}

const char *cfg(const char *env_name) {
    const char *e = getenv(env_name);
    if (e) return e;                      /* the environment always wins */
    if (!loaded) config_load();
    const char *k = env_name;
    if (strncmp(k, "DB4A_", 5) == 0) k += 5;
    for (unsigned i = 0; i < n_entries; i++) {
        size_t j = 0;
        for (; k[j] && entries[i].key[j]; j++)
            if (tolower((unsigned char)k[j]) != entries[i].key[j]) break;
        if (!k[j] && !entries[i].key[j]) return entries[i].val;
    }
    return NULL;
}
