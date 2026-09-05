// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * wave.c - theme the detailed waveform where its colours are DECIDED.
 *
 * Everything else in this directory recolours pixels. That is the right answer for a
 * PNG, whose colours were chosen by whoever drew it, and the wrong one for the detailed
 * waveform, which the deck COMPUTES: docs/waveform-3band-re.md has the column format, and
 * a column carries a colour INDEX naming a set of bands rather than a colour. Somewhere
 * an index becomes an RGB, and that somewhere is a table with eight entries in it.
 *
 * The cost of not knowing that, measured on a playing deck: the strip is 1280x190 --
 * 243,200 pixels blitted once per frame -- and holds 21 distinct colours. That is
 * 16,400,000 palette lookups a second (counted, against 1,700 setFills) to produce
 * twenty-one answers, and on a real CDJ-3000 it was 31.8% of the whole process. Theming
 * the table instead is eight writes per track load, which is what ORIGINAL costs: nothing.
 *
 * ---- the table ----
 *
 * Eight juce::Colour at 8-byte stride (four bytes of colour, four of padding), sitting
 * inside a larger colour array. Read off the running deck:
 *
 *   +0x00  ffffffff   peaks            +0x20  ffd2dcfa
 *   +0x08  ffffa600   orange ink       +0x28  ffb4690a
 *   +0x10  ff0055e1   blue ink         +0x30  fff5ebd7
 *   +0x18  fffff0d7                    +0x38  ff000000   THE GROUND
 *
 * The ground is not a detail. It is 72% of the strip's pixels, so a theme that maps the
 * ink and leaves it stock puts a black rectangle in the middle of a light UI -- and it is
 * exactly the entry that a per-pixel transform used to handle for free, which is how it
 * would go missing.
 *
 * The other 13 colours the census counts are anti-aliased blends BETWEEN these eight, so
 * they follow without being named.
 *
 * ---- finding it ----
 *
 * The values are CONSTRUCTED AT RUN TIME, not stored: searching the binary for 0055e1 or
 * ffa600 finds nothing at all, which cost an hour before it was believed. They live in
 * the anonymous rw mapping after .data, and the only way to find them is to look for the
 * run itself -- which is why the stock group below is spelled out. It is a measurement
 * off 3.19, so a firmware that moves it makes the scan find zero sites and say so, rather
 * than quietly leaving the waveform stock.
 *
 * 185 copies exist. Only ONE draws the strip on screen -- established by writing a
 * DIFFERENT tag colour into every site in one pass and decoding which tag appeared, one
 * screenshot instead of a bisection -- but patching all of them changed nothing else on
 * the play screen, and picking the live one out would mean tracking widget instances for
 * no gain. So all of them get the same treatment.
 *
 * ---- keeping it ----
 *
 * THE APP REBUILDS THE TABLE ON EVERY TRACK LOAD. Verified: a tagged entry reads back
 * stock after a track change. So this is not a patch that is applied once, and the
 * re-assertion has to be idempotent under a rebuild that can happen at any moment.
 *
 * It is made idempotent by being STATELESS. A site is written when it holds the stock
 * group, or when it holds THE GROUP WE LAST WROTE; anything else is not ours and is not
 * touched. There is no "have I done this" flag to get out of step with a rebuild, and no
 * path on which the palette is applied twice.
 *
 * The second half of that test is not optional, and leaving it out is a bug that only
 * appears on the SECOND theme: switching WHITE -> MOCHA leaves every site holding WHITE's
 * colours, which are neither stock nor the new theme's, so a stock-only test walks past
 * all of them and the waveform stays on the previous palette until the next track load.
 * Caught on screen -- the strip read #1654bb (WHITE's blue) while the overview beside it
 * had already moved to MOCHA's #005fe0.
 *
 * ---- the ground ----
 *
 * The eight colours are the INK. The strip's background is not among them: entry 7 is
 * 0xff000000, and mapping it changes nothing on screen -- checked rather than assumed,
 * and checked twice, the second time by poking the live entry to a colour loud enough
 * that the fill below could not have hidden it. The region above the waveform is painted
 * black from somewhere the palette does not reach. Entry 7 is not unused, though: at
 * de-zoom it turns up as a column colour like any other.
 *
 * That matters because the ground is 72% of the strip's pixels. It comes from
 * renderBackground, which writes one colour per column into the array renderWaveform_new
 * lerps against, and that is where it is themed -- see wrap_render_bg. Nothing walks the
 * finished strip putting it back, and nothing should: a pass that did had to key on exact
 * black to know what was background, which ATE PEAKS, since the peak ink themes to
 * 0x010101 and a blend of it rounds to exact black.
 *
 * ---- the contour ----
 *
 * The dark edge that crawls along the waveform on a light theme is neither anti-aliasing
 * nor a resample. draw_new picks one of seven solid colours per column per row and blends
 * nothing at all; the blend is one step earlier, in renderWaveform_new, which lerps
 * between the source columns falling under a single output pixel. Its two endpoints are
 * where the black gets in, and neither is the palette:
 *
 *   renderBackground   one colour per column, and every entry read 0x00000000 on a
 *                      running deck. THIS is the contour -- the colour the outer edge is
 *                      averaged against, and fixing it is what removed the crawl. See
 *                      wrap_render_bg.
 *   the provider       a band a column does not have comes back as a default-constructed
 *                      juce::Colour. Its out pointer is in x8, which C cannot name, so it
 *                      is wrapped by an assembly stub. See WAVE_PROVIDER_OFF.
 *
 * Both are fixed where they start, and nothing here walks the finished pixels.
 *
 * A per-pixel pass over the strip CANNOT do this job, and one shipped that tried. Half the
 * fringe colours are blends between two INKS -- already right, both ends themed -- and by
 * the time they reach the buffer they are indistinguishable from the blends toward black
 * that are wrong. Every rule that tried to separate them lightened interior colour
 * boundaries too, which reads on the deck as a flicker through the middle of the waveform.
 *
 * A theme whose ground stays black skips all of it -- then black IS the ground, and the
 * deck's own blend was right all along.
 *
 * Everything here is [message]: the juce UI thread.
 */
