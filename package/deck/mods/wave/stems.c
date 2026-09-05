// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * wave_stems.c - the reply hooks, the worker, and the feature's lifecycle.
 *
 * The design, the thread rules and the shared state all live in wave.h.
 */
#include "wave/wave.h"
#include "kit/mod.h"

/* trackinfo_stocker::DetailedWaveformRequestHandler<DetailedWaveform_3Band>
 *   ::Reception, vtable 0x1ea0448 slot 5 -> replyDetailedWaveformRequest_3Band.
 * The listener is held as an interface pointer, so unlike the data provider's
 * own accessor this call is a real indirect one and can be hooked. */
/* Each Reception implements the listener interface for ALL THREE styles and
 * does real work only in the one matching its own template argument, so the
 * useful slot is a different one per style:
 *
 *   slots 0,1  destructors
 *   slots 2-4  onEmptyDetailedWaveformCreated_{3Band,RGB,Blue}
 *   slots 5-7  replyDetailedWaveformRequest_{3Band,RGB,Blue}
 *
 * Only the style the DJ has selected is ever requested, so hooking all three
 * costs nothing and whichever fires is the active one. The classes come from
 * the resolver; only the offsets are stated here, because the offset is what
 * distinguishes the three replies from one another. */
#define WS_OFF_REPLY_3BAND 0x28
#define WS_OFF_REPLY_RGB   0x30
#define WS_OFF_REPLY_BLUE  0x38


typedef void *(*reply_fn)(void *self, void *req_id, void *track_id,
                          void *shareptr, void *result);
static uintptr_t g_orig_reply;

/* ---- state shared between the reply hook and the worker -------------------
 *
 * The hook runs on whatever thread the repository replies on and does nothing
 * but publish a pointer; everything expensive happens on our own thread. */
volatile uintptr_t wave_g_obj_style[3];
const char *const wave_k_style_name[3] = { "3band", "rgb", "blue" };
const int wave_k_stride[3] = { 6, 2, 1 };          /* 3band, rgb, blue */
struct style_state wave_g_st[3];
float   *wave_g_power;
float  (*wave_g_ratio)[MOD_WAVE_BANDS];
float   *wave_g_ratio_broad;
uint32_t wave_g_ncols;
float    wave_g_applied[N_STEMS];
int      wave_g_have_analysis;
uint32_t wave_g_tail_lo, wave_g_tail_hi;
int      wave_g_tail_dirty;

static int g_described[3];

static volatile int g_track_ready;   /* a stem set became playable */
volatile int wave_g_track_gone;      /* teardown: forget everything */



uintptr_t wave_deref(uintptr_t at)
{
    uintptr_t v = 0;

    if (mod_safe_read(at, &v, sizeof(v)) != 0)
        return 0;
    return v;
}

/* One line per style, the first time it is seen: enough of the Content and the
 * head of the array to read the WavePoint stride off the log rather than guess
 * it. The 3-band stride was recovered exactly this way. */
static void describe(int style, uintptr_t obj)
{
    uintptr_t content = wave_deref(obj + OBJ_CONTENT_OFF);
    uintptr_t columns = content ? wave_deref(content + CONTENT_COLUMNS_OFF) : 0;
    uint32_t count = 0;
    uint8_t head[48];
    char line[3 * sizeof(head) + 1];
    int i, off = 0;

    if (!columns ||
        mod_safe_read(content + CONTENT_COUNT_OFF, &count, sizeof(count)) != 0)
        return;
    if (mod_safe_read(columns, head, sizeof(head)) != 0)
        return;
    for (i = 0; i < (int)sizeof(head); i++)
        off += sprintf(line + off, "%02x ", head[i]);
    line[off] = '\0';
    MDBG("wave_stems: style=%s content=%p columns=%p count=%u\n",
         wave_k_style_name[style], (void *)content, (void *)columns, count);
    MDBG("wave_stems:   head %s\n", line);
}

struct wave_tid_slot wave_g_tid[3];

/* Publish the id alongside the object, so the worker sees a pair that belongs
 * together. Reply thread. */
static void publish_tid(int style, const struct wave_trackid *id)
{
    struct wave_tid_slot *s = &wave_g_tid[style];
    uint32_t gen = s->gen;

    __atomic_store_n(&s->gen, gen + 1, __ATOMIC_RELAXED);      /* -> odd */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->id = *id;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&s->gen, gen + 2, __ATOMIC_RELAXED);      /* -> even */
}

