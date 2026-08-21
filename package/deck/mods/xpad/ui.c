// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/ui.c - the quick-menu button, the band it opens, and the clocks.
 *
 * The shared contract is in xpad.h.
 *
 * The button and the strip are juce::Labels with a cloned vtable, the same
 * technique the stem row uses (see stem/ui/ui.h): a Label already paints a
 * background, an outline and centred text, which is the whole of a flat button,
 * and cloning its vtable into an array of ours lets one slot be replaced
 * without touching any stock Label.
 */
#include "xpad/xpad.h"
#include "kit/mod.h"
#include "kit/menu.h"

/* ---- what the parts share ------------------------------------------------ */

const char *const xpad_div_name[XP_BRICKS]  = { "1/16", "1/8", "1/4", "1/2", "1", "2" };
const float       xpad_div_beats[XP_BRICKS] = { 0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f };

struct xpad_touch xpad_g_touch = { .div = XP_DIV_NONE };
int       xpad_g_hold;
int       xpad_g_overdub;
int       xpad_g_vol = 80;

uintptr_t xpad_g_btn, xpad_g_strip, xpad_g_pad, xpad_g_pane;
int       xpad_g_open;
/* ENABLE X-PAD. OFF by default, so a deck that never opts in gets its band slot
 * and its track title back and behaves exactly as stock. The same shape as
 * ENABLE STEMS: a runtime toggle, re-asserted from sync rather than read once. */
int       xpad_g_on;

static uintptr_t xpad_g_orig_anchor_paint, xpad_g_orig_tick;
static uintptr_t xpad_g_btn_vt[VT_CLONE_WORDS], xpad_g_btn_vptr;
/* A finger is on the band button. See xpad_btn_mousedown. */
static int xpad_g_btn_held;
/* A press the button would not honour, counting itself out. See MOD_BLINK_* in
 * ../draw.h for what the number means; zero is at rest. */
static int xpad_g_refuse;
static int       xpad_g_built, xpad_g_building, xpad_g_api_ok;
static int       xpad_g_dirty;
unsigned  xpad_g_ticks;

/* ---- the band client ----------------------------------------------------- */

/* Ours alone, and past both the app's four and the stem row's 5. The kit reads
 * this back out of the app's mode register to tell "still ours" from "taken",
 * so two clients sharing a number would be two clients it cannot tell apart. */
#define XPAD_BAND_MODE 6

/* CLOSING IS WHAT STOPS THE FEATURE, whichever way it closes. Every voice goes
 * quiet, the gesture is dropped, the bar is freed, and the three borrowed
 * controls are back to being MEMORY, CALL/DELETE and the brake -- they read
 * xpad_open() on every gesture, so there is nothing to unwind for them.
 *
 * HOLD goes with it: it is a property of a gesture that no longer exists, and a
 * latch surviving a close would take hold of the next thing the DJ touched. */
static void xpad_stop(void)
{
    xpad_silence();
    xpad_overdub_set(0);
    xpad_g_hold        = 0;
    xpad_g_touch.div   = XP_DIV_NONE;
    xpad_g_touch.semis = 0.0f;
    xpad_g_touch.held  = 0;
    xpad_g_touch.snapped = 0;
}

static void xpad_band_closed(void)
{
    if (!xpad_g_open) return;
    MDBG("xpad: the band went elsewhere -> closing\n");
    xpad_g_open = 0;
    xpad_stop();
    xpad_sync();
}

static void xpad_reslot(int32_t x)
{
    if (xpad_g_btn)
        ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
            ((void *)xpad_g_btn, x, KIT_BAND_SLOT_Y,
             KIT_BAND_SLOT_W, KIT_BAND_SLOT_H);
}

KIT_BAND(k_band_xpad,
         .name = "xpad", .mode = XPAD_BAND_MODE, .closed = xpad_band_closed,
         .shown = &xpad_g_on, .reslot = xpad_reslot, .order = 1);

int xpad_open(void) { return xpad_g_open; }

/* ---- the button ---------------------------------------------------------- */