#include "theme/theme.h"
#include "stem/stem.h"

#define WAVE_N        8               /* colours in the group                    */
#define WAVE_STRIDE   2               /* uint32 words per entry: colour + pad    */
#define WAVE_WORDS    (WAVE_N * WAVE_STRIDE)
#define WAVE_BYTES    (WAVE_WORDS * 4)

/* Measured off EP122 3.19. See the header: this doubles as the search key, so it is not
 * merely documentation of what the deck happens to use -- it is how the table is found. */
static const uint32_t k_wave_stock[WAVE_N] = {
    0xffffffffu, 0xffffa600u, 0xff0055e1u, 0xfffff0d7u,
    0xffd2dcfau, 0xffb4690au, 0xfff5ebd7u, 0xff000000u,
};

/* Sized well clear of the 185 seen on a deck with every pane opened: a slot is a pointer,
 * so the headroom is free, and overflowing would leave part of the UI on the slow path
 * with nothing to say why. */
#define WAVE_MAX_SITES 512

static uint32_t *g_site[WAVE_MAX_SITES];
static int       g_site_n;
static int       g_scanned;

/* What the current palette turns the group into. Recomputed when the theme changes, and
 * the ONLY thing ever written to a site -- see the header on why that is what makes the
 * re-assertion safe to run at any time. */
static uint32_t  g_themed[WAVE_N];
static int       g_themed_id = -1;

/* The themed ground, and the one switch that arms everything downstream of the palette:
 * the provider wrappers and the ground fill both read it, and zero -- a dark theme, or
 * ORIGINAL -- makes both of them do nothing. Defined here because theme_wave_apply sets
 * it well before either is defined. */
static uint32_t  g_ground;
static void      wave_provider_install(void);

/* The live palette, for the two modes whose inks cannot be themed at rest. Set beside
 * g_ground so the two are never out of step. */
static const struct theme_palette *g_pal;

/* WHAT WE LAST WROTE, which is not the same question as what we want to write now. A site
 * still carrying the previous theme's colours is ours to update; without this it matches
 * neither stock nor the new theme and would be left behind. See the header. */
static uint32_t  g_written[WAVE_N];
static int       g_written_ok;

/* WHICH STYLE LAST REPLIED. A WAVEFORM COLOR change swaps in a different widget, which
 * may bring a table that did not exist when we scanned; noticing the style change is a
 * precise trigger for a rescan and costs nothing, where rescanning on a timer would pay
 * for a 58 MB walk over and over to catch an event that happens by hand. */