/* Worker side of the same. 0 when a write was in progress both times, which is
 * "ask again next tick", never "it changed". */
int wave_latched_tid(int style, struct wave_trackid *out)
{
    struct wave_tid_slot *s = &wave_g_tid[style];
    int spins;

    for (spins = 0; spins < 4; spins++) {
        uint32_t before = __atomic_load_n(&s->gen, __ATOMIC_RELAXED);
        uint32_t after;

        if (before & 1u)
            continue;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        *out = s->id;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        after = __atomic_load_n(&s->gen, __ATOMIC_RELAXED);
        if (before == after)
            return 1;
    }
    return 0;
}

/* Which object each track's reply named. See wave.h for why this has to exist.
 *
 * Small and fixed, on the same reasoning as the sid->path binding in decode.c: a
 * set touches a handful of tracks and the oldest entry is the right one to lose.
 * One seqlock for the whole table rather than per entry -- the reply thread is
 * the only writer, and a torn read would pair a track with another's object. */
#define OBJ_BINDS 8

static struct {
    volatile uint32_t gen;
    struct {
        struct wave_trackid id;
        uintptr_t obj;
        int       used;
    } e[OBJ_BINDS];
    int next;
} g_obj_bind[3];

/* Reply thread. Cheap by construction: a few compares and stores, no syscall and
 * nothing written to stderr -- see the note in latch() about what this thread
 * cannot afford. */
static void remember_obj(int style, const struct wave_trackid *id, uintptr_t obj)
{
    typeof(g_obj_bind[0]) *t = &g_obj_bind[style];
    uint32_t gen = t->gen;
    int i, at = -1;

    for (i = 0; i < OBJ_BINDS; i++) {
        if (t->e[i].used && t->e[i].id.lo == id->lo && t->e[i].id.hi == id->hi) {
            if (t->e[i].obj == obj)
                return;                  /* already what we would write */
            at = i;
            break;
        }
    }
    if (at < 0) {
        at = t->next;
        t->next = (t->next + 1) % OBJ_BINDS;
    }

    __atomic_store_n(&t->gen, gen + 1, __ATOMIC_RELAXED);      /* -> odd */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->e[at].id = *id;
    t->e[at].obj = obj;
    t->e[at].used = 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&t->gen, gen + 2, __ATOMIC_RELAXED);      /* -> even */
}

