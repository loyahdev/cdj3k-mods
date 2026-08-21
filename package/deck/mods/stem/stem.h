// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem.h - the contract between the stem modules.
 *
 * Threading model, the three thread boundaries, module layout and lifetime:
 * docs/stem-engine.md. Each declaration below is tagged with the thread that may
 * call it; ../mod_core.h defines the tags.
 *
 *   ui/        [message]  the play-screen UI, split by component; see ui/ui.h
 *   audio.c    [audio]    the mix point; reads gains and readiness only
 *   decode.c   [filler]   reader open() capture; the 44.1 kHz decode chain
 *   job.c      [worker]   job state machine; sole writer of the UI snapshot
 *   ipc.c      [worker]   framing to the sidecar
 *   store.c    [worker]   stem files, open-then-unlink, page-pool registration
 *
 * A function is called on its module's thread unless its own comment says
 * otherwise. If a new one fits none of these, that is a design question rather
 * than a place to add a lock.
 */
#ifndef EP122_MODS_STEM_H
#define EP122_MODS_STEM_H

#include "core/mod_core.h"
#include "lamp/lamp.h"
#include "juce/draw.h"
#include "theme/theme.h"
#include "stem_proto.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ---- settings (persisted; see mod_settings.h) ----------------------------- */

/* ENABLE STEMS. Off by default; the quick-menu button and every stem setting
 * below hang off it. */
extern int g_stems_on;

/* 0 = AUTO (discover on the LAN), 1 = MANUAL (use the address below). A bool so
 * the stock two-value radio works; only the labels differ from OFF/ON. */
extern int g_stem_manual;

/* Host name or address, typed on the software keyboard. Empty until set; the
 * value column ellipsises what does not fit. */
#define STEM_ADDR_MAX 64
extern char g_stem_addr[STEM_ADDR_MAX];

/* The server's identity for the separations it produces -- backend, model, preset
 * and stemd version, in one opaque string. It scopes the on-media cache so two
 * models coexist on a stick.
 *
 * Persisted because a cache lookup needs it before any server could supply it: a
 * deck that has seen a server once plays stems from the stick without one. Empty
 * until a server identifies itself, which disables the cache rather than guessing
 * a directory name.
 *
 * STEM_SEP_ID_LEN is from stem_proto.h -- it crosses the socket in stem_status. */
#define STEM_SEP_ID_MAX STEM_SEP_ID_LEN
extern char g_stem_sep_id[STEM_SEP_ID_MAX];

/* Tell the worker one of the two settings above moved. The sidecar learns AUTO vs
 * MANUAL and the address from the HELLO only, and re-runs discovery on each one,
 * so a change has to be pushed or the old discovery result stands.
 *
 * Raises a flag; the worker does the round trip, so a settings screen never blocks
 * on the network. Cheap enough to call for a value the sidecar ignores.
 *
 * It is also where ENABLE STEMS being switched on acts: the loaded track is asked
 * for, and the DJ gets the notice about needing a server. A settings screen calls
 * this and needs to know nothing else about stems. */
void mods_stem_settings_changed(void);   /* [message] */

/* Stems the model produces: level sliders on the row, bands the waveform
 * analysis derives. */
#define N_STEMS 3

/* The levels as the AUDIO thread sees them: plain normalised floats, 1.0 = unity.
 * The UI mirrors every slider change into these, because the value itself lives in
 * a juce::Value and reading it goes through Value::getValue -> juce::var -> ~var,
 * which allocates. Allocation on the audio thread is how you get dropouts, so the
 * audio side never touches juce -- it reads a float.
 *
 * Defaults are unity and g_stem_bypass is clear, so a deck with STEMS enabled but
 * untouched passes audio through bit-identically.
 *
 * Relaxed atomics, not volatile: volatile has no memory model in C11 and does not
 * rule out a torn read. Relaxed is sufficient because each level is independent --
 * a block mixed from one level 5 ms stale is inaudible; half a float is not. On
 * aarch64 both compile to a single LDR/STR.
 *
 * Always go through the accessors. */
extern float g_stem_gain[N_STEMS];
extern int   g_stem_bypass;   /* BYPASS: stems out of circuit */

