// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * wave_paint.c - the pristine copy, and turning gains into pixels.
 *
 * Part of the waveform-follows-the-faders feature. The shared contract, and the
 * reasoning behind the whole design, is in wave.h.
 */
#include "wave/wave.h"

/* ---- the pristine copy ---------------------------------------------------- */

static uint32_t digest(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Where `obj` currently points, or 0 if it will not resolve. Three reads, cheap
 * enough to run on every tick. */
int wave_resolve_obj(uintptr_t obj, struct wave_array *out)
{
    uintptr_t content, columns;
    uint32_t count = 0;

    if (!obj)
        return 0;
    content = wave_deref(obj + OBJ_CONTENT_OFF);
    columns = content ? wave_deref(content + CONTENT_COLUMNS_OFF) : 0;
    if (!columns ||
        mod_safe_read(content + CONTENT_COUNT_OFF, &count, sizeof(count)) != 0 ||
        count == 0 || count > MAX_COLUMNS)
        return 0;
    out->obj = obj;
    out->content = content;
    out->columns = columns;
    out->count = count;
    return 1;
}

/* The object a style is BOUND to, which is not always the last one replied.
 *
 * The deck answers for waveforms it is not showing -- a browse preview comes
 * back through the same Reception as the deck view's own -- so the latch drifts
 * onto another track's array while the DJ scrolls a list. Measured: with Rabbit
 * Hole loaded and playing, the latch moved to High Priestess's 63307-column
 * array and this track's ratios were painted onto it.
 *
 * So a style binds ONCE and stays bound: the reply that first yields a filled
 * array wins, and the binding is only given up when the copy is dropped, which
 * is a real track change (the audio source moving) or the deck refilling the
 * array under us. Nothing here can tell a preview from the deck view, so the
 * length check in wave_analyze.c is what catches a binding that went to the
 * wrong one. */
int wave_resolve(int style, struct wave_array *out)
{
    struct style_state *st = &wave_g_st[style];

    return wave_resolve_obj(st->from_obj ? st->from_obj : wave_g_obj_style[style],
                            out);
}

/* Has the deck put something else in the array since we last wrote to it?
 *
 * See the note in wave.h: this is the only reliable answer, because none of the
 * pointers move between tracks. `written` is what we know the array holds; with
 * nothing written yet the pristine copy is the same claim. */
static int array_diverged(struct style_state *st)
{
    const uint8_t *ref = st->written;
    size_t bytes = (size_t)st->ncols * st->stride;
    uint8_t buf[ARRAY_PROBE_BYTES];
    int i, differed = 0;

    if (!ref) {
        /* Modified with no record of what was written -- wave_write_style could
         * not allocate one. The array is ours and unlike the pristine copy, so
         * comparing against pristine would report a replacement on every tick.
         * The honest answer is that we cannot tell. */
        if (st->modified)
            return 0;
        ref = st->pristine;
    }
    if (!ref || !st->columns || bytes < sizeof(buf))
        return 0;
    for (i = 0; i < ARRAY_PROBES; i++) {
        size_t at = (bytes - sizeof(buf)) / (ARRAY_PROBES - 1) * (size_t)i;

        if (mod_safe_read(st->columns + at, buf, sizeof(buf)) != 0)
            return 1;                    /* unreadable is not ours either */
        if (memcmp(buf, ref + at, sizeof(buf)) != 0 &&
            ++differed >= ARRAY_PROBE_DIFFS)
            return 1;
    }
    return 0;
}

/* Does the copy we hold still describe what the deck is drawing? */
int wave_stale(int style)
{
    struct style_state *st = &wave_g_st[style];
    struct wave_array a;
    uint64_t lo = 0, hi = 0;

    if (!st->pristine)
        return 0;                        /* nothing to be stale */

    /* The reply this copy came from named its track, and now that a block has
     * been read we know which track is playing. A reply taken on trust before
     * the sourceId was known gets checked here, which is the only place it can
     * be: the deck view's waveform and any other reply are indistinguishable
     * until this comparison is possible. */
    if (stem_source_id(&lo, &hi) &&
        (st->from_tid.lo != lo || st->from_tid.hi != hi)) {
        MDBG("wave_stems: %s copy is track %llx:%llx, playing %llx:%llx\n",
             wave_k_style_name[style],
             (unsigned long long)st->from_tid.hi,
             (unsigned long long)st->from_tid.lo,
             (unsigned long long)hi, (unsigned long long)lo);
        return 1;
    }
    if (!wave_resolve(style, &a))
        return 1;
    if (a.obj != st->from_obj || a.content != st->from_content ||
        a.columns != st->columns || a.count != st->ncols)
        return 1;
    return array_diverged(st);
}

/* What we last LEFT in an array, kept across the invalidate a track change
 * causes, for the case where the array could not be handed back.
 *
 * Our painted columns are not blank and nothing is writing them, so they are
 * stable -- and a capture that finds them adopts them as the pristine copy,
 * making the fader positions what "untouched" means.
 *
 * Retained only when our version DIFFERED from the deck's. At unity the two are
 * identical by construction, adopting those bytes is correct, and refusing to
 * would stall the capture on every reload of the same track. */
static struct {
    uint8_t  *bytes;
    uintptr_t columns;
    uint32_t  ncols;
} g_left[3];

static void forget_left(int style)
{
    free(g_left[style].bytes);
    memset(&g_left[style], 0, sizeof(g_left[style]));
}

/* Is this array still exactly what we left in it -- i.e. has the deck not yet
 * put the new track's columns here? */
static int still_our_paint(int style, const struct wave_array *a,
                           const uint8_t *probe, size_t bytes)
{
    if (!g_left[style].bytes || g_left[style].columns != a->columns ||
        g_left[style].ncols != a->count)
        return 0;
    return memcmp(probe, g_left[style].bytes, bytes) == 0;
}

/* Put the deck's own columns back before letting go of the array.
 *
 * Letting go without restoring was deliberate, on the reading that the array
 * goes away with the track. It does NOT: the deck keeps a waveform per track and
 * hands the SAME array back when the DJ returns to it, still holding whatever we
 * last wrote. So leaving a track with a fader down leaves its array as the
 * reduced picture, and the capture on the way back adopts that as pristine --
 * the reduced peak becomes the reference the next paint scales, and every visit
 * compounds it. Reset the faders and the picture stays short, because at unity
 * the paint reproduces pristine exactly and pristine is now the short one.
 *
 * Reading the array back first is what makes the write safe. One that was freed
 * and its memory reused will not still be byte-for-byte what we left, so the
 * restore is skipped -- and mod_safe_write cannot fault whichever way it goes. */
static void restore_if_ours(int style, size_t bytes)
{
    struct style_state *st = &wave_g_st[style];
    uint8_t *live;

    if (!st->modified || !st->columns || !st->written || !st->pristine || !bytes)
        return;
    if (memcmp(st->written, st->pristine, bytes) == 0)
        return;                          /* nothing of ours is in it */
    live = malloc(bytes);
    if (!live)
        return;
    if (mod_safe_read(st->columns, live, bytes) == 0 &&
        memcmp(live, st->written, bytes) == 0) {
        wave_write_style(style, st->pristine);
        st->modified = 0;
        MDBG("wave_stems: %s array handed back as the deck drew it\n",
             wave_k_style_name[style]);
    }
    free(live);
}

void wave_invalidate(int style)
{
    struct style_state *st = &wave_g_st[style];
    size_t bytes = (size_t)st->ncols * st->stride;

    restore_if_ours(style, bytes);
    forget_left(style);
    /* Only reached when the restore could not be made -- the array had already
     * been refilled, or it would not read back. Then the record below is what
     * stops the capture adopting our paint. */
    if (st->written && st->pristine && bytes &&
        memcmp(st->written, st->pristine, bytes) != 0) {
        g_left[style].bytes = st->written;
        g_left[style].columns = st->columns;
        g_left[style].ncols = st->ncols;
        st->written = NULL;              /* handed over rather than freed */
    }
    free(st->pristine);
    free(st->scratch);
    free(st->written);
    free(st->probe);
    memset(st, 0, sizeof(*st));
}

/* Is this array simply not filled in yet?
 *
 * The stability wait cannot answer that on its own: an array the deck has not
 * touched is all zeros, and all zeros are perfectly stable. Adopting one gives a
 * blank pristine copy, and because unity gains round-trip exactly, the first
 * paint then writes that blank over the waveform the deck had meanwhile
 * finished drawing -- a flat line across the display with every fader at full,
 * and nothing to correct it afterwards, since from then on the array holds
 * exactly what we last wrote.
 *
 * Blank is unambiguous rather than a heuristic about level. A 3-band column
 * encodes silence as 0x1008 with zero heights, so even a silent track leaves
 * two non-zero bytes in every column; only an unwritten array is zero
 * throughout. The 1-in-64 floor is far below any real waveform and far above
 * the zero an unfilled one gives. */
static int not_filled_yet(const uint8_t *p, size_t n)
{
    size_t i, nonzero = 0;

    for (i = 0; i < n; i++) {
        if (p[i])
            nonzero++;
    }
    return nonzero * 64 < n;
}

/* Why a capture is not finishing.
 *
 * Three of this function's returns are silent by design -- they are the normal
 * "not yet" -- which is fine until one of them becomes permanent, and then there
 * is nothing in the log at all: no copy, no analysis, no paint, and no reason.
 * Throttled hard because the answer only changes when something else does. */
#define STALL_TICKS 100

static void capture_stalled(int style, const char *why)
{
    static int ticks[3];
    static const char *last[3];
    int first;

    first = last[style] != why;
    if (first) {
        last[style] = why;
        ticks[style] = 0;
    }
    if (ticks[style]++ % STALL_TICKS)
        return;
    /* A reason is worth saying once. Saying it every few seconds for as long as
     * the stall lasts -- which for a waveform nobody asked for is the whole
     * session -- is three lines a tick that bury whatever else happened. */
    if (first)
        MDBG("wave_stems: %s capture waiting: %s\n",
             wave_k_style_name[style], why);
    else
        MTRACE("wave_stems: %s capture waiting: %s\n",
               wave_k_style_name[style], why);
}

/* One poll towards a pristine copy. 1 when the copy is complete.
 *
 * Called once per worker tick while `pristine` is NULL, and it returns straight
 * away on most of them -- see POLL_TICKS in wave.h for why the waiting is done
 * this way round rather than in a loop here. */
int wave_capture_step(int style)
{
    struct style_state *st = &wave_g_st[style];
    struct wave_trackid tid;
    struct wave_array a;
    uint64_t lo = 0, hi = 0;
    size_t bytes;
    uint32_t d;
    int ours;
    /* Which object this capture binds to, and which track it is then a copy OF.
     * Normally the latched reply's; see the fallback below for when it is not. */
    uintptr_t bind_obj = wave_g_obj_style[style];
    struct wave_trackid bound = { 0, 0 };
    int rebound = 0;

    /* Only bind to a reply about the track that is playing. Until the sourceId
     * is known there is nothing to compare against and the latest reply is
     * taken on trust; wave_stale rechecks it the moment there is. */
    if (stem_source_id(&lo, &hi) && wave_latched_tid(style, &tid) &&
        (tid.lo != lo || tid.hi != hi)) {
        /* The latch is on another track. That is not always a wait: a track
         * loaded from an earlier index in the same list is never replied for at
         * all, so waiting here is forever. Fall back to the object this track
         * was replied for the last time it was, which is the one the deck is
         * showing. Still keyed on the id, so this cannot bind to another
         * track's array. */
        bind_obj = wave_obj_for_tid(style, lo, hi);
        if (!bind_obj) {
            capture_stalled(style, "latched reply is about another track, and"
                            " this one has never been replied for");
            return 0;
        }
        bound.lo = lo;
        bound.hi = hi;
        rebound = 1;
    }

    /* A probe stays on the object it started on. Replies keep arriving while it
     * settles -- three seconds is a long time in a browse list -- and following
     * each one restarts the poll and never converges. */
    if (st->probe && !wave_resolve_obj(st->probe_obj, &a)) {
        free(st->probe);
        st->probe = NULL;
    }
    if (!st->probe && !wave_resolve_obj(bind_obj, &a)) {
        capture_stalled(style, "no array to read -- nothing latched, or it will"
                        " not resolve");
        return 0;
    }
    if (st->probe && (a.columns != st->probe_columns || a.count != st->probe_ncols)) {
        free(st->probe);                 /* refilled under us: start over */
        st->probe = NULL;
        capture_stalled(style, "array moved under the probe, starting over");
    }
    bytes = (size_t)a.count * wave_k_stride[style];
    if (!st->probe) {
        st->probe = malloc(bytes);
        if (!st->probe) {
            capture_stalled(style, "no memory for the probe buffer");
            return 0;
        }
        st->probe_obj = a.obj;
        st->probe_columns = a.columns;
        st->probe_ncols = a.count;
        st->probe_blank = 0;
        st->probe_digest = 0;
        st->probe_polls = 0;
        st->probe_stable = 0;
        st->probe_wait = 0;
    }
    if (st->probe_wait > 0) {
        st->probe_wait--;
        return 0;
    }
    st->probe_wait = POLL_TICKS;

    if (mod_safe_read(a.columns, st->probe, bytes) != 0) {
        free(st->probe);
        st->probe = NULL;
        capture_stalled(style, "array will not read back");
        return 0;
    }
    /* Two ways an array can be stable without holding this track: never written,
     * and written by US for the track before. Both are "wait", not "adopt". */
    ours = still_our_paint(style, &a, st->probe, bytes);
    if (ours || not_filled_yet(st->probe, bytes)) {
        /* Stable, but stably not the deck's. probe_polls goes back to zero
         * because there is no earlier digest worth comparing the first real one
         * against. */
        st->probe_stable = 0;
        st->probe_digest = 0;
        st->probe_polls = 0;
        capture_stalled(style, ours ? "array is still our own last paint"
                                    : "array has nothing in it yet");
        if (++st->probe_blank >= CAPTURE_GIVEUP_POLLS) {
            MDBG("wave_stems: %s array still %s after %d polls, restarting\n",
                 wave_k_style_name[style], ours ? "our own last paint" : "blank",
                 st->probe_blank);
            free(st->probe);
            st->probe = NULL;
            /* A deck that reuses an array without rewriting it would otherwise
             * be refused forever. Waiting is what this is for; never capturing
             * is worse than capturing late. */
            if (ours)
                forget_left(style);
        }
        return 0;
    }
    st->probe_blank = 0;

    d = digest(st->probe, bytes);
    st->probe_stable = (st->probe_polls && d == st->probe_digest)
                       ? st->probe_stable + 1 : 0;
    st->probe_digest = d;
    st->probe_polls++;

    if (st->probe_stable < STABLE_POLLS) {
        capture_stalled(style, "array still changing under the probe");
        /* An array that never settles is a real condition, not a wait: say so
         * once and start again, which also re-resolves in case it moved. */
        if (st->probe_polls >= CAPTURE_GIVEUP_POLLS) {
            MDBG("wave_stems: %s array still moving after %d polls, restarting\n",
                 wave_k_style_name[style], st->probe_polls);
            free(st->probe);
            st->probe = NULL;
        }
        return 0;
    }

    free(st->pristine);
    free(st->scratch);
    free(st->written);
    st->written = NULL;      /* a new array: nothing of ours is in it yet */
    forget_left(style);      /* and the deck's own columns are in it now */
    st->pristine = st->probe;
    st->probe = NULL;
    st->scratch = malloc(bytes);
    if (!st->scratch) {
        MDBG("wave_stems: out of memory for %u %s columns\n", a.count,
             wave_k_style_name[style]);
        free(st->pristine);              /* half a copy is worse than none: it
                                          * blocks the retry without painting */
        st->pristine = NULL;
        return 0;
    }
    memcpy(st->scratch, st->pristine, bytes);  /* unpainted spans stay the deck's */
    st->ncols = a.count;
    st->columns = a.columns;
    st->from_obj = a.obj;
    st->from_content = a.content;
    /* Which track this copy is OF. When the object came from the fallback that
     * is the playing track by construction, and recording the latched id
     * instead would make wave_stale condemn the copy on its very next tick. */
    if (rebound)
        st->from_tid = bound;
    else
        wave_latched_tid(style, &st->from_tid);
    st->stride = wave_k_stride[style];
    st->modified = 0;
    MDBG("wave_stems: pristine %s copy, %u columns at %p%s\n",
         wave_k_style_name[style], a.count, (void *)a.columns,
         rebound ? " (bound by track, no reply for it)" : "");
    return 1;
}

/* ---- applying a fader position -------------------------------------------- */

/* Poking the array is the expensive half of a fader move -- three arrays,
 * 655 KB, through /proc/self/mem. Most of it is unnecessary: a fader step of a
 * percent or two leaves the majority of quantised bytes exactly as they were,
 * because the fields are 5 and 3 bits wide. So only the blocks that actually
 * differ from what we last wrote are sent, which turns a large steady cost into
 * one proportional to how much the picture really changed.
 *
 * Block rather than byte granularity: the syscall is the cost, not the bytes. */
#define WRITE_BLOCK 4096

/* Poke one span. Same differential block scheme, bounded to the span so the
 * two-phase wave_paint does not re-scan the whole array twice. */
void wave_write_style_range(int style, uint32_t lo, uint32_t hi)
{
    struct style_state *st = &wave_g_st[style];
    size_t start, end, off;

    if (!st->columns || !st->scratch)
        return;
    if (!st->written) {
        /* No baseline yet, so there is nothing to diff against; scratch is a
         * copy of pristine outside the painted span, so this is still exact. */
        wave_write_style(style, st->scratch);
        return;
    }
    start = (size_t)lo * st->stride;
    end = (size_t)hi * st->stride;
    for (off = start; off < end; off += WRITE_BLOCK) {
        size_t n = end - off < WRITE_BLOCK ? end - off : WRITE_BLOCK;

        if (memcmp(st->written + off, st->scratch + off, n) == 0)
            continue;
        if (mod_safe_write(st->columns + off, st->scratch + off, n) != 0)
            return;
        memcpy(st->written + off, st->scratch + off, n);
    }
}

void wave_write_style(int style, const uint8_t *src)
{
    struct style_state *st = &wave_g_st[style];
    size_t bytes, off;

    if (!st->columns || !st->ncols)
        return;
    bytes = (size_t)st->ncols * st->stride;

    if (!st->written) {
        if (mod_safe_write(st->columns, src, bytes) != 0) {
            MDBG("wave_stems: %s write of %u columns failed\n",
                 wave_k_style_name[style], st->ncols);
            return;
        }
        st->written = malloc(bytes);
        if (st->written)
            memcpy(st->written, src, bytes);
        return;
    }

    for (off = 0; off < bytes; off += WRITE_BLOCK) {
        size_t n = bytes - off < WRITE_BLOCK ? bytes - off : WRITE_BLOCK;

        if (memcmp(st->written + off, src + off, n) == 0)
            continue;
        if (mod_safe_write(st->columns + off, src + off, n) != 0) {
            MWARN("wave_stems: %s write at +%zu failed\n",
                 wave_k_style_name[style], off);
            return;
        }
        memcpy(st->written + off, src + off, n);
    }
}

void wave_restore(void)
{
    int k;

    for (k = 0; k < 3; k++) {
        if (wave_g_st[k].pristine && wave_g_st[k].modified) {
            wave_write_style(k, wave_g_st[k].pristine);
            wave_g_st[k].modified = 0;
        }
    }
}

/* Ratios for one span of columns. */
void wave_ratios_for(const float *g, uint32_t lo, uint32_t hi)
{
    uint32_t i, n = wave_g_ncols;
    int b, st;

    for (i = lo; i < hi; i++) {
        double unity_all = 0.0, moved_all = 0.0;

        for (b = 0; b < MOD_WAVE_BANDS; b++) {
            double unity = 0.0, moved = 0.0;

            for (st = 0; st < N_STEMS; st++) {
                double p = wave_g_power[((size_t)st * MOD_WAVE_BANDS + b) * n + i];

                unity += p;
                moved += (double)g[st] * g[st] * p;
            }
            /* A band with no energy keeps its column verbatim: there is nothing
             * to attribute, and 0/0 would otherwise blank it. */
            wave_g_ratio[i][b] = unity > 0.0 ? (float)sqrt(moved / unity) : 1.0f;
            unity_all += unity;
            moved_all += moved;
        }
        wave_g_ratio_broad[i] = unity_all > 0.0
                         ? (float)sqrt(moved_all / unity_all) : 1.0f;
    }
}

/* Scale one span and poke it, for every style that has been latched. */
void wave_paint(uint32_t lo, uint32_t hi)
{
    int k;

    for (k = 0; k < 3; k++) {
        struct style_state *st = &wave_g_st[k];
        uint32_t a = lo, b = hi;
        size_t off;

        if (!st->pristine || !st->scratch)
            continue;
        /* The analysis and the array can disagree by a column or two at the
         * end; scale what both cover and leave the tail as the deck drew it. */
        if (b > st->ncols)
            b = st->ncols;
        if (a >= b)
            continue;
        off = (size_t)a * st->stride;
        if (k == WS_STYLE_3BAND)
            mod_wave_scale_ratios(st->pristine + off, st->scratch + off,
                                  b - a, wave_g_ratio + a);
        else if (k == WS_STYLE_RGB)
            mod_wave_scale_rgb(st->pristine + off, st->scratch + off, b - a,
                               wave_g_ratio + a, wave_g_ratio_broad + a);
        else
            mod_wave_scale_blue(st->pristine + off, st->scratch + off, b - a,
                                wave_g_ratio_broad + a);
        wave_write_style_range(k, a, b);
        st->modified = 1;
    }
}

/* Columns outside the last painted window, still showing the previous gains. */

/* Paint what the window left behind. Called once the fader has stopped. */
void wave_paint_tail(void)
{
    if (!wave_g_tail_dirty || !wave_g_have_analysis)
        return;
    if (wave_g_tail_lo > 0) {
        wave_ratios_for(wave_g_applied, 0, wave_g_tail_lo);
        wave_paint(0, wave_g_tail_lo);
    }
    if (wave_g_tail_hi < wave_g_ncols) {
        wave_ratios_for(wave_g_applied, wave_g_tail_hi, wave_g_ncols);
        wave_paint(wave_g_tail_hi, wave_g_ncols);
    }
    wave_g_tail_dirty = 0;
    MDBG("wave_stems: tail painted [0..%u) and [%u..%u)\n",
         wave_g_tail_lo, wave_g_tail_hi, wave_g_ncols);
}

void wave_apply(const float *g)
{
    static struct timespec last_report;
    struct timespec t0, t1, t2;
    uint32_t n = wave_g_ncols, centre = 0, w0 = 0, w1 = 0;

    if (!wave_g_have_analysis || !wave_g_ratio || !wave_g_ratio_broad)
        return;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* The column under the needle, from the playhead the audio path already
     * tracks. Absent it (nothing played yet) the whole track is painted in one
     * go. */
    {
        int64_t pos = stem_source_pos();
        int rate = stem_pool_rate();

        if (pos > 0 && rate > 0) {
            int64_t c = pos * COLUMNS_PER_SEC / rate;

            if (c > 0 && c < (int64_t)n)
                centre = (uint32_t)c;
        }
    }

    if (centre) {
        w0 = centre > VISIBLE_BEHIND ? centre - VISIBLE_BEHIND : 0;
        w1 = centre + VISIBLE_AHEAD;
        if (w1 > n)
            w1 = n;
        wave_ratios_for(g, w0, w1);
        wave_paint(w0, w1);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);    /* what the DJ is looking at is done */

    memcpy(wave_g_applied, g, sizeof(wave_g_applied));
    if (!centre) {
        /* No playhead yet, so there is no window to be right about. */
        wave_ratios_for(g, 0, n);
        wave_paint(0, n);
        wave_g_tail_dirty = 0;
    } else {
        /* The remainder is left for wave_paint_tail once the fader stops. */
        wave_g_tail_lo = w0;
        wave_g_tail_hi = w1;
        wave_g_tail_dirty = (w0 > 0 || w1 < n);
    }

    clock_gettime(CLOCK_MONOTONIC, &t2);
    if (t2.tv_sec != last_report.tv_sec) {
        long vis = (t1.tv_sec - t0.tv_sec) * 1000000L +
                   (t1.tv_nsec - t0.tv_nsec) / 1000;
        long all = (t2.tv_sec - t0.tv_sec) * 1000000L +
                   (t2.tv_nsec - t0.tv_nsec) / 1000;

        last_report = t2;
        MDBG("wave_stems: wave_apply visible[%u..%u] %ldus  total %ldus%s\n",
             w0, w1, vis, all, wave_g_tail_dirty ? "  (tail deferred)" : "");
    }
}

int wave_gains_moved(const float *g)
{
    int s;

    for (s = 0; s < N_STEMS; s++) {
        if (fabsf(g[s] - wave_g_applied[s]) > GAIN_EPSILON)
            return 1;
    }
    return 0;
}
