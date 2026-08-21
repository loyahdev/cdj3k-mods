// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * theme.c - the setFill choke point, and install.
 *
 * Why not the obvious routes
 * --------------------------
 * JUCE's LookAndFeel is not the lever here: the app subclasses it only for
 * table headers and one slider, so LookAndFeel::setColour would leave the deck
 * and utility screens untouched. The real skin is Pioneer's own -- layout rows
 * in skin16/layout named `kind_#rrggbb_id`, where the SAME name string is also a
 * literal in .rodata and the lookup is an exact match on the full name, hex
 * included. Patching it works (verified live), but it needs BOTH sides changed
 * in lockstep, the CSVs live on the ramdisk, and the colour is resolved at
 * widget construction -- so it costs an app restart. Not a hot swap.
 *
 * Mechanism
 * ---------
 * Every colour the UI ever paints -- rect fills, paths, and text glyphs alike --
 * funnels through one pure-virtual choke point:
 *
 *     juce::LowLevelGraphicsContext::setFill (const FillType&)
 *
 * The app renders with juce::LowLevelGraphicsSoftwareRenderer, where setFill is
 * virtual index 18. Repointing that one slot recolours the entire interface on
 * the next repaint -- which is what makes switching themes live.
 *
 * juce::FillType (JUCE 5.3.2, layout confirmed against the stock setFill):
 *     +0x00  Colour colour        (uint32 ARGB)   <- all we touch
 *     +0x08  unique_ptr<ColourGradient> gradient
 *     +0x10  Image image
 *     +0x18  AffineTransform transform (6 floats)
 *     sizeof == 0x30
 * setFill deep-copies the gradient and ref-counts the image out of the object it
 * is handed, and never takes ownership of it, so we can pass a byte-copy on the
 * stack. That keeps us from writing through the caller's `const FillType&`,
 * which may legitimately live in read-only memory.
 *
 * Images go through image.c: blitted sprites and the waveform via drawImage, and
 * tiled textures via the Image carried inside the FillType itself. Gradients are
 * not handled -- they keep their colours inside the ColourGradient object.
 */
#include "theme/theme.h"
#include "stem/stem.h"
#include "kit/menu.h"
#include "kit/mod.h"

#define FILLTYPE_SIZE      0x30u
#define FILLTYPE_GRAD_OFF  0x08u   /* unique_ptr<ColourGradient> inside FillType */
#define FILLTYPE_IMAGE_OFF 0x10u   /* juce::Image (a bare Ptr) inside FillType */

/* juce::ColourGradient (5.3.2), read off the running deck rather than assumed:
 *     +0x00  Point<float> point1
 *     +0x08  Point<float> point2
 *     +0x10  bool isRadial
 *     +0x18  ColourPoint* elements
 *     +0x20  int numAllocated
 *     +0x28  int numUsed
 *     ColourPoint { double position; Colour colour; }   == 16 bytes, colour at +8
 *
 * numUsed is at +0x28, NOT the +0x24 that counting the fields suggests: Array holds
 * ArrayAllocationBase by value and that struct pads out to 16 bytes, so Array's own
 * numUsed starts a full 16 in. The wrong offset reads the padding and yields 0 -- a
 * gradient with three visible stops reporting none is what caught it. */
#define GRAD_ELEMS_OFF     0x18u
#define GRAD_NUMALLOC_OFF  0x20u
#define GRAD_NUMUSED_OFF   0x28u
#define GRAD_POINT_SIZE    16u
#define GRAD_POINT_COLOUR  8u
/* Stops are stashed on the stack to be put back. Three is what the waveform uses and
 * JUCE gradients are small by nature; a gradient with more than this is left alone
 * rather than half-transformed. */
#define GRAD_MAX_STOPS     16

typedef void (*setfill_t)(void *ctx, const void *fill);

static uintptr_t g_orig_setfill;

/* Gradient accounting: how many fills carried one, how many looked like a ColourGradient,
 * how many we actually substituted. seen>0 with sub==0 says the validation is throwing
 * them out; sub>0 with no visible change says the substitution is not what paints them. */
