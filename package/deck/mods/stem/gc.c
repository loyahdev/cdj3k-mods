// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem/gc.c - GROOVE CIRCUIT: eight slots, each replacing one stem with a file.
 *
 * Put loops on the stick and name them in mods/gc/gc-config.txt:
 *
 *     # audio path, slot, stem, bpm
 *     loops/hard-kick.wav,  1, d, 124
 *     Contents/pads/Am.wav, 4, h, 124
 *     loops/shout.wav,      8, v, 90
 *
 * Slot 1..8 is pad A..H and the letter says which stem goes: d drums, h
 * harmonics, v vocals. A pad no entry names is left alone and is a hot cue
 * exactly as it always was, so the DJ decides which pads the circuit owns.
 *
 * A LIST RATHER THAN A NAMING RULE, because the audio is the DJ's and lives
 * where they keep it -- a path in a file lets a loop stay in the folder it came
 * in, be used by two slots, and carry its BPM, none of which a filename can do
 * without becoming a language.
 *
 * ONE AT A TIME across the whole feature. Two replacements would be two answers
 * to "what is the drums part", and even for different stems it is one gesture
 * the DJ is making -- so arming a slot disarms the one before it.
 *
 * WHY A FILE AND NOT A REGION of the track: a region can only be a stem the deck
 * already has, which rules out the drums (the residual exists at the play head
 * and nowhere else) and rules out anything the track does not contain. A file is
 * neither, costs a few MB, and is the DJ's own material.
 *
 * THE FIRST BANK ONLY. These belong to the deck rather than to a track, so there
 * is no track to break a tie with if two sticks disagreed about slot 3 -- see
 * stem_media_first_root.
 *
 * Threading. [worker] scans and decodes and is the only writer of the table;
 * [audio] reads it under the same acquire/release discipline the stems use, and
 * [deck] arms a slot. Nothing here allocates on the audio thread.
 */
#include "stem/stem.h"
#include "lamp/lamp.h"
#include "cue/cue.h"
#include "kit/mod.h"

/* ---- what a slot is ------------------------------------------------------- */

/* The subdirectory is ours, so the DJ's own folders cannot collide with it and a
 * stick with no `mods` is simply a stick with no slots. */
#define GC_DIR          "mods/gc"
#define GC_CONFIG       GC_DIR "/gc-config.txt"
#define GC_PATH_MAX     STEM_CACHE_PATH_MAX
#define GC_LINE_MAX     512

/* A loop is bars, not minutes. The cap is what stops a mis-named full track
 * eating the memory the stems need: at the pool's 96 kHz stereo s16 this is
 * about 11 MB a slot, 92 MB if a DJ fills all eight, against stems that are
 * already ~350 MB of a 3 GiB guest. Longer files load truncated rather than
 * refused, because a truncated loop still plays. */
#define GC_MAX_SECONDS  30

struct gc_slot {
    int16_t *pcm;        /* interleaved stereo at the pool rate, NULL if empty */
    int64_t  frames;     /* what the decoder produced, padding included        */
    int64_t  span;       /* what the loop is -- whole beats when a BPM is known*/
    double   spb;        /* samples a beat at the pool rate, 0 without a BPM   */
    int      part;       /* STEM_PART_* -- which stem this one stands in for   */
};

static struct gc_slot gc_g_slot[CUE_PADS];

/* What the table was built for. A pool rate change or a different stick means
 * the buffers are wrong rather than stale, so both are re-scanned. */
static char gc_g_root[GC_PATH_MAX];
static int  gc_g_rate;

/* Readers in the mix, so a scan cannot free a buffer under one. Same shape as
 * the stem store's, and for the same reason: the audio thread must never wait
 * and the worker must never free early. */
static int gc_g_readers;
static int gc_g_live;

/* The armed slot and where the track was when it was armed. `active` is
 * published LAST and cleared FIRST, which is the whole of the synchronisation:
 * a reader that sees a slot sees the position written before it, and one that
 * catches a writer mid-update sees -1 and mixes the track live for a block.
 *
 * THE POSITION IS THE PHASE. The file's index is (track pos - engage) at the
 * loop's own tempo, wrapped over its length, so there is no cursor to keep and
 * no state to drift: every block computes where the loop is from where the track
 * is. Seek, and the loop lands wherever the track did, which is what a loop
 * locked to the timeline should do.
 *
 * `engage` is the GRID's first downbeat, not the moment the pad went down, so
 * the phase reduces to how many beats into the track we are -- and the loop's
 * downbeat lands on the track's rather than on the DJ's reaction time. Only a
 * track with no readable grid falls back to the play head.
 *
 * [deck] writes, [audio] and [message] read. */
