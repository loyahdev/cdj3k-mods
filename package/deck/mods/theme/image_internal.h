/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/theme/image_internal.h - what the theme's image files share.
 *
 * image.c owns the drawImage and paint hooks, image_sync.c brings one image in
 * line with the theme, image_pixel.c reads a sprite's pixels and decides what
 * it is. The pixel helpers are inline here: they run once per PIXEL of every
 * waveform frame.
 */
#ifndef EP122_MOD_THEME_IMAGE_INTERNAL_H
#define EP122_MOD_THEME_IMAGE_INTERNAL_H

#include "theme/theme.h"

/* juce::SoftwarePixelData, and initialiseBitmapData as ImagePixelData virtual
 * index 5. We only ever touch pixels behind an exact vptr match, because
 * SoftwarePixelData::initialiseBitmapData is the one implementation verified to
 * just fill the struct in -- it allocates no BitmapDataReleaser, so a zeroed
 * stack BitmapData needs no destructor. Any other image type is left alone. */
#define THEME_VT_SOFTPIXELDATA  ep122_sym(EP122_SOFT_PIXEL_DATA)

#define IPD_VT_INITBITMAP       5

/* juce::ImagePixelData fields. The base ReferenceCountedObject is vptr + a bare
 * Atomic<int> refCount with NO tail padding, so pixelFormat packs into the same
 * 8-byte word at +0x0c and the dimensions follow at +0x10/+0x14; NamedValueSet
 * userData starts at +0x18. Confirmed by observation: reading one field later gave
 * a heap pointer where the height should be, and these offsets give self-consistent
 * sprite sizes (114x90, 602x84, 194x88) with pixelFormat always 1..3. */
#define IPD_PIXELFORMAT_OFF  0x0c

#define IPD_WIDTH_OFF        0x10

#define IPD_HEIGHT_OFF       0x14

/* juce::Image::PixelFormat */
#define PIXFMT_RGB           1

#define PIXFMT_ARGB          2

/*
 * Baked skin PNGs (the timer and tempo sprite sheets, buttons, badges, icons)
 * never reach setFill -- they are blitted. They are handled here instead.
 *
 * Chrome vs artwork: album art and the waveform must NOT be inverted, so each
 * image is classified once, on first sight, and the verdict cached against its
 * ImagePixelData. Chrome is an ARGB sprite of sprite-like size; artwork is opaque
 * RGB, and the waveform panel is far wider than any sprite.
 *
 * Each classified sprite keeps a private copy of its ORIGINAL pixels. Recolouring
 * always reads from that copy, and switching back to ORIGINAL is a memcpy back, so
 * restoring is exact for any transform -- the accent-darkening variant is not an
 * involution, so flipping in place would not round-trip.
 *
 * Sprites we adopt are also PINNED (refcount bumped once). JUCE releases cached
 * images, and a freed ImagePixelData whose address is later reused would otherwise
 * match a stale table entry -- at best recolouring the wrong image, at worst calling
 * a vtable on an object of another type. Pinning removes that whole class of bug;
 * the cost is a few MB of sprites that were already meant to stay cached.
 */
/* Every image seen takes a slot, adopted or not, and panes that are opened late
 * (beat loop, beat jump, key shift) each bring their own set -- an idle deck already
 * shows ~280. Sized well clear of that: a slot is ~40 bytes, so even 2048 is noise,
 * whereas overflowing silently leaves whole panes unthemed. */
#define THEME_IMG_MAX     2048

#define THEME_IMG_MAXDIM  256           /* wider/taller than any sprite -> artwork */

#define THEME_COPY_BUDGET (24u << 20)   /* cap on retained originals */

