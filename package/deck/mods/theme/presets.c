// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * presets.c - the themes.
 *
 * The only file adding a theme has to touch. See struct theme_palette in
 * theme.h for what each field does and in what order; nothing here is code, so
 * a new theme cannot break the machinery, only look wrong.
 *
 * A note on what these are worth. WHITE is the one that has been looked at on a
 * real panel and tuned there, and its two numbers are the result of that. It is
 * not quite bit-identical to what shipped: the darkening factor was 30/25500 and
 * is now 77/65280 because the model works in Q8, which is at most ONE off per
 * channel across every (chroma, value) pair -- checked exhaustively, not
 * assumed. The other three are authored blind against the model. It predicts
 * polarity and saturation well and taste not at all, so expect to move
 * `tint_q8` first and `sat_q8` second.
 */
#include "theme/theme.h"

/* The tuned one. Lightness inversion with the accent-darkening that a light
 * background needs, and the blue carve-out that keeps selection rows readable.
 * No duotone: the deck's own hues are the point, only the polarity changes. */
static const struct theme_palette k_pal_white = {
    .invert_l      = 1,
    .exempt_blue   = 1,
    .sat_darken_q8 = 77,      /* ~0.30 */
    .sat_q8        = 256,
};



/* ---- the authored themes ----
 *
 * These are AUTHORED: colours chosen, not derived. A theme built as a TRANSFORM --
 * lightness inversion plus a wash -- cannot be kept distinct from another built the
 * same way, because two transforms of one source at the same polarity land in the
 * same place however they are tuned. Authored themes share no source and so cannot
 * converge.
 *
 * Each also carries a `theme_palette`: the seed dresses only OUR controls, and the
 * deck's own screens need the duotone or the panel goes neon while the browser stays
 * stock.
 *
 * The duotone's HIGHLIGHT carries a theme's identity across the app and must have
 * CHROMA. It is the ACCENT lifted ~44% toward white -- lifted rather than raw because
 * white lettering lands at the top of that ramp, and a raw accent puts text at full
 * chroma and mid-brightness. Pointing it at the seed's ink instead yields a near-black
 * to near-white ramp (ink is a neutral pale by construction, being lettering), which is
 * the identity transform: every theme comes out a faint cast.
 *
 * The ramp only gets the GREYS -- chrome, panels, lettering. Anything with a hue goes
 * through .hue, so the deck's blue/orange/red/green rotate onto the nearest of the
 * seed's three stems and the distinctions it draws with colour survive. One identity,
 * not two that can drift.
 *
 * sat_q8 stays at unity: the mapping does the work, and multiplying the deck's existing
 * chroma on top of it blows out the contrast. */

/* Sky is the accent and yellow is the alarm, not the other way round. The palette has no
 * red, and warn/refuse have to be tellable from "this is on" -- so it borrows the deck's
 * own grammar, where blue means working and amber means override. That also puts the
 * brightest colour in the set on the things that want to be noticed rather than on every
 * lit control.
 *
 * #9a9f17 is the plate. It is the only mid-tone here, which is what a plate needs, and it
 * cannot serve as a hue: at 44/256 it sits five steps from the yellow's 39, so the two
 * would map to each other and one of the three slots would be wasted. */
static const struct theme_seed k_seed_cyberpunk = {
    .ground = 0x00060e, .ink    = 0xdff3f7,
    .alarm  = 0xfee801,                           /* yellow: the override family */
    /* NOT the alarm's yellow for DRUMS, and not a second cyan for VOCALS. Both were:
     * drums WAS the alarm colour exactly, so the bypass icon and the drums wedge beside
     * it were one colour, and sky against teal came to dE 31 where three stems need 40
     * to keep naming which is which. */
    .stem   = { 0xff2e88, 0x54c1e6, 0x2bf58a },   /* magenta / sky / spring */
};
static const struct theme_palette k_pal_cyberpunk = {
    .selection     = 0x007ba5,   /* white-cyan ink on the mapped #00a7e0 was 2.41; this is 4.2, the deck's own */
    .sat_q8 = 256,          /* explicit: see the trap noted in palette.c */
    .shadow = 0x00060e, .highlight = 0x9fdcf1, .tint_q8 = 96,
    .hue = { 0xfee801, 0x54c1e6, 0x39c4b6 }, .nhue = 3,
};

