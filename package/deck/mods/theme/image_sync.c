// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/theme/image_sync.c - bringing one image in line with the theme in force.
 */
#include "theme/image_internal.h"
#include <pthread.h>

struct theme_img_slot g_img[THEME_IMG_MAX];
int    g_img_n;
size_t g_img_bytes;

/* THE TABLE IS WRITTEN FROM TWO THREADS, and a slot's fields only mean anything together.
 *
 * The message thread arrives through drawImage and setFill. The database reply thread
 * arrives through the waveform replyers, which bake an overview per track -- see
 * g_theme_in_bake, which is __thread for exactly that reason. A bake is a FIRST SIGHTING, so
 * it runs img_evict_live, and eviction COMPACTS: img_release moves the last slot into the
 * freed index and zeroes the tail. A message-thread sighting holds a bare index across the
 * fingerprint, the reconcile and the whole map pass, so a concurrent bake can move that slot
 * out from under it.
 *
 * What that costs is `applied` describing one image while `orig` describes another. The
 * buffer then wears a palette the slot does not name, img_reconcile computes map(orig) with
 * the wrong one, calls every pixel the app's, and snapshots OUR OWN OUTPUT as the pristine.
 * The state is closed from there: a lightness inversion on a neutral grey is an involution,
 * so mapping that pristine lands back on the app's own pixels and no test in this file can
 * tell it from an image we never touched -- measured on the artwork placeholder as a buffer
 * holding the deck's exact stock greys against a pristine holding none of them.
 *
 * Held across the whole sighting rather than per field: classification, the identity tests,
 * the copy and the map are one transaction. Nothing inside blocks and nothing re-enters, so
 * a plain mutex is enough, and libpthread is already a DT_NEEDED. */
static pthread_mutex_t g_img_lock = PTHREAD_MUTEX_INITIALIZER;

/* Has a palette-bearing theme been selected at least once? Only ever goes 0 -> 1. */
static int g_img_ever_on;

/* What live content is holding, and a sighting counter to order it by. The clock
 * only has to be monotonic within a run; wrapping after four billion sightings would
 * cost one round of evictions in the wrong order. */
static size_t   g_img_live_bytes;
static uint32_t g_img_clock;

/* Put the pixels back, if what is in the buffer is still our doing.
 *
 * THE INVARIANT: a buffer we stop tracking must not be left wearing our transform. `orig`
 * is the only record of what was underneath, so freeing the copy while the pixels stay
 * mapped destroys that information -- and the next sighting then adopts the buffer afresh,
 * snapshots OUR OWN OUTPUT as the pristine original, and maps it a second time.
 *
 * That is what the browse previews were doing. A page of rows does not fit in the live
 * budget, so the LRU evicts a strip that is about to be drawn again -- and every
 * evict/re-adopt cycle lays ONE MORE PASS of the palette on it. What N passes of WHITE do
 * to a strip is the whole reported symptom, in one table:
 *
 *   passes    0        1        2        3        4        5        6
 *   ground    000000   ffffff   000000   ffffff   000000   ffffff   000000
 *   peaks     ffffff   000000   ffffff   000000   ffffff   000000   ffffff
 *   orange    ffa600   b27400   c9983c   a2792d   b59150   9a7a41   aa8d5a
 *   blue      0055e1   1654bb   3668bb   3963a9   4a6fab   4a6ba0   5573a2
 *
 * The extremes are a 2-cycle, so the BACKGROUND flips cleanly between white and black. The
 * mid-tones are not: they walk into a fixed point, so the INK stays recognisably ink while
 * getting muddier every cycle. "The waveform appears white or black background, with
 * waveform themed... an addition of muddier waveforms (still themed)" is that table read
 * off a screen, and nothing else in this file produces it.
 *
 * It also explains why it would not hold still. The state of a row is its pass COUNT, which
 * is its eviction history -- so it survives a repaint, changes under scrolling, and does not
 * reset when the list is rebuilt by track, key or category. Dark palettes move 3..32 in
 * their worst channel over a second pass against WHITE's 255, which is why five of the seven
 * themes show none of this; and a list short enough to fit in the budget never evicts at
 * all, which is "only on long lists".
 *
 * Ordered before the pin is dropped, because the pin is the whole reason this pointer is
 * safe to follow.
 *
 * Every guard is RE-READ from the object rather than trusted from adoption time, and each
 * one marks a case where writing would be worse than leaving it alone: STOCK means there is
 * nothing of ours in the buffer to put back, and a changed vptr, geometry or buffer address
 * means it is no longer the object the copy describes.
 *
 * MIXED is deliberately NOT declined: its copy has just been reconciled against the buffer,
 * so it describes these pixels, and the transform is still in there to take out. */
