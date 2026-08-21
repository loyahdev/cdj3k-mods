// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * browse/drag.c - carrying a row to a new place in the list.
 *
 * The gesture half of the reorder. See browse.h for the mode it lives inside
 * and for the offsets this reads.
 *
 * ---- what makes the row fly ------------------------------------------------
 *
 * Its own bounds. juce sets a child's Graphics origin from where the child IS,
 * so moving the RowComp moves everything it draws -- the number, the title, the
 * key, the waveform strip -- with no snapshot to take and no transform to apply
 * to somebody else's paint. The translucency is then one fill over the top of
 * the stock paint, in the row's own paint slot.
 *
 * The finger's position arrives relative to the row, and the row is the thing
 * being moved, so every measurement is taken in the PARENT's space:
 * `current bounds y + event y` is the finger, whatever the row is doing.
 *
 * ---- not chaining is the feature -------------------------------------------
 *
 * The stock handlers are what scroll the list and move the selection. While a
 * drag is live they are simply not called, which is the whole of "the list does
 * not scroll under the gesture" -- there is no flag to set on the viewport and
 * nothing to put back afterwards.
 *
 * ---- and it fails open -----------------------------------------------------
 *
 * Every hook returns to stock on its first line unless a drag is live, and a
 * drag only starts on a row of the list EDIT is on. RowComp is shared by every
 * touchable table in the app, so anything less than that would put this in
 * front of DJ SETTINGS' scrolling too.
 */
#include "browse/browse.h"

static uintptr_t dg_g_row;          /* the RowComp under the finger  */
static uintptr_t dg_g_wrap;         /* its Row wrapper: what actually moves */
static uintptr_t dg_g_owner;        /* the list it came from         */
static int32_t   dg_g_orig[4];      /* where the WRAPPER sat before the drag */
static int       dg_g_from;         /* its row number, 0-based       */
static int       dg_g_insert;       /* where it would land, 0-based  */
static int       dg_g_press;        /* the finger's y at mouseDown, in the parent */
static int       dg_g_rows;         /* how many rows the model has   */
static int       dg_g_marked;       /* the mark has been drawn at least once */
static int       dg_g_settle;       /* ticks a release must survive to count */
static int       dg_g_lifted;       /* the row is off its slot, following a finger */
static uintptr_t dg_g_row_vptr;     /* what the two carried objects were, so a */
static uintptr_t dg_g_wrap_vptr;    /* deleted one is not written back into    */
static uintptr_t dg_g_view;         /* the scrolled content: the rows' parent  */
static uintptr_t dg_g_clip;         /* what shows a window onto it             */
static int       dg_g_screen;       /* the finger, in that window's space      */
static uintptr_t dg_g_stock_content_paint;
static uintptr_t dg_g_stock_down, dg_g_stock_drag, dg_g_stock_up;
static uintptr_t dg_g_stock_paint;
static uintptr_t dg_g_stock_wrap_paint, dg_g_stock_wrap_over;

/* ---- reading the row ----------------------------------------------------- */

static int dg_row_no(uintptr_t rc)
{
    int32_t v = -1;

    return mod_safe_read(rc + RC_ROW_OFF, &v, sizeof(v)) == 0 ? (int)v : -1;
}

#define DG_TI_WRAP  juce_class_of(ep122_sym(EP122_LISTBOX_ROW))

static uintptr_t dg_owner(uintptr_t rc)
{
    uintptr_t v = 0;

    mod_safe_read(rc + RC_OWNER_OFF, &v, sizeof(v));
    return v;
}

/* The model's own row count, through the same slot the deck reads it from.
 * Needed because the insertion point may sit past the last row, and "past the
 * last row" is a different answer from "on it". */
static int dg_num_rows(uintptr_t owner)
{
    uintptr_t model = 0, vt = 0, fn = 0;

    if (mod_safe_read(owner + LB_MODEL_OFF, &model, sizeof(model)) != 0 || !model)
        return 0;
    if (mod_safe_read(model, &vt, sizeof(vt)) != 0 || !vt)
        return 0;
    if (mod_safe_read(vt + MODEL_NUMROWS, &fn, sizeof(fn)) != 0 || !fn)
        return 0;
    return (int)((int64_t (*)(void *))fn)((void *)model);
}

static int dg_event_y(void *event)
{
    float y = 0.0f;

    if (mod_safe_read((uintptr_t)event + ME_Y_OFF, &y, sizeof(y)) != 0)
        return 0;
    return (int)y;
}

/* The WRAPPER, which is what carries a row's position. */
static void dg_place(int y)
{
    uintptr_t fn = ep122_sym(EP122_JUCE_COMP_SETBOUNDS);

    if (fn && dg_g_wrap)
        ((void (*)(void *, int, int, int, int))fn)((void *)dg_g_wrap,
                                                   dg_g_orig[0], y,
                                                   dg_g_orig[2], dg_g_orig[3]);
}

