// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/shortcut.c - GATE CUE shortcut on the play screen.
 *
 * Visuals reproduced from the working EP122 3.22 Gate Cue update, but placed
 * from the live TIME/TEMPO widgets.  The update's paint rectangle is in the
 * rack paint context; using it as a child rectangle displaced the visible
 * control badly on hardware.
 */
#include "cue/cue.h"
#include "cue/shortcut_tap.h"
#include "juce/juce.h"
#include "juce/draw.h"
#include "kit/mod.h"
#include <stdlib.h>

#define RACK_TI   juce_class_of(ep122_sym(EP122_PLAYERINFO_RACK))
#define TIME_TI   juce_class_of(ep122_sym(EP122_PLAYERINFO_TIME))
#define TEMPO_TI  juce_class_of(ep122_sym(EP122_PLAYERINFO_TEMPO))

#define BTN_INSET_X 4
/* Sam Leone's validated native layout separates the forgiving touch target from
 * the smaller visible control: 156x80 touch, with a centered 104x40 face. */
#define BTN_TOUCH_W 156
#define BTN_H       80
#define BTN_FACE_W  104
#define BTN_FACE_H  40

#define GC_COL_ON       0xff007de1u
#define GC_COL_OFF_EDGE 0xff536570u
#define GC_COL_OFF_FILL 0xff171c20u

#define GC_LABEL_W 76
#define GC_LABEL_H 13
#define GC_LABEL_BYTES ((GC_LABEL_W * GC_LABEL_H) / 2)

static uintptr_t g_btn;
static uintptr_t g_rack;
static uintptr_t g_vptr;
static uintptr_t g_vt[VT_CLONE_WORDS];
static int g_shown;
static struct cue_shortcut_tap g_tap;

/* Exact scanline corner inset table used by gc_fill_rounded_rect(). */
static const uint8_t k_round8[8] = { 5, 3, 2, 1, 1, 0, 0, 0 };