static int g_style = -1;

/* ---- the scan ------------------------------------------------------------ */

/* Anonymous, writable, and not the heap. The group lives in the mapping that follows
 * .data -- 58 MB on this build -- while the heap is hundreds of megabytes of allocator
 * churn that cannot hold a static colour array and would dominate the walk. */
static int wave_region_wanted(const char *line, uintptr_t *lo, uintptr_t *hi)
{
    unsigned long a, b;
    char perms[8], rest[256];
    int n;

    rest[0] = '\0';
    n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &a, &b, perms, rest);
    if (n < 3) return 0;
    if (perms[0] != 'r' || perms[1] != 'w') return 0;
    if (rest[0] != '\0') return 0;                  /* named: file, heap, stack, vdso */
    if (b <= a || b - a > (256u << 20)) return 0;
    *lo = a; *hi = b;
    return 1;
}

static void wave_note(uint32_t *p)
{
    int i;

    for (i = 0; i < g_site_n; i++)
        if (g_site[i] == p) return;
    if (g_site_n < WAVE_MAX_SITES) g_site[g_site_n++] = p;
}

/* Walk every candidate mapping for the stock group. Only the first word is compared in
 * the inner loop -- 0xffffffff is common enough to be a cheap filter and rare enough that
 * the full compare below runs almost never. */
static void wave_scan(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int before = g_site_n;

    g_scanned = 1;
    if (f == NULL) { MDBG("theme: wave scan: no maps\n"); return; }

    while (fgets(line, sizeof(line), f) != NULL) {
        uintptr_t lo, hi, a;

        if (!wave_region_wanted(line, &lo, &hi)) continue;
        for (a = lo; a + WAVE_BYTES <= hi; a += 4) {
            const uint32_t *p = (const uint32_t *)a;
            int k;

            if (p[0] != k_wave_stock[0]) continue;
            for (k = 0; k < WAVE_N; k++)
                if (p[k * WAVE_STRIDE] != k_wave_stock[k] ||
                    p[k * WAVE_STRIDE + 1] != 0u) break;
            if (k == WAVE_N) wave_note((uint32_t *)a);
        }
    }
    fclose(f);

    /* Loud on zero. The group is a measurement, so a firmware that changes one of those
     * eight colours makes every site vanish -- and the failure mode without this line is
     * a waveform that is silently stock while everything around it is themed. */
    if (g_site_n == 0)
        MDBG("theme: wave scan found NO colour table -- waveform stays on the pixel "
             "path (did the deck's own colours change?)\n");
    else
        MDBG("theme: wave scan found %d colour tables (%d new)\n",
             g_site_n, g_site_n - before);
}

/* ---- applying ------------------------------------------------------------ */

static int wave_is(const uint32_t *p, const uint32_t *want)
{
    int k;

    for (k = 0; k < WAVE_N; k++)
        if (p[k * WAVE_STRIDE] != want[k]) return 0;
    return 1;
}

int theme_wave_source_on(void)
{
    return g_site_n > 0 && g_themed_id > 0;
}

uint32_t theme_wave_ground(void)
{
    return g_ground;
}

/* The same entry BEFORE the palette. Its counterpart above is what a strip baked through
 * the themed tables carries; this is what one baked before them carries, and telling the
 * two apart is the whole job in img_ground_themed. */
uint32_t theme_wave_ground_stock(void)
{
    return k_wave_stock[WAVE_N - 1] & 0xffffffu;
}

/* [message] Put the theme back into every table that has gone stock. Cheap by design:
 * the common pass compares eight words per site and writes nothing. */
