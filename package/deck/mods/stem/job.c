// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * job.c - the stem job state machine.
 *
 * This is the only module that writes state two other threads read, which is
 * what makes the threading contract auditable: if a field crosses a boundary
 * and is not written here, that is the thing to question.
 *
 *   -> the message thread, via `struct stem_ui_state` and a generation counter
 *   -> the audio thread, via g_stem_ready and nothing else
 *
 * The job itself runs on a worker thread of our own. It owns the sidecar socket
 * end to end, so nothing here has to be safe against a second thread poking the
 * fd, and blocking on a PCM write is not merely allowed but wanted -- it is the
 * backpressure that keeps a whole track from being resident anywhere.
 *
 * Lifecycle, one track at a time:
 *
 *   track load (decode.c) -> stem_job_request()
 *     worker: connect -> JOB_BEGIN -> stream PCM -> JOB_END
 *             -> poll PROGRESS -> adopt each STEM_READY -> g_stem_ready = 1
 *   track change          -> stem_track_gone()
 *     message: g_stem_ready = 0; loader frees and re-examines the new track
 */
#include "stem/job_internal.h"
/* The server's separation identity is persisted the moment it changes, and the
 * waveform is told when a set becomes playable. */
#include "core/mod_settings.h"
#include "wave/wave.h"
#include "db/db.h"
#include "xpad/ext.h"
#include "kit/menu.h"
#include "kit/mod.h"
#include "kit/popup.h"

#include <pthread.h>


/* How long the sidecar may say NOTHING before the job is written off. It sends
 * a PROGRESS frame twice a second while a separation runs, so this is 120x the
 * normal cadence -- generous enough for the gap between JOB_END and the server
 * accepting the POST, and far short of the sidecar's own 900 s cap. */
#define JOB_SILENCE_SEC 60

/* One PCM chunk on the wire. Matches the decoder's natural chunk so nothing is
 * re-buffered between fileRead and write. */
#define JOB_PCM_FRAMES      4096

/* The audio thread's entire view of us. See stem.h for the ordering rule. */
volatile int g_stem_ready;

/* ---- the UI snapshot ------------------------------------------------------
 *
 * Seqlock, not a mutex: the message thread must never block, and it is also the
 * only repaint tick we have. An odd generation means a write is in progress, so
 * a reader retries; a torn read costs one repaint and nothing else. */
static struct stem_ui_state g_ui;

/* The shape of the run in flight, carried into every publish. Set where the path is
 * CHOSEN -- a cache hit in loader_serve, the server in sep_request -- because that is
 * the only place it is known for certain, and the row that draws it cannot derive it
 * from a stage. See via_server in stem.h. */
int g_job_via_server;

/* READ-MODIFY-WRITE ON THE GENERATION, because there are two writers: the loader
 * publishes a job's stages and the message thread clears them the moment the
 * track moves. Two writers each doing load-then-store can both write the same
 * odd number and leave it odd for good, which is a reader that never gets an
 * answer again. Interleaved writers still cost a torn read, and a torn read
 * still costs one repaint -- that was always the bargain. */
void ui_publish(int stage, int percent, int queue_position)
{
    __atomic_add_fetch(&g_ui.gen, 1, __ATOMIC_RELAXED);      /* -> odd */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_ui.stage = stage;
    g_ui.percent = percent;
    g_ui.queue_position = queue_position;
    g_ui.via_server = g_job_via_server;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_add_fetch(&g_ui.gen, 1, __ATOMIC_RELAXED);      /* -> even */
}

/* Whether the LAST published status was a usable server, and whether it has just
 * become one with nobody having acted on it yet.
 *
 * THE EDGE IS TAKEN HERE, at the publish, and not where a status is read. "Not
 * usable" is published from four places -- no sidecar at all, a HELLO that would
 * not go out, a link that dropped, and the sidecar's own STATUS -- and only the
 * last of them used to record it. So the state that decides "a server appeared"
 * still said `up` while the badge said `!`, and when the server really did come
 * back there was no edge to see: the badge cleared, nothing re-requested, and the
 * row sat grey with the server plainly there. Which is exactly what restarting
 * stemd under a loaded track looked like.
 *
 * Separator thread only, like every caller of ui_publish_status. */
static int  g_status_ok;
static int  g_server_up_edge;

