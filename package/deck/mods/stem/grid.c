// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem/grid.c - the loaded track's beat grid, reduced to samples per beat.
 *
 * The groove circuit needs one number about the track: how long a beat is. With
 * it, a loop the DJ recorded at 124 BPM plays in time under a 126 BPM track;
 * without it, the loop runs at its own tempo and drifts within a bar.
 *
 * WHERE IT COMES FROM. A cue slot embeds a pcmbuf::PositionWithSourceInfo, and
 * that last field is the page source the position belongs to. The deck's own cue
 * quantiser walks it to reach the grid (sub_10dfac8, called from setAt), and
 * this walks the same chain:
 *
 *     pwsi + 0x28    ->  source     the page source, a meow::RefCountedObjEx
 *     source + 0x68  ->  holder     the grid as this source publishes it
 *     holder + 0x28  ->  appnd_trk_info::BeatGrid::Content
 *     holder + 0x30  ->  origin, added to every beat position
 *     holder + 0x40  ->  the rate those positions are counted at
 *     content + 0x28 ->  the beats, 16 bytes each: int64 position, then that
 *                        beat's BPM as a DOUBLE (see BEAT_STRIDE)
 *     content + 0x30 ->  how many
 *
 * A CUE, RATHER THAN THE BLOCK BEING READ. The audio thread has a position on
 * every block, but read() takes a RANGE -- two bare pcmbuf::Position, `from` and
 * an unbounded `to` -- and a bare Position carries a source ID, not the source.
 * The cue table is where the deck keeps the longer form, and it is already on the
 * path a pad press walks.
 *
 * ONE NUMBER, NOT THE GRID. The first and last beats and the count give the
 * average beat length, which for a fixed-tempo track is exactly its beat length
 * and for a drifting one is the honest average of it. Carrying the whole array
 * would let a loop follow a tempo change, and would also mean copying it,
 * versioning it against the track, and searching it on the audio thread -- for
 * material that is overwhelmingly fixed-tempo. The average is what the deck's
 * own BPM display is derived from, so a DJ can check this against the screen.
 *
 * NOTHING HERE IS TRUSTED. The pointers are the app's, several links deep, and
 * read while it is playing; every step goes through mod_safe_read, both objects
 * are checked against the tag the app writes into them, and the result is
 * refused unless the rate is one the engine uses, the beats are ordered, and the
 * tempo lands in a range a track could actually have. A refused grid means loops
 * play at their own tempo, which is the behaviour without this file at all.
 *
 * Threading. [deck] reads it as a pad is pressed, which is the only moment a cue
 * slot is reachable and the only moment the answer is wanted. [audio] loads one
 * double.
 */
#include "stem/grid_internal.h"
#include "cue/cue.h"
#include "db/db.h"
#include "kit/mod.h"









/* Pool-rate samples per beat, as IEEE bits so the load is one aligned integer.
 * 0 means unknown, which is also 0.0 as a double. [deck] writes, [audio] reads. */
static int64_t grid_g_spb_bits;

/* Where the grid's first downbeat sits on the pool timeline. Read by [deck] as a
 * slot is armed and never by the mix, which sees it only as the engage position
 * gc publishes. */
static int64_t grid_g_beat0;

/* The last grid the deck was told about, in the grid's own units. [message]
 * writes as a reply lands, [deck] reads it as a slot is armed.
 *
 * THE LAST ONE IS THE LOADED ONE. Measured: across four track loads the reply
 * count rose by exactly one each time, and every one of those included selecting
 * a row in the browser first -- selecting a track and scrolling a list produce no
 * reply at all. Only loading does, which is what makes "the last one" a safe
 * answer rather than a guess. (The one case not measured is auditioning with
 * PREVIEW; if that turns out to request a grid, this wants the TrackID match --
 * the id arrives with the reply and only has to be compared, never parsed.)
 *
 * Used ONLY when no cue slot answers. A cue names the loaded track's own source,
 * so it cannot be another track's; this can only be checked by argument. */
/* WHICH TRACK THAT GRID IS FOR, as the reply named it.
 *
 * THE TRACKID IS THE POOL SOURCEID. Measured: the id the audio thread reports
 * for the playing track is 1010200000001:b, and the TrackID bytes arriving with
 * that track's grid are 01000000 02010100 0b000000 00000000 -- the same pair of
 * little-endian words. So the answer to "is this grid the one being played" is a
 * comparison of two numbers we already have, with nothing to parse and nothing to
 * infer from timing.
 *
 * That matters because neither source is self-describing otherwise: a cue slot
 * keeps a source from the track before, and a reply is only re-sent when the deck
 * actually re-reads a grid. Matched, a stale reply is simply not used. */
/* The loaded track's grid holder, remembered wherever one is walked so an edit
 * does not need a cue slot in hand. Defined here because grid_read sets it and
 * the edit code at the bottom of the file consumes it. [deck] */
uintptr_t grid_g_holder;

