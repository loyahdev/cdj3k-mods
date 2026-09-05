// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem/prestem_ui.c - play-screen controls for source-built PRE-STEMS.
 *
 * This module reproduces the recovered PRE-STEMS controls as ordinary JUCE
 * children so the controls remain visible even when the companion's direct
 * paint hook does not reach this screen.  It deliberately uses the same proven
 * title-bar slot and waveform-band parents as Server Stems; the recovered paint
 * rectangles are not player-info child coordinates.
 */
#include "stem/stem.h"
#include "juce/juce.h"
#include "juce/draw.h"
#include "kit/band.h"
#include "kit/mod.h"

#include <stdlib.h>
#include <stdint.h>

/* The title button and panel use kit/band's live, stock-derived rectangles. */
#define PST_HEAD_W KIT_BAND_SLOT_W
#define PST_HEAD_H KIT_BAND_SLOT_H
#define PST_VOC_X 387
#define PST_VOC_W 246
#define PST_INS_X 647
#define PST_INS_W 246

static uintptr_t g_head;
static uintptr_t g_panel;
static uintptr_t g_status;
static uintptr_t g_parts[2], g_part_vptr, g_part_vt[VT_CLONE_WORDS];
static uintptr_t g_head_vptr;
static uintptr_t g_panel_vptr;
static uintptr_t g_head_vt[VT_CLONE_WORDS];
static uintptr_t g_panel_vt[VT_CLONE_WORDS];
static int g_building;
static int g_api_ok;
static char g_last_detail[32];
static int g_menu_open;
static int g_last_master = -1;
static unsigned int g_last_state = ~0U;
static unsigned int g_last_flags = ~0U;
static int g_last_status = -1;
static unsigned int g_last_theme_gen = ~0U;
static int g_panel_h = 84;

enum pst_status {
    PST_STATUS_OFF = 0,
    PST_STATUS_NO_ENGINE,
    PST_STATUS_NO_STEMS,
    PST_STATUS_READY,
};

static void pst_refresh(void);

/* PRE-STEMS and Server Stems are mutually exclusive, so both can occupy order
 * zero without colliding. Mode 7 is unique and follows Server Stems (5) and
 * X-PAD (6). */
#define PRESTEM_BAND_MODE 7

static void pst_band_closed(void)
{
    if (!g_menu_open)
        return;
    g_menu_open = 0;
    pst_refresh();
}

static void pst_reslot(int32_t x)
{
    if (g_head)
        ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)((void *)g_head, x, KIT_BAND_SLOT_Y,
                                                              KIT_BAND_SLOT_W, KIT_BAND_SLOT_H);
}

KIT_BAND(k_band_prestem, .name = "prestem", .mode = PRESTEM_BAND_MODE, .closed = pst_band_closed,
         .shown = &g_prestems_on, .reslot = pst_reslot, .order = 0);

static void pst_colour(void *g, uint32_t argb)
{
    mod_draw_enter();
    mod_gfx_colour(g, argb);
    mod_draw_leave();
}

/* The recovered renderer alternates 4px cells. */
static void pst_checker(void *g, int x, int y, int w, int h, uint32_t a, uint32_t b)
{
    int yy, xx;

    pst_colour(g, a);
    mod_gfx_fill(g, x, y, w, h);
    pst_colour(g, b);
    for (yy = 0; yy < h; yy += 4)
        for (xx = ((yy >> 2) & 1) ? 0 : 4; xx < w; xx += 8)
            mod_gfx_fill(g, x + xx, y + yy, (xx + 4 <= w) ? 4 : w - xx, (yy + 4 <= h) ? 4 : h - yy);
}

static int pst_snapshot(struct prestem_ui_snapshot *snap)
{
    return g_prestems_on && !g_stems_on && prestem_ui_snapshot(snap);
}

static int pst_available(const struct prestem_ui_snapshot *snap)
{
    return snap && (snap->flags & PRESTEM_UI_READY);
}

