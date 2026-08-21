// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * resolve.c - find EP122's internals at run time.
 *
 * The three mechanisms and why a class name is the durable handle: docs/mods.md.
 *
 * Reading our own image: every other pointer in the mods goes through
 * mod_safe_read because it is a guess that might not be mapped. These do not --
 * the segment bounds come from the program headers via getauxval(AT_PHDR), so a
 * scan confined to a PT_LOAD's file-backed range is reading pages this process is
 * running out of. A pread per 8 bytes across ~30 MB would not be safer, only
 * slower.
 *
 * The scans stay inside p_filesz rather than p_memsz: .bss is mapped but holds
 * nothing we are looking for, and its tail may be short.
 *
 * Cost on the deck is one pass for the names, two for the pointers and one over
 * .text. Nothing here allocates and nothing here writes.
 */

#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <elf.h>

#define EP122_SYMS_IMPL
#include "core/resolve_internal.h"
#include "core/resolve.h"

/* How many times one class name may appear before the walker stops. */
#define NAME_OCCURRENCES 2

#define VT_GROUP_WORDS 4096

struct vt_work {
    uintptr_t name[NAME_OCCURRENCES];
    int       nname;
    uintptr_t ti;
    int       nti;         /* >1 means the name does not identify a class */
    long      want_top;
    /* Non-zero for a VIRTUAL base: where in the vtable its real offset lives.
     * want_top is then 0, so the sweep brings back the class's PRIMARY vtable
     * and vt_virtual_base walks from there to the one we actually want. */
    long      vbase_at;
    uintptr_t vt;
    int       nvt;
};


static int ti_collect(uintptr_t at, int which, void *user);
static int vt_collect(uintptr_t at, int which, void *user);

static void sig_scan_all(void);
static void resolve_fn(int idx);

uintptr_t g_ep122_sym[EP122_SYM__COUNT];
static uintptr_t g_ep122_cap[EP122_CAP__COUNT];
static int g_resolved, g_missing;

const char *ep122_sym_name(int id)
{
    if (id < 0 || id >= EP122_SYM__COUNT)
        return "?";
    return k_ep122_sym_name[id];
}

void ep122_capture_set(int id, uintptr_t value)
{
    if (id >= 0 && id < EP122_CAP__COUNT && value)
        __atomic_store_n(&g_ep122_cap[id], value, __ATOMIC_RELEASE);
}

uintptr_t ep122_capture_get(int id)
{
    if (id < 0 || id >= EP122_CAP__COUNT)
        return 0;
    return __atomic_load_n(&g_ep122_cap[id], __ATOMIC_ACQUIRE);
}

int ep122_resolve_missing(void) { return g_missing; }

/* The names behind the count, for the refusal path: a count cannot be acted on,
 * the names say which classes moved. Wrapped rather than one per line -- a failed
 * RTTI bootstrap leaves every symbol missing. */
