// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/strip.c - the pad: six bricks, and the bend inside the one under the
 * finger.
 *
 * X picks the loop length, Y bends the pitch, and the two are read from ONE
 * touch -- so the whole design problem is that a sideways move to change loop
 * length drags the pitch with it, and a bend drags the loop length with it.
 * Two things answer that, and neither is a mode:
 *
 *   the ANGLE GATE routes every step by its own direction, continuously, so
 *   sideways travel adds nothing to the bend and vertical travel does not move
 *   the brick. See xpad.h for why the bend has to be a running total for that
 *   to work.
 *
 *   the SNAP holds pitch at exactly zero within half a semitone, so returning
 *   to unity is reliable rather than approximate. It costs no vertical range,
 *   which matters because there is very little of it -- the band the app frees
 *   is 84 pixels and the whole range fits in half of that.
 *
 * The only signal that the snap has hold of you is the cursor thickening from
 * 3px to 6px. It needs no room of its own and it is visible under a finger.
 *
 * WHY THE NAME SITS IN A NOTCH. The fill has to be the loudest mark the strip
 * can make, because it is what reads from across a booth; the name has to stay
 * legible, because it is what says which loop is armed. Drawing one over the
 * other loses whichever went down first. So the fill is cut around the
 * lettering -- 66px out of a 153px brick -- leaving 43px of unbroken fill down
 * each gutter at any value, which is enough for the shape to read on its own.
 */
#include "xpad/xpad.h"

/* ---- geometry ------------------------------------------------------------
 *
 * Derived from the pad's own height, never written down: the band is whatever
 * the app frees, and the design's numbers were drawn against a taller strip.
 * Unity is centred and the travel is symmetric, because the axis is linear in
 * semitones and the two ends of the range are the same interval. */
int xpad_unity_y(int h) { return (h - XP_CURSOR_H) / 2; }
int xpad_travel(int h)  { return xpad_unity_y(h) - XP_EDGE_LIT; }

/* Brick edges, computed rather than tabulated so the six always tile the pad
 * exactly -- 916 does not divide by six, and a rounded constant would leave a
 * seam. */
static int brick_x0(int i) { return i * XP_PAD_W / XP_BRICKS; }
static int brick_x1(int i) { return (i + 1) * XP_PAD_W / XP_BRICKS; }

static int brick_at(int x)
{
    int i;

    if (x < 0 || x >= XP_PAD_W) return XP_DIV_NONE;
    for (i = 0; i < XP_BRICKS; i++)
        if (x < brick_x1(i)) return i;
    return XP_BRICKS - 1;
}

int xpad_pitch_pct(float semis)
{
    float p = semis * (float)XP_PCT_FULL / (float)XP_SEMITONES;

    /* Rounded to the nearest. The audio side takes the float and never this. */
    return (int)(p + (p < 0 ? -0.5f : 0.5f));
}

/* ---- paint --------------------------------------------------------------- */

/* The fill in its two shapes, both clipped to a y range so the caller can hand
 * them the three bands of the brick without testing anything itself. Clear of
 * the name, the fill spans the whole brick; level with it, only the two gutters
 * either side -- 43px of solid colour each at any value, which is enough for the
 * shape to read without the digits.
 *
 * BOTH INSET BY THE LIT OUTLINE. The active brick wears a heavier edge and the
 * fill belongs inside it: spanning x0..x0+w painted straight over that edge, so
 * a bend large enough to reach past the lettering appeared to burst three pixels
 * out of the brick's own frame on each side. */
static void xpad_fill_wide(void *g, int x0, int w, int y0, int y1)
{
    if (y1 > y0)
        mod_gfx_fill(g, x0 + XP_EDGE_LIT, y0, w - 2 * XP_EDGE_LIT, y1 - y0);
}