static int64_t gc_g_engage;
static int     gc_g_active = -1;

/* ---- the table, as the mix sees it ---------------------------------------- */

int gc_acquire(int slot, struct gc_view *out)
{
    if (slot < 0 || slot >= CUE_PADS)
        return 0;
    __atomic_fetch_add(&gc_g_readers, 1, __ATOMIC_ACQ_REL);
    if (!__atomic_load_n(&gc_g_live, __ATOMIC_ACQUIRE) || !gc_g_slot[slot].pcm) {
        __atomic_fetch_sub(&gc_g_readers, 1, __ATOMIC_ACQ_REL);
        return 0;
    }
    out->pcm    = gc_g_slot[slot].pcm;
    out->frames = gc_g_slot[slot].frames;
    out->span   = gc_g_slot[slot].span;
    out->spb    = gc_g_slot[slot].spb;
    out->part   = gc_g_slot[slot].part;
    return 1;
}

void gc_release(void)
{
    __atomic_fetch_sub(&gc_g_readers, 1, __ATOMIC_ACQ_REL);
}

/* ONE predicate for "this slot can run", asked by the pad that would claim the
 * press and by the lamp that would colour it. Splitting them is how a pad ends
 * up lit for something it will not do -- the stems have to be resident too,
 * because every replacement is expressed against the track's harmonics and
 * vocals and there is nothing to express without them.
 *
 * GATED ON THE STEMS ROW, which is the whole of the mode. Eight slots would
 * otherwise take all eight hot cues away for as long as the stick is in, and a
 * hot cue is not something to spend without asking. With the row closed every
 * pad is stock -- no claim, no colour, and nothing of ours in the way; open it
 * and the pads become the circuit's. The lamp says which state the deck is in,
 * so there is nothing to remember.
 *
 * THE GATE IS ON THE PADS, NOT ON THE CIRCUIT. Closing the row hands the pads
 * back and leaves a running replacement running, exactly as it leaves the stem
 * levels where the DJ put them: the row is where the controls live, not what the
 * feature is. So this answers -1 for a pad whose slot is at that moment
 * REPLACING a stem, which is correct -- the pad will not toggle it while the row
 * is closed, and a lamp lit for something the pad will not do is worse than no
 * lamp. The cost is that a replacement is not visible on the pads until the row
 * is open again; the row itself is where it shows. */
int gc_slot_part(int slot)
{
    if (slot < 0 || slot >= CUE_PADS)
        return -1;
    if (!stems_row_open())
        return -1;
    if (!__atomic_load_n(&gc_g_live, __ATOMIC_ACQUIRE) || !gc_g_slot[slot].pcm)
        return -1;
    if (!stem_store_complete())
        return -1;
    return gc_g_slot[slot].part;
}

int gc_active_slot(void)
{
    return __atomic_load_n(&gc_g_active, __ATOMIC_ACQUIRE);
}

/* ---- the pads' colours ---------------------------------------------------
 *
 * A slot's colour IS the stem it takes away, so a glance at the pads says what
 * the circuit is holding rather than merely which pads are loaded. Red drums,
 * blue harmonics, green vocals, indexed by STEM_PART_*.
 *
 * gc_slot_part() answers -1 for a slot that could not run -- the stems row
 * closed, no file, or no stems resident to express a replacement against -- so
 * a lamp is never lit for something the pad will not do. It is the same
 * predicate the press asks, which is what keeps the two from disagreeing.
 *
 * The blink is off the monotonic clock rather than a count of draws: the lamp
 * is redrawn at whatever rate the panel runs at, so counting frames would make
 * the rate a property of how busy the deck is. */
#define GC_BLINK_MS 350

static const uint8_t k_gc_part_hue[N_STEMS][3] = {
    { 255,   0,   0 },      /* STEM_PART_DRUMS     */
    {   0,   0, 255 },      /* STEM_PART_HARMONICS */
    {   0, 255,   0 },      /* STEM_PART_VOCALS    */
};

