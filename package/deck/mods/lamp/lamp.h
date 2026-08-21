// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * lamp/lamp.h - what a colour IS on this panel, and who gets to say.
 *
 * Three layers, and this is the middle one:
 *
 *   cue/led.c   the TRANSPORT. Hooks hui::MultiColor::update, learns which
 *               ordinal is which pad, holds each lamp's writer, and writes.
 *   lamp/       the CONTROL. What a lamp value is, what the panel can render,
 *               and which feature owns a pad when more than one wants it.
 *   xpad/ stem/ the SOURCES. Each answers for its own pads and nothing else.
 *
 * A source never writes a lamp and never learns the law twice: it fills a
 * struct lamp and says whether the pad is its own.
 *
 * ================================================================== *
 * THE LAW (measured on 3.19, by sweeping known triples out of a source
 * and reading the panel's own MOSI frame back with `emuctl leds`)
 * ================================================================== *
 *
 * The app hands the write a colour AND a slot index; the write runs the pair
 * through a transform whose result is what reaches the panel. The transform's
 * class is not identified -- the binary is stripped -- so this is measured,
 * not read off the code.
 *
 * THE SLOT IS THE BRIGHTNESS, THE TRIPLE IS A CHROMA.
 *
 *   1. The triple is scaled by its own maximum first. (0,64,0) and (0,255,0)
 *      come back identical, and so do (128,128,128) and (255,255,255). There
 *      is no dimming a lamp by writing smaller numbers.
 *
 *   2. The slot is the light. Every hue tops out at 0x7f on slot 2 and 0x0c on
 *      slot 1 -- the same ratio for all of them, which is what makes the two
 *      slots a brightness and not two palettes.
 *
 *   3. Saturation is pushed up. (255,200,200) comes back pure red and
 *      (200,255,200) pure green: a washed-out request is rendered as the hue it
 *      is nearest, not washed out.
 *
 *   4. The channels are calibrated against each other. R, G and B each reach
 *      the slot maximum alone, but white returns #44787f and cyan #004b7f -- a
 *      mix is weighted, not summed, and not by a single gamma (fitting one
 *      gives 1.79 from the R/G pairs against 1.46 from the G/B pairs). A
 *      secondary has to be measured; it cannot be predicted from its primaries.
 *
 * So there are THREE brightnesses on this panel and no more: off, slot 1, slot
 * 2. An animation gets its envelope from those three and from how many lamps
 * are lit at once -- not from fading one.
 *
 * There is no neutral grey. White lands on the panel's calibrated near-white,
 * which on slot 1 is #060c0c, and that is the exact colour the deck itself puts
 * on a pad holding no hot cue -- so white-on-dim is what "nothing here" looks
 * like.
 */
#ifndef EP122_MOD_LAMP_H
#define EP122_MOD_LAMP_H

#include "core/mod_core.h"

#ifdef __cplusplus
extern "C" {
#endif


/* The eight hot-cue pads. The same eight as CUE_PADS and XP_BANKS, named again
 * because this layer sits under both and must not include either. */
#define LAMP_PADS 8

#define LAMP_OFF  0
#define LAMP_DIM  1   /* slot 1: every hue tops out at 0x0c */
#define LAMP_LIT  2   /* slot 2: every hue tops out at 0x7f */

/* One lamp's picture. `rgb` is a HUE and carries no brightness -- see the law.
 * A source that wants something dark says LAMP_OFF; scaling the triple down
 * does nothing at all. */
struct lamp {
    uint8_t rgb[3];
    uint8_t level;
};

static inline void lamp_set(struct lamp *l, uint8_t r, uint8_t g, uint8_t b,
                            int level)
{
    l->rgb[0] = r;
    l->rgb[1] = g;
    l->rgb[2] = b;
    l->level  = (uint8_t)level;
}

static inline void lamp_dark(struct lamp *l)
{
    lamp_set(l, 0, 0, 0, LAMP_OFF);
}

/* The hue wheel, measured. Every entry has been sent to the panel and read
 * back, so these are hues that are known to arrive distinct from each other:
 *
 *   red #7f0000  orange #7f2400  yellow #7f7f00  chartreuse #267f00
 *   green #007f00  spring #007c3b  cyan #004b7f  azure #00247f
 *   blue #00007f  violet #33007f  magenta #7e007f  rose #7f0017
 *
 * Anything not on this list is a guess until it has been through `emuctl leds`,
 * because rule 4 says a mix cannot be predicted. */
#define LAMP_HUES 12
extern const uint8_t k_lamp_wheel[LAMP_HUES][3];

/* ---- the control --------------------------------------------------------- */

/* What one pad should look like, or 0 when no feature claims it and the app's
 * own colour stands. [any] */
int lamp_pad(int pad, struct lamp *out);

/* Every pad's answer folded into one word, for a repaint gate: the transport
 * writes only when this moves. A 32-bit hash of eight (level, hue) pairs, so a
 * collision costs one skipped frame and the next one repaints. [any] */
uint32_t lamp_word(void);

/* Milliseconds off CLOCK_MONOTONIC. A blink or a sweep timed off a counter of
 * draws would run at whatever rate the deck is busy at. [any] */
uint32_t lamp_now_ms(void);

/* The transport has seen all eight lamps write, which is the whole path proven
 * end to end: the hook is in, the ordinals are mapped and the holders are
 * known. Idempotent -- only the first call means anything. [message] */
void lamp_panel_ready(void);

/* ---- internal to lamp/ --------------------------------------------------- */

void lamp_dance_start(void);              /* lamp.c, on lamp_panel_ready */
int  lamp_dance_ask(int pad, struct lamp *out);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_LAMP_H */