/* ...and how much of it LIVE content may hold. Measured on a running emulator after
 * fourteen minutes: 97 real sprites cost 1.01 MB and 101 waveform strips cost 22.99 MB,
 * which spent the whole budget and left every later sighting unadopted -- the overview
 * silently stock from then on, and only a restart put it back.
 *
 * The two are not the same kind of thing and must not share one pool. A sprite is
 * authored, static and finite: the skin has as many as it has. A strip is BAKED, one
 * new buffer per track load and per grid state, so it has no bound at all -- it is a
 * leak wearing a cache's clothes, and left to itself it starves the sprites.
 *
 * So live content gets its own ceiling and its own eviction, and the sprites keep the
 * rest.
 *
 * SIZED TO HOLD THE VISIBLE WORKING SET, which 4 MB did not. Measured live: six live slots
 * for 3.52 MB, so a strip averages ~590 KB -- and a browse page shows more preview rows
 * than that on its own, before the deck's extended overview, its beat-grid rulers and the
 * current track's preview are counted. An LRU that cannot hold what is on screen evicts a
 * strip that is about to be drawn again, so every scroll re-adopted and re-mapped rows that
 * had never left the display.
 *
 * That thrash is no longer a correctness bug -- img_restore puts a released buffer back to
 * its pristine pixels, so any number of evict/re-adopt cycles is now idempotent -- but it
 * is still a per-row memcpy of the copy plus a full palette pass, on a scroll, on this
 * hardware. 12 MB holds about twenty strips, which is a page over several times, and still
 * leaves twelve for sprites that measured 1.01 MB. */
#define THEME_LIVE_BUDGET (12u << 20)

/* Full-width chrome is a sprite too. The software keyboard's backdrop is one 1280x284
 * ARGB image, so capping BOTH dimensions at 256 left it stock-dark while the vector fill
 * covering its middle row inverted normally -- the keyboard came out half themed (rows 1
 * and 3 measured stock #232323/#111111, row 2 #dcdcdc). Shape is what keeps artwork out:
 * a banner is at least 3:1, while album art and logos are square-ish (the 320x240 ARGB
 * image already on screen stays excluded at 1.33:1). The ARGB test still does the real
 * work of separating sprites from photos, the waveform is claimed earlier by
 * theme_is_waveform, and the copy budget still bounds what is retained. */
#define THEME_BANNER_MAXW  2048

#define THEME_BANNER_MAXH  512

#define THEME_BANNER_RATIO 3

#define IPD_REFCOUNT_OFF  0x08

/* ---- where a blit is GOING ----
 *
 * juce::LowLevelGraphicsSoftwareRenderer holds its current SavedState at +0x08, and the
 * state names the destination image's ImagePixelData at +0x68. Read off a running deck:
 * for one 50x50 source the chain answers 48x48 on one blit and 40x40 on the next, which
 * nothing but a real destination pointer does.
 *
 * The chain is only ever walked through mod_safe_read, and only believed when it lands on
 * a SoftwarePixelData. The screen is NOT one -- it answers a different vptr on every blit
 * that reaches the display -- so "is the destination a SoftwarePixelData" separates a blit
 * into an offscreen surface from a blit onto the screen. A firmware that moved either
 * offset reads something that is not a SoftwarePixelData, the caller declines, and the
 * behaviour is what it was before: the failure mode is the old bug, never a fault. */
#define GFXCTX_STATE_OFF    0x08

#define GFXSTATE_IMAGE_OFF  0x68

/* ---- is this still the image we thought it was? ----
 *
 * The sprite cache is built on "these pixels never change": snapshot the original once,
 * recolour once, and from then on `applied` short-circuits every repaint. That holds for
 * a button or a glyph. It is FALSE for the extended overview waveform, which is a 100x58
 * image scaled up ~10x on screen -- small enough to be filed as a sprite, and rewritten
 * from scratch on every track load.
 *
 * The consequence was both halves of this bug at once. We recolour it once, while the
 * widget still holds the previous track's waveform (or nothing at all); the app then
 * paints the new track over our work and `applied` guarantees we never look again, so it
 * shows stock colours. And the pristine copy we kept is now a different track's pixels,
 * so anything that does force a re-apply memcpy's them over the live buffer.
 *
 * Shape cannot separate the two cases -- 100x58 is an ordinary sprite size -- so stop
 * trying to classify and just CHECK. Fingerprint what we leave behind; if what we find
 * next time differs, the app has repainted and the snapshot is stale.
 *
 * Sparse on purpose: a fixed number of samples regardless of size, so the cost does not
 * scale with the 1280x284 banners, and spread by a stride that is not a power of two so
 * a regular pattern in the image cannot alias with the sampling. */
#define THEME_FP_SAMPLES 192

/* What `applied` says when it is not naming a theme.
 *
 * STOCK is the app's own pixels and nothing of ours. MIXED is our output with a fresh
 * repaint through it: the pristine copy has just been reconciled against the buffer so it
 * describes these pixels exactly, but the BUFFER still wears the transform. Reading MIXED as
 * STOCK is reading "we already put it back" off a buffer that is still recoloured, and the
 * snapshot that follows then takes our own output for the original. */