int gc_pad_lamp(int pad, struct lamp *out)
{
    int part = gc_slot_part(pad);

    if (part < 0 || part >= N_STEMS)
        return 0;
    if (pad == gc_active_slot() && ((lamp_now_ms() / GC_BLINK_MS) & 1)) {
        lamp_dark(out);                 /* the dark half of the blink */
        return 1;
    }
    lamp_set(out, k_gc_part_hue[part][0], k_gc_part_hue[part][1],
             k_gc_part_hue[part][2], LAMP_LIT);
    return 1;
}

/* Which stem the running replacement stands in for, or -1 when none is running.
 *
 * UNGATED, unlike gc_slot_part: the row gate is about what a PAD does, and this
 * answers for the badge on the title bar, which is the one thing that says a
 * replacement is running while the row is closed. Gating it would blank exactly
 * the indicator the gate creates the need for. */
int gc_active_part(void)
{
    int slot = __atomic_load_n(&gc_g_active, __ATOMIC_ACQUIRE);

    if (slot < 0 || slot >= CUE_PADS || !__atomic_load_n(&gc_g_live, __ATOMIC_ACQUIRE))
        return -1;
    return gc_g_slot[slot].pcm ? gc_g_slot[slot].part : -1;
}

int gc_active(int64_t *engage)
{
    int slot = __atomic_load_n(&gc_g_active, __ATOMIC_ACQUIRE);

    if (slot >= 0)
        *engage = gc_g_engage;
    return slot;
}

void gc_arm(int slot, int64_t engage)
{
    __atomic_store_n(&gc_g_active, -1, __ATOMIC_RELEASE);
    gc_g_engage = engage;
    __atomic_store_n(&gc_g_active, slot, __ATOMIC_RELEASE);
}

void gc_disarm(void)
{
    __atomic_store_n(&gc_g_active, -1, __ATOMIC_RELEASE);
}

/* ---- loading (worker) ----------------------------------------------------- */

/* One decode's worth of state. stem_decode_pull hands over chunks of float and
 * this converts to s16 as they arrive, so the peak is the s16 buffer plus one
 * chunk rather than a whole float copy of the file. */
struct gc_load {
    int16_t *pcm;
    int64_t  cap;        /* frames the buffer holds */
    int64_t  n;          /* frames written so far   */
};

static int gc_sink(const float *in, int64_t frames, void *user)
{
    struct gc_load *l = user;
    int64_t i, take = frames;

    if (l->n + take > l->cap)
        take = l->cap - l->n;
    for (i = 0; i < take * 2; i++) {
        float v = in[i] * 32767.0f;

        /* The DJ's own file, so it is already whatever level they made it --
         * clipped rather than scaled, because quietening someone's loop to fit
         * a sample they will never see is a decision that belongs to them. */
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        l->pcm[l->n * 2 + i] = (int16_t)v;
    }
    l->n += take;
    return take < frames;          /* full -> stop the decoder */
}

static int gc_part_of(char c)
{
    switch (c) {
    case 'd': case 'D': return STEM_PART_DRUMS;
    case 'h': case 'H': return STEM_PART_HARMONICS;
    case 'v': case 'V': return STEM_PART_VOCALS;
    default:            return -1;
    }
}

static const char *gc_part_name(int part)
{
    switch (part) {
    case STEM_PART_DRUMS:     return "drums";
    case STEM_PART_HARMONICS: return "harmonics";
    case STEM_PART_VOCALS:    return "vocals";
    default:                  return "?";
    }
}

/* Decode one file into `s`. Returns 1 when the slot ends up playable. */
static int gc_load_one(const char *path, int rate, int part, float bpm,
                       struct gc_slot *s)
{
    struct gc_load l;
    int64_t frames;

    /* What the DECODER will produce, which is not the file's own count: the
     * deck's chain pads, and the length is what the buffer has to hold. */
    frames = stem_decode_pull(path, rate, NULL, NULL);
    if (frames <= 0) {
        MDBG("gc: %s will not decode\n", path);
        return 0;
    }
    if (frames > (int64_t)rate * GC_MAX_SECONDS) {
        MDBG("gc: %s is %lld frames, truncated to %d s\n",
             path, (long long)frames, GC_MAX_SECONDS);
        frames = (int64_t)rate * GC_MAX_SECONDS;
    }