/* The deck's own title-bar dress, which is what STEMS wears and what the three
 * stock buttons beside it wear: a stippled surface and a short bar flush with the
 * bottom edge.
 *
 * BOTH GO DOWN BEFORE Label::paint. Label draws its background and its lettering
 * in one call, so a stipple laid afterwards falls across the word -- which is why
 * the Label's own background colour stays transparent and the plate is painted
 * here instead.
 *
 * The plate carries the state and the lettering does not: open is the surface
 * going accent, exactly as it is on STEMS. A word that changed colour as well
 * would be saying the same thing twice and reading differently from its
 * neighbours. */
/* THE PLATE FOLLOWS THE THEME AND THE LETTERING HAS TO BE PUT BACK.
 *
 * Everything filled below is read out of mod_ui() on the spot, so it is right whatever
 * the theme is. A juce::Label's ink is not: it is STORED on the component when the
 * button is built and painted from there for ever after. Built under one theme and
 * looked at under another, the word kept the first theme's colour -- measured, X-PAD
 * still wearing SANDSTONE's near-black navy on ORIGINAL's dark plate, invisible, with
 * the four deck buttons beside it white and its own plate correctly dark. The stems
 * button never showed it because its own refresh already puts its ink back. */
static void xpad_ink_sync(const struct theme_ui *ui)
{
    static unsigned seen;
    unsigned gen = mod_ui_gen();

    if (gen == seen)
        return;
    seen = gen;
    if (xpad_g_btn)
        juce_comp_colour(xpad_g_btn, LBL_COL_TEXT, ui->text_deck);
    if (xpad_g_strip)
        juce_comp_colour(xpad_g_strip, LBL_COL_TEXT, ui->text);
}

/* The loud half of a refusal. Derived from the budget rather than stored, so the paint
 * cannot be looking at a phase the tick has already moved past. */
static int xpad_refuse_hot(void)
{
    return xpad_g_refuse && !((xpad_g_refuse / MOD_BLINK_PERIOD) & 1);
}

/* One tick of it. Repainted only when the half turns over: the button is not otherwise
 * invalidated while the panel is shut, and an every-tick repaint is a shimmer rather
 * than a blink. */
static void xpad_refuse_step(void)
{
    if (!xpad_g_refuse)
        return;
    if (--xpad_g_refuse % MOD_BLINK_PERIOD == 0)
        juce_comp_repaint(xpad_g_btn);
}

static void xpad_btn_paint(void *self, void *g)
{
    int32_t b[4];

    if (juce_comp_bounds((uintptr_t)self, b) == 0) {
        const struct theme_ui *ui = mod_ui();

        uint32_t lift = xpad_g_btn_held ? MOD_CHECKER_HOT_Q8 : 0;

        xpad_ink_sync(ui);
        mod_checker_plate(g, 0, 0, b[2], b[3],
                          xpad_refuse_hot() ? ui->refuse
                          : xpad_g_open     ? ui->accent
                                            : ui->surface, lift);
        /* The bar lifts with the plate -- one colour, so directly rather than through
         * the surface call, exactly as the stems button does it. */
        mod_btn_bar(g, 0, 0, b[2], b[3],
                    mod_colour_lift(xpad_g_open ? ui->bar_on : ui->bar, lift));
    }
    /* Bracketed: Label draws its lettering from a colour we set on it, already resolved
     * through the theme, so the generic pass must not transform it again. */
    mod_draw_enter();
    ((void (*)(void *, void *))LABEL_FN_PAINT)(self, g);
    mod_draw_leave();
}

void xpad_sync(void)
{
    static int last = -1;
    int state = (xpad_g_on ? 1 : 0) | (xpad_g_open ? 2 : 0);

    if (state == last) return;
    last = state;
    /* THE GATE GOING OFF CLOSES THE PANEL, and closes it properly: hiding the
     * strip alone would leave the waveform compacted around nothing and the band
     * held by a client that is no longer on screen. */
    if (!xpad_g_on && xpad_g_open) {
        xpad_g_open = 0;
        xpad_stop();
        kit_band_give(&k_band_xpad);
    }
    juce_comp_set_visible(xpad_g_btn, xpad_g_on);
    juce_comp_set_visible(xpad_g_strip, xpad_g_on && xpad_g_open);
    juce_comp_repaint(xpad_g_btn);
    kit_band_slots_changed();
}