static void img_restore(struct theme_img_slot *e)
{
    /* Its own BitmapData: the caller is part-way through filling one in for the image
     * being adopted, and that one still has to be good when we return. */
    struct theme_bitmapdata bd;

    if (e->applied == THEME_APPLIED_STOCK || e->orig == NULL || e->pd == NULL) return;
    if (*(const uintptr_t *)e->pd != THEME_VT_SOFTPIXELDATA) return;
    if (*(int32_t *)((uintptr_t)e->pd + IPD_WIDTH_OFF)  != e->w ||
        *(int32_t *)((uintptr_t)e->pd + IPD_HEIGHT_OFF) != e->h) return;
    if (!theme_bitmap((void *)(uintptr_t)e->pd, e->w, e->h, &bd)) return;
    if ((size_t)bd.line_stride * (size_t)e->h != e->bytes) return;
    /* Last, and the one the others cannot stand in for: everything above can be true of a
     * DIFFERENT image that landed on this address, and writing e->bytes into it is a heap
     * overflow rather than a restore. */
    if (bd.data != e->data) return;
    memcpy(bd.data, e->orig, e->bytes);
    e->applied = THEME_APPLIED_STOCK;
}

/* How many pixels img_ground_themed looks at. It runs on an adoption and after a
 * reconcile, never per frame, so it can afford to read the whole picture rather than an
 * edge of it. */
#define IMG_GROUND_SAMPLES 1024

/* Is this buffer already wearing the source route's ground?
 *
 * Two ARGB surfaces reach the adopted path with the same shape and opposite needs. A
 * beat-grid ruler arrives stock and has to be mapped. A browse preview is BAKED THROUGH THE
 * THEMED COLOUR TABLES, so it arrives with the themed ground already in it -- and the
 * palette is a lightness inversion, so mapping it again turns a white ground black.
 *
 * The two are told apart by the pixels, because shape and format say the same thing about
 * both: whichever of the two grounds covers more of the picture is the one this strip was
 * baked with. The ground is the majority of a strip by a wide margin -- the ink is a band
 * through the middle -- so the count is not close, and a buffer that answers neither falls
 * through to being mapped, which is what it would have got anyway.
 *
 * READ ACROSS THE WHOLE BUFFER, not along its edges. Sampling the top and bottom rows was
 * enough for a sparse strip and wrong for a loud one: a track whose waveform reaches the
 * bottom row all the way across has no ground on that edge to find, so a themed strip
 * scored no better than a stock one and had its white ground mapped to black. Under WHITE
 * the edges cannot settle it even in principle -- a stock PEAK is 0xffffff, which is
 * exactly the themed ground, and peaks are precisely what the top and bottom rows of a
 * loud strip are made of. */
static int img_ground_themed(const struct theme_bitmapdata *bd, int32_t w, int32_t h)
{
    const uint32_t themed = theme_wave_ground();
    const uint32_t stock  = theme_wave_ground_stock();
    int64_t npx = (int64_t)w * (int64_t)h, step, i;
    int hit_themed = 0, hit_stock = 0;

    /* Equal on a theme whose ground stays black. Nothing to tell apart, and nothing at
     * stake either: a second pass over a black ground leaves it black. */
    if (themed == 0 || themed == stock || npx <= 0 || bd->pixel_stride < 3) return 0;
    step = npx > IMG_GROUND_SAMPLES ? npx / IMG_GROUND_SAMPLES : 1;
    for (i = 0; i < npx; i += step) {
        const uint8_t *p = bd->data + (i / w) * (size_t)bd->line_stride
                                    + (i % w) * (size_t)bd->pixel_stride;
        uint32_t c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];

        if (c == themed)     hit_themed++;
        else if (c == stock) hit_stock++;
    }
    return hit_themed > hit_stock;
}