    l.pcm = malloc((size_t)frames * 2 * sizeof(int16_t));
    if (!l.pcm) {
        MDBG("gc: no memory for %lld frames of %s\n", (long long)frames, path);
        return 0;
    }
    l.cap = frames;
    l.n   = 0;

    if (stem_decode_pull(path, rate, gc_sink, &l) < 0 || l.n <= 0) {
        MWARN("gc: %s decoded nothing\n", path);
        free(l.pcm);
        return 0;
    }
    s->pcm    = l.pcm;
    s->frames = l.n;
    s->part   = part;
    s->spb    = 0.0;
    s->span   = l.n;

    /* THE LOOP IS WHOLE BEATS, NOT THE BUFFER. What came back is the file plus
     * whatever the decoder padded it with -- an encoder delay is tens of
     * milliseconds, which is a hole at the end of every bar. A stated BPM says
     * how long a beat is, the nearest whole number of them is what the DJ
     * exported, and rounding to it puts the wrap back on the beat and leaves the
     * padding beyond the end of the loop where it is never read.
     *
     * Nearest, not floor: the padding makes the buffer longer than the loop, so
     * rounding down would drop a real beat off any file whose padding happens to
     * exceed half of one. */
    if (bpm > 0.0f) {
        double spb = (double)rate * 60.0 / (double)bpm;
        int64_t beats = (int64_t)((double)l.n / spb + 0.5);
        int64_t span;

        if (beats < 1)
            beats = 1;
        span = (int64_t)((double)beats * spb + 0.5);
        if (span > l.n)
            span = l.n;              /* a BPM that says the file is longer */
        s->spb  = spb;
        s->span = span;
    }
    return 1;
}

/* Drop the table and wait for the mix to let go of it. */
static void gc_unload(void)
{
    int i;

    __atomic_store_n(&gc_g_live, 0, __ATOMIC_RELEASE);
    while (__atomic_load_n(&gc_g_readers, __ATOMIC_ACQUIRE) > 0)
        usleep(1000);
    for (i = 0; i < CUE_PADS; i++) {
        free(gc_g_slot[i].pcm);
        gc_g_slot[i].pcm    = NULL;
        gc_g_slot[i].frames = 0;
        gc_g_slot[i].part   = -1;
        gc_g_slot[i].span   = 0;
        gc_g_slot[i].spb    = 0.0;
    }
    gc_disarm();
}

/* ---- the config file ------------------------------------------------------
 *
 *     # audio path, slot, stem, bpm
 *     loops/hard-kick.wav, 1, d, 124
 *
 * Comma-separated because a path may contain spaces and a DJ should not have to
 * quote one. `#` to end of line is a comment, blank lines are skipped, and
 * surrounding whitespace on every field is trimmed -- this is a file people
 * edit, so the format forgives what a person would naturally type.
 *
 * A relative path is relative to the VOLUME ROOT, not to mods/gc: the audio is
 * the DJ's own and belongs wherever they keep it, which is usually beside their
 * tracks rather than beside our config. An absolute path is taken as given.
 *
 * BPM may be omitted or 0, which means the loop's tempo is simply not stated. */

static char *gc_trim(char *p)
{
    char *e;

    while (*p == ' ' || *p == '\t') p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return p;
}

/* Split `line` on commas into at most `max` trimmed fields. Returns the count. */
static int gc_split(char *line, char **out, int max)
{
    int n = 0;

    while (n < max) {
        char *c = strchr(line, ',');

        if (c) *c = '\0';
        out[n++] = gc_trim(line);
        if (!c) break;
        line = c + 1;
    }
    return n;
}

