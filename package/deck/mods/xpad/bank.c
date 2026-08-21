// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/bank.c - eight samples off the stick, decoded onto the pool's timeline.
 *
 * Put audio in mods/loops/ and the first eight files, sorted by name, become
 * pads A..H. Nothing else is needed and nothing else is read: a sample is a
 * sample, and the one thing GROOVE CIRCUIT's config file exists to state -- the
 * loop's own BPM -- has no meaning for a one-shot the deck plays at its own
 * length.
 *
 * THE FIRST BANK ONLY, like the circuit's slots: these belong to the deck rather
 * than to a track, so there is no track to break a tie with if two sticks
 * disagreed about pad 3.
 *
 * Threading. [worker] scans and decodes and is the only writer of the table;
 * [audio] reads it under the acquire/release discipline the stems use, so a
 * re-scan can never free a buffer under a sounding voice.
 */
#include "xpad/xpad.h"
#include "stem/stem.h"
#include "kit/mod.h"

#include <dirent.h>

#define XP_PATH_MAX     STEM_CACHE_PATH_MAX
/* A whole d_name, so nothing is ever truncated: the name is what the DJ reads in
 * the log to check which pad got which file, and a clipped one is worse than a
 * long one. Eight of these is 2 KB against banks measured in megabytes. */
#define XP_NAME_MAX     256

struct xpad_bank {
    int16_t *pcm;                   /* interleaved stereo at the pool rate */
    int64_t  frames;
    char     name[XP_NAME_MAX];     /* the file's own, for the readout */
};

static struct xpad_bank xpad_g_bank[XP_BANKS];
static int  xpad_g_nbank;

/* What the table was built for. A pool rate change or a different stick means
 * the buffers are wrong rather than stale, so both are re-scanned. */
static char xpad_g_root[XP_PATH_MAX];
static int  xpad_g_rate;

/* Readers in the mix. Same shape as the stem store's, and for the same reason:
 * the audio thread must never wait and the worker must never free early. */
static int  xpad_g_readers;
static int  xpad_g_live;

/* ---- the table, as the mix sees it ---------------------------------------- */

int xpad_bank_acquire(int bank, struct xpad_bank_view *out)
{
    if (bank < 0 || bank >= XP_BANKS)
        return 0;
    __atomic_fetch_add(&xpad_g_readers, 1, __ATOMIC_ACQ_REL);
    if (!__atomic_load_n(&xpad_g_live, __ATOMIC_ACQUIRE) || !xpad_g_bank[bank].pcm) {
        __atomic_fetch_sub(&xpad_g_readers, 1, __ATOMIC_ACQ_REL);
        return 0;
    }
    out->pcm    = xpad_g_bank[bank].pcm;
    out->frames = xpad_g_bank[bank].frames;
    return 1;
}

void xpad_bank_release(void)
{
    __atomic_fetch_sub(&xpad_g_readers, 1, __ATOMIC_ACQ_REL);
}

int xpad_bank_ready(int bank)
{
    if (bank < 0 || bank >= XP_BANKS)
        return 0;
    if (!__atomic_load_n(&xpad_g_live, __ATOMIC_ACQUIRE))
        return 0;
    return xpad_g_bank[bank].pcm != NULL;
}

int xpad_bank_count(void)
{
    return __atomic_load_n(&xpad_g_live, __ATOMIC_ACQUIRE) ? xpad_g_nbank : 0;
}

const char *xpad_bank_name(int bank)
{
    if (bank < 0 || bank >= XP_BANKS || !xpad_g_bank[bank].pcm)
        return "";
    return xpad_g_bank[bank].name;
}

/* ---- loading (worker) ----------------------------------------------------- */

/* One decode's worth of state. stem_decode_pull hands over chunks of float and
 * this converts to s16 as they arrive, so the peak is the s16 buffer plus one
 * chunk rather than a whole float copy of the file. */