static const struct theme_seed k_seed_neon = {
    .ground = 0x0b0d17, .ink    = 0xc9d1d9,
    /* Orange, not the pink: the pink is DRUMS, and an alarm that IS a stem puts the
     * bypass icon and that stem's wedge on screen in one colour. Orange is also the
     * deck's own word for an override, which is the family this seeds. */
    .alarm  = 0xff9100,
    .stem   = { 0xff2daa, 0x00e5ff, 0x7c4dff },   /* pink / cyan / violet */
};
static const struct theme_palette k_pal_neon = {
    .selection     = 0x006874,   /* its ink on the mapped #00cae0 was 1.29; this is 4.2, the deck's own */
    .sat_q8 = 256,          /* explicit: see the trap noted in palette.c */
    .shadow = 0x0b0d17, .highlight = 0x70f0ff, .tint_q8 = 96,
    .hue = { 0xff2daa, 0x00e5ff, 0x7c4dff }, .nhue = 3,
};

/* Catppuccin Mocha, straight off the published palette:
 *
 *   Base #1e1e2e   Text #cdd6f4   Mauve #cba6f7   Peach #fab387
 *   Red  #f38ba8   Blue #89b4fa   Green #a6e3a1
 *
 * Mauve is the accent because that is Mocha's own signature, and it keeps the lit control
 * clear of Blue, which is a stem here. Peach takes the alarm: the deck already says
 * "override" in orange (MASTER PLAYER, the +-10 badge), so the palette's warm entry lands
 * exactly where the deck's grammar wants it. Red/Blue/Green as the stems preserves
 * ORIGINAL's own DRUMS/HARMONICS/VOCALS ordering, and all three are more than 100 degrees
 * apart -- the widest separation of any theme here.
 *
 * The FOURTH hue is Peach, and it is what keeps the deck legible rather than pretty: with
 * three hues the deck's oranges fall nearer Red than Green and the override colour stops
 * being a colour of its own. hue[] holds four.
 *
 * tint_q8 is 160, not the 96 the older themes use. Those tint the deck; this one is
 * supposed to BE Mocha, and at 96 the deck's background lands two thirds of the way back
 * to black. Safe to push, because the ramp only ever gets the greys -- everything with a
 * hue goes through hue[] and is untouched by it. */
static const struct theme_seed k_seed_mocha = {
    /* Mocha's own Base and Text, with plate derived from the palette below -- the method
     * is written up under SANDSTONE. plate lands between Surface0 and Surface1, which is
     * the check that the anchors below are the right ones. */
    .ground = 0x1e1e2e, .ink    = 0xcdd6f4,
    .alarm  = 0xfab387,                           /* Peach                  */
    .stem   = { 0xf38ba8, 0x89b4fa, 0xa6e3a1 },   /* Red / Blue / Green     */
};
static const struct theme_palette k_pal_mocha = {
    .sat_q8 = 256,          /* explicit: see the trap noted in palette.c */
    /* Solved, not chosen: these are the anchors for which the deck's own black and white
     * come out at Mocha's Base and Text. Neither is a palette entry itself -- they are the
     * ends of a ramp that only its middle is ever read from. */
    .shadow = 0x30304a, .highlight = 0xafbded, .tint_q8 = 160,
    .hue = { 0xf38ba8, 0x89b4fa, 0xa6e3a1, 0xfab387 }, .nhue = 4,
};

