// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/wedge.c - the three level controls, drawn and dragged
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* ================================================================== */
/* The wedge                                                          */
/* ================================================================== */

/* One Component per stem, painted by us: a right triangle, thin at the left and full
 * height at the right, whose left part is filled to the level.
 *
 * juce::Slider is gone, and with it everything that existed only to make a Slider look
 * like something it is not -- the LookAndFeel getSliderThumbRadius override and its
 * answer-wide-then-narrow trick, the Listener, the two Labels standing in for a rail
 * juce insists on drawing 6px thick, and the Pimpl round trip that read the value back
 * out of a juce::Value. A shape juce cannot draw was always going to end in our own
 * paint; taking it means the value is just an int we own.
 *
 * The wedge is a Label with a second cloned vtable -- paint AND the three mouse slots --
 * so the Label ctor still does the allocation and the Component base for us and its own
 * paint is simply never reached. */
uintptr_t stems_g_wedge[N_STEMS];
static int32_t   g_wedge_w[N_STEMS];    /* cached: the drag maps x onto this */
int       stems_g_level[N_STEMS] = { 100, 100, 100 };   /* percent, 0..STEM_LEVEL_MAX */
/* MUTE, latched per stem, worked by the caption above the wedge. Kept apart from the
 * level rather than folded into it: a mute has to be releasable back to whatever the
 * fader was on, which a level of zero cannot remember. */
int       stems_g_mute[N_STEMS];

/* The audio side's half of the mute. Defined here beside the UI's own so the two
 * are read together; see stem.h for which thread owns which. */
int       g_stem_mute_want[N_STEMS];
int       g_stem_mute_live[N_STEMS];
unsigned  g_stem_mute_commits;
float     g_stem_gain_unmuted[N_STEMS] = { 1.0f, 1.0f, 1.0f };

static uintptr_t g_wedge_vt[2 + VT_CLONE_SLOTS];
static uintptr_t g_wedge_vptr;
/* ---- one gesture, one control ------------------------------------------------------
 *
 * Whoever takes the gesture at mouseDown owns EVERY control in the row until the finger
 * lifts -- not just the other wedges.
 *
 * This is a palm rule, not a tidiness rule. A wedge jumps to wherever it is touched, so a
 * hand sweeping one fader to maximum crosses its neighbours near their left edge and
 * slams them to MINIMUM on the way past; going the other way it hands them maximum. Same
 * sweep across a caption engages that stem's mute, and across BYPASS it bypasses the
 * lot -- mid-set, with no way to know which of the four just happened.
 *
 * So the grab is global to the row. A control that did not start the gesture does nothing
 * at all, including hover: a highlight on something that cannot act is still a message
 * that it might. */
uintptr_t stems_g_grab;        /* who owns the gesture right now */
uintptr_t stems_g_grab_last;   /* who owned it most recently */
static unsigned  g_grab_freed;  /* the display tick it was let go on */
unsigned  stems_g_ticks;       /* display ticks; exists only for the cooldown */

/* A grab does not end the instant a mouseUp arrives -- it goes cold for a moment first.
 *
 * The plain version of this did not work, and the reason is the whole point: dragging
 * across a boundary does not produce a drag, it produces an UP on the control being left
 * and a DOWN on the one being entered, microseconds apart. A latch cleared on mouseUp is
 * therefore already open by the time the neighbour asks, which is exactly the fault this
 * was meant to fix -- three faders catching on one sweep with a latch supposedly in
 * place.
 *
 * There is no event that distinguishes a finger leaving the panel from a finger crossing
 * into the next control, so the difference has to be time. A crossing is instantaneous; a
 * deliberate move to another fader is not. Four ticks is about ninety milliseconds --
 * orders of magnitude longer than a crossing, and short enough that a DJ re-gripping a
 * different fader will never notice it. Re-taking the SAME control is never delayed. */
#define GRAB_COOLDOWN 4

static int stems_wedge_index(uintptr_t comp);

int stems_grab_take(uintptr_t self)
{
    if (stems_g_grab)
        return stems_g_grab == self;
    /* The cooldown is a FADER rule. Only a wedge acts on the press itself -- it jumps to
     * wherever it was touched -- so only a wedge can be wrecked by a crossing. A button
     * that is merely entered does nothing until it is released, and holding one cold for
     * ninety milliseconds would make the row feel like it was dropping presses. */
    if (stems_wedge_index(self) >= 0 &&
        stems_g_grab_last && stems_g_grab_last != self &&
        (unsigned)(stems_g_ticks - g_grab_freed) < GRAB_COOLDOWN)
        return 0;
    stems_g_grab = self;
    stems_g_grab_last = self;
    return 1;
}