/* ---- carrying a row past the edge of the window ---------------------------
 *
 * A list longer than the window is the ordinary case, and without this the only
 * rows a track can be dropped on are the ten already showing.
 *
 * THIS LIST DOES NOT SCROLL THE JUCE WAY. meow::TouchableViewport is not a
 * juce::Viewport -- it is a juce::Component that happens to be a
 * ComponentListener and a MultiTimer -- so juce::Viewport::setViewPosition is
 * the wrong lever and does not reach it. What carries the offset is the CONTENT
 * COMPONENT'S OWN Y, measured on an 18-row playlist scrolled to row 6:
 *
 *   Row (wrapper)     {0,240,1188,48}      row 5 * 48, so wrapper y is content
 *   ViewedComponent   {0,-240,1188,864}    18 * 48 tall, offset by five rows
 *   juce::Component   {0,0,1188,480}       the window, ten rows
 *
 * Which is also why the whole gesture measures in TRAVEL: content coordinates do
 * not move when the list scrolls, so the row number the finger is over survives
 * a scroll without any correction.
 *
 * Moving the content is what the viewport itself does, and it listens to its own
 * content -- that is what the ComponentListener base is for -- so the rows get
 * recycled onto their new positions by the deck's own code. */
#define DG_EDGE_PX   44     /* how near an edge starts it: about one row */
#define DG_SCROLL_PX  9     /* per display tick, so ~7 rows a second */

/* Scroll `px` further down the list, or up when negative. Returns how far the
 * content actually moved, which is 0 at either end. */
static int dg_scroll_by(int px)
{
    uintptr_t fn = ep122_sym(EP122_JUCE_COMP_SETBOUNDS);
    int32_t v[4], c[4];
    int want, least;

    if (!fn || !dg_g_view || !dg_g_clip ||
        juce_comp_bounds(dg_g_view, v) != 0 ||
        juce_comp_bounds(dg_g_clip, c) != 0)
        return 0;
    /* Down the list means the content moves UP, so its y goes negative -- as far
     * as the last window's worth, and not at all when it all fits. */
    least = c[3] - v[3];
    if (least > 0)
        least = 0;
    want = v[1] - px;
    if (want > 0)
        want = 0;
    if (want < least)
        want = least;
    if (want == v[1])
        return 0;
    ((void (*)(void *, int, int, int, int))fn)((void *)dg_g_view,
                                               v[0], want, v[2], v[3]);
    return want - v[1];
}

/* PUT THE CARRIED ROW'S OWN TRACK BACK ON IT.
 *
 * The list hands its pool out as `components[row % N]`, so a scroll of two rows
 * is enough to give the carried one to whatever has just come into view --
 * measured: the component carrying row 0 was re-drawn as row 12 of an 18-row
 * list. The `#` column follows the RowComp's own row number, but the title does
 * not: it lives in a child that refreshComponentForRow fills in, so the thing
 * under the finger silently became a different track.
 *
 * So re-assert the row through the deck's own refresh, with the same three
 * arguments its updateVisibleArea uses. Called straight after a scroll step,
 * which is when -- and only when -- the list re-assigns: the content's move
 * notifies the viewport synchronously, so by the time the scroll returns the
 * damage is done and undoing it here lands before anything paints. */
static void dg_reclaim(void)
{
    uintptr_t owner = 0, model = 0, vt = 0, fn = 0, child = 0, back;
    int32_t row = -1;
    uint8_t sel = 0;

    if (!dg_g_wrap || dg_g_from < 0)
        return;
    if (mod_safe_read(dg_g_wrap + ROW_INDEX_OFF, &row, sizeof(row)) != 0 ||
        row == dg_g_from)
        return;
    if (mod_safe_read(dg_g_wrap + ROW_OWNER_OFF, &owner, sizeof(owner)) != 0 ||
        !owner ||
        mod_safe_read(owner + LB_ROW_MODEL, &model, sizeof(model)) != 0 ||
        !model ||
        mod_safe_read(model, &vt, sizeof(vt)) != 0 || !vt ||
        mod_safe_read(vt + MODEL_REFRESH, &fn, sizeof(fn)) != 0 || !fn ||
        mod_safe_read(dg_g_wrap + ROW_CHILD_OFF, &child, sizeof(child)) != 0)
        return;

    row = dg_g_from;
    mod_safe_write(dg_g_wrap + ROW_INDEX_OFF, &row, sizeof(row));
    mod_safe_write(dg_g_wrap + ROW_SEL_OFF, &sel, sizeof(sel));
    back = ((uintptr_t (*)(void *, int, int, void *))fn)((void *)model,
                                                         dg_g_from, 0,
                                                         (void *)child);
    /* It fills the component it is given and hands the same one back, which is
     * the only case worth taking: a NEW component would have to be adopted and
     * bounded the way the list does it, and the list is about to do that itself
     * on the next scroll step anyway. */
    if (back && back != child)
        MDBG("browse: the row refresh returned a different component -- the "
             "carried row keeps the one it had\n");
}