/* ---- the held MUTE, quantized --------------------------------------------
 *
 * A mute that lands wherever the finger did is a hole in the bar; on the beat it
 * is an edit. So the caption plate answers the press at once -- the word, the
 * blink, the wedge all change in the frame it happened, because that is the DJ's
 * own feedback -- and the AUDIO waits for the next quarter beat.
 *
 * `want` is the intent, written by [message]; `live` is what the mix applies,
 * committed by [audio] on the boundary. `unmuted` is the fader's own gain, kept
 * separately so the commit can restore it without reaching into the row's UI
 * state from the audio thread.
 *
 * GATED ON THE DECK'S OWN QUANTIZE, exactly as the sampler is: with it off the
 * commit is immediate, because a DJ who turned quantize off has said what they
 * want from every timed gesture on the deck. */
#define STEM_MUTE_QUANT  4        /* quarter beat */

extern int   g_stem_mute_want[N_STEMS];
extern int   g_stem_mute_live[N_STEMS];
extern float g_stem_gain_unmuted[N_STEMS];

/* [audio] Once per block, from the mix. Cheap when nothing is pending: three
 * integer compares. */
void stem_mute_commit(void);
/* [any] How many mutes the mix has actually applied. Counted because the press
 * and the sound are deliberately not the same moment, so "the caption changed"
 * is not evidence the audio did. [audio] adds, [message] reads. */
extern unsigned g_stem_mute_commits;

/* [audio] read, [message] write. The generic __atomic_load/__atomic_store, not
 * the _n forms: those take integer and pointer types only, and a level is a
 * float. */
static inline float stem_gain_get(int i)
{
    float v;

    __atomic_load(&g_stem_gain[i], &v, __ATOMIC_RELAXED);
    return v;
}

static inline void stem_gain_set(int i, float v)
{
    __atomic_store(&g_stem_gain[i], &v, __ATOMIC_RELAXED);
}

static inline int stem_bypass_get(void)
{
    return __atomic_load_n(&g_stem_bypass, __ATOMIC_RELAXED);
}

static inline void stem_bypass_set(int v)
{
    __atomic_store_n(&g_stem_bypass, v, __ATOMIC_RELAXED);
}

/* ---- worker -> audio ------------------------------------------------------
 *
 * The whole surface the realtime thread sees. g_stem_gain and g_stem_bypass are
 * declared below because the UI writes them too; this is the third.
 *
 * Cleared BEFORE any teardown begins and set only AFTER a stem set is completely
 * in place, so the audio thread never observes a half-built or half-torn-down
 * set. It reads this once per block and takes the stock path when clear. */
extern volatile int g_stem_ready;

/* ---- worker -> message ---------------------------------------------------- */

/* What the UI needs to draw, written by job.c and by nothing else.
 *
 * `gen` is bumped before and after each write, so a reader that sees the same
 * even value twice around its copy knows the copy is consistent. No lock: the
 * message thread must not block, and a stale frame is invisible. */
struct stem_ui_state {
    volatile uint32_t gen;
    int stage;            /* enum stem_stage */
    /* 0..100 WITHIN THE CURRENT LEG, not across the job: the upload counts its own
     * frames, the server its own separation, the decode its own frames. Weighting it
     * onto the bar is the row's, because the division of the bar is the row's -- see
     * the map in ui/ui.h. */
    int percent;
    int queue_position;   /* jobs ahead of us; meaningful when QUEUED */
    /* The shape of this run: 1 when it goes through the separator, 0 when it came off
     * the media. Published rather than inferred, because the stage in hand does not
     * carry it -- LOADING is the last leg of a separation and the only leg of a cache
     * hit -- and the row is only watching while it is open, so it cannot count on
     * having seen a run begin. */
    int via_server;
    int reachable;        /* a server answered a health probe */
    int compatible;       /* server topology we can actually play */
    /* Whether `reachable`/`compatible` mean anything yet. Before the first STATUS
     * they are merely zero, which is indistinguishable from "no server" -- and a
     * warning shown at boot, before anything has been asked, is a lie the DJ has
     * no way to check. */
    int status_seen;
};

/* Copy the current state consistently. Message thread. Returns 0 if no writer
 * has published anything yet, in which case *out is left zeroed. */
int stem_ui_read(struct stem_ui_state *out);   /* [message] */

/* ---- job.c: the state machine (worker thread) ----------------------------- */

/* Start work for the track decode.c most recently saw opened. Idempotent per
 * track: a second call for a track already in flight or already done is a no-op.
 * Safe to call from the message thread -- it only sets a request flag, the work
 * happens on the worker. */
