/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/core/resolve_internal.h - what the resolver's three mechanisms share.
 *
 * resolve.c maps the image and drives the passes; resolve_rtti.c walks the
 * typeinfo; resolve_sig.c matches instructions. Declarations only.
 */
#ifndef EP122_MOD_CORE_RESOLVE_INTERNAL_H
#define EP122_MOD_CORE_RESOLVE_INTERNAL_H

#include "core/mod_core.h"

int ti_looks_real(uintptr_t ti);
int ti_kind(uintptr_t ti);
int ti_from_name_ref(uintptr_t at, void *user);
int ti_classify(uintptr_t at, void *user);
int seen_vptr(uintptr_t *seen, int *n, uintptr_t v);
int rtti_base_offset(uintptr_t ti, const char *base, int depth, long at, long *off_out, int *virt);
uintptr_t vt_virtual_base(uintptr_t primary, uintptr_t ti, long want_top);
int bl_targets(uintptr_t start, unsigned span, uintptr_t *out, int max);
uintptr_t adrp_pair(uintptr_t fn, unsigned insn);
uintptr_t bl_target(uintptr_t fn, unsigned insn);

#define SIG_HITS_MAX 32

#define TI_SAMPLE_MAX 64

/* ---- the batched vtable phase --------------------------------------------
 *
 * Resolving each class on its own means three full sweeps of a 45 MB image per
 * class -- one to find its name, one to find the type_info pointing at the name,
 * one to find the vtable pointing at the type_info. At 44 classes that is over a
 * hundred sweeps before a single signature has been looked at, and this runs on
 * every EP122 start on an RK3399.
 *
 * So all 44 are resolved together: one sweep finds every name, one finds every
 * type_info, one finds every vtable. Same answers, a fortieth of the traffic.
 *
 * Two name occurrences are kept per class rather than one. The linker packs
 * these strings against whatever precedes them and merges shared tails, so a
 * name can legitimately appear more than once and the copy that matters is not
 * always the first -- which is exactly how pcmbuf::SimpleBuffer went missing
 * the first time. The type_info test downstream is what picks between them. */


struct ti_hunt { uintptr_t ti; int count; };

struct ti_sample {
    uintptr_t vptr;
    int       n;      /* records wearing it (capped)           */
    int       si;     /* ...whose tail reads as a base pointer  */
    int       vmi;    /* ...whose tail reads as (flags, count)  */
};


/* The mapped image and the RTTI pass state, defined in resolve.c. */
extern uintptr_t g_ti_class, g_ti_si, g_ti_vmi;

extern size_t    g_text_len;
extern uintptr_t g_text_va;

int in_text(uintptr_t va);

/* How wide a vtable group the sweep reads at a time. */
#define VT_GROUP_WORDS 4096

/* The mapped image, read by the RTTI walk. Defined in resolve.c. */
int in_image(uintptr_t va, size_t len);
uintptr_t peek(uintptr_t va);

#endif /* EP122_MOD_CORE_RESOLVE_INTERNAL_H */