/* The whole LIST, not the row and not the row's parent. What has to be redrawn
 * is the space the row left, the row wearing the insertion mark, and the row
 * itself -- and the first version repainted the immediate parent, on the
 * assumption that every row shares one. The insertion mark never appeared once,
 * which is what that assumption looks like when it is wrong. */
static void dg_repaint(void)
{
    uintptr_t fn = ep122_sym(EP122_JUCE_COMP_REPAINT);

    if (fn && dg_g_owner)
        ((void (*)(void *))fn)((void *)dg_g_owner);
}

/* The row's ancestry, once per drag: which component actually holds it, how it
 * is positioned, and how far up the list is. Everything above turned on getting
 * this wrong, so it is worth a line. */
static void dg_chain(uintptr_t rc)
{
    char name[96];
    uintptr_t c = rc;
    int i;

    for (i = 0; i < 5 && c; i++) {
        int32_t b[4] = { 0, 0, 0, 0 };

        juce_comp_bounds(c, b);
        MDBG("browse:   %s%p %s {%d,%d,%d,%d} children=%d\n",
             i ? "  ^ " : "row ", (void *)c,
             juce_comp_class_name(c, name, sizeof(name)),
             (int)b[0], (int)b[1], (int)b[2], (int)b[3], juce_comp_nchild(c));
        c = juce_comp_parent(c);
    }
}

/* Last in the parent's child array, which IS juce's z-order -- a component is
 * painted in array order, so a row moved over a sibling that comes after it is
 * simply painted under it. Dragging up looked fine and dragging down lost the
 * row entirely, which is that asymmetry exactly.
 *
 * Done by moving the pointer rather than by calling toFront: the array is what
 * toFront edits, we are on the message thread that owns it, and re-adding the
 * child instead would take it out of its parent mid-gesture -- with the mouse
 * grab on it. Nothing puts the order back afterwards, and nothing needs to: the
 * rows do not overlap when they are where the list put them. */
static void dg_to_front(uintptr_t rc)
{
    uintptr_t p = juce_comp_parent(rc), arr = 0, cur = 0;
    int32_t n = 0;
    int i, at = -1;

    if (!p || mod_safe_read(p + JUCE_CHILDREN_OFF, &arr, sizeof(arr)) != 0 || !arr)
        return;
    if (mod_safe_read(p + JUCE_NCHILD_OFF, &n, sizeof(n)) != 0 || n <= 1 || n > 256)
        return;
    for (i = 0; i < n; i++)
        if (mod_safe_read(arr + (size_t)i * sizeof(cur), &cur, sizeof(cur)) == 0 &&
            cur == rc) {
            at = i;
            break;
        }
    if (at < 0 || at == n - 1)
        return;
    for (i = at; i < n - 1; i++) {
        if (mod_safe_read(arr + (size_t)(i + 1) * sizeof(cur), &cur, sizeof(cur)) != 0)
            return;
        mod_safe_write(arr + (size_t)i * sizeof(cur), &cur, sizeof(cur));
    }
    mod_safe_write(arr + (size_t)(n - 1) * sizeof(rc), &rc, sizeof(rc));
}

/* ---- the gesture --------------------------------------------------------- */

/* ARE THE TWO CARRIED OBJECTS STILL THE ONES WE GRABBED?
 *
 * The list owns them and can rebuild itself under a drag -- a media eject, a
 * view change, the deck recycling a RowComp. Their vtable pointer is what says
 * so, the same one-read test the EDIT plate uses on itself, and it is the guard
 * that matters: putting a row back means WRITING to it, and writing to a freed
 * component is the one way this feature could take the deck down. */
static int dg_alive(void)
{
    uintptr_t v = 0;

    return dg_g_row && dg_g_wrap &&
           mod_safe_read(dg_g_row, &v, sizeof(v)) == 0 && v == dg_g_row_vptr &&
           mod_safe_read(dg_g_wrap, &v, sizeof(v)) == 0 && v == dg_g_wrap_vptr;
}

static void dg_end(void)
{
    if (dg_alive()) {
        dg_place(dg_g_orig[1]);
        dg_repaint();
    }
    dg_g_row = dg_g_wrap = dg_g_owner = 0;
    dg_g_view = dg_g_clip = 0;
    dg_g_row_vptr = dg_g_wrap_vptr = 0;
    dg_g_from = dg_g_insert = -1;
}

/* Both defined with the drag they belong to, below; a press has to reach them
 * because a fast one arrives partly as presses. */
static int  dg_finger(uintptr_t rc, void *event);
static void dg_carry_to(int finger);

/* WHY a press did not become a drag. Four conditions could refuse one and all
 * four returned to stock without a word, so a press that did nothing looked
 * exactly like a hook that was never installed. Said once per distinct reason,
 * because a press repeats. */
static void dg_refuse(const char *why)
{
    static const char *last;

    if (why == last)
        return;
    last = why;
    MDBG("browse: a press on a row did not start a drag: %s\n", why);
}

