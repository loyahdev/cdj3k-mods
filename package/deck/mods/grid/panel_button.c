// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/grid/panel_button.c - one button on the grid panel: its plate, its arrows, its label and its presses.
 */
#include "grid/panel_internal.h"
#include "core/ep122_syms.h"
#include "juce/draw.h"
#include "juce/juce.h"
#include "kit/mod.h"
#include "stem/stem.h"

static int gp_btn_index(uintptr_t self)
{
    int i;

    for (i = 0; i < GP_N; i++)
        if (gp_g_btn[i] == self)
            return i;
    return -1;
}

static void gp_plate(void *g, int x, int y, int w, int h, int state)
{
    int e = GP_BTN_EDGE;
    uint32_t line = GP_COL_LINE, fill = GP_COL_FILL;

    if (w <= 2 * e || h <= 2 * e)
        return;
    if (state == GP_STATE_ON) {
        line = GP_COL_LINE_ON;
        fill = GP_COL_FILL_ON;
    } else if (state == GP_STATE_OFF) {
        line = GP_COL_LINE_OFF;
        fill = GP_COL_FILL_OFF;
    }
    /* Through the theme, because these are deck values and not roles -- see
     * mod_colour_stock, and the note on the colours in panel_internal.h. */
    mod_gfx_colour(g, mod_colour_stock(line));
    mod_gfx_fill(g, x, y, w, h);
    mod_gfx_colour(g, mod_colour_stock(fill));
    mod_gfx_fill(g, x + e, y + e, w - 2 * e, h - 2 * e);
}

static int gp_reset_active(uintptr_t comp)
{
    uint8_t v = 0;

    if (!comp || mod_safe_read(comp - GP_ACTIVE_OFF, &v, sizeof(v)) != 0)
        return stem_grid_offset() != 0;   /* the grid itself, if it cannot be read */
    return v == GP_ACTIVE_VAL;
}

void gp_reset_paint(void *self, void *g)
{
    int32_t b[4];
    int on;

    if (juce_comp_bounds((uintptr_t)self, b) != 0)
        return;
    on = gp_reset_active((uintptr_t)self);

    gp_plate(g, GP_PLATE_INSET, GP_PLATE_INSET,
             b[2] - 2 * GP_PLATE_INSET, b[3] - 2 * GP_PLATE_INSET,
             on ? GP_STATE_IDLE : GP_STATE_OFF);
    mod_gfx_text(g, "RESET", GP_RESET_FONT,
                 mod_colour_stock(on ? GP_COL_TEXT : GP_COL_TEXT_OFF),
                 0, 0, b[2], b[3], JUCE_JUSTIFY_CENTRED);
}

/* One triangle, `right` for the direction it points. Columns, base to apex. */
static void gp_tri(void *g, int cx, int cy, int right)
{
    int i;

    for (i = 0; i < GP_ARROW_W; i++) {
        int half = (GP_ARROW_HALF * (GP_ARROW_W - 1 - i)) / (GP_ARROW_W - 1);
        int x    = right ? cx - GP_ARROW_W / 2 + i : cx + GP_ARROW_W / 2 - i;

        mod_gfx_fill(g, x, cy - half, 1, 2 * half + 1);
    }
}

static void gp_arrows(void *g, int which, int w, int h)
{
    int cy = h / 2;

    if (which != GP_ENLARGE && which != GP_REDUCE)
        return;
    mod_gfx_colour(g, mod_colour_stock(GP_COL_TEXT));
    gp_tri(g, w / 2 - GP_ARROW_OFF, cy, which == GP_REDUCE);
    gp_tri(g, w / 2 + GP_ARROW_OFF, cy, which == GP_ENLARGE);
}

void gp_label_paint(void *self, void *g)
{
    int i = gp_btn_index((uintptr_t)self);
    int hot = (i >= 0 && i == gp_g_hot);
    int state = hot ? GP_STATE_ON : GP_STATE_IDLE;
    int32_t b[4];

    if (i < 0 || juce_comp_bounds((uintptr_t)self, b) != 0) {
        ((void (*)(void *, void *))LABEL_FN_PAINT)(self, g);
        return;
    }
    /* OUR RESET says whether there is anything to undo, the same as the deck's
     * beside it -- and here the answer is exact, because this is the state the
     * panel itself keeps. */
    if (i == GP_RESET && !hot && gp_g_mult == 1.0 && gp_g_steps == 0) {
        state = GP_STATE_OFF;
        juce_comp_colour((uintptr_t)self, LBL_COL_TEXT, GP_COL_TEXT_OFF);
    } else if (i == GP_RESET) {
        juce_comp_colour((uintptr_t)self, LBL_COL_TEXT, GP_COL_TEXT);
    }

    gp_plate(g, GP_PLATE_INSET, GP_PLATE_INSET,
             b[2] - 2 * GP_PLATE_INSET, b[3] - 2 * GP_PLATE_INSET, state);
    ((void (*)(void *, void *))LABEL_FN_PAINT)(self, g);
    gp_arrows(g, i, b[2], b[3]);
}

void gp_label_mousedown(void *self, void *event)
{
    int i = gp_btn_index((uintptr_t)self);

    (void)event;
    if (i < 0)
        return;
    gp_g_hot = i;
    gp_repaint((uintptr_t)self);
    gp_action(i);
    /* The readout and the deck's own BPM both come off the grid we just moved,
     * so the whole strip is asked to redraw rather than just the button. */
    gp_repaint(gp_g_panel);
}

void gp_label_mouseup(void *self, void *event)
{
    int i = gp_btn_index((uintptr_t)self);

    if (i >= 0 && gp_g_hot == i) {
        gp_g_hot = -1;
        gp_repaint((uintptr_t)self);
    }
    if (gp_g_orig_mouseup)
        ((void (*)(void *, void *))gp_g_orig_mouseup)(self, event);
}