/* Give a slot back.
 *
 * FOUR THINGS, and the last two are the ones that are easy to miss. The pixels go back
 * first -- see img_restore, and it has to happen while the pin still holds. The copy is
 * ours and frees normally. The PIN is not ours to keep: we added one to the refcount so the
 * object could not be recycled under a stale pointer, and putting it back is what
 * lets the app's own release actually reach zero and free the buffer -- otherwise
 * eviction reclaims our 450 KB copy and leaves the app's 450 KB original pinned for
 * ever, which is half a fix. And the SLOT itself goes, rather than being left with
 * chrome cleared: the moment the pin is gone that address can be recycled, and a
 * stale entry matching a new image is exactly the aliasing the pin existed to stop.
 *
 * Swapped with the last, because nothing holds an index across a call -- both scans
 * run to completion inside one sighting. */
static void img_release(int k)
{
    struct theme_img_slot *e = &g_img[k];

    if (e->chrome) {
        img_restore(e);
        g_img_bytes -= e->bytes;
        if (e->live) g_img_live_bytes -= e->bytes;
    }
    /* Keyed on the pin and not on chrome: the two come apart, and a slot that lost its copy
     * while keeping its reference leaks the app's buffer for the life of the process. */
    if (e->pinned)
        __atomic_fetch_sub((int *)((uintptr_t)e->pd + IPD_REFCOUNT_OFF), 1,
                           __ATOMIC_RELAXED);
    free(e->orig);
    g_img_n--;
    if (k != g_img_n) g_img[k] = g_img[g_img_n];
    memset(&g_img[g_img_n], 0, sizeof(g_img[g_img_n]));
}

/* Which pixels of this buffer are still OURS?
 *
 * The only honest way to ask is to compute it: map(orig) is exactly what we left, so a
 * pixel that still equals it was not touched and its pristine is still good, while one
 * that differs is the app's and becomes the new pristine. Per pixel, because the case
 * this exists for interleaves the two in every row.
 *
 * MAPPED WITH THE PALETTE WE USED, not the one selected now -- `applied` names it. Using
 * the current palette after a theme switch would find every pixel "different" and
 * snapshot our own old output as pristine, which is the very thing this prevents.
 * a negative `applied` means we left orig itself in the buffer, so the compare is
 * direct. */
static void img_reconcile(struct theme_img_slot *e, const struct theme_bitmapdata *bd)
{
    const struct theme_palette *was =
        (e->applied >= 0 && e->applied < MOD_THEME_MAX)
            ? k_mod_themes[e->applied].palette : NULL;
    const int has_alpha = (bd->pixel_format == PIXFMT_ARGB);
    const int n = bd->pixel_stride > 4 ? 4 : bd->pixel_stride;
    int32_t x, y;

    for (y = 0; y < e->h; y++) {
        uint8_t *brow = bd->data + (size_t)y * (size_t)bd->line_stride;
        uint8_t *orow = e->orig  + (size_t)y * (size_t)bd->line_stride;

        for (x = 0; x < e->w; x++) {
            uint8_t *bp = brow + (size_t)x * (size_t)bd->pixel_stride;
            uint8_t *op = orow + (size_t)x * (size_t)bd->pixel_stride;
            /* p[3] is only ever read behind has_alpha, so an RGB pixel's fourth byte is
             * zeroed here rather than borrowed from the next pixel's blue. */
            uint8_t mine[4];
            int k;

            mine[0] = op[0]; mine[1] = op[1]; mine[2] = op[2];
            mine[3] = has_alpha ? op[3] : 0;
            if (was) theme_map_pixel(was, mine, has_alpha);

            for (k = 0; k < n; k++)
                if (bp[k] != mine[k]) break;
            if (k == n)
                continue;                          /* still ours: keep the pristine */
            for (k = 0; k < n; k++)
                op[k] = bp[k];                     /* the app wrote here */
        }
    }
}