/* Is this row inside the list EDIT is on? RowComp belongs to every touchable
 * table in the app, so without this the mode makes the browse sidebar and DJ
 * SETTINGS draggable as well -- and it is not a theoretical reach: a tap on the
 * sidebar's TRACK tab was swallowed by a drag on the tab itself. Bounded, like
 * every walk over the live tree; a row is five deep in the list that owns it. */
static int dg_in_edit_list(uintptr_t rc)
{
    uintptr_t list = browse_edit_list(), c = rc;
    int i;

    if (!list)
        return 0;
    for (i = 0; i < 12 && c; i++, c = juce_comp_parent(c))
        if (c == list)
            return 1;
    return 0;
}

static void dg_mousedown(void *self, void *event)
{
    uintptr_t rc = (uintptr_t)self, wrap;
    int32_t b[4], wb[4];

    if (!browse_edit_on()) {
        dg_refuse("EDIT is off");
        ((void (*)(void *, void *))dg_g_stock_down)(self, event);
        return;
    }
    /* A SECOND PRESS WITH NO RELEASE BETWEEN IS NOT A SECOND GESTURE.
     *
     * juce's contract is one mouseDown, then drags, then exactly one mouseUp --
     * so if a press arrives while a row is still carried, the finger never came
     * up and this is the same gesture still going. It happens on a FAST drag:
     * the deck's touch layer reads a big jump between samples as the finger
     * having been lifted and put down again, and the row then re-latched onto
     * whatever was under it, which is the "drag it fast and it goes unstable"
     * this feature was reported with. Treated as movement, it simply works.
     *
     * Not chained to stock, for the same reason the drag is not: this is the
     * middle of a gesture the list must not scroll or re-select under. */
    if (dg_g_row) {
        int finger = dg_alive() ? dg_finger(rc, event) : DG_NO_FINGER;

        if (finger != DG_NO_FINGER && dg_in_edit_list(rc)) {
            if (!dg_g_lifted) {
                MDBG("browse: the finger came back -- row %d is still the one "
                     "being carried\n", dg_g_from);
                dg_g_lifted = 1;
                dg_to_front(dg_g_wrap);
            }
            /* dg_g_press is deliberately untouched: the travel that decides
             * where the row lands is measured from the ORIGINAL press, so a
             * gesture that was interrupted resumes rather than restarts. */
            dg_g_settle = 0;
            dg_carry_to(finger);
            return;
        }
        MDBG("browse: a press somewhere else while row %d was carried -- "
             "putting it back\n", dg_g_from);
        dg_end();
    }
    if (!dg_in_edit_list(rc)) {
        dg_refuse("the row is not in the list EDIT is on");
        ((void (*)(void *, void *))dg_g_stock_down)(self, event);
        return;
    }
    if (juce_comp_bounds(rc, b) != 0 || b[3] <= 0 ||
        (dg_g_from = dg_row_no(rc)) < 0) {
        dg_refuse("the row has no bounds or no row number");
        ((void (*)(void *, void *))dg_g_stock_down)(self, event);
        return;
    }
    /* The wrapper, by class rather than by "the parent": if this row is ever
     * built into something else, a drag that moved whatever happened to be
     * above it would move the wrong thing. */
    wrap = juce_comp_parent(rc);
    if (!wrap || juce_comp_class(wrap) != DG_TI_WRAP ||
        juce_comp_bounds(wrap, wb) != 0) {
        dg_refuse("its parent is not a TouchableListBox::Row");
        ((void (*)(void *, void *))dg_g_stock_down)(self, event);
        return;
    }
    dg_g_owner = dg_owner(rc);
    dg_g_rows = dg_num_rows(dg_g_owner);
    if (dg_g_rows <= 1 || dg_g_from >= dg_g_rows) {
        /* One row cannot be reordered, and a row number the model does not
         * claim is a row this does not understand. Stock either way. */
        dg_refuse(dg_g_rows <= 1 ? "the model reports no rows"
                                 : "the model does not claim that row");
        dg_g_owner = 0;
        ((void (*)(void *, void *))dg_g_stock_down)(self, event);
        return;
    }

    dg_g_row = rc;
    dg_g_wrap = wrap;
    /* The rows' own parent is the scrolled content, and its parent is the window
     * that clips it. Both are read here rather than walked per tick: a drag that
     * cannot find them simply does not scroll. */
    dg_g_view = juce_comp_parent(wrap);
    dg_g_clip = dg_g_view ? juce_comp_parent(dg_g_view) : 0;
    dg_g_orig[0] = wb[0]; dg_g_orig[1] = wb[1];
    dg_g_orig[2] = wb[2]; dg_g_orig[3] = wb[3];
    dg_g_insert = dg_g_from;
    /* The finger in the LIST's space. The RowComp fills the wrapper at {0,0},
     * so its own event y needs only the wrapper's position added. */
    dg_g_press = wb[1] + dg_event_y(event);
    (void)mod_safe_read(rc, &dg_g_row_vptr, sizeof(dg_g_row_vptr));
    (void)mod_safe_read(wrap, &dg_g_wrap_vptr, sizeof(dg_g_wrap_vptr));
    dg_g_lifted = 1;
    dg_to_front(wrap);
    dg_repaint();
    dg_chain(rc);
    MDBG("browse: carrying row %d of %d, wrapper at {%d,%d,%d,%d}, finger %d\n",
         dg_g_from, dg_g_rows, wb[0], wb[1], wb[2], wb[3], dg_g_press);
}

