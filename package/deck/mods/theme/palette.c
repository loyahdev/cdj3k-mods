// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * palette.c - the whole colour model, in one function.
 *
 * Every theme is this code with different numbers. The stages and the reasoning
 * behind each of them are documented on struct theme_palette in theme.h; this
 * file is the arithmetic.
 *
 * It runs on every fill the UI performs -- thousands per frame -- and on every
 * pixel of every sprite when a theme is switched, so it stays in integer maths
 * with no division by a variable in the common path and no floating point at
 * all. There is no HSL round trip: each stage is expressed directly on RGB in a
 * form that provably preserves what it claims to.
 */
#include "theme/theme.h"

/* Lightness, the HSL definition, doubled to keep it integral: max + min.
 * Used as the duotone ramp index and as the pivot for the chroma scale. */
static inline uint32_t luma2(uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t max = r > g ? r : g; if (b > max) max = b;
    uint32_t min = r < g ? r : g; if (b < min) min = b;
    return max + min;
}


/* Hue as a 0..255 angle, the standard integer form. `c` is the chroma the caller has
 * already computed, and must be non-zero -- a grey has no hue to ask for. */
static inline uint32_t hue256(uint32_t r, uint32_t g, uint32_t b,
                              uint32_t max, uint32_t c)
{
    int h;

    if (max == r)      h =       43 * ((int)g - (int)b) / (int)c;
    else if (max == g) h =  85 + 43 * ((int)b - (int)r) / (int)c;
    else               h = 171 + 43 * ((int)r - (int)g) / (int)c;
    return (uint32_t)(h & 0xff);
}

/* Below this, a pixel is grey enough that "its hue" is noise -- rotating it would put
 * colour into the deck's neutral chrome, which is exactly where none belongs. Those go to
 * the duotone instead. */
#define HUE_MIN_CHROMA 24u

/* The arithmetic proper. `p` is never NULL here -- theme_palette_rgb and
 * theme_palette_map own that case -- and this is a PURE function of
 * (p, r, g, b, is_fill), which is what lets the memo in front of it be
 * transparent. */
