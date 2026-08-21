// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/paint.c - row drawing, mirroring the stock paintCell field styling.
 * Font heights come off the live model, colours off the live theme.
 */
#include "menu/internal.h"

/* ================================================================== */
/* Row rendering (mirrors stock paintCell field styling)              */
/* ================================================================== */

/* Fetch a live .bss theme colour (juce ARGB); opaque-white fallback. */
void menu_draw_field(void *self, void *g, uintptr_t fonth_off, uint32_t colour,
                       const char *text, int x, int w, int h, int justif)
{
    float fh = 24.0f;
    uint32_t bits = 0;
    if (mod_safe_read((uintptr_t)self + fonth_off, &bits, sizeof(bits)) == 0) {
        float v; memcpy(&v, &bits, sizeof(v));
        if (v >= 6.0f && v <= 200.0f) fh = v;
    }
    font_ret_t font = ((font_build_t)FN_FONT_BUILD)(fh);   /* s0=fh, x8=&font */
    ((void (*)(void *, void *))FN_G_SETFONT)(g, &font);
    ((void (*)(void *))FN_FONT_DTOR)(&font);

    if (colour == 0) colour = 0xffffffffu;                 /* fallback: opaque white */
    /* THROUGH THE DRAW KIT, not setColour directly. setColour IS the theme's setFill
     * hook, and `colour` came out of mod_ui() already resolved through the theme once --
     * so calling it raw applied the palette a SECOND time. Measured on SANDSTONE: our
     * row labels came out #7b809d against the deck's own #262944 in the same list, a
     * washed blue-grey where every row above them was near-black. mod_gfx_colour
     * brackets the call, which is the whole reason it exists. */
    mod_gfx_colour(g, colour);

    uint8_t s[16] __attribute__((aligned(16)));
    int j = justif;
    ((str_ctor_t)FN_STR_CTOR)(s, text);
    ((draw_text_t)FN_DRAW_TEXT)(g, s, x, 0, w, h, &j, 1);
    ((str_dtor_t)FN_STR_DTOR)(s);
}

void menu_draw_mod_row(void *self, void *g, int w, int h, const char *label,
                      const char *value, int sel)
{
    const struct theme_ui *ui = mod_ui();

    if (!menu_g_render_ok) return;
    menu_draw_field(self, g, MODEL_LBL_FONTH_OFF, ui->text_deck, label, 0xe, w, h, 1);
    /* THE VALUE GOES TO FULL INK ON THE SELECTED ROW. That is what the deck does and it
     * is a state this ignored: measured on its own rows, the dim #7d7d7d becomes the
     * label's #ffffff the moment a row is selected, on the focused blue and on the
     * unfocused grey alike. Ours stayed dim, so our selected row read as a row that was
     * not quite selected next to the deck's. */
    menu_draw_field(self, g, MODEL_VAL_FONTH_OFF, sel ? ui->text_deck : ui->text_value,
                    value, 0, w - 0xe, h, 2);
}

/* The overlay title row: "MOD SETTINGS" in the accent colour, so the collapsed
 * list clearly reads as the mod pane rather than a stray DJ SETTING row. */
void menu_draw_mod_header(void *self, void *g, int w, int h, const char *text)
{
    if (!menu_g_render_ok) return;
    menu_draw_field(self, g, MODEL_LBL_FONTH_OFF, MOD_HEADER_COLOUR, text, 0xe, w, h, 1);
    /* The build, right-aligned where every row below puts its value. This is the
     * only place a DJ can read which mods they are running without a shell, which
     * is what makes a report from a booth actionable. drawText ellipsises, so a
     * long version degrades rather than colliding with the title. */
    menu_draw_field(self, g, MODEL_VAL_FONTH_OFF, MOD_HEADER_COLOUR,
               EP122_MOD_VERSION, 0, w - 0xe, h, 2);
}