void ui_publish_status(int reachable, int compatible)
{
    int ok = reachable && compatible;

    if (ok && !g_status_ok)
        g_server_up_edge = 1;
    g_status_ok = ok;

    __atomic_add_fetch(&g_ui.gen, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_ui.reachable = reachable;
    g_ui.compatible = compatible;
    g_ui.status_seen = 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_add_fetch(&g_ui.gen, 1, __ATOMIC_RELAXED);
}

void stem_progress_set(int stage, int percent)
{
    ui_publish(stage, percent, 0);
}

int stem_ui_read(struct stem_ui_state *out)
{
    int spins;

    for (spins = 0; spins < 8; spins++) {
        uint32_t before = __atomic_load_n(&g_ui.gen, __ATOMIC_RELAXED);
        uint32_t after;

        if (before & 1u)
            continue;                    /* writer mid-update */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        *out = g_ui;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        after = __atomic_load_n(&g_ui.gen, __ATOMIC_RELAXED);
        if (before == after)
            return before != 0;
    }
    /* Eight failed attempts means the writer is unusually busy, not that
     * anything is wrong. Skip this frame rather than spin on the one thread
     * that must never be held up. */
    memset(out, 0, sizeof(*out));
    return 0;
}

/* ---- worker state --------------------------------------------------------- */

/* A job that could not run for a passing reason is re-run, not abandoned. Long
 * enough that a deck sitting at the cue point is not re-probed constantly,
 * short enough that pressing play is followed by stems rather than by a wait. */
#define JOB_RETRY_SEC 5

/* How quiet the deck's loader has to be before a separation starts pushing PCM,
 * and how long that is worth waiting for. Wide, because it is free on this path
 * -- see run_separation. */
#define SEP_QUIET_MS       1500
#define SEP_SETTLE_MAX_MS 45000

void job_retry_later(void);

/* A job died, so the server's state is in doubt and the next loop turn re-probes
 * instead of waiting out the refresh interval.
 *
 * Without this the status was STARVED by its own retries. A job that reached the
 * server is proof the server is up, so finishing one legitimately defers the
 * periodic probe -- but the deferral was applied to every attempt, including the
 * failures. With a retry every JOB_RETRY_SEC and a probe every
 * STATUS_REFRESH_SEC, the probe was re-armed six times over before it could ever
 * fall due: cut the server mid-job and the deck retried forever while its status
 * line said the server was fine, because nothing ever asked again. */
volatile int  g_probe_now;

static pthread_t     g_loader, g_separator;
static int           g_worker_up;
volatile uint64_t g_retry_at; /* the loader may not re-probe before this */
volatile int  g_quit;         /* process is going away */
volatile int  g_resettle;     /* a stem setting moved: re-HELLO now */

/* ---- the current track: written by the MESSAGE thread, read by the loader ---
 *
 * A generation rather than a flag, so the loader cannot miss two changes inside
 * one of its ticks and cannot mistake "changed back" for "never changed". */
char              g_cur_path[STEM_CACHE_PATH_MAX];
volatile uint32_t g_cur_gen;

/* ---- loader -> separator ---------------------------------------------------
 *
 * Asked for ONLY on a cache miss, which is the whole of the cancel rule: a track
 * whose stems are already on the media never gets here, so switching to one
 * cannot disturb a separation that is running for something else. */
static char              g_want_path[STEM_CACHE_PATH_MAX];
static int64_t           g_want_frames;
volatile uint32_t g_want_gen;
volatile int      g_sep_supersede;   /* the job in flight is unwanted */
volatile int      g_sep_busy;        /* a separation is running now */

/* ---- separator -> loader ---------------------------------------------------
 *
 * PATHS, NEVER MEMORY. set_retire spins for in-flight audio readers and must
 * never race a publish; the loader being the only thread that ever touches
 * g_set is what makes that impossible rather than merely unlikely. */
struct job_delivery g_delivery;

/* What g_stems_on was the last time a settings change came through, so the
 * master gate's EDGE can be told from its level. Seeded at install from the
 * setting restored off eMMC: without that, a deck that boots with STEMS already
 * on would read its first settings change as the DJ having just switched it on.
 * -1 is "not seeded yet". */
int g_stems_was_on = -1;

/* Stems announced by the sidecar but not yet decoded. The two parts arrive as
 * separate frames and the store publishes both at once, so they are held here
 * until the pair is complete. Worker thread only, and reset per job. */
struct job_arrived g_arrived[STEM_N_PARTS];

/* What the job in flight is for. Worker thread only, so no synchronisation: it
 * is set at the top of run_one_job and read when the stems land. A copy rather
 * than decode.c's pointer, which is only stable until the next track load --
 * and a job outliving its track is exactly when this would be read. */
char    g_job_path[STEM_CACHE_PATH_MAX];
int64_t g_job_frames;


/* ---- the seam between the two workers ------------------------------------- */

/* Is `path` the track the DJ currently has loaded? The only question either
 * worker ever has to ask about relevance. */
int track_is_current(const char *path)
{
    return path && path[0] && strcmp(path, g_cur_path) == 0;
}

/* Separator -> loader: stems exist for `track`, at these two files.
 *
 * Published under a generation the loader polls. If `track` is no longer
 * current the loader drops it -- the media cache already has the durable copy,
 * so nothing is lost by nobody wanting it right now. */
/* OUR OWN stages, by the same judgement, because a stage belongs to the LOADED
 * track and not to whatever the loader happens to be busy with.
 *
 * A separation outlives the track that asked for it -- deliberately; it lands in
 * the cache for when that track comes back -- so a job the DJ has moved off goes
 * on running for minutes. Every other publisher already knows that: sep_progress
 * checks the track, job_failed checks `mine`, the delivery and the cache hit both
 * check it twice. The upload did not, so it painted its own progress over a track
 * that was already served, walked it to 100%, and then went quiet -- because the
 * frames AFTER the upload do go through sep_progress, which correctly dropped
 * them. The row then read UPLOADING 100% until the next track change. */
void job_progress(int stage, int pct)
{
    if (track_is_current(g_job_path))
        ui_publish(stage, pct, 0);
}

/* A job that died on the wire, told to the DJ and then tried again.
 *
 * The failure this is reached by most often is not permanent at all: the sidecar
 * stops reading this socket while it finishes the PREVIOUS job -- a cancel does
 * not make it drop what it already handed to the server -- so the next upload
 * fills the buffer and dies. Observed end to end: job cancelled at 10:20:29,
 * next one starts 10:20:39, FAILED at 10:20:43, and the sidecar only drains at
 * 10:20:50. Without a retry the DJ is left with a grey row and no way to ask
 * again short of reloading.
 *
 * So the stage is published -- the failure is real and the bar should say so --
 * and the job is scheduled again. If the cause is permanent the retry fails the
 * same way, at one attempt per JOB_RETRY_SEC, which is visible in the log rather
 * than silent.
 *
 * BOTH of those belong to the track the job was FOR, which is why this asks the
 * same relevance question sep_progress does. A job outlives its track whenever
 * the DJ loads another one over a separation, and it dies shortly after:
 *
 *   - the bar is the CURRENT track's, so publishing FAILED there paints a
 *     failure over the new track's IDLE for a job that was not about it
 *   - the retry re-serves whatever is loaded WHEN it falls due, which is the new
 *     track -- one the loader has already dealt with off the generation, and
 *     re-serving it decodes a cached pair a second time for nothing
 *
 * The probe is unconditional: it is about the SERVER, and the server is equally
 * in doubt whichever track the dead job belonged to. */
void job_failed(void)
{
    int mine = track_is_current(g_job_path);

    /* Ask the sidecar what it can still see. A failure is the one moment the
     * cached answer is least likely to be true, and it is also when the DJ most
     * needs the status line to say so. */
    g_probe_now = 1;

    if (!mine) {
        MDBG("stem_job: %s failed, but is no longer loaded -> nothing to say\n",
             g_job_path);
        return;
    }
    ui_publish(STEM_STAGE_FAILED, 0, 0);
    MDBG("stem_job: failed -> another attempt in %d s\n", JOB_RETRY_SEC);
    job_retry_later();
}

/* Loader -> separator: this track needs the server.
 *
 * Reached ONLY from a cache miss. A new want supersedes whatever is in flight;
 * that is the one place a separation is ever abandoned, and it cannot be reached
 * by a track whose stems are already on the media. */
void sep_request(const char *path, int64_t frames)
{
    if (!g_worker_up)
        return;
    /* Asking twice for a separation that is ALREADY RUNNING would supersede it
     * with itself, restarting from zero -- which is what leaving a track and
     * coming back to it does. A want that is no longer running is spent, and
     * asking again is the retry: gating on the path alone made job_failed's
     * reschedule inert, which is the grey row that never recovers. */
    if (strcmp(path, g_want_path) == 0 && g_want_gen &&
        __atomic_load_n(&g_sep_busy, __ATOMIC_ACQUIRE))
        return;
    /* Through the server, so the bar is divided: set here rather than when the upload
     * starts, because a run's shape is decided by asking, not by getting that far. */
    g_job_via_server = 1;
    snprintf(g_want_path, sizeof(g_want_path), "%s", path);
    g_want_frames = frames;
    __atomic_store_n(&g_sep_supersede, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_want_gen, g_want_gen + 1, __ATOMIC_RELEASE);
    MDBG("stem_job: %s needs the server -> asking the separator\n", path);
}

/* Sink for stem_decode_pull: one chunk straight onto the socket. Returning
 * non-zero aborts the decode, which is how cancellation reaches a thread that
 * is otherwise inside the deck's decoder. */

/* Map a sidecar frame onto the UI snapshot / stem store. Returns 0 to keep
 * waiting, 1 when the job reached a terminal state. */

/* The separator's whole job: upload the track it was asked for and see it
 * through. Tied to a PATH, not to whatever is loaded -- a track change does not
 * reach here, only a supersede does. */

void run_separation(void)
{
    const char *path = g_want_path;
    struct stem_job_begin begin;
    struct upload_ctx ctx;
    int64_t frames = g_want_frames;
    int fresh;

    if (!path[0] || frames <= 0)
        return;

    /* Let the deck finish loading before a full-track decode and a 171 MB
     * upload start competing with it.
     *
     * The cache path defers for the same reason, but it asks for the shortest
     * quiet it can get away with because that wait is the latency the DJ sees.
     * Here the wait is free: this job is about to spend tens of seconds on the
     * server, so a window wide enough that no load can still be running costs
     * nothing measurable and takes the upload off the loader's back entirely.
     *
     * Not fatal if it times out. A deck that has been opening readers for
     * SEP_SETTLE_MAX_MS is doing something other than the load we meant to keep
     * clear, and refusing to separate over it would strand the track. */
    stem_decode_wait_deck_quiet(SEP_QUIET_MS, SEP_SETTLE_MAX_MS);
    if (!track_is_current(path))
        return;                  /* the DJ moved on while we deferred */

    snprintf(g_job_path, sizeof(g_job_path), "%s", path);
    g_job_frames = frames;

    fresh = stem_ipc_ensure();
    if (fresh < 0) {
        /* THROUGH job_failed, for the same reason the await below is: this leaves
         * with the request already spent, so a bare return is a track nobody ever
         * looks at again -- the loader marks it served the moment it asks, and only
         * a scheduled retry or a track change brings it back. stem_ipc_ensure fails
         * for passing reasons (the sidecar restarting under systemd is the common
         * one), so that was a grey row waiting on an event that never comes. */
        ui_publish_status(0, 0);
        job_failed();            /* stem_ipc_ensure logged why */
        return;
    }

    /* Do not send a byte of PCM until the sidecar says it is ready -- but ONLY
     * when this connection is new.
     *
     * A HELLO makes the sidecar go and probe the server, and it blocks there
     * for about a second without reading this socket. The decoder runs at better
     * than 200x realtime, so it fills the whole socket buffer in that window,
     * every time, and the write that finds it full has nowhere to go: 28672
     * frames in, 229 KB, dead upload. Waiting for the STATUS that follows the
     * probe closes the window by construction, and saves pushing 171 MB at a
     * server that turns out to be unreachable.
     *
     * On a REUSED connection none of that applies: no HELLO went out, so the
     * sidecar never left its read loop and will never send a STATUS. Waiting
     * there simply hangs the job until the timeout and throws it away -- which
     * is what it did to the second track after a connect. */
    if (fresh && await_sidecar_ready() != 0) {
        /* THROUGH job_failed, not a bare return. The sidecar answering "the
         * server is not usable" is a failure the DJ has to be told about -- it
         * is the one that puts the ! on the button -- and it is also the one
         * most worth retrying, because a server coming back is exactly what the
         * DJ is waiting for.
         *
         * Returning silently here left the stage at IDLE with nothing scheduled,
         * so stems_available() went on answering "fine", the badge never lit and
         * the row opened grey with no explanation. */
        job_failed();
        return;
    }

    /* A REUSED connection sends no HELLO and so gets no STATUS, which means the guard
     * above does not run on it and the last answer we did get is the only thing that
     * says whether there is anything worth uploading to.
     *
     * Asking matters now that a failed job reliably reschedules itself: without this,
     * a retry against a server that is down decodes the whole track and pushes 171 MB
     * into a sidecar that decided to refuse it at the first frame, once every
     * JOB_RETRY_SEC, for as long as the server stays away. The refusal comes back as
     * JOB_FAILED at the end of all that -- the same answer this gives immediately.
     *
     * job_failed re-arms the probe, so the stale case costs one retry interval and
     * not a stuck row: the next idle turn asks the sidecar and the answer moves. */
    if (!fresh && !g_status_ok) {
        MDBG("stem_job: last status says no usable server -> not uploading %s\n", path);
        job_failed();
        return;
    }

    MDBG("stem_job: starting for %s\n", path);

    memset(&begin, 0, sizeof(begin));
    begin.frames = (uint64_t)frames;
    begin.sample_rate = 44100;
    begin.channels = 2;
    begin.output_sample_rate = sep_output_rate(path);
    /* FLAC back, because those bytes go straight onto the DJ's media: lossless
     * so the reconstruction stays exact, and roughly a third of the raw size
     * (measured on the reference track: a 171 MB pair becomes 61 MB, with the
     * sparse vocals stem alone falling to 21%). The deck's own FileReadFlac
     * opens it through the same createReaderFor path a WAV would use, so
     * nothing downstream changes. */
    begin.output_format = STEM_PCM_FLAC;
    if (stem_ipc_send(STEM_MSG_JOB_BEGIN, &begin, sizeof(begin)) != 0) {
        /* Every other failed send on this path already does this; this one was the
         * odd one out, and a dead socket at exactly the first frame is not a rarer
         * event than a dead socket four frames later. */
        stem_ipc_close();
        job_failed();
        return;
    }

    ctx.sent = 0;
    ctx.total = frames;
    ctx.cancelled = 0;
    job_progress(STEM_STAGE_UPLOADING, 0);
    if (stem_decode_pull(path, STEM_UPLOAD_RATE, upload_chunk, &ctx) < 0) {
        /* SAY it is a cancel rather than just going quiet.
         *
         * Dropping the socket aborts the POST just as effectively, but it is the wrong
         * word: the sidecar reads a short stream as a broken one, answers a JOB_FAILED
         * into a socket we already closed, and tears the session down -- so the next
         * track pays for a reconnect and the HELLO probe that follows it. A CANCEL frame
         * is what its upload pull is watching for (err 2, "cancelled by the deck"), and
         * it leaves the connection up for the track that caused the cancel in the first
         * place.
         *
         * Nothing to DELETE on this path: the POST never completed, so no job id came
         * back and there is no handle to release. That case is the poll loop below, and
         * it already sends this same frame. */
        if (ctx.cancelled) {
            stem_ipc_send(STEM_MSG_CANCEL, NULL, 0);
            return;
        }
        stem_ipc_close();
        job_failed();
        return;
    }

    /* Make the promise true.
     *
     * JOB_BEGIN commits to a byte count that becomes the POST's Content-Length,
     * and the count comes from the converter's own out_len -- which a decode is
     * not obliged to match. A mono WAV delivered short, the sidecar's body pull
     * then hit JOB_END with bytes still owed, and the POST died as `pull err 1`.
     *
     * Padding with silence rather than renegotiating: the shortfall is at the
     * tail, silence is what a track's tail is, and the alternative is a length
     * we cannot know until after the decode we already streamed. */
    if (ctx.sent < frames) {
        static const float k_silence[JOB_PCM_FRAMES * 2];
        int64_t missing = frames - ctx.sent;

        MDBG("stem_job: decode gave %lld of %lld frames, padding %lld\n",
             (long long)ctx.sent, (long long)frames, (long long)missing);
        while (missing > 0) {
            int64_t n = missing > JOB_PCM_FRAMES ? JOB_PCM_FRAMES : missing;

            if (stem_ipc_send(STEM_MSG_PCM, k_silence,
                              (uint32_t)(n * 2 * sizeof(float))) != 0) {
                stem_ipc_close();
                job_failed();
                return;
            }
            missing -= n;
        }
    }

    if (stem_ipc_send(STEM_MSG_JOB_END, NULL, 0) != 0) {
        stem_ipc_close();
        job_failed();
        return;
    }

    /* Wait for the separation, but not forever.
     *
     * A silence budget rather than a deadline on the whole job: the sidecar
     * polls the server every 500 ms and forwards a PROGRESS frame each time, so
     * a live job is never quiet for long however slow the server is, while a
     * wedged one says nothing at all. A total-time cap cannot tell those apart
     * and would kill legitimately long separations.
     *
     * Without this the loop below spun on a 250 ms timeout with no way out, and
     * the only thing that ever ended it was the DJ loading another track --
     * observed as a job parked for more than ten minutes with the server up and
     * the progress bar frozen. */
    {
        uint64_t quiet_since = job_now_sec();

        for (;;) {
            static char frame[4096];
            uint32_t type = 0, len = 0;
            int rc;

            if (__atomic_load_n(&g_sep_supersede, __ATOMIC_ACQUIRE) || g_quit) {
                stem_ipc_send(STEM_MSG_CANCEL, NULL, 0);
                return;
            }
            rc = stem_ipc_recv(&type, frame, sizeof(frame), &len,
                               JOB_RECV_TIMEOUT_MS);
            if (rc < 0) {
                stem_ipc_close();
                job_failed();
                return;
            }
            if (rc == 0) {
                if (job_now_sec() - quiet_since < JOB_SILENCE_SEC)
                    continue;
                MDBG("stem_job: sidecar silent for %d s -> abandoning the job\n",
                     JOB_SILENCE_SEC);
                stem_ipc_send(STEM_MSG_CANCEL, NULL, 0);
                stem_ipc_close();
                job_failed();
                return;
            }
            quiet_since = job_now_sec();
            if (handle_frame(type, frame, len))
                return;
        }
    }
}

#define STATUS_REPROBE_MS    3000

/* CLOCK_MONOTONIC, not the wall clock: the shim time-shifts gettimeofday for the
 * guest and a jump there must not park the health check for hours. */
uint64_t job_now_sec(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec;
}

/* Look at the current track again shortly.
 *
 * A time, not a flag: the loader re-examines whatever is loaded WHEN the delay
 * falls due, so a track change arriving in the meantime simply wins -- it clears
 * the delay and the loader works on the new track instead of resurrecting work
 * for one nobody is listening to. */
void job_retry_later(void)
{
    g_retry_at = job_now_sec() + JOB_RETRY_SEC;
}

/* A job given up on for want of a server, restarted when one turns up.
 *
 * Every other way a job can fail is either permanent (the file will not decode)
 * or self-rescheduling (job_retry_later). This one was neither: run_one_job
 * returns as soon as stem_ipc_ensure or await_sidecar_ready says there is
 * nothing to talk to, and the request that brought it there has already been
 * consumed -- so a deck that loaded a track while the server was down never
 * asked again, however long it waited afterwards. Start the server, open the
 * panel, and the faders sit grey with no bar and nothing happening, which is
 * indistinguishable from the feature being broken.
 *
 * The ARRIVAL is the trigger rather than a timer: nothing is spent while the
 * server is absent, and the length probe is not re-run every few seconds
 * against a track whose stems cannot be made yet. The cost of that choice is
 * latency -- the idle branch refreshes every STATUS_REFRESH_SEC, so this fires
 * within that of the server coming up, or as soon as a press asks for the probe
 * outright (stem_job_probe_now).
 *
 * Only from the idle branch, where nothing is in flight to disturb -- which is why
 * the separator calls this rather than refresh_status doing it on the way out. Three
 * of refresh_status's four exits are early returns that publish "no server" and never
 * reach the bottom, so a call sited there ran on the one path least likely to be the
 * one that mattered. */
void server_arrived(void)
{
    /* An EDGE, latched at the publish. Level would re-request on every refresh for
     * as long as a track had no stems -- which is every track the DJ chooses not to
     * separate. Consumed whether or not it leads anywhere: an edge nobody could act
     * on is spent, and holding it would fire on the next unrelated idle turn. */
    if (!g_server_up_edge)
        return;
    g_server_up_edge = 0;
    if (!g_stems_on)
        return;
    /* A track has to be loaded and still without stems. stem_store_complete is
     * the honest test for the second: g_request would only say one was asked
     * for, not that it ever landed. */
    if (!g_cur_path[0] || stem_store_complete())
        return;
    MDBG("stem_job: a server appeared and %s has no stems -> asking again\n",
         g_cur_path);
    stem_job_request();
}

void refresh_status(void)
{
    static char frame[4096];
    int fresh = stem_ipc_ensure();
    int waited;

    if (fresh < 0) {
        /* No sidecar at all is a reachability answer in its own right, and the
         * one the UI most needs: nothing can separate anything. */
        ui_publish_status(0, 0);
        return;
    }
    if (fresh == 0 && stem_ipc_hello() != 0) {
        stem_ipc_close();
        ui_publish_status(0, 0);
        return;
    }

    for (waited = 0; waited < STATUS_REPROBE_MS; waited += JOB_RECV_TIMEOUT_MS) {
        uint32_t type = 0, len = 0;
        int rc;

        if (__atomic_load_n(&g_sep_supersede, __ATOMIC_ACQUIRE) || g_quit)
            return;                  /* real work outranks a health check */
        rc = stem_ipc_recv(&type, frame, sizeof(frame), &len,
                           JOB_RECV_TIMEOUT_MS);
        if (rc < 0) {
            stem_ipc_close();
            ui_publish_status(0, 0);
            return;
        }
        if (rc == 0)
            continue;
        /* STATUS and nothing else. A late STEM_READY from a job that was
         * cancelled would otherwise be adopted here -- publishing the previous
         * track's stems over whatever is loaded now. */
        if (type != STEM_MSG_STATUS)
            continue;
        handle_frame(type, frame, len);
        return;
    }
}

/* ---- the loader: owns the resident stems and the UI snapshot --------------
 *
 * Always works on the CURRENT track and abandons whatever it is doing the moment
 * that changes. It is the only thread that ever touches g_set. */

/* The generation the running publish belongs to, and whether one is running at
 * all. Armed here rather than passed down because the question is asked from
 * inside the store's two decode threads, where a plain atomic compare is the
 * only thing cheap enough to ask per chunk. */
volatile uint32_t g_load_gen;
volatile int      g_load_armed;

int stem_job_load_wanted(void)
{
    if (!__atomic_load_n(&g_load_armed, __ATOMIC_ACQUIRE))
        return 1;                    /* not a publish the loader started */
    return __atomic_load_n(&g_load_gen, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(&g_cur_gen, __ATOMIC_ACQUIRE);
}

/* Publish, and let a track change cut it short.
 *
 * A publish is two waits and seconds of decode per stem, and it used to run to
 * completion whatever happened: load a track during one and nothing was looked
 * at until the PREVIOUS track had finished loading -- which is what the deck
 * showed, the old track completing its load before the new one started. The
 * generation is latched here and compared inside the store. */
/* ---- entry points --------------------------------------------------------- */

/* The message thread naming the track that is now loaded.
 *
 * This is the ONLY place the current track changes, and it does the one thing
 * that cannot wait: clearing readiness, so the audio thread is back on the stock
 * path before the next block rather than mixing the previous song's stems into
 * this one. Everything else -- freeing, probing, asking -- is the loader's, and
 * it picks this up from the generation. */
void stem_job_set_track(const char *path)
{
    if (path && path[0] && strcmp(path, g_cur_path) == 0)
        return;
    __atomic_store_n(&g_stem_ready, 0, __ATOMIC_RELEASE);
    /* The groove circuit belongs to the track that was playing: its phase is
     * measured from a position in that track's timeline, and its replacement is
     * expressed against that track's stems. Dropping it here rather than letting
     * the mix carry it over is the difference between the effect ending and it
     * landing somewhere arbitrary in the new one. */
    gc_disarm();
    stem_grid_forget();
    snprintf(g_cur_path, sizeof(g_cur_path), "%s", path ? path : "");
    __atomic_store_n(&g_cur_gen, g_cur_gen + 1, __ATOMIC_RELEASE);
    /* AND THE ROW GOES BLANK HERE, not when the loader next comes round. The
     * loader does clear it on the generation -- but it only reaches that test
     * between jobs, and a separation is minutes long, so the stage of the track
     * before stayed on screen for the whole of the next one. Published AFTER
     * g_cur_path moves, so the job that is still running cannot put its own
     * stage back: job_progress is asking about this path. */
    ui_publish(STEM_STAGE_IDLE, 0, 0);
}

/* Ask the sidecar what it can see, at the next idle turn rather than at the next
 * refresh.
 *
 * The refresh interval is thirty seconds and the whole of what it buys is latency:
 * a server that came up a moment ago is invisible until it falls due. That is fine
 * for a deck nobody is touching and wrong the instant the DJ reaches for STEMS,
 * because a press IS the question this answers.
 *
 * A flag, not a probe: the socket is the worker's and this is called from the
 * message thread. The separator picks it up between jobs, so a press during a
 * separation costs nothing and is honoured when the job ends. */
void stem_job_probe_now(void)
{
    if (!g_worker_up)
        return;
    g_probe_now = 1;
}

/* Ask for the loaded track to be looked at again.
 *
 * Not "start a job": whether one is needed is the loader's to decide, and on a
 * cached track the answer is no server at all. Used by the settings gate and by
 * a server turning up. */
void stem_job_request(void)
{
    if (!g_worker_up)
        return;
    g_retry_at = 0;
    __atomic_store_n(&g_cur_gen, g_cur_gen + 1, __ATOMIC_RELEASE);
}

/* Drop the stems held for the track that has gone.
 *
 * Deliberately NOT a cancel of the separation. A separation belongs to a track,
 * not to the transport, and one the DJ has switched away from is still worth
 * finishing -- it lands in the media cache and is there when they come back.
 * The only thing that ever abandons one is another track needing the server,
 * which is sep_request's business. */
void stem_track_gone(void)
{
    stem_job_set_track(NULL);
}


/* ---- the feature's MOD SETTINGS rows --------------------------------------
 *
 * Declared here because this is the module they configure: the sidecar link,
 * which is also what mods_stem_settings_changed pushes them to. STEM SERVER LOCATION
 * hangs off ENABLE STEMS and the address off MANUAL, so the whole feature is one
 * switch away from not existing and no row can be edited into a state nothing
 * reads. [message] for the callbacks; registration is [init]. */
static const char *const k_stem_where[2] = { "AUTO", "MANUAL" };

/* A strict IPv4 dotted quad: four decimal octets, 0-255, and nothing else. The
 * address is handed to connect(), so a prefix (10.0.0.5/24) names a network
 * rather than a host and is refused along with host names, IPv6 and anything
 * malformed. Leading zeros go too: "010" is ten to some resolvers and eight to
 * others, and that ambiguity is not worth carrying into a field nobody will
 * think to re-read. */

/* An empty address is NOT SET, a legitimate state, so only a typed one is
 * checked; anything else is dropped rather than kept, because a row holding an
 * address the client cannot use is worse than one that plainly reads NOT SET.
 * The overlay calls this BEFORE it persists, so what lands on disk is the
 * cleared value and never the bad one. */
static const struct kit_row k_rows[] = {
    KIT_ROW_BOOL("ENABLE STEMS", &g_stems_on,
                 .idx = KIT_IDX_STEMS, .changed = mods_stem_settings_changed),
    { .label = "STEM SERVER LOCATION", .idx = 0, .parent = &k_rows[0], .show_when = 1,
      .state = &g_stem_manual, .values = k_stem_where, .nvalues = 2,
      .changed = mods_stem_settings_changed },
    { .label = "STEM SERVER ADDRESS", .idx = 0, .parent = &k_rows[1], .show_when = 1,
      .text = g_stem_addr, .text_cap = STEM_ADDR_MAX, .changed = addr_changed },
};

void stem_job_poll(void)
{
    /* Message thread. Reads the snapshot and nothing else -- see stem.h for why
     * the socket deliberately is not touched here. The widget update lands in
     * ui.c, which owns every Component. */
}

static int stem_job_install(void)
{
    if (pthread_create(&g_separator, NULL, separator_main, NULL) != 0 ||
        pthread_create(&g_loader, NULL, loader_main, NULL) != 0) {
        MERR("stem_job: worker thread failed to start; STEMS unavailable\n");
        return -1;
    }
    pthread_detach(g_separator);
    pthread_detach(g_loader);
    g_worker_up = 1;
    /* After mods_settings_load(), so this is the DJ's saved choice rather than
     * the compiled default. */
    g_stems_was_on = g_stems_on ? 1 : 0;
    kit_menu_add(k_rows, (int)(sizeof(k_rows) / sizeof(k_rows[0])));
    MDBG("stem_job: worker thread up (STEMS %s)\n",
         g_stems_was_on ? "on" : "off");
    return 0;
}

KIT_MOD(k_mod_stem_job,
        .name = "stem_job", .prio = 60, .install = stem_job_install,
        .what = "the worker thread that owns the sidecar link");
