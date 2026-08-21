// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * resolve.h - EP122 symbols by identity instead of by address.
 *
 * EP122 ships stripped. mods/ep122_syms.spec names what the mods need -- an RTTI
 * class, a vtable slot, a masked instruction signature -- and this resolves those
 * against whatever binary the deck is running. An address would be true for
 * exactly one firmware build and silently wrong for the next.
 *
 * Call ep122_resolve() ONCE from the constructor, before any mod installs. A
 * symbol that could not be resolved stays 0, which the mods treat exactly as
 * they already treat a failed prologue guard: skip the hook, log why, leave the
 * deck stock. Nothing here writes to EP122's memory.
 */
#ifndef EP122_RESOLVE_H
#define EP122_RESOLVE_H

#include <stdint.h>

#include "core/ep122_syms.h"

#ifdef __cplusplus
extern "C" {
#endif


/* Resolve everything in the spec. Returns the number of symbols that resolved;
 * ep122_resolve_missing() is how many did not. Safe to call twice (the second
 * call is a no-op). */
int ep122_resolve(void);
int ep122_resolve_missing(void);

/* Print the names of the unresolved symbols, unconditionally. */
void ep122_resolve_log_missing(void);

/* Cheap "is this process even the deck?", without the scan.
 *
 * Maps the program headers and looks at how much code there is: EP122 carries
 * ~45 MB, every shell helper the shim is preloaded into carries a fraction of
 * that. It exists so the boot-strike flag can be raised BEFORE the scan runs --
 * arming after would leave the scan itself unguarded, and arming without this
 * would have apl_start.sh's bash children writing the flag. */
int ep122_image_is_deck(void);

/* The resolved addresses, indexed by enum ep122_sym. 0 means "not found" --
 * always the value a mod has to be prepared for. */
extern uintptr_t g_ep122_sym[EP122_SYM__COUNT];

static inline uintptr_t ep122_sym(int id)
{
    return (id >= 0 && id < EP122_SYM__COUNT) ? g_ep122_sym[id] : 0;
}

/* For the one log line that says what did not resolve. */
const char *ep122_sym_name(int id);

/* The address of the single live object of a class, found by scanning the
 * writable segments for its vptr. For a global singleton whose address nothing
 * in .text ever computes -- every caller already holds the pointer -- this is
 * the only handle there is. Returns 0 unless exactly one instance exists, so an
 * ambiguous answer is a refusal rather than a guess.
 *
 * Call it LATE. A global's vptr is written by the static-init pass, so asking
 * during the shim's own constructor can legitimately find nothing. */
uintptr_t ep122_find_instance(int vt_sym);

/* Late-bound values that no static description can reach, recorded from a live
 * call instead. See the `capture` entries in ep122_syms.spec. */
void      ep122_capture_set(int id, uintptr_t value);
uintptr_t ep122_capture_get(int id);

/* Ids for the captured values. They are not in enum ep122_sym because there is
 * nothing to resolve -- they exist so the inventory is complete and so the two
 * users agree on a slot. */
enum ep122_capture {
    EP122_CAP_READER_FACTORY,
    EP122_CAP_VALUE_COLOUR,
    EP122_CAP__COUNT
};


#ifdef __cplusplus
}
#endif

#endif /* EP122_RESOLVE_H */