void stem_job_request(void);   /* [message] */

/* Ask the sidecar to go and look for a server NOW -- an mDNS browse when the address
 * it already has does not answer -- instead of waiting out the 30 s status refresh.
 * Sets a flag the worker reads between jobs; it never touches the socket, so it is
 * safe from the message thread and free while a separation is running. */
void stem_job_probe_now(void);   /* [message] */

/* Name the track the next request is for. Message thread, called from the track
 * watch: the path has to be resolved where the sourceId is known, because the
 * worker cannot tell a re-served track from a newly opened one. */
void stem_job_set_track(const char *path);   /* [message] */

/* Abandon whatever is in flight and drop any stems held for the old track.
 * Cooperative: the worker observes the flag at its next chunk boundary. Never
 * pthread_cancel -- that would abandon a SampleRateConverter mid-call and leak
 * a reader inside the deck's own allocator. */
/* The track that was loaded has gone: drop the stems held for it.
 *
 * NOT a cancel of any separation in flight. A separation belongs to a track
 * rather than to the transport, and one the DJ has switched away from is still
 * worth finishing -- it lands in the media cache and is waiting when they come
 * back. The only thing that abandons one is another track needing the server. */
void stem_track_gone(void);   /* [message] */

/* Publish a stage the WORKER itself is inside, for the phases with no sidecar to
 * report them: the length probe, and the seconds-long decode of the pair onto the
 * pool timeline. Worker thread only -- the snapshot has exactly one writer, and
 * store.c's second decode thread must not call this.
 *
 * It lives here rather than in store.c because job.c owns the snapshot; see the
 * seqlock note at the top of job.c for why a second writer would be a bug rather
 * than a race that shows up as a stale frame. */
void stem_progress_set(int stage, int percent);   /* [worker] */

/* Message thread, called from the repaint tick. Reads the snapshot and updates
 * the widgets -- and does nothing else. It deliberately does NOT touch the
 * sidecar socket: the worker owns that fd end to end, which keeps a descriptor
 * out of two threads and leaves this call unable to block by construction. */
void stem_job_poll(void);   /* [message] */

/* ---- ipc.c: framing to the sidecar ----------------------------------------
 *
 * WORKER THREAD ONLY, all of it. The socket is blocking on purpose: a large PCM
 * send throttling against the sidecar's drain is exactly the backpressure that
 * keeps a whole track from ever being resident. That is only safe because no
 * other thread ever waits on this fd. */

/* Connect if not connected, sending HELLO on success. Retries are rate-limited
 * internally, so a dead sidecar costs one failed attempt per interval rather
 * than a spin.
 *
 *   1  a NEW connection, so a HELLO just went out
 *   0  the existing connection was reused
 *  -1  no usable socket
 *
 * The 1-vs-0 distinction is load-bearing, not informational. HELLO is what sends
 * the sidecar off to probe the server -- the second in which it stops reading
 * this socket, and the only thing that makes it answer with STATUS. So a caller
 * must wait for STATUS before pushing PCM on a fresh connection, and must NOT
 * wait on a reused one, where that frame never comes. */
int  stem_ipc_ensure(void);
void stem_ipc_close(void);

/* Send a HELLO down a connection that already exists. The sidecar answers it by
 * re-running discovery and reporting STATUS, so this is how a reachability answer
 * is refreshed without dropping the socket.
 *
 * Only safe while nothing is in flight: the sidecar stops reading this socket for
 * about a second while it probes, which is the window a concurrent PCM push dies
 * in. The worker's idle branch is the one place that holds. */
int  stem_ipc_hello(void);

/* Send one frame; `payload` may be NULL when len is 0. Blocks until written.
 * Returns 0, or -1 on a broken socket (the caller closes and reports it). */
int  stem_ipc_send(uint32_t type, const void *payload, uint32_t len);

/* Receive one frame. Returns 1 when the out-parameters were filled, 0 when
 * nothing was ready within `timeout_ms`, -1 on a broken socket. */
int  stem_ipc_recv(uint32_t *type, void *buf, uint32_t cap, uint32_t *len,
                   int timeout_ms);

/* ---- decode.c: the PCM source (worker thread) ----------------------------- */

/* The file behind a sourceId, binding it on first sight to whatever the deck
 * last opened. This is the honest answer to "what is playing", and the only one
 * safe to upload or to key the cache on.
 *
 * Message thread: it is called from the track watch, which is where the sid is
 * known and where string work is allowed. Returns NULL before any track has
 * been opened. */
