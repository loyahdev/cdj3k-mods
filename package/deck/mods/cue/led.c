// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/led.c - the panel's lamps: the wire onto them.
 *
 * THE TRANSPORT, NOT THE POLICY. This finds the lamps, learns which ordinal is
 * which pad, holds each one's writer and writes. WHAT a pad should look like is
 * lamp/lamp.h -- including the law that says a triple is a hue and the slot is
 * the whole brightness. Nothing here knows what a groove or a sample bank is.
 *
 * ON THE APP, NOT THE WIRE. The frame the colours end up in leaves through an
 * ioctl, and hooking that would be easier -- but mods/ is being separated from
 * the emulation shim it currently lives in, and a syscall hook does not survive
 * that. Everything here is EP122's own path, the same way every other mod in
 * this tree works.
 *
 * Mechanism (RE-verified against EP122-3.19)
 * ------------------------------------------
 * hui::HuiIndicatorAbs is a lamp. It keeps an UpdateBehavior at +0x10 and its
 * own update just tail-calls that behaviour's, of which there are two:
 * SingleColor for a lamp that is only on or off, and MultiColor for one with a
 * colour -- which is what the hot cue pads are.
 *
 * MultiColor::update (vtable 0x21e7010 slot 2) is three lines: ask the source at
 * +0x08 for a packed u64 through ITS vtable +0x18, unpack, hand the pair to the
 * one function every lamp in the app writes through.
 *
 *   packed  byte 0     the LED index -- which lamp
 *           bytes 4-6  R, G, B
 *
 * So ONE wrapper sees every coloured lamp the app draws, with the index that
 * says which, and needs nothing from the indicator that owns it.
 *
 * The wrapper REPLACES that update rather than following it, because the colour
 * has to be decided before the write rather than corrected after: the write is
 * a change-notifier, and a second one carrying a different colour would be seen
 * by every listener as the lamp changing twice. Replacing it is faithful -- the
 * stock body is reproduced exactly, and any read that fails falls back to
 * calling it.
 *
 * WHICH INDEX IS WHICH PAD IS NOT ASSUMED. Nothing in the binary says the eight
 * hot cues are indices 0..7, so this logs what it sees the first time each lamp
 * takes a colour and the mapping is read off the deck. Guessing it would be the
 * same mistake as guessing a class name.
 */
#include "cue/cue.h"
#include "lamp/lamp.h"       /* what a pad should look like, and whose it is */
#include "kit/mod.h"

/* MultiColor's own layout, from sub_1803ae0. */
#define MC_SOURCE_OFF      0x08   /* the thing that knows this lamp's colour */
#define MC_COLOUR_SLOT     0x18   /* ...asked through its vtable            */

/* indicator_control::IndicatorWithIdentifier<DeckIndicatorKind>, built 35 to a
 * loop by DeckIndicatorController: 0x70 bytes, vtable at +0x00 and the loop
 * index -- the ordinal -- at +0x68. */
#define SRC_ORDINAL_OFF    0x68

#define VT_IND_DECK_ID     ep122_sym(EP122_IND_DECK_ID)

/* Ordinals. There are 35 deck indicators and the eight hot cue pads are
 * consecutive from this one.
 *
 * The pad SPACING was measured first, from lamps going green on a track cued
 * A, D and F -- three lit, two apart then three apart, which is the pad
 * spacing. But that counted position in the holder array, and the array does
 * not start where the indicator vector does: the base is one further on, which
 * showed up as every pad blinking its left-hand neighbour. The ordinal the
 * source was constructed with is the real numbering; the array only ever gave
 * the spacing. */
#define ORD_HOTCUE_A       4

/* The packed u64 that comes back. */
#define PACK_INDEX(v)      ((unsigned)((v) & 0xffu))
#define PACK_R(v)          ((uint8_t)(((v) >> 32) & 0xffu))
#define PACK_G(v)          ((uint8_t)(((v) >> 40) & 0xffu))
#define PACK_B(v)          ((uint8_t)(((v) >> 48) & 0xffu))

#define FN_LED_WRITE       ep122_sym(EP122_HUI_LED_WRITE)

typedef int64_t  (*mc_update_fn_t)(void *self, void *dst);
typedef int64_t  (*led_write_fn_t)(void *dst, uint32_t index, const void *rgb);
typedef uint64_t (*colour_fn_t)(void *src);

static uintptr_t led_g_orig;

/* ---- which lamp is which pad -------------------------------------------
 *
 * Asked of the colour source, which is the one object on this path that still
 * knows. The vtable is the family -- a media, browse or other lamp answers a
 * different one and is passed through untouched -- and +0x68 is the ordinal it
 * was constructed with. A source that is not the deck's is not a pad. */