/* One config line into `gc_g_slot`. Returns 1 when a slot ended up playable. */
static int gc_config_line(const char *root, char *line, int rate)
{
    char *f[4], path[GC_PATH_MAX];
    char *hash = strchr(line, '#');
    int nf, slot, part;
    float bpm;

    if (hash) *hash = '\0';
    if (!*gc_trim(line))
        return 0;

    nf = gc_split(line, f, 4);
    if (nf < 3) {
        MDBG("gc: \"%s\" needs at least path, slot and stem -> ignored\n", line);
        return 0;
    }
    slot = atoi(f[1]);
    part = gc_part_of(f[2][0]);
    bpm  = nf >= 4 ? (float)atof(f[3]) : 0.0f;
    if (slot < 1 || slot > CUE_PADS) {
        MDBG("gc: slot %d is not 1..%d -> ignored\n", slot, CUE_PADS);
        return 0;
    }
    if (part < 0) {
        MDBG("gc: \"%s\" is not d, h or v -> slot %d ignored\n", f[2], slot);
        return 0;
    }
    if (gc_g_slot[slot - 1].pcm) {
        MDBG("gc: slot %d named twice -> the later entry is ignored\n", slot);
        return 0;
    }
    if ((size_t)(f[0][0] == '/'
                 ? snprintf(path, sizeof(path), "%s", f[0])
                 : snprintf(path, sizeof(path), "%s/%s", root, f[0]))
        >= sizeof(path)) {
        MDBG("gc: slot %d path is too long -> ignored\n", slot);
        return 0;
    }
    if (!gc_load_one(path, rate, part, bpm, &gc_g_slot[slot - 1]))
        return 0;

    /* Both lengths, because the difference between them is the whole story: the
     * beats are what the loop is, the frames are what the decoder handed over,
     * and a gap between them that is not a few milliseconds of padding is a
     * wrong BPM -- caught here, rather than as a loop drifting on a dance
     * floor. */
    if (bpm > 0.0f)
        MDBG("gc: slot %d = %s (%s, %.1f BPM, %.2f beats -> %lld of %lld frames)\n",
             slot, path, gc_part_name(part), (double)bpm,
             (double)gc_g_slot[slot - 1].frames / rate * bpm / 60.0,
             (long long)gc_g_slot[slot - 1].span,
             (long long)gc_g_slot[slot - 1].frames);
    else
        MDBG("gc: slot %d = %s (%s, %lld frames, no BPM stated)\n",
             slot, path, gc_part_name(part),
             (long long)gc_g_slot[slot - 1].frames);
    return 1;
}

/* Rebuild the table from `root`. Worker thread. */
static void gc_scan(const char *root, int rate)
{
    char path[GC_PATH_MAX], line[GC_LINE_MAX];
    FILE *fp;
    int n = 0;

    gc_unload();
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", root, GC_CONFIG) >= sizeof(path))
        return;
    fp = fopen(path, "r");
    if (!fp) {
        /* No config is a stick with no slots, which is most sticks. Said once
         * per scan rather than silently, because "the pads are not coloured" is
         * otherwise indistinguishable from a bug. */
        MDBG("gc: no %s -> no slots\n", path);
        snprintf(gc_g_root, sizeof(gc_g_root), "%s", root);
        gc_g_rate = rate;
        __atomic_store_n(&gc_g_live, 1, __ATOMIC_RELEASE);
        return;
    }
    while (fgets(line, sizeof(line), fp))
        n += gc_config_line(root, line, rate);
    fclose(fp);

    snprintf(gc_g_root, sizeof(gc_g_root), "%s", root);
    gc_g_rate = rate;
    __atomic_store_n(&gc_g_live, 1, __ATOMIC_RELEASE);
    MDBG("gc: %d slot%s loaded from %s at %d Hz\n",
         n, n == 1 ? "" : "s", path, rate);
}

/* Called from the worker's idle branch. Cheap when nothing has moved: two
 * string compares against what the table was built for. */
void mod_stem_gc_poll(void)
{
    char root[GC_PATH_MAX];
    int rate = stem_pool_rate();

    if (rate <= 0)
        return;                     /* no timeline to decode onto yet */
    if (!stem_media_first_root(root, sizeof(root))) {
        if (gc_g_root[0]) {
            MDBG("gc: media gone -> slots dropped\n");
            gc_unload();
            gc_g_root[0] = '\0';
            gc_g_rate = 0;
        }
        return;
    }
    if (rate == gc_g_rate && strcmp(root, gc_g_root) == 0)
        return;
    gc_scan(root, rate);
}

/* ---- the gesture (deck) --------------------------------------------------- */

/* A pad the DJ has put a file behind is the circuit's; every other pad is a hot
 * cue and never sees this. That is the whole gating rule -- there is no mode to
 * enter and no panel to open, because the lamp already says which pads are
 * which and the stick is where the DJ decided it. */
static int gc_claim(const struct cue_event *ev)
{
    return gc_slot_part(ev->pad) >= 0;
}

