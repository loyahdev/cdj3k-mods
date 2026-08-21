// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/grid_edit.c - moving, scaling and resetting the deck's beat grid.
 */
#include "stem/grid_internal.h"
#include "cue/cue.h"
#include "db/db.h"
#include "kit/mod.h"

static double grid_pos_at(const int64_t *pos, int32_t n, double x)
{
    int32_t i;
    double f;

    if (n < 2)
        return n ? (double)pos[0] : 0.0;
    if (x <= 0.0)
        return (double)pos[0] + x * (double)(pos[1] - pos[0]);
    if (x >= (double)(n - 1))
        return (double)pos[n - 1] +
               (x - (double)(n - 1)) * (double)(pos[n - 1] - pos[n - 2]);
    i = (int32_t)x;
    f = x - (double)i;
    return (double)pos[i] + f * (double)(pos[i + 1] - pos[i]);
}

static int grid_swap_pair(uintptr_t content, unsigned ptr_off, unsigned cnt_off,
                          uintptr_t arr, int32_t count, int32_t old_count)
{
    if (count > old_count) {
        if (mod_safe_write(content + ptr_off, &arr, sizeof(arr)) != 0 ||
            mod_safe_write(content + cnt_off, &count, sizeof(count)) != 0)
            return -1;
    } else {
        if (mod_safe_write(content + cnt_off, &count, sizeof(count)) != 0 ||
            mod_safe_write(content + ptr_off, &arr, sizeof(arr)) != 0)
            return -1;
    }
    return 0;
}

static int grid_swap(uintptr_t content, uintptr_t beats, int32_t count,
                     int32_t old_count)
{
    return grid_swap_pair(content, CONTENT_BEATS_OFF, CONTENT_COUNT_OFF,
                          beats, count, old_count);
}

static uintptr_t grid_build_bars(int32_t bar0, double k, int32_t n_new,
                                 int32_t *n_out)
{
    int32_t phase, nbar, m;
    uintptr_t blk;
    uint8_t *arr;

    phase = (int32_t)((double)bar0 * k + 0.5) % BEATS_PER_BAR;
    if (phase < 0)
        phase += BEATS_PER_BAR;
    if (phase >= n_new)
        phase = 0;
    nbar = (n_new - 1 - phase) / BEATS_PER_BAR + 1;

    blk = (uintptr_t)malloc((size_t)BAR_STRIDE + (size_t)nbar * BAR_STRIDE +
                            (size_t)BAR_STRIDE);
    if (!blk)
        return 0;
    memset((void *)blk, GRID_RED_LO, BAR_STRIDE);
    arr = (uint8_t *)(blk + BAR_STRIDE);
    memset(arr + (size_t)nbar * BAR_STRIDE, GRID_RED_HI, BAR_STRIDE);
    for (m = 0; m < nbar; m++) {
        int32_t at = phase + m * BEATS_PER_BAR;

        memcpy(arr + (size_t)m * BAR_STRIDE, &at, sizeof(at));
    }
    *n_out = nbar;
    return (uintptr_t)arr;
}