/* Make room for `want` bytes of live content, oldest sighting first.
 *
 * ONLY LIVE SLOTS ARE CANDIDATES. A sprite evicted here would be re-adopted and
 * re-mapped the next time it is drawn, which is every frame -- the memory back is
 * not worth the work, and it is not what is overflowing anyway.
 *
 * Linear, and that is not worth improving: it runs once per BAKE, which is a track
 * load or a grid change, against a few hundred slots. */
static void img_evict_live(size_t want)
{
    while (g_img_live_bytes + want > THEME_LIVE_BUDGET) {
        uint32_t oldest = 0xffffffffu;
        int k, victim = -1;

        for (k = 0; k < g_img_n; k++)
            if (g_img[k].chrome && g_img[k].live && g_img[k].seen <= oldest) {
                oldest = g_img[k].seen;
                victim = k;
            }
        if (victim < 0)
            return;               /* nothing live left: the caller goes unadopted */
        MDBG("theme: evicting live %dx%d (%zuK, seen %u) for %zuK\n",
             (int)g_img[victim].w, (int)g_img[victim].h, g_img[victim].bytes >> 10,
             g_img[victim].seen, want >> 10);
        img_release(victim);
    }
}


/* Bring one image in line with the theme in force.
 *
 * Does nothing at all until a theme that maps images has been selected at least
 * once -- ORIGINAL never wakes it. Both callers
 * reach here with the mode OFF -- deliberately, because that is how a themed
 * image gets put back -- but on a deck where the mode has never been switched on
 * there is nothing to put back, and cataloguing every image anyway was not free:
 * each first sighting took a table slot, malloc'd and copied the sprite's
 * pixels, bumped the refcount to pin the object for the life of the process, and
 * logged a line. Tens of megabytes and a wall of `theme: image #N` for a feature
 * that was switched off.
 *
 * Deferring costs nothing, because toggling the mode repaints the whole UI --
 * that is the premise of the feature -- so every visible image comes straight
 * back through here with the pixels still stock, which is exactly when the
 * original needs capturing. `ever_on` only ever goes 0 -> 1, so the race with a
 * toggle on another thread costs at worst one extra pass. */
void theme_sync_image(const void *image)
{
    theme_sync_image_x(image, THEME_BLIT_NOW);
}

/* setFill's image, and the baked overviews. See enum theme_blit at the top of the file
 * for why the call site has to say this rather than the classifier work it out. */
void theme_sync_image_deferred(const void *image)
{
    theme_sync_image_x(image, THEME_BLIT_LATER);
}

/* Is this image one we have adopted and left recoloured? The stateless path asks, so the
 * two treatments cannot both land on the same buffer: stashing pixels we already mapped
 * and mapping them again would double the transform, and the restore afterwards would
 * put our own output back where the pristine copy belongs. */
int theme_img_adopted(const void *pd)
{
    int i, r = 0;

    /* Under the table lock like every other reader: the scan walks g_img_n entries a bake on
     * the database reply thread can be compacting. */
    pthread_mutex_lock(&g_img_lock);
    for (i = 0; i < g_img_n; i++)
        if (g_img[i].pd == pd) { r = g_img[i].chrome; break; }
    pthread_mutex_unlock(&g_img_lock);
    return r;
}

/* Is this blit going somewhere we will theme ourselves?
 *
 * See GFXCTX_STATE_OFF for the chain and why walking it cannot fault. The answer is only
 * yes for a destination this file would ADOPT AS A SPRITE, which is the whole point: the
 * question being asked is "will the destination get the palette in its own right", and a
 * surface we never touch has to keep receiving themed pixels or it comes out stock.
 *
 * Sprites only, deliberately. The waveform surfaces have their own arrangement with the
 * source route, and nothing blits a sprite into one. */