void stems_grab_release(void)
{
    if (!stems_g_grab) return;
    g_grab_freed = stems_g_ticks;
    stems_g_grab = 0;
}

void stems_progress_poll(void);
int  stems_available(void);


/* Invalidate a whole component. The rect is passed by POINTER -- see FN_COMP_REPAINT. */
void stems_repaint(uintptr_t comp)
{
    int32_t r[4];

    if (!comp || stems_bounds(comp, r) != 0) return;
    r[0] = 0;
    r[1] = 0;
    ((void (*)(void *, const int32_t *))FN_COMP_REPAINT)((void *)comp, r);
}
/* How hard a component is lit while the gesture is live on it, as a Q8 for the draw
 * kit. Zero is "at rest", and BOTH buttons plus the bar take it from here, so nothing
 * can end up lit in one place and flat in another.
 *
 * What it lifts is the colour the button is about to WEAR, not the one it is leaving --
 * and that is the behaviour, so it is worth saying why it comes out that way rather than
 * by luck. mouseDown moves the state before it returns, and juce coalesces the
 * invalidation into a paint that runs afterwards, so the plate colour has already
 * settled on the DESTINATION by the time it is read. Pressing an unlit button therefore
 * flashes a lightened accent and pressing a lit one flashes a lightened grey: activating
 * and disabling do not look alike, because the feedback previews where the press goes.
 *
 * Note it returns the LIFT and not a colour. The surface has two halves and they have to
 * be lifted independently -- see mod_checker_lift -- so handing out a pre-lightened
 * colour is exactly the mistake that call exists to prevent. */
uint32_t stems_touch_lift(uintptr_t comp)
{
    return comp && stems_g_grab == comp ? MOD_CHECKER_HOT_Q8 : 0;
}

/* Set it. Repaints on a real change only -- this is called from the display tick's warn
 * blink, where the common case is that nothing moved. */
/* State -> role. The one place the button's three appearances are named, so a theme
 * change is picked up by the next repaint and nothing has to be invalidated for it. */
uint32_t stems_btn_surface(void)
{
    const struct theme_ui *ui = mod_ui();

    return stems_g_btn_state == BTN_ON     ? ui->accent
         : stems_g_btn_state == BTN_REFUSE ? ui->refuse
                                     : ui->surface;
}

void stems_btn_state(enum btn_state st)
{
    if (stems_g_btn_state == st) return;
    stems_g_btn_state = st;
    stems_repaint(stems_g_btn_stems);
}


/* How many steps fit, and where step k sits. One place for the arithmetic, so the paint
 * and the hit test cannot disagree about which step a finger is on. */
static int wedge_steps(int w)
{
    int n = (w + WEDGE_BAR_GAP) / (WEDGE_BAR_W + WEDGE_BAR_GAP);

    return n < 2 ? 2 : n;
}

/* Which stem a component is, or -1. Three entries, so a scan beats a back-pointer. */
static int stems_wedge_index(uintptr_t comp)
{
    int i;

    for (i = 0; i < N_STEMS; i++)
        if (comp && stems_g_wedge[i] == comp) return i;
    return -1;
}

/* The level, published to the audio thread and put on screen.
 *
 * The gain array is the audio side's ENTIRE view of the row, so it is written here
 * and nowhere else -- one place that cannot drift from what is drawn. */
void stems_publish_gain(int i)
{
    float open_gain = (float)stems_g_level[i] / (float)STEM_LEVEL_MAX;

    if (i < 0 || i >= N_STEMS) return;
    /* The fader's own gain, kept where the audio thread can restore it without
     * reading the row's UI state. */
    __atomic_store(&g_stem_gain_unmuted[i], &open_gain, __ATOMIC_RELAXED);
    __atomic_store_n(&g_stem_mute_want[i], stems_g_mute[i], __ATOMIC_RELAXED);

    /* The LEVEL is immediate and only the MUTE waits: a fader is a continuous
     * control and snapping it to a beat would make it feel broken. So the gain
     * published here follows whatever the mute is CURRENTLY applying, and the
     * commit below moves it when the boundary comes. */
    stem_gain_set(i, __atomic_load_n(&g_stem_mute_live[i], __ATOMIC_RELAXED)
                     ? 0.0f : open_gain);
}