int stem_grid_edit_scale(double k)
{
    uintptr_t content = 0, beats = 0, blk;
    int64_t *orig = NULL;
    double  *obpm = NULL;
    int32_t count = 0, n_new, j;
    uint8_t *cells;
    size_t bytes;

    if (!(k >= GRID_EDIT_K_MIN) || !(k <= GRID_EDIT_K_MAX)) {
        MDBG("grid: rescale %.4f is outside %.3f..%.1f -> refused\n",
             k, GRID_EDIT_K_MIN, GRID_EDIT_K_MAX);
        return -1;
    }
    if (!grid_g_holder ||
        mod_safe_read(grid_g_holder + HOLDER_CONTENT_OFF, &content,
                      sizeof(content)) != 0 || !content ||
        mod_safe_read(content + CONTENT_BEATS_OFF, &beats, sizeof(beats)) != 0 ||
        !beats ||
        mod_safe_read(content + CONTENT_COUNT_OFF, &count, sizeof(count)) != 0 ||
        count < GRID_MIN_BEATS) {
        MDBG("grid: no grid to rescale\n");
        return -1;
    }

    /* The FIRST edit on this content is what RESET goes back to. A later edit
     * rescales the ORIGINAL rather than compounding, so tapping x2 twice is x2
     * and not x4 -- which is what a control with its own RESET means. */
    if (grid_g_orig_content != content) {
        grid_g_orig_content = content;
        grid_g_orig_beats   = beats;
        grid_g_orig_count   = count;
        grid_g_orig_bars    = 0;
        grid_g_orig_barcnt  = 0;
        (void)mod_safe_read(content + CONTENT_BARS_OFF, &grid_g_orig_bars,
                            sizeof(grid_g_orig_bars));
        (void)mod_safe_read(content + CONTENT_BARCNT_OFF, &grid_g_orig_barcnt,
                            sizeof(grid_g_orig_barcnt));
    }
    beats = grid_g_orig_beats;
    count = grid_g_orig_count;

    n_new = (int32_t)(((double)(count - 1) * k) + 0.5) + 1;
    if (n_new < GRID_MIN_BEATS || n_new > GRID_BEATS_MAX) {
        MDBG("grid: rescale by %.4f would give %d beats -> refused\n", k, n_new);
        return -1;
    }

    /* The original, read once into our own memory: the interpolation below
     * reads it n_new times and each mod_safe_read is a syscall. Positions and
     * per-beat BPMs both, because a cell is both. */
    orig = malloc((size_t)count * sizeof(*orig));
    obpm = malloc((size_t)count * sizeof(*obpm));
    if (!orig || !obpm) {
        free(orig);
        free(obpm);
        return -1;
    }
    for (j = 0; j < count; j++) {
        if (mod_safe_read(beats + (uintptr_t)j * BEAT_STRIDE, &orig[j],
                          sizeof(orig[j])) != 0) {
            free(orig);
            free(obpm);
            return -1;
        }
        obpm[j] = grid_beat_bpm(beats, j);
    }

    bytes = BEAT_STRIDE + (size_t)n_new * BEAT_STRIDE + BEAT_STRIDE;
    blk = (uintptr_t)malloc(bytes);
    if (!blk) {
        free(orig);
        free(obpm);
        return -1;
    }
    memset((void *)blk, GRID_RED_LO, BEAT_STRIDE);
    cells = (uint8_t *)(blk + BEAT_STRIDE);
    memset(cells, 0, (size_t)n_new * BEAT_STRIDE);
    memset(cells + (size_t)n_new * BEAT_STRIDE, GRID_RED_HI, BEAT_STRIDE);

    for (j = 0; j < n_new; j++) {
        double  x = (double)j / k;
        int64_t p = (int64_t)(grid_pos_at(orig, count, x) + 0.5);
        /* The cell's own BPM, scaled by the same k -- this is what the deck's
         * readout shows, so leaving it alone makes a rescaled grid read 0.0.
         * Taken from the source beat rather than a constant, so a variable
         * grid keeps its shape here too. */
        int32_t i0 = (int32_t)x;
        double  b;

        if (i0 < 0)          i0 = 0;
        if (i0 > count - 1)  i0 = count - 1;
        b = obpm[i0] * k;

        memcpy(cells + (size_t)j * BEAT_STRIDE, &p, sizeof(p));
        memcpy(cells + (size_t)j * BEAT_STRIDE + BEAT_BPM_OFF, &b, sizeof(b));
    }
    free(orig);
    free(obpm);

    if (grid_swap(content, (uintptr_t)cells, n_new, grid_g_orig_count) != 0) {
        free((void *)blk);
        MDBG("grid: could not publish the rescaled grid\n");
        return -1;
    }

    /* And the bars, which are the same grid counted differently. A Content with
     * no bar array is left without one: the deck answers "no bar" for every
     * beat there already, and inventing a structure it never had is a change
     * nobody asked for. */
    if (grid_g_orig_bars && grid_g_orig_barcnt > 0) {
        int32_t bar0 = 0, nbar = 0;
        uintptr_t bars;

        if (mod_safe_read(grid_g_orig_bars, &bar0, sizeof(bar0)) != 0)
            bar0 = 0;
        bars = grid_build_bars(bar0, k, n_new, &nbar);
        if (!bars)
            MDBG("grid: no memory for the bars -> they stay as they were\n");
        else if (grid_swap_pair(content, CONTENT_BARS_OFF, CONTENT_BARCNT_OFF,
                                bars, nbar, grid_g_orig_barcnt) != 0)
            MWARN("grid: the beats moved but the bars did not\n");
        else
            MDBG("grid: %d bars (was %d, first downbeat was beat %d)\n",
                 (int)nbar, (int)grid_g_orig_barcnt, (int)bar0);
    }

    /* Ours leaks deliberately -- see the header. */
    MDBG("grid: rescaled by %.4f -> %d beats (was %d)\n", k, n_new,
         (int)grid_g_orig_count);
    return 0;
}

