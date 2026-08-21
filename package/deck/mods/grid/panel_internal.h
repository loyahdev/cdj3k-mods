/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/grid/panel_internal.h - what the grid panel files share.
 *
 * panel.c owns the hooks, the build and every piece of state below;
 * panel_button.c one button; panel_layout.c where they sit; panel_debug.c the
 * tree dump. Declarations only.
 */
#ifndef EP122_MOD_GRID_PANEL_INTERNAL_H
#define EP122_MOD_GRID_PANEL_INTERNAL_H

#include "core/mod_core.h"
#include "juce/draw.h"
#include "juce/juce.h"

/* The five buttons, in the order they are built and laid out. */
enum { GP_HALF, GP_DOUBLE, GP_ENLARGE, GP_REDUCE, GP_RESET };

/* The panel's geometry, in screen pixels. */
#define GP_BTN_W        116
#define GP_BTN_H        84
#define GP_PLATE_INSET  1
#define GP_RESET_W      90
#define GP_RESET_H      50
#define GP_N            5
#define GP_VGAP         10             /* between any two neighbours           */
#define GP_VRESET       (2 * GP_VGAP)  /* the last button to that group's RESET */
#define GP_GROUP_L      GP_MARGIN

#define GP_BTN_EDGE     2

#define GP_COL_LINE     0xff7d7d7dU

#define GP_COL_FILL     0xff323232U

#define GP_COL_LINE_ON  0xffffffffU

#define GP_COL_FILL_ON  0xff646464U

/* ...and the state the deck's own artwork carries that this had not: nothing to
 * undo. Off the design, not sampled. */
#define GP_COL_LINE_OFF 0xff262626U

#define GP_COL_FILL_OFF 0xff101010U

#define GP_COL_TEXT_OFF 0xff4b4b4bU

#define GP_COL_TEXT     0xffffffffU

#define GP_COL_GROUP    0xff1a1a1aU   /* the plate a group sits on */

#define GP_FONT         20.0f

/* The band over the group is a small dim "BPM" and a value that has to be
 * READABLE -- one Label has one font, so it is three components: a backdrop that
 * is the band, and the word and the number inside it. The two are placed as ONE
 * unit centred on the band, not one at each end: "BPM" is a label FOR the number
 * and has to sit against it to read as one.
 *
 * BOTH BOXES ARE OVERSIZED ON PURPOSE. juce::Label narrows text to its minimum
 * horizontal scale before it clips, so a box a few pixels short does not look
 * like a box a few pixels short -- it looks like somebody changed the typeface to
 * a condensed one. "126.0" at this size is ~60px and Label insets 5px each side,
 * so 76 was already squashing it. They abut rather than each being centred in
 * its own box, which is the only way the space between them is a chosen number
 * and not whatever slack the boxes happen to have.
 *
 * They also share the band's FULL HEIGHT rather than the word getting a short
 * box of its own: juce centres text within whatever box it is given, so two
 * boxes of different heights centre two different line boxes and the word ends
 * up sitting above the number. One box height, one centre line. */
#define GP_RO_FONT      20.0f

#define GP_CAP_FONT     14.0f

#define GP_COL_CAP      0xff7d7d7dU

#define GP_CAP_W        50

#define GP_VAL_W        100

#define GP_UNIT_W       (GP_CAP_W + GP_VAL_W)

/* ---- the arrows are DRAWN, not typed --------------------------------------
 *
 * U+25C0/U+25B6 are in two of the deck's three faces and render correctly, but
 * at the size the bars want they are far heavier than the bars beside them, and
 * a Label has ONE font -- shrinking the glyph shrinks the |||. So the label is
 * the bars alone and the triangles are laid down here, sized independently.
 *
 * Drawn as columns of a filled rect each, which is what the kit has; at seven
 * columns that is seven fills per arrow, on a strip that repaints when a finger
 * lands on it. */
#define GP_ARROW_W      7     /* columns from base to apex */

#define GP_ARROW_HALF   6     /* half the base height      */

/* From the BUTTON'S CENTRE, not in from its edges: the arrows belong to the bars
 * beside them, so they have to be placed against the lettering. Measured off the
 * running deck, ||| at this size is 13px wide, so its edges are at +-7 and this
 * leaves ~4px of air either side. Placed from the edges instead they sat 15px
 * out and read as three separate marks. */