void stems_level_set(int i, int pct)
{
    if (i < 0 || i >= N_STEMS) return;
    if (pct < 0) pct = 0;
    else if (pct > STEM_LEVEL_MAX) pct = STEM_LEVEL_MAX;
    if (pct == stems_g_level[i]) return;
    stems_g_level[i] = pct;
    stems_publish_gain(i);
    stems_repaint(stems_g_wedge[i]);
}

/* Is there a stem set resident RIGHT NOW?
 *
 * The row can be open with no stems behind it, and more easily than expected: a probe
 * that fails, a server that never answers, a track whose job never started. The progress
 * bar covers the case where a job is running and says so; this covers the case where
 * nothing is running at all, which is the one that leaves live-looking faders over a mix
 * they are not in. */
int stems_ready(void)
{
    return __atomic_load_n(&g_stem_ready, __ATOMIC_ACQUIRE) != 0;
}

/* Is this stem out of the mix, for any reason? The wedge greys and goes inert on all of
 * them, because a control that looks live and moves a value nothing reads is a lie
 * however it got that way. */
static int stems_stem_off(int i)
{
    return !stems_ready() || stems_g_bypass_on || (i >= 0 && i < N_STEMS && stems_g_mute[i]);
}

/* Paint one wedge.
 *
 * The triangle is drawn as horizontal scanlines rather than as a juce::Path: two calls
 * pinned instead of a Path ctor, lineTo, closeSubPath and a dtor, and at one row per
 * pixel the hypotenuse is the same staircase the rasteriser would produce anyway. The
 * cost is two setColour calls and 2*h fillRects -- on a 54px wedge, about a hundred
 * spans, on a component that repaints only when something moved.
 *
 * Row y (0 at the top) spans x from `xs` to the right edge, where xs shrinks to 0 at the
 * bottom row. The level fills from the LEFT, so a quiet stem is a small triangle and a
 * stem at unity is the whole shape -- the area IS the value, which is the reason for the
 * shape over a bar. */
static void stems_wedge_paint(void *self, void *g)
{
    int32_t b[4];
    int i, k, n, w, h, step, base, x0, lit;

    i = stems_wedge_index((uintptr_t)self);
    if (i < 0 || stems_bounds((uintptr_t)self, b) != 0) return;
    w = b[2];
    h = b[3];
    if (w <= 0 || h <= WEDGE_MIN_H) return;

    n = wedge_steps(w);
    /* The rise per bar is a WHOLE number of pixels, and the shortest bar is whatever
     * that leaves over. Deriving it in this order is the whole point.
     *
     * Fixing the shortest bar instead and dividing the rest made the rise 48/42 of a
     * pixel per bar, and integer truncation spends that as six +1s and a +2 -- a hitch
     * every seventh bar, regular enough to be the first thing the eye finds. Pixels are
     * whole, so SOMETHING has to absorb the remainder; better the one bar at the end
     * than a beat running the length of the control. */
    step = (h - WEDGE_MIN_H) / (n - 1);
    if (step < 1) step = 1;
    base = h - step * (n - 1);
    if (base < 1) base = 1;
    /* Rounded, not truncated: at 50% of 43 steps the boundary lands mid-step, and
     * truncating there makes the control feel like it lags the finger by one. */
    lit = (stems_g_level[i] * n + STEM_LEVEL_MAX / 2) / STEM_LEVEL_MAX;
    /* Centred, so the leftover from the integer division is split between the two ends
     * rather than left as a gap on the right. */
    x0 = (w - (n * WEDGE_BAR_W + (n - 1) * WEDGE_BAR_GAP)) / 2;

    /* Resolved once, not per bar: forty-odd bars asking three times each would be a
     * hundred-odd role lookups for an answer that cannot change inside one paint. */
    {
    const struct theme_ui *ui = mod_ui();
    int off = stems_stem_off(i);

    for (k = 0; k < n; k++) {
        int bh = base + step * k;
        int on = k < lit;
        /* Both ENDS are always marked, whatever the count works out to. The modulo
         * alone gets the first bar for free and the last one only by luck -- at 42 bars
         * it lands on 41 and misses, which is exactly what happened when the row was
         * inset from the screen edge. Minimum and maximum are the two positions worth
         * being able to find without looking. */
        int mark = (k % WEDGE_TICK) == 0 || k == n - 1;
        uint32_t col = on ? (off ? ui->text_off : ui->stem[i]) : ui->surface;

        /* A mark is brighter AND a pixel wider. Only the width it is drawn at changes;
         * the slot it is drawn in does not, so the pitch survives. */
        if (mark)
            col = on ? stems_lighter(col) : ui->tick;

        /* One setColour per bar rather than two passes over the row: at forty-odd bars
         * the call count is the same either way, and this keeps the geometry in one
         * loop where the colours cannot drift apart from it. */
        mod_gfx_colour(g, col);
        mod_gfx_fill(g, x0 + k * (WEDGE_BAR_W + WEDGE_BAR_GAP), h - bh,
                       mark ? WEDGE_BAR_W : WEDGE_BAR_THIN, bh);
    }
    }
}