static void xpad_fill_gutters(void *g, int x0, int w, int nx, int y0, int y1)
{
    if (y1 <= y0) return;
    mod_gfx_fill(g, x0 + XP_EDGE_LIT, y0, nx - x0 - XP_EDGE_LIT, y1 - y0);
    mod_gfx_fill(g, nx + XP_NOTCH_W, y0,
                 x0 + w - XP_EDGE_LIT - (nx + XP_NOTCH_W), y1 - y0);
}

static void xpad_pad_paint(void *self, void *g)
{
    const struct theme_ui *ui = mod_ui();
    const struct xpad_touch *t = &xpad_g_touch;
    int32_t b[4];
    int h, unity, travel, i, lit_on, live;

    if (juce_comp_bounds((uintptr_t)self, b) != 0) return;
    /* WHAT IS ACTING, not what was last touched. Switching HOLD off releases the
     * latch, and the strip has to go dormant with it -- a lit brick over silence
     * is the strip lying about what the deck is doing. */
    live   = xpad_gesture_live();
    h      = b[3];
    unity  = xpad_unity_y(h);
    travel = xpad_travel(h);

    /* The pad's own outline, and the five dividers between the six bricks. */
    mod_gfx_colour(g, ui->edge);
    mod_gfx_fill(g, 0, 0, XP_PAD_W, XP_EDGE);
    mod_gfx_fill(g, 0, h - XP_EDGE, XP_PAD_W, XP_EDGE);
    mod_gfx_fill(g, 0, 0, XP_EDGE, h);
    mod_gfx_fill(g, XP_PAD_W - XP_EDGE, 0, XP_EDGE, h);
    for (i = 1; i < XP_BRICKS; i++)
        mod_gfx_fill(g, brick_x0(i) - XP_EDGE / 2, 0, XP_EDGE, h);

    /* The six names. Dimmed as a set the moment one is live, so the lit one is
     * the only thing on the strip competing for the eye. */
    for (i = 0; i < XP_BRICKS; i++) {
        int x0 = brick_x0(i), w = brick_x1(i) - x0;

        if (live && i == t->div) continue;   /* drawn below, in its notch */
        mod_gfx_text(g, xpad_div_name[i], XP_FONT_BRICK,
                     live ? ui->text_off : ui->text,
                     x0, 0, w, h, JUCE_JUSTIFY_CENTRED);
    }
    if (!live)
        return;

    {
        int x0 = brick_x0(t->div), w = brick_x1(t->div) - x0;
        int nx = x0 + (w - XP_NOTCH_W) / 2;
        int cur_h = t->snapped ? XP_CURSOR_SNAP_H : XP_CURSOR_H;
        int px = (int)(t->semis * (float)travel / (float)XP_SEMITONES + (t->semis < 0 ? -0.5f : 0.5f));

        /* The active brick wears a heavier outline, so which one is armed is
         * legible even at unity where there is no fill to see. */
        mod_gfx_colour(g, ui->text_dim);
        mod_gfx_fill(g, x0, 0, w, XP_EDGE_LIT);
        mod_gfx_fill(g, x0, h - XP_EDGE_LIT, w, XP_EDGE_LIT);
        mod_gfx_fill(g, x0, 0, XP_EDGE_LIT, h);
        mod_gfx_fill(g, x0 + w - XP_EDGE_LIT, 0, XP_EDGE_LIT, h);

        /* The bend, growing out of unity: up for positive, down for negative, so
         * magnitude and sign are one shape with no floating object to track.
         * The pad's own red (mod_ui()->xpad), not the accent: on this deck blue
         * means "on, selected", which a live value in an engaged mode is not.
         *
         * The fill STOPS at the lettering rather than being masked back over it.
         * That needs no background colour, which matters because the ground here
         * is the app's own and a theme is free to move it -- and it is the same
         * picture, since a mask would be painting the ground back anyway. */
        if (px) {
            int y0 = px > 0 ? unity - px : unity + XP_CURSOR_H;
            int y1 = px > 0 ? unity      : unity + XP_CURSOR_H - px;
            int ny0 = (h - XP_NOTCH_H) / 2, ny1 = ny0 + XP_NOTCH_H;

            mod_gfx_colour(g, ui->xpad);
            xpad_fill_wide(g, x0, w, y0, y1 < ny0 ? y1 : ny0);
            xpad_fill_gutters(g, x0, w, nx, y0 > ny0 ? y0 : ny0,
                                            y1 < ny1 ? y1 : ny1);
            xpad_fill_wide(g, x0, w, y0 > ny1 ? y0 : ny1, y1);
        }

        /* The name, in the gap the fill left for it. It blinks while the brick is
         * live: asymmetric, so the lit phase dominates and the name reads as
         * information first and an indicator second. */
        lit_on = (int)(xpad_g_ticks % XP_BLINK_PERIOD) < XP_BLINK_ON;
        mod_gfx_text(g, xpad_div_name[t->div], XP_FONT_LIT,
                     lit_on ? ui->xpad : mod_colour_scale(ui->xpad, XP_BLINK_DIM_Q8),
                     nx, 0, XP_NOTCH_W, h, JUCE_JUSTIFY_CENTRED);

        /* The unity hairline, in the two gutters only: it must never cross the
         * digits, which is the whole reason the notch exists. */
        mod_gfx_colour(g, ui->text);
        mod_gfx_fill(g, x0 + XP_EDGE_LIT, unity, nx - x0 - XP_EDGE_LIT, cur_h);
        mod_gfx_fill(g, nx + XP_NOTCH_W, unity,
                     x0 + w - XP_EDGE_LIT - (nx + XP_NOTCH_W), cur_h);
    }
}