struct xpad_load {
    int16_t *pcm;
    int64_t  cap;
    int64_t  n;
};

static int xpad_sink(const float *in, int64_t frames, void *user)
{
    struct xpad_load *l = user;
    int64_t i, take = frames;

    if (l->n + take > l->cap)
        take = l->cap - l->n;
    for (i = 0; i < take * 2; i++) {
        float v = in[i] * 32767.0f;

        /* The DJ's own file, at whatever level they made it -- clipped rather
         * than scaled, because quietening someone's sample to fit a headroom
         * they will never see is a decision that belongs to them. */
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        l->pcm[l->n * 2 + i] = (int16_t)v;
    }
    l->n += take;
    return take < frames;          /* full -> stop the decoder */
}

/* Decode one file into `b`. Returns 1 when the bank ends up playable. */
static int xpad_load_one(const char *path, const char *name, int rate,
                         struct xpad_bank *b)
{
    struct xpad_load l;
    int64_t frames;

    /* What the DECODER will produce, which is not the file's own count: the
     * deck's chain pads, and the length is what the buffer has to hold. */
    frames = stem_decode_pull(path, rate, NULL, NULL);
    if (frames <= 0) {
        MDBG("xpad: %s will not decode\n", path);
        return 0;
    }
    if (frames > (int64_t)rate * XP_MAX_SECONDS) {
        MDBG("xpad: %s is %lld frames, truncated to %d s\n",
             path, (long long)frames, XP_MAX_SECONDS);
        frames = (int64_t)rate * XP_MAX_SECONDS;
    }

    l.pcm = malloc((size_t)frames * 2 * sizeof(int16_t));
    if (!l.pcm) {
        MDBG("xpad: no memory for %lld frames of %s\n", (long long)frames, path);
        return 0;
    }
    l.cap = frames;
    l.n   = 0;

    if (stem_decode_pull(path, rate, xpad_sink, &l) < 0 || l.n <= 0) {
        MDBG("xpad: %s decoded nothing\n", path);
        free(l.pcm);
        return 0;
    }
    b->pcm    = l.pcm;
    b->frames = l.n;
    snprintf(b->name, sizeof(b->name), "%s", name);
    return 1;
}

/* Drop the table and wait for the mix to let go of it. */
static void xpad_unload(void)
{
    int i;

    xpad_silence();
    __atomic_store_n(&xpad_g_live, 0, __ATOMIC_RELEASE);
    while (__atomic_load_n(&xpad_g_readers, __ATOMIC_ACQUIRE) > 0)
        usleep(1000);
    for (i = 0; i < XP_BANKS; i++) {
        free(xpad_g_bank[i].pcm);
        xpad_g_bank[i].pcm    = NULL;
        xpad_g_bank[i].frames = 0;
        xpad_g_bank[i].name[0] = '\0';
    }
    xpad_g_nbank = 0;
}

/* ---- the directory --------------------------------------------------------
 *
 * Sorted by name because that is the DJ's only way to say which pad is which,
 * and an insertion sort over eight entries needs no allocation: the list is
 * bounded by the pads, so a directory of a thousand files still costs one pass
 * and eight slots. Files that sort past the eighth are simply not reached.
 *
 * Case-insensitive, so a stick written on a case-preserving filesystem orders
 * the way its owner reads it rather than the way ASCII does. */

static int xpad_name_cmp(const char *a, const char *b)
{
    for (;;) {
        int ca = *a & 0xff, cb = *b & 0xff;

        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
        a++; b++;
    }
}

/* A name a decoder will make something of. The extension list is the decoder's,
 * not ours, so this only screens out the things a media directory always carries
 * -- dotfiles, rekordbox's own .asd analysis siblings -- and leaves the rest to
 * stem_decode_pull, which either produces frames or does not. */
static int xpad_name_wanted(const char *name)
{
    size_t n = strlen(name);

    if (name[0] == '.')
        return 0;
    if (n > 4 && strcmp(name + n - 4, ".asd") == 0)
        return 0;
    return 1;
}