void xpad_toggle(void)
{
    if (!xpad_g_open) {
        /* NOTHING TO OPEN INTO. A sampler with no samples is eight dead pads and a
         * strip that plays nothing, so the press is ANSWERED rather than obeyed: the
         * plate flashes and the panel stays shut. The finger gets the first flash in
         * the frame it pressed, because mousedown repaints after this returns.
         *
         * Only OPENING is gated. A panel already up must always close, whatever became
         * of the stick while it was open -- trapping the DJ under a strip they can see
         * is worse than any warning.
         *
         * This is also what makes the pad claim below honest: with the open gated on
         * having banks, an open panel always has at least one live pad. */
        if (xpad_bank_count() == 0) {
            xpad_g_refuse = MOD_BLINK_TICKS;
            MDBG("xpad: no loops in %s -> refusing to open, blinking\n", XP_LOOP_DIR);
            return;
        }
        /* THE BAND FIRST, AND ONLY OPEN IF IT COMES. The app refuses the mode
         * outright with no track loaded, and a strip drawn into a rect it never
         * laid out lands across the middle of the screen. */
        if (kit_band_take(&k_band_xpad) < 0) {
            MDBG("xpad: no band to open into (no track loaded?)\n");
            return;
        }
        xpad_g_open = 1;
    } else {
        xpad_g_open = 0;
        xpad_stop();
        kit_band_give(&k_band_xpad);
    }
    xpad_sync();
    MDBG("xpad: panel %s (%d bank%s)\n", xpad_g_open ? "open" : "closed",
         xpad_bank_count(), xpad_bank_count() == 1 ? "" : "s");
}

/* A FINGER ON IT LIFTS THE PLATE, which every other button in this band does and this
 * one did not: measured against the deck's held button, ours stayed at its resting
 * colour while BEAT LOOP went #323232 -> #616161. The toggle happens on the way DOWN, so
 * the lift rides on top of whichever plate that left -- the press reads as a press
 * whether the panel just opened or just closed. MOD_CHECKER_HOT_Q8 is the deck's own
 * fraction, measured off a stock button held down. */
static void xpad_btn_mousedown(void *self, void *event)
{
    (void)event;
    if ((uintptr_t)self != xpad_g_btn) return;
    xpad_g_btn_held = 1;
    xpad_toggle();
    /* THE PRESS IS THE BUTTON'S OWN STATE, so it paints whether or not the
     * toggle took. A press the band refuses moves no panel state, so the
     * repaint xpad_sync does when that state moves would never come. */
    juce_comp_repaint(xpad_g_btn);
}

static void xpad_btn_mouseup(void *self, void *event)
{
    (void)event;
    if ((uintptr_t)self != xpad_g_btn) return;
    xpad_g_btn_held = 0;
    juce_comp_repaint(xpad_g_btn);
}

/* ---- build --------------------------------------------------------------- */

