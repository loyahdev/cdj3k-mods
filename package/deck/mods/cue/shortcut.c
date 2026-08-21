// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/shortcut.c - the GATE CUE shortcut on the play screen's bottom rack.
 *
 * The MOD SETTINGS row is three screens away from a deck that is playing, and
 * the gate is the one setting a DJ changes mid-set. This is the same flag with
 * a thumb on it: white on blue while the gate is armed, dim while it is not.
 *
 * Where it goes (RE-verified against EP122-3.19)
 * ----------------------------------------------
 * The rack is gui::NormalPlayerInfoWidget. It is NOT reachable by name: the
 * gui::PlayerInfo*Widget classes are UpdaterComponent models whose primary
 * vtable is nine slots of listener, and the juce::Component each of them also
 * IS lives at a secondary vtable. So the rack and its neighbours are found by
 * TYPEINFO, which every vtable in a class's group carries -- see juce.h.
 *
 * The button sits in the gap between the timer and the tempo, and the gap is
 * measured rather than written down: the timer's right edge and the tempo's
 * left edge come off the live components, so a firmware that moves either one
 * moves the button with it, and it is centred on the timer for the same reason.
 * Measured on 3.19, in rack coordinates, the timer is {395,13,327,80} and the
 * tempo {839,13,190,83} -- a 117px gap, with the SINGLE play-mode caption
 * {643,13,80,12} ending above it.
 */
#include "cue/cue.h"
#include "juce/juce.h"
#include "core/mod_settings.h"
#include "theme/theme.h"
#include "kit/mod.h"

/* Identity of the three widgets this needs, as typeinfo. See juce.h for why the
 * vtable itself is the wrong handle for a model-first widget. */
#define RACK_TI   juce_class_of(ep122_sym(EP122_PLAYERINFO_RACK))
#define TIME_TI   juce_class_of(ep122_sym(EP122_PLAYERINFO_TIME))
#define TEMPO_TI  juce_class_of(ep122_sym(EP122_PLAYERINFO_TEMPO))

/* The button, inside the measured gap. Insets keep it clear of both neighbours
 * rather than filling the space between them, which would read as a third
 * readout rather than as a control. */
#define BTN_INSET_X   8

/* Height is the plate's padding, and the rack's own badges set it: A.HOT CUE, MT
 * and +-10 are all 28px around lettering this size. A few px over that, because
 * unlike those three this one is pressed -- 28px is 4.4mm on the deck's glass,
 * which is under what a thumb hits reliably mid-set. Centred on the timer rather
 * than placed at a y of its own, so it sits on the row its neighbours sit on
 * whatever the firmware does with them. */
#define BTN_H         32
#define BTN_MIN_W     70

/* One line, at the size the rack sets its own captions in -- SINGLE and TEMPO on
 * either side, and A.HOT CUE, which fits nine characters in a box the same width
 * as this one.
 *
 * Sized with margin rather than to the edge: measured on the glass, "GATE CUE"
 * is 78px of the 91px juce::Label leaves inside a 101px box, so ~17.5 is where it
 * would start to squash. Label narrows text to its minimum horizontal scale
 * before it clips, so overshooting reads as a condensed font rather than as
 * something obviously wrong -- which is the failure worth leaving room against. */
#define BTN_FONT      15.0f
#define BTN_TEXT      "GATE CUE"

static uintptr_t g_btn;
static uintptr_t g_rack;
static uintptr_t g_vptr;
static uintptr_t g_vt[VT_CLONE_WORDS];
static int       g_shown;               /* what the button is currently painted as */

/* Lit is a filled accent plate with the deck's own lettering on it -- the same
 * shape as a selected DJ SETTING row, which is where a DJ has already learnt what
 * white-on-blue means here. Unlit is an outline and no fill, like A.HOT CUE and MT
 * two readouts away: an OFF shortcut belongs to the rack rather than sitting on it
 * as a second slab of grey.
 *
 * `text` rather than `text_on_accent` on the lit plate. That role is tuned for the
 * BYPASS amber, which is a light fill; the accent is the deck's blue and carries
 * white the way the deck's own does. A theme that made the accent light would want
 * the other role. */
static void cue_shortcut_paint_state(void)
{
    const struct theme_ui *ui = mod_ui();
    int on = g_gate_on ? 1 : 0;

    if (!g_btn) return;
    juce_comp_colour(g_btn, LBL_COL_BG,      on ? ui->accent : 0x00000000u);
    juce_comp_colour(g_btn, LBL_COL_TEXT,    on ? ui->text   : ui->text_dim);
    juce_comp_colour(g_btn, LBL_COL_OUTLINE, on ? 0x00000000u : ui->edge);
    g_shown = on;
}

/* The press. Toggles the same flag the MOD SETTINGS row owns, and persists it
 * for the same reason that row does -- a shortcut a restart silently undoes is
 * worse than no shortcut. */