/* x within the component -> level. juce::MouseEvent begins with Point<float> position,
 * relative to the component that is handling the event.
 *
 * No plausibility check on the movement, deliberately. The panel reports ONE averaged
 * point for two contacts, so a second finger anywhere teleports the value -- but that is
 * the panel, not us, and it is visible the instant it happens. Filtering it would mean a
 * heuristic that can also reject a genuinely fast sweep, which is the worse failure of
 * the two. */
static void stems_wedge_track(void *self, void *event)
{
    int i = stems_wedge_index((uintptr_t)self);
    float pos[2];

    if (i < 0 || !event || g_wedge_w[i] <= 0) return;
    memcpy(pos, event, sizeof(pos));
    stems_level_set(i, (int)((pos[0] * STEM_LEVEL_MAX) / (float)g_wedge_w[i] + 0.5f));
}

static void stems_wedge_down(void *self, void *event)
{
    if (stems_stem_off(stems_wedge_index((uintptr_t)self)))
        return;                         /* out of circuit: inert, not merely greyed */
    if (!stems_grab_take((uintptr_t)self)) return;
    stems_wedge_track(self, event);
}

static void stems_wedge_drag(void *self, void *event)
{
    if (stems_g_grab != (uintptr_t)self ||
        stems_stem_off(stems_wedge_index((uintptr_t)self))) return;
    stems_wedge_track(self, event);
}

static void stems_wedge_up(void *self, void *event)
{
    int i = stems_wedge_index((uintptr_t)self);

    (void)event;
    if (stems_g_grab == (uintptr_t)self && i >= 0)
        MDBG("stems: %s = %d%%\n", k_stem_name[i], stems_g_level[i]);
    /* Released on ANY up, not only the owner's. A grab that outlives its gesture would
     * leave the whole row dead to the touch, which is far worse than the stray press it
     * exists to stop. */
    stems_grab_release();
}

/* Same clone-and-override as the button vtable, one slot further: paint is ours too, so
 * the post-condition is checked against Label's own paint before it is replaced. */