#define GP_ARROW_OFF    14

#define GP_RESET_FONT   20.0f

#define GP_N_PLATE      4     /* ...of them on the plate; RESET stands off it */

#define GP_MARGIN       20    /* screen edge to the outermost button          */

#define GP_VPLATE       GP_VGAP        /* the plate's border round its four    */

#define GP_VGROUP       (4 * GP_VGAP)  /* the deck's RESET to that plate       */

#define GP_OGAP         (GP_VGAP - 2 * GP_PLATE_INSET)  /* ours, in bounds     */

#define GP_PAD          GP_PLATE_PAD

/* The plate hugs its four buttons -- a backdrop that groups them, not a frame
 * around them. It is also the vertical margin, so the BPM band above sits this
 * close to the buttons rather than a gap's worth away. */
#define GP_PLATE_PAD    (GP_VPLATE - GP_PLATE_INSET)

#define GP_LEFT_W       (GP_N_PLATE * GP_BTN_W + GP_RESET_W + 2 * GP_VGAP + \
                         (GP_VGAP - GP_PLATE_INSET))

#define GP_OURS_W       (GP_N_PLATE * GP_BTN_W + 3 * GP_OGAP + \
                         GP_VRESET - 2 * GP_PLATE_INSET + GP_RESET_W)

#define GP_PLATE_W      (GP_N_PLATE * GP_BTN_W + 3 * GP_OGAP + 2 * GP_PLATE_PAD)

#define GP_PANEL_H      (GP_BTN_H + 2 * GP_PAD)

/* The band above the buttons is narrower than the plate again: its left edge is
 * the middle of the gap after the first button, and its right edge is the SEAM
 * of the flush pair. So both edges continue a line that is already there rather
 * than cutting in beside a button. It also has to stay left of x 1180, where the
 * waveform's ZOOM/GRID indicator sits -- covering the control that leaves grid
 * mode is not a trade worth making. */
#define GP_BAND_X(f)    ((f) + GP_BTN_W + GP_OGAP / 2)

#define GP_BAND_W       (2 * (GP_BTN_W + GP_OGAP))

/* The BPM readout sits ABOVE the group, which is outside the 84px strip -- so it
 * is a child of the panel's PARENT, not of the panel (juce clips a child to its
 * parent's bounds, so a negative y would simply not draw).
 *
 * It carries the group's plate colour ITSELF and sits flush on the strip, which
 * is how the readout ends up inside the group's block rather than floating over
 * the waveform above it: two rects that meet, in two components, reading as one.
 * That is also the only way to get it -- the panel cannot draw outside its own
 * 84px, and the parent's paint is under the waveform's. */
#define GP_RO_H         22

/* The text box hangs GP_RO_DESC below the band and is BOTTOM-justified, so what
 * lands near the band's bottom edge is the GLYPHS rather than the descender line
 * juce would otherwise leave sitting there. Measured before: 6px of that empty
 * descent plus the plate's 10px border put the value 16px off the buttons.
 *
 * THE OVERHANG IS THE FONT'S DESCENT AND HAS TO BE UNDER IT, because the band
 * clips its children -- at 6 the digits were pushed 2px past the bottom edge and
 * came back with their feet cut off, which is what an overhang bigger than the
 * descent looks like. Measured at 3: the glyphs stop one pixel inside the band
 * and the value sits a border's width off the buttons. */
#define GP_RO_DESC      3

#define GP_BAND_H       GP_RO_H   /* the lettering, and no padding of its own */

/* ---- identity ------------------------------------------------------------
 *
 * A CLASS IDENTITY IS ITS TYPEINFO, NOT ITS VTABLE. ep122_sym gives the vtable's
 * address point; juce_comp_class gives the typeinfo one word before whichever
 * vtable an object points at. Comparing those two directly never matches -- and
 * never matching looks exactly like "the panel is not built yet", which cost a
 * deploy cycle. Every vtable in a class's group shares the typeinfo, which is
 * the whole reason this works for a virtual base. */
#define GP_TI_PANEL   juce_class_of(ep122_sym(EP122_GRIDPANEL))

#define GP_TI_SNAP    juce_class_of(ep122_sym(EP122_GRIDBTN_SNAP))

#define GP_TI_SHIFT   juce_class_of(ep122_sym(EP122_GRIDBTN_SHIFT))