static void pst_head_paint(void *self, void *g)
{
    struct prestem_ui_snapshot snap;
    const struct theme_ui *ui = mod_ui();
    if (!g)
        return;
    /* Show the selected stem mix even with the panel closed. Both parts on
     * is normal playback; a queued loading choice uses the same indication. */
    int active = pst_snapshot(&snap) && prestem_ui_selectable(snap.flags) &&
                 (snap.state & 3U) != 3U;
    pst_checker(g, 0, 0, PST_HEAD_W, PST_HEAD_H, active ? ui->accent2 : ui->surface2,
                active ? ui->accent : ui->surface);
    /* The label stores the already-resolved theme text colour. Keep the stock
     * theme transform from applying a second time while it paints that value. */
    mod_draw_enter();
    ((void (*)(void *, void *))LABEL_FN_PAINT)(self, g);
    mod_draw_leave();
    pst_colour(g, active ? ui->accent : ui->edge);
    mod_gfx_fill(g, (PST_HEAD_W - 56) / 2, PST_HEAD_H - 3, 56, 3);
}

/* Render words with the same stock Label path as the working title control.
 * Each complete toggle is a Label, so clicks never depend on child-to-parent
 * mouse coordinate conversion. */
static void pst_part_paint(void *self, void *g)
{
    struct prestem_ui_snapshot snap;
    const struct theme_ui *ui = mod_ui();
    unsigned bit = (uintptr_t)self == g_parts[0] ? 1U : 2U;
    unsigned state = pst_snapshot(&snap) ? snap.state : 3U;
    if (!g)
        return;
    pst_checker(g, 0, 0, PST_VOC_W, g_panel_h, state & bit ? ui->accent2 : ui->surface2,
                state & bit ? ui->accent : ui->surface);
    mod_draw_enter();
    ((void (*)(void *, void *))LABEL_FN_PAINT)(self, g);
    mod_draw_leave();
}
static void pst_part_down(void *self, void *event)
{
    struct prestem_ui_snapshot snap;
    unsigned bit = (uintptr_t)self == g_parts[0] ? 1U : 2U;
    (void)event;
    if (!pst_snapshot(&snap) || !prestem_ui_selectable(snap.flags))
        return;
    if (prestem_ui_request_state(snap.token, (snap.state & 3U) ^ bit))
        pst_refresh();
}
static void pst_panel_paint(void *self, void *g)
{
    (void)self;
    /* The container is only a parent for the two controls. Leaving its paint
     * empty lets the deck's own waveform/background show through without an
     * opaque strip whose shade can drift from the active theme. */
    (void)g;
}

static void pst_head_down(void *self, void *event)
{
    (void)self;
    (void)event;
    struct prestem_ui_snapshot snap;
    if (!pst_snapshot(&snap) || !prestem_ui_selectable(snap.flags))
        return;
    if (!g_menu_open) {
        if (kit_band_take(&k_band_prestem) < 0) {
            MDBG("prestem_ui: no waveform band to open into\n");
            return;
        }
        g_menu_open = 1;
    } else {
        g_menu_open = 0;
        kit_band_give(&k_band_prestem);
    }
    pst_refresh();
}

