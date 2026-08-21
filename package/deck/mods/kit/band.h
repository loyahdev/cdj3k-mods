// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/band.h - the strip under the waveform, borrowed.
 *
 * The deck's four quick-menu panels (BEAT LOOP, KEY SHIFT, BEAT JUMP, GRID) all
 * share one rect inside the waveform view, and opening one shrinks the waveform
 * to free it. A mod wanting a control strip of its own borrows that rect the
 * same way, by writing the app's own quick-menu mode register -- so the waveform
 * is laid out and rescaled by the code that owns it rather than by us.
 *
 * THE BAND HOLDS ONE PANEL. That is the app's rule, not ours, and it is why this
 * is a kit rather than a part of whichever feature got there first: STEMS and
 * X-PAD both want the strip, a stock panel may take it from either, and the
 * arbitration has to live somewhere that knows about all of them.
 *
 * A client declares itself next to its own code, the way KIT_MOD does, and is
 * told when the band goes away. It never learns who took it -- there is nothing
 * useful to do with that, and every route out is the same route out.
 *
 * Everything here is [message]: the juce UI thread, which is also the only
 * repaint tick.
 */
#ifndef EP122_MOD_KIT_BAND_H
#define EP122_MOD_KIT_BAND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


struct kit_band {
    const char *name;               /* for the log; tags nothing else */

    /* The quick-menu mode this client claims. It must be UNIQUE among clients
     * and past the app's own four, because the mode register -- not our own
     * bookkeeping -- is the authoritative record of who holds the band, and the
     * watch tells "still ours" from "taken" by reading it back.
     *
     * sub_14a40e0 branches only on 0 (close) and 4 (the grid panel); every other
     * value is stored and opens the band with no stock panel attached to it. */
    int32_t mode;

    /* The band went to somebody else -- a stock quick menu, another client, or a
     * route we have not found. Put the row away; do NOT hand the band back,
     * because whoever wrote that mode owns it now and releasing would shut the
     * panel that just opened. */
    void (*closed)(void);

    /* THE GATE THIS CLIENT IS BEHIND, or NULL for one that is always there. Read
     * to pack the slots: a client switched off holds none, so the ones that are
     * on close up rightward and the track title gets the difference back. */
    const int *shown;

    /* Put the quick-menu button at `x`. Called when the set of shown clients
     * changes -- a settings toggle, and nothing else -- so a client that builds
     * its button from kit_band_slot_x needs nothing else to stay in place. */
    void (*reslot)(int32_t x);

    /* WHERE THIS CLIENT SITS AMONG THE SHOWN ONES: 0 is the slot nearest the
     * app's own three. Stated rather than taken from the order the descriptors
     * happen to link in -- that order is arbitrary, and a row of buttons that
     * rearranges itself when a file is renamed is not a layout. Must be unique. */
    unsigned char order;
};

/* One descriptor. `used` because nothing in C refers to it: the section is the
 * reference. */
#define KIT_BAND(sym, ...) \
    static const struct kit_band sym __attribute__((used, \
        section("ep122_band"))) = { __VA_ARGS__ }

extern const struct kit_band __start_ep122_band[] __attribute__((visibility("hidden")));
extern const struct kit_band __stop_ep122_band[] __attribute__((visibility("hidden")));

/* ---- install ------------------------------------------------------------- */

/* [init] Verify the mode setter resolved and hook the stock quick-menu buttons.
 * Returns 0 when the band can be borrowed at all. Clients still work without it
 * -- kit_band_take falls back to moving components by hand -- so a refusal here
 * is reported, not fatal. */
int kit_band_install(void);

/* [message] The waveform title bar drew. Idempotent, and cheap after the first
 * call: every client's anchor calls it, since whichever draws first is the one
 * that resolves the tree. Returns 0 once the view and the rect are known. */
int kit_band_attach(uintptr_t touch_aria);

/* [message] Record the closed layout, so opening is a straight write of the
 * other one. Only valid while no panel is showing. */
void kit_band_snapshot(void);

/* [message] "This component is ours." The scans that identify the app's own
 * panels compare bounds, and a client's strip has the panel rect by
 * construction; its quick-menu button has the stock button's size. Both would
 * therefore match, so both are excluded by identity instead. Call it for the
 * strip and for the button, once each, after building them. */