#define GP_TI_RESET   juce_class_of(ep122_sym(EP122_GRIDBTN_RESET))

/* ---- what the four do ----------------------------------------------------
 *
 * All five are one call into stem_grid_edit_*. The 3000X's wording is exact and
 * worth keeping: [x2]/[x1/2] "doubles or halves the number of beats", and
 * [Enlarge]/[Reduce] "moves the beatgrid by 1 msec based on the first grid" --
 * an interval change anchored at beat one, not an offset shift, which is why the
 * deck's own gridAdjustChengeReq cannot express either of them. */
#define GP_MS           0.001         /* what Enlarge/Reduce move the interval by */

/* Far past any usable tempo either way, and inside what the machinery accepts,
 * so a held finger stops at a limit rather than at a refusal. */
#define GP_MULT_MAX     8.0

#define GP_MULT_MIN     0.125

#define GP_STEPS_MAX    200

/* The deck's RESET, repainted to our shape. Its stock paint is NOT chained --
 * that is what draws the artwork this replaces. Everything else about the
 * button, the press included, is untouched.
 *
 * IT IS DRAWN IN ONE STATE, and that is the cost of taking the paint. The stock
 * artwork carries a greyed look while there is nothing to undo, and juce's own
 * enabled flag is NOT where that comes from: measured on the live panel, all
 * five stock buttons read flags 0x2 -- SNAP GRID lit and SHIFT GRID greyed
 * alike -- so the state is in the button's model and not in the Component. */
/* WHETHER THE DECK'S RESET HAS ANYTHING TO UNDO -- its own answer, not ours.
 *
 * MEASURED: the object was diffed across its paints over a 768-byte window while
 * the button was driven through every state, and exactly ONE byte ever moves --
 * at -0x28 from the Component subobject, 0 when there is something to reset and
 * 2 when there is not. Confirmed twice on the glass, and it is the deck's own
 * model rather than the inference from the grid offset that stood here before.
 *
 * ITS HELD LOOK IS NOT REPRODUCED, deliberately. The stock button does have one
 * -- it is simply so brief that it is often not visible at 60fps (user, having
 * watched for it) -- and nothing in the object says when it is on: its paint is
 * a thunk into the generic "draw this widget's image" path, and across a
 * 768-byte diff no byte moves under a finger. Reading it would mean standing in
 * its press path, which is exactly what crashed the deck. Ours has a held look
 * because ours IS ours: a Label with our own vtable, so the press arrives in our
 * own mouseDown. Not worth a second crash for something you cannot see.
 *
 * Read, never written. */
#define GP_ACTIVE_OFF   0x28          /* BEFORE the Component subobject */

#define GP_ACTIVE_VAL   0

/* ---- what a button looks like, from the design ---------------------------
 *
 * Outline 2 px, inner 112x80, so 114x82 of plate inside a 116x84 component.
 * Inactive is #7D7D7D on #323232; touched is #FFF on #646464. Straight from the
 * panel design rather than sampled, so these are the authority.
 *
 * DECK VALUES, NOT mod_ui() ROLES, and they go through the theme as such -- every
 * one of them is wrapped in mod_colour_stock at the point it is used.
 *
 * They were literals behind a mod_draw_enter bracket, on the reasoning that the five
 * stock buttons beside them are PNGs no theme touches, so a themed plate next to an
 * unthemed one would be worse than neither. The first half of that stopped being
 * true when image.c learned sprites: an ARGB sprite inside THEME_IMG_MAXDIM is
 * adopted and mapped per pixel, so the deck's five DO theme -- and ours were the
 * unthemed plate in the strip. On WHITE it was a black slab.
 *
 * A role for each would be the house answer and is the wrong one here. Four of these
 * ten have no role carrying their value (#646464, #101010, #262626, #4b4b4b), so
 * either ORIGINAL moves off the design or the shared struct grows eight members for
 * one panel. Neither buys anything: none of the three things a role exists to
 * protect -- stem hues staying apart, a checker's fixed ratio, the accent's own blue
 * -- is in play in a strip of greys and white. The transform is the right answer,
 * and mod_colour_stock is how a caller asks for it.
 *
 * So the values below stay the authority, ORIGINAL stays bit-identical to the
 * design, and every theme maps them exactly as it maps the sprites beside them. */