const char *stem_decode_path_for_sid(uint64_t lo, uint64_t hi);

/* The rate the separation model is trained at, and so the only rate the server
 * accepts. Playback uses stem_pool_rate() instead -- see audio.c. */
#define STEM_UPLOAD_RATE 44100

/* Decode the whole track at `rate`, handing each chunk to `sink`. The sink
 * returns 0 to continue or non-zero to abort, which is how cancellation gets
 * in without a signal. Returns the number of frames delivered, or -1 on error.
 *
 * With `sink == NULL` it builds the chain, reads the converter's output length
 * and tears down without decoding anything -- that is how the caller learns the
 * frame count for Content-Length without paying for a second full pass.
 *
 * The count is the DECODER's, not the file's: the deck pads (4116 frames on the
 * reference track), and stems built against the file's count come back
 * misaligned with the deck's own timeline.
 *
 * Worker thread only: it opens files and runs a decoder. */
typedef int (*stem_pcm_sink_fn)(const float *interleaved, int64_t frames,
                                void *user);
int64_t stem_decode_pull(const char *path, int rate, stem_pcm_sink_fn sink,
                         void *user);

void mod_stem_decode_report(void);

/* How long the deck's own track loader has been quiet, in ms.
 *
 * Every reader the factory builds runs through our open hook, and the ones we
 * build for stems are excluded, so this is the deck opening files for the track
 * it is loading -- the activity a stem decode must not race. It answers the
 * question the stretcher rate cannot: that rate reports whether the audio engine
 * is being pulled, which stays healthy right through a load. */
#define STEM_DECK_NEVER_OPENED  ((uint64_t)-1)
uint64_t stem_decode_deck_quiet_ms(void);

/* Block until the loader has been quiet for `quiet_ms`, or `max_ms` passes.
 * Returns 1 when it went quiet, 0 on timeout or when the DJ moved on.
 *
 * The patience is the caller's because waiting does not cost the same on both
 * paths. A cache hit is the whole latency the DJ sees, so it asks for the least
 * quiet that still means "the loader stopped" and gets moving. A separation is
 * about to spend tens of seconds on the server regardless, so it can afford to
 * be sure, and it asks for a window long enough that no load can still be
 * running -- which is free there and buys the deck a clean load. */
int stem_decode_wait_deck_quiet(unsigned quiet_ms, unsigned max_ms);

/* ---- store.c: stem files and the page pool (worker thread) ---------------- */

/* Decode both stem files onto the pool's timeline and publish them for the
 * audio thread. Worker thread: it runs the deck's decoder twice and allocates
 * hundreds of MB.
 *
 * The two failures are DIFFERENT and the caller must not merge them. A cache
 * hit that returns RETRY is a perfectly good entry the deck was not ready to
 * load; treating that as a damaged one throws away a valid pair and starts a
 * 171 MB upload for a track already on the stick. That is what
 * "cached pair failed to decode, re-separating" was, every time the deck had
 * not played yet. */
#define STEM_PUBLISH_OK      0
#define STEM_PUBLISH_RETRY (-1)   /* not now -- no pool rate, no memory */
#define STEM_PUBLISH_BAD   (-2)   /* these files will not decode */
#define STEM_PUBLISH_ABORT (-3)   /* the track moved on; nothing is wrong */

int  stem_store_publish(const char *harmonics_path, float harmonics_gain,
                       const char *vocals_path, float vocals_gain);

/* Is the publish now running still for the track the DJ has loaded?
 *
 * A publish is seconds of decode per stem plus two waits. The store asks this
 * wherever it is about to spend time -- both decode threads and both waits --
 * and gives up the moment the answer is no, so a track loaded during one is
 * not held behind the previous track. That is ABORT, not BAD: the files are
 * fine and the cache entry must not be condemned over it.
 *
 * Answers 1 for any publish the loader did not start: the question is about the
 * loader's generation, so a publish from anywhere else has nothing to compare
 * against and must not be cut short by a mismatch that means nothing. */
int  stem_job_load_wanted(void);

/* Drop every stem held for the current track and wait until no audio thread is
 * still reading them. The caller clears g_stem_ready first. */
void stem_store_release_all(void);