int stem_grid_edit_reset(void)
{
    int32_t cur = 0;

    if (!grid_g_orig_content || !grid_g_orig_beats)
        return -1;
    if (mod_safe_read(grid_g_orig_content + CONTENT_COUNT_OFF, &cur,
                      sizeof(cur)) != 0)
        cur = grid_g_orig_count;
    if (grid_swap(grid_g_orig_content, grid_g_orig_beats, grid_g_orig_count,
                  cur) != 0)
        return -1;
    if (grid_g_orig_bars && grid_g_orig_barcnt > 0) {
        int32_t curbar = 0;

        if (mod_safe_read(grid_g_orig_content + CONTENT_BARCNT_OFF, &curbar,
                          sizeof(curbar)) != 0)
            curbar = grid_g_orig_barcnt;
        (void)grid_swap_pair(grid_g_orig_content, CONTENT_BARS_OFF,
                             CONTENT_BARCNT_OFF, grid_g_orig_bars,
                             grid_g_orig_barcnt, curbar);
    }
    MDBG("grid: reset to the deck's own %d beats\n", (int)grid_g_orig_count);
    return 0;
}

static uint64_t grid_map_id(const char *name)
{
    uint64_t h = 0;
    size_t   n = strlen(name);

    while (n--)
        h = ((uint64_t)(unsigned char)name[n] + GRID_MAP_MUL * h) % GRID_MAP_MOD;
    return h;
}

/* The repository cache, from the app's own object map. */
static uintptr_t grid_cache(void)
{
    static struct grid_mapped_ptr cif;
    uintptr_t link = ep122_sym(EP122_TIR_CACHE_LINK), vt = 0;

    if (!link)
        return 0;
    if (!cif.id)
        cif.id = grid_map_id(GRID_CACHE_NAME);
    if (!((grid_link_fn)link)(&cif) || !cif.obj) {
        MDBG("grid: no repository cache under %s (id %llu)\n", GRID_CACHE_NAME,
             (unsigned long long)cif.id);
        cif.obj = 0;
        return 0;
    }
    /* It answered; is it the class we think it is. */
    if (mod_safe_read(cif.obj, &vt, sizeof(vt)) != 0 ||
        vt != ep122_sym(EP122_TIR_CACHE)) {
        MDBG("grid: %s is a %#lx, not a TrackInfoRepositoryCache\n",
             GRID_CACHE_NAME, (unsigned long)vt);
        cif.obj = 0;
        return 0;
    }
    return cif.obj;
}

int stem_grid_edit_save(void)
{
    struct grid_listener_ref lr = { 0, 0, 0 };
    uint8_t   req[16] __attribute__((aligned(8)));
    uint8_t   value[GRID_VALUE_BYTES] __attribute__((aligned(8)));
    uint64_t  tid[2];
    uintptr_t cache, reqid = ep122_sym(EP122_ASYNC_REQUEST_ID), vt = 0, fn = 0;
    uintptr_t content = 0;
    int32_t   count = 0;
    double    bpm;
    int       ok;

    /* Which track, before anything else: a holder we cannot name is one we can
     * edit and must not write. */
    if (!grid_g_holder_tid_lo && !grid_g_holder_tid_hi) {
        MDBG("grid: this grid did not come with a TrackID -> not saved\n");
        return -1;
    }
    if (!grid_g_holder ||
        mod_safe_read(grid_g_holder + HOLDER_CONTENT_OFF, &content,
                      sizeof(content)) != 0 || !content ||
        mod_safe_read(content + CONTENT_COUNT_OFF, &count, sizeof(count)) != 0 ||
        count < GRID_MIN_BEATS) {
        MDBG("grid: no grid to save\n");
        return -1;
    }
    if (mod_safe_read(grid_g_holder + GRID_VALUE_OFF, value, sizeof(value)) != 0)
        return -1;

    cache = grid_cache();
    if (!cache || !reqid)
        return -1;
    if (mod_safe_read(cache, &vt, sizeof(vt)) != 0 ||
        mod_safe_read(vt + GRID_TIR_REGISTER, &fn, sizeof(fn)) != 0 || !fn) {
        MDBG("grid: the cache has no registerBeatGrid\n");
        return -1;
    }

    ((grid_reqid_fn)reqid)(req);
    tid[0] = grid_g_holder_tid_lo;
    tid[1] = grid_g_holder_tid_hi;

    /* Posts a task and returns; the write happens on the repository's own
     * thread, which is why our replacement beat array is never freed. */
    ok = ((grid_register_fn)fn)(cache, req, tid, value, &lr, GRID_SAVE_PRIO);
    MDBG("grid: register %d beats for track %016llx%016llx -> %s\n", (int)count,
         (unsigned long long)tid[0], (unsigned long long)tid[1],
         ok ? "accepted" : "REFUSED");

    /* The library's own copy of the tempo, which the grid does not reach. The
     * content id is the TrackID's third word -- the low half of its second
     * doubleword. */
    bpm = stem_grid_bpm();
    if (bpm > 0.0)
        (void)mod_djdb_set_bpm((uint32_t)tid[1],
                               (int)(bpm * 100.0 + 0.5));
    return ok ? 0 : -1;
}