void theme_wave_apply(void)
{
    const struct theme_palette *pal = mod_theme()->palette;

    if (prestem_native_owner_active()) {
        /* Native PRE-STEMS patches the detailed-waveform providers itself. */
        g_ground = 0;
        g_pal = NULL;
        return;
    }
    int id = __atomic_load_n(&g_theme_id, __ATOMIC_RELAXED);
    int i, k, wrote = 0;

    if (pal == NULL) {
        /* ORIGINAL. Anything we themed has to go back, and a table the app has since
         * rebuilt is already stock -- which the stateless test handles without knowing
         * which case it is in. */
        if (g_written_ok) {
            int back = 0;

            for (i = 0; i < g_site_n; i++)
                if (wave_is(g_site[i], g_written)) {
                    for (k = 0; k < WAVE_N; k++)
                        g_site[i][k * WAVE_STRIDE] = k_wave_stock[k];
                    back++;
                }
            MDBG("theme: wave source -> ORIGINAL, %d/%d tables restored\n",
                 back, g_site_n);
            g_written_ok = 0;
        }
        g_themed_id = -1;
        /* Disarms every wrapper in one move -- none is unpatched, all read these. */
        g_ground = 0;
        g_pal = NULL;
        return;
    }

    if (!g_scanned) wave_scan();
    g_pal = pal;
    wave_provider_install();

    if (id != g_themed_id) {
        /* is_fill 0: these are ink and ground in a picture, which is what the per-pixel
         * route called them, so the two produce the same colours and switching to this
         * one changes the cost and not the look. */
        for (k = 0; k < WAVE_N; k++) {
            uint32_t c = theme_palette_map(pal, k_wave_stock[k] & 0xffffffu, 0);

            /* Pure black is reserved: theme_wave_ground_fill reads it as "the deck painted
             * its background here". Under WHITE the waveform's PEAKS (stock 0xffffff)
             * invert onto it, and the fill then repaints them the ground colour -- white
             * streaks through the middle of the waveform, reported off the deck and
             * invisible in a still until you know to look. The nudge has to go into the
             * TABLE, not just our copy, or the renderer keeps painting peaks in a colour
             * the fill cannot tell from its background. One level, which no eye resolves.
             * The ground entry is exempt: black there means a dark theme, and then neither
             * the fill nor the provider wrappers run. */
            if (c == 0 && k != WAVE_N - 1) c = 0x010101u;
            g_themed[k] = 0xff000000u | c;
        }
        g_ground = g_themed[WAVE_N - 1] & 0xffffffu;
        g_themed_id = id;
        MDBG("theme: wave source palette: ink %06x->%06x  ground %06x->%06x\n",
             k_wave_stock[2] & 0xffffffu, g_themed[2] & 0xffffffu,
             k_wave_stock[7] & 0xffffffu, g_themed[7] & 0xffffffu);
    }

    for (i = 0; i < g_site_n; i++) {
        /* Already right. Checked FIRST because it is the answer almost every time this
         * runs -- once a site is themed it stays themed until the app rebuilds it, and
         * without this the pass rewrites all 185 identical groups on every tick. */
        if (wave_is(g_site[i], g_themed)) continue;
        /* Stock: the app rebuilt it. Ours-from-before: the theme changed under it. Both
         * are ours to write; anything else belongs to somebody we do not know about. */
        if (!wave_is(g_site[i], k_wave_stock) &&
            !(g_written_ok && wave_is(g_site[i], g_written))) continue;
        for (k = 0; k < WAVE_N; k++)
            g_site[i][k * WAVE_STRIDE] = g_themed[k];
        wrote++;
    }
    if (wrote) {
        for (k = 0; k < WAVE_N; k++) g_written[k] = g_themed[k];
        g_written_ok = 1;
        MDBG("theme: wave source re-applied to %d/%d tables\n", wrote, g_site_n);
    }
}

/* [message] A waveform replyer ran, which is the track-load event that rebuilds the
 * tables. `style` distinguishes the three WAVEFORM COLOR modes; a change of style can
 * bring a widget that did not exist at scan time, so that -- and only that -- rescans. */
void theme_wave_replied(int style)
{
    if (style != g_style) {
        g_style = style;
        g_scanned = 0;
    }
    theme_wave_apply();
}

/* ---- the ground, and the black the blender averages against --------------- */

/* ---- the provider slot, reached through an assembly stub ------------------ */