static int stems_wedge_vt_ready(void)
{
    if (g_wedge_vptr) return 1;
    if (mod_safe_read(LABEL_VTABLE - 2 * sizeof(uintptr_t),
                      g_wedge_vt, sizeof(g_wedge_vt)) != 0) return 0;
    if (g_wedge_vt[2 + VT_SLOT_PAINT / sizeof(uintptr_t)] != LABEL_FN_PAINT) {
        MERR("stems: label vtable paint slot holds %#lx, expected %#lx -> no wedges\n",
             (unsigned long)g_wedge_vt[2 + VT_SLOT_PAINT / sizeof(uintptr_t)],
             (unsigned long)LABEL_FN_PAINT);
        return 0;
    }
    g_wedge_vt[2 + VT_SLOT_PAINT     / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_paint;
    g_wedge_vt[2 + VT_SLOT_MOUSEDOWN / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_down;
    g_wedge_vt[2 + VT_SLOT_MOUSEDRAG / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_drag;
    g_wedge_vt[2 + VT_SLOT_MOUSEUP   / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_up;
    g_wedge_vptr = (uintptr_t)&g_wedge_vt[2];
    MDBG("stems: cloned juce::Label vtable for wedges -> %#lx\n",
         (unsigned long)g_wedge_vptr);
    return 1;
}

/* BYPASS, as a drawn icon: the three stems, struck out when the bypass is engaged.
 *
 * Three glyphs came before this one and each described the wrong thing. Three equal bars
 * is a hamburger menu. A revert arrow says "undo what I did", and this undoes nothing --
 * the levels are kept and come back the moment it is released. A power mark says a state
 * but not WHOSE: it could be switching off the panel, the deck, anything.
 *
 * What the control does is take these three parts out of circuit, so the icon is those
 * three parts. It borrows the wedges' own colours, which is the one thing on screen that
 * already means "stems" -- and when the bypass is on they go dark and a bar strikes
 * through them. Off is then a picture rather than a word.
 *
 * All rectangles. The strike is horizontal rather than the usual diagonal for the reason
 * the wedge became bars: a diagonal on this screen has to be antialiased by hand. */
static uintptr_t g_icon_vt[2 + VT_CLONE_SLOTS];
uintptr_t stems_g_icon_vptr;

#define ICON_BAR_W     8
#define ICON_BAR_GAP   5
#define ICON_BAR_H     20
#define ICON_BASE_H    3     /* the line the three stand on */
#define ICON_BASE_GAP  3     /* clearance between the bars and that line */
#define ICON_BASE_OVER 4     /* how far it runs past them at each end */

static void stems_bypass_paint(void *self, void *g)
{
    int32_t b[4];
    int w, h, i, span, x0, y0, by;

    if (stems_bounds((uintptr_t)self, b) != 0) return;
    w = b[2];
    h = b[3];
    if (w <= 0 || h <= 0) return;

    /* The plate stays grey in both states. Lighting the whole button meant the icon had
     * to be read against two different backgrounds, and the amber then said "engaged"
     * twice -- once in the fill and once in the glyph. One place says it now. */
    mod_checker_lift(g, 0, 0, w, h, stems_bypass_colour(),
                     stems_touch_lift((uintptr_t)self));
    mod_gfx_colour(g, mod_ui()->edge);
    mod_gfx_fill(g, 0, 0, w, 1);
    mod_gfx_fill(g, 0, h - 1, w, 1);
    mod_gfx_fill(g, 0, 0, 1, h);
    mod_gfx_fill(g, w - 1, 0, 1, h);

    span = N_STEMS * ICON_BAR_W + (N_STEMS - 1) * ICON_BAR_GAP;
    if (span + 2 * ICON_BASE_OVER > w) return;
    x0 = (w - span) / 2;
    y0 = (h - (ICON_BAR_H + ICON_BASE_GAP + ICON_BASE_H)) / 2;
    by = y0 + ICON_BAR_H + ICON_BASE_GAP;

    for (i = 0; i < N_STEMS; i++) {
        mod_gfx_colour(g, !stems_ready() ? mod_ui()->dead
                          : stems_g_bypass_on ? mod_ui()->icon_disabled : mod_ui()->stem[i]);
        mod_gfx_fill(g, x0 + i * (ICON_BAR_W + ICON_BAR_GAP), y0,
                       ICON_BAR_W, ICON_BAR_H);
    }
    /* Always drawn, never a strike. A line that appears and disappears is a second thing
     * to notice; a line that is always there and changes colour is one. Under the bars
     * rather than through them, so it reads as what they stand on -- the signal the three
     * of them are riding, and the thing the bypass takes over. */
    mod_gfx_colour(g, !stems_ready() ? mod_ui()->dead
                          : stems_g_bypass_on ? mod_ui()->bypass : mod_ui()->text_off);
    mod_gfx_fill(g, x0 - ICON_BASE_OVER, by, span + 2 * ICON_BASE_OVER, ICON_BASE_H);
}

/* The button clone with paint taken as well, so the press behaviour is shared with
 * STEMS and only the drawing differs. */
int stems_icon_vt_ready(void)
{
    if (stems_g_icon_vptr) return 1;
    if (!stems_label_vt_ready()) return 0;
    memcpy(g_icon_vt, stems_g_label_vt, sizeof(g_icon_vt));
    g_icon_vt[2 + VT_SLOT_PAINT / sizeof(uintptr_t)] = (uintptr_t)stems_bypass_paint;
    stems_g_icon_vptr = (uintptr_t)&g_icon_vt[2];
    return 1;
}

uintptr_t stems_wedge(uintptr_t parent, int x, int y, int w, int h, int stem)
{
    uintptr_t p;

    if (!stems_wedge_vt_ready()) return 0;
    /* Transparent, no text, no outline: every pixel it shows comes from our paint. */
    p = stems_label(parent, "", FONT_CAPTION, 0x00000000u, mod_ui()->text, 0, x, y, w, h);
    if (!p) return 0;
    *(uintptr_t *)p = g_wedge_vptr;
    stems_g_wedge[stem] = p;
    g_wedge_w[stem] = w;
    stem_gain_set(stem, (float)stems_g_level[stem] / (float)STEM_LEVEL_MAX);
    return p;
}
