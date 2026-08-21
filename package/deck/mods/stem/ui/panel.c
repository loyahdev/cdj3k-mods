// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/panel.c - the row and the waveform band it borrows
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* ---- the band ------------------------------------------------------------
 *
 * kit/band.c owns the strip and arbitrates who has it; this is only the STEMS
 * end of that contract. The mode number is ours alone and must stay unique
 * across clients -- it is what the kit's watch reads back to tell "still ours"
 * from "taken". */
#define STEMS_BAND_MODE 5

static void stems_band_closed(void)
{
    if (!stems_g_row_open) return;
    MDBG("stems: the band went elsewhere -> closing our row\n");
    stems_g_row_open = 0;
    stems_sync();
}

static void stems_reslot(int32_t x)
{
    if (stems_g_btn_stems)
        ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
            ((void *)stems_g_btn_stems, x, KIT_BAND_SLOT_Y,
             KIT_BAND_SLOT_W, KIT_BAND_SLOT_H);
}

KIT_BAND(k_band_stems,
         .name = "stems", .mode = STEMS_BAND_MODE, .closed = stems_band_closed,
         .shown = &g_stems_on, .reslot = stems_reslot, .order = 0);

int32_t stems_slot_x(void)
{
    return kit_band_slot_x(&k_band_stems);
}


/* ================================================================== */
/* Interaction                                                        */
/* ================================================================== */

/* ENABLE STEMS in MOD SETTINGS is the master gate and can be flipped while the play
 * screen is up, so visibility is re-asserted from the paint hook rather than only at
 * build time. Paint runs per frame, hence the memo: juce's own setters already
 * no-op on an unchanged value, but this keeps the common case to one comparison. */
void stems_sync(void)
{
    static int last = -1;
    int state = (g_stems_on ? 1 : 0) | (stems_g_row_open ? 2 : 0);
    int on, lit, gate_moved;

    if (state == last) return;
    gate_moved = (last >= 0) && ((last & 1) != (state & 1));
    last = state;
    on  = (state & 1) != 0;
    lit = on && (state & 2) != 0;

    /* The gate moving changes which slots are in use, so the other client's
     * button and the track title both move with it. */
    if (gate_moved) kit_band_slots_changed();
    stems_set_visible(stems_g_btn_stems, on);
    stems_set_visible(stems_g_row, lit);
    /* Closing the row ends any gesture outright, cooldown included: the next thing
     * the DJ does after reopening is a fresh intent, not the tail of an old sweep.
     *
     * The STEMS button is exempt, because it is not IN the row -- it is what closed it,
     * and the finger is still down on it. Clearing its grab here dropped the touch tier
     * mid-press, so a press that CLOSED the panel stayed flat grey while one that opened
     * it lit up. Its own mouseUp releases the grab, which is where that belongs. */
    if (!lit && stems_g_grab != stems_g_btn_stems) { stems_g_grab = 0; stems_g_grab_last = 0; }
    stems_btn_state(lit ? BTN_ON : BTN_OFF);

    /* The master gate is the one route that changes whether the row is up without
     * touching stems_g_row_open -- every other one moves that flag and claims or releases
     * the band on its way. Hiding the strip on its own would leave the waveform
     * compacted around nothing.
     *
     * A BACKSTOP, not the main path. Switching STEMS off also drops the resident
     * set, which makes stems_available() false, and the warn branch further down
     * then closes the row properly and hands the band back -- that is what is
     * observed doing it, a second later, because that branch waits out
     * WARN_SETTLE_TICKS first. This covers the second in between, for a DJ who
     * leaves the settings screen faster than that, and the case where there was
     * no set resident to drop.
     *
     * Gated on the gate having MOVED rather than on `lit`, which is what tells it
     * apart from an ordinary open or close -- and from the stock-quick-menu
     * takeover, where releasing here would shut the panel that is about to open. */
    if (gate_moved && stems_g_row_open) {
        if (on) kit_band_take(&k_band_stems);
        else    kit_band_give(&k_band_stems);
    }
}

/* One-shot component-tree dump, debug builds only. This is how the stock quick-menu
 * panel's rect and the waveform's rect were found: open a stock panel, then open ours,
 * and read the two off the log. Bounded on depth and on child count so a corrupt or
 * unexpected pointer cannot turn into an unbounded walk. */
#define STEMS_TREE_DEPTH   4
#define STEMS_TREE_MAXKIDS 64

void stems_dump_tree(uintptr_t comp, int depth)
{
    uintptr_t vt = 0, kids = 0, child;
    int32_t n = 0, b[4], i;
    uint8_t flags = 0;

    if (depth > STEMS_TREE_DEPTH || !comp) return;
    if (mod_safe_read(comp, &vt, sizeof(vt)) != 0) return;
    if (stems_bounds(comp, b) != 0) return;
    if (mod_safe_read(comp + COMP_NCHILD_OFF, &n, sizeof(n)) != 0) return;
    mod_safe_read(comp + COMP_FLAGS_OFF, &flags, 1);
    MDBG("tree %*s%#lx vt=%#lx {%d,%d,%d,%d} flags=%02x kids=%d\n",
         depth * 2, "", (unsigned long)comp, (unsigned long)vt,
         b[0], b[1], b[2], b[3], flags, n);
    if (n <= 0 || n > STEMS_TREE_MAXKIDS) return;
    if (mod_safe_read(comp + COMP_CHILDREN_OFF, &kids, sizeof(kids)) != 0 || !kids) return;
    for (i = 0; i < n; i++) {
        child = 0;
        if (mod_safe_read(kids + (uintptr_t)i * sizeof(uintptr_t), &child, sizeof(child)) == 0)
            stems_dump_tree(child, depth + 1);
    }
}

/* The flag itself, for the parts of the feature that are not the UI. The groove
 * circuit is gated on it and its lamps follow, so this is read from [deck] and
 * from the display tick as well as from here. A plain aligned int, written only
 * by the toggle below. */
int stems_row_open(void)
{
    return stems_g_row_open;
}

void stems_toggle_row(void)
{
    if (!stems_g_row_open) {
        /* THE BAND FIRST, AND ONLY OPEN IF IT COMES -- see kit_band_take. With no
         * track the app refuses the mode, and the row went up over the rekordbox
         * logo with its wedges grey and the poll flapping it. */
        if (kit_band_take(&k_band_stems) < 0) {
            MDBG("stems: no band to open into (no track loaded?)\n");
            return;
        }
        stems_g_row_open = 1;
    } else {
        stems_g_row_open = 0;
        kit_band_give(&k_band_stems);
    }
    stems_sync();
    /* Pick up any in-flight processing the moment the row appears, rather than waiting
     * for the paint tick -- the title bar repaints rarely, so that tick is not dependable
     * on its own. */
    if (stems_g_row_open) {
        /* Nothing cached survives the row being shut: the poll stops with it while the
         * job carries on, so whatever is on the bar describes a state that is gone. */
        stems_progress_forget();
        stems_progress_poll();
    }
    MDBG("stems: control row %s\n", stems_g_row_open ? "open" : "closed");
    if (MLOG_AT(MOD_LOG_DEBUG) && stems_g_row_open && kit_band_view()) {
        MDBG("tree --- OURS open ---\n");
        stems_dump_tree(kit_band_view(), 0);
    }
}