/* [message] What the lerp asks for a colour, and why C alone cannot wrap it.
 *
 * The three providers -- one per WAVEFORM COLOR mode -- answer one column and band with
 * {height, B, G, R, A}. A band a column does not have comes back as a DEFAULT-CONSTRUCTED
 * juce::Colour, which is the second black the lerp can average against.
 *
 * THE OUT POINTER ARRIVES IN X8. The return type has a non-trivial destructor, so AAPCS
 * returns it indirectly whatever its size, and x8 is not something C can name:
 *
 *     0x14a0f60   mov  x19, x8            <- the out pointer
 *     0x14a0f8c   strb w2, [x0], #1       <- through x19
 *     0x14a0f9c   mov  x0, x19            <- and RETURNED in x0
 *
 * A wrapper that declares `out` as a fourth parameter gets x3 instead and uses whatever
 * is left there. That does not fault reliably: it silently read and wrote unrelated
 * memory for several builds and looked like it worked, because wrap_render_bg was doing
 * the real job. One more parameter moved the allocation, x3 became unmapped, and it
 * segfaulted on every track load and every SHORTCUT menu.
 *
 * So the wrapper is four instructions of assembly. It needs no access to x8 at all --
 * only to LEAVE IT ALONE until the call, which C will not promise -- and the original
 * hands the pointer back in x0, so the fix-up is an ordinary C function taking it as an
 * argument. x16 is the procedure-call scratch register, which is what a linker veneer
 * uses for exactly this.
 *
 * `map` asks for the ink itself to go through the palette, which only two modes need:
 *
 *   3Band  a table of 8 colours in .bss, themed at rest -- the ink arrives ALREADY
 *          themed and mapping it again would transform it twice
 *   Blue   a table too, at 0x6698d38 just below 3Band's, but the scan only knows 3Band's
 *          stock group so nothing themes it
 *   RGB    NO table: the colour is built from packed 3-3-3 bits of the track's own
 *          analysis data, so this is the only place it can be themed at all
 */
#define WAVE_PROVIDER_OFF 0x30

/* Not static: the stubs below reach these by name through the assembler. */
uintptr_t wave_orig_3band, wave_orig_rgb, wave_orig_blue;

__thread int g_theme_in_bake;

void *wave_sample_fix(unsigned char *out, int map);

void *wave_sample_fix(unsigned char *out, int map)
{
    /* Not during a bake -- see g_theme_in_bake. Substituting here left the overview
     * STOCK: theme_bake_recolour expects the deck's own stock image and maps it once. */
    if (out == NULL || g_theme_in_bake) return out;

    if (out[4] == 0u) {                               /* absent band -> themed ground */
        /* THE GROUND GATE IS THE GROUND'S ALONE. Several themes map black to black --
         * MOCHA is one -- and for those the deck's own black already IS the ground, so
         * there is nothing to substitute. Gating the INK on the same test is a bug: it
         * left RGB and Blue stock on every dark theme, which is what a light-ground test
         * cannot see. */
        if (g_ground == 0u) return out;
        out[1] = (unsigned char)g_ground;             /* B */
        out[2] = (unsigned char)(g_ground >> 8);      /* G */
        out[3] = (unsigned char)(g_ground >> 16);     /* R */
        out[4] = 0xffu;
    } else if (map && g_pal != NULL) {
        uint32_t rgb = ((uint32_t)out[3] << 16) | ((uint32_t)out[2] << 8) | out[1];

        /* is_fill 0: ink in a picture, the same question the per-pixel route asked. */
        rgb = theme_palette_map(g_pal, rgb, 0);
        out[1] = (unsigned char)rgb;
        out[2] = (unsigned char)(rgb >> 8);
        out[3] = (unsigned char)(rgb >> 16);
    }
    return out;
}

/* x0/w1/w2 and x8 pass through untouched to the original; x16 is the only register
 * touched before the call. */