void kit_band_own(uintptr_t comp);

/* ---- the tree, for building into ----------------------------------------- */

/* The title bar's own quick-menu buttons, measured off its ctor (sub_15c3140):
 * BEAT LOOP at x=898, KEY SHIFT at 1022, BEAT JUMP at 1146, each 114x90 on a
 * 124px stride. The app leaves exactly ONE gap in front of them, at x=774.
 *
 * THE SECOND SLOT IS TAKEN OUT OF THE TRACK TITLE. Its Label is {216, 10, 650,
 * 40}, so it runs to x=866 and is already under the 774 slot: a button one
 * stride further left lands on the lettering, which was built and looked at.
 * So the title is cut back to end before the new slot -- see KIT_BAND_TITLE_W.
 *
 * That is a real cost, 226px of track name, and it is the right one. The deck's
 * far corners are hard to reach behind a retracted bezel, so a half-height
 * button in the corner is the control that gets missed; a long title merely
 * ellipsizes, which is what the play screen already does to it.
 *
 * Slot numbers are assigned here rather than by each client, because they have
 * to be unique and two features picking their own would collide silently. */
#define KIT_BAND_BTN_W      114     /* a STOCK button, which is how the scans find one */
#define KIT_BAND_BTN_H      90
#define KIT_BAND_SLOT_W     114
#define KIT_BAND_SLOT_H     90
#define KIT_BAND_SLOT_STRIDE 124    /* the app's own, between its three */
#define KIT_BAND_SLOT_X(n)  (774 - (n) * KIT_BAND_SLOT_STRIDE)
#define KIT_BAND_SLOT_Y     0

/* WHERE A CLIENT'S BUTTON GOES, over the clients that are switched on. Slots are
 * no longer a number a client owns: with one of two features gated off the other
 * moves up, so the answer depends on the whole set and only the kit can give it. */
int32_t kit_band_slot_x(const struct kit_band *c);

/* [message] A client's gate moved. Re-places every shown client's button and
 * re-cuts the track title. */
void kit_band_slots_changed(void);

/* The track title's Label, and what it is cut back to. Keyed on all three of
 * the bounds that do not change so the squeeze cannot land on another child,
 * and re-asserted on the tick because the app re-sets them on a track change. */
#define KIT_BAND_TITLE_X    216
#define KIT_BAND_TITLE_Y    10
#define KIT_BAND_TITLE_H    40
#define KIT_BAND_TITLE_W    (KIT_BAND_SLOT_X(1) - KIT_BAND_TITLE_X - 10)

uintptr_t       kit_band_bar(void);     /* the title bar: a quick-menu button's parent */
uintptr_t       kit_band_view(void);    /* the waveform view: the strip's parent       */
const int32_t  *kit_band_rect(void);    /* {x,y,w,h} the stock panels share            */

/* True while one of the APP's own panels is up: it has already shrunk the
 * waveform, so ours must not shrink it again and must not restore it on the way
 * out. */
int kit_band_stock_up(void);

/* ---- holding it ---------------------------------------------------------- */

/* Take the band. Any client already holding it is told it lost it first, so two
 * strips can never draw into the same rect.
 *
 *   1  taken through the app's own mode register: it laid the waveform out
 *   0  the mode setter could not be reached, so the band is held by a by-hand
 *      shrink -- usable, but the layout may not have rescaled
 *  -1  THE APP REFUSED, which is what it does with no track loaded. THE CALLER
 *      MUST NOT OPEN: there is no panel rect to draw into, and nothing was taken
 *      so nothing has to be given back. */
int  kit_band_take(const struct kit_band *c);

/* Hand it back, by whichever route it was taken. Safe to call when not held. */
void kit_band_give(const struct kit_band *c);

int  kit_band_holds(const struct kit_band *c);

/* [message] Per display tick, from any client's clock. Notices the band being
 * taken without anybody pressing a button of ours -- GRID ADJUST opens on a
 * one-second rotary hold and is the reason this exists -- and tells the holder.
 * Also reconciles a band left shrunk around a mode nobody owns. */
void kit_band_poll(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_KIT_BAND_H */