/* ---- AURORA ----
 *
 * Green, blue and violet ribbons over a near-black, with a pale mint at the top of the
 * range -- the colours an aurora is made of, and the name is the palette rather than
 * anything it was lifted from. Seven, as given:
 *
 *   #141414 ground   #292929 plate   #F0FEF9 mint
 *   #00D26C green    #00E575 green   #006AFB blue    #B869FF violet
 *
 * The two greys are PANEL COLOURS, and putting them in the ramp instead is what makes
 * this palette look like the screen behind smoke. A duotone is linear in lightness, so
 * anchoring black at #141414 while white lands on the mint fixes the slope at 0.86 and
 * lifts every grey on the deck by twenty levels -- the blacks stop being black and the
 * whole range compresses. There is no tuning out of it; it follows from the two ends.
 *
 * So the ramp runs the FULL range, black to the mint, and the greys keep their exact
 * relative values:
 *
 *   shadow     #000000 and tint_q8 at 256, so the deck's black chrome stays black.
 *   highlight  #F0FEF9 itself, not an anchor solved for it. At full tint a grey is the
 *              ramp indexed by its own lightness, which is a per-channel gain of
 *              240/254/249 -- contrast untouched, everything carrying the mint.
 *
 * The greys are where they belong instead, as the panel they describe:
 *
 *   ground  #141414, our panel, sitting on the deck's now-black screen the way Engine's
 *           own panels sit on its canvas.
 *   plate   #292929, an unlit control. Everywhere else in this file plate is DERIVED, so
 *           that ours match the deck's own buttons in the same bar; here the derived
 *           value is #2f3130 and the palette's is #292929, six levels under it. Close
 *           enough to read as one family, and it is the colour the palette names.
 *   ink     #F0FEF9, the mint, which is also the top of the ramp -- so our lettering and
 *           the deck's are the same white.
 *
 * Assignments:
 *
 *   stems   violet / blue / green, at 272, 215 and 151 degrees. Forced: those are the
 *           only three hues in the palette, and they hold ORIGINAL's DRUMS / HARMONICS /
 *           VOCALS ordering. VOCALS takes #00E575 rather than #00D26C -- same angle to
 *           within a fifth of a degree, and the brighter one is the one being read
 *           literally.
 *   alarm   #00E575. The bright green is what this palette accents with, so warn, refuse
 *           and the mode plate are green and the violet is left as a stem. It costs the
 *           usual collision -- alarm and VOCALS now share a hue, as CYBERPUNK's yellow
 *           does -- and what it must NOT collide with is the lit colour, which is derived
 *           from the deck's own blue and lands on the blue stem.
 *   hue[]   green / blue / violet -- the two band colours plus the third hue.
 *
 * What the amber slot does is the point of the theme rather than a cost of it. The deck
 * draws its waveform blue against amber; with no warm entry in the palette the amber
 * lands on the green, so the waveform comes out blue over green, which is the whole
 * reason for the set.
 *
 * The deck's own RED family is what the palette cannot place. Nearest-hue puts it on the
 * violet -- the boundary between green and violet sits at 22/256, so ambers and yellows
 * go green and reds and the deeper oranges (the cue line, hot-cue markers, the +-10
 * badge) go violet. Dropping the violet from hue[] does not fix it, it makes it worse:
 * red is then 103 from the blue against 107 from the green, so the whole family lands on
 * the blue and its markers disappear into the waveform, and #d93025 sits at a dead tie
 * that only list order resolves. A warm hue would fix it and the palette has none. */