static void palette_compute(const struct theme_palette *p,
                            uint32_t *pr, uint32_t *pg, uint32_t *pb, int is_fill)
{
    uint32_t r = *pr, g = *pg, b = *pb;
    uint32_t max, min, c;

    /* The selection blue, stated by the theme rather than transformed -- see
     * theme_palette::selection. The fill path only: through drawImage this same blue is
     * picture content, and a waveform is not a selected row. */
    if (is_fill && p->selection) {
        uint32_t src = (r << 16) | (g << 8) | b;

        if (src == THEME_DECK_SELECT || src == THEME_DECK_SELECT2) {
            uint32_t q = src == THEME_DECK_SELECT ? 256u : THEME_SELECT2_Q8;

            *pr = (((p->selection >> 16) & 0xffu) * q) >> 8;
            *pg = (((p->selection >>  8) & 0xffu) * q) >> 8;
            *pb = ((  p->selection        & 0xffu) * q) >> 8;
            return;
        }
    }

    max = r > g ? r : g; if (b > max) max = b;
    min = r < g ? r : g; if (b < min) min = b;
    c = max - min;                    /* chroma of the ORIGINAL colour */

    /* ---- 1. lightness inversion, hue and chroma exact --------------------
     *
     * L' = 255 - L leaves |2L-255| unchanged, so the chroma span survives and
     * every channel shifts by the same amount:
     *
     *   c' = c - min + (255 - max) = c + (255 - max - min)
     *
     * c is in [min,max], so c' lands in [255-max, 255-min] -- always in range,
     * no clamping. Worked examples:
     *
     *   #191919 list bg   -> #e6e6e6   (dark grey  -> light grey)
     *   #ffffff text      -> #000000   (white      -> black)
     *   #007de1 accent    -> #1e9bff   (still blue, lifted for a light bg)
     *   #ff0000 warning   -> #ff0000   (fully saturated: unchanged, correctly)
     */
    if (p->invert_l) {
        const uint32_t k = 255u - max - min;
        r += k; g += k; b += k;
    }

    /* ---- 2. pull saturated colours darker -------------------------------
     * f = 1 - sat_darken*(C/255). All three channels scale together, so hue is
     * exact and greys (C == 0) are untouched. Not an involution, which is why
     * image.c keeps a pristine copy rather than flipping pixels back. */
    if (p->sat_darken_q8 > 0 && c > 0) {
        int exempt = is_fill && p->exempt_blue && b > r && b > g;

        if (!exempt) {
            uint32_t f = 255u * 256u - (uint32_t)p->sat_darken_q8 * c;
            r = r * f / (255u * 256u);
            g = g * f / (255u * 256u);
            b = b * f / (255u * 256u);
        }
    }

    /* ---- 3. chroma scale about the pixel's own lightness -----------------
     * Push each channel away from (or toward) the local mid-point. Signed
     * arithmetic and an explicit clamp: unlike stage 1 this one can leave the
     * representable range, and a wrap here would show up as confetti. */
    /* ZERO MEANS UNCHANGED, not greyscale.
     *
     * theme.h promises "a field left zero does nothing", and this was the one field that
     * broke it -- 0 read as "scale chroma by 0", so a palette that simply did not mention
     * sat_q8 had every pixel stripped to grey before any later stage ran. Four themes
     * shipped that way and came out monochrome no matter what their hues said. A struct
     * whose default destroys the image is a trap, so the default is now the identity and
     * greyscale is asked for explicitly with 1. */
    if (p->sat_q8 != 0 && p->sat_q8 != 256) {
        int32_t mid = (int32_t)(luma2(r, g, b) / 2u);
        int32_t s = p->sat_q8;
        int32_t ch[3] = { (int32_t)r, (int32_t)g, (int32_t)b };
        int i;

        for (i = 0; i < 3; i++) {
            int32_t v = mid + ((ch[i] - mid) * s >> 8);

            ch[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
        }
        r = (uint32_t)ch[0]; g = (uint32_t)ch[1]; b = (uint32_t)ch[2];
    }

    /* ---- 4a. hue map: the palette proper ----------------------------------
     *
     * Rotate a chromatic pixel to the NEAREST hue this theme owns, holding its own
     * lightness and chroma. That is what keeps a palette from collapsing into one
     * colour: the deck distinguishes things by hue, and mapping preserves the
     * distinction while replacing the colours. The duotone below cannot do this -- it
     * has two anchors, so everything it produces is one hue.
     *
     * Reconstructed without an inverse HSL: take the target, measure how far each of
     * its channels sits from its own mid-point, and scale that spread to the SOURCE's
     * chroma about the SOURCE's mid-point. Hue comes from the target, weight from the
     * source. */
    if (p->nhue > 0) {
        /* Recomputed here on purpose: `max` and `c` at the top of this function describe
         * the colour as it ARRIVED, and stages 1-3 have moved it since. Asking for the
         * hue of one pixel while weighting it by another's chroma is how a mapped colour
         * ends up somewhere neither of them was. */
        uint32_t nmax = r > g ? r : g; if (b > nmax) nmax = b;
        uint32_t nmin = r < g ? r : g; if (b < nmin) nmin = b;
        uint32_t nc = nmax - nmin;
        uint32_t hs, best = 0, bestd = 0xffffffffu;
        uint32_t i;

        if (nc < HUE_MIN_CHROMA) goto duotone;
        c  = nc;
        hs = hue256(r, g, b, nmax, nc);
        for (i = 0; i < p->nhue && i < 4; i++) {
            uint32_t tr = (p->hue[i] >> 16) & 0xffu;
            uint32_t tg = (p->hue[i] >> 8) & 0xffu;
            uint32_t tb = p->hue[i] & 0xffu;
            uint32_t tmax = tr > tg ? tr : tg; if (tb > tmax) tmax = tb;
            uint32_t tmin = tr < tg ? tr : tg; if (tb < tmin) tmin = tb;
            uint32_t tc = tmax - tmin, ht, d;

            if (tc == 0) continue;                /* a grey in the hue list means nothing */
            ht = hue256(tr, tg, tb, tmax, tc);
            d  = hs > ht ? hs - ht : ht - hs;
            if (d > 128u) d = 256u - d;           /* the angle wraps */
            if (d < bestd) { bestd = d; best = i; }
        }
        if (bestd != 0xffffffffu) {
            uint32_t tr = (p->hue[best] >> 16) & 0xffu;
            uint32_t tg = (p->hue[best] >> 8) & 0xffu;
            uint32_t tb = p->hue[best] & 0xffu;
            uint32_t tmax = tr > tg ? tr : tg; if (tb > tmax) tmax = tb;
            uint32_t tmin = tr < tg ? tr : tg; if (tb < tmin) tmin = tb;
            uint32_t tc = tmax - tmin;
            int32_t  tmid = (int32_t)(tmax + tmin) / 2;
            int32_t  smid = (int32_t)luma2(r, g, b) / 2;
            int32_t  ch[3] = { (int32_t)tr, (int32_t)tg, (int32_t)tb };
            int32_t  k = p->hue_pull_q8;
            int32_t  mid, spread;
            int j;

            /* How much of the target comes along besides its hue -- see hue_pull_q8.
             * At k = 0 these collapse to `smid` and `c` and the whole stage is exactly
             * what it was before the field existed, which is what keeps WHITE and the
             * dark themes bit-identical. */
            if (k < 0) k = 0; else if (k > 256) k = 256;
            mid    = smid + (tmid - smid) * k / 256;
            spread = (int32_t)c + ((int32_t)tc - (int32_t)c) * k / 256;

            for (j = 0; j < 3; j++) {
                int32_t v = mid + (ch[j] - tmid) * spread / (int32_t)tc;

                ch[j] = v < 0 ? 0 : (v > 255 ? 255 : v);
            }
            *pr = (uint32_t)ch[0];
            *pg = (uint32_t)ch[1];
            *pb = (uint32_t)ch[2];
            return;                               /* mapped: the duotone is for greys */
        }
    }

duotone:
    /* ---- 4b. duotone -------------------------------------------------------
     * Blend toward a ramp between two anchors, indexed by lightness: `shadow` is
     * what black becomes, `highlight` what white becomes. tint_q8 is how far.
     *
     * This is the stage that gives a theme an identity. Everything above only
     * re-polarises or re-saturates the deck's own hues; this one replaces them,
     * which is why it is last and why it is the only stage that can make the
     * accent blue stop being blue. */
    if (p->tint_q8 > 0) {
        uint32_t l = luma2(r, g, b) / 2u;         /* 0..255 */
        uint32_t t = (uint32_t)p->tint_q8;
        uint32_t sr = (p->shadow >> 16) & 0xffu, hr = (p->highlight >> 16) & 0xffu;
        uint32_t sg = (p->shadow >> 8) & 0xffu,  hg = (p->highlight >> 8) & 0xffu;
        uint32_t sb = p->shadow & 0xffu,         hb = p->highlight & 0xffu;
        uint32_t tr = sr + (hr - sr) * l / 255u;  /* the ramp at this lightness */
        uint32_t tg = sg + (hg - sg) * l / 255u;
        uint32_t tb = sb + (hb - sb) * l / 255u;

        /* Anchors are authored dark-to-light, so the subtractions above stay
         * non-negative; a reversed pair would wrap, hence the assert-by-comment
         * rather than a branch on a hot path. presets.c keeps to it. */
        r = (r * (256u - t) + tr * t) >> 8;
        g = (g * (256u - t) + tg * t) >> 8;
        b = (b * (256u - t) + tb * t) >> 8;
    }

    *pr = r > 255u ? 255u : r;
    *pg = g > 255u ? 255u : g;
    *pb = b > 255u ? 255u : b;
}

/* ================================================================== */
/* The memo                                                           */
/* ================================================================== */

/*
 * palette_compute is pure, and the UI asks it the same questions over and over.
 * setFill runs thousands of times a frame over a handful of chrome colours, and
 * the waveform hands drawImage a whole 1200x128 strip of pixels every frame drawn
 * from that strip's own small ink palette. Each answer costs a dozen integer
 * divisions, and integer division is the one thing an A72 does not pipeline.
 *
 * That arithmetic is the whole of why a themed deck drops under 30 fps while
 * ORIGINAL stays fluid: ORIGINAL returns at the top of theme_palette_map and pays
 * none of it. Profiled on a real CDJ-3000 wearing MOCHA, this function was 31.8%
 * of the entire process. So put a cache in front rather than making the transform
 * cheaper -- the transform is what the themes are, and the answers repeat.
 *
 * The hit path is INLINE, in theme.h, and this file holds only the miss. That is
 * deliberate and it is why the table is exposed there rather than kept private: at
 * one call per pixel the call itself was a measurable part of the remaining cost,
 * and a lookup the compiler can fold into the pixel loop keeps the colour in a
 * register instead of round-tripping it through memory.
 *
 * Sizing, off the DATA rather than a guess. Counted on a stock-theme capture: the
 * overview strip holds 46 distinct colours and FOUR of them -- #000000 ground,
 * #0055e1 ink, #ffffff peaks, #ffa600 -- cover 99% of its pixels; the browse
 * previews hold 518. Simulated over the real pixel stream, the hit rate by pixel
 * runs 98.1% at 256 slots, 99.3% at 1024 and 99.5% at 2048: past a kilobyte or so
 * the curve is flat, because a waveform is a bar chart and not a photograph.
 *
 * So the size is chosen for the CACHE, not the hit rate. 1024 slots is 8 KB, a
 * quarter of the A72's 32 KB L1, and the 600 KB strip being walked wants the rest
 * -- a table that evicts the pixels it is being asked about costs more than the
 * 0.2% of hits it buys back.
 *
 * The failure is not gentle in the other direction either. Measured over a
 * synthetic strip, at a few hundred distinct colours the memo is 2x on WHITE and
 * ~7x on a palette with a hue map, and at 4096 it turns into a LOSS -- every miss
 * pays the hash and the store on top of the arithmetic it did not avoid. That is
 * why the report prints the hit rate and not just the totals: a rate that falls
 * off says this table is being shown something it was not sized for, and it is the
 * one number that separates "make it bigger" from "take it out".
 */

/* THE TABLE. One naturally-aligned 64-bit word per slot, which aarch64 loads and
 * stores single-copy atomically -- so an entry can never be read half-written and
 * the paint thread and the waveform bake need no lock between them. Each entry
 * carries the question beside the answer, so a collision reads as a miss rather
 * than as the wrong colour. Layout in theme.h, beside the code that reads it. */
uint64_t g_theme_memo[THEME_MEMO_N];

/* WHOSE ANSWERS THESE ARE. Every caller passes mod_theme()->palette and the presets
 * are const statics, so one pointer means one palette for the life of the process
 * and a pointer change is a theme switch -- the only event that can invalidate an
 * answer. Emptying 8 KB on a switch is nothing. The cost of the invariant breaking
 * is a memset per call rather than a wrong colour, i.e. slow, not incorrect. */
const struct theme_palette *g_theme_memo_pal;

/* Read off the running deck, and SPLIT BY is_fill because the two callers are
 * different questions. is_fill 1 is setFill: one call per rect, path or glyph the
 * UI paints. is_fill 0 is image pixels: one call per PIXEL of every waveform frame,
 * which is three orders of magnitude more and is what the exercise is about. A
 * single total would let the larger swallow the smaller and hide which one moved. */
unsigned g_theme_memo_hit[2], g_theme_memo_miss[2];

/* The transform on a separated triplet. A NULL palette is ORIGINAL and leaves the
 * triplet untouched, matching theme_palette_map. */
void theme_palette_rgb(const struct theme_palette *p,
                       uint32_t *pr, uint32_t *pg, uint32_t *pb, int is_fill)
{
    if (p == NULL)
        return;
    palette_compute(p, pr, pg, pb, is_fill);
}

/* The miss. Out of line on purpose: it is the cold half, and keeping it out of the
 * pixel loop is most of what the inline hit path buys. */
uint32_t theme_palette_slow(const struct theme_palette *p, uint32_t rgb, int is_fill)
{
    uint32_t r = (rgb >> 16) & 0xffu, g = (rgb >> 8) & 0xffu, b = rgb & 0xffu;
    uint32_t q = (rgb << 1) | (is_fill ? 1u : 0u);
    uint64_t ent;

    if (p != g_theme_memo_pal) {
        memset(g_theme_memo, 0, sizeof(g_theme_memo));
        g_theme_memo_pal = p;
    }

    palette_compute(p, &r, &g, &b, is_fill);
    rgb = (r << 16) | (g << 8) | b;
    g_theme_memo_miss[is_fill ? 1 : 0]++;

    ent = THEME_MEMO_OCCUPIED | ((uint64_t)q << THEME_MEMO_Q_SHIFT) | (uint64_t)rgb;
    __atomic_store_n(&g_theme_memo[THEME_MEMO_SLOT(q)], ent, __ATOMIC_RELAXED);
    return rgb;
}

/* [message] Print at most every 3 s, from the setFill tick. Cheap enough to call
 * unconditionally: MDBG tests the level before it evaluates anything. */
void theme_memo_report(void)
{
    static unsigned last_t, last_h[2], last_m[2];
    unsigned h[2] = { g_theme_memo_hit[0], g_theme_memo_hit[1] };
    unsigned m[2] = { g_theme_memo_miss[0], g_theme_memo_miss[1] };
    unsigned now = (unsigned)time(NULL), dt, dh[2], dm[2], i;

    if (last_t == 0) goto rearm;
    dt = now - last_t;
    if (dt < 3u) return;

    for (i = 0; i < 2; i++) {
        dh[i] = h[i] - last_h[i];
        dm[i] = m[i] - last_m[i];
    }
    MDBG("theme: memo  pixels %u/s (%u%% hit)  fills %u/s (%u%% hit)  over %us\n",
         (dh[0] + dm[0]) / dt,
         (dh[0] + dm[0]) ? (unsigned)(100ull * dh[0] / (dh[0] + dm[0])) : 0u,
         (dh[1] + dm[1]) / dt,
         (dh[1] + dm[1]) ? (unsigned)(100ull * dh[1] / (dh[1] + dm[1])) : 0u, dt);

rearm:
    last_t = now;
    for (i = 0; i < 2; i++) { last_h[i] = h[i]; last_m[i] = m[i]; }
}

uint32_t theme_palette_argb(const struct theme_palette *p, uint32_t argb,
                            int is_fill)
{
    return (argb & 0xff000000u) |                 /* alpha untouched */
           theme_palette_map(p, argb & 0xffffffu, is_fill);
}