/* Where the finger is, in the LIST's own space, from an event delivered to any
 * row. A RowComp fills its wrapper at {0,0}, so the wrapper's y plus the event's
 * y is the answer whichever row the event came to -- which matters, because a
 * fast drag does not always keep coming to the same one. */
static int dg_finger(uintptr_t rc, void *event)
{
    uintptr_t wrap = juce_comp_parent(rc);
    int32_t wb[4];

    if (!wrap || juce_comp_bounds(wrap, wb) != 0)
        return DG_NO_FINGER;
    return wb[1] + dg_event_y(event);
}

/* Carry the row to a finger position. Split out of mouseDrag because a fast drag
 * arrives partly as presses, and both have to move the same row the same way. */
static void dg_carry_to(int finger)
{
    int y, dy, insert, h = dg_g_orig[3];
    int32_t v[4];

    /* WHERE THE FINGER IS ON SCREEN, which the tick needs and cannot work out:
     * `finger` is a content coordinate and the tick has no event to read one
     * from. Kept here because this is the only place a fresh one arrives. */
    if (dg_g_view && juce_comp_bounds(dg_g_view, v) == 0)
        dg_g_screen = finger + v[1];

    dy = finger - dg_g_press;

    /* HOW FAR IT HAS TRAVELLED, not where it is. A row's own y is not
     * `index * height` -- row 1 of this list reports y=0 -- so an absolute
     * position says nothing about which row it is over, and reading it that way
     * put every drag a row short. Rounded to the nearest whole row, so the mark
     * moves once the row has travelled half of one, which is where the eye
     * expects it rather than when an edge crosses a boundary. */
    insert = dg_g_from + (dy >= 0 ? (dy + h / 2) / h : -((-dy + h / 2) / h));
    if (insert < 0)
        insert = 0;
    if (insert > dg_g_rows - 1)
        insert = dg_g_rows - 1;

    /* ON A SLOT, not one past the end of them. The content component is exactly
     * as tall as the list has rows, and juce clips a child to its parent -- so a
     * row carried past the last slot is drawn into the empty stripes below the
     * list, where it is cut off and then disappears entirely. It also stops a
     * flick from parking the wrapper somewhere the deck will not put it back.
     *
     * Bounded by TRAVEL, the same quantity the insertion index is bounded by,
     * because a row's own y is not `index * height`. The two now agree: every
     * position the row can be carried to is one the mark can name. */
    if (dy < -dg_g_from * h)
        dy = -dg_g_from * h;
    if (dy > (dg_g_rows - 1 - dg_g_from) * h)
        dy = (dg_g_rows - 1 - dg_g_from) * h;
    y = dg_g_orig[1] + dy;

    dg_place(y);
    if (insert != dg_g_insert)
        MDBG("browse: over row %d (moved %d of %d px rows)\n", insert, dy, h);
    dg_g_insert = insert;
    dg_repaint();
}

/* NOT "is this the row we grabbed". After a synthesised release-and-press the
 * gesture continues on whatever row the finger is now over, and its drags come
 * to THAT RowComp -- so keying on the carried one sent them to stock, which
 * scrolls the list out from under the drag. dg_finger reads the position from
 * whichever row the event reached, so any row of this list will do. */
static void dg_mousedrag(void *self, void *event)
{
    uintptr_t rc = (uintptr_t)self;
    int finger;

    if (!dg_g_row || !dg_alive() || !dg_in_edit_list(rc)) {
        ((void (*)(void *, void *))dg_g_stock_drag)(self, event);
        return;
    }
    /* THE LIST RECYCLES RowComps, AND THAT IS NOT A REASON TO STOP. It hands its
     * dozen components round to whatever is on screen, so carrying a row far
     * enough for the list to scroll gives ours away -- which is ordinary once the
     * drag can scroll, and used to end the gesture at about the eleventh row.
     *
     * Nothing the drag needs is carried by the component: the row it started as
     * and where it would land are both held here, and both are measured in
     * CONTENT coordinates, which a scroll does not move. What the component does
     * carry is what it paints, and dg_paint lends the original row number back
     * for the length of the stock paint. dg_alive stays the real guard -- a
     * DELETED component is the one thing that must not be written to. */
    dg_g_settle = 0;
    finger = dg_finger(rc, event);
    if (finger != DG_NO_FINGER)
        dg_carry_to(finger);
}

/* The drop itself, once the finger is agreed to be gone. Called from the tick
 * rather than from mouseUp; see dg_mouseup. */