static const struct theme_seed k_seed_aurora = {
    .ground = 0x141414, .ink    = 0xf0fef9,
    /* The violet, which DRUMS gives up. The alarm was the green -- the same word as
     * VOCALS -- so the bypass icon, the refusal and the X-PAD's value all wore the
     * vocals colour, and the pad's armed flags came out identical to its value. */
    .alarm  = 0xd451ff,                           /* violet: the override family  */
    /* Pink rather than violet for DRUMS: violet against this blue is dE 33, and three
     * stems that name three parts need to be further apart than that. */
    .stem   = { 0xff4fc3, 0x006afb, 0x00e575 },   /* pink / blue / green          */
};
static const struct theme_palette k_pal_aurora = {
    .sat_q8 = 256,          /* explicit: see the trap noted in palette.c */
    .shadow = 0x000000, .highlight = 0xf0fef9, .tint_q8 = 256,
    .hue = { 0x00d26c, 0x006afb, 0xb869ff }, .nhue = 3,
};

/* ---- SANDSTONE ----
 *
 * An authored palette, five colours:
 *
 *   #E8705D coral   #FE985F orange   #FEE5A0 butter   #6C7C6B sage   #393F61 navy
 *
 * It carries no background, so ground and ink are deduced FROM THE PALETTE rather than
 * derived from the deck's black and white through an inverting duotone. A theme built
 * that way has only tint strength and three pigments left as free variables, so every
 * one lands in the same place. Only plate stays derived, because it has to match the
 * deck's own buttons.
 *
 *   ground  #fbf0d9  the butter at ~92% lightness, less chroma. The butter itself is
 *           too strong for a full screen and leaves nothing paler to accent against.
 *   ink     #262a44  the navy deepened. HARMONICS wants the navy exactly as given, and
 *           a stem the colour of ordinary lettering stops naming its wedge; deepening
 *           separates them while keeping the hue.
 *
 * Assignments:
 *
 *   stems   coral / navy / sage, at 8, 231 and 117 degrees -- the only three both dark
 *           enough to be ink on cream and far enough apart to stay tellable. They hold
 *           ORIGINAL's DRUMS-red, HARMONICS-blue, VOCALS-green ordering.
 *   alarm   the orange. 14 degrees off the coral, too close for a hue slot, and "orange
 *           means override" is the deck's own grammar (MASTER PLAYER, +-10).
 *   hue[]   coral / orange / sage / navy.
 *
 * The butter is NOT a hue target. It looks like the safe choice -- 36 degrees clear of
 * the coral where the orange is only 14 -- but the amber slot is PROPORTIONALLY half the
 * screen (the waveform is blue against orange), so pointing it at a pale yellow turns
 * the deck cream-and-gold and leaves the palette's reds in a few badges.
 *
 * The 14 degrees suffice because nearest-hue is a comparison, not a threshold: the deck's
 * red at 0 is 8 from the coral and 22 from the orange; its amber at 35 is 13 from the
 * orange and 27 from the coral. Each lands on the right one.
 *
 * The butter is upstream of all this: it is what the page was deduced from, so it is
 * already everywhere, as the paper.
 */