struct xpad_pick { char name[XP_NAME_MAX]; };

/* Keep `name` if it belongs in the first XP_BANKS by name. `n` is how many are
 * held so far; returns the new count. */
static int xpad_pick_insert(struct xpad_pick *pick, int n, const char *name)
{
    int i, j;

    for (i = 0; i < n; i++)
        if (xpad_name_cmp(name, pick[i].name) < 0)
            break;
    if (i >= XP_BANKS)
        return n;                          /* sorts past the last pad */
    if (n < XP_BANKS)
        n++;
    for (j = n - 1; j > i; j--)
        pick[j] = pick[j - 1];
    snprintf(pick[i].name, sizeof(pick[i].name), "%s", name);
    return n;
}

/* Rebuild the table from `root`. Worker thread. */
static void xpad_scan(const char *root, int rate)
{
    struct xpad_pick pick[XP_BANKS];
    char dir[XP_PATH_MAX], path[XP_PATH_MAX];
    struct dirent *de;
    DIR *d;
    int n = 0, i, loaded = 0;

    xpad_unload();
    if ((size_t)snprintf(dir, sizeof(dir), "%s/%s", root, XP_LOOP_DIR) >= sizeof(dir))
        return;

    d = opendir(dir);
    if (!d) {
        /* No directory is a stick with no samples, which is most sticks. Said
         * once per scan rather than silently, because "the pads do nothing" is
         * otherwise indistinguishable from a bug. */
        MDBG("xpad: no %s -> no banks\n", dir);
        snprintf(xpad_g_root, sizeof(xpad_g_root), "%s", root);
        xpad_g_rate = rate;
        __atomic_store_n(&xpad_g_live, 1, __ATOMIC_RELEASE);
        return;
    }
    while ((de = readdir(d)) != NULL)
        if (xpad_name_wanted(de->d_name))
            n = xpad_pick_insert(pick, n, de->d_name);
    closedir(d);

    for (i = 0; i < n; i++) {
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, pick[i].name)
            >= sizeof(path)) {
            MDBG("xpad: %s is too long a path -> skipped\n", pick[i].name);
            continue;
        }
        /* Packed towards pad A: a file that will not decode gives its pad to the
         * next one rather than leaving a hole, because the DJ counts pads from
         * the left and a gap in the middle is a pad that looks broken. */
        if (xpad_load_one(path, pick[i].name, rate, &xpad_g_bank[loaded]))
            loaded++;
    }
    xpad_g_nbank = loaded;

    snprintf(xpad_g_root, sizeof(xpad_g_root), "%s", root);
    xpad_g_rate = rate;
    __atomic_store_n(&xpad_g_live, 1, __ATOMIC_RELEASE);

    MDBG("xpad: %d bank%s from %s at %d Hz\n",
         loaded, loaded == 1 ? "" : "s", dir, rate);
    for (i = 0; i < loaded; i++)
        MDBG("xpad:   %c = %s (%lld frames, %.2f s)\n",
             'A' + i, xpad_g_bank[i].name, (long long)xpad_g_bank[i].frames,
             (double)xpad_g_bank[i].frames / rate);
}

/* Called from the worker's idle branch. Cheap when nothing has moved: one
 * integer compare and one string compare against what the table was built for. */
void xpad_bank_poll(void)
{
    char root[XP_PATH_MAX];
    int rate = stem_pool_rate();

    if (rate <= 0)
        return;                     /* no timeline to decode onto yet */
    if (!stem_media_first_root(root, sizeof(root))) {
        if (xpad_g_root[0]) {
            MDBG("xpad: media gone -> banks dropped\n");
            xpad_unload();
            xpad_g_root[0] = '\0';
            xpad_g_rate = 0;
        }
        return;
    }
    if (rate == xpad_g_rate && strcmp(root, xpad_g_root) == 0)
        return;
    xpad_scan(root, rate);
}