static void dg_commit(void)
{
    uint32_t pid;
    int from = dg_g_from, to = dg_g_insert;

    dg_end();
    if (!dg_g_marked)
        MDBG("browse: the insertion mark never drew -- the rows are not being "
             "repainted, or the row number is not where it is read from\n");
    dg_g_marked = 0;
    if (from == to) {
        MDBG("browse: row %d put back where it was\n", from);
        return;
    }
    /* 1-based, because that is what the browser's `#` column shows and what
     * DJDBSONGPLAYLIST.TRACKNO stores.
     *
     * ASYNC because this is the message thread, which has no djdb context by
     * construction -- the write lands on the next library message, the same way
     * a held tempo does. The playlist is the one the cache this list is served
     * from was asked for. */
    pid = mod_djdb_playlist_now();
    if (!pid) {
        /* Refused, not guessed: writing into whichever playlist was opened last
         * would reorder a list the DJ is not looking at. */
        MWARN("browse: #%d -> #%d NOT written -- no playlist is known for this "
             "list\n", from + 1, to + 1);
        return;
    }
    MDBG("browse: MOVE #%d -> #%d in playlist %u\n", from + 1, to + 1,
         (unsigned)pid);
    /* dg_g_rows is what the DJ was actually looking at, and it is evidence the
     * write holds independently of how `pid` was arrived at. See db.h. */
    if (mod_djdb_move_track_async(pid, from + 1, to + 1, dg_g_rows) != 0)
        return;
    /* FORGET THE ROWS THE DECK CACHED FOR THIS LIST, so that when it next fills
     * the list it reads them rather than serving its own stale copy.
     *
     * Then ask for it again, which is both how the list catches up and how the
     * write gets a thread at all -- see browse_sort_refetch. */
    mod_djdb_drop_list_cache(pid);
    browse_sort_refetch();
}

/* A LIFTED FINGER HAS TO STAY LIFTED. The deck's touch layer emits a RELEASE
 * AND A PRESS when the finger jumps far between two samples -- the MISO frame
 * shows one unbroken touch throughout, so the break is synthesised, not real.
 * Taken at face value it ends the drag and grabs whatever row the finger had
 * reached, which is the "drag fast and it swaps to another track" this was
 * reported with.
 *
 * So a release only counts once it has survived a few ticks. A press inside
 * that window cancels it and the drag carries on; nothing else can, because a
 * human cannot lift and land again in 70ms. The drop is that much later than
 * the finger, which is not a delay anyone can see. 44 Hz. */
/* MEASURED: 14 ticks, ~320ms. Not a jiggle -- the deck DROPS the touch on a
 * fast move and re-acquires it a third of a second later, and the release it
 * sends in between is indistinguishable from a finger leaving the glass.
 *
 * 20 gives margin. The cost is that a deliberate lift-and-press-again inside
 * ~450ms on the same list resumes the old drag instead of starting a new one --
 * possible, rare, and undone by simply dragging again; against a fast drag
 * silently swapping to a different track, which is what it buys. */
#define DG_SETTLE_TICKS 20

static void dg_mouseup(void *self, void *event)
{
    if (!dg_g_row || !dg_in_edit_list((uintptr_t)self)) {
        ((void (*)(void *, void *))dg_g_stock_up)(self, event);
        return;
    }
    /* THE ROW GOES BACK NOW, THE WRITE WAITS. Holding it in the air for the
     * length of the settle would put a third of a second of hang into every
     * ordinary drop just to survive the fast ones. So the release looks final
     * and only the commit is deferred; a press inside the window lifts the SAME
     * row again and the gesture carries on from where the finger is. */
    dg_place(dg_g_orig[1]);
    dg_repaint();
    dg_g_lifted = 0;
    dg_g_settle = DG_SETTLE_TICKS;
}

/* ---- what the drag looks like -------------------------------------------- */

/* NO SELECTION WHILE THE MODE IS ON. The blue plate means "this is the track the
 * rotary and LOAD are pointing at", and in EDIT neither is what the DJ is doing:
 * it reads as "this row is the one being moved", which is a different row every
 * time and usually not the one lit.
 *
 * Taken away in the PAINT rather than by deselecting the list. The deck's own
 * selection is a real thing it uses -- for LOAD, and for where the rotary
 * resumes -- so clearing it would be a change the DJ has to get back afterwards.
 * The byte goes down for the length of the stock paint and straight back up, so
 * nothing outside this function can tell. */