static const struct theme_seed k_seed_sandstone = {
    /* plate derived from the palette below -- see the note on ground/plate/ink further
     * down; the other two are the palette's own. */
    .ground = 0xfbf0d9, .ink    = 0x262a44,
    /* Amber rather than the salmon it was: at dE 21 from the coral DRUMS the bypass
     * icon sat inside the drums wedge's own colour. The deck's own pair is 27 apart
     * and this is 45. */
    .alarm  = 0xfdb03f,                           /* amber                   */
    /* The sage lifted off the navy -- they met at dE 40, which is the floor, and a
     * floor is not a margin. */
    .stem   = { 0xe8705d, 0x393f61, 0x869a5f },   /* coral / navy / sage     */
};
static const struct theme_palette k_pal_sandstone = {
    .selection     = 0x7c89ce,   /* near-black ink on the mapped #2c40b0 was 1.65; this is 4.2, the deck's own */
    /* invert_l for the same reason WHITE has it: without the flip the deck's own chrome
     * stays dark and the mod's paper panel floats on top of a black deck. exempt_blue
     * comes with it -- it is what keeps a lit control bright enough to carry the dark
     * lettering that the same inversion just produced. */
    .invert_l      = 1,
    .exempt_blue   = 1,
    /* Well above WHITE's 77. Saturated colours are near fixed points of a lightness
     * inversion, so the deck's waveform arrives on the page as bright as it left --
     * electric on cream. Darkening turns it into a pigment, which is what this palette
     * is made of. Costs nothing on the fills, where the blues are exempt anyway. */
    .sat_darken_q8 = 115,
    .sat_q8        = 256,
    /* Solved BACKWARDS from the two colours above: these are the anchors for which the
     * deck's own black and white come out at #fbf0d9 and #262a44 after the inversion.
     * Note where the dark one landed -- #3d436d is within a few levels of the palette's
     * own navy, which is the check that the deduction was in family rather than merely
     * arithmetic.
     *
     * They are read AFTER the inversion, which is the order palette.c applies them in:
     * the deck's black chrome has become white by the time the duotone indexes it, so it
     * is `highlight` that lands on the paper and `shadow` on the lettering. Written the
     * other way round -- the way a dark theme writes them -- a light theme comes out
     * inverted twice and looks like a bug in the transform rather than a choice. */
    .shadow = 0x3d436d, .highlight = 0xf9e7c2, .tint_q8 = 160,
    .hue = { 0xe8705d, 0xfe985f, 0x6c7c6b, 0x393f61 }, .nhue = 4,
    /* The field this theme exists to use. Without it a light theme cannot re-colour, only
     * tint: the deck keeps its own lightness and chroma and the palette gets a hue nudge,
     * which is WHITE with extra steps. At 128 the palette's own value and strength lead
     * while the deck's shading still comes through -- push it to 256 and every pixel on a
     * given hue becomes one flat colour, taking the waveform's relief with it. */
    .hue_pull_q8 = 128,
};

/* Order IS the on-disk format -- the settings file stores an index. Append only;
 * see theme.h. Index 0 must stay the palette-less one: every hook reads a NULL
 * palette as "do nothing", and every clamp falls back to 0. */
/* The third column is the UI roles -- what the MOD's own controls wear. NULL derives
 * them from ORIGINAL's through the palette beside it, which is coherent with the rest of
 * the UI and is the right starting point for every theme. Author one only where the
 * generic transform gets a role wrong, and in practice that means stem[]: a duotone pulls
 * all three stem hues onto one ramp, and once two of them meet the colour has stopped
 * saying which stem it is. See struct theme_ui. */
const struct mod_theme k_mod_themes[MOD_THEME_MAX] = {
    /*  name           palette              ui    seed                  light */
    { "ORIGINAL",    NULL,                  NULL, NULL,                 0 },
    { "WHITE",       &k_pal_white,          NULL, NULL,                 1 },
    { "CYBERPUNK",   &k_pal_cyberpunk,      NULL, &k_seed_cyberpunk,    0 },
    { "NEON",        &k_pal_neon,           NULL, &k_seed_neon,         0 },
    { "MOCHA",       &k_pal_mocha,          NULL, &k_seed_mocha,        0 },
    { "AURORA",      &k_pal_aurora,         NULL, &k_seed_aurora,       0 },
    { "SANDSTONE",   &k_pal_sandstone,      NULL, &k_seed_sandstone,    1 },
};

const struct mod_theme *mod_theme(void)
{
    int id = __atomic_load_n(&g_theme_id, __ATOMIC_RELAXED);

    /* The id comes off the eMMC, so it is input, not a fact. Anything out of
     * range reads as ORIGINAL: a settings file written by a build that had more
     * themes leaves this one stock rather than indexing past the table. */
    if (id < 0 || id >= MOD_THEME_MAX)
        id = 0;
    return &k_mod_themes[id];
}

const char *mod_theme_name(int id)
{
    if (id < 0 || id >= MOD_THEME_MAX)
        id = 0;
    return k_mod_themes[id].name;
}
