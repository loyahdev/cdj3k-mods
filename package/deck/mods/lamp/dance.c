// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * lamp/dance.c - the startup greeting: a breath of rainbow across the pads.
 *
 * Runs once, when the transport has proven the whole lamp path end to end (see
 * lamp_panel_ready), and hands the pads back for good when it is done.
 *
 * SHAPE. Each pad holds a fixed hue, red at A through violet at H, and a single
 * broad wave of BRIGHTNESS travels left to right over them. The rainbow is not
 * moved, it is revealed and let go.
 *
 * The breath cannot come from fading a lamp: the panel has three brightnesses
 * and no more (lamp.h, rule 1-2). It comes from the wave's width instead --
 * a lit core about one and a half pads across inside a dim body of four and a
 * half, so four or five lamps are glowing at any moment and each one climbs
 * off -> dim -> lit -> dim -> off as the wave crosses it. The wave starts and
 * ends off the ends of the row, so the row is dark at both.
 *
 * TIMED, NOT COUNTED. The frames arrive from the display tick, whose rate is a
 * property of how busy the deck is; the wave's position is read off the clock,
 * so a slow tick drops frames rather than slowing the greeting down.
 */
#include "lamp/lamp.h"

#include <math.h>

/* How long the whole pass takes, and how wide the wave is in pad-widths. */
#define DANCE_MS      1700u
#define DANCE_SPREAD  2.2f

/* The lit core, as a fraction of the spread. */
#define DANCE_CORE    0.34f

/* Red through violet, as indices into the measured wheel. Skips spring and
 * azure, which sit too close to their neighbours to be worth one of the eight
 * places; magenta and rose are past violet and would turn the row back towards
 * red. */
static const uint8_t k_dance_hue[LAMP_PADS] = {
    0,  /* red        */
    1,  /* orange     */
    2,  /* yellow     */
    3,  /* chartreuse */
    4,  /* green      */
    6,  /* cyan       */
    8,  /* blue       */
    9,  /* violet     */
};

static int      dance_g_on;
static uint32_t dance_g_t0;

void lamp_dance_start(void)
{
    __atomic_store_n(&dance_g_t0, lamp_now_ms(), __ATOMIC_RELAXED);
    __atomic_store_n(&dance_g_on, 1, __ATOMIC_RELEASE);
    MDBG("lamp: startup dance\n");
}

int lamp_dance_ask(int pad, struct lamp *out)
{
    uint32_t     el;
    float        centre, d;
    const uint8_t *hue;

    if (!__atomic_load_n(&dance_g_on, __ATOMIC_ACQUIRE))
        return 0;

    el = lamp_now_ms() - __atomic_load_n(&dance_g_t0, __ATOMIC_RELAXED);
    if (el >= DANCE_MS) {
        /* Over. Declines from here on, and the pads go back to whoever else
         * wants them on this same frame. */
        __atomic_store_n(&dance_g_on, 0, __ATOMIC_RELAXED);
        return 0;
    }

    /* The wave's centre, in pad units, from one spread off the left end to one
     * spread off the right. */
    centre = (float)el / (float)DANCE_MS
             * ((float)(LAMP_PADS - 1) + 2.0f * DANCE_SPREAD) - DANCE_SPREAD;
    d = fabsf((float)pad - centre);

    hue = k_lamp_wheel[k_dance_hue[pad]];
    lamp_set(out, hue[0], hue[1], hue[2],
             d <= DANCE_SPREAD * DANCE_CORE ? LAMP_LIT :
             d <= DANCE_SPREAD             ? LAMP_DIM : LAMP_OFF);
    return 1;
}