static void pst_refresh(void)
{
    struct prestem_ui_snapshot snap;
    int master = g_prestems_on && !g_stems_on;
    int have_snapshot = 0;
    int status = PST_STATUS_OFF;
    unsigned int state = ~0U, flags = ~0U;
    unsigned int theme_gen = mod_ui_gen();
    const struct theme_ui *ui = mod_ui();

    if (!master && g_menu_open) {
        g_menu_open = 0;
        kit_band_give(&k_band_prestem);
    }
    if (g_head)
        juce_comp_set_visible(g_head, master);
    if (master && prestem_ui_snapshot(&snap)) {
        have_snapshot = 1;
        state = snap.state;
        flags = snap.flags;
    }
    int selectable = master && have_snapshot && prestem_ui_selectable(flags);
    if (!selectable && g_menu_open) {
        g_menu_open = 0;
        kit_band_give(&k_band_prestem);
    }
    if (g_panel)
        juce_comp_set_visible(g_panel, master && g_menu_open);
    if (master) {
        if (!have_snapshot)
            status = PST_STATUS_NO_ENGINE;
        else if (!pst_available(&snap))
            status = PST_STATUS_NO_STEMS;
        else
            status = PST_STATUS_READY;
    }
    char detail[32];
    prestem_status(detail, sizeof(detail));
    if (g_status && (status != g_last_status || theme_gen != g_last_theme_gen ||
                     strcmp(detail, g_last_detail))) {
        snprintf(g_last_detail, sizeof(g_last_detail), "%s", detail);
        const char *text = "";
        uint32_t colour = mod_ui_stock()->text_dim;

        if (status == PST_STATUS_NO_ENGINE)
            text = "NO ENGINE";
        else if (status == PST_STATUS_NO_STEMS)
            text = detail;
        else if (status == PST_STATUS_READY) {
            text = "READY";
            colour = mod_ui_stock()->text;
        }
        juce_label_text(g_status, text);
        juce_comp_colour(g_status, LBL_COL_TEXT, colour);
        MINFO("prestem_ui: status=%s state=%u flags=%#x%s\n", text[0] ? text : "OFF", state, flags,
              status == PST_STATUS_READY ? " (validated sidecar pair)" : "");
    }
    if (master != g_last_master || state != g_last_state || flags != g_last_flags ||
        theme_gen != g_last_theme_gen) {
        g_last_master = master;
        g_last_state = state;
        g_last_flags = flags;
        if (g_head) {
            juce_comp_colour(g_head, LBL_COL_TEXT,
                             selectable ? ui->text_deck : ui->text_dim);
            juce_comp_repaint(g_head);
        }
        if (g_panel && g_menu_open)
            juce_comp_repaint(g_panel);
        for (int i = 0; i < 2; i++)
            if (g_parts[i]) {
                /* Sam's controls use one consistent white label treatment. The
                 * checkerboard and accent carry the state; the words themselves
                 * must not flip between black and white when one part changes. */
                uint32_t part_text = selectable ? mod_ui_stock()->text
                                                : mod_ui_stock()->text_dim;
                juce_comp_colour(g_parts[i], LBL_COL_TEXT,
                                 part_text);
                juce_comp_repaint(g_parts[i]);
            }
    }
    g_last_status = status;
    g_last_theme_gen = theme_gen;
    prestem_ui_observed(g_head && g_panel && g_status && g_parts[0] && g_parts[1],
                        master && g_menu_open);
}