static int led_pad_of(uintptr_t src, uintptr_t vt)
{
    int32_t ordinal = 0;

    if (!VT_IND_DECK_ID || vt != VT_IND_DECK_ID ||
        mod_safe_read(src + SRC_ORDINAL_OFF, &ordinal, sizeof(ordinal)) != 0)
        return -1;
    ordinal -= ORD_HOTCUE_A;
    return (ordinal >= 0 && ordinal < CUE_PADS) ? (int)ordinal : -1;
}

/* A lamp carries several colour slots and holds ONE of them at a time: the write
 * function keeps the index at +0xc0 and the colour at +0xc1, and runs the pair
 * through a transform at +0xb0 whose result is what the panel gets. So the index
 * is not a layer that composites -- it selects HOW the colour is rendered.
 *
 * THE INDEX IS BRIGHTNESS, and slot 2 is the lit one. Measured on 3.19: the app
 * writes slot 2 for a pad that holds a cue and slot 1 for one that does not, and
 * the same 255 comes out of the panel as 0x7f through slot 2 against 0x0c
 * through slot 1. Passing the app's index through therefore gave a groove-circuit
 * pad the right hue at a twentieth of the brightness -- our red, unreadable
 * across a booth -- on exactly the pads that hold no cue, which is most of them.
 * A pad we have taken is written on slot 2 whatever slot the app asked for. */
#define LED_PAD_SLOT      2
#define LED_PAD_DIM_SLOT  1

/* Where each pad's lamp is, and the last write the APP made to it.
 *
 * Both are needed because the app writes a lamp only when its OWN state
 * changes. Opening the stems row is not such a change, so without a repaint of
 * our own only the pads that happened to move would take our colours -- which
 * is exactly how one pad kept sitting on the stock idle colour while its
 * neighbours went red.
 *
 * The app's write is kept whole -- colour AND index -- because putting a pad
 * back is replaying it verbatim, and its index is half of what it looked like. */
static struct {
    uintptr_t holder;
    uint8_t   app[3];
    uint32_t  app_index;
    uint8_t   known;
    uint8_t   ours;      /* we coloured it last, so we owe it a restore */
} led_g_pad_hw[CUE_PADS];

/* One lamp value onto the wire: fills the triple and answers the slot.
 *
 * LAMP_OFF DROPS THE HUE. The level is the whole brightness (lamp.h), and the
 * panel scales a triple by its own maximum -- so a hue carried through on the
 * dim slot renders exactly as LAMP_DIM does, whatever the level says. Black on
 * the dim slot is what dark is, and the level alone cannot put a lamp out. */
static unsigned led_wire(const struct lamp *l, uint8_t *rgb)
{
    if (l->level == LAMP_OFF) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return LED_PAD_DIM_SLOT;
    }
    rgb[0] = l->rgb[0];
    rgb[1] = l->rgb[1];
    rgb[2] = l->rgb[2];
    return l->level == LAMP_LIT ? LED_PAD_SLOT : LED_PAD_DIM_SLOT;
}

/* All eight pads have written at least once, so the path is proven end to end:
 * the update hook is in, the ordinals are mapped and every holder is known.
 * Install returning 0 says only that the slots were patched. */
static void led_ready(void)
{
    int p;

    for (p = 0; p < CUE_PADS; p++)
        if (!led_g_pad_hw[p].known)
            return;
    lamp_panel_ready();
}

/* Repaint the pads when what they should say has moved.
 *
 * DRIVEN FROM THE DISPLAY TIMER, not from lamp writes. The app writes a lamp
 * only when its OWN state changes, and none of what lamp/ decides is such a
 * change: a groove running, a sample sounding, the startup sweep. Measured:
 * eight samples over three seconds, all identical. The stems row learned the
 * same thing about its progress bar and chained the app's refresh timer for it.
 *
 * Only on a change, not every tick: the write is a change-notifier and telling
 * it the same colour every frame is a lot of dispatch for nothing. It calls the
 * write directly, which is a different function from the update we replaced, so
 * there is no re-entry. */