static void xpad_build(uintptr_t anchor)
{
    const int32_t *panel;
    int y, h;

    if (xpad_g_built || xpad_g_building || !xpad_g_api_ok) return;
    if (kit_band_attach(anchor) != 0) return;
    if (!xpad_g_btn_vptr) {
        static const struct juce_vt_override ov[] = {
            { JUCE_VT_MOUSEDOWN, (void *)xpad_btn_mousedown, 0 },
            { JUCE_VT_MOUSEUP,   (void *)xpad_btn_mouseup,   0 },
            { JUCE_VT_PAINT,     (void *)xpad_btn_paint,     0 },
        };

        xpad_g_btn_vptr = juce_label_vt_clone(xpad_g_btn_vt, ov,
                                              (int)(sizeof(ov) / sizeof(ov[0])));
        if (!xpad_g_btn_vptr) return;
    }
    xpad_g_building = 1;
    panel = kit_band_rect();
    /* The same three pixels the stem row takes: the band above the panel rect is
     * not empty -- the loop indicator draws in it -- so this is what was measured
     * to be free rather than the ten PANEL_GAP would suggest. */
    y = panel[1] - 3;
    h = panel[3] + 3;

    if (!xpad_g_btn) {
        xpad_g_btn = juce_label(kit_band_bar(), "X-PAD", XP_FONT_BTN, 0x00000000u,
                                mod_ui()->text_deck, xpad_g_btn_vptr,
                                kit_band_slot_x(&k_band_xpad), KIT_BAND_SLOT_Y,
                                KIT_BAND_SLOT_W, KIT_BAND_SLOT_H);
        /* Label ends its paint with a one-pixel drawRect and that is its whole
         * outline. The stock buttons have none, so neither does this. */
        juce_comp_colour(xpad_g_btn, LBL_COL_OUTLINE, 0x00000000u);
        kit_band_own(xpad_g_btn);
    }
    if (!xpad_g_strip) {
        xpad_g_strip = juce_label(kit_band_view(), "", XP_FONT_FLAG, 0x00000000u,
                                  mod_ui()->text, 0, panel[0], y, panel[2], h);
        kit_band_own(xpad_g_strip);
        if (xpad_g_strip) {
            xpad_g_pad  = xpad_build_pad(xpad_g_strip, XP_PAD_X, 0, XP_PAD_W, h);
            xpad_g_pane = xpad_build_pane(xpad_g_strip, XP_PANE_X, 0,
                                          panel[2] - XP_PANE_X, h);
        }
    }
    xpad_g_building = 0;

    xpad_sync();
    if (!xpad_g_btn || !xpad_g_strip || !xpad_g_pad || !xpad_g_pane) {
        MDBG("xpad: build incomplete (btn=%#lx strip=%#lx pad=%#lx pane=%#lx)\n",
             (unsigned long)xpad_g_btn, (unsigned long)xpad_g_strip,
             (unsigned long)xpad_g_pad, (unsigned long)xpad_g_pane);
        return;
    }
    juce_comp_set_visible(xpad_g_strip, 0);
    xpad_g_built = 1;
    MDBG("xpad: built -- strip {%d,%d,%d,%d}, unity y=%d travel %d\n",
         panel[0], y, panel[2], h, xpad_unity_y(h), xpad_travel(h));
}

/* ---- the clocks ---------------------------------------------------------- */

/* The title bar draws whenever the loaded track or its labels change, which is
 * what makes paint the trigger: it needs no user action and the parent chain is
 * already wired by the time anything is drawn. Chained -- the stem row hooks the
 * same slot. */
static void xpad_anchor_paint(void *self, void *g)
{
    if (xpad_g_orig_anchor_paint)
        ((void (*)(void *, void *))xpad_g_orig_anchor_paint)(self, g);
    xpad_build((uintptr_t)self);
}

void xpad_repaint(void)
{
    __atomic_store_n(&xpad_g_dirty, 1, __ATOMIC_RELEASE);
}