/* ---- RESET is SHORTER than the four it undoes -----------------------------
 *
 * The 3000X draws it that way, and it is the right way round: a control that
 * undoes the row is not one of the row. It applies to BOTH -- the deck's own
 * RESET is repainted to the same shape rather than left as its stock artwork,
 * because two RESETs of different heights in one strip is worse than either.
 *
 * Only its PAINT is taken. The press still goes to the firmware's own handler,
 * so the deck's grid reset does exactly what it always did; what changes is the
 * pixels. The cost is its artwork's own states -- pressed, and the grey it wears
 * while there is nothing to undo. See gp_reset_paint for why the Component's own
 * enabled flag is not a way to get the second one back. */
/* ---- the layout ----------------------------------------------------------
 *
 * ONE COMMON GAP, AND THREE PLACES THAT ARE NOT IT. Everything between two
 * buttons is GP_GAP; the exceptions are the only three things the design says:
 *
 *   1. the deck's 1/2 PAIR IS FLUSH, zero -- and only that pair. The reference
 *      draws those two as one rounded rect with a divider and the four BPM ones
 *      as four separate rects, so the rule does not carry over to ours.
 *   2. the BPM group's RESET stands off its plate, twice the common gap.
 *   3. the two GROUPS are separated, three times it.
 *
 * The deck's own RESET takes the common gap: it is the second reset that is set
 * apart, because it is the one with a plate to be apart from.
 *
 *   [ SNAP ][SHIFT][1/2][1/2][RST]     [[ x1/2 ][ x2 ][ <| ][ |> ]]    [RST]
 *          12     12   0    12     36   \__ plate, margin 4 __/    24
 *
 *   10 |<---- 596 ---->| 36 |<-- plate 508 -->| 24 |96| 10        = 1280
 *
 * EVERY BUTTON IS THE SAME HEIGHT as the deck's, 84, and the two RESETs are the
 * same smaller box -- they sit in one row with five pieces of firmware artwork
 * and any difference reads as a mistake. The plate's margin therefore cannot
 * come out of a button: GridAdjust itself is grown by GP_PAD at each end and
 * every button moved GP_PAD down it, which puts them back on exactly the row the
 * firmware had them on inside a taller strip. juce clips a child to its parent,
 * so there was no other way to have both the margin and the height. */
/* ---- GAPS ARE STATED AS WHAT A DJ SEES ------------------------------------
 *
 * Not as component bounds, and that distinction is the whole of this block. The
 * deck's five are PNGs whose artwork FILLS their bounds; every button we draw
 * stops GP_PLATE_INSET inside its own. So one bounds gap renders as three
 * different gaps depending on what is either side of it -- measured on the deck
 * at a bounds gap of 12: 12px between two of the deck's, 13px against a drawn
 * RESET, 14px between two of ours. Setting bounds and reading artwork is how a
 * layout that is exactly uniform in the source looks uneven on the glass.
 *
 * So these are the SEEN distances and gp_gap_before subtracts whatever insets
 * are in the way. */


/* All defined in panel.c, which builds the panel that owns them. */
extern uintptr_t gp_g_panel;
extern int32_t   gp_g_panel_b[4];
extern int       gp_g_panel_h;
extern uintptr_t gp_g_btn[GP_N];
extern int       gp_g_btn_y;
extern int       gp_g_first;
extern uintptr_t gp_g_readout;
extern double    gp_g_shown_bpm;
extern uintptr_t gp_g_stock[GP_N];

/* The pieces panel.c wires together when it builds. */
void gp_label_mousedown(void *self, void *event);
void gp_label_mouseup(void *self, void *event);
void gp_label_paint(void *self, void *g);
void gp_reset_paint(void *self, void *g);
int gp_btn_x(int first, int i, int ours);
void gp_layout(void);
void gp_readout_sync(void);
void gp_dump(uintptr_t comp, int depth);

/* Three states, in the order a finger meets them: OFF is nothing to undo, ON is
 * a finger on it, and the plain one is the rest of the time. */
enum { GP_STATE_ON = -1, GP_STATE_IDLE = 0, GP_STATE_OFF = 1 };

extern int       gp_g_hot;
extern double    gp_g_mult;
extern uintptr_t gp_g_orig_mouseup;
extern int       gp_g_steps;

void gp_action(int which);
void gp_repaint(uintptr_t comp);

#endif /* EP122_MOD_GRID_PANEL_INTERNAL_H */
