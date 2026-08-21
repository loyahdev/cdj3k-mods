// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/state.c - the state every part of the row shares
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* ================================================================== */
/* State                                                              */
/* ================================================================== */

uintptr_t stems_g_orig_ta_paint, stems_g_orig_ta_mousedown;
uintptr_t stems_g_orig_drc_timer;   /* the app's display refresh, chained by our tick */
int       stems_g_dumped_stock;   /* the app's own open layout has been logged once */
int       stems_g_api_ok;        /* every juce primitive verified at install */
int       stems_g_built;         /* the button + row exist and are attached  */
int       stems_g_building;      /* re-entry guard: building repaints        */
int       stems_g_row_open;      /* the STEMS button is lit / the row is up  */

uintptr_t stems_g_btn_stems;     /* the 4th quick-menu button (a Label)       */
/* Its plate colour, held here instead of in the Label's own background.
 *
 * The stipple has to go down BEFORE juce::Label::paint, because Label draws its
 * background and its lettering in one call and a pattern laid after would fall across
 * the word. So the Label's background is transparent and this is what the paint hook
 * reads -- which also keeps it off findColour, a keyed-array walk that has no business
 * running per frame. */
/* Its STATE, not its colour: a stored ARGB is a cache that goes stale on a theme
 * change, leaving the button on the previous palette until something rewrites it.
 * The colour is resolved at paint from whatever theme is in force. */
enum btn_state stems_g_btn_state = BTN_OFF;
uintptr_t stems_g_warn;          /* the badge in its corner, hidden unless earned */
/* Blink budget, counted down on the display tick. Odd/even decides the badge's
 * colour, so one variable carries both "still blinking" and "which half". */
int       stems_g_warn_blink;
int       stems_g_warn_up;       /* the badge is currently shown               */
uintptr_t stems_g_edit;          /* "the stems are doing something", other corner */
uint32_t  stems_g_edit_col;      /* what it is currently painted, 0 = hidden   */
uintptr_t stems_g_row;           /* the control strip (a Label, used as a panel) */
uintptr_t stems_g_btn_bypass;  /* BYPASS, left of the sliders             */
uintptr_t stems_g_caption[N_STEMS];

/* The row holds two full-size containers and shows exactly one. Swapping state is then
 * a pair of setVisible calls instead of hiding eleven widgets by hand, and the progress
 * bar genuinely replaces the row rather than covering it. */
uintptr_t stems_g_controls;      /* BYPASS + captions + rails + fills + sliders */
uintptr_t stems_g_progress;      /* the processing bar                            */
uintptr_t stems_g_prog_fill, stems_g_prog_text, stems_g_prog_track;
uintptr_t stems_g_prog_mark[N_PROG_BOUND];  /* one per handover boundary */
/* The bar's rect inside the progress container. Y is derived from the wedge band at
 * build time rather than written as a constant -- see PROG_BAR_H in ui.h. */
int32_t   stems_g_prog_x, stems_g_prog_y, stems_g_prog_w;
int       stems_g_bypass_on;   /* bypass: stems out of circuit, sliders inert    */

/* The audio thread's view of the row. See ../stem.h: these exist so stem/audio.c
 * never has to read a juce::Value from the audio callback. */
float   g_stem_gain[N_STEMS] = { 1.0f, 1.0f, 1.0f };
int     g_stem_bypass;
int       stems_g_processing;

const char *const k_stem_name[N_STEMS] = { "DRUMS", "HARMONICS", "VOCALS" };



/* Our clone of the juce::Label vtable: identical to stock except mouseDown. The two
 * words ahead of the slots (offset-to-top, typeinfo) are cloned too, so anything that
 * reaches for RTTI through vptr[-1] still finds Label's. */
uintptr_t stems_g_label_vt[VT_CLONE_WORDS];
uintptr_t stems_g_label_vptr;
uintptr_t stems_g_label_mouseup;  /* stock juce::Label::mouseUp, chained by ours */