/* What the realtime side sees: two flat s16 buffers on the pool's timeline and
 * the number of frames both of them cover.
 *
 * REALTIME. acquire() returns 1 when the buffers are live and MUST be paired
 * with release() before the read returns -- that pairing is the only thing
 * stopping the worker freeing memory mid-mix. */
struct stem_view {
    const int16_t *harmonics;
    const int16_t *vocals;
    /* 1.0/gain for each part. The stored samples are the server's NORMALISED
     * ones -- a separated stem can peak past full scale, so the restored value
     * does not fit an int16 and clipping it there would corrupt the derived
     * drums part. The restoration is folded into the mix coefficient instead,
     * which costs one multiply per block. */
    float          h_scale;
    float          v_scale;
    int64_t        frames;
};
int  stem_store_acquire(struct stem_view *out);
void stem_store_release(void);

/* True once both shipped parts are registered and playable. */
int  stem_store_complete(void);

/* The rate the RESIDENT stems were decoded at, 0 if none are. This is what the
 * pool measurement is checked against -- not whatever source would answer now.
 * Any thread. */
int  stem_store_rate(void);

/* ---- cache.c: stems on the DJ's own media (worker thread) -----------------
 *
 * The point of this is not latency -- stemd caches its own output, so a repeat
 * request already skips separation. It is that STEMS has to work with no server
 * on the network at all: stick in, track loads, stems play.
 *
 * Every path here is absolute and lives on the stick, so both buffers have to
 * hold a mount point, the layout and a hashed key with room to spare. */
#define STEM_CACHE_PATH_MAX 384

struct stem_cache_entry {
    char    harmonics_path[STEM_CACHE_PATH_MAX];
    char    vocals_path[STEM_CACHE_PATH_MAX];
    float   harmonics_gain;
    float   vocals_gain;
};

/* Is there a cached pair for this track? `frames` is the DECODER's count, from
 * the probe in stem_decode_pull -- it is part of the key, because stems are
 * aligned to EP122's padded decode and a firmware that pads differently must
 * miss rather than load something silently misaligned.
 *
 * Returns 0 and fills `out` on a hit. A miss is not an error condition: no
 * media, no server ever seen, or simply not separated yet all land here. */
int stem_cache_lookup(const char *track_path, int64_t frames,
                      struct stem_cache_entry *out);

/* Copy a freshly separated pair from tmpfs onto the media. Returns 0 when the
 * entry is committed. Failure is survivable by design -- the stems are already
 * resident and will play; only the next load pays for it again. */
int stem_cache_store(const char *track_path, int64_t frames,
                     const char *harmonics_src, float harmonics_gain,
                     const char *vocals_src, float vocals_gain);

/* ---- audio.c ---- */
void mod_stem_audio_report(void);

/* The page pool's sample rate, i.e. the rate every stem has to be decoded at to
 * share the deck's timeline. Never assumed: it follows the engine's output
 * setting, and this deck happens to run 96 kHz while its library is 44.1.
 *
 * Two sources, cross-checked against each other -- how fast pcmbuf::Position
 * advances at the mix point, which needs playback, and what the PCM is open at,
 * which does not. Returns 0 only when neither has an answer yet; callers must
 * treat that as "not yet" and not as a rate. Safe from any thread. */
int  stem_pool_rate(void);

/* Frames the deck's time stretcher has been asked for, cumulatively, or 0 if
 * nothing is counting them. The rate this moves at says whether the deck has its
 * CPU back after a track load -- which is the precondition for starting a decode
 * of our own, and the one that works on a paused deck. See the definition. */
uint64_t stem_engine_frames(void);

/* Measure the engine's rate now, blocking for a quarter of a second. Worker
 * thread only. Returns 0 when nothing is counting. Use this rather than waiting
 * for stem_pool_rate() to become non-zero on its own: the passive source only
 * updates while the play screen paints, and a track is loaded from the browser. */
int stem_engine_rate_measure(void);

/* The playhead, in pool-rate samples, or -1 if nothing has been read yet. Any
 * thread. Divided by stem_pool_rate() it is the position in seconds, and times
 * 150 it is the waveform column under the needle. */
int64_t stem_source_pos(void);

/* The playing track's id, read off pcmbuf::Position. Returns 0 and leaves the
 * pair meaningless until a block has been read. Any thread.
 *
 * This is the same 128-bit value the track-info replies carry as
 * `trackid::TrackID` -- see wave_stems.c, where it is what tells the deck
 * view's waveform from a browse preview's. */