/* ---- the gesture --------------------------------------------------------- */

/* juce::MouseEvent carries the position as two floats at +0x00 -- the same read
 * the MOD SETTINGS overlay makes (see menu/). */
#define ME_X 0x00
#define ME_Y 0x04

/* Where the finger was on the previous event. Every step is measured from here
 * rather than from the press, which is what lets the bend survive a sideways
 * excursion: travel that goes to the loop simply adds nothing to it. */
static float xpad_g_last_x, xpad_g_last_y;

static int xpad_event_pos(void *event, float pos[2])
{
    return mod_safe_read((uintptr_t)event + ME_X, pos, 2 * sizeof(float));
}

static void xpad_settle(void)
{
    if (xpad_g_touch.semis >  (float)XP_SEMITONES) xpad_g_touch.semis =  (float)XP_SEMITONES;
    if (xpad_g_touch.semis < -(float)XP_SEMITONES) xpad_g_touch.semis = -(float)XP_SEMITONES;
    xpad_g_touch.snapped = xpad_g_touch.semis >  -XP_SNAP_ST &&
                           xpad_g_touch.semis <   XP_SNAP_ST;
    if (xpad_g_touch.snapped) xpad_g_touch.semis = 0.0f;
}

static void xpad_pad_down(void *self, void *event)
{
    float pos[2];
    int   latched;

    if (xpad_event_pos(event, pos) != 0) return;
    xpad_g_last_x = pos[0];
    xpad_g_last_y = pos[1];

    /* A press reads zero wherever it lands, and picks the brick under it. Both
     * halves of that are the point: the DJ chooses a loop by putting a finger on
     * it, and starts bending from there rather than from wherever the strip
     * thinks unity is.
     *
     * UNLESS HOLD IS LATCHING A BEND, and then it carries on from it. HOLD's
     * whole meaning is that the gesture survives the lift; zeroing on the next
     * press broke that at the one moment the DJ reached back for it, and did it
     * audibly, because the held bend is what is sounding. Carrying the value
     * costs nothing -- the axis is a running total, so where the finger lands
     * has never meant a pitch. Read BEFORE the brick is picked: `div` is what
     * says something is latched, and it is about to be overwritten. */
    latched = xpad_g_hold && xpad_g_touch.div != XP_DIV_NONE;

    xpad_g_touch.div = brick_at((int)pos[0]);
    if (!latched) {
        xpad_g_touch.semis   = 0.0f;
        xpad_g_touch.snapped = 1;
    }
    xpad_g_touch.held = 1;

    juce_comp_repaint((uintptr_t)self);
    juce_comp_repaint(xpad_g_pane);
}