void cue_led_tick(void)
{
    static uint32_t last_word;
    static int      have_last;
    uint32_t        word = lamp_word();
    int             p;

    if (have_last && word == last_word)
        return;
    have_last = 1;
    last_word = word;

    for (p = 0; p < CUE_PADS; p++) {
        struct lamp l;
        uint8_t     rgb[4];

        if (!led_g_pad_hw[p].known)
            continue;

        if (!lamp_pad(p, &l)) {
            /* NOT OURS. Written once, on the turn -- closing the row, ejecting
             * the stick, losing the stems and the sweep finishing all reach
             * here, and the app has no reason to repaint a lamp whose own state
             * has not moved. Its index goes back with its colour: the app chose
             * both, and the pair is what the pad looked like. */
            if (led_g_pad_hw[p].ours) {
                led_g_pad_hw[p].ours = 0;
                rgb[0] = led_g_pad_hw[p].app[0];
                rgb[1] = led_g_pad_hw[p].app[1];
                rgb[2] = led_g_pad_hw[p].app[2];
                rgb[3] = 0;
                ((led_write_fn_t)FN_LED_WRITE)((void *)led_g_pad_hw[p].holder,
                                               led_g_pad_hw[p].app_index, rgb);
            }
            continue;
        }
        rgb[3] = 0;
        led_g_pad_hw[p].ours = 1;
        ((led_write_fn_t)FN_LED_WRITE)((void *)led_g_pad_hw[p].holder,
                                       led_wire(&l, rgb), rgb);
    }
}

/* ---- the hook ------------------------------------------------------------ */

static int64_t led_wrap_update(void *self, void *dst)
{
    uintptr_t src = 0, vt = 0, fn = 0;
    uint64_t packed;
    uint8_t rgb[4];
    unsigned idx;

    if (!FN_LED_WRITE ||
        mod_safe_read((uintptr_t)self + MC_SOURCE_OFF, &src, sizeof(src)) != 0 ||
        !src ||
        mod_safe_read(src, &vt, sizeof(vt)) != 0 ||
        mod_safe_read(vt + MC_COLOUR_SLOT, &fn, sizeof(fn)) != 0 || !fn)
        return ((mc_update_fn_t)led_g_orig)(self, dst);

    /* Asked ONCE. The stock body asks once too, and a lamp's colour is not
     * something to poll twice per draw in case it has an opinion about it. */
    packed = ((colour_fn_t)fn)((void *)src);
    idx    = PACK_INDEX(packed);
    rgb[0] = PACK_R(packed);
    rgb[1] = PACK_G(packed);
    rgb[2] = PACK_B(packed);
    rgb[3] = 0;

    {
        int pad = led_pad_of(src, vt);

        if (pad >= 0) {
            /* The holder is the LAMP, so either slot names it -- and it has to
             * be taken from either, because the app writes slot 2 only for a pad
             * that HOLDS a cue. Taking it from slot 2 alone left every empty pad
             * unknown, so the repaint below skipped exactly the pads that were
             * supposed to turn red. */
            if (!led_g_pad_hw[pad].known)
                MDBG("led: pad %c is ordinal %d, lamp %#lx\n", 'A' + pad,
                     pad + ORD_HOTCUE_A, (unsigned long)dst);
            led_g_pad_hw[pad].holder = (uintptr_t)dst;
            led_g_pad_hw[pad].known  = 1;
            /* The whole write, whichever index it came on: this is the picture
             * to put back, and the app names both halves of it. */
            led_g_pad_hw[pad].app[0]    = rgb[0];
            led_g_pad_hw[pad].app[1]    = rgb[1];
            led_g_pad_hw[pad].app[2]    = rgb[2];
            led_g_pad_hw[pad].app_index = idx;

            /* Every holder known is the whole path proven, which is what the
             * startup sweep waits for. */
            led_ready();

            {
                struct lamp l;

                if (lamp_pad(pad, &l)) {
                    /* ON OUR SLOT, not the one the app asked for. The app drives
                     * a pad holding no cue on the dim slot continuously, so
                     * leaving its index alone let it undo the brightness between
                     * every repaint of ours -- the hue was right and the pad
                     * still read as unlit. */
                    idx = led_wire(&l, rgb);
                    led_g_pad_hw[pad].ours = 1;
                }
            }
        }
    }

    return ((led_write_fn_t)FN_LED_WRITE)(dst, (uint32_t)idx, rgb);
}

static int led_install(void)
{
    if (!FN_LED_WRITE) {
        MDBG("led: no write function -> lamps left alone\n");
        return -1;
    }
    if (mod_patch_vslot("huiMultiColor", EP122_HUI_MULTICOLOR, 0x10,
                        (void *)led_wrap_update, &led_g_orig) != 0)
        return -1;
    MDBG("led: coloured lamps intercepted\n");
    return 0;
}

KIT_MOD(k_mod_cue_led,
        .name = "cue_led", .prio = 6, .install = led_install,
        .what = "the panel's coloured lamps, decided in the app");