int stem_source_id(uint64_t *lo, uint64_t *hi);

/* ---- gc.c: GROOVE CIRCUIT, a file in place of a stem ----------------------
 *
 * Eight slots on the DJ's own stick, one per pad:
 *
 *     mods/gc/slot1-d.wav    pad A replaces the DRUMS with this file
 *     mods/gc/slot4-h.wav    pad D replaces the HARMONICS
 *     mods/gc/slot8-v.wav    pad H replaces the VOCALS
 *
 * A pad with a file owns that pad; a pad without one is a hot cue exactly as it
 * always was. ONE replacement at a time across the whole feature, because two
 * would be two answers to the same gesture.
 *
 * A FILE RATHER THAN A REGION of the track. A region can only be a stem the deck
 * already has, which rules out the drums -- the residual `mix - harmonics -
 * vocals` exists at the play head and nowhere else -- and rules out anything the
 * track does not contain. A file is neither, and it is the DJ's own material. */

/* The first mounted volume under the USB base, which is the first bank. 0 when
 * nothing is mounted. Lives in cache.c, which owns the mount test. [worker] */
int stem_media_first_root(char *out, size_t cap);

/* Rescan the stick if the volume or the pool rate has moved. Cheap otherwise --
 * called from the worker's idle branch. [worker] */
void mod_stem_gc_poll(void);

/* ---- grid.c -------------------------------------------------------------- */

/* Pool-rate samples per beat for the loaded track, or 0 when it is not known --
 * no cue to read it from, or a track with no analysis. This is what rate-matches
 * a loop; see grid.c for the walk that produces it. Any thread. */
double stem_grid_spb(void);

/* Where the grid's first downbeat sits, in pool-rate samples. Meaningless while
 * stem_grid_spb() is 0. Anchoring a loop here rather than at the play head is
 * what puts its downbeat on the track's. [any] */
int64_t stem_grid_beat0(void);

/* Read it off whichever cue the deck has, and publish it. A cue slot is where
 * the source object that owns the beat grid is reachable, and a pad press is
 * when one is in hand -- so this is called as a slot is armed, and answers for
 * the track playing at that moment. [deck] */
struct cue_event;
int stem_grid_take(const struct cue_event *ev);

/* [message] Arm the grid from the deck's own reply, off the display clock, so a
 * track nobody has pressed a pad on still has one. Cheap once it has taken. */
void stem_grid_tick(void);

/* Forget it, so a stale tempo cannot be applied to the next track. [any] */
void stem_grid_forget(void);

/* THE WHOLE GRID, not the one number above it: every beat of the loaded track,
 * ascending, in pool-rate samples.
 *
 * WHY BOTH. stem_grid_spb() is the AVERAGE beat, which is exactly right for a
 * fixed-tempo track and wrong by however much the tempo moves for one that
 * drifts -- a loop phased off it slides a little further out of place every bar.
 * The array is what makes a loop land on beat 129 when the track does, whatever
 * the tempo did to get there. The average stays because it is what a track with
 * no readable array still has, and because the file's own tempo is a single
 * number either way.
 *
 * REALTIME. acquire() returning 1 must be paired with release() before the read
 * returns; that pairing is the only thing stopping the next track's grid freeing
 * this one mid-mix. 0 means there is no array to use, which is not a failure --
 * it is the fixed-tempo route, and it is what every track had before this. */
struct stem_grid_view {
    const int64_t *beats;
    int32_t        count;
};
int  stem_grid_beats_acquire(struct stem_grid_view *out);
void stem_grid_beats_release(void);

/* ---- editing the deck's own grid -----------------------------------------
 *
 * Rescale the loaded track's beat grid by `k`, where k is a BPM MULTIPLIER: 2.0
 * doubles the number of beats (the 3000X's [x2]), 0.5 halves it, and a value
 * near 1 is the fine [Enlarge]/[Reduce] stretch. Returns 0 when the grid moved.
 *
 * THE SHAPE IS PRESERVED, not flattened. A new beat j is placed at the original
 * grid's position for fractional beat j/k, interpolated -- so a hand-gridded or
 * drifting track keeps its drift and only its tempo scales. Laying beats down at
 * one constant interval would silently discard exactly the analysis a DJ went to
 * the trouble of making.
 *
 * `stem_grid_edit_reset` puts back what the deck loaded, which is what the
 * panel's own RESET is: the original array is kept, never freed and never
 * written to, so a reset cannot depend on the arithmetic being invertible.
 *
 * [message] -- grid/panel.c is the only caller, so there is exactly one writer;
 * see the definition for why this is safe to do under live readers. */