static void gc_pad(const struct cue_event *ev, enum cue_phase phase)
{
    int64_t at;

    /* The press carries the whole gesture; the release is the DJ letting go of
     * a switch that is already thrown. */
    if (phase != CUE_PAD_PRESSED)
        return;

    if (ev->pad == gc_active_slot()) {
        gc_disarm();
        MDBG("gc: slot %d off -> back with the track\n", ev->pad + 1);
        return;
    }
    if (gc_active_slot() >= 0)
        MDBG("gc: slot %d takes over from slot %d\n",
             ev->pad + 1, gc_active_slot() + 1);

    /* HERE, because a pad press is the one moment a cue slot is in reach, and
     * because the answer is only wanted while a slot is armed. Re-read on every
     * arm rather than cached: it costs a handful of reads and it cannot then be
     * the previous track's. */
    stem_grid_take(ev);

    /* THE GRID IS THE PHASE, not the moment the pad went down.
     *
     * Anchoring to the play head makes the loop run at the track's tempo and
     * start wherever the finger landed, which is a bar turned by however late
     * the press was -- in time and out of place. Anchoring to the grid's first
     * downbeat instead makes the loop's own downbeat land on the track's: the
     * phase becomes how many beats into the track we are, modulo the loop, and
     * every press, re-press and takeover drops into the same alignment.
     *
     * The cost is that a press does not restart the loop from its first frame.
     * That is the right way round for a part standing in for a stem, which has
     * to sit in the track's bar rather than in the DJ's reaction time.
     *
     * Without a grid there is nothing to align to, and the play head is the only
     * reference left. */
    at = stem_grid_spb() > 0.0 ? stem_grid_beat0() : stem_source_pos();
    if (at < 0)
        at = 0;                     /* nothing has been read yet; phase from 0 */
    gc_arm(ev->pad, at);

    /* Both tempos, because "the loop is not in time" has two causes that look
     * identical: a BPM the config states wrongly, and a track whose grid was not
     * readable. One line separates them. */
    {
        double file_spb  = gc_g_slot[ev->pad].spb;
        double track_spb = stem_grid_spb();

        if (file_spb > 0.0 && track_spb > 0.0)
            MDBG("gc: slot %d on -> %s from the file, %.1f into %.1f BPM, "
                 "on the grid from %lld\n",
                 ev->pad + 1, gc_part_name(gc_slot_part(ev->pad)),
                 (double)gc_g_rate * 60.0 / file_spb,
                 (double)gc_g_rate * 60.0 / track_spb, (long long)at);
        else
            MDBG("gc: slot %d on -> %s from the file, at %lld, at its own tempo "
                 "(%s)\n", ev->pad + 1, gc_part_name(gc_slot_part(ev->pad)),
                 (long long)at,
                 file_spb > 0.0 ? "the track's tempo is not known"
                                : "no BPM in the config");
    }
}

CUE_HANDLER(k_cue_gc,
            .name = "groove", .prio = 5,
            .pad = gc_pad, .pad_claim = gc_claim);

/* Ahead of the behaviours that react to the deck's press (gate at 10, smart at
 * 20, preview at 30): this one replaces that press rather than following it, so
 * it has to be asked before anything is built on top of one. */

static int gc_install(void)
{
    if (prestem_native_owner_active()) {
        MDBG("stem_gc: native PRE-STEMS owns stem interaction path -> skipped\n");
        return 0;
    }

    if (!cue_pad_ready()) {
        MDBG("gc: no cue interception -> GROOVE CIRCUIT unavailable\n");
        return -1;
    }
    return 0;
}

KIT_MOD(k_mod_stem_gc,
        .name = "stem_gc", .prio = 62, .install = gc_install,
        .what = "groove circuit: a file in place of a stem, eight slots");

/* ---- what takes a slot off ------------------------------------------------
 *
 * ONE THING: the track changing, from stem_job_set_track. A replacement is
 * expressed against a particular track's stems and phased against its grid, so
 * it cannot survive one; everything else leaves it alone.
 *
 * PAUSING IS NOT A STOP. The DJ pausing has thrown no switch, and a slot that
 * comes off under the finger is a slot that has to be re-armed before the next
 * press of PLAY. The pad and the lamp say what is engaged; the transport does
 * not get a vote.
 *
 * There is no play-state call in the layer, and the play head is not a stand-in
 * for one: stem_source_pos() is the stretcher's last read position, so it also
 * sits still whenever the audio thread does. Deciding "stopped" from it turned
 * every stall -- and a resample is most of a load -- into a slot switching
 * itself off. */