#define THEME_APPLIED_STOCK (-1)
#define THEME_APPLIED_MIXED (-2)


/*
 * The waveform is a wide opaque RGB buffer the app repaints every frame as the
 * track scrolls, so the sprite treatment does not fit: a cached "already done"
 * flag would be wiped by the next repaint, and re-applying on top of our own
 * output would compound. It is handled statelessly instead -- recolour, let the
 * blit happen, put the original back -- which is correct whether or not the app
 * rewrote the buffer, and keeps no pointer that could go stale.
 *
 * Album art is also opaque RGB, so shape is what separates them: strips are wide
 * and short, artwork is roughly square.
 *
 * Two independent tests, because either one alone lets something through:
 *
 *   height  - stable under occlusion. When a popup covers part of the waveform
 *             the app repaints only the exposed slice, so the WIDTH it hands us
 *             shrinks while the height does not. Measured case: the MENU
 *             shortcut overlay starts at x=712, and the 712x190 slice missed a
 *             4:1 width test (needing 760) and was drawn unthemed, leaving a
 *             black band under the overlay.
 *   aspect  - still needed, because artwork can be short as well as small, and
 *             a height ceiling alone would catch it. Kept deliberately loose so
 *             a clipped strip survives: artwork would have to be BOTH short and
 *             2:1 to be mistaken for a waveform, which square cover art is not.
 */
#define THEME_WAVE_ASPECT  2       /* width >= 2*height; loose, see above */

#define THEME_WAVE_MAX_H   256     /* strips are short; artwork is tall too */

struct theme_bitmapdata {
    uint8_t *data;
    int32_t  pixel_format;
    int32_t  line_stride;
    int32_t  pixel_stride;
    int32_t  width;
    int32_t  height;
    uint8_t  tail[32];
};

typedef void (*drawimg_t)(void *ctx, const void *image, const void *transform);

typedef void (*initbitmap_t)(void *pd, void *bitmap, int x, int y, int mode);

/*
 * What the CALLER knows and the pixels cannot say: WHEN they get read.
 *
 *   NOW    the blit happens inside the call being wrapped. That is drawImage, and only
 *          drawImage. A live buffer can be recoloured, blitted and put straight back,
 *          which is correct however often the app rewrites it and leaves no state to go
 *          stale.
 *   LATER  the pixels are consumed after we return. A FillType image is read by the
 *          fillRect that FOLLOWS setFill; a baked overview is read frames later, by a
 *          repaint we are not inside. Nothing can be restored in time, so the recolour
 *          has to PERSIST -- and the fingerprint further down is what keeps that honest.
 *
 * This distinction is the whole reason the deck's extended overview went unthemed. It is
 * a live buffer by shape, so the classifier refused to adopt it; and it arrives through
 * setFill rather than drawImage, so the stateless path never saw it either. It fell
 * between the two, and no amount of tuning either test alone could have caught it --
 * the missing information was never about the image, it was about the call site.
 */
enum theme_blit { THEME_BLIT_NOW, THEME_BLIT_LATER };

static inline int theme_alpha_is_coverage(const uint8_t *p, int has_alpha)
{
    uint32_t a = p[3];

    return has_alpha && p[0] <= a && p[1] <= a && p[2] <= a;
}