static unsigned g_grad_seen, g_grad_ok, g_grad_sub;

/* ---- who paints the overview ----
 *
 * Counts fills per DRAWING FUNCTION. The strip is hundreds of columns painted in one
 * burst, so its painter sits far above the handful of fills a button or a row costs.
 *
 * The depth is the whole trick, and getting it wrong is what wasted the first attempt.
 * A watchpoint on the strip's own framebuffer bytes caught the real call shape:
 *
 *     WidgetBase::paint -> <the widget's draw> -> Graphics::fillRect -> context fillRect
 *
 * so from this wrapper: depth 0 is inside juce::Graphics::fillRect, depth 1 is the
 * widget's draw, depth 2 is WidgetBase::paint. Depth 2 is shared by EVERY widget in the
 * app, so attributing there collapsed all of them into one bucket and guaranteed that no
 * individual painter could stand out -- which is exactly what happened, and it read as
 * "nothing paints the strip" rather than as a broken measurement. Depth 1 or nothing. */
static uintptr_t g_orig_fillrect, g_orig_fillrectf, g_orig_fillrectlist, g_orig_fillpath;

/* One step up the AArch64 frame chain per level: x29 holds the caller's frame pointer and
 * the return address sits beside it. Checked for alignment and range at every hop -- a
 * bad chain here would fault inside the app's paint, not just spoil a log line.
 *
 * ALWAYS INLINE, and called from the wrapper rather than from census(): the depth is
 * measured from whatever frame this expands into, so a helper that the compiler chooses
 * to keep as a real function silently shifts every level by one. That is not theoretical
 * -- factoring the counting into census() did exactly that and moved the whole census one
 * frame down onto the juce::Graphics helpers, which look plausible and are useless. */
static inline __attribute__((always_inline)) uintptr_t frame_lr(int depth)
{
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t lr = 0;
    int i;

    for (i = 0; i <= depth; i++) {
        if (fp < 0x10000u || (fp & 15u)) return 0;
        lr = *(const uintptr_t *)(fp + 8);
        fp = *(const uintptr_t *)fp;
    }
    return lr;
}

static struct { uintptr_t lr; unsigned n; uint8_t tag, fired; } g_who[128];
static int g_who_n, g_who_burst;
static const char *const k_who_tag[] = { "rect", "rectf", "rectlist", "path" };

static void census(int tag, uintptr_t lr)
{
    int i;

    /* EP122 only. Our own stem-row checkerboard is hundreds of fills in one go and kept
     * tripping the trigger, zeroing the window each time. mod_drawing() does not cover it
     * (that bracket spans only setColour), so the test is the address: the app is non-PIE
     * at 0x400000 and the shim is mapped up at 0xffff......... */
    if (lr < 0x400000u || lr > 0x3000000u) return;

    for (i = 0; i < g_who_n; i++)
        if (g_who[i].lr == lr && g_who[i].tag == tag) break;
    if (i == g_who_n && g_who_n < (int)(sizeof(g_who) / sizeof(g_who[0]))) {
        g_who[g_who_n].lr = lr; g_who[g_who_n].n = 0;
        g_who[g_who_n].tag = (uint8_t)tag; g_who_n++;
    }
    if (i >= g_who_n) return;
    g_who[i].n++;

    /* CUMULATIVE, and never reset. The burst-triggered version clipped every painter at
     * the trigger threshold: with several widgets each drawing ~150 fills, the first to
     * reach it zeroed the window, so a painter doing 400 or 1200 in one pass was
     * indistinguishable from one doing 150. Totals over time separate them by an order of
     * magnitude instead -- and a per-column strip repainted on every track load pulls
     * away from a button that fills once. */
    if (((++g_who_burst) & 0x1fff) == 0) {
        int j;

        MDBG("theme: --- draw totals @%d ---\n", g_who_burst);
        for (j = 0; j < g_who_n; j++)
            if (g_who[j].n >= 200)
                MDBG("theme: %s draw %#lx x%u\n", k_who_tag[g_who[j].tag],
                     (unsigned long)g_who[j].lr, g_who[j].n);
    }
}

