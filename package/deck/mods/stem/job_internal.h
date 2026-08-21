/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/stem/job_internal.h - what the STEMS job files share.
 *
 * job.c owns the job state machine and every flag below; job_loader.c hands
 * decoded stems to the audio side; job_settings.c owns the MOD SETTINGS rows.
 * Declarations only.
 */
#ifndef EP122_MOD_STEM_JOB_INTERNAL_H
#define EP122_MOD_STEM_JOB_INTERNAL_H

#include "stem/stem.h"

/* Set when a settings change means the job must be re-placed, and whether
 * STEMS was on last time the rows were read. Defined in job.c. */
extern volatile int  g_resettle;
extern int g_stems_was_on;

/* The settings watcher, and the loader thread job.c starts. */
void addr_changed(void);
void * loader_main(void *arg);

/* A job that could not run for a passing reason is re-run, not abandoned. Long
 * enough that a deck sitting at the cue point is not re-probed constantly,
 * short enough that pressing play is followed by stems rather than by a wait. */
#define JOB_RETRY_SEC 5

/* The job state the loader thread reads. Defined in job.c. */
extern char              g_cur_path[STEM_CACHE_PATH_MAX];
extern int g_job_via_server;

uint64_t job_now_sec(void);

extern volatile uint32_t g_cur_gen;
extern volatile int      g_load_armed;
extern volatile uint32_t g_load_gen;
extern volatile int  g_quit;
extern volatile uint64_t g_retry_at;

struct job_delivery {
    char  track[STEM_CACHE_PATH_MAX];   /* which track these are for */
    char  h[STEM_CACHE_PATH_MAX], v[STEM_CACHE_PATH_MAX];
    float hg, vg;
    int   tmpfs;                        /* ours to unlink once loaded */
    volatile uint32_t gen;
};

struct job_arrived {
    char  path[192];
    float gain;
    int   have;
};

extern struct job_delivery g_delivery;
extern struct job_arrived g_arrived[STEM_N_PARTS];

void job_retry_later(void);
void sep_request(const char *path, int64_t frames);
int track_is_current(const char *path);
void ui_publish(int stage, int percent, int queue_position);

/* Block until the sidecar has probed the server and reported back.
 *
 * Generous, because AUTO discovery browses mDNS before it can probe anything and
 * that legitimately takes seconds on a cold LAN. Returns 0 when the server is
 * usable, -1 otherwise -- including "reachable but incompatible", which is a
 * reason not to upload rather than a reason to try and be told later. */
#define JOB_READY_TIMEOUT_MS  20000

/* How long the worker waits for a sidecar frame before looping to re-check the
 * cancel flag. Short enough that a track change is acted on promptly, long
 * enough that an idle job is not a spin. */
#define JOB_RECV_TIMEOUT_MS 250

#define SEP_RATE_POLL_MS    250

/* The rate to ask the server to deliver stems at: the pool's, when we can
 * establish it.
 *
 * Stems that arrive already on the pool's timeline are opened by the deck's own
 * decoder and passed through, so the 44.1 -> 96 k conversion never runs here.
 * Measured on this deck it is most of what a cached load costs -- a whole track
 * decodes at 907x realtime when the rates match and 102x when they do not.
 *
 * This is only safe because the server converts with the DECK's filter.
 *
 * Drums is derived as `mix - harmonics - vocals` and that mix is resampled by
 * the pool, so a stem converted by a different filter leaves the difference
 * between the two on the drums fader -- measured at -36 dB of that part when
 * stemd used its own rubato, and plainly audible. The deck's converter was
 * identified instead (LTI, 320/147 polyphase, 18880 taps, see
 * docs/ep122-resampler.md) and stemd reproduces it to -141.5 dB, which is below
 * the 16 bits a stem is stored at.
 *
 * Nothing negotiates the filter and nothing should: the shim and stemd are the
 * two halves of one design and ship together, so a version of one that disagrees
 * with the other is not a configuration to detect at runtime. Read as a warning
 * instead -- a stemd built without ep122.rs answers this request with stems at
 * the right rate and the wrong phase, and only the drums fader will say so.
 *
 * Waited for rather than sampled once. The rate is a property of the deck, not
 * of the track, but nothing has measured it yet on the first separation after a
 * restart: the position measurement needs playback, and the stretcher needs the
 * engine to be running, which it is not a quarter of a second after a load on a
 * paused deck. Sampling once got a 0 there and quietly asked for the native
 * rate, which is correct and slow -- exactly the failure that is hardest to
 * notice. Waiting is free on this path for the same reason the deck-quiet defer
 * is: the job behind it is tens of seconds of model time.
 *
 * Zero when the rate still cannot be established, which asks for the server's
 * default and leaves the conversion on the deck. Slower, never wrong: stem files
 * are self-describing and stem_decode_pull converts whatever it opens. */
#define SEP_RATE_WAIT_MS   8000

/* Ask the sidecar again whether it can see a server.
 *
 * The warning under STEMS is only worth showing if it is current, and STATUS
 * arrives exactly once, at connect. Without this the answer is as old as the last
 * cache miss: a server that came up an hour ago still reads as absent, and one that
 * went away still reads as present -- and the second is the one that matters,
 * because it is the state where pressing STEMS gets you nothing.
 *
 * Idle branch only. A HELLO puts the sidecar into a blocking probe for about a
 * second, which is safe here precisely because nothing is in flight to be starved.
 * The whole exchange is bounded: no answer, no update, try again next interval. */
#define STATUS_REFRESH_SEC   30

struct upload_ctx {
    int64_t sent;
    int64_t total;
    /* Why the sink stopped. A track change and a dead socket both abort the decode the
     * same way, and the sidecar needs to be told which -- see run_one_job. */
    int     cancelled;
};

extern int64_t g_job_frames;
extern char    g_job_path[STEM_CACHE_PATH_MAX];
extern volatile int  g_probe_now;
extern volatile int      g_sep_busy;
extern volatile int      g_sep_supersede;
extern volatile uint32_t g_want_gen;

void job_failed(void);
void job_progress(int stage, int pct);
void refresh_status(void);
/* Act on a server that has just become usable. The edge is latched at the publish
 * (see ui_publish_status), so this is safe to call every idle turn: it does nothing
 * until one is waiting, and consumes it when it is. Idle branch only -- it re-serves
 * the loaded track, which must not happen with a job in flight. */
void server_arrived(void);
void run_separation(void);
void ui_publish_status(int reachable, int compatible);

/* The sidecar conversation, in job_separator.c. */
void * separator_main(void *arg);
int upload_chunk(const float *pcm, int64_t frames, void *user);
int await_sidecar_ready(void);
int handle_frame(uint32_t type, const void *buf, uint32_t len);
uint32_t sep_output_rate(const char *path);

#endif /* EP122_MOD_STEM_JOB_INTERNAL_H */
