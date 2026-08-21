// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/mod.h - what makes a feature a mod.
 *
 * A feature declares itself next to its own install function. The descriptors
 * land in one linker section and common.c walks it, so adding a mod is a new
 * file and nothing else -- nothing lists the features and nothing includes them
 * all.
 *
 * Declaration is a static initialiser; install is [init].
 */
#ifndef EP122_MOD_KIT_MOD_H
#define EP122_MOD_KIT_MOD_H

struct kit_mod {
    /* Tags every slot this mod patches, so it is what an uninstall is addressed
     * by and what the install summary reports. */
    const char *name;

    const char *what;   /* one line, in the install log */

    /* Install order, ascending; ties broken by name. The mods sit at 10..80 in
     * tens, so a feature that must run between two others has nine numbers to
     * take without renumbering either. */
    short prio;

    /* 0 when the mod is in, -1 when the deck runs stock for this feature.
     * Whatever a refusing install patched is unwound by the caller. */
    int (*install)(void);   /* [init] */
};

/* One descriptor. `used` because nothing in C refers to it: the section is the
 * reference. */
#define KIT_MOD(sym, ...) \
    static const struct kit_mod sym __attribute__((used, \
        section("ep122_mods"))) = { __VA_ARGS__ }

/* The section bounds, defined by GNU ld for any C-identifier section name.
 * Hidden explicitly: -fvisibility=hidden covers what the compiler emits, not
 * what the linker defines, and every symbol the shim exports interposes that
 * name in EP122. */
extern const struct kit_mod __start_ep122_mods[] __attribute__((visibility("hidden")));
extern const struct kit_mod __stop_ep122_mods[] __attribute__((visibility("hidden")));

#endif /* EP122_MOD_KIT_MOD_H */