void ep122_resolve_log_missing(void)
{
    int i, col = 0;

    if (!g_missing)
        return;
    MERR("resolve: unresolved:");
    for (i = 0; i < EP122_SYM__COUNT; i++) {
        const char *n;

        if (g_ep122_sym[i])
            continue;
        n = ep122_sym_name(i);
        if (col > 60) {
            fprintf(stderr, "\n[ep122_mod] resolve:  ");
            col = 0;
        }
        fprintf(stderr, " %s", n);
        col += (int)strlen(n) + 1;
    }
    fputc('\n', stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------------ image */

/* The loaded segments, straight out of our own program headers. */
#define MAX_SEG 8

struct seg {
    uintptr_t va;        /* first mapped address                              */
    size_t    file_len;  /* file-backed length: the only part worth scanning  */
    size_t    mem_len;   /* including .bss, which is mapped but holds nothing */
    int       exec;
};

static struct seg g_seg[MAX_SEG];
static int g_nseg;
uintptr_t g_text_va;
size_t    g_text_len;

static int image_map(void)
{
    const Elf64_Phdr *ph = (const Elf64_Phdr *)getauxval(AT_PHDR);
    unsigned long n = getauxval(AT_PHNUM);
    unsigned long ent = getauxval(AT_PHENT);
    unsigned long i;

    if (g_nseg)
        return 0;               /* idempotent: the segment table is append-only */
    if (!ph || !n || ent != sizeof(Elf64_Phdr)) {
        MDBG("resolve: no usable program headers (phdr %p n %lu ent %lu)\n",
             (const void *)ph, n, ent);
        return -1;
    }
    /* EP122 is ET_EXEC and non-PIE, so p_vaddr is already the runtime address
     * and there is no load bias to add. Asserting that rather than assuming it:
     * AT_PHDR has to fall inside the segment that maps the headers. */
    for (i = 0; i < n && g_nseg < MAX_SEG; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_filesz)
            continue;
        g_seg[g_nseg].va = (uintptr_t)ph[i].p_vaddr;
        g_seg[g_nseg].file_len = (size_t)ph[i].p_filesz;
        g_seg[g_nseg].mem_len = (size_t)ph[i].p_memsz;
        g_seg[g_nseg].exec = (ph[i].p_flags & PF_X) != 0;
        if (g_seg[g_nseg].exec) {
            g_text_va = g_seg[g_nseg].va;
            g_text_len = g_seg[g_nseg].file_len;
        }
        g_nseg++;
    }
    for (i = 0; i < (unsigned long)g_nseg; i++)
        if ((uintptr_t)ph >= g_seg[i].va &&
            (uintptr_t)ph < g_seg[i].va + g_seg[i].file_len)
            break;
    if (i == (unsigned long)g_nseg) {
        MDBG("resolve: AT_PHDR %p outside every PT_LOAD -- image is relocated, "
             "refusing to scan\n", (const void *)ph);
        g_nseg = 0;
        return -1;
    }
    if (!g_text_len) {
        MDBG("resolve: no executable PT_LOAD\n");
        g_nseg = 0;
        return -1;
    }
    MTRACE("resolve: %d segments, text %#lx+%#lx\n", g_nseg,
         (unsigned long)g_text_va, (unsigned long)g_text_len);
    return 0;
}

/* Is `va` inside a mapped segment? Everything dereferenced below passes through
 * here first.
 *
 * Against p_memsz, not p_filesz: .bss is mapped and some of what we resolve
 * lives there -- the skin's juce::Colour globals are zero-initialised, so they
 * have no bytes in the file and every one of them would be rejected if this
 * only accepted file-backed addresses. The SCANS still stop at p_filesz, since
 * a region that is zero at load time holds no names, vtables or code. */
int in_image(uintptr_t va, size_t len)
{
    int i;

    for (i = 0; i < g_nseg; i++)
        if (va >= g_seg[i].va && len <= g_seg[i].mem_len &&
            va - g_seg[i].va <= g_seg[i].mem_len - len)
            return 1;
    return 0;
}

uintptr_t peek(uintptr_t va)
{
    uintptr_t v;

    if (!in_image(va, sizeof(v)))
        return 0;
    memcpy(&v, (const void *)va, sizeof(v));
    return v;
}

int in_text(uintptr_t va)
{
    return va >= g_text_va && va < g_text_va + g_text_len;
}

/* ------------------------------------------------------------------- RTTI */

/* The three __cxxabiv1 type_info flavour vptrs.
 *
 * They cannot be found by name: this binary carries no RTTI for __cxxabiv1's
 * own classes (the three `N10__cxxabiv1..._type_infoE` strings that exist
 * belong to the demangler, and nothing points at them). They do not need to be.
 * A type_info's first word IS its flavour vptr, so the first spec class we look
 * up hands us one, and walking its base graph reaches the other two.
 *
 * Flavour then comes from layout rather than from a name. Past the 16-byte
 * head, __si_ holds one type_info pointer, __vmi_ holds (flags:u32,
 * count:u32), and the base-less flavour holds nothing at all, so whatever
 * follows it satisfies neither rule.
 */
uintptr_t g_ti_class, g_ti_si, g_ti_vmi;

/* Every 8-aligned qword in the image equal to any of `targets`, handed to `fn`
 * along with which target matched.
 *
 * Batched: 44 classes need two lookups each, so a pass per lookup is 88 sweeps of
 * a 45 MB image before a single signature is scanned. Sorting the targets turns
 * the inner test into a branch plus an occasional binary search, so the whole set
 * costs one pass.
 *
 * `targets` must be sorted ascending and free of duplicates. */
typedef int (*ptr_batch_fn)(uintptr_t at, int which, void *user);

static int target_index(const uintptr_t *targets, int n, uintptr_t v)
{
    int lo = 0, hi = n - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;

        if (targets[mid] == v) return mid;
        if (targets[mid] < v) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static void for_each_ptr_to_any(const uintptr_t *targets, int ntarget,
                                ptr_batch_fn fn, void *user)
{
    uintptr_t lo, hi;
    int i;

    if (ntarget <= 0)
        return;
    lo = targets[0];
    hi = targets[ntarget - 1];

    for (i = 0; i < g_nseg; i++) {
        const uintptr_t *p = (const uintptr_t *)g_seg[i].va;
        size_t n = g_seg[i].file_len / sizeof(uintptr_t), k;

        for (k = 0; k < n; k++) {
            uintptr_t v = p[k];
            int w;

            /* The range test rejects the overwhelming majority for two
             * compares; only survivors reach the search. */
            if (v < lo || v > hi)
                continue;
            w = target_index(targets, ntarget, v);
            if (w < 0)
                continue;
            /* One address can appear several times with different owners, so
             * the whole run of equal targets is reported, not just the entry
             * the search happened to land on. */
            while (w > 0 && targets[w - 1] == v)
                w--;
            for (; w < ntarget && targets[w] == v; w++)
                if (fn(g_seg[i].va + k * sizeof(uintptr_t), w, user))
                    return;
        }
    }
}

/* Single-target form, for the two places that genuinely want one. */
typedef int (*ptr_visit_fn)(uintptr_t at, void *user);

static void for_each_ptr_to(uintptr_t target, ptr_visit_fn fn, void *user)
{
    int i;

    for (i = 0; i < g_nseg; i++) {
        const uintptr_t *p = (const uintptr_t *)g_seg[i].va;
        size_t n = g_seg[i].file_len / sizeof(uintptr_t), k;

        for (k = 0; k < n; k++)
            if (p[k] == target &&
                fn(g_seg[i].va + k * sizeof(uintptr_t), user))
                return;
    }
}



struct vt_work g_vtw[EP122_N_VT];

/* Pass 1: every spec class name, in one sweep per distinct first byte.
 * Itanium mangling gives all of these the same leading character ('N' for a
 * nested name, '*' for the unique-name form), so this is one or two sweeps
 * total; each candidate position is then compared against the names sharing
 * that byte.
 *
 * Returns how many of the spec's classes were found. Zero means the process
 * does not contain EP122's C++ at all, which is the cheapest honest answer to
 * "is this the deck" available -- see ep122_image_is_deck(). */
static int g_names_done, g_names_found;

static int names_pass(void)
{
    /* Lengths once, not once per candidate: there are ~180k 'N' bytes in a
     * 45 MB image and 44 names to test at each, so a strlen in the inner loop
     * is a quarter of a gigabyte of reads doing nothing. */
    static unsigned short len[EP122_N_VT];
    unsigned char firsts[4];
    int nfirst = 0, i, j, s;

    if (g_names_done)
        return g_names_found;
    g_names_done = 1;

    for (i = 0; i < EP122_N_VT; i++) {
        unsigned char c = (unsigned char)k_ep122_vt[i].cls[0];

        len[i] = (unsigned short)strlen(k_ep122_vt[i].cls);
        for (j = 0; j < nfirst; j++)
            if (firsts[j] == c)
                break;
        if (j == nfirst && nfirst < (int)sizeof(firsts))
            firsts[nfirst++] = c;
    }

    for (s = 0; s < g_nseg; s++)
        for (j = 0; j < nfirst; j++) {
            const char *p = (const char *)g_seg[s].va;
            size_t left = g_seg[s].file_len;

            while (left > 1) {
                const char *hit = memchr(p, firsts[j], left - 1);
                size_t room;

                if (!hit)
                    break;
                room = g_seg[s].file_len -
                       (size_t)((uintptr_t)hit - g_seg[s].va);
                for (i = 0; i < EP122_N_VT; i++) {
                    if (g_vtw[i].nname >= NAME_OCCURRENCES ||
                        (unsigned char)k_ep122_vt[i].cls[0] != firsts[j])
                        continue;
                    if (len[i] + 1u <= room &&
                        !memcmp(hit, k_ep122_vt[i].cls, len[i] + 1u)) {
                        if (!g_vtw[i].nname)
                            g_names_found++;
                        g_vtw[i].name[g_vtw[i].nname++] = (uintptr_t)hit;
                    }
                }
                left -= (size_t)(hit - p) + 1;
                p = hit + 1;
            }
        }
    return g_names_found;
}

/* Build the sorted, de-duplicated target array the batched sweep wants, plus a
 * parallel array saying which class each target belongs to. */
static int build_targets(uintptr_t *targets, unsigned char *owner, int max,
                         int use_ti)
{
    int n = 0, i, k;

    for (i = 0; i < EP122_N_VT; i++) {
        uintptr_t vals[NAME_OCCURRENCES];
        int nval, v;

        if (use_ti) {
            vals[0] = g_vtw[i].ti;
            nval = (g_vtw[i].nti == 1 && g_vtw[i].ti) ? 1 : 0;
        } else {
            for (k = 0; k < g_vtw[i].nname; k++)
                vals[k] = g_vtw[i].name[k];
            nval = g_vtw[i].nname;
        }
        for (v = 0; v < nval && n < max; v++) {
            int pos = n, j;

            /* Insertion sort: 44 classes, at most 88 entries, and the sweep
             * that follows costs orders of magnitude more than this does.
             *
             * Duplicates are KEPT. Three spec entries name gui::UtilityView --
             * the view itself and the two secondary vtables picked out by base
             * -- so one address legitimately has several owners, and dropping
             * the repeats silently resolved only the first of them. */
            while (pos > 0 && targets[pos - 1] > vals[v])
                pos--;
            for (j = n; j > pos; j--) {
                targets[j] = targets[j - 1];
                owner[j] = owner[j - 1];
            }
            targets[pos] = vals[v];
            owner[pos] = (unsigned char)i;
            n++;
        }
    }
    return n;
}

unsigned char g_owner[EP122_N_VT * NAME_OCCURRENCES];


/* One candidate flavour vptr and what its records look like past the head. */


static int rtti_bootstrap(void)
{
    uintptr_t todo[64], seen_ti[64], vptr[8];
    int ntodo = 0, nseen = 0, nvptr = 0, i;

    /* Any class in the spec will do; the first whose name the sweep found seeds
     * the walk. Its own type_info costs one more sweep and nothing after that:
     * the base graph is walked through pointers, not searched for. */
    for (i = 0; i < EP122_N_VT && !ntodo; i++) {
        struct ti_hunt h = { 0, 0 };

        if (!g_vtw[i].nname)
            continue;
        for_each_ptr_to(g_vtw[i].name[0], ti_from_name_ref, &h);
        if (h.ti)
            todo[ntodo++] = h.ti;
    }
    if (!ntodo) {
        MDBG("resolve: no spec class name found in the image at all\n");
        return -1;
    }

    while (ntodo) {
        uintptr_t ti = todo[--ntodo], nxt;
        uint32_t cnt, flags;
        int dup = 0;

        for (i = 0; i < nseen; i++)
            if (seen_ti[i] == ti) { dup = 1; break; }
        if (dup || !ti_looks_real(ti))
            continue;
        if (nseen < (int)(sizeof(seen_ti) / sizeof(seen_ti[0])))
            seen_ti[nseen++] = ti;
        seen_vptr(vptr, &nvptr, peek(ti));

        /* Try BOTH base layouts. A wrong reading yields addresses that fail
         * ti_looks_real and contributes nothing, so there is no need to know
         * the flavour before we have classified it. */
        nxt = peek(ti + 16);
        if (ntodo < (int)(sizeof(todo) / sizeof(todo[0])))
            todo[ntodo++] = nxt;
        flags = (uint32_t)(nxt & 0xFFFFFFFFu);
        cnt = (uint32_t)(nxt >> 32);
        if (cnt >= 1 && cnt <= 64 && flags < 0x20)
            for (i = 0; i < (int)cnt; i++)
                if (ntodo < (int)(sizeof(todo) / sizeof(todo[0])))
                    todo[ntodo++] = peek(ti + 24 + (size_t)i * 16);
    }

    /* Classify: for every record wearing a candidate vptr, does the word past
     * the head read as a base pointer, as a (flags, count) pair, or neither?
     *
     * Sampled by scanning the image for that vptr rather than from the walk
     * above. The walk exists to DISCOVER the three candidates and reaches only a
     * handful of records doing it; classifying off that handful gives counts too
     * small to tell a real flavour from a stray. */
    for (i = 0; i < nvptr; i++) {
        struct ti_sample s = { vptr[i], 0, 0, 0 };

        for_each_ptr_to(vptr[i], ti_classify, &s);
        /* A real flavour vptr is worn by hundreds of type_infos. Reading a
         * base-less record's tail as if it held a base pointer occasionally
         * lands on something that passes ti_looks_real, and that stray is worn
         * by exactly one -- the count is what separates them. */
        if (s.n < 4)
            continue;
        /* __si_ is allowed stragglers: a base can be a pointer or fundamental
         * type_info, which wears a fourth vptr we never reach. __vmi_ and the
         * base-less flavour have no such escape and must be unanimous. */
        if (s.vmi == s.n && s.si == 0)
            g_ti_vmi = vptr[i];
        else if (s.si * 10 >= s.n * 9 && s.vmi == 0)
            g_ti_si = vptr[i];
        else if (s.si == 0 && s.vmi == 0)
            g_ti_class = vptr[i];
    }

    if (!g_ti_si || !g_ti_vmi) {
        MDBG("resolve: RTTI bootstrap failed (class %#lx si %#lx vmi %#lx from "
             "%d records, %d vptrs)\n", (unsigned long)g_ti_class,
             (unsigned long)g_ti_si, (unsigned long)g_ti_vmi, nseen, nvptr);
        return -1;
    }
    MDBG("resolve: type_info vptrs class %#lx si %#lx vmi %#lx\n",
         (unsigned long)g_ti_class, (unsigned long)g_ti_si,
         (unsigned long)g_ti_vmi);
    return 0;
}

/* Where `base` sits inside `ti`: 0 when it is a base, -1 when it is not.
 * Depth-limited, because the base graph is a DAG and UtilityView alone has
 * 40-odd of them.
 *
 * *virt is set when the base is VIRTUAL, and *off is then NOT an offset: the
 * ABI cannot record one, because where a virtual base sits depends on the
 * complete object rather than on the class. What it records instead is where in
 * the vtable the real offset lives, which is what the caller goes and reads.
 * This is not a corner: juce::Component is a virtual base of gui::WidgetBase,
 * so it is how EVERY gui:: widget reaches its own Component.
 *
 * That makes *off signed and freely negative, which is why found/not-found is
 * the return value and the offset comes back by pointer -- an in-band -1 read a
 * virtual base as absent, and absent means the symbol does not resolve, which
 * means every mod is off.
 *
 * A virtual edge is only followed while the walk is still at offset 0: the
 * vtable offset is recorded relative to the vptr of the class DECLARING the
 * base, so reaching it needs that class to share the complete object's vptr --
 * true all the way down a primary base chain, which is where all of these live,
 * and reported as "not a base" rather than as a wrong number anywhere else. */
/* The vtable serving a VIRTUAL base, given the class's primary one.
 *
 * A class's vtables are emitted as ONE object, primary first, so the one we
 * want is a bounded walk ahead rather than another sweep of the image -- and
 * the sweep is the expensive half of resolving. Each is preceded by its
 * offset-to-top and its typeinfo, which is what this matches on.
 *
 * Refuses on a second match rather than taking the first. What else carries
 * this typeinfo at this offset-to-top is a CONSTRUCTION vtable, whose slot 0 is
 * null because a half-built object cannot be deleted -- so the code test below
 * is what separates them, and two survivors would mean it stopped being true. */

/* -------------------------------------------------------------- signatures */

/* The function the BL at `insn` of `fn` calls.
 *
 * For a callee whose SIGNATURE is worthless -- a template instantiation with a
 * stock prologue names hundreds of functions, more than the scan will hold --
 * but whose position in a known caller is exact. Same argument as adrp_pair
 * above: the deck's own code computes the address, so read what it computes. */
/* ---------------------------------------------------------------- instances */

uintptr_t ep122_find_instance(int vt_sym)
{
    uintptr_t vt = ep122_sym(vt_sym), found = 0;
    int n = 0, i;

    if (!vt)
        return 0;
    /* Over p_memsz, not p_filesz: a global C++ object lives in .bss, has no
     * bytes in the file, and gets its vptr written by the static-init pass. It
     * is exactly the case this function exists for -- an address nothing in
     * .text ever computes, because every caller already holds the pointer.
     *
     * Writable segments only. The vtable address also appears in .rodata as the
     * typeinfo's neighbour and in other classes' vtables, and none of those are
     * objects. */
    for (i = 0; i < g_nseg && n < 2; i++) {
        const uintptr_t *p = (const uintptr_t *)g_seg[i].va;
        size_t words = g_seg[i].mem_len / sizeof(uintptr_t), k;

        if (g_seg[i].exec)
            continue;
        for (k = 0; k < words; k++)
            if (p[k] == vt) {
                found = g_seg[i].va + k * sizeof(uintptr_t);
                if (++n >= 2)
                    break;
            }
    }
    if (n != 1) {
        MDBG("resolve: %s has %d live instances, want exactly 1\n",
             ep122_sym_name(vt_sym), n);
        return 0;
    }
    return found;
}

/* ------------------------------------------------------------------- drive */


/* Every signature's matches, filled by ONE sweep of .text.
 *
 * Scanning per signature meant 48 sweeps of a 45 MB image. Every pattern has an
 * anchor -- its first fully-unmasked instruction word -- so one sweep can test
 * all 48 anchors at each position and only fall through to a full compare when
 * one hits. A 32-bit instruction word is selective enough that the fall-through
 * is rare. */
static struct {
    uintptr_t hit[SIG_HITS_MAX];
    int       n;              /* may exceed SIG_HITS_MAX; only the count matters */
} g_sig[EP122_N_FN];


/* Anchor index.
 *
 * The scan walks .text once, and for every word it has to decide which patterns
 * could start there. Testing each pattern in turn makes that O(words x patterns)
 * -- 11.4M words x 70 patterns is 0.8G compares on every EP122 start, and it
 * grows with the spec. Hashing the one fully-unmasked word each pattern already
 * carries turns it into a single bucket lookup, so the pattern count stops
 * mattering and a second processor's patterns cost nothing to carry.
 *
 * Buckets chain, because two patterns may legitimately share an anchor word; the
 * word is still compared inside the chain, so a hash collision only costs a test.
 */
#define SIG_HASH_BITS 8
#define SIG_HASH_SIZE (1u << SIG_HASH_BITS)
static int g_sig_head[SIG_HASH_SIZE];
static int g_sig_next[EP122_N_FN];

#define FNV64_OFF   0xcbf29ce484222325ULL
#define FNV64_PRIME 0x100000001b3ULL

/* Which bits of an instruction a signature compares.
 *
 * The twin of mask_word() in tools/gen-syms.py, and it has to agree with it
 * exactly. Only PC- and data-relative fields are dropped, because those move
 * when the linker puts the function somewhere else; everything that says what
 * the code DOES is kept.
 *
 * The mask is derived from the instruction in front of us rather than carried
 * in the table, so the tree holds none of the vendor's code and none of the
 * masks derived from it. That is sound because the bits which decide an
 * instruction's class are bits the mask KEEPS: if two windows agree once masked
 * then they agreed on the classes, so they produced the same masks. Verified
 * over every symbol on every extracted build.
 */
static uint32_t mask_word(uint32_t w, uint32_t *adrp_regs)
{
    if ((w & 0x7C000000u) == 0x14000000u)         /* B / BL: imm26 */
        return 0xFC000000u;
    if ((w & 0x1F000000u) == 0x10000000u) {       /* ADR / ADRP */
        if (w & 0x80000000u)
            *adrp_regs |= 1u << (w & 0x1F);       /* this reg now holds a page */
        return 0x9F00001Fu;
    }
    if ((w & 0x3B000000u) == 0x18000000u)         /* LDR/LDRSW literal: imm19 */
        return 0xFF00001Fu;
    if ((w & 0x7F800000u) == 0x11000000u) {       /* ADD imm, on an ADRP result */
        if (*adrp_regs & (1u << ((w >> 5) & 0x1F))) {
            *adrp_regs |= 1u << (w & 0x1F);       /* the sum is still that address */
            return 0xFFC003FFu;
        }
    }
    if ((w & 0x3B000000u) == 0x39000000u) {       /* LDR/STR imm, through one */
        if (*adrp_regs & (1u << ((w >> 5) & 0x1F)))
            return 0xFFC003FFu;
    }
    return 0xFFFFFFFFu;
}

/* FNV-1a over the window, each instruction masked by its own encoding. */
static unsigned long long window_digest(uintptr_t start, unsigned insns)
{
    const uint32_t *w = (const uint32_t *)start;
    unsigned long long h = FNV64_OFF;
    uint32_t adrp = 0;
    unsigned i, b;

    for (i = 0; i < insns; i++) {
        uint32_t v = w[i] & mask_word(w[i], &adrp);

        for (b = 0; b < 4; b++)
            h = (h ^ ((v >> (b * 8)) & 0xff)) * FNV64_PRIME;
    }
    return h;
}

/* The 16-bit key the table indexes an anchor word by.
 *
 * This runs on every word of .text, so it is a single multiply rather than a
 * digest -- and 16 bits of a 32-bit product identifies no instruction, which is
 * the point: the tree carries a key, never the vendor's word. A collision only
 * costs the window digest below, which then rejects it.
 */
static unsigned anchor_key(uint32_t w)
{
    return (unsigned)((w * 2654435761u) >> 16);
}

int ep122_image_is_deck(void)
{
    int found;

    if (image_map() != 0)
        return 0;

    /* Does this process contain EP122's own C++ classes, by name?
     *
     * Better than the size heuristic this replaces, which only said "big enough
     * to plausibly be a DJ player" and would have been fooled by any other large
     * binary the shim was ever preloaded into. Better than the obvious
     * alternatives too: AT_EXECFN or /proc/self/exe test what the file is
     * CALLED, which a rename or a wrapper script breaks, and a marker string
     * like "EP122Application" tests for a build artefact nobody promised to keep
     * -- whereas gui::UtilityView existing is the same fact the mods depend on.
     *
     * It is also not a separate cost. names_pass() is step one of the resolve
     * either way; running it here just lets everything more expensive be skipped
     * when it comes back empty. On a shell helper that is a memchr over a
     * megabyte or two, which is as close to free as a real answer gets. */
    found = names_pass();
    if (!found) {
        /* Every shell helper apl_start.sh spawns is preloaded too and every one
         * of them says this, so it is the single most numerous line in the log
         * and the least informative -- the deck is the process anyone is reading
         * the log for. */
        MTRACE("resolve: no EP122 class names in this image -- not the deck\n");
        return 0;
    }
    MDBG("resolve: %d/%d spec classes present -> this is the deck\n",
         found, EP122_N_VT);
    return 1;
}

int ep122_resolve(void)
{
    int i;

    if (g_resolved || g_missing)
        return g_resolved;
    if (!ep122_image_is_deck())
        return 0;

    if (rtti_bootstrap() != 0) {
        g_missing = EP122_SYM__COUNT;
        return 0;
    }

    {
        uintptr_t targets[EP122_N_VT * NAME_OCCURRENCES];
        int n;

        /* name -> type_info, then type_info -> vtable. One sweep each. */
        n = build_targets(targets, g_owner, (int)(sizeof(targets) / sizeof(targets[0])), 0);
        for_each_ptr_to_any(targets, n, ti_collect, NULL);

        for (i = 0; i < EP122_N_VT; i++) {
            struct vt_work *w = &g_vtw[i];

            if (w->nti != 1) {
                if (w->nti > 1)
                    MDBG("resolve: %s names %d type_infos -- refusing\n",
                         ep122_sym_name(k_ep122_vt[i].sym), w->nti);
                w->ti = 0;
                continue;
            }
            /* A secondary vtable is named by the base it serves, so the base's
             * offset has to be known before the sweep can recognise it. */
            w->want_top = 0;
            w->vbase_at = 0;
            if (k_ep122_vt[i].base) {
                int virt = 0;
                long off = 0;

                if (rtti_base_offset(w->ti, k_ep122_vt[i].base, 0, 0, &off,
                                     &virt) != 0) {
                    MDBG("resolve: %s is not a base of %s\n",
                         k_ep122_vt[i].base, k_ep122_vt[i].cls);
                    w->ti = 0;
                    continue;
                }
                /* A virtual base's offset is in the vtable, so the sweep has to
                 * bring the primary back first and the real answer comes after
                 * it. */
                if (virt)
                    w->vbase_at = off;
                else
                    w->want_top = -off;
            }
        }

        n = build_targets(targets, g_owner, (int)(sizeof(targets) / sizeof(targets[0])), 1);
        for_each_ptr_to_any(targets, n, vt_collect, NULL);

        for (i = 0; i < EP122_N_VT; i++) {
            struct vt_work *w = &g_vtw[i];

            if (w->nvt == 1 && w->vbase_at) {
                long off = (long)peek(w->vt + w->vbase_at);

                w->vt = vt_virtual_base(w->vt, w->ti, -off);
                if (!w->vt) {
                    MDBG("resolve: %s: no single vtable for virtual base %s "
                         "at offset %ld\n", ep122_sym_name(k_ep122_vt[i].sym),
                         k_ep122_vt[i].base, off);
                    continue;
                }
            }
            if (w->nvt == 1)
                g_ep122_sym[k_ep122_vt[i].sym] = w->vt;
            else if (w->nvt > 1)
                MDBG("resolve: %s has %d vtables at offset-to-top %ld\n",
                     ep122_sym_name(k_ep122_vt[i].sym), w->nvt, w->want_top);
        }
    }

    for (i = 0; i < EP122_N_SLOT; i++) {
        uintptr_t vt = ep122_sym(k_ep122_slot[i].vt), fn;

        if (!vt)
            continue;
        fn = peek(vt + k_ep122_slot[i].off);
        if (in_text(fn))
            g_ep122_sym[k_ep122_slot[i].sym] = fn;
    }

    /* A symbol may carry a descriptor per application processor: the .UPD ships
     * two EP122 binaries built by different compilers, so one signature cannot
     * cover both. The rows are emitted reference-first, and every write below is
     * guarded on the symbol still being empty, so the wrong processor's row can
     * only ever fill a gap -- never overwrite an answer that already resolved. */
    sig_scan_all();
    for (i = 0; i < EP122_N_FN; i++)
        resolve_fn(i);

    for (i = 0; i < EP122_N_DATA; i++) {
        uintptr_t src = ep122_sym(k_ep122_data[i].src);
        uintptr_t addr = src ? adrp_pair(src, k_ep122_data[i].insn) : 0;

        if (addr && in_image(addr, 8) && !g_ep122_sym[k_ep122_data[i].sym])
            g_ep122_sym[k_ep122_data[i].sym] = addr;
    }

    for (i = 0; i < EP122_N_CALL; i++) {
        uintptr_t src = ep122_sym(k_ep122_call[i].src);
        uintptr_t addr = src ? bl_target(src, k_ep122_call[i].insn) : 0;

        if (addr && in_text(addr) && !g_ep122_sym[k_ep122_call[i].sym])
            g_ep122_sym[k_ep122_call[i].sym] = addr;
    }

    for (i = 0; i < EP122_SYM__COUNT; i++) {
        if (g_ep122_sym[i])
            g_resolved++;
        else {
            g_missing++;
            MDBG("resolve: UNRESOLVED %s\n", ep122_sym_name(i));
        }
    }
    /* Which symbols a build found is the first thing anyone needs when a mod
     * silently does nothing on a firmware we have not seen -- but a deck where they
     * all resolved has nothing to say, and the refusal above is already ERROR. */
    MINFO("resolve: %d/%d symbols\n", g_resolved, EP122_SYM__COUNT);
    return g_resolved;
}

/*
 * tmp
 */

static void sig_scan_all(void)
{
    const uint32_t *base = (const uint32_t *)g_text_va;
    size_t words = g_text_len / 4, w;
    int i;

    for (w = 0; w < SIG_HASH_SIZE; w++)
        g_sig_head[w] = -1;
    for (i = 0; i < EP122_N_FN; i++) {
        unsigned h = k_ep122_fn[i].akey & (SIG_HASH_SIZE - 1);

        g_sig_next[i] = g_sig_head[h];
        g_sig_head[h] = i;
    }

    for (w = 0; w < words; w++) {
        uint32_t v = base[w];

        unsigned key = anchor_key(v);

        for (i = g_sig_head[key & (SIG_HASH_SIZE - 1)]; i >= 0; i = g_sig_next[i]) {
            const struct ep122_fn_spec *f = &k_ep122_fn[i];
            uintptr_t start;
            unsigned back = (unsigned)f->anchor * 4;

            if (key != f->akey)
                continue;              /* same bucket, different anchor word */
            if (w * 4 < back)
                continue;
            start = g_text_va + (w * 4 - back);
            if (!in_text(start) || !in_text(start + (uintptr_t)f->insns * 4 - 1))
                continue;
            if (window_digest(start, f->insns) != f->digest)
                continue;
            if (g_sig[i].n < SIG_HITS_MAX)
                g_sig[i].hit[g_sig[i].n] = start;
            g_sig[i].n++;
        }
    }
}

static void resolve_fn(int idx)
{
    const struct ep122_fn_spec *f = &k_ep122_fn[idx];
    const uintptr_t *hits = g_sig[idx].hit;
    int n = g_sig[idx].n;

    /* A truncated hit list would make the scoped path pick from a subset and
     * the count guard pass for the wrong reason, so overflow is a refusal. */
    if (n > SIG_HITS_MAX) {
        MDBG("resolve: %s matched %d times, more than the %d recorded\n",
             ep122_sym_name(f->sym), n, SIG_HITS_MAX);
        return;
    }

    if (f->scope == 0xffff) {
        if (g_sig[idx].n == 1 && !g_ep122_sym[f->sym])
            g_ep122_sym[f->sym] = hits[0];
        else
            MDBG("resolve: %s signature matched %d times, want 1\n",
                 ep122_sym_name(f->sym), g_sig[idx].n);
        return;
    }

    /* Scoped: the signature alone cannot separate a run of siblings that differ
     * only in the global they touch, so take the nth of those the enclosing
     * function calls. `total` is the guard -- a firmware that adds or drops one
     * changes the count, and we would rather resolve nothing than the wrong
     * sibling. */
    {
        uintptr_t anchor = ep122_sym(f->scope), calls[64], ordered[32];
        int ncall, nord = 0, i, j;

        if (!anchor) {
            MDBG("resolve: %s has no enclosing function\n",
                 ep122_sym_name(f->sym));
            return;
        }
        ncall = bl_targets(anchor, f->span, calls,
                           (int)(sizeof(calls) / sizeof(calls[0])));
        for (i = 0; i < ncall && nord < (int)(sizeof(ordered) / sizeof(ordered[0])); i++) {
            int match = 0, dup = 0;

            for (j = 0; j < n; j++)
                if (hits[j] == calls[i]) { match = 1; break; }
            if (!match)
                continue;
            for (j = 0; j < nord; j++)
                if (ordered[j] == calls[i]) { dup = 1; break; }
            if (!dup)
                ordered[nord++] = calls[i];
        }
        if (nord != f->total) {
            MDBG("resolve: %s found %d scoped matches, reference build had %d "
                 "-- refusing to guess\n",
                 ep122_sym_name(f->sym), nord, f->total);
            return;
        }
        if (f->nth < nord && !g_ep122_sym[f->sym])
            g_ep122_sym[f->sym] = ordered[f->nth];
    }
}

static int ti_collect(uintptr_t at, int which, void *user)
{
    struct vt_work *w = &g_vtw[g_owner[which]];
    uintptr_t ti = at - 8;

    (void)user;
    /* See ti_from_name_ref: once the flavours are known, insist on one. */
    if (g_ti_si ? !ti_kind(ti) : !ti_looks_real(ti))
        return 0;
    if (w->nti && w->ti == ti)
        return 0;                       /* same record, second pointer to it */
    w->ti = ti;
    w->nti++;
    return 0;
}

static int vt_collect(uintptr_t at, int which, void *user)
{
    struct vt_work *w = &g_vtw[g_owner[which]];
    long top = (long)peek(at - 8);

    (void)user;
    /* Itanium: [offset-to-top][typeinfo][slot0...], and the address point --
     * what a vptr actually holds -- is &slot0. */
    if (top > 0 && top < 0x100000)
        return 0;                       /* not an offset-to-top */
    if (top != w->want_top)
        return 0;
    /* SLOT 0 HOLDS CODE, in the vtable an object really points at. Two other
     * things carry a pointer to a typeinfo at a plausible offset-to-top: a
     * construction vtable, whose destructor slots the ABI leaves null; and an
     * aligned word beside a string or a relocation table, which is not a vtable
     * at all. Both showed up as "N vtables at offset-to-top 0" for classes with
     * a virtual base, i.e. as an unresolved symbol, i.e. as every mod off.
     *
     * Slot 0 ONLY: an interface with no virtual destructor puts its first
     * method there and can be a single entry long, so slot 1 of
     * SoftwareKeyboardPopupWidget::IListener's is the next vtable's
     * offset-to-top. */
    if (!in_text(peek(at + 8)))
        return 0;
    w->vt = at + 8;
    w->nvt++;
    return 0;
}