static int img_dest_is_ours(const void *ctx, int32_t *pdw, int32_t *pdh)
{
    uintptr_t state = 0, dst = 0, vt = 0;
    int32_t dw = 0, dh = 0, df = 0;

    if (ctx == NULL) return 0;
    if (mod_safe_read((uintptr_t)ctx + GFXCTX_STATE_OFF, &state, sizeof(state)) != 0)
        return 0;
    if (mod_safe_read(state + GFXSTATE_IMAGE_OFF, &dst, sizeof(dst)) != 0)
        return 0;
    if (mod_safe_read(dst, &vt, sizeof(vt)) != 0 || vt != THEME_VT_SOFTPIXELDATA)
        return 0;
    if (mod_safe_read(dst + IPD_PIXELFORMAT_OFF, &df, sizeof(df)) != 0 ||
        mod_safe_read(dst + IPD_WIDTH_OFF,  &dw, sizeof(dw)) != 0 ||
        mod_safe_read(dst + IPD_HEIGHT_OFF, &dh, sizeof(dh)) != 0)
        return 0;
    *pdw = dw;
    *pdh = dh;
    return df == PIXFMT_ARGB && theme_sprite_size(dw, dh);
}

/* Hand ONE blit the sprite's stock pixels, and say whether it has to be undone.
 *
 * THE APP BUILDS SMALL IMAGES OUT OF OTHER IMAGES. A browse row's 48x48 artwork cell is
 * the skin's 50x50 img_noArtWork.png rescaled, and the rescale is a drawImage into a
 * fresh offscreen surface -- so it copies whatever the source holds at that moment. With
 * our transform in the source, the copy is born themed; we then adopt it, take those
 * pixels as its pristine, and map them a second time. On a light theme the palette is a
 * lightness inversion, and on the neutral greys this asset is made of it is an exact
 * involution, so the second pass lands back on the STOCK colours -- pixel for pixel, and
 * consistent with itself for ever after. That is why the cell stayed dark on WHITE while
 * the same artwork was correctly themed everywhere it is drawn straight to the screen.
 *
 * So the derivation gets the stock pixels and the screen keeps the themed ones. The copy
 * is then just another sprite, adopted with a real pristine and mapped exactly once.
 *
 * Cheap because img_dest_is_ours says no to every blit that reaches the display, which is
 * nearly all of them; the table work below only runs on an actual derivation. */
int theme_img_lend_stock(const void *image, const void *ctx)
{
    int32_t dw = 0, dh = 0;
    void *pd;
    int i, lent = 0;

    if (image == NULL || (pd = *(void **)image) == NULL) return 0;
    if (!img_dest_is_ours(ctx, &dw, &dh)) return 0;

    pthread_mutex_lock(&g_img_lock);
    for (i = 0; i < g_img_n; i++) {
        struct theme_img_slot *e = &g_img[i];
        struct theme_bitmapdata bd;

        if (e->pd != pd) continue;
        if (!e->chrome || e->applied == THEME_APPLIED_STOCK) break;
        img_restore(e);                       /* every identity test lives in there */
        if (e->applied != THEME_APPLIED_STOCK) break;   /* it declined: nothing changed */
        /* Stamped, so the sighting that puts the transform back does not read the
         * restore as an app repaint and reconcile a pristine copy against itself. */
        if (theme_bitmap(pd, e->w, e->h, &bd))
            e->stamp = theme_fingerprint(&bd, e->w, e->h);
        lent = 1;
        MDBG("theme: lending %dx%d stock to a %dx%d offscreen\n",
             (int)e->w, (int)e->h, (int)dw, (int)dh);
        break;
    }
    pthread_mutex_unlock(&g_img_lock);
    return lent;
}

