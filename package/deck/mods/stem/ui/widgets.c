// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/widgets.c - juce primitives: a Label made into a flat button
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* ================================================================== */
/* Small helpers                                                      */
/* ================================================================== */

void stems_set_visible(uintptr_t comp, int visible)
{
    juce_comp_set_visible(comp, visible);
}

int stems_bounds(uintptr_t comp, int32_t out[4])
{
    return juce_comp_bounds(comp, out);
}

void stems_colour(uintptr_t comp, int id, uint32_t argb)
{
    juce_comp_colour(comp, id, argb);
}

/* Halfway to white: a marked bar reads as emphasis rather than as a bar belonging to
 * something else. The distance is the only thing that differs from a touched button
 * (MOD_CHECKER_HOT_Q8), which is why both come off one helper. */
uint32_t stems_lighter(uint32_t argb)
{
    return mod_colour_lift(argb, 128u);
}

/* Retext a Label we built. juce's ValueSource does not compare, so calling this
 * with an unchanged string still sends a repaint: don't, per frame. */
void stems_text(uintptr_t label, const char *text)
{
    if (stems_g_api_ok) juce_label_text(label, text);
}

/* ================================================================== */
/* juce::Label as a flat button                                       */
/* ================================================================== */


/* Clone once, at first use. The overrides are ours; the check that Label's paint
 * is still in Label's paint slot -- which stems_label_paint CHAINS to -- belongs
 * to juce_label_vt_clone and is why this is not a memcpy here. */
int stems_label_vt_ready(void)
{
    const struct juce_vt_override ov[] = {
        { VT_SLOT_MOUSEDOWN, (void *)stems_label_mousedown, NULL },
        { VT_SLOT_MOUSEUP,   (void *)stems_label_mouseup,   &stems_g_label_mouseup },
        { VT_SLOT_PAINT,     (void *)stems_label_paint,     NULL },
    };

    if (stems_g_label_vptr) return 1;
    stems_g_label_vptr = juce_label_vt_clone(stems_g_label_vt, ov,
                                             (int)(sizeof(ov) / sizeof(ov[0])));
    if (!stems_g_label_vptr) return 0;
    MDBG("stems: cloned juce::Label vtable -> %#lx (mouseDown %p)\n",
         (unsigned long)stems_g_label_vptr, (void *)stems_label_mousedown);
    return 1;
}

/* Build one Label. `clickable` swaps in the cloned vtable; leave it clear for plain
 * captions and for the strip itself, which only ever need to paint. */
uintptr_t stems_label(uintptr_t parent, const char *text, float font_h,
                             uint32_t bg, uint32_t fg, int clickable,
                             int x, int y, int w, int h)
{
    uintptr_t p = juce_label(parent, text, font_h, bg, fg,
                             clickable ? stems_g_label_vptr : 0, x, y, w, h);

    if (p) juce_comp_colour(p, LBL_COL_OUTLINE, COL_OUTLINE);
    return p;
}