/* WHICH TRACK THAT HOLDER IS FOR, or 0/0 for "we do not know".
 *
 * An edit only has to move the object in front of it, so the holder alone is
 * enough for one. SAVING one is a different question: it names a track, and a
 * holder reached through a cue slot carries no id -- a slot keeps whatever
 * source was last put in it, so it can be the track before. The reply carries
 * its TrackID, so only a holder that came from a reply can be saved.
 *
 * 0/0 cannot be a real id: registerBeatGrid's own isValid() refuses one whose
 * first byte is zero. [deck] and [message] both write, in the same breath as
 * grid_g_holder. */
uint64_t grid_g_holder_tid_lo, grid_g_holder_tid_hi;



/* Left by [message], taken by [deck]. NULL when there is nothing new. */
static struct grid_beats *grid_g_pending;


struct grid_reply {
    uint64_t tid_lo, tid_hi;    /* 0/0 for an empty slot */
    int64_t  spb_bits;          /* as IEEE bits, like the published one */
    int64_t  beat0;
    int      rate;
    /* AND ITS BEATS, ours, at the grid's own rate. Kept because the flat route
     * is only right for a track whose tempo holds still, and a track coming
     * back deserves the same route it got the first time. A long track is a
     * couple of thousand beats, so sixteen of them is a couple of hundred
     * kilobytes at the very worst. */
    struct grid_beats *beats;
};

static struct grid_reply grid_g_replies[GRID_REPLY_SLOTS];
static int grid_g_reply_next;

/* A copy of one, so an array can be in a slot AND on the mix's timeline without
 * either owning the other -- the published one is converted to the pool rate in
 * place, and the slot has to keep the rate it was replied at. */
static struct grid_beats *grid_beats_dup(const struct grid_beats *b)
{
    struct grid_beats *d;
    size_t n;

    if (!b || b->count <= 0)
        return NULL;
    n = sizeof(*b) + (size_t)b->count * sizeof(b->pos[0]);
    d = malloc(n);
    if (d)
        memcpy(d, b, n);
    return d;
}

/* [message] Keep this track's answer, replacing its own slot if it has one --
 * or a track reloaded sixteen times would push out every other. */