static void xpad_pad_drag(void *self, void *event)
{
    int32_t b[4];
    float pos[2], dx, dy, adx, ady;
    int travel;

    if (juce_comp_bounds((uintptr_t)self, b) != 0) return;
    if (xpad_event_pos(event, pos) != 0) return;
    travel = xpad_travel(b[3]);
    if (travel <= 0) return;

    dx  = pos[0] - xpad_g_last_x;
    dy  = pos[1] - xpad_g_last_y;
    adx = dx < 0 ? -dx : dx;
    ady = dy < 0 ? -dy : dy;

    /* A FINGER CANNOT DO THAT. A step this large is a second contact, or a lift
     * reported at the origin -- both of which arrive as one enormous delta and
     * would slam the bend to an end stop. Re-anchor and change nothing: the next
     * real step is measured from where the finger actually is.
     *
     * Seen on the deck: lifting off the pad drove the readout to +50% from a
     * touch that never moved. */
    if (adx > (float)XP_JUMP_PX || ady > (float)XP_JUMP_PX) {
        xpad_g_last_x = pos[0];
        xpad_g_last_y = pos[1];
        return;
    }

    /* Route THIS step by its own direction. Nothing is latched, so a single
     * touch can bend, cross to another brick carrying the bend, and bend again. */
    if (adx > ady) {
        if (adx >= (float)XP_STEP_PX) {
            int d = brick_at((int)pos[0]);

            if (d != XP_DIV_NONE) xpad_g_touch.div = d;
            xpad_g_last_x = pos[0];
            xpad_g_last_y = pos[1];
        }
        /* Below the floor nothing moves and nothing is consumed, so a slow
         * crawl still arrives -- the residual stays in the delta. */
    } else if (ady > 0.0f) {
        /* Up is positive, which is the way the fill grows and the way the
         * readout signs it. */
        xpad_g_touch.semis += -dy * (float)XP_SEMITONES / (float)travel;
        xpad_settle();
        xpad_g_last_x = pos[0];
        xpad_g_last_y = pos[1];
    }

    juce_comp_repaint((uintptr_t)self);
    juce_comp_repaint(xpad_g_pane);
}

/* Lifting ends the gesture unless HOLD is lit, which latches the last loop and
 * bend exactly as they were left -- the strip keeps showing them because they
 * are still what is sounding. */
static void xpad_pad_up(void *self, void *event)
{
    (void)event;
    xpad_g_touch.held = 0;
    if (!xpad_g_hold) {
        xpad_g_touch.div     = XP_DIV_NONE;
        xpad_g_touch.semis   = 0.0f;
        xpad_g_touch.snapped = 0;
    }
    juce_comp_repaint((uintptr_t)self);
    juce_comp_repaint(xpad_g_pane);
}

uintptr_t xpad_build_pad(uintptr_t parent, int x, int y, int w, int h)
{
    static uintptr_t vt[VT_CLONE_WORDS], vptr;

    if (!vptr) {
        static const struct juce_vt_override ov[] = {
            { JUCE_VT_PAINT,     (void *)xpad_pad_paint, 0 },
            { JUCE_VT_MOUSEDOWN, (void *)xpad_pad_down,  0 },
            { JUCE_VT_MOUSEDRAG, (void *)xpad_pad_drag,  0 },
            { JUCE_VT_MOUSEUP,   (void *)xpad_pad_up,    0 },
        };

        vptr = juce_label_vt_clone(vt, ov, (int)(sizeof(ov) / sizeof(ov[0])));
        if (!vptr) return 0;
    }
    return juce_label(parent, "", XP_FONT_BRICK, 0x00000000u, mod_ui()->text,
                      vptr, x, y, w, h);
}