static void wrap_fillrect(void *ctx, const void *a, int r, int64_t v)
{
    census(0, frame_lr(1));
    ((void (*)(void *, const void *, int, int64_t))g_orig_fillrect)(ctx, a, r, v);
}
static void wrap_fillrectf(void *ctx, const void *a)
{
    census(1, frame_lr(1));
    ((void (*)(void *, const void *))g_orig_fillrectf)(ctx, a);
}
static void wrap_fillrectlist(void *ctx, const void *a)
{
    census(2, frame_lr(1));
    ((void (*)(void *, const void *))g_orig_fillrectlist)(ctx, a);
}
static void wrap_fillpath(void *ctx, const void *p, const void *xf)
{
    census(3, frame_lr(1));
    ((void (*)(void *, const void *, const void *))g_orig_fillpath)(ctx, p, xf);
}

static void wrap_setfill(void *ctx, const void *fill)
{
    /* A FillType can carry an IMAGE as well as a colour -- that is how the tiled
     * textures are painted (the beat-loop pads and friends; cf. meow::TiledImageCache).
     * Those pixels never reach drawImage, so remapping FillType::colour alone left the
     * pads stubbornly dark. Sync it here instead. Gated on the pointer being non-null,
     * which almost every fill is, so the table lookup stays off the hot path; and
     * theme_sync_image itself returns immediately until a pixel theme has been on.
     *
     * _deferred, because a FillType is not consumed here: setFill only ARMS the context,
     * and the pixels are read by the fillRect/fillPath that follows. Nothing this hook
     * does can be undone before then, so an image arriving on this path has to be left
     * recoloured -- which is the difference between this and drawImage, and the reason
     * the deck's extended overview (baked once per track, blitted as a FillType) was the
     * one waveform surface no amount of work on the drawImage side could reach. */
    if (fill != NULL && *(void *const *)((const uint8_t *)fill + FILLTYPE_IMAGE_OFF))
        theme_sync_image_deferred((const uint8_t *)fill + FILLTYPE_IMAGE_OFF);

    /* Borrow this hook as the audio side's heartbeat. stem/audio.c may only
     * print from the message thread -- MDBG can block on a journald-drained stderr,
     * and doing that on the audio or loader thread stalls track loading -- but the
     * obvious message-thread hook (the waveform title bar's paint) barely repaints
     * during playback, so the counters were never being drained. Every repaint comes
     * through setFill, so this is the one place guaranteed to tick.
     *
     * Gated on a plain counter first: setFill runs thousands of times per frame and
     * mod_stem_audio_report() starts with an isb+mrs, which is not something to put
     * in that loop unthrottled. The report throttles again on its own 3 s window. */
    {
        static unsigned tick;
        if (((++tick) & 0x3ff) == 0) {
            MTRACE("theme: setfill %u  grad seen=%u ok=%u sub=%u\n",
                 tick, g_grad_seen, g_grad_ok, g_grad_sub);
            mod_stem_audio_report();
            mod_stem_decode_report();
            theme_memo_report();
            /* Cheap enough to sit here: eight word compares per table, and nothing
             * written unless one has gone stock. This is what carries a theme switch
             * and anything that rebuilds a table outside a waveform reply. */
            theme_wave_apply();
        }
    }

    {
        const struct theme_palette *pal = mod_theme()->palette;

        /* Our own controls are already painted in theme-correct colours -- they asked
         * mod_ui() for a role, which resolved through this same palette once. Running
         * them through it again here is not a near-miss, it is the transform applied
         * twice: an inverted lightness inverts back, a duotone tints a tint. So the draw
         * kit brackets its calls and we chain straight through.
         *
         * Cheap and leak-proof by construction rather than by discipline: the flag only
         * ever spans one synchronous call into juce (see mod_draw_enter), so there is no
         * path where it is left set with the app's own painting still to come. */
        if (mod_drawing()) {
            ((setfill_t)g_orig_setfill)(ctx, fill);
            return;
        }

        if (pal == NULL || fill == NULL) {
            ((setfill_t)g_orig_setfill)(ctx, fill);
            return;
        }

        /* Byte-copy so we never write through the caller's const FillType&. */
        uint8_t copy[FILLTYPE_SIZE];
        memcpy(copy, fill, FILLTYPE_SIZE);
        *(uint32_t *)copy = theme_palette_argb(pal, *(const uint32_t *)fill, 1);

        /* ---- gradients ----
         *
         * A gradient fill IGNORES FillType::colour and takes its pixels from the stops
         * inside the ColourGradient, so mapping the colour above does nothing for one.
         * That was the whole of the BLUE/RGB overview waveform never theming: the deck
         * draws it as ~1200 per-column gradient fills (measured -- switching WAVEFORM
         * COLOR to RGB costs +1200 gradient setFills, to 3BAND +1), while 3BAND ships a
         * prebaked image through drawImage and so themed correctly all along. Same
         * widget slot, two entirely different routes.
         *
         * The byte-copy above does not help by itself: it copies the POINTER, so `copy`
         * still refers to the caller's ColourGradient.
         *
         * We do NOT write into that object. The first attempt did -- transform the stops
         * in place, let setFill copy, put the originals back -- and it took the deck down
         * with a SIGSEGV. That approach needs two things to hold for EVERY gradient the
         * app ever sets, not just the waveform's: that these offsets describe it, and
         * that setFill has finished with it by the time we restore. Neither was proven,
         * and the cost of being wrong is a write into somebody else's heap.
         *
         * So build our own gradient instead and point the copy at it. Reads of the app's
         * object stay reads; every write lands in our own buffer. If anything about the
         * object fails to look like a ColourGradient we simply leave the pointer alone
         * and the fill goes through untouched -- unthemed, which is what it already was.
         *
         * Safe to hand over a static: setFill copies what it is given (the whole file
         * depends on that already -- `copy` is a stack temporary too), and painting is
         * the message thread's alone. */
        {
            const uint8_t *gr = *(const uint8_t *const *)
                                    ((const uint8_t *)fill + FILLTYPE_GRAD_OFF);

            if (gr != NULL) g_grad_seen++;
            if (gr != NULL) {
                const uint8_t *el = *(const uint8_t *const *)(gr + GRAD_ELEMS_OFF);
                int nused  = *(const int *)(gr + GRAD_NUMUSED_OFF);
                int nalloc = *(const int *)(gr + GRAD_NUMALLOC_OFF);

                /* Does this actually look like a ColourGradient? The positions are the
                 * strong test and the reason it is worth doing: a real gradient's stops
                 * run 0..1 and never go backwards, and arbitrary memory read as doubles
                 * almost never does that. The counts alone would not have caught a
                 * wrong-layout object; a monotonic 0..1 double sequence is hard to fake. */
                int ok = el != NULL && nused >= 2 && nused <= GRAD_MAX_STOPS &&
                         nalloc >= nused;

                if (ok) {
                    double prev = -1.0;
                    for (int s = 0; s < nused; s++) {
                        double p = *(const double *)(el + s * GRAD_POINT_SIZE);
                        if (!(p >= 0.0 && p <= 1.0 && p >= prev)) { ok = 0; break; }
                        prev = p;
                    }
                }

                if (ok) g_grad_ok++;
                if (ok) {
                    /* sizeof(ColourGradient) -- header through numUsed, rounded up. */
                    static uint8_t mine[0x30];
                    static uint8_t stops[GRAD_MAX_STOPS * GRAD_POINT_SIZE];

                    memcpy(mine, gr, sizeof(mine));
                    memcpy(stops, el, (size_t)nused * GRAD_POINT_SIZE);
                    for (int s = 0; s < nused; s++) {
                        uint32_t *c = (uint32_t *)(stops + s * GRAD_POINT_SIZE +
                                                   GRAD_POINT_COLOUR);
                        *c = theme_palette_argb(pal, *c, 1);
                    }
                    /* Our own array, and numAllocated must describe IT rather than the
                     * larger block the app may have reserved -- a copy sized from a
                     * capacity we do not own would read off the end of `stops`. */
                    *(uint8_t **)(mine + GRAD_ELEMS_OFF)  = stops;
                    *(int *)(mine + GRAD_NUMALLOC_OFF)    = nused;
                    *(int *)(mine + GRAD_NUMUSED_OFF)     = nused;
                    *(const uint8_t **)(copy + FILLTYPE_GRAD_OFF) = mine;
                    /* Who is drawing these? The substitution provably happens and provably
                     * changes nothing on screen, so the next question is what these 1200
                     * fills actually are. EP122 is non-PIE at 0x400000, so a return
                     * address is directly usable against the binary. */
                    if (g_grad_sub < 3)
                        MDBG("theme: grad sub#%u from %p, stop0 %08x\n", g_grad_sub,
                             __builtin_return_address(0),
                             *(const uint32_t *)(stops + GRAD_POINT_COLOUR));
                    g_grad_sub++;
                }
            }
        }

        ((setfill_t)g_orig_setfill)(ctx, copy);
    }
}

