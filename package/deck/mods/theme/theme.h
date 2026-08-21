// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * theme.h - what a theme IS, and the seam the rest of this directory is built on.
 *
 * The model, ORIGINAL's role and how to add a theme: docs/mods.md.
 *
 *   theme.h    this: the palette model, the registry, the internal seam
 *   presets.c  the themes themselves -- the only file a new theme touches
 *   palette.c  the generic transform: one colour in, one colour out
 *   image.c    sprite/waveform pixels: classify, pin, recolour, restore
 *   theme.c    the two hooks and install
 *
 * Add a `struct theme_palette` and a `{ name, &palette }` row at the END of
 * k_mod_themes: the settings file stores the INDEX, so the table's order is part
 * of the on-disk format.
 *
 * All themes are selectable from the menu. The pane borrows the deck's OFF/ON
 * option set, which is two rows long, so a longer list means HOOKING getNumRows
 * rather than writing the field behind it -- a written count outlives the row that
 * wanted it, and the model then claims five over an array holding two.
 * menu_rnumrows and menu_pane_labels both derive their length from
 * menu_pane_rows(), so the count and the array cannot disagree.
 */
#ifndef EP122_MOD_THEME_H
#define EP122_MOD_THEME_H

#include "core/mod_core.h"
#include "juce/draw.h"      /* struct theme_ui: the roles, shared with every mod's paint */

