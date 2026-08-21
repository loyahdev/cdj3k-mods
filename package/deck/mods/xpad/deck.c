// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/deck.c - the eight pads and the three panel controls, borrowed.
 *
 * THE PANEL IS THE MODE, and this file is where that is enforced. With the X-PAD
 * shut, every one of these is exactly the button it says on the lid: the pads are
 * hot cues, MEMORY stores a memory cue, CALL/DELETE deletes one and VINYL SPEED
 * ADJUST sets the brake. Open the panel and each becomes the sampler's for as
 * long as it is open. Nothing the DJ has not opened is changed, which is what
 * makes taking DELETE -- a destructive button -- acceptable at all.
 *
 * The three controls all reach the app the same way: as an AsyncTask whose run
 * carries the gesture. NOT CALLING THE STOCK RUN IS THE WHOLE OF THE GATE -- the
 * value never reaches the deck, the control does nothing, and releasing the gate
 * restores it with no state to unwind.
 *
 *   MEMORY   input_devices::MemoryCueHandler::memoryCue   -> HOLD
 *   DELETE   input_devices::MemoryCueHandler::deleteCue   -> OVERDUB
 *   VINYL    dj_player::PlayPauseControlFacade::vinylSpeedAdjust -> VOL
 *
 * The first two capture nothing but `this`, so their run IS the gesture and
 * fires once per press. The vinyl one captures its float at +0x18 and the
 * VinylSpeedAdjustKind at +0x1c, measured off sub_1055850:
 *
 *     ldr s0, [x0, #0x18]      // <- the knob
 *     ldr w1, [x0, #0x1c]      // <- which of the two curves
 *
 * Everything here is [deck] -- the pads on the deck's own pad path, the three
 * tasks on whichever thread the box pops them.
 */
#include "xpad/xpad.h"
#include "cue/cue.h"
#include "stem/stem.h"       /* stem_grid_take: the roll's clock */
#include "kit/mod.h"

/* ---- the pads ------------------------------------------------------------- */

/* THE PANEL IS THE MODE, so an open panel takes all eight -- a file behind the pad or
 * not.
 *
 * cue.h says to claim only what can be honoured, on the grounds that a dead pad is
 * worse than the behaviour it replaced. That weighs the other way here, because the
 * behaviour it replaces is not neutral: an unclaimed pad fires a HOT CUE, and a hot cue
 * JUMPS THE PLAYHEAD. Leaving the empty pads unclaimed means a hand on the sampler's
 * pad row throws the track to a cue point mid-mix. A pad that does nothing is silent;
 * that one is audible to the room.
 *
 * What makes it honest rather than a blanket grab is the open gate in ui.c: the panel
 * refuses to open with no banks at all, so a panel that is up always has at least one
 * live pad and the empties are the tail of a partly-filled set. */
static int xpad_pad_claim(const struct cue_event *ev)
{
    (void)ev;
    return xpad_open();
}

static void xpad_pad(const struct cue_event *ev, enum cue_phase phase)
{
    /* THE PRESS IS THE WHOLE GESTURE. A pad selects its bank and plays it once;
     * the release means nothing, because nothing was held. What repeats a sample
     * is a finger on the X-PAD, not a finger on the pad.
     *
     * DOWN arrives for every pad, claimed or not -- see cue.h -- so the work is
     * on PRESSED, which only reaches the pads this behaviour took. */
    if (phase != CUE_PAD_PRESSED)
        return;

    /* HERE, because a pad press is the one moment a cue slot is in reach and a
     * cue slot is where the track's beat grid is readable. Without it there is
     * no clock: no quantize, no roll, and no bar. Re-read on every press rather
     * than cached -- it costs a handful of reads and it cannot then be the
     * previous track's. Before the empty check, because which pad was pressed says
     * nothing about whether the grid is worth refreshing. */
    stem_grid_take(ev);
    /* An empty pad is SILENT. Swallowed above so it cannot reach the hot cue, and
     * dropped here so it cannot reach the sampler either: xpad_fire would make this the
     * SELECTED bank, leaving the strip pointing at nothing for the next gesture. */
    if (!xpad_bank_ready(ev->pad)) {
        MDBG("xpad: pad %c has no sample -> swallowed, no hot cue\n", 'A' + ev->pad);
        return;
    }
    xpad_fire(ev->pad);
    MDBG("xpad: pad %c selected -> %s\n", 'A' + ev->pad,
         xpad_bank_name(ev->pad));
}

/* Ahead of GROOVE CIRCUIT at 5, though the two can never both claim: the circuit
 * is gated on the stem row and this on the X-PAD panel, and the band holds one
 * panel. The order is stated anyway so the rule does not rest on that. */
CUE_HANDLER(k_cue_xpad,
            .name = "xpad", .prio = 4,
            .pad = xpad_pad, .pad_claim = xpad_pad_claim);

/* ---- the three panel controls --------------------------------------------- */

static uintptr_t xpad_g_orig_memory, xpad_g_orig_delete, xpad_g_orig_vinyl;

/* The knob's float, as the readout's percentage.
 *
 * Reported once per direction of travel while unmeasured, because the knob's
 * units are the deck's business -- a normalised position and a brake time look
 * identical in a register. The mapping below assumes 0..1 and clamps, so a value
 * outside that lands at an end of the range rather than anywhere strange, and
 * the log says what actually arrived. */
#define VINYL_TASK_FLOAT_OFF  0x18
#define VINYL_TASK_KIND_OFF   0x1c

static void xpad_vinyl_run(void *task)
{
    static int said;
    float v = 0.0f;
    int32_t kind = 0;
    int pct;

    if (!xpad_open()) {
        if (xpad_g_orig_vinyl)
            ((void (*)(void *))xpad_g_orig_vinyl)(task);
        return;
    }

    memcpy(&v, (const char *)task + VINYL_TASK_FLOAT_OFF, sizeof(v));
    memcpy(&kind, (const char *)task + VINYL_TASK_KIND_OFF, sizeof(kind));

    pct = (int)(v * 100.0f + 0.5f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    __atomic_store_n(&xpad_g_vol, pct, __ATOMIC_RELAXED);

    if (said < 8) {
        said++;
        MDBG("xpad: vinyl knob %.4f kind %d -> vol %d%%\n", (double)v, kind, pct);
    }
    /* The rail follows the knob, which is the only thing that moves it -- without
     * this the readout only caught up when something else happened to repaint it,
     * so the level appeared to jump on the next touch of the strip. */
    xpad_repaint();
    /* SWALLOWED: the brake stays where the DJ left it, and stays there after the
     * panel shuts until the knob is moved again -- measured end to end, PLAY
     * then PAUSE, watching whether the deck stops dead or coasts.
     *
     * KNOWN INCOMPLETE, and worth knowing before trusting it anywhere else. The
     * value lands in one store, dj_player 0x1127180, and three things reach it:
     * this task's run, a SYNCHRONOUS FALLBACK inside the facade itself when its
     * async predicate fails (0x1056f4c / 0x1056fb4), and a second public entry
     * at 0x10583e0 that tail-calls the store outright. Only the task is a
     * vtable slot, so only the task can be gated in this tree -- the other two
     * would write straight past. The knob takes the async path today and the
     * gate holds because of that, not because it is airtight.
     *
     * If it ever leaks: the object is *(*(facade + 0x58) + 0x80) with the facade
     * at task +0x20, holding the coefficient at +0x18 (kind 1) and +0x1c
     * (kind 2) and the raw 0..255 at +0x20 and +0x24. Snapshot those four when
     * the panel opens and re-assert them on the display tick, which covers every
     * writer rather than one of three.
     *
     * Every event seen so far arrives as kind 0, which the facade maps to its
     * kind 2 -- the brake. The kind 1 half is untested because nothing on this
     * chassis drives it. */
}

static void xpad_memory_run(void *task)
{
    if (!xpad_open()) {
        if (xpad_g_orig_memory)
            ((void (*)(void *))xpad_g_orig_memory)(task);
        return;
    }
    xpad_g_hold = !xpad_g_hold;
    MDBG("xpad: MEMORY -> hold %s\n", xpad_g_hold ? "on" : "off");
    xpad_repaint();
}

static void xpad_delete_run(void *task)
{
    if (!xpad_open()) {
        if (xpad_g_orig_delete)
            ((void (*)(void *))xpad_g_orig_delete)(task);
        return;
    }
    xpad_overdub_set(!xpad_g_overdub);
    xpad_repaint();
}

/* ---- the deck's own QUANTIZE ----------------------------------------------
 *
 * gui::QuantizeState listens to the two settings and keeps both in itself, so
 * one object answers the whole question:
 *
 *   +0x8c  the MODE, a bool
 *   +0x90  the BEAT VALUE, 1..4
 *
 * We learn where it lives from either of its two onSettingChanged closures --
 * which hold it at DIFFERENT offsets, +0x18 on the mode task and +0x20 on the
 * value task -- and then read the pair off the object rather than off whichever
 * closure happened to fire. That way one change teaches us both fields, and a
 * change to either keeps them current.
 *
 * UNTIL ONE FIRES WE DO NOT KNOW, and the honest answer to not knowing is not to
 * snap: an unasked-for quantize on every pad press is worse than none. The app
 * applies its settings at startup, so in practice this is answered before the
 * panel can be opened -- and the log says which value arrived. */
#define QUANT_STATE_MODE_OFF   0x8c
#define QUANT_STATE_VALUE_OFF  0x90
#define QUANT_MODE_TASK_STATE  0x18
#define QUANT_VALUE_TASK_STATE 0x20

static uintptr_t xpad_g_orig_quant_mode, xpad_g_orig_quant_value;
static uintptr_t xpad_g_quant_state;
static int       xpad_g_quant_on, xpad_g_quant_val;

/* QuantizeBeatValue -> the divisor of a beat. The enum runs 1..4 FINEST FIRST,
 * which is the opposite of how the deck lists the choices and was worth pinning
 * down rather than assuming: at boot the app reported value 4 while the play
 * screen's own box read "1", and those are two readings of one setting at one
 * moment. Anything outside 1..4 is refused rather than guessed at, which shows
 * up as quantize not applying instead of applying at a wrong length. */
static int quant_div_of(int v)
{
    switch (v) {
    case 1:  return 8;      /* 1/8 beat */
    case 2:  return 4;      /* 1/4      */
    case 3:  return 2;      /* 1/2      */
    case 4:  return 1;      /* 1 beat   */
    default: return 0;
    }
}

int xpad_quantize_div(void)
{
    if (!__atomic_load_n(&xpad_g_quant_on, __ATOMIC_RELAXED))
        return 0;
    return quant_div_of(__atomic_load_n(&xpad_g_quant_val, __ATOMIC_RELAXED));
}

/* Read both fields off the state object and publish them. Called after the stock
 * run, so what is read is what the app just stored. */
static void quant_refresh(uintptr_t state)
{
    int32_t mode = 0, val = 0;
    static int said_on = -1, said_val = -1;

    if (!state) return;
    xpad_g_quant_state = state;
    if (mod_safe_read(state + QUANT_STATE_MODE_OFF, &mode, sizeof(mode)) != 0 ||
        mod_safe_read(state + QUANT_STATE_VALUE_OFF, &val, sizeof(val)) != 0)
        return;
    __atomic_store_n(&xpad_g_quant_on, mode != 0, __ATOMIC_RELAXED);
    __atomic_store_n(&xpad_g_quant_val, (int)val, __ATOMIC_RELAXED);
    if (mode != said_on || val != said_val) {
        said_on = mode; said_val = val;
        MDBG("xpad: deck quantize %s, value %d -> 1/%d beat\n",
             mode ? "on" : "off", (int)val, quant_div_of((int)val));
    }
}

static void xpad_quant_mode_run(void *task)
{
    uintptr_t state = 0;

    if (xpad_g_orig_quant_mode)
        ((void (*)(void *))xpad_g_orig_quant_mode)(task);
    if (mod_safe_read((uintptr_t)task + QUANT_MODE_TASK_STATE,
                      &state, sizeof(state)) == 0)
        quant_refresh(state);
}

static void xpad_quant_value_run(void *task)
{
    uintptr_t state = 0;

    if (xpad_g_orig_quant_value)
        ((void (*)(void *))xpad_g_orig_quant_value)(task);
    if (mod_safe_read((uintptr_t)task + QUANT_VALUE_TASK_STATE,
                      &state, sizeof(state)) == 0)
        quant_refresh(state);
}

/* ---- install -------------------------------------------------------------- */

static int xpad_deck_install(void)
{
    int ok = 0;

    if (!cue_pad_ready())
        MERR("xpad: no cue interception -> the pads stay hot cues\n");

    if (mod_patch_vslot("xpadMemory", EP122_MEMCUE_MEMORY_TASK, 0x10,
                        (void *)xpad_memory_run, &xpad_g_orig_memory) == 0)
        ok++;
    else
        MWARN("xpad: no MEMORY task -> HOLD has no button\n");

    if (mod_patch_vslot("xpadDelete", EP122_MEMCUE_DELETE_TASK, 0x10,
                        (void *)xpad_delete_run, &xpad_g_orig_delete) == 0)
        ok++;
    else
        MWARN("xpad: no DELETE task -> OVERDUB has no button\n");

    if (mod_patch_vslot("xpadVinyl", EP122_VINYL_ADJ_TASK, 0x10,
                        (void *)xpad_vinyl_run, &xpad_g_orig_vinyl) == 0)
        ok++;
    else
        MWARN("xpad: no VINYL task -> VOL has no knob\n");

    /* Chained, never swallowed: the deck's QUANTIZE is the deck's, and we are
     * only listening. Failing either leaves the sampler unquantized, which is
     * the same as the DJ having switched it off -- so it is reported and not
     * fatal. */
    if (mod_patch_vslot("xpadQuantMode", EP122_QUANT_MODE_TASK, 0x10,
                        (void *)xpad_quant_mode_run, &xpad_g_orig_quant_mode) != 0)
        MDBG("xpad: no QUANTIZE mode task -> the sampler will not snap\n");
    if (mod_patch_vslot("xpadQuantValue", EP122_QUANT_VALUE_TASK, 0x10,
                        (void *)xpad_quant_value_run, &xpad_g_orig_quant_value) != 0)
        MDBG("xpad: no QUANTIZE value task -> the sampler will not snap\n");

    /* Not fatal, any of it. The pad half is the feature and the three controls
     * are how it is played; a firmware that moved one of them leaves the rest
     * working and says which one went. */
    return ok > 0 ? 0 : -1;
}

KIT_MOD(k_mod_xpad_deck,
        .name = "xpad_deck", .prio = 36, .install = xpad_deck_install,
        .what = "X-PAD SAMPLER: the pads, and MEMORY/DELETE/VINYL while it is open");