int  stem_grid_edit_scale(double k);
int  stem_grid_edit_reset(void);

/* Make the edited grid the track's, through the deck's own register path --
 * which ends in the DB server writing the Quantize atom of the track's analysis
 * file, so the edit survives the cache entry, the track being reloaded, and the
 * media being ejected. Returns 0 when the request was accepted; the write itself
 * lands on the repository's thread shortly after.
 *
 * REFUSES a grid whose TrackID we do not know -- a holder reached through a cue
 * slot can be the previous track's, and writing this track's beats onto that
 * one's id would be a corruption a DJ could not see coming.
 *
 * The BROWSER's tempo is a different back end and does NOT follow; see the
 * definition. [message] */
int  stem_grid_edit_save(void);

/* The grid's current BPM, for the panel's readout, or 0 when there is none. */
double stem_grid_bpm(void);

/* The tempo the deck loaded, before any edit -- what an edit is measured
 * against, since stem_grid_edit_scale always rescales the ORIGINAL and so
 * replaces the previous edit rather than compounding with it.
 *
 * `stem_grid_id` is a token for WHICH grid that is, only ever compared: it
 * changes when the deck loads another one, which is when a panel has to forget
 * whatever edit state it was accumulating. 0 when there is no grid. */
double    stem_grid_orig_bpm(void);
uintptr_t stem_grid_id(void);

/* The offset the DECK's own grid-adjust modes move; non-zero means its RESET has
 * something to undo. See the definition. */
int64_t   stem_grid_offset(void);

/* One slot as the mix sees it: the file, at the pool rate, interleaved stereo.
 * REALTIME -- acquire() returning 1 must be paired with release() before the
 * read returns, which is the only thing stopping a rescan freeing it mid-mix.
 *
 * `span` is the loop, `frames` the buffer. They differ because a decoder pads:
 * looping over what came back would put that padding at the end of every bar,
 * so a slot whose config line states a BPM loops over whole beats instead and
 * the padding is simply never reached. Without a stated BPM there is nothing to
 * round to and the two are equal. */
struct gc_view {
    const int16_t *pcm;
    int64_t        frames;
    int64_t        span;
    double         spb;       /* file samples a beat, 0 when no BPM was stated */
    int            part;      /* STEM_PART_* this one stands in for */
};
int  gc_acquire(int slot, struct gc_view *out);
void gc_release(void);

/* Which stem slot `slot` replaces, or -1 when it holds no file. Any thread; the
 * lamps ask it for a colour and the pads for whether to claim a press. */
int gc_slot_part(int slot);

/* The groove circuit's answer for one pad, for lamp/lamp.c: 0 when the pad is
 * not a runnable slot, else *out is the stem the slot takes away -- steady
 * while it is only loaded, blinking while it is the one actually replacing. */
int gc_pad_lamp(int pad, struct lamp *out);

/* The armed slot, or -1. `gc_active` also hands back the position the loop's
 * phase is measured from -- the grid's first downbeat, or the play head on a
 * track with no grid. The index into the file is (track pos - engage) at the
 * loop's own tempo, wrapped over its length, so there is no cursor to keep and
 * nothing to drift. [deck] arms and disarms, [audio] and [message] read. */
int  gc_active_slot(void);
int  gc_active(int64_t *engage);

/* STEM_PART_* of the running replacement, or -1. Ungated by the stems row --
 * see the definition, and the badge in stem/ui/ that is its only caller. */
int  gc_active_part(void);
void gc_arm(int slot, int64_t engage);
void gc_disarm(void);

/* ---- ui.c / widgets.c / layout.c (message thread) ------------------------- */

/* Whether the STEMS control row is up. The groove circuit is gated on it, so a
 * pad with a slot file behind it is an ordinary hot cue until the DJ opens the
 * row -- which is what gives them their eight cues back. [any] */
int stems_row_open(void);

/* Return every stem level to full mix. Message thread only: it moves Sliders.
 * Called on a track change, so a level set for the previous track cannot silently
 * apply to the next one -- see the note at the definition for why that is a
 * hazard rather than an inconvenience. */
void mod_stems_reset_levels(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_STEM_H */