#define WAVE_STUB(name, orig, mapval)                                   \
    __asm__(".text\n"                                                   \
            ".align  2\n"                                               \
            ".hidden " #name "\n"                                       \
            ".globl  " #name "\n"                                       \
            ".type   " #name ", %function\n"                            \
            #name ":\n"                                                 \
            "   stp  x29, x30, [sp, #-16]!\n"                           \
            "   mov  x29, sp\n"                                         \
            "   adrp x16, " #orig "\n"                                  \
            "   ldr  x16, [x16, #:lo12:" #orig "]\n"                    \
            "   blr  x16\n"                                             \
            "   mov  w1, #" #mapval "\n"                                \
            "   bl   wave_sample_fix\n"                                 \
            "   ldp  x29, x30, [sp], #16\n"                             \
            "   ret\n"                                                  \
            ".size   " #name ", .-" #name "\n")

WAVE_STUB(wave_stub_3band, wave_orig_3band, 0);
WAVE_STUB(wave_stub_rgb,   wave_orig_rgb,   1);
WAVE_STUB(wave_stub_blue,  wave_orig_blue,  1);

void wave_stub_3band(void);
void wave_stub_rgb(void);
void wave_stub_blue(void);

/* [message] The colour the waveform's OUTER EDGE is averaged against.
 *
 * renderBackground fills one juce::Colour per column into the array that
 * renderWaveform_new then lerps the ink against, so the edge between waveform and
 * background is decided here and NOT by the palette or the provider. Read live off the
 * deck: mode 1, 1020 columns, every entry 0x00000000. Black on a dark deck is right and
 * invisible; on a light one it is the contour, and it is why a blue edge leaves a dark
 * trail as the waveform scrolls.
 *
 * Only entries that came out black are moved. The two colours renderBackground chooses
 * between are the widget's own, and a firmware that gives them a real value means it
 * somewhere. */
typedef void *(*render_bg_t)(void *, unsigned char **, int, void *, float);
static uintptr_t g_tramp_bg;

static void *wrap_render_bg(void *colours, unsigned char **span, int x0,
                            void *info, float scale)
{
    void *r = ((render_bg_t)g_tramp_bg)(colours, span, x0, info, scale);

    if (g_ground != 0u && span != NULL && span[0] != NULL && span[1] != NULL) {
        unsigned char *p = span[0], *end = span[1];

        for (; p + 4 <= end; p += 4)
            if ((p[0] | p[1] | p[2]) == 0u) {
                p[0] = (unsigned char)g_ground;             /* B */
                p[1] = (unsigned char)(g_ground >> 8);      /* G */
                p[2] = (unsigned char)(g_ground >> 16);     /* R */
                p[3] = 0xffu;
            }
    }
    return r;
}

/* Patched once and left in place. The wrappers are inert while g_ground is 0, so a theme
 * change needs no repatching and ORIGINAL costs one predictable branch per band. */
static void wave_provider_install(void)
{
    static int done;

    if (done) return;
    done = 1;
    mod_patch_fn("waveRenderBg", ep122_sym(EP122_WAVE_RENDER_BG),
                 (void *)wrap_render_bg, &g_tramp_bg);
    mod_patch_vslot("waveSample3Band", EP122_WAVE_PROVIDER_3BAND, WAVE_PROVIDER_OFF,
                    (void *)wave_stub_3band, &wave_orig_3band);
    /* Not fatal: a miss costs the fix for a style this DJ may never select. */
    mod_patch_vslot("waveSampleRGB", EP122_WAVE_PROVIDER_RGB, WAVE_PROVIDER_OFF,
                    (void *)wave_stub_rgb, &wave_orig_rgb);
    mod_patch_vslot("waveSampleBlue", EP122_WAVE_PROVIDER_BLUE, WAVE_PROVIDER_OFF,
                    (void *)wave_stub_blue, &wave_orig_blue);
}

/* [message] Put the themed ground under a detailed waveform strip, in place.
 *
 * The strip's background is NOT one of the palette entries. Poking entry 7 -- the one the
 * ground maps to -- leaves the screen unchanged, twice checked with a colour loud enough
 * that the fill could not have hidden it, so the region above the waveform is painted
 * black from somewhere the palette does not reach and has to be put right here.
 *
 * Only EXACT black. Every partially covered pixel is now the deck's own blend against the
 * themed ground -- see WAVE_PROVIDER_OFF -- so it is already what it should be, and the
 * one thing this must not do is touch it.
 *
 * Idempotent, which is what lets it write into the app's own buffer with no stash and no
 * restore: the ground is not black, so a second pass finds nothing left to fill. */
void theme_wave_ground_fill(void *data, int32_t w, int32_t h,
                            int32_t line_stride, int32_t pixel_stride)
{
    int32_t x, y;

    if (data == NULL || pixel_stride < 3 || g_ground == 0) return;

    for (y = 0; y < h; y++) {
        uint8_t *p = (uint8_t *)data + (size_t)y * line_stride;

        for (x = 0; x < w; x++, p += pixel_stride) {
            if ((p[0] | p[1] | p[2]) != 0) continue;
            p[0] = (uint8_t)g_ground;
            p[1] = (uint8_t)(g_ground >> 8);
            p[2] = (uint8_t)(g_ground >> 16);
        }
    }
}