static void grid_reply_keep(uint64_t lo, uint64_t hi, int64_t bits,
                            int64_t beat0, int rate,
                            const struct grid_beats *beats)
{
    struct grid_reply *r;
    int i;

    if (!lo && !hi)
        return;
    for (i = 0; i < GRID_REPLY_SLOTS; i++)
        if (grid_g_replies[i].tid_lo == lo && grid_g_replies[i].tid_hi == hi)
            break;
    if (i == GRID_REPLY_SLOTS) {
        i = grid_g_reply_next;
        grid_g_reply_next = (i + 1) % GRID_REPLY_SLOTS;
    }
    r = &grid_g_replies[i];
    __atomic_store_n(&r->tid_lo, (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&r->tid_hi, (uint64_t)0, __ATOMIC_RELAXED);
    free(r->beats);
    r->beats = grid_beats_dup(beats);
    __atomic_store_n(&r->spb_bits, bits, __ATOMIC_RELAXED);
    __atomic_store_n(&r->beat0, beat0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->rate, rate, __ATOMIC_RELAXED);
    __atomic_store_n(&r->tid_hi, hi, __ATOMIC_RELAXED);
    __atomic_store_n(&r->tid_lo, lo, __ATOMIC_RELEASE);
}

/* Put this track's kept beats where an arm will find them, if nothing fresher
 * is already staged. Called from inside the arm, which is serialised. */
static int grid_reply_stage_beats(uint64_t lo, uint64_t hi)
{
    struct grid_beats *d;
    int i;

    if (__atomic_load_n(&grid_g_pending, __ATOMIC_ACQUIRE))
        return 0;                       /* a live reply's copy is newer */
    for (i = 0; i < GRID_REPLY_SLOTS; i++) {
        struct grid_reply *r = &grid_g_replies[i];

        if (__atomic_load_n(&r->tid_lo, __ATOMIC_ACQUIRE) != lo ||
            __atomic_load_n(&r->tid_hi, __ATOMIC_RELAXED) != hi || !r->beats)
            continue;
        d = grid_beats_dup(r->beats);
        if (!d)
            return 0;
        free(__atomic_exchange_n(&grid_g_pending, d, __ATOMIC_ACQ_REL));
        return 1;
    }
    return 0;
}

/* [any] This track's answer, or 0 if no reply for it has been seen. */
static int grid_reply_find(uint64_t lo, uint64_t hi, struct grid_reply *out)
{
    int i;

    if (!lo && !hi)
        return 0;
    for (i = 0; i < GRID_REPLY_SLOTS; i++) {
        struct grid_reply *r = &grid_g_replies[i];

        if (__atomic_load_n(&r->tid_lo, __ATOMIC_ACQUIRE) != lo ||
            __atomic_load_n(&r->tid_hi, __ATOMIC_RELAXED) != hi)
            continue;
        out->spb_bits = __atomic_load_n(&r->spb_bits, __ATOMIC_RELAXED);
        out->beat0    = __atomic_load_n(&r->beat0, __ATOMIC_RELAXED);
        out->rate     = __atomic_load_n(&r->rate, __ATOMIC_RELAXED);
        /* Still this track's after the read, or the slot was taken under us. */
        if (__atomic_load_n(&r->tid_lo, __ATOMIC_ACQUIRE) != lo ||
            __atomic_load_n(&r->tid_hi, __ATOMIC_RELAXED) != hi)
            continue;
        return 1;
    }
    return 0;
}

/* What the mix reads: the same array converted to POOL samples. [deck] is the
 * only publisher; readers are counted so a publish cannot free one mid-read,
 * exactly as the stem store and the groove circuit do it. */
static struct grid_beats *grid_g_beats;
static int grid_g_beats_readers;
static int grid_g_beats_live;

/* The pool rate the live array was converted for. A rate change makes the
 * positions wrong rather than stale, and the raw ones are gone by then, so the
 * array is dropped instead -- a track change comes with a new reply, so this
 * heals on the next load rather than staying broken. */
static int grid_g_beats_rate;

int stem_grid_beats_acquire(struct stem_grid_view *out)
{
    __atomic_fetch_add(&grid_g_beats_readers, 1, __ATOMIC_ACQ_REL);
    if (!__atomic_load_n(&grid_g_beats_live, __ATOMIC_ACQUIRE) ||
        !grid_g_beats || grid_g_beats->count < 2) {
        __atomic_fetch_sub(&grid_g_beats_readers, 1, __ATOMIC_ACQ_REL);
        return 0;
    }
    out->beats = grid_g_beats->pos;
    out->count = grid_g_beats->count;
    return 1;
}

void stem_grid_beats_release(void)
{
    __atomic_fetch_sub(&grid_g_beats_readers, 1, __ATOMIC_ACQ_REL);
}

double stem_grid_spb(void)
{
    int64_t bits = __atomic_load_n(&grid_g_spb_bits, __ATOMIC_RELAXED);
    double spb;

    memcpy(&spb, &bits, sizeof(spb));
    return spb;
}

int64_t stem_grid_beat0(void)
{
    return __atomic_load_n(&grid_g_beat0, __ATOMIC_RELAXED);
}

static void grid_publish(double spb, int64_t beat0)
{
    int64_t bits;

    memcpy(&bits, &spb, sizeof(bits));
    __atomic_store_n(&grid_g_beat0, beat0, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_spb_bits, bits, __ATOMIC_RELAXED);
}

/* Take the array out of the mix's reach. Not freed: freeing waits for readers,
 * and the next publish frees it anyway -- this only has to be instant and to
 * leave nothing of the wrong track's behind. [any] */
static void grid_beats_drop(void)
{
    __atomic_store_n(&grid_g_beats_live, 0, __ATOMIC_RELEASE);
}

/* Which track the published grid is for, so a second ask is two atomic loads. */
static uint64_t grid_g_armed_lo, grid_g_armed_hi;
static int      grid_g_armed_rate;
static int      grid_g_arming;

void stem_grid_forget(void)
{
    /* Only the published answer, and the array that goes with it. The reply
     * cache keeps its TrackID, so a track change cannot make it wrong -- it
     * either matches the new track or is not used. Dropping that here would
     * throw away the grid for the track being loaded, which arrives BEFORE the
     * change is seen.
     *
     * AND THE LATCH, or nothing ever puts the grid back. The reply for the new
     * track lands BEFORE the change is noticed, so the ordering is: arm the new
     * grid, then forget it -- and grid_arm_reply answers "already armed, nothing
     * to do" from then on, for that track, for as long as it is loaded. Which is
     * a roll that plays one hit per touch and never repeats, on every track
     * after the first. */
    grid_beats_drop();
    grid_publish(0.0, 0);
    __atomic_store_n(&grid_g_armed_lo, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_armed_hi, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_armed_rate, 0, __ATOMIC_RELAXED);
}


/* One holder, in POOL samples: what the mix and the engage position are counted
 * in. Grid positions are counted at the grid's own rate; they are the same
 * number today and the scale is what keeps this true when they are not. */
/* ---- copying the beats out ------------------------------------------------ */

/* Every beat of `holder`, in the grid's own units with the origin already added.
 * NULL when there is nothing to copy, which leaves the caller on the average.
 *
 * Read in chunks rather than a beat at a time: mod_safe_read is a pread on
 * /proc/self/mem, so a beat at a time is a syscall a beat -- a couple of
 * thousand of them while the DJ is pressing a pad. A chunk is one syscall for
 * five hundred.
 *
 * ASCENDING OR NOTHING. stem_beat_at bisects this, and a bisection over an
 * unordered array does not fail, it answers wrongly -- which here means a loop
 * quietly in the wrong place. Equal neighbours are allowed through (a beat of no
 * duration is something a hand-edited grid can contain, and the search handles
 * it); going backwards is refused outright, and said out loud, because nothing
 * we have seen produces one. */
/* Make `b` the array the mix reads, converted to pool samples. Takes ownership
 * of it and of whatever it replaces. [deck] */
static void grid_publish_beats(struct grid_beats *b, int pool_rate)
{
    struct grid_beats *old = grid_g_beats;
    int32_t i;

    for (i = 0; i < b->count; i++)
        b->pos[i] = grid_to_pool(b->pos[i], pool_rate, b->rate);
    b->rate = pool_rate;

    __atomic_store_n(&grid_g_beats_live, 0, __ATOMIC_RELEASE);
    while (__atomic_load_n(&grid_g_beats_readers, __ATOMIC_ACQUIRE) > 0)
        usleep(1000);
    grid_g_beats      = b;
    grid_g_beats_rate = pool_rate;
    free(old);
    __atomic_store_n(&grid_g_beats_live, 1, __ATOMIC_RELEASE);
}

/* Point the mix's array at the loaded track, if it is not there already.
 *
 * `holder` is the one the cue walk reached, or 0 when the answer came from a
 * reply -- in which case the array was copied when that reply landed and is
 * waiting in `grid_g_pending`. Either way the TrackID decides: an array that is
 * not this track's is freed rather than used, because beats from the track
 * before are exactly the failure the average does not have. [deck] */
static void grid_beats_for(uint64_t lo, uint64_t hi, uintptr_t holder,
                           int pool_rate)
{
    struct grid_beats *b;

    if (grid_g_beats && __atomic_load_n(&grid_g_beats_live, __ATOMIC_ACQUIRE) &&
        grid_g_beats_rate == pool_rate &&
        grid_g_beats->tid_lo == lo && grid_g_beats->tid_hi == hi)
        return;

    b = __atomic_exchange_n(&grid_g_pending, (struct grid_beats *)NULL,
                            __ATOMIC_ACQ_REL);
    if (b && (b->tid_lo != lo || b->tid_hi != hi)) {
        free(b);
        b = NULL;
    }
    if (!b && holder) {
        b = grid_copy_beats(holder);
        if (b) {
            b->tid_lo = lo;
            b->tid_hi = hi;
        }
    }
    if (!b) {
        /* Nothing for this track. The average still stands -- that is the
         * fixed-tempo route, and it is what every track had before this. */
        __atomic_store_n(&grid_g_beats_live, 0, __ATOMIC_RELEASE);
        return;
    }
    grid_publish_beats(b, pool_rate);
    MDBG("grid: %d beats on the mix's timeline -> the loop follows the tempo\n",
         (int)grid_g_beats->count);
}

/* Walk one PositionWithSourceInfo to its grid holder, then read it. */
static double grid_read(uintptr_t pwsi, int pool_rate, int64_t *beat0,
                        uintptr_t *holder_out, const char **why)
{
    uintptr_t src = 0, holder = 0;
    int32_t sig = 0;

    /* NO TAG TEST on the position itself. Its constant differs between a bare
     * Position and this longer form, and a constant read off one build is a
     * thing to get wrong quietly; the source's own signature below is the check
     * the app makes, and an empty slot cannot pass it. */
    *why = "no cue to read";
    if (!pwsi ||
        mod_safe_read(pwsi + PWSI_SOURCE_OFF, &src, sizeof(src)) != 0 || !src)
        return 0.0;

    *why = "not a source";
    if (mod_safe_read(src + OBJ_SIG_OFF, &sig, sizeof(sig)) != 0 || sig != OBJ_SIG)
        return 0.0;

    grid_probe_sid(src);

    *why = "no grid holder";
    if (mod_safe_read(src + SRC_HOLDER_OFF, &holder, sizeof(holder)) != 0 ||
        !holder)
        return 0.0;

    *holder_out = holder;
    grid_g_holder = holder;
    /* A cue slot does not say whose source it holds, so this holder can be
     * edited but not saved -- see grid_g_holder_tid_lo. */
    grid_g_holder_tid_lo = grid_g_holder_tid_hi = 0;
    return grid_scale(holder, pool_rate, beat0, why);
}


typedef void (*grid_reply_fn_t)(void *self, void *req, void *tid, void *grid,
                                void *res);
static uintptr_t grid_g_orig_reply;


static void grid_wrap_reply(void *self, void *req, void *tid, void *grid,
                            void *res)
{
    char id[GRID_TID_BYTES * 2 + 1];
    uintptr_t deref = 0;
    const char *why = NULL;
    uint64_t tlo = 0, thi = 0;
    int64_t beat0 = 0;
    double spb = 0.0;
    int rate = 0, ok, hop = 0;

    grid_hex((uintptr_t)tid, GRID_TID_BYTES, id);
    if (mod_safe_read((uintptr_t)tid, &tlo, sizeof(tlo)) != 0 ||
        mod_safe_read((uintptr_t)tid + 8, &thi, sizeof(thi)) != 0)
        tlo = thi = 0;

    /* Straight first, then one hop. Whichever reads is the answer. */
    ok = grid_from_holder((uintptr_t)grid, &spb, &beat0, &rate, &why);
    if (!ok && mod_safe_read((uintptr_t)grid, &deref, sizeof(deref)) == 0 &&
        deref) {
        ok = grid_from_holder(deref, &spb, &beat0, &rate, &why);
        hop = ok;
    }

    if (ok) {
        struct grid_beats *b;
        int64_t bits;

        memcpy(&bits, &spb, sizeof(bits));

        /* HERE, because this is the moment the object is certainly alive -- the
         * deck is handing it over. Left for the next pad press to collect; if
         * none comes before the next reply, the array we displace is ours to
         * free, which is what makes the exchange the whole of the handoff. */
        grid_g_holder = hop ? deref : (uintptr_t)grid;
        grid_g_holder_tid_lo = tlo;
        grid_g_holder_tid_hi = thi;
        b = grid_copy_beats(grid_g_holder);
        if (b) {
            b->tid_lo = tlo;
            b->tid_hi = thi;
        }
        /* KEPT BEFORE IT IS STAGED, because staging gives it away: the arm
         * converts the staged array to the pool rate in place, and the slot has
         * to hold the rate the reply came at. */
        grid_reply_keep(tlo, thi, bits, beat0, rate, b);
        if (b)
            free(__atomic_exchange_n(&grid_g_pending, b, __ATOMIC_ACQ_REL));

        /* The beat count is on this line rather than its own because a copy
         * that failed is not an event, it is a track that will phase off the
         * average -- and the only place that shows is next to the tempo it
         * would otherwise have followed. */
        MDBG("grid: reply track %s -> %.1f BPM, beat0 %lld at %d Hz%s, %d beats"
             " copied\n", id, (double)rate * 60.0 / spb, (long long)beat0, rate,
             hop ? " (via *ptr)" : "", b ? (int)b->count : 0);
    } else {
        MDBG("grid: reply track %s -> no grid (%s)\n", id, why ? why : "?");
    }

    ((grid_reply_fn_t)grid_g_orig_reply)(self, req, tid, grid, res);
}

static int grid_install(void)
{
    if (prestem_native_owner_active()) {
        MDBG("stem_grid: native PRE-STEMS owns track lifecycle -> skipped\n");
        return 0;
    }

    if (mod_patch_vslot("gridReply", EP122_GRID_RECEPTION,
                        GRID_RECEPTION_SLOT, (void *)grid_wrap_reply,
                        &grid_g_orig_reply) != 0) {
        MDBG("grid: no beat-grid reply -> cue slots are the only route\n");
        return -1;
    }
    return 0;
}

KIT_MOD(k_mod_stem_grid,
        .name = "stem_grid", .prio = 61, .install = grid_install,
        .what = "beat grid: watch the deck's own grid replies");

/* The deck's own reply, armed without waiting for anything else.
 *
 * THE REPLY NEEDS NO PAD. It arrives BECAUSE a track loaded and carries its own
 * TrackID, so everything the mix needs is staged the moment it lands. Collecting
 * it only on a pad press left a freshly loaded track with no grid at all --
 * measured: spb 0 and no beats until a hot cue was pressed, and since the
 * X-PAD's clock runs on that grid and the stems row's quantized MUTE rides that
 * clock, every held mute committed on the block it was asked in.
 *
 * 0 when there is no reply for the source the audio thread is reading, which is
 * the case the cue walk in stem_grid_take exists for. NOT [audio]: publishing
 * frees the displaced array once its readers are gone. */
static int grid_arm_reply(int rate, uint64_t tid_lo, uint64_t tid_hi, int have_id)
{
    struct grid_reply r;
    int64_t beat0;
    double  raw, spb;
    int     grate;

    /* A reply is this track's only if the id it came with is the id the audio
     * thread is reading. Anything else is another track's. */
    if (!have_id || !grid_reply_find(tid_lo, tid_hi, &r))
        return 0;

    grate = r.rate;
    memcpy(&raw, &r.spb_bits, sizeof(raw));
    if (!(raw > 0.0) || grate <= 0)
        return 0;

    if (tid_lo == __atomic_load_n(&grid_g_armed_lo, __ATOMIC_RELAXED) &&
        tid_hi == __atomic_load_n(&grid_g_armed_hi, __ATOMIC_RELAXED) &&
        rate   == __atomic_load_n(&grid_g_armed_rate, __ATOMIC_RELAXED))
        return 1;

    /* One arming at a time. The publish takes the beat array out of the mix's
     * reach and frees it once the readers have gone, and two threads doing that
     * to one array is a double free -- the pad press is [deck] and the tick is
     * [message]. The loser answers 1 all the same: the reply is this track's
     * either way, and the winner is publishing exactly what it would have. */
    if (__atomic_exchange_n(&grid_g_arming, 1, __ATOMIC_ACQ_REL))
        return 1;

    spb   = raw * (double)rate / (double)grate;
    beat0 = grid_to_pool(r.beat0, rate, grate);
    MDBG("grid: %.1f BPM from the deck's own reply\n",
         (double)rate * 60.0 / spb);
    /* WITH THE REPLY'S OWN HOLDER, so a second arm can REBUILD the array rather
     * than drop it. grid_beats_for keeps a live array only while the track AND
     * the pool rate both match, and the staged copy is consumed by the first
     * arm -- so an arm that follows the pool rate settling had nothing left to
     * offer and silently took the beats away, leaving the flat route to answer
     * for a track whose grid was right there. */
    /* WITH THIS TRACK'S OWN BEATS. The live reply stages a copy when it lands;
     * a track coming back has no reply behind it, so the kept one is staged
     * here instead and the array route survives a second load. */
    grid_reply_stage_beats(tid_lo, tid_hi);
    grid_beats_for(tid_lo, tid_hi,
                   (grid_g_holder_tid_lo == tid_lo &&
                    grid_g_holder_tid_hi == tid_hi) ? grid_g_holder : 0, rate);
    grid_publish(spb, beat0);
    __atomic_store_n(&grid_g_armed_lo, tid_lo, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_armed_hi, tid_hi, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_armed_rate, rate, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_arming, 0, __ATOMIC_RELEASE);
    return 1;
}

/* Ask for it off the display clock, because the reply and the pool rate do not
 * arrive together: the reply lands when the track loads, the rate once the audio
 * path is running. Asked every frame until it takes, and two atomic loads after.
 * [message] */
void stem_grid_tick(void)
{
    uint64_t lo = 0, hi = 0;
    int rate = stem_pool_rate();
    int have = stem_source_id(&lo, &hi);

    if (rate > 0)
        grid_arm_reply(rate, lo, hi, have);
}

/* The loaded track's grid: the deck's own answer first, cue slots second.
 *
 * THE REPLY WINS BECAUSE A CUE SLOT GOES STALE. A slot keeps whatever source was
 * last put in it, and loading a track does not clear one the new track has no cue
 * for -- so the memory cue slot answered for the PREVIOUS track. Measured: with
 * a 126.0 BPM track loaded, kind 0 returned 120.0 and the anchor 4404, both of
 * them the track before it. A grid from the wrong track is worse than no grid:
 * the loop is confidently in the wrong time, which is what "it does not latch"
 * looks like from the floor.
 *
 * The reply cannot be stale in that way. It arrives BECAUSE this track loaded,
 * carries its own TrackID, and nothing else produces one -- browsing and
 * selecting a row are silent, measured. The cue walk stays as the fallback for
 * the case the reply cannot cover: a shim that started mid-session, where no load
 * has happened since and there is nothing cached to prefer.
 *
 * Called as a pad is pressed. [deck] */
int stem_grid_take(const struct cue_event *ev)
{
    int rate = stem_pool_rate();
    const char *why = "no cue to read";
    /* One character per kind, memory cue first: '-' no slot, '.' a slot that
     * led nowhere, 'g' a slot that gave a grid. Printed on every arm because it
     * is the only view of HOW MANY slots answer, which is what says whether an
     * unset cue still names the source or whether this depends on the DJ having
     * set one. */
    char map[GRID_KIND_MAX + 2];
    uint64_t tid_lo = 0, tid_hi = 0;
    uintptr_t holder = 0;
    int kind, got = -1, have_id;
    int64_t beat0 = 0;
    double spb = 0.0;

    if (rate <= 0)
        return 0;

    have_id = stem_source_id(&tid_lo, &tid_hi);

    /* Once, before the shortcut below can skip the walk entirely: does a source
     * say which track it is? That is what would let the walk check its own
     * answer rather than trust it. */
    {
        static int probed;
        int k;

        for (k = 0; !probed && k <= GRID_KIND_MAX; k++) {
            uintptr_t slot = cue_slot(ev, k), s = 0;
            int32_t sg = 0;

            if (!slot ||
                mod_safe_read(slot + SLOT_PWSI_OFF + PWSI_SOURCE_OFF, &s,
                              sizeof(s)) != 0 || !s ||
                mod_safe_read(s + OBJ_SIG_OFF, &sg, sizeof(sg)) != 0 ||
                sg != OBJ_SIG)
                continue;
            probed = 1;
            grid_probe_sid(s);
        }
    }

    /* The deck's own answer for the track it loaded, before anything that can be
     * left over from the one before it. Usually already done by the tick. */
    if (grid_arm_reply(rate, tid_lo, tid_hi, have_id))
        return 1;

    for (kind = 0; kind <= GRID_KIND_MAX; kind++) {
        uintptr_t slot = cue_slot(ev, kind), h = 0;
        int64_t b = 0;
        double s;

        if (!slot) {
            map[kind] = '-';
            continue;
        }
        s = grid_read(slot + SLOT_PWSI_OFF, rate, &b, &h, &why);
        map[kind] = s > 0.0 ? 'g' : '.';
        if (s > 0.0 && got < 0) {
            got = kind;
            spb = s;
            beat0 = b;
            holder = h;
        }
    }
    map[GRID_KIND_MAX + 1] = '\0';

    if (got < 0) {
        MDBG("grid: %s [%s] -> loops play at their own tempo\n", why, map);
        grid_beats_drop();
        grid_publish(0.0, 0);
        return 0;
    }
    MDBG("grid: %.1f BPM from cue kind %d [%s]\n",
         (double)rate * 60.0 / spb, got, map);

    /* The array only when the id says which track it is for. Without one there
     * is nothing to stamp it with, and an array that cannot be checked against
     * the next track is the stale-cue-slot failure again with more beats in it. */
    if (have_id)
        grid_beats_for(tid_lo, tid_hi, holder, rate);
    else
        grid_beats_drop();
    grid_publish(spb, beat0);
    return 1;
}

/* ---- editing the grid -----------------------------------------------------
 *
 * The four controls the CDJ-3000X has in its grid panel and the 3000 does not --
 * [x2], [x1/2], [Enlarge], [Reduce] -- are all one operation: rescale the beat
 * interval. Every mode the deck's own gridAdjustChengeReq offers moves the int16
 * OFFSET and nothing else, so there is no firmware path to borrow; the grid has
 * to be replaced.
 *
 * ---- why replacing the array is safe --------------------------------------
 *
 * Probed on the live deck, the array the deck loaded looks like this:
 *
 *     [ chunk size 0x3465 ][ 16 x 0xAF ][ N cells ][ 16 x 0xEF ]
 *                           ^ beats-16   ^ beats
 *
 * The 0xAF/0xEF runs are meow::overrun_helper::Checker's red zones -- the
 * "signiture" it asserts on -- and the word at beats-24 is a glibc chunk header
 * (0x3460 with PREV_INUSE|NON_MAIN_ARENA), sized exactly for 16 + N*16 + 16. So
 * the array is ORDINARY HEAP from operator new, not one of this binary's pool
 * allocators, and `operator delete` on it will be a plain free of beats-16.
 *
 * The shim is in the same process and on the same heap, so a replacement built
 * with malloc and handed over as ours+16, with both red zones written, is
 * indistinguishable from the deck's own. That is the whole safety argument, and
 * it is why [x2] -- which needs MORE cells than exist and so cannot be done in
 * place -- is no riskier than the three that can.
 *
 * THE ORIGINAL IS NEVER FREED AND NEVER WRITTEN TO. It is kept so RESET is a
 * restore rather than an inverse calculation; ours leak instead, at ~13 kB an
 * edit, which is the right way round for a control a DJ taps a few times.
 *
 * ---- why the swap is safe under live readers ------------------------------
 *
 * Pointer and count are two words and cannot be exchanged atomically, so the
 * ORDER is chosen to keep every intermediate state in bounds:
 *
 *   growing   pointer first, then count -- a reader in between sees the NEW,
 *             longer array with the OLD, smaller count
 *   shrinking count first, then pointer -- a reader in between sees the OLD
 *             array with the NEW, smaller count
 *
 * Either way no reader indexes past the end of whichever array it is holding.
 * The two arrays are swapped one after the other, so a reader can briefly pair
 * new beats with old bars -- which is a bar number one frame stale, not a read
 * out of bounds, because each pair is made consistent before the next.
 */



/* What the deck loaded, so RESET is a restore. Both arrays: a reset that put
 * back the beats and left our bars would leave the grid describing itself in two
 * different time signatures. [deck] */
uintptr_t grid_g_orig_content;
uintptr_t grid_g_orig_beats;
int32_t   grid_g_orig_count;
uintptr_t grid_g_orig_bars;
int32_t   grid_g_orig_barcnt;

/* The first beat's own BPM field, which is the number the deck puts on the play
 * screen -- so the panel's readout and the deck's readout cannot disagree, and a
 * rescale moves both because it writes that field.
 *
 * Read from the holder rather than derived from the published interval: the
 * published one is in POOL-RATE samples, and the pool rate is the stem engine's.
 * A deck with STEMS off has no pool rate, so the derived route answered 0 on
 * exactly the deck this readout is for. */
static uintptr_t grid_live_content(void)
{
    uintptr_t content = 0;

    if (!grid_g_holder ||
        mod_safe_read(grid_g_holder + HOLDER_CONTENT_OFF, &content,
                      sizeof(content)) != 0)
        return 0;
    return content;
}

uintptr_t stem_grid_id(void)
{
    return grid_live_content();
}

double stem_grid_bpm(void)
{
    uintptr_t content = grid_live_content(), beats = 0;
    int32_t count = 0;

    if (!content ||
        mod_safe_read(content + CONTENT_BEATS_OFF, &beats, sizeof(beats)) != 0 ||
        !beats ||
        mod_safe_read(content + CONTENT_COUNT_OFF, &count, sizeof(count)) != 0 ||
        count < 1)
        return 0.0;
    return grid_beat_bpm(beats, 0);
}

/* The tempo before anything the panel did. Every edit is computed against this
 * and REPLACES the last one rather than compounding, so a caller that wants two
 * presses of a fine adjust to add up has to state the total -- and it cannot do
 * that from the edited grid, which is what it is trying to move away from. */
/* The offset the deck's own grid modes move -- every one of the six moves this
 * and nothing else, see [ep122_grid_adjust_surface] -- so non-zero means its
 * RESET has something to undo. 0 when there is no grid, which is also "nothing
 * to undo" and therefore the right answer either way. */
int64_t stem_grid_offset(void)
{
    int64_t origin = 0;

    if (!grid_g_holder ||
        mod_safe_read(grid_g_holder + HOLDER_ORIGIN_OFF, &origin,
                      sizeof(origin)) != 0)
        return 0;
    return origin;
}

double stem_grid_orig_bpm(void)
{
    uintptr_t content = grid_live_content();

    if (content && content == grid_g_orig_content && grid_g_orig_beats)
        return grid_beat_bpm(grid_g_orig_beats, 0);
    return stem_grid_bpm();
}

/* The original grid's position at fractional beat index `x`, interpolated.
 * Outside the array the end intervals extend, so a rescale that needs beats
 * past the last one still gets sensible positions. */
/* Point one (pointer, count) pair at a new array, in the order that keeps every
 * intermediate state in bounds. Both of a Content's arrays are swapped through
 * here; only the offsets differ. */
/* The bar array for a grid of `n_new` beats whose original first downbeat was
 * `bar0`, rescaled by `k`.
 *
 * FOUR BEATS TO A BAR, PHASED ON THE ORIGINAL DOWNBEAT. Scaling the old bar
 * indices by k on their own would keep the number of bars, which after a x2
 * means bars of eight beats -- and a x2 is the DJ saying the track is twice as
 * fast, which is twice as many bars, not longer ones. So the phase is carried
 * and the spacing is rebuilt: the beat that was a downbeat still is one, and a
 * bar is still four beats.
 *
 * The phase is taken mod 4 because the deck's own reader assumes the first
 * downbeat lands inside the first bar -- for anything before it, it answers
 * i - bars[0] + 4, which goes negative if bars[0] is 4 or more.
 *
 * Returns the array (as the pointer the Content wants, guards already written)
 * or 0, and leaves the count in *n_out. */
/* ---- making an edit stick -------------------------------------------------
 *
 * Everything above moves the Content object the deck cached for this track, and
 * that object is as long-lived as the cache entry -- so the edit is gone at the
 * next eject, and a media re-read can undo it sooner.
 *
 * The deck's own way to keep a grid is to REGISTER it, and that is the only
 * acceptable way here: no file is opened, no format is reimplemented, and the
 * write lands where the deck's own writes land. The chain, client end first:
 *
 *   TrackInfoRepositoryCache::registerBeatGrid    slot 0x50, the CACHE is
 *     |                                           updated on the way past
 *     -> CacheableBeatGrid::registerBeatGrid
 *        -> BeatGridBodyRegisterCommand::doExecute
 *           -> TrackInfoRepositoryRequestFacade, as IBeatGridRegistrar
 *              -> Quantize_RegisterTicket    CMD_SAV_SPECIFIED_ATOM_INFO
 *                 -> [DB][SRV] "update specified atom"
 *
 * The last step writes the Quantize atom of the track's own analysis file, the
 * ANLZ under PIONEER/USBANLZ -- which is where a beat grid lives on a rekordbox
 * stick, and is a different file from the library the browser reads.
 *
 * EVERY ARGUMENT WAS READ OFF A WORKING CALL, not inferred. The deck's own GRID
 * ADJUST registers an OFFSET through slot 0x58, whose argument list differs only
 * in carrying an int16 where this carries a grid, and
 * trackinfo_stocker::BeatGridOffsetRegistHandler::doRequest is where it is
 * assembled -- register by register, including the shape of the listener
 * reference and the priority byte.
 *
 * AND THE BROWSER'S NUMBER, which is not this. The tempo beside the title in the
 * list is DJDBCONTENT.BPM in the media library -- a different back end, so
 * registering a grid does not move it and mod_djdb_set_bpm does. Both happen
 * here because both are what "the track is now at this tempo" means, and
 * because this is where the TrackID that names the row is known.
 */





/* meow::MappedObjPtr: the cached pointer and the id to fill it from. link()
 * leaves a non-null pointer alone, so this is kept between calls exactly as the
 * deck keeps its own. */

/* meow::ListenerReference, a tagged union. Tag 2 is a shared listener with its
 * interface at +8 and its owner at +0x10; tag 0 is EMPTY, and the app's own copy
 * helper returns without reading anything else. Nothing here wants a callback,
 * so empty it is: the outcome is read from the deck's log and from the file. */