static void img_sync_locked(const void *image, enum theme_blit when)
{
    const struct theme_palette *pal;
    void *pd;
    int32_t w, h, fmt;
    struct theme_bitmapdata bd;
    int i, want, on;

    pal = mod_theme()->palette;
    on = pal != NULL ? 1 : 0;
    if (!on && !g_img_ever_on) return;
    g_img_ever_on |= on;

    if (image == NULL) return;
    pd = *(void **)image;                       /* juce::Image is just the Ptr */
    if (pd == NULL) return;

    fmt = *(int32_t *)((uintptr_t)pd + IPD_PIXELFORMAT_OFF);
    w   = *(int32_t *)((uintptr_t)pd + IPD_WIDTH_OFF);
    h   = *(int32_t *)((uintptr_t)pd + IPD_HEIGHT_OFF);

    for (i = 0; i < g_img_n; i++)
        if (g_img[i].pd == pd) break;

    if (i == g_img_n) {                          /* first sight: classify once */
        int ok, live = 0;

        /* Chrome vs artwork is decided by PIXEL FORMAT, not by how colourful the
         * image is. Skin sprites are authored with an alpha channel (ARGB); decoded
         * photos and the waveform buffer are opaque RGB -- observed live as album art
         * 240x240 fmt1 and waveform 1200x128 fmt1, against fmt2 for every sprite.
         * A mean-chroma test was tried first and got this wrong: the beat-loop pads
         * carry an orange border, which lifted their average above the threshold and
         * filed them as artwork, so one row of pads stayed dark while the row without
         * a border themed correctly. Colourfulness does not distinguish a photo from
         * a decorated button; the alpha channel does. */
        /* A LIVE buffer is not a sprite, and the difference is not how it looks -- it
         * is whether the pixels are still the pixels we snapshotted.
         *
         * The waveform strips and beat-grid rulers are wide, short and ARGB, so
         * theme_sprite_size' banner rule (w >= 3h) claims them. Adopting one caches a
         * verdict about pixels that then move: the widget exists before a track loads,
         * so `orig` captures a blank buffer, every later repaint finds applied == want
         * and returns early, and a theme switch memcpy's the stale snapshot over live
         * pixels.
         *
         * So the shape test does not decide adoption; it decides which TREATMENT fits,
         * and the fingerprint below re-reads what is actually in the buffer on every
         * pass so an adoption is only ever as old as the last sighting:
         *
         *   live + NOW    the stateless path owns it. Cheaper (no copy retained, no
         *                 hashing) and right for a buffer redrawn every frame.
         *   live + LATER  nobody can restore these in time, so they are adopted and the
         *                 fingerprint carries the weight. This is the deck's extended
         *                 overview: baked once per track, blitted through a FillType.
         *   sprite        unchanged -- ARGB, small, and genuinely static.
         *
         * theme_is_waveform is reused rather than replaced: it already answers "is this
         * per-frame content", it is deliberately looser than the banner rule (2:1,
         * capped at 256 tall), and one test means the two paths cannot drift apart. */
        {
            int32_t ww, hh; int aa;

            ok = *(uintptr_t *)pd == THEME_VT_SOFTPIXELDATA;
            if (ok) {
                if (theme_is_waveform(pd, &ww, &hh, &aa)) {
                    ok = (when == THEME_BLIT_LATER);
                    /* Remembered on the slot, because eviction has to know which of
                     * these it may take back and the shape alone cannot say -- the
                     * banner rule claims strips and the keyboard backdrop alike. */
                    live = ok;
                } else {
                    ok = (fmt == PIXFMT_ARGB && theme_sprite_size(w, h));
                }
            }
            ok = ok && theme_bitmap(pd, w, h, &bd);
        }

        {
            size_t bytes = ok ? (size_t)bd.line_stride * (size_t)h : 0;

            /* ROOM BEFORE THE SLOT, and this order is load-bearing: img_evict_live
             * compacts the table, so an index chosen ahead of it does not survive --
             * the entry would land past g_img_n and be invisible to every later
             * scan, with a zeroed slot left inside the range pretending to be it.
             *
             * Only ever at the expense of other live content. A new bake supersedes
             * an old one; the old one is a track nobody is looking at any more. */
            if (live && bytes)
                img_evict_live(bytes);

            /* Evicting frees slots as well as bytes, so the ceiling is tested here
             * rather than on the way in -- a table that was full a moment ago may
             * not be one now. */
            if (g_img_n == THEME_IMG_MAX) {      /* never silently: a full table
                                                  * leaves whole panes unthemed */
                static int warned;
                if (!warned) { warned = 1; MWARN("theme: image table FULL at %d -> "
                                                 "later panes stay untouched\n",
                                                 THEME_IMG_MAX); }
                return;
            }
            i = g_img_n;
            memset(&g_img[i], 0, sizeof(g_img[i]));
            g_img[i].pd = pd;
            /* On EVERY entry, refused ones included: this is what a later sighting checks the
             * address against, and a refusal that cannot be re-taken is a permanent wrong
             * answer for whatever lands here next. */
            g_img[i].w = w;
            g_img[i].h = h;
            g_img[i].fmt = (int16_t)fmt;

            if (bytes && (!live || g_img_live_bytes + bytes <= THEME_LIVE_BUDGET) &&
                g_img_bytes + bytes <= THEME_COPY_BUDGET &&
                (g_img[i].orig = malloc(bytes)) != NULL) {
                memcpy(g_img[i].orig, bd.data, bytes);
                g_img[i].bytes = bytes;
                g_img[i].data = bd.data;
                g_img[i].live = (uint8_t)live;
                g_img_bytes += bytes;
                if (live) g_img_live_bytes += bytes;
                __atomic_fetch_add((int *)((uintptr_t)pd + IPD_REFCOUNT_OFF), 1,
                                   __ATOMIC_RELAXED);      /* pin: see img_release */
                g_img[i].pinned = 1;
                g_img[i].chrome = 1;
            } else if (bytes) {
                /* Still worth saying, but it is no longer a countdown to an unthemed
                 * deck: live content evicts its own and the sprites keep their pool,
                 * so reaching this means a skin larger than the budget was sized for. */
                static int warned;
                if (!warned) { warned = 1; MWARN("theme: copy budget spent at %zuK "
                                                 "(live %zuK) -> later sprites stay "
                                                 "untouched\n",
                                                 (size_t)(g_img_bytes >> 10),
                                                 (size_t)(g_img_live_bytes >> 10)); }
            }
        }
        g_img_n++;
        MDBG("theme: image #%d pd=%p %dx%d fmt%d -> %s\n", g_img_n, pd,
             (int)w, (int)h, (int)fmt, g_img[i].chrome ? "chrome" : "left alone");
    }

    /* SAME ADDRESS, DIFFERENT IMAGE, asked before the chrome gate rather than after. The app
     * recycles an ImagePixelData's memory, so a slot only describes the buffer that was there
     * when it was taken -- and a verdict carried over to a new one is wrong whichever way it
     * went: a refusal leaves a themeable surface stock for the life of the process, and an
     * adoption walks the new buffer with the old geometry and memcpy's a copy of the wrong
     * size into it. That last one corrupts the heap. */
    if (w != g_img[i].w || h != g_img[i].h || (int16_t)fmt != g_img[i].fmt) {
        MDBG("theme: image pd=%p was %dx%d fmt%d, now %dx%d fmt%d -- dropping the slot\n",
             pd, (int)g_img[i].w, (int)g_img[i].h, (int)g_img[i].fmt,
             (int)w, (int)h, (int)fmt);
        img_release(i);
        return;
    }

    if (!g_img[i].chrome) return;
    /* Sighted. The eviction order is "least recently drawn", and this is the only
     * place that can say so -- a strip three tracks ago and the one on screen are
     * indistinguishable by anything else on the slot. */
    g_img[i].seen = ++g_img_clock;
    want = on ? __atomic_load_n(&g_theme_id, __ATOMIC_RELAXED) : -1;

    /* The vptr, first of the identity tests below: this object may be neither the one we
     * adopted nor a SoftwarePixelData at all. */
    if (*(uintptr_t *)pd != THEME_VT_SOFTPIXELDATA) return;

    if (!theme_bitmap(pd, g_img[i].w, g_img[i].h, &bd)) return;
    if ((size_t)bd.line_stride * (size_t)g_img[i].h != g_img[i].bytes) return;
    /* The last identity test, and the only one a recycled allocation of the SAME shape cannot
     * pass: a different allocation answers a different address. */
    if (bd.data != g_img[i].data) {
        MDBG("theme: image pd=%p buffer moved %p -> %p -- dropping the slot\n",
             pd, (const void *)g_img[i].data, (const void *)bd.data);
        img_release(i);
        return;
    }

    /* Has the app repainted this since we last left it? If so our pristine copy belongs
     * to something else -- the previous track, most of the time -- and both branches
     * below would use it: the restore would stamp stale pixels over live ones, and the
     * recolour would map the wrong image entirely.
     *
     * The fingerprint only says THAT something moved. Taking the whole buffer as the new
     * pristine because of it is what broke the browse list: on a long list the app
     * RECYCLES a row's buffer, and it draws the next track's waveform over what is
     * already there rather than onto a cleared surface. What it hands back is our themed
     * background with the app's stock ink through it -- and snapshotting that as pristine
     * puts the background through the palette a second time, then a third, compounding
     * every time the row is reused. Which is why the ink came out discoloured whichever
     * state the background had reached, and why it only ever happened on a list long
     * enough for rows to be recycled.
     *
     * So ask the question per PIXEL, and ask it exactly. The map is deterministic and we
     * still hold the pristine, so map(orig) IS what we left there -- anything else is the
     * app's, and only those pixels are re-snapshotted. Ink and background separate
     * cleanly even though they interleave in every row, which is what no amount of
     * banding could do.
     *
     * Behind the fingerprint, so the paused-deck case still costs one 192-sample hash
     * and nothing else; the reconciliation only runs on a buffer that actually moved. */
    {
        uint64_t now = theme_fingerprint(&bd, g_img[i].w, g_img[i].h);

        if (now != g_img[i].stamp) {
            img_reconcile(&g_img[i], &bd);
            /* MIXED, not STOCK. The copy is good again, but our transform is still in the
             * buffer -- and want < 0 reads STOCK as "already put back" and returns without
             * restoring, which leaves the transform on screen under ORIGINAL and lets the
             * next reconcile snapshot it as the original. */
            g_img[i].applied = THEME_APPLIED_MIXED;
        }
    }

    if (g_img[i].applied == want) return;

    /* Left to the source route, which got here first. Recorded as applied so the next sighting
     * costs one fingerprint, and not written at all: these pixels are already the ones we want
     * on screen, and there is no stock copy of them to go back to -- ORIGINAL reverts the
     * tables and the app re-bakes. */
    if (want >= 0 && g_img[i].live && img_ground_themed(&bd, g_img[i].w, g_img[i].h)) {
        g_img[i].applied = (int16_t)want;
        g_img[i].stamp = theme_fingerprint(&bd, g_img[i].w, g_img[i].h);
        return;
    }

    if (want < 0) {
        memcpy(bd.data, g_img[i].orig, g_img[i].bytes);    /* exact restore */
    } else {
        const int has_alpha = (bd.pixel_format == PIXFMT_ARGB);

        memcpy(bd.data, g_img[i].orig, g_img[i].bytes);    /* always map from pristine */
        for (int y = 0; y < g_img[i].h; y++) {
            uint8_t *row = bd.data + (size_t)y * bd.line_stride;
            for (int x = 0; x < g_img[i].w; x++)
                theme_map_pixel(pal, row + (size_t)x * bd.pixel_stride, has_alpha);
        }
    }
    g_img[i].applied = (int16_t)want;
    /* Record what we are leaving behind, so the next sighting can tell our own output
     * from a repaint. Must be after the write, and after BOTH branches -- stamping only
     * the recolour would make every restore look like an app repaint and re-snapshot the
     * pristine copy from pristine pixels, which is harmless but hides real changes. */
    g_img[i].stamp = theme_fingerprint(&bd, g_img[i].w, g_img[i].h);
}

void theme_sync_image_x(const void *image, enum theme_blit when)
{
    pthread_mutex_lock(&g_img_lock);
    img_sync_locked(image, when);
    pthread_mutex_unlock(&g_img_lock);
}
