// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * loglevel.h - the log level grammar, shared by the shim and the sidecar.
 *
 * Two binaries, two toolchains, one spelling. They are separate units with
 * separate environment variables (EP122_MOD_LOGLEVEL, STEMD_LOGLEVEL), so
 * turning one up does not drag the other with it, but a level means the same
 * thing in both and parses the same way. That is the whole reason this is a
 * header rather than two private copies that drift.
 *
 * ERROR is the default everywhere. A deck in a booth says nothing while it
 * works.
 */
#ifndef CDJ3K_LOGLEVEL_H
#define CDJ3K_LOGLEVEL_H

#include <stddef.h>   /* NULL -- this header stands alone */

enum {
    LOG_ERROR = 0,   /* only what is broken */
    LOG_WARN  = 1,   /* + degraded: a feature that refused, a value not saved */
    LOG_INFO  = 2,   /* + what installed, what a track load decided */
    LOG_DEBUG = 3,   /* + everything a user action produces */
    LOG_TRACE = 4    /* + everything a frame produces */
};

/* Parse a level from an environment string: a name ("warn") or a single digit
 * ("1"), case-insensitive, leading blanks allowed.
 *
 *   >= 0   the level
 *   -1     `s` names no level -- the caller decides what to say about that
 *
 * NULL and empty are NOT errors: they mean unset, which is LOG_ERROR. Only text
 * that was meant to select something and failed returns -1, so a typo can be
 * reported instead of silently reading as the quietest setting.
 */
static inline int log_level_from(const char *s)
{
    static const char *const k_names[] = { "error", "warn", "info", "debug", "trace" };
    unsigned i;

    if (s == NULL) return LOG_ERROR;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return LOG_ERROR;

    if (s[0] >= '0' && s[0] <= '9' && s[1] == '\0')
        return s[0] - '0' > LOG_TRACE ? LOG_TRACE : s[0] - '0';

    for (i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
        const char *a = s, *b = k_names[i];

        while (*b && *a && (*a | 32) == *b) { a++; b++; }
        /* Whole word only: "info" matches, "infomercial" does not. */
        if (*b == '\0' && (*a == '\0' || *a == ' ' || *a == '\t'))
            return (int)i;
    }
    return -1;
}

#endif /* CDJ3K_LOGLEVEL_H */
