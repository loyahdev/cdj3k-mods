// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/wavewait.c - "the picture cannot follow the faders yet".
 *
 * A fader moves the AUDIO the moment it is touched, and the waveform some seconds
 * later: the stems landing only raises a flag, and the worker then re-decodes the
 * track to derive drums, analyses it into per-column band shares, and separately
 * waits for a stable copy of the deck's own columns -- ~2.8 s of settle before the
 * capture can even bind (STABLE_POLLS x POLL_TICKS in ../../wave.h) with the
 * analysis on top. For all of that the faders are live and the picture is frozen,
 * with nothing on screen to say so.
 *
 * THE GATE IS ONE FLAG. wave_stems.c's worker does `if (!wave_g_have_analysis)
 * continue;` before anything reaches the array, and the per-style captures are
 * upstream of that -- the 3-band copy has to bind before the analysis can run at
 * all. So this reads that flag and nothing else, and no waveform source is touched
 * to expose it.
 *
 * Read from the message thread while the worker owns the write. Deliberate: a stale
 * read costs one frame of an indicator, which is the cheapest thing in the file.
 */
#include "stem/ui/ui.h"
#include "wave/wave.h"

/* ---- the glyph -----------------------------------------------------------
 *
 * The deck's own sampling-rate icon, skin16/picture/browse/LIST-2, which draws a
 * sine crossing an axis -- so it reads as "waveform" rather than as a symbol that
 * has to be learned.
 *
 * EMBEDDED AS RUNS, not loaded. The source is 25x34 white with an alpha ramp, i.e.
 * an alpha mask rather than artwork, so the whole of it is 420 bytes of run-length
 * and the colour is ours to choose. Loading the file instead would need the deck's
 * skin loader or a juce::Image, and would buy a bitmap that cannot follow the theme
 * without being transformed on every blit.
 *
 * Max-pooled to 18x24 rather than resampled: the strokes are one pixel wide at the
 * source size, so an averaging filter takes them below one pixel and the glyph goes
 * to grey mush. Taking the strongest sample in each cell keeps a stroke a stroke.
 * It is also cheaper than the original -- 105 runs against 160.
 *
 * That footprint is the warn badge's (18x20), which is the size that reads as a mark
 * rather than as something to press.
 *
 * TRIMMED TO THE INK. The source carries four blank rows above the glyph and four
 * below; kept, they are padding that spends the cell's height without drawing anything,
 * and every measurement against the button below becomes four pixels off what it says.
 * The table holds the ink and nothing else, so WAVEWAIT_H is what is actually on
 * screen. */
#define WAVEWAIT_W      18
#define WAVEWAIT_H      16

struct wavewait_run { uint8_t y, x, len, alpha; };

static const struct wavewait_run k_glyph[] = {
    { 0, 3, 1, 80}, { 0, 4, 2,207}, { 0,12, 2,207}, { 0,14, 1, 80},
    { 1, 2, 1, 32}, { 1, 3, 1,240}, { 1, 4, 1, 64}, { 1, 5, 1,240},
    { 1, 6, 1, 48}, { 1,11, 1, 48}, { 1,12, 1,240}, { 1,13, 1, 64},
    { 1,14, 1,240}, { 1,15, 1, 32}, { 2, 2, 1,144}, { 2, 3, 1,128},
    { 2, 5, 1,112}, { 2, 6, 1,160}, { 2,11, 1,160}, { 2,12, 1,112},
    { 2,14, 1,128}, { 2,15, 1,144}, { 3, 2, 1,255}, { 3, 3, 1, 64},
    { 3, 5, 1, 48}, { 3, 6, 1,255}, { 3,11, 1,255}, { 3,12, 1, 48},
    { 3,14, 1, 64}, { 3,15, 1,255}, { 4, 2, 1,255}, { 4, 6, 1,224},
    { 4, 7, 1, 32}, { 4,10, 1, 32}, { 4,11, 1,224}, { 4,15, 1,255},
    { 5, 2, 1,255}, { 5, 6, 1,192}, { 5, 7, 1, 64}, { 5,10, 1, 64},
    { 5,11, 1,192}, { 5,15, 1,208}, { 6, 2, 1,255}, { 6, 6, 1,192},
    { 6, 7, 1, 64}, { 6,10, 1, 64}, { 6,11, 1,192}, { 6,15, 1,192},
    { 7, 0, 1, 64}, { 7, 1, 1, 63}, { 7, 2, 1,255}, { 7, 3, 3, 63},
    { 7, 6, 1,207}, { 7, 7, 1,111}, { 7, 8, 2, 63}, { 7,10, 1,111},
    { 7,11, 1,207}, { 7,12, 3, 63}, { 7,15, 1,207}, { 7,16, 2, 63},
    { 8, 0,18,255}, { 9, 2, 1,255}, { 9, 6, 1,192}, { 9, 7, 1, 64},
    { 9,10, 1, 64}, { 9,11, 1,192}, { 9,15, 1,192}, {10, 2, 1,255},
    {10, 6, 1,192}, {10, 7, 1, 64}, {10,10, 1, 64}, {10,11, 1,192},
    {10,15, 1,192}, {11, 2, 1,192}, {11, 6, 1,128}, {11, 7, 1,112},
    {11,10, 2,128}, {11,15, 1,128}, {12, 2, 1,192}, {12, 6, 1,128},
    {12, 7, 1,160}, {12,10, 1,160}, {12,11, 1,128}, {12,15, 1,176},
    {13, 2, 1,160}, {13, 6, 1, 64}, {13, 7, 1,207}, {13,10, 1,208},
    {13,11, 1, 48}, {13,15, 1,224}, {14, 1, 1, 16}, {14, 2, 1,239},
    {14, 7, 1,224}, {14,10, 1,224}, {14,15, 1,224}, {14,16, 1, 48},
    {15, 0, 1,255}, {15, 1, 1,160}, {15, 2, 1,144}, {15, 7, 1,207},
    {15, 8, 2,255}, {15,10, 1,207}, {15,15, 1,112}, {15,16, 1,207},
    {15,17, 1,255},
};

#define N_WAVEWAIT_RUN  ((int)(sizeof(k_glyph) / sizeof(k_glyph[0])))

/* ---- what it is saying ----------------------------------------------------
 *
 * Two tiers, and the MOTION carries the difference rather than the colour: breathing
 * means the picture is on its way, still means it is not coming. One thing to learn
 * instead of two, and the still state settles onto the quiet end of its own pulse so
 * the change reads as the mark going to rest.
 *
 * Amber, through the warn role: nothing is broken, the picture is merely behind the
 * sound. Red is the deck's colour for something the DJ has to act on, and there is
 * nothing to act on here. */
enum wavewait_state { WAVEWAIT_OFF, WAVEWAIT_PULSE, WAVEWAIT_STILL };

/* Scales the glyph's OWN alpha, so the antialiasing survives at both levels. */
#define WAVEWAIT_A_DIM      90
#define WAVEWAIT_A_BRIGHT  255
/* Ticks per half-cycle. Three times the refusal's, deliberately: a fast flash is how
 * this UI says no, and a slow breath is how it says wait. */
#define WAVEWAIT_PULSE     (MOD_BLINK_PERIOD * 3)
/* When to stop promising. The capture can genuinely never bind -- the deck keys a
 * waveform reply on the browse REQUEST, so a track loaded from an earlier index in the
 * same list is never replied for at all (see wave.h) -- and a mark that breathes for
 * ever is one the DJ learns to ignore.
 *
 * Must outlast the waveform's own capture give-up, which is CAPTURE_GIVEUP_POLLS at
 * POLL_TICKS of its APPLY_MS loop, about eleven seconds. Going still before that would
 * say "not coming" about a picture that is still on its way. */
#define WAVEWAIT_GIVEUP    700

/* How far the mark floats above BYPASS, in pixels.
 *
 * It is an indicator standing over a button, and sitting at the cell's centre the two
 * read as one stacked control. Lifting it off the button is what separates them.
 *
 * Expressed as the clearance rather than as an offset from centre, because the
 * clearance is the thing being chosen.
 *
 * AT ITS CEILING, which is CAPTION_H - WAVEWAIT_H: the ink's top row is flush with the
 * top of the row, and there is no further to go without leaving the cell. What lies
 * immediately above is NOT ours -- the deck's loop indicator sits there, and it is what
 * holds ROW_RISE at 3 -- so if the mark reads as crowded against it on screen, this is
 * the number to bring down. Clamped rather than trusted, so a later change to CAPTION_H
 * cannot push the glyph out of its own cell instead. */
#define WAVEWAIT_GAP        (CAPTION_H - WAVEWAIT_H)

static uintptr_t g_wavewait;
static uintptr_t g_vt[2 + VT_CLONE_SLOTS];
static uintptr_t g_vptr;
static int       g_state;
static int       g_waited;      /* ticks in PULSE; also the pulse's own phase */

/* Derived from the same counter the poll advances, never stored: a level cached beside
 * the counter that decides it is a level that can disagree with it. */
static uint32_t wavewait_alpha_q8(void)
{
    if (g_state == WAVEWAIT_STILL)
        return WAVEWAIT_A_DIM;
    return ((g_waited / WAVEWAIT_PULSE) & 1) ? WAVEWAIT_A_DIM : WAVEWAIT_A_BRIGHT;
}

/* The row is transparent -- COL_ROW_BG is 0x00000000 and the deck's waveform is what
 * lies behind it -- so there is no background to pre-blend against. Each run carries
 * its own alpha into juce::Colour and the renderer blends it over whatever is there,
 * which is what the source PNG would have done. */
static void wavewait_paint(void *self, void *g)
{
    uint32_t rgb = mod_ui()->warn & 0x00ffffffu;
    uint32_t lvl = wavewait_alpha_q8();
    int32_t b[4];
    int i, x0, y0;

    if (stems_bounds((uintptr_t)self, b) != 0) return;
    if (b[2] < WAVEWAIT_W || b[3] < WAVEWAIT_H) return;
    x0 = (b[2] - WAVEWAIT_W) / 2;
    y0 = b[3] - WAVEWAIT_H - WAVEWAIT_GAP;
    if (y0 < 0) y0 = 0;

    for (i = 0; i < N_WAVEWAIT_RUN; i++) {
        const struct wavewait_run *r = &k_glyph[i];
        uint32_t a = (uint32_t)r->alpha * lvl / 255u;

        if (!a) continue;
        mod_gfx_colour(g, (a << 24) | rgb);
        mod_gfx_fill(g, x0 + r->x, y0 + r->y, r->len, 1);
    }
}

static int wavewait_vt_ready(void)
{
    if (g_vptr) return 1;
    if (!stems_label_vt_ready()) return 0;
    memcpy(g_vt, stems_g_label_vt, sizeof(g_vt));
    g_vt[2 + VT_SLOT_PAINT / sizeof(uintptr_t)] = (uintptr_t)wavewait_paint;
    g_vptr = (uintptr_t)&g_vt[2];
    return 1;
}

/* NOT a child of BYPASS, which is the button it sits over. A mark inside that button
 * would take its press, and this is an indicator: built on the controls container, a
 * finger on it lands on a container that does nothing. Not clickable either, so it
 * keeps stock Label::mouseDown -- the empty stub. */
uintptr_t stems_wavewait_build(uintptr_t parent, int x, int y, int w, int h)
{
    if (!wavewait_vt_ready()) return 0;
    g_wavewait = stems_label(parent, "", FONT_CAPTION, 0x00000000u, mod_ui()->warn, 0,
                             x, y, w, h);
    if (!g_wavewait) return 0;
    *(uintptr_t *)g_wavewait = g_vptr;
    stems_set_visible(g_wavewait, 0);
    g_state = WAVEWAIT_OFF;
    g_waited = 0;
    return g_wavewait;
}

void stems_wavewait_poll(void)
{
    int want;

    if (!g_wavewait) return;
    /* Stems resident and in circuit, and the analysis not there yet. Under BYPASS the
     * deck's own waveform is on screen BY DESIGN and the faders are inert, so nothing
     * is pending; with no stems at all there is nothing for the picture to follow. */
    want = stems_ready() && !stems_g_bypass_on && !wave_g_have_analysis;
    if (!want) {
        if (g_state != WAVEWAIT_OFF) {
            g_state  = WAVEWAIT_OFF;
            g_waited = 0;
            stems_set_visible(g_wavewait, 0);
        }
        return;
    }
    if (g_state == WAVEWAIT_OFF) {
        g_state  = WAVEWAIT_PULSE;
        g_waited = 0;
        stems_set_visible(g_wavewait, 1);
        return;                     /* addAndMakeVisible already drew the first half */
    }
    if (g_state == WAVEWAIT_STILL)
        return;                     /* at rest: nothing moves, so nothing repaints */
    if (++g_waited >= WAVEWAIT_GIVEUP) {
        g_state = WAVEWAIT_STILL;
        stems_repaint(g_wavewait);
        MDBG("stems: waveform analysis has not arrived -> the picture will not follow"
             " the faders on this track\n");
        return;
    }
    /* One repaint per half-cycle. At the display tick an every-frame repaint of 105
     * fills is a shimmer and a cost, for a mark that changes twice a second. */
    if (g_waited % WAVEWAIT_PULSE == 0)
        stems_repaint(g_wavewait);
}