static void xpad_tick(void *self)
{
    if (xpad_g_orig_tick)
        ((void (*)(void *))xpad_g_orig_tick)(self);
    xpad_g_ticks++;
    if (!xpad_g_built) return;
    /* Ahead of the open gate, because a refusal is by definition a panel that did not
     * open -- behind it, the flash would never be drawn. */
    xpad_refuse_step();
    if (!xpad_g_open) return;

    /* HOLD GOING OFF DROPS WHAT IT WAS HOLDING. Reconciled here rather than in
     * the MEMORY handler because this thread owns xpad_g_touch and that one runs
     * on the deck's task thread -- and because it catches every route that puts
     * HOLD down, not just the button.
     *
     * The values have to GO, not merely stop acting: left in place, pressing
     * MEMORY again with no finger on the strip would bring the old brick back to
     * life, and HOLD means "keep the gesture I am making", not "restore the last
     * one I made". */
    if (!xpad_g_hold && !xpad_g_touch.held &&
        xpad_g_touch.div != XP_DIV_NONE) {
        xpad_g_touch.div     = XP_DIV_NONE;
        xpad_g_touch.semis   = 0.0f;
        xpad_g_touch.snapped = 0;
        juce_comp_repaint(xpad_g_pad);
        juce_comp_repaint(xpad_g_pane);
    }
    /* The active brick's name blinks, and nothing else in the app invalidates
     * this strip while a finger rests on it. */
    if (xpad_gesture_live())
        juce_comp_repaint(xpad_g_pad);
    /* The readout, when one of the three borrowed controls moved it. Taken here
     * rather than repainted there: those run on the deck's task thread. */
    if (__atomic_exchange_n(&xpad_g_dirty, 0, __ATOMIC_ACQUIRE)) {
        juce_comp_repaint(xpad_g_pane);
        /* The pad as well: MEMORY releasing the latch changes what the STRIP
         * shows, not just the readout. */
        juce_comp_repaint(xpad_g_pad);
    }

    /* What the mix did with the last second, printed from here because the mix
     * itself may not log. Silent when nothing sounded. */
    if ((xpad_g_ticks % XPAD_STAT_TICKS) == 0) {
        struct xpad_mix_stat st;

        xpad_mix_stat(&st);
        if (st.blocks || st.fired || xpad_seq_count())
            MDBG("xpad: mix %u blocks, deck fired %u, mix fired %u, peak %.3f,"
                 " vol %d%%, bar %d, clock %s beat %.3f span %.5f\n",
                 st.blocks, st.fired, st.fires, (double)st.peak, xpad_g_vol,
                 xpad_seq_count(),
                 st.clock == 2 ? "grid" : st.clock == 1 ? "tempo" : "NONE",
                 st.beat, st.span);
    }
}

/* ---- install ------------------------------------------------------------- */

static void xpad_gate_changed(void)
{
    /* Nothing to tear down here: sync owns the close, and the tick calls it. */
    MDBG("xpad: ENABLE X-PAD %s\n", xpad_g_on ? "on" : "off");
    xpad_sync();
}

static const struct kit_row k_rows[] = {
    KIT_ROW_BOOL("ENABLE X-PAD", &xpad_g_on,
                 .idx = KIT_IDX_XPAD, .changed = xpad_gate_changed),
};

static int xpad_ui_install(void)
{
    xpad_g_api_ok = FN_ADD_VISIBLE && FN_SET_BOUNDS && FN_LABEL_CTOR &&
                    FN_LABEL_SETFONT && FN_LABEL_JUSTIFY &&
                    MOD_FN_GFX_SETCOLOUR && MOD_FN_GFX_FILLRECT &&
                    MOD_FN_GFX_SETFONT && MOD_FN_GFX_DRAWTEXT;
    if (!xpad_g_api_ok) {
        MDBG("xpad: juce primitives did not resolve -> feature off\n");
        return -1;
    }
    if (mod_patch_vslot("xpadAnchor", EP122_TOUCHARIA, JUCE_VT_PAINT,
                        (void *)xpad_anchor_paint, &xpad_g_orig_anchor_paint) != 0) {
        MDBG("xpad: no anchor -> feature off\n");
        xpad_g_api_ok = 0;
        return -1;
    }
    if (mod_patch_vslot("xpadTick", EP122_DISPLAY_REFRESH, 0x10,
                        (void *)xpad_tick, &xpad_g_orig_tick) != 0)
        MDBG("xpad: no display tick -> the lit brick will not blink\n");
    kit_menu_add(k_rows, (int)(sizeof(k_rows) / sizeof(k_rows[0])));
    return 0;
}

KIT_MOD(k_mod_xpad_ui,
        .name = "xpad_ui", .prio = 35, .install = xpad_ui_install,
        .what = "X-PAD SAMPLER quick-menu button + touch strip");