/* ================================================================== */
/* Install                                                            */
/* ================================================================== */

/* The MOD SETTINGS row. Its values ARE the registry -- filled from it at install
 * rather than written out again -- so adding a theme in presets.c puts it in the
 * menu with nothing to change here. `state` is an index into k_mod_themes, and a
 * theme is resolved at draw time, so switching needs no notification. */
static const char *k_theme_values[MOD_THEME_MAX];

static const struct kit_row k_rows[] = {
    { .label = "THEME", .idx = KIT_IDX_THEME, .state = &g_theme_id,
      .values = k_theme_values, .nvalues = MOD_THEME_MAX },
};

static int theme_install(void)
{
    int images, i;

    if (mod_patch_vslot("setFill", EP122_GFX_RENDERER, THEME_SLOT_SETFILL,
                        (void *)wrap_setfill, &g_orig_setfill) != 0) {
        MDBG("theme: setFill hook unavailable -> themes disabled\n");
        return -1;
    }

    /* Diagnostic, and deliberately not fatal: it names the code that paints the overview
     * strip. Nothing else depends on it. */
    /* Diagnostic, deliberately not fatal: names the code that paints the overview strip. */
    if (MLOG_AT(MOD_LOG_DEBUG)) {
        mod_patch_vslot("fillRect", EP122_GFX_RENDERER, THEME_SLOT_FILLRECT,
                        (void *)wrap_fillrect, &g_orig_fillrect);
        mod_patch_vslot("fillRectF", EP122_GFX_RENDERER, THEME_SLOT_FILLRECTF,
                        (void *)wrap_fillrectf, &g_orig_fillrectf);
        mod_patch_vslot("fillRectList", EP122_GFX_RENDERER, THEME_SLOT_FILLRECTLIST,
                        (void *)wrap_fillrectlist, &g_orig_fillrectlist);
        mod_patch_vslot("fillPath", EP122_GFX_RENDERER, THEME_SLOT_FILLPATH,
                        (void *)wrap_fillpath, &g_orig_fillpath);
    }

    /* Images are a bonus: without them the vector chrome still re-themes, so a
     * failure there degrades rather than disables. */
    images = theme_image_install() == 0;

    for (i = 0; i < MOD_THEME_MAX; i++)
        k_theme_values[i] = mod_theme_name(i);
    kit_menu_add(k_rows, (int)(sizeof(k_rows) / sizeof(k_rows[0])));

    MDBG("theme: installed (theme=%s images=%s, live-switchable)\n",
         mod_theme()->name, images ? "on" : "off");
    return 0;
}

KIT_MOD(k_mod_theme,
        .name = "theme", .prio = 70, .install = theme_install,
        .what = "live re-theme via setFill");