/* Exact gc_gatecue_label_alpha4 payload from the working update. */
static const uint8_t k_gatecue_alpha4[GC_LABEL_BYTES] = {
    0x00, 0x00, 0x25, 0x54, 0x10, 0x00, 0x00, 0x01, 0x33, 0x10, 0x01, 0x33, 0x33, 0x33, 0x33, 0x13,
    0x33, 0x33, 0x33, 0x31, 0x00, 0x00, 0x00, 0x00, 0x14, 0x64, 0x10, 0x00, 0x23, 0x20, 0x00, 0x13,
    0x30, 0x03, 0x33, 0x33, 0x33, 0x31, 0x00, 0x5d, 0xff, 0xff, 0xe6, 0x00, 0x00, 0x0a, 0xff, 0xa0,
    0x08, 0xff, 0xff, 0xff, 0xff, 0x6c, 0xff, 0xff, 0xff, 0xf9, 0x00, 0x00, 0x00, 0x19, 0xff, 0xff,
    0xe6, 0x00, 0xcf, 0xc0, 0x00, 0x6f, 0xf3, 0x0c, 0xff, 0xff, 0xff, 0xf9, 0x04, 0xff, 0xff, 0xff,
    0xff, 0x40, 0x00, 0x1f, 0xff, 0xf1, 0x09, 0xff, 0xff, 0xff, 0xff, 0x6c, 0xff, 0xff, 0xff, 0xf9,
    0x00, 0x00, 0x00, 0xaf, 0xff, 0xff, 0xff, 0x50, 0xcf, 0xc0, 0x00, 0x6f, 0xf3, 0x0c, 0xff, 0xff,
    0xff, 0xf9, 0x0d, 0xff, 0xb2, 0x4c, 0xff, 0xa0, 0x00, 0x7f, 0xff, 0xf7, 0x01, 0x22, 0x7f, 0xf6,
    0x22, 0x1c, 0xfd, 0x22, 0x22, 0x22, 0x00, 0x00, 0x03, 0xff, 0xf5, 0x3b, 0xff, 0xb0, 0xcf, 0xc0,
    0x00, 0x6f, 0xf3, 0x0c, 0xfd, 0x22, 0x22, 0x22, 0x1f, 0xfd, 0x10, 0x02, 0xed, 0x90, 0x00, 0xdf,
    0xff, 0xfd, 0x00, 0x00, 0x6f, 0xf4, 0x00, 0x0c, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xff,
    0x70, 0x01, 0xee, 0xa0, 0xcf, 0xc0, 0x00, 0x6f, 0xf3, 0x0c, 0xfc, 0x00, 0x00, 0x00, 0x5f, 0xf6,
    0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xab, 0xff, 0x40, 0x00, 0x6f, 0xf4, 0x00, 0x0c, 0xff, 0xaa,
    0xaa, 0xa1, 0x00, 0x00, 0x0b, 0xff, 0x00, 0x00, 0x10, 0x00, 0xcf, 0xc0, 0x00, 0x6f, 0xf3, 0x0c,
    0xff, 0xaa, 0xaa, 0xa1, 0x7f, 0xf4, 0x02, 0xee, 0xee, 0xe0, 0x09, 0xff, 0x55, 0xff, 0xa0, 0x00,
    0x6f, 0xf4, 0x00, 0x0c, 0xff, 0xff, 0xff, 0xf3, 0x00, 0x00, 0x0d, 0xfe, 0x00, 0x00, 0x00, 0x00,
    0xcf, 0xc0, 0x00, 0x6f, 0xf3, 0x0c, 0xff, 0xff, 0xff, 0xf3, 0x5f, 0xf5, 0x02, 0xff, 0xff, 0xf0,
    0x1e, 0xff, 0x88, 0xff, 0xf1, 0x00, 0x6f, 0xf4, 0x00, 0x0c, 0xfe, 0x77, 0x77, 0x72, 0x00, 0x00,
    0x0c, 0xff, 0x00, 0x00, 0x21, 0x00, 0xcf, 0xc0, 0x00, 0x7f, 0xf3, 0x0c, 0xfe, 0x77, 0x77, 0x72,
    0x2f, 0xfb, 0x00, 0x33, 0xbf, 0xf0, 0x6f, 0xff, 0xff, 0xff, 0xf7, 0x00, 0x6f, 0xf4, 0x00, 0x0c,
    0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xff, 0x40, 0x01, 0xef, 0xb0, 0xcf, 0xe0, 0x00, 0x8f,
    0xf2, 0x0c, 0xfc, 0x00, 0x00, 0x00, 0x0c, 0xff, 0xa4, 0x48, 0xef, 0xf0, 0xcf, 0xf9, 0x99, 0x9f,
    0xfd, 0x00, 0x6f, 0xf4, 0x00, 0x0c, 0xfe, 0x44, 0x44, 0x42, 0x00, 0x00, 0x04, 0xff, 0xe5, 0x5b,
    0xff, 0xa0, 0xaf, 0xfa, 0x45, 0xef, 0xf1, 0x0c, 0xfe, 0x44, 0x44, 0x42, 0x01, 0xdf, 0xff, 0xff,
    0xff, 0xc3, 0xff, 0xb0, 0x00, 0x0b, 0xff, 0x40, 0x6f, 0xf4, 0x00, 0x0c, 0xff, 0xff, 0xff, 0xfc,
    0x00, 0x00, 0x00, 0x9f, 0xff, 0xff, 0xfe, 0x20, 0x3f, 0xff, 0xff, 0xff, 0x90, 0x0c, 0xff, 0xff,
    0xff, 0xfc, 0x00, 0x29, 0xdf, 0xfe, 0xc6, 0x07, 0xff, 0x60, 0x00, 0x05, 0xff, 0x80, 0x6f, 0xf3,
    0x00, 0x0b, 0xff, 0xff, 0xff, 0xfc, 0x00, 0x00, 0x00, 0x06, 0xdf, 0xff, 0xb3, 0x00, 0x04, 0xdf,
    0xff, 0xe9, 0x10, 0x0b, 0xff, 0xff, 0xff, 0xfc, 0x00, 0x00, 0x01, 0x20, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x21, 0x00, 0x00, 0x00, 0x02, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void cue_exact_colour(void *g, uint32_t argb)
{
    mod_draw_enter();
    mod_gfx_colour(g, argb);
    mod_draw_leave();
}

static void cue_round_fill(void *g, int x, int y, int w, int h, int radius)
{
    int i, start;

    if (radius < 1 || radius > 8 || w <= 2 * radius || h <= 2 * radius) {
        mod_gfx_fill(g, x, y, w, h);
        return;
    }

    start = 8 - radius;
    for (i = 0; i < radius; i++) {
        int inset = k_round8[start + i];
        mod_gfx_fill(g, x + inset, y + i, w - 2 * inset, 1);
        mod_gfx_fill(g, x + inset, y + h - i - 1, w - 2 * inset, 1);
    }
    mod_gfx_fill(g, x, y + radius, w, h - 2 * radius);
}

static unsigned cue_alpha_at(int x, int y)
{
    unsigned int p = (unsigned int)y * GC_LABEL_W + (unsigned int)x;
    uint8_t b = k_gatecue_alpha4[p >> 1];
    return (p & 1U) ? (unsigned)(b & 0x0fU) : (unsigned)(b >> 4);
}

static void cue_draw_label(void *g, int x, int y)
{
    unsigned int a;
    int row;

    for (a = 1; a <= 15; a++) {
        cue_exact_colour(g, (a * 0x11000000u) | 0x00ffffffu);
        for (row = 0; row < GC_LABEL_H; row++) {
            int col = 0;
            while (col < GC_LABEL_W) {
                int start;
                while (col < GC_LABEL_W && cue_alpha_at(col, row) != a)
                    col++;
                start = col;
                while (col < GC_LABEL_W && cue_alpha_at(col, row) == a)
                    col++;
                if (col > start)
                    mod_gfx_fill(g, x + start, y + row, col - start, 1);
            }
        }
    }
}

static void cue_shortcut_paint(void *self, void *g)
{
    int32_t b[4];
    int lx, ly, face_w, face_h, face_x, face_y;
    int active;

    if (!self || !g || juce_comp_bounds((uintptr_t)self, b) != 0)
        return;

    face_w = b[2] < BTN_FACE_W ? b[2] : BTN_FACE_W;
    face_x = (b[2] - face_w) / 2;
    face_h = b[3] < BTN_FACE_H ? b[3] : BTN_FACE_H;
    face_y = (b[3] - face_h) / 2;
    active = cue_gate_runtime_active();
    if (active) {
        cue_exact_colour(g, GC_COL_ON);
        cue_round_fill(g, face_x, face_y, face_w, face_h, 8);
    } else {
        cue_exact_colour(g, GC_COL_OFF_EDGE);
        cue_round_fill(g, face_x, face_y, face_w, face_h, 8);
        if (face_w > 4 && face_h > 4) {
            cue_exact_colour(g, GC_COL_OFF_FILL);
            cue_round_fill(g, face_x + 2, face_y + 2, face_w - 4, face_h - 4, 6);
        }
    }

    lx = (b[2] - GC_LABEL_W) / 2;
    ly = (b[3] - GC_LABEL_H) / 2;
    cue_draw_label(g, lx, ly);
    g_shown = active;
}

static void cue_shortcut_mousedown(void *self, void *event)
{
    (void)self;
    (void)event;
    if (g_gate_on)
        cue_tap_down(&g_tap, cue_gate_runtime_active());
}

static void cue_shortcut_mouseup(void *self, void *event)
{
    float pos[2];
    int32_t b[4];
    int inside = g_gate_on && self && event &&
                 !mod_safe_read((uintptr_t)event, pos, sizeof(pos)) &&
                 !juce_comp_bounds((uintptr_t)self, b) &&
                 pos[0] >= 0 && pos[1] >= 0 && pos[0] < b[2] && pos[1] < b[3];
    cue_tap_up(&g_tap, inside);
}

void cue_shortcut_tick(void)
{
    int desired;
    cue_gate_tick();
    /* The native parent also sees child mouse-up events. Apply an absolute
     * state after event dispatch so its old hit rectangle cannot double-toggle
     * this button, and taps anywhere in the enlarged button work equally. */
    if (!cue_tap_take(&g_tap, g_gate_on, &desired))
        return;
    if (cue_gate_runtime_active() != desired && cue_gate_runtime_set_active(desired) != 0)
        MWARN("cue_shortcut: Gate Cue state unavailable\n");
    cue_shortcut_refresh();
}

static void cue_shortcut_build(uintptr_t rack)
{
    static const struct juce_vt_override ov[] = {
        { JUCE_VT_PAINT,     (void *)cue_shortcut_paint,     NULL },
        { JUCE_VT_MOUSEDOWN, (void *)cue_shortcut_mousedown, NULL },
        { JUCE_VT_MOUSEUP,   (void *)cue_shortcut_mouseup,   NULL },
    };
    uintptr_t time_w, tempo_w;
    int32_t tb[4], pb[4];
    int x, y, w;

    time_w = juce_comp_child_of_class(rack, TIME_TI);
    tempo_w = juce_comp_child_of_class(rack, TEMPO_TI);
    if (!time_w || !tempo_w ||
        juce_comp_bounds(time_w, tb) != 0 ||
        juce_comp_bounds(tempo_w, pb) != 0)
        return;

    /* The established source layout measures the real free gap.  On the deck
     * this is between TIME {395,13,327,80} and TEMPO {839,13,190,83}; it also
     * follows firmware layouts that move either neighbour. */
    x = tb[0] + tb[2] + BTN_INSET_X;
    w = BTN_TOUCH_W;
    y = tb[1] + (tb[3] - BTN_H) / 2;
    if (tb[3] < BTN_H || pb[0] <= x) {
        g_rack = rack;
        return;
    }

    if (!g_vptr) {
        g_vptr = juce_label_vt_clone(
            g_vt, ov, (int)(sizeof(ov) / sizeof(ov[0])));
        if (!g_vptr)
            return;
    }

    g_btn = juce_label(rack, "", 1.0f, 0x00000000u, 0x00000000u,
                       g_vptr, x, y, w, BTN_H);
    if (!g_btn)
        return;

    g_rack = rack;
    g_shown = cue_gate_runtime_active();
    juce_comp_set_visible(g_btn, g_gate_on ? 1 : 0);
    juce_comp_repaint(g_btn);
    MDBG("cue_shortcut: GATE CUE measured between TIME/TEMPO at {%d,%d,%d,%d}"
         " in player-info %#lx\n", x, y, w, BTN_H, (unsigned long)rack);
}

static uintptr_t g_orig_anchor_paint;

/* NormalPlayerInfoWidget is model-first and juce::Component-second. Its primary
 * vtable is useful as an RTTI identity, but JUCE_VT_PAINT is not a Component
 * slot in that primary table. Anchor on TouchAria, whose primary object really
 * is a Component, then locate the live player-info Component subobject by RTTI.
 * This is also late enough that the play-screen tree is fully attached. */
static void cue_shortcut_anchor_paint(void *self, void *g)
{
    uintptr_t rack;

    if (g_orig_anchor_paint)
        ((void (*)(void *, void *))g_orig_anchor_paint)(self, g);

    rack = juce_comp_find_class(juce_comp_root((uintptr_t)self), RACK_TI);
    if (rack && (!g_rack || g_rack != rack)) {
        g_btn = 0;
        g_rack = 0;
        memset(&g_tap, 0, sizeof(g_tap));
        cue_shortcut_build(rack);
    }
    cue_shortcut_refresh();
}

void cue_shortcut_refresh(void)
{
    int active = cue_gate_runtime_active();

    if (!g_gate_on)
        memset(&g_tap, 0, sizeof(g_tap));
    if (!g_btn)
        return;
    juce_comp_set_visible(g_btn, g_gate_on ? 1 : 0);
    if (g_shown != active) {
        g_shown = active;
        juce_comp_repaint(g_btn);
    }
}

static int cue_shortcut_install(void)
{
    if (!FN_LABEL_CTOR || !FN_LABEL_SETFONT || !FN_LABEL_JUSTIFY ||
        !FN_ADD_VISIBLE || !FN_SET_BOUNDS ||
        !FN_COMP_SETCOLOUR || !MOD_FN_GFX_SETCOLOUR || !MOD_FN_GFX_FILLRECT) {
        MDBG("cue_shortcut: JUCE primitives did not resolve -> no shortcut\n");
        return -1;
    }
    if (!RACK_TI || !TIME_TI || !TEMPO_TI || !ep122_sym(EP122_TOUCHARIA) ||
        mod_patch_vslot("cueShortcutPaint", EP122_TOUCHARIA, JUCE_VT_PAINT,
                        (void *)cue_shortcut_anchor_paint,
                        &g_orig_anchor_paint) != 0) {
        MDBG("cue_shortcut: no anchor -> no shortcut\n");
        return -1;
    }

    MDBG("cue_shortcut: installed visible Gate Cue plate on player-info lifecycle%s\n",
         cue_gate_native_owned() ? " (mirroring recovered transport)" : "");
    return 0;
}

KIT_MOD(k_mod_cue_shortcut,
        .name = "cue_shortcut", .prio = 11, .install = cue_shortcut_install,
        .what = "native EP122 3.22 GATE CUE shortcut UI");