static inline void theme_map_pixel(const struct theme_palette *pal, uint8_t *p,
                                   int has_alpha)
{
    uint32_t r, g, b;
    /* Alpha the pixel has EARNED: 255 where the byte is not coverage, because then the
     * colour beside it is already final. */
    int opaque = !theme_alpha_is_coverage(p, has_alpha);
    uint32_t a = opaque ? 255u : p[3];

    if (a == 0) return;                       /* premultiplied transparent: nothing there */
    if (a == 255u) {
        /* The packed form, and this is the call that made it worth having: it is one
         * per pixel of every waveform frame, so the colour stays in registers instead
         * of being written out and read back through three pointers. */
        uint32_t v = theme_palette_map(pal, ((uint32_t)p[2] << 16) |
                                            ((uint32_t)p[1] << 8) | p[0],
                                       0 /* image: blue is ink, darken it */);
        r = v >> 16; g = (v >> 8) & 0xffu; b = v & 0xffu;
    } else {
        uint32_t v;

        r = (uint32_t)p[2] * 255u / a;        /* unpremultiply */
        g = (uint32_t)p[1] * 255u / a;
        b = (uint32_t)p[0] * 255u / a;
        if (r > 255u) r = 255u;
        if (g > 255u) g = 255u;
        if (b > 255u) b = 255u;
        v = theme_palette_map(pal, (r << 16) | (g << 8) | b, 0 /* image */);
        r = v >> 16; g = (v >> 8) & 0xffu; b = v & 0xffu;
        r = r * a / 255u; g = g * a / 255u; b = b * a / 255u;
    }
    p[2] = (uint8_t)r; p[1] = (uint8_t)g; p[0] = (uint8_t)b;

    /* Make it opaque for real. A premultiplied source composites as
     * `dst = src + dst*(1 - srcA)`, so alpha 0 does not mean "invisible" here -- it means
     * ADDITIVE. Against the deck's stock black backdrop addition and replacement are the
     * same picture, which is why nothing looked wrong until a LIGHT theme moved the
     * backdrop to white and the whole strip clipped there: measured 94.8% pure white, with
     * only the handful of pixels that did carry real alpha still visible.
     *
     * Writing the byte is what makes the recolour mean what it says on any backdrop, and
     * it is safe precisely because we got here by proving this alpha was never coverage.
     * Guarded on has_alpha because in an RGB buffer p[3] is the NEXT pixel's blue. */
    if (opaque && has_alpha) p[3] = 255u;
}

/* The retained originals: one entry per image the theme has adopted. */
struct theme_img_slot {
    const void *pd;
    uint8_t     chrome;      /* passed classification -> we may recolour it */
    /* BAKED, not authored: one buffer per track load or grid state, superseded and
     * never asked for again. Only these are evictable -- a sprite that went away
     * would have to be re-adopted and re-mapped the next time it is drawn, which is
     * every frame, for no memory worth having back. */
    uint8_t     live;
    /* When this was last sighted, off g_img_clock. The eviction order, and the only
     * thing that tells a strip still on screen from one three tracks ago. */
    uint32_t    seen;
    /* WHICH theme these pixels are currently wearing: -1 for pristine, else the theme
     * id. A bool ("is it recoloured") was not enough and the failure was invisible until
     * there were two themes worth switching between -- going from one palette straight to
     * another left it set on both sides, the early-return below fired, and every sprite
     * kept the FIRST theme's pixels. */
    int16_t     applied;
    uint8_t    *orig;        /* pristine pixels */
    size_t      bytes;
    int32_t     w, h;
    /* THE PIXELS WE ADOPTED, as an identity rather than as a place to write. A pd is an
     * address and the app recycles them, so a slot has been seen matching a pd whose object
     * had already become a different size; every guard downstream then validates against
     * memory that is no longer the object it describes, and a restore memcpy's the old copy
     * over whatever is there now. A different allocation answers a different address. */
    const uint8_t *data;
    /* Recorded for every entry, adopted or refused, because a refusal is a verdict about an
     * image and has to be re-taken when the address carries a different one. */
    int16_t     fmt;
    /* We hold one reference on pd. Recorded rather than inferred from `chrome`, which a slot
     * can lose while the reference is still outstanding. */
    uint8_t     pinned;
    /* What we LEFT in the buffer last time. See theme_fingerprint. */
    uint64_t    stamp;
};

extern struct theme_img_slot g_img[THEME_IMG_MAX];
extern int    g_img_n;
extern size_t g_img_bytes;

int  theme_sprite_size(int32_t w, int32_t h);
int  theme_bitmap(void *pd, int32_t w, int32_t h, struct theme_bitmapdata *bd);
int  theme_is_waveform(const void *pd, int32_t *pw, int32_t *ph, int *palpha);
uint64_t theme_fingerprint(const struct theme_bitmapdata *bd, int32_t w, int32_t h);
void theme_img_probe(const void *pd, const struct theme_bitmapdata *bd,
                            int32_t w, int32_t h, int32_t fmt, const char *verdict);
void theme_wave_census(const struct theme_bitmapdata *bd, int32_t w, int32_t h);
void theme_sync_image_x(const void *image, enum theme_blit when);

/* Has the theme adopted this image, and may it be recoloured? */
int theme_img_adopted(const void *pd);

/* Lend the blit about to run this sprite's STOCK pixels. See image_sync.c. */
int  theme_img_lend_stock(const void *image, const void *ctx);

#endif /* EP122_MOD_THEME_IMAGE_INTERNAL_H */