#ifdef __cplusplus
extern "C" {
#endif


/* Which theme is in force, as an index into the registry below. Read on every
 * setFill, so changing it recolours the UI on the next repaint -- no restart, and
 * nothing on disk changes but the settings file. 0 is ORIGINAL. */
extern int g_theme_id;

/* NON-ZERO WHILE A WAVEFORM REPLYER IS BAKING THE OVERVIEW, on that replyer's own thread.
 *
 * The overview is baked once per track on the database reply thread, and the deck rebuilds
 * its colour table to STOCK just before doing it -- so theme_bake_recolour maps a stock
 * image through the palette exactly once. Anything that themes a colour DURING the bake
 * hands that pass an input it was not written for, and the overview comes out stock. The
 * detailed strip paints on the message thread and is unaffected, which is why this is a
 * thread-local and not a global. */
extern __thread int g_theme_in_bake;

/* ================================================================== */
/* The palette model                                                  */
/* ================================================================== */

/*
 * Every stage is optional and every one has a visual meaning, applied in this
 * order. A field left zero does nothing, so a minimal theme is one line.
 *
 *   1. invert_l     HSL lightness inversion: L -> 1-L, hue and chroma EXACT.
 *                   This is the dark-to-light flip, and it is exact because
 *                   inverting L leaves the chroma span unchanged, which
 *                   collapses the whole HSL round trip to one add per channel.
 *
 *   2. sat_darken_q8  Pull saturated colours darker in proportion to their
 *                   chroma. A pure lightness inversion leaves them untouched,
 *                   which is right in theory and costs contrast in practice --
 *                   the orange +-10 badge and the red hot-cue marker were tuned
 *                   to sit on black. Hue is exact (all channels scale together)
 *                   and greys are untouched (chroma 0).
 *
 *   3. sat_q8       Chroma scale about the pixel's own lightness. 256 leaves it
 *                   alone, 0 is greyscale, above 256 is punchier.
 *
 *   4. shadow/highlight + tint_q8   Duotone. Blend the result toward a ramp
 *                   between two anchor colours, indexed by lightness: `shadow`
 *                   is what black becomes, `highlight` what white becomes. This
 *                   is what gives a theme an identity rather than just a
 *                   polarity -- everything else preserves the deck's own hues.
 *
 * exempt_blue is a carve-out on stage 2 for the FILL path only. The call site
 * knows what no colour value can: a fill is a surface or a mark drawn by the UI,
 * where blue means the selection row, focus bar or a badge, and darkening it
 * costs the contrast of the label on top. Pixels arriving through drawImage are
 * picture content -- the waveform above all -- where blue is the ink, not a
 * backdrop, and exempting it washes the waveform out.
 *
 * Q8 means 256 == 1.0.
 */
struct theme_palette {
    uint8_t  invert_l;        /* 1 = flip lightness, hue and chroma exact       */
    uint8_t  exempt_blue;     /* 1 = spare blues from sat_darken on fills       */
    /* THE SELECTION BLUE, stated rather than transformed. 0 = the generic mapping.
     *
     * #007de1 is one specific colour on this deck and it always carries lettering: it
     * is the selected row in every list AND the lit quick-menu plate. What no pixel
     * transform can know is that the ink lands ON it, so the two have to stay legible
     * together -- and a lightness inversion cannot deliver that, because saturated
     * colours are near fixed points of one (see hue_pull_q8 below). The ink flips and
     * the blue does not, so a light theme ends up with near-black on mid-blue.
     *
     * Measured, white-or-ink on the mapped blue: ORIGINAL 4.18, WHITE 5.96, MOCHA 4.21,
     * AURORA 5.45 -- and NEON 1.29, SANDSTONE 1.65, CYBERPUNK 2.41, which is a selected
     * row you cannot read, on the deck's own rows as much as ours. A theme that lands
     * badly states the colour here; the rest leave it 0 and are transformed as before.
     *
     * exempt_blue is the older, weaker half of the same idea: it spares blues from
     * being DARKENED, which helps a dark theme and cannot lighten anything. */
    uint32_t selection;
    int16_t  sat_darken_q8;   /* 0 = off; 77 is WHITE's tuned value (~0.30)     */
    int16_t  sat_q8;          /* 256 = unchanged; 0 = greyscale                 */
    uint32_t shadow;          /* duotone: what black becomes    (0xRRGGBB)      */
    uint32_t highlight;       /* duotone: what white becomes    (0xRRGGBB)      */
    int16_t  tint_q8;         /* 0 = no duotone; 256 = fully the ramp           */

    /* ---- the palette proper ----
     *
     * A duotone is MONOCHROME by construction: it blends toward a ramp between two
     * anchors, so every colour it can produce lies on one line in RGB space. Give it a
     * palette of three accent hues and it can express exactly none of them -- raising
     * tint_q8 only makes the result more single-hued, which is the trap.
     *
     * So chromatic pixels take a different stage. The deck's own UI already distinguishes
     * things BY hue -- the blue selection, the orange master badge, the red warning, the
     * green marker -- and that structure is worth keeping. Each chromatic pixel is rotated
     * to the NEAREST of these hues, holding its own lightness and chroma, so the deck's
     * distinctions survive while wearing the theme's colours. Greys have no hue to map, so
     * they keep the shadow/highlight ramp above: that is what a duotone is actually good
     * at, and between them the two stages cover every pixel. */
    uint32_t hue[4];          /* accent hues to map the deck's own onto          */
    uint8_t  nhue;            /* 0 = leave chromatic pixels to the duotone       */

    /* How much of the target a mapped pixel actually TAKES. 0 = its hue and nothing
     * else, which is the historical behaviour and what every theme written before this
     * field relies on; 256 = the pixel becomes the palette colour outright.
     *
     * The stage above borrows only a hue ANGLE: the result keeps the source's own
     * lightness and chroma, so the deck decides how light and how saturated everything
     * stays and the palette only says which way round the wheel it points. On a DARK
     * theme that is invisible, because a dark screen is mostly grey and the duotone
     * re-skins all of it -- chromatic pixels are the trim. On a LIGHT theme it is fatal:
     * invert_l leaves saturated colours nearly where they were (they are close to fixed
     * points of a lightness inversion), chromatic pixels return before the duotone ever
     * sees them, and what is left is WHITE with the hues nudged. Three light themes were
     * authored against that and all three converged on the same look, whatever their
     * colours said, because none of the numbers involved could reach the value.
     *
     * Above 0 the target's midpoint and chroma are blended in too, so the palette gets to
     * set how light and how strong its colours are. Not 256 in practice: at full pull
     * every pixel mapped to one hue becomes the SAME colour, and the waveform's internal
     * shading -- which is lightness variation within a hue -- flattens to a silhouette.
     *
     * WHITE deliberately leaves this at 0. Its whole premise is the deck's own hues at
     * the opposite polarity, which is a transform, not a palette. */
    int16_t  hue_pull_q8;
};

/* `struct theme_ui` -- the roles the mods' own controls paint with -- is in
 * ../draw.h, with the rest of the drawing kit: every mod that owns a paint slot
 * reads them, and none of them has any other business with the theme layer. */

/* ================================================================== */
/* The registry                                                       */
/* ================================================================== */

struct mod_theme {
    /* Shown in the MOD SETTINGS value column and in log lines. Not persisted --
     * the settings file stores the index -- so renaming a theme is free. */
    const char *name;

    /* NULL for ORIGINAL. Non-NULL also means "this theme touches image pixels",
     * because there is no useful theme that recolours fills and leaves the
     * sprites sitting next to them stock. */
    const struct theme_palette *palette;

    /* NULL to derive every role from ORIGINAL's through `palette` -- which is what
     * makes ORIGINAL itself exact, since its palette is the identity. Author it only
     * where the generic transform gets a role wrong. */
    const struct theme_ui *ui;

    /* Authored the short way: seven colours, expanded at first use. Beats `palette`
     * derivation and loses to a fully authored `ui`. */
    const struct theme_seed *seed;

    /* Does this theme put the UI on a LIGHT ground?
     *
     * Declared by the theme, never toggled by the user -- a global dark/light switch
     * would double the list while leaving polarity to do all the work, which is exactly
     * why SOLARIZED and KAWAII came out looking like each other. It is a FACT about the
     * theme, and the machinery needs it because two things have no correct answer
     * without it:
     *
     *   - the button stipple's contrast. Its two halves sit 15 levels apart on stock's
     *     dark grey; deriving the dark half by the same RATIO on a bright surface opens
     *     that to 60-odd and the checker stops being one surface.
     *   - which way an accent moves to stay legible. On a dark ground it lifts; on a
     *     light one it has to go darker, and nothing about the colour itself says so.
     */
    uint8_t light;
};

/* The deck's own selection blue and its checker partner, measured. The pair the
 * palette anchors when a theme states a selection of its own. */
#define THEME_DECK_SELECT   0x007de1u
#define THEME_DECK_SELECT2  0x0064a5u
/* draw.h records the lit pair at x0.73; 187/256 is that. */
#define THEME_SELECT2_Q8    187u

/* ---- authoring a theme, the short way ----
 *
 * Eighteen roles is the machinery's view, not an author's. A theme is really a handful
 * of decisions -- what the panel is, what an unlit control is, what lettering is, what
 * "lit" means, what "wrong" means, and three hues that must stay apart -- and the rest
 * are shades of those. So a preset states the seven and roles.c expands them.
 *
 * The three stems are the reason this cannot be fewer: their whole job is to be
 * TELLABLE APART, in the wedge, on the caption and on the bypass icon at once. No rule
 * derives three mutually distinct hues from one accent, so they are named. */
/* No `accent` here, deliberately. A LIT control is not a thing a theme gets to invent:
 * the deck lights its own quick-menu buttons, ours sits in the same bar, and the two
 * have to agree or our button reads as belonging to a different program. So it is
 * DERIVED -- the deck's own lit colour run through this theme's palette -- exactly the
 * way ground, plate and ink are. Authoring it is what made the lit STEMS button mauve on
 * one theme and gold on another while every deck button beside it stayed blue. */
struct theme_seed {
    uint32_t ground;    /* the panel everything sits on                   */
    /* READ BY NOTHING. An unlit control is no more a theme's to invent than a lit
     * one: the X-PAD and STEMS buttons stand in the deck's own band between BEAT
     * LOOP and KEY SHIFT, so the plate is DERIVED -- the deck's own grey through
     * this theme's palette -- and the stipple is the deck's stipple in every
     * theme. An authored value takes the checker's second half, the border and
     * the label's contrast with them.
     *
     * Kept only so a preset that still states one compiles. Delete the field and the
     * seeds' initialisers together. */
    uint32_t plate;
    uint32_t ink;       /* lettering                                      */
    uint32_t alarm;     /* the override / refusal family                  */
    uint32_t stem[3];   /* DRUMS / HARMONICS / VOCALS -- keep them apart  */
};

/* Expand a seed into a full role set. `pal` is the theme's own palette, needed for the
 * roles that are derived from the deck rather than authored; `light` its own .light. */
/* The deck's own role colours. What ORIGINAL expands to, unchanged. */
extern const struct theme_ui k_ui_original;

void theme_ui_expand(struct theme_ui *out, const struct theme_palette *pal,
                     const struct theme_seed *seed, int light);

/* How many themes exist. Free to raise -- what it does NOT control is how many
 * the menu can offer; see "how many are REACHABLE" above. */
#define MOD_THEME_MAX 7

extern const struct mod_theme k_mod_themes[MOD_THEME_MAX];

/* g_theme_id -- the selected theme as an index into k_mod_themes -- is declared
 * above: the MOD SETTINGS row that edits it is generic over `int *` and knows
 * nothing about themes.
 *
 * The selected theme, never NULL: an out-of-range id reads as ORIGINAL rather
 * than indexing off the end, because the id arrives from a file on the eMMC. */
const struct mod_theme *mod_theme(void);

/* The name of a theme, for the menu's value list and for log lines -- an index
 * on its own tells a reader nothing. Out of range reads as ORIGINAL. */
const char *mod_theme_name(int id);

/* ================================================================== */
/* Internal seam (palette.c / image.c / theme.c)                      */
/* ================================================================== */

/* The generic transform. `is_fill` selects the exempt_blue carve-out above.
 * Both forms take the palette explicitly rather than reading the selected theme,
 * so a caller resolves it once per fill or once per image and not once per
 * pixel.
 *
 * ---- the memo ----
 *
 * Why the table is out here rather than private to palette.c: this is asked once
 * per PIXEL of every waveform frame, and at that rate the function call is itself
 * a measurable share of the cost. Inline, the compiler folds the lookup into the
 * pixel loop and keeps the colour in a register; out of line it round-trips
 * through memory and pays a prologue for a hit that is four instructions of work.
 * The miss stays in palette.c, which is where all the actual arithmetic lives.
 *
 * An entry, in one naturally-aligned word so aarch64 cannot tear it:
 *
 *   bits  0..23   the answer, 0xRRGGBB
 *   bits 24..48   the question: (0xRRGGBB << 1) | is_fill
 *   bit  49       occupied
 *
 * Sizing, and the measurements behind it: palette.c. */
#define THEME_MEMO_BITS     10
#define THEME_MEMO_N        (1u << THEME_MEMO_BITS)
#define THEME_MEMO_Q_SHIFT  24
#define THEME_MEMO_OCCUPIED (1ull << 49)
/* Knuth's multiplicative hash. The colours that matter differ in their LOW bits --
 * a waveform's ink is one hue at many levels -- and this is the cheap mix that
 * carries those upward. */
#define THEME_MEMO_SLOT(q)  ((uint32_t)((q) * 2654435761u) >> (32 - THEME_MEMO_BITS))

extern uint64_t g_theme_memo[THEME_MEMO_N];
extern const struct theme_palette *g_theme_memo_pal;
extern unsigned g_theme_memo_hit[2], g_theme_memo_miss[2];

void     theme_memo_report(void);
/* The transform on a separated triplet. Components are updated in place and are
 * each 0..255 on return. A NULL palette is ORIGINAL and leaves them unchanged. */
void theme_palette_rgb(const struct theme_palette *p,
                       uint32_t *pr, uint32_t *pg, uint32_t *pb, int is_fill);

uint32_t theme_palette_slow(const struct theme_palette *p, uint32_t rgb, int is_fill);

/* One colour in, one colour out, 0xRRGGBB. A NULL palette is ORIGINAL: not even an
 * identity pass, which is why a stock deck pays nothing for any of this. */
static inline uint32_t theme_palette_map(const struct theme_palette *p, uint32_t rgb,
                                         int is_fill)
{
    uint32_t q;
    uint64_t ent;

    if (p == NULL)
        return rgb;

    q   = (rgb << 1) | (is_fill ? 1u : 0u);
    ent = __atomic_load_n(&g_theme_memo[THEME_MEMO_SLOT(q)], __ATOMIC_RELAXED);

    /* One compare covers both halves: nothing is stored above bit 49, so shifting
     * the question down puts `occupied` exactly where the constant expects it. The
     * palette check is what makes an entry left by the previous theme a miss -- see
     * g_theme_memo_pal; it is a load of a hot global and a predictable branch. */
    if (p == g_theme_memo_pal &&
        (ent >> THEME_MEMO_Q_SHIFT) ==
            ((uint64_t)q | (THEME_MEMO_OCCUPIED >> THEME_MEMO_Q_SHIFT))) {
        g_theme_memo_hit[is_fill ? 1 : 0]++;
        return (uint32_t)(ent & 0xffffffu);
    }
    return theme_palette_slow(p, rgb, is_fill);
}

uint32_t theme_palette_argb(const struct theme_palette *p, uint32_t argb,
                            int is_fill);

/* Bring one image in line with the theme in force. Cheap and idempotent: the
 * common case is a table hit and a compare.
 *
 * Two entry points, differing only in what the CALLER promises about when the pixels get
 * read. theme_sync_image is for a blit that happens inside the call you are wrapping;
 * _deferred is for pixels consumed after you return -- a FillType image, or a bake the
 * app caches and blits later. Only the caller can know which, and getting it wrong is not
 * cosmetic: a deferred image handed to the plain entry point goes unthemed, and a
 * per-frame buffer handed to _deferred is recoloured on top of our own output. */
void theme_sync_image(const void *image);
void theme_sync_image_deferred(const void *image);

/* image.c's half of install. Returns 0 if the drawImage hook went in. */
int  theme_image_install(void);

/* ---- the waveform, themed at its SOURCE (wave.c) -------------------------
 *
 * The detailed waveform is COMPUTED from a table of eight colours, so it is themed by
 * mapping those eight rather than by transforming a quarter of a million pixels a frame.
 * See wave.c for the table, how it is found, and why the re-assertion is stateless. */

/* [message] Put the theme back into every table that has gone stock. Safe to call at any
 * time and as often as convenient: a table already carrying our colours is left alone. */
void theme_wave_apply(void);

/* [message] A waveform replyer ran -- the track-load event that rebuilds the tables.
 * `style` is which WAVEFORM COLOR mode replied, so a mode change can trigger a rescan. */
void theme_wave_replied(int style);

/* Is the source route actually live? False means no table was found, and the per-pixel
 * path has to keep doing the work -- so a firmware that moves the colours degrades to the
 * old cost rather than to an unthemed waveform. */
int  theme_wave_source_on(void);

/* The colour the source route themed the waveform ground to, 0x00RRGGBB, or zero when it is
 * not theming one -- a dark theme whose ground stays black, or ORIGINAL. A strip baked while
 * this is non-zero comes out of the renderer already carrying it. */
uint32_t theme_wave_ground(void);

/* The same entry with the palette NOT applied -- what a strip baked before the tables were
 * themed carries. img_ground_themed compares a buffer against both. */
uint32_t theme_wave_ground_stock(void);

/* [message] Put the themed ground under a detailed waveform strip, in place. The eight
 * table colours are the INK; the background is the part the renderer never wrote, so it
 * has no colour object and is done here. Idempotent, hence no stash and no restore --
 * see wave.c. */
void theme_wave_ground_fill(void *data, int32_t w, int32_t h,
                            int32_t line_stride, int32_t pixel_stride);

/* juce::LowLevelGraphicsSoftwareRenderer virtuals we repoint. */
#define THEME_SLOT_SETFILL  (18 * 8)
#define THEME_SLOT_DRAWIMG  (25 * 8)
/* fillRect(Rectangle<int>, bool) -- read off juce::Graphics::fillRect(int,int,int,int),
 * which loads *(vtable + 0xa8) and calls it. Every rect the UI paints goes through it. */
#define THEME_SLOT_FILLRECT (21 * 8)
/* The rest of the shape primitives, ordered as juce::LowLevelGraphicsContext declares
 * them. Anchored at both ends rather than counted from one: fillRect(int) at 0xa8 is
 * read off Graphics::fillRect, drawImage at 0xc8 is already in use, and these are the
 * three slots between. */
#define THEME_SLOT_FILLRECTF    (22 * 8)   /* fillRect(Rectangle<float>)   */
#define THEME_SLOT_FILLRECTLIST (23 * 8)   /* fillRectList(RectangleList&) */
#define THEME_SLOT_FILLPATH     (24 * 8)   /* fillPath(Path&, transform)   */


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_THEME_H */