static void dg_paint(void *self, void *g)
{
    uintptr_t rc = (uintptr_t)self;
    int32_t b[4];
    uint8_t was = 0, off = 0;
    int hidden = 0;

    if (browse_edit_on() && dg_in_edit_list(rc) &&
        mod_safe_read(rc + RC_SELECTED_OFF, &was, sizeof(was)) == 0 && was) {
        mod_safe_write(rc + RC_SELECTED_OFF, &off, sizeof(off));
        hidden = 1;
    }
    ((void (*)(void *, void *))dg_g_stock_paint)(self, g);
    if (hidden)
        mod_safe_write(rc + RC_SELECTED_OFF, &was, sizeof(was));
    if (!dg_g_lifted || juce_comp_bounds(rc, b) != 0)
        return;

    mod_draw_enter();
    if (rc != dg_g_row &&
        dg_row_no(rc) == dg_g_insert && dg_g_insert != dg_g_from) {
        dg_g_marked = 1;
        /* WHICH EDGE IS THE DIRECTION, and it has to be, or the last position
         * cannot be drawn. A drag DOWN to row k lands after k -- that is what
         * the write does, #1 to #8 puts the row after the one that was 8th --
         * so the mark belongs on k's bottom edge; a drag UP lands before k, so
         * it belongs on the top. Drawn on the top always, the mark sat ABOVE the
         * last row while the write moved the track below it, which reads as
         * "second to last" and leaves nothing on screen that means "last".
         *
         * Drawn by the row rather than by the list, so it is always in step with
         * where the rows actually are -- including while they scroll.
         *
         * THE LIST'S OWN SELECTED GREEN, which is a change from the white it used
         * to be and the point of it: the mark says "this is the row in question"
         * about a row that does not exist yet, and the browser already has a colour
         * for that. White said nothing, in a list full of bright edges -- and
         * hardcoded it was white on WHITE's inverted list, an insert marker nobody
         * could see during the one gesture that needs one. */
        mod_gfx_colour(g, mod_colour_stock(DG_MARK_COL));
        mod_gfx_fill(g, 0, dg_g_insert > dg_g_from ? b[3] - DG_LINE_H : 0,
                     b[2], DG_LINE_H);
    }
    mod_draw_leave();
}

/* ---- the carried row's translucency ---------------------------------------
 *
 * A real transparency layer, not a wash. A wash DARKENS -- it puts black over
 * the row, so the row reads dimmer and nothing behind it shows through, which
 * is what it looked like. juce composites a layer instead: the row is rendered
 * into its own buffer and blended over what is already on screen, so the rows
 * it crosses are visible underneath it.
 *
 * Bracketed on the WRAPPER, whose paint runs before its children and whose
 * paintOverChildren runs after, so the layer spans the RowComp and the two
 * children the RowComp has -- the waveform strip and the artwork dot. A layer
 * opened in the row's own paint would have closed before either of them drew,
 * which is the other half of why only the lettering ever changed.
 *
 * The two slots are juce::LowLevelGraphicsContext's, +0x80 and +0x88, read off
 * the software renderer's vtable and confirmed by what they do: one allocates a
 * buffer and stashes the opacity, the other pops it, sets that opacity and
 * blends the buffer back. juce::Graphics' first member is the context. */
#define GFX_VT_LAYER_BEGIN  0x80
#define GFX_VT_LAYER_END    0x88

static uintptr_t dg_ctx_slot(void *g, unsigned off, uintptr_t *ctx_out)
{
    uintptr_t ctx = 0, vt = 0, fn = 0;

    if (mod_safe_read((uintptr_t)g, &ctx, sizeof(ctx)) != 0 || !ctx)
        return 0;
    if (mod_safe_read(ctx, &vt, sizeof(vt)) != 0 || !vt)
        return 0;
    if (mod_safe_read(vt + off, &fn, sizeof(fn)) != 0 || !fn)
        return 0;
    *ctx_out = ctx;
    return fn;
}

/* Balanced by construction: the flag is what paintOverChildren closes on, so a
 * frame where the layer could not be opened does not try to end one. */
static int dg_g_layered;

static void dg_wrap_paint(void *self, void *g)
{
    uintptr_t ctx = 0, fn;

    ((void (*)(void *, void *))dg_g_stock_wrap_paint)(self, g);
    dg_g_layered = 0;
    if (!dg_g_lifted || (uintptr_t)self != dg_g_wrap)
        return;
    fn = dg_ctx_slot(g, GFX_VT_LAYER_BEGIN, &ctx);
    if (!fn)
        return;
    ((void (*)(void *, float))fn)((void *)ctx, DG_GHOST_ALPHA);
    dg_g_layered = 1;
}

static void dg_wrap_paint_over(void *self, void *g)
{
    uintptr_t ctx = 0, fn;

    if ((uintptr_t)self == dg_g_wrap && dg_g_layered) {
        dg_g_layered = 0;
        fn = dg_ctx_slot(g, GFX_VT_LAYER_END, &ctx);
        if (fn)
            ((void (*)(void *))fn)((void *)ctx);
    }
    ((void (*)(void *, void *))dg_g_stock_wrap_over)(self, g);
}

/* The hole. Drawn by the component the rows sit ON, whose paint runs BEFORE its
 * children -- so this lands behind every row and shows only where the carried
 * one used to be. */