static void pst_build(uintptr_t anchor)
{
    static const struct juce_vt_override hov[] = {
        {JUCE_VT_PAINT, (void *)pst_head_paint, NULL},
        {JUCE_VT_MOUSEDOWN, (void *)pst_head_down, NULL},
    };
    static const struct juce_vt_override pov[] = {
        {JUCE_VT_PAINT, (void *)pst_panel_paint, NULL},
    };
    static const struct juce_vt_override partov[] = {
        {JUCE_VT_PAINT, (void *)pst_part_paint, NULL},
        {JUCE_VT_MOUSEDOWN, (void *)pst_part_down, NULL},
    };
    const int32_t *pb;
    uintptr_t bar, view;
    int head_x;

    /* This is the same attachment route used by the already-working Server
     * Stems UI: TouchAria -> 1280x90 title bar, and the stock quick-menu panel
     * rect inside the waveform view. */
    if (!anchor || g_building || !g_api_ok || kit_band_attach(anchor) != 0)
        return;
    bar = kit_band_bar();
    view = kit_band_view();
    pb = kit_band_rect();
    if (!bar || !view || !pb || pb[2] < 1000 || pb[3] < 40)
        return;

    if (!g_head_vptr)
        g_head_vptr = juce_label_vt_clone(g_head_vt, hov, (int)(sizeof(hov) / sizeof(hov[0])));
    if (!g_panel_vptr)
        g_panel_vptr = juce_label_vt_clone(g_panel_vt, pov, (int)(sizeof(pov) / sizeof(pov[0])));
    if (!g_part_vptr)
        g_part_vptr = juce_label_vt_clone(g_part_vt, partov, 2);
    if (!g_head_vptr || !g_panel_vptr || !g_part_vptr)
        return;

    head_x = kit_band_slot_x(&k_band_prestem);
    g_building = 1;
    if (!g_head)
        g_head = juce_label(bar, "STEMS", 22.0f, 0, mod_ui()->text_deck, g_head_vptr, head_x,
                            KIT_BAND_SLOT_Y, PST_HEAD_W, PST_HEAD_H);
    if (!g_panel)
        g_panel = juce_label(view, "", 1.0f, 0, 0, g_panel_vptr, pb[0], pb[1], pb[2], pb[3]);
    if (!g_status)
        g_status = g_head ? juce_label(g_head, "CHECKING", 12.0f, 0x00000000u,
                                       mod_ui_stock()->text_dim, 0,
                                       0, 57, PST_HEAD_W, 18)
                          : 0;
    if (g_panel) {
        if (!g_parts[0])
            g_parts[0] = juce_label(g_panel, "VOCAL", 24.0f, 0, mod_ui_stock()->text, g_part_vptr,
                                    PST_VOC_X, 0, PST_VOC_W, pb[3]);
        if (!g_parts[1])
            g_parts[1] = juce_label(g_panel, "INSTRUMENTAL", 24.0f, 0, mod_ui_stock()->text, g_part_vptr,
                                    PST_INS_X, 0, PST_INS_W, pb[3]);
    }
    g_building = 0;
    if (!g_head || !g_panel || !g_status || !g_parts[0] || !g_parts[1]) {
        if (g_panel)
            juce_comp_set_visible(g_panel, 0);
        return;
    }
    g_panel_h = pb[3];
    g_menu_open = 0;
    juce_comp_set_visible(g_panel, 0);
    kit_band_own(g_head);
    kit_band_own(g_panel);
    pst_refresh();
    MINFO("prestem_ui: title slot={%d,%d,%d,%d} waveform band={%d,%d,%d,%d}"
          " bar=%#lx view=%#lx\n",
          head_x, KIT_BAND_SLOT_Y, PST_HEAD_W, PST_HEAD_H, pb[0], pb[1], pb[2], pb[3],
          (unsigned long)bar, (unsigned long)view);
}

/* Share Server Stems' already-installed anchor and clock. A second native
 * paint hook is no longer a prerequisite for making the controls visible. */
void prestem_ui_attach(uintptr_t anchor)
{
    if (g_building)
        return;
    if (!g_head || !g_panel || !g_status || !g_parts[0] || !g_parts[1])
        pst_build(anchor);
    if (g_head)
        pst_refresh();
}
void prestem_ui_refresh(void)
{
    if (!g_building && g_head)
        pst_refresh();
}
static int prestem_ui_install(void)
{
    g_api_ok = FN_LABEL_CTOR && FN_LABEL_SETFONT && FN_LABEL_JUSTIFY && FN_VALUE_SETVALUE &&
               FN_STR_DEFCTOR && FN_STR_CTOR && FN_FONT_BUILD && FN_ADD_VISIBLE && FN_SET_BOUNDS &&
               LABEL_FN_PAINT && MOD_FN_GFX_SETCOLOUR && MOD_FN_GFX_FILLRECT;
    if (!g_api_ok) {
        MWARN("prestem_ui: JUCE primitives unavailable\n");
        return -1;
    }
    MINFO("prestem_ui: registered on shared play-screen lifecycle\n");
    return 0;
}
KIT_MOD(k_mod_prestem_ui, .name = "prestem_ui", .prio = 32, .install = prestem_ui_install,
        .what = "PRE-STEMS source controls on shared title/band layout");
