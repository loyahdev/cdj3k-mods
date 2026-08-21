// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/rows.c - list positions to registered rows, and the accessors over them.
 *
 * The rows belong to the features that declared them (../kit/menu.h). What is
 * here is the mapping from a JUCE row index to one of them, and the answers the
 * centre value column and the right pane need: how many values a row has, which
 * is in force, and what each is called.
 */
#include "menu/internal.h"

/* Map a list row to its definition. NULL for the title row and for the blank
 * filler below the last live setting. Rows are addressed by POSITION -- a row's
 * index shifts as the gates above it open and close. */
const struct kit_row *menu_row(int row)
{
    return (row < MOD_ROW_FIRST) ? NULL : kit_menu_row(row - MOD_ROW_FIRST);
}

/* The last selectable list row. Live rows are contiguous from MOD_ROW_FIRST. */
int menu_row_last(void)
{
    int n = kit_menu_count();

    return n > 0 ? MOD_ROW_FIRST + n - 1 : MOD_ROW_FIRST;
}

const char *menu_row_label(int row)
{
    const struct kit_row *r = menu_row(row);

    return r ? r->label : NULL;
}

/* How many values a row offers. A TEXT row has none; the pane it gets is the
 * borrowed set's own length (see menu_pane_rows), showing empty strings. */
int menu_row_nvalues(const struct kit_row *r)
{
    return r ? (int)r->nvalues : 0;
}

const char *menu_row_value(const struct kit_row *r, int idx)
{
    if (!r) return "";
    if (r->text) return r->text[0] ? r->text : "NOT SET";
    if (!r->values || r->nvalues == 0) return "";
    if (idx < 0 || idx >= (int)r->nvalues) idx = 0;
    return r->values[idx];
}

/* The value in force, as the row's own index -- may be anything the row defines.
 * For DISPLAY only: see menu_pane_index for what the model is allowed to see. */
int menu_row_index(const struct kit_row *r)
{
    int n, v;

    if (!r || !r->state) return 0;
    n = menu_row_nvalues(r);
    v = *r->state;
    return (v < 0 || v >= n) ? 0 : v;
}

/* How many rows the pane has WHILE THIS ROW OWNS IT.
 *
 * Ours by construction rather than by poking: menu_rnumrows answers getNumRows
 * with exactly this, and menu_pane_labels puts an array of exactly this length
 * behind it. The two cannot disagree, which is the whole reason the count is a
 * hook now and not a write -- a written field outlives the row that wanted it,
 * and that is what took the deck down the first time. */
int menu_pane_rows(const struct kit_row *r)
{
    int n = menu_row_nvalues(r);

    if (n < MOD_PANE_ROWS_STOCK) n = MOD_PANE_ROWS_STOCK;
    return n > MOD_ROW_VALUES_MAX ? MOD_ROW_VALUES_MAX : n;
}

/* The value in force, clamped to the rows the pane actually has. Every index
 * that reaches the model goes through here: `state` comes off the eMMC, and an
 * index the array cannot answer for is a read off its end. */
int menu_pane_index(const struct kit_row *r)
{
    int v = menu_row_index(r), n = menu_pane_rows(r);

    if (v < 0) return 0;
    return v >= n ? n - 1 : v;
}

/* State of the row being edited; the pane is only ever built for a real settings row.
 * A text row has no state, so it reports 0 and never drives the radio. */
int menu_active_on(void)
{
    return menu_pane_index(menu_row(menu_g_setting_row));
}