static void dg_content_paint(void *self, void *g)
{
    ((void (*)(void *, void *))dg_g_stock_content_paint)(self, g);
    if (!dg_g_lifted || (uintptr_t)self != juce_comp_parent(dg_g_wrap))
        return;
    /* The list's own ground, so it goes through the theme as the deck value it is
     * rather than as a role -- there is no "background" role and there should not be
     * one, the answer is always whatever the transform does to the deck's. Left
     * literal it was a black rectangle punched through WHITE's light list. */
    mod_gfx_colour(g, mod_colour_stock(DG_HOLE_COL));
    mod_gfx_fill(g, dg_g_orig[0], dg_g_orig[1], dg_g_orig[2], dg_g_orig[3]);
}

/* NOT A TIMEOUT. juce's contract is mouseDown, then any number of mouseDrags,
 * then exactly one mouseUp -- so a drag that has stopped sending events is a
 * FINGER HELD STILL, not a gesture that got away, and there is nothing to
 * recover from. Counting idle ticks only ever cancelled the drag of somebody
 * who paused to decide where to drop.
 *
 * What this watches instead is the thing juce does not promise: that the row
 * still exists. The list can rebuild under a drag, and the release would then
 * arrive for a component nobody is holding. */
/* THE FINGER HELD AT AN EDGE HAS TO KEEP SCROLLING, and that is why this is on
 * the tick rather than in mouseDrag: a finger parked against the bottom of the
 * window sends no more drag events, so anything driven by them stops after one
 * row. The tick runs at 44 Hz whether the finger moves or not.
 *
 * The carried row is then re-placed from the finger's SCREEN position, which has
 * not changed -- what changed is the content under it, so the same screen point
 * is now a different row. */
static void dg_autoscroll(void)
{
    int32_t c[4], v[4];
    int px = 0;

    if (!dg_g_lifted || !dg_g_clip || juce_comp_bounds(dg_g_clip, c) != 0)
        return;
    if (dg_g_screen < DG_EDGE_PX)
        px = -DG_SCROLL_PX;
    else if (dg_g_screen > c[3] - DG_EDGE_PX)
        px = DG_SCROLL_PX;
    if (!px || !dg_scroll_by(px) || juce_comp_bounds(dg_g_view, v) != 0)
        return;
    dg_reclaim();
    dg_carry_to(dg_g_screen - v[1]);
}

void browse_drag_tick(void)
{
    if (dg_g_row && dg_g_lifted && dg_alive())
        dg_autoscroll();
    if (dg_g_row && dg_g_settle && dg_alive() && --dg_g_settle == 0) {
        dg_commit();
        return;
    }
    if (!dg_g_row || dg_alive())
        return;
    dg_g_settle = 0;
    MDBG("browse: the list rebuilt under the drag -- letting row %d go\n",
         dg_g_from);
    dg_g_row = dg_g_wrap = dg_g_owner = 0;
    dg_g_view = dg_g_clip = 0;
    dg_g_row_vptr = dg_g_wrap_vptr = 0;
    dg_g_from = dg_g_insert = -1;
}

/* ---- install ------------------------------------------------------------- */

int browse_drag_install(void)
{
    if (!ep122_sym(EP122_JUCE_COMP_SETBOUNDS) ||
        !ep122_sym(EP122_JUCE_COMP_REPAINT) ||
        !MOD_FN_GFX_SETCOLOUR || !MOD_FN_GFX_FILLRECT) {
        MDBG("browse: juce primitives did not resolve -> no reorder gesture\n");
        return -1;
    }
    /* All four or none: a mouseDown that starts a drag with no mouseUp to end
     * it leaves a row stranded halfway down the list. */
    if (mod_patch_vslot("rowDown", EP122_ROWCOMP, JUCE_VT_MOUSEDOWN,
                        (void *)dg_mousedown, &dg_g_stock_down) != 0 ||
        mod_patch_vslot("rowDrag", EP122_ROWCOMP, JUCE_VT_MOUSEDRAG,
                        (void *)dg_mousedrag, &dg_g_stock_drag) != 0 ||
        mod_patch_vslot("rowUp", EP122_ROWCOMP, JUCE_VT_MOUSEUP,
                        (void *)dg_mouseup, &dg_g_stock_up) != 0 ||
        mod_patch_vslot("rowPaint", EP122_ROWCOMP, JUCE_VT_PAINT,
                        (void *)dg_paint, &dg_g_stock_paint) != 0 ||
        mod_patch_vslot("rowWrapPaint", EP122_LISTBOX_ROW, JUCE_VT_PAINT,
                        (void *)dg_wrap_paint, &dg_g_stock_wrap_paint) != 0 ||
        mod_patch_vslot("rowWrapOver", EP122_LISTBOX_ROW, JUCE_VT_PAINTOVER,
                        (void *)dg_wrap_paint_over, &dg_g_stock_wrap_over) != 0 ||
        mod_patch_vslot("rowContent", EP122_VIEWED_COMP, JUCE_VT_PAINT,
                        (void *)dg_content_paint,
                        &dg_g_stock_content_paint) != 0) {
        MDBG("browse: could not take the row's touch -> no reorder gesture\n");
        return -1;
    }
    dg_g_from = dg_g_insert = -1;
    return 0;
}