uintptr_t wave_obj_for_tid(int style, uint64_t lo, uint64_t hi)
{
    typeof(g_obj_bind[0]) *t = &g_obj_bind[style];
    int spins;

    for (spins = 0; spins < 4; spins++) {
        uint32_t before = __atomic_load_n(&t->gen, __ATOMIC_RELAXED);
        uint32_t after;
        uintptr_t obj = 0;
        int i;

        if (before & 1u)
            continue;                    /* writer mid-update */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (i = 0; i < OBJ_BINDS; i++) {
            if (t->e[i].used && t->e[i].id.lo == lo && t->e[i].id.hi == hi) {
                obj = t->e[i].obj;
                break;
            }
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        after = __atomic_load_n(&t->gen, __ATOMIC_RELAXED);
        if (before == after)
            return obj;
    }
    return 0;
}

/* Every reply is latched, whatever track it is about.
 *
 * Rejecting the mismatched ones here looks obvious and is wrong: a new track's
 * replies arrive while the PREVIOUS one is still playing, so "does this match
 * what is playing" turns away precisely the replies the next track needs. The
 * deck then never replies again, the worker keeps re-binding to the old track's
 * object, finds it stale by id, and re-captures forever -- measured, a tight
 * loop of pristine copies of a track that had already been replaced.
 *
 * So the id travels WITH the object and the worker decides: it captures only
 * once the latched id is the playing one, and drops a copy whose id stops being
 * the playing one. Both orderings then work with no window. */
static void latch(int style, void *track_id, void *shareptr)
{
    struct wave_trackid tid = { 0, 0 };
    uintptr_t obj = 0;

    if (shareptr)
        mod_safe_read((uintptr_t)shareptr, &obj, sizeof(obj));
    if (!obj || !track_id ||
        mod_safe_read((uintptr_t)track_id, &tid, sizeof(tid)) != 0)
        return;

    publish_tid(style, &tid);
    wave_g_obj_style[style] = obj;
    remember_obj(style, &tid, obj);
    /* The description is NOT written here. wave.h says this hook publishes a
     * pointer and nothing else, and describe() breaks that: two safe reads, a
     * 48-byte hexdump and two fflush'd writes to stderr, which is a socket to
     * journald. It runs on the repository's reply thread, at the one moment a
     * track load is waiting on that side, and the deck answers a stalled loader
     * with AsyncLoadFunctionHandler timing out. The worker does it instead --
     * same information, off the critical path. */
}

static void *wrap_reply(void *self, void *req_id, void *track_id,
                        void *shareptr, void *result)
{
    latch(WS_STYLE_3BAND, track_id, shareptr);
    return ((reply_fn)g_orig_reply)(self, req_id, track_id, shareptr, result);
}

static uintptr_t g_orig_rgb, g_orig_blue;

static void *wrap_reply_rgb(void *self, void *req_id, void *track_id,
                            void *shareptr, void *result)
{
    latch(WS_STYLE_RGB, track_id, shareptr);
    return ((reply_fn)g_orig_rgb)(self, req_id, track_id, shareptr, result);
}

static void *wrap_reply_blue(void *self, void *req_id, void *track_id,
                             void *shareptr, void *result)
{
    latch(WS_STYLE_BLUE, track_id, shareptr);
    return ((reply_fn)g_orig_blue)(self, req_id, track_id, shareptr, result);
}

/* ---- the worker ----------------------------------------------------------- */

static char g_pending_path[512];

void wave_stems_track_ready(const char *path)
{
    if (path && path[0])
        snprintf(g_pending_path, sizeof(g_pending_path), "%s", path);
    g_track_ready = 1;
}

void wave_stems_track_gone(void)
{
    wave_g_track_gone = 1;
}

static void drop_track(void)
{
    int k;

    /* Deliberately NOT wave_restore(). Teardown means the deck is finished with this
     * track, and restoring would write the saved bytes back into an array it may
     * already have freed -- either a failed write, or worse, a write into memory
     * something else now owns. There is nothing to put right: the waveform is
     * going away with the track. Bypass is the case that needs restoring, and
     * there the array is live by construction. */
    for (k = 0; k < 3; k++)
        wave_invalidate(k);
    free(wave_g_power);
    wave_g_power = NULL;
    free(wave_g_ratio);
    wave_g_ratio = NULL;
    free(wave_g_ratio_broad);
    wave_g_ratio_broad = NULL;
    wave_g_have_analysis = 0;
    wave_g_ncols = 0;
    wave_g_tail_dirty = 0;
    memset(wave_g_applied, 0, sizeof(wave_g_applied));
    /* The latched objects are deliberately NOT cleared. They are the only
     * pointers we are ever handed, and the deck reuses them across tracks --
     * every reply for the next track may already have come and gone by the time
     * this runs. wave_resolve re-reads through them each tick, so a stale one
     * costs nothing and a cleared one stops the feature dead. Forgetting which
     * reply each copy came from, which wave_invalidate does above, is enough. */
}

/* Make the next wave_gains_moved() say yes whatever the faders read.
 *
 * The applied gains are the record of what is currently PAINTED, so any time the
 * array stops matching them -- a restore, a bypass, a fresh analysis -- that
 * record has to be thrown away or the next comparison finds no movement and
 * nothing repaints. The impossible -1 is what forces the first paint through:
 * zeroing alone would be a legitimate fader position. */
static void forget_applied(void)
{
    memset(wave_g_applied, 0, sizeof(wave_g_applied));
    wave_g_applied[0] = -1.0f;
}

static void *worker(void *unused)
{
    int settle = 0, analysis_wait = 0, stems_were_on = 0, pre_was_on = 0;

    (void)unused;

    for (;;) {
        float g[N_STEMS];
        int s;

        usleep(APPLY_MS * 1000);

        if (analysis_wait > 0)
            analysis_wait--;

        if (wave_g_track_gone) {
            wave_g_track_gone = 0;
            g_track_ready = 0;
            drop_track();
            continue;
        }
        /* ENABLE STEMS is a runtime toggle, so BOTH edges have to be acted on
         * and they are not symmetric by accident.
         *
         * Off: the deck's own waveform goes back, because the audio goes back to
         * it too -- the mix is gated per block by this same flag, so a picture
         * still carrying fader positions would be describing a mix nothing is
         * playing.
         *
         * On: the faders are wherever the DJ left them, and the picture has to
         * catch up with them. Restoring alone is what left the waveform at full
         * height with the stems audibly reduced underneath it: the applied gains
         * still matched the faders, so wave_gains_moved saw no movement and
         * nothing ever repainted. */
        if (pre_was_on && !g_prestems_on) { wave_restore(); forget_applied(); }
        pre_was_on = g_prestems_on;
        if (!g_stems_on && !g_prestems_on) {
            if (stems_were_on) {
                stems_were_on = 0;
                wave_restore();
                forget_applied();
            }
            continue;
        }
        if (!stems_were_on) {
            stems_were_on = 1;
            forget_applied();
        }

        /* The pristine copy is the precondition for everything else, and the
         * reply that provides it can land before or after the stems do. */
        for (s = 0; s < 3; s++) {
            if (wave_g_obj_style[s] && !g_described[s]) {
                g_described[s] = 1;
                describe(s, wave_g_obj_style[s]);
            }
            if (wave_stale(s)) {
                MDBG("wave_stems: %s waveform replaced -> recapturing\n",
                     wave_k_style_name[s]);
                wave_invalidate(s);
                /* The analysis belonged to the waveform that just went away, so
                 * it cannot describe this one.
                 *
                 * And nothing is asked for in its place. The waveform and the
                 * stem engine learn about a track change from DIFFERENT events:
                 * this fires when the deck replies with the new track's array,
                 * the stem job fires when the AUDIO SOURCE moves, and a track
                 * loaded with TRACK_NEXT and left paused does the first without
                 * ever doing the second. Re-requesting here runs whatever
                 * g_pending_path still names -- measured: the previous track's
                 * audio analysed into this track's column count, then written
                 * over the previous track's band cache. Only the stem job knows
                 * which track its stems are for, so only the stem job asks. */
                if (s == WS_STYLE_3BAND)
                    wave_g_have_analysis = 0;
            }
            if (!wave_g_st[s].pristine)
                wave_capture_step(s);
        }

        if (g_prestems_on && !g_stems_on) {
            prestem_wave_apply();
            continue;
        }

        if (g_track_ready && !analysis_wait &&
            wave_g_st[WS_STYLE_3BAND].pristine && !wave_g_have_analysis) {
            int r = wave_run_analysis(g_pending_path);

            /* Only a definite answer consumes the request. A transient one --
             * no pool rate observed yet because the track has not been played,
             * or stems being swapped underneath -- comes back here shortly.
             * Consuming those is what left the waveform at full height with the
             * loading bar gone and the stems already playing. */
            if (r != 0)
                g_track_ready = 0;
            analysis_wait = ANALYSIS_RETRY_TICKS;
            if (r > 0)
                forget_applied();               /* force the first wave_apply */
        }

        if (!wave_g_have_analysis)
            continue;

        /* ORIGINAL takes the stems out of circuit, so the waveform goes back to
         * what the deck computed -- the two have to agree. */
        if (stem_bypass_get()) {
            wave_restore();
            forget_applied();
            continue;
        }

        for (s = 0; s < N_STEMS; s++)
            g[s] = stem_gain_get(s);
        if (wave_gains_moved(g)) {
            wave_apply(g);
            settle = 0;
        } else if (wave_g_tail_dirty && ++settle >= SETTLE_TICKS) {
            wave_paint_tail();
            settle = 0;
        }
    }
    return NULL;
}


static int wave_stems_install(void)
{
    pthread_t th;

    mod_wave_codec_init();

    if (mod_patch_vslot("waveReply3Band", EP122_WAVE_RECEPTION,
                        WS_OFF_REPLY_3BAND, (void *)wrap_reply, &g_orig_reply) != 0) {
        MDBG("wave_stems: reply slot unavailable -> not installed\n");
        return -1;
    }
    /* Not fatal: a deck whose DJ never selects these styles is unaffected, and
     * a miss here only costs the description that teaches us their format. */
    mod_patch_vslot("waveReplyRGB", EP122_WAVE_RECEPTION_RGB,
                    WS_OFF_REPLY_RGB, (void *)wrap_reply_rgb, &g_orig_rgb);
    mod_patch_vslot("waveReplyBlue", EP122_WAVE_RECEPTION_BLUE,
                    WS_OFF_REPLY_BLUE, (void *)wrap_reply_blue, &g_orig_blue);
    if (pthread_create(&th, NULL, worker, NULL) != 0) {
        MDBG("wave_stems: worker thread would not start\n");
        return -1;
    }
    pthread_detach(th);
    MDBG("wave_stems: installed (waveform follows the stem faders)\n");
    return 0;
}

KIT_MOD(k_mod_wave_stems,
        .name = "wave_stems", .prio = 80, .install = wave_stems_install,
        .what = "the waveform follows the stem faders");