static void cue_shortcut_mousedown(void *self, void *event)
{
    (void)self; (void)event;
    g_gate_on = !g_gate_on;
    cue_shortcut_paint_state();
    mods_settings_save();
    MDBG("cue_shortcut: GATE CUE -> %s (shortcut)\n", g_gate_on ? "ON" : "OFF");
}

/* Build once, when the rack has both neighbours attached. A rack that is still
 * being wired up is simply not ready yet, and the next repaint tries again. */
static void cue_shortcut_build(uintptr_t rack)
{
    static const struct juce_vt_override ov[] = {
        { JUCE_VT_MOUSEDOWN, (void *)cue_shortcut_mousedown, NULL },
    };
    uintptr_t time_w, tempo_w;
    int32_t tb[4], pb[4];
    int x, y, w;

    time_w  = juce_comp_child_of_class(rack, TIME_TI);
    tempo_w = juce_comp_child_of_class(rack, TEMPO_TI);
    if (!time_w || !tempo_w) {
        MDBG("cue_shortcut: rack %#lx has timer=%#lx tempo=%#lx -> not ready\n",
             (unsigned long)rack, (unsigned long)time_w, (unsigned long)tempo_w);
        return;
    }
    if (juce_comp_bounds(time_w, tb) != 0 || juce_comp_bounds(tempo_w, pb) != 0)
        return;

    x = tb[0] + tb[2] + BTN_INSET_X;
    w = pb[0] - x - BTN_INSET_X;
    y = tb[1] + (tb[3] - BTN_H) / 2;
    if (w < BTN_MIN_W) {
        MDBG("cue_shortcut: only %dpx between the timer and the tempo -> no shortcut\n", w);
        g_rack = rack;                  /* remembered, so this is not retried per frame */
        return;
    }

    if (!g_vptr) {
        g_vptr = juce_label_vt_clone(g_vt, ov, (int)(sizeof(ov) / sizeof(ov[0])));
        if (!g_vptr) return;
    }
    g_btn = juce_label(rack, BTN_TEXT, BTN_FONT, 0x00000000u, mod_ui()->text_dim,
                       g_vptr, x, y, w, BTN_H);
    if (!g_btn) return;
    g_rack = rack;
    cue_shortcut_paint_state();
    MDBG("cue_shortcut: GATE CUE shortcut at {%d,%d,%d,%d} in rack %#lx\n",
         x, y, w, BTN_H, (unsigned long)rack);
}

/* Anchored on the waveform title bar's TouchAria, the same component the STEMS
 * row builds from and for the same reason: its paint fires once the play screen
 * is wired up, and the rack is a walk away from the root.
 *
 * Not the rack's own paint. The rack's juce::Component is a secondary subobject,
 * so there is no vtable in the spec that can be patched at the Component slot
 * numbering without writing over the listener vtable that shares its group. */
static uintptr_t g_orig_ta_paint;

static void cue_shortcut_ta_paint(void *self, void *g)
{
    if (g_orig_ta_paint)
        ((void (*)(void *, void *))g_orig_ta_paint)(self, g);

    if (!g_rack) {
        uintptr_t rack = juce_comp_find_class(juce_comp_root((uintptr_t)self), RACK_TI);

        if (rack) cue_shortcut_build(rack);
    }
}

/* The MOD SETTINGS row and the shortcut are two thumbs on one flag, so whichever
 * moves it tells the other. gate_cue.c hands this to its row as `changed`. */
void cue_shortcut_refresh(void)
{
    if (g_btn && g_shown != (g_gate_on ? 1 : 0))
        cue_shortcut_paint_state();
}

static int cue_shortcut_install(void)
{
    if (!FN_LABEL_CTOR || !FN_ADD_VISIBLE || !FN_SET_BOUNDS || !FN_FONT_BUILD ||
        !FN_LABEL_SETFONT || !FN_LABEL_JUSTIFY || !FN_COMP_SETCOLOUR) {
        MDBG("cue_shortcut: juce primitives did not resolve -> no shortcut\n");
        return -1;
    }
    if (!RACK_TI || !TIME_TI || !TEMPO_TI) {
        MDBG("cue_shortcut: player-info rack did not resolve -> no shortcut\n");
        return -1;
    }
    if (mod_patch_vslot("cueShortcutPaint", EP122_TOUCHARIA, JUCE_VT_PAINT,
                        (void *)cue_shortcut_ta_paint, &g_orig_ta_paint) != 0) {
        MDBG("cue_shortcut: no anchor -> no shortcut\n");
        return -1;
    }
    MDBG("cue_shortcut: installed (waiting for the play screen to draw)\n");
    return 0;
}

KIT_MOD(k_mod_cue_shortcut,
        .name = "cue_shortcut", .prio = 11, .install = cue_shortcut_install,
        .what = "GATE CUE shortcut on the player-info rack");
