// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/ext.h - the X-PAD SAMPLER as the rest of the shim sees it.
 *
 * Two entry points, both called from layers that are not the X-PAD, and a header
 * carrying nothing else so neither of them takes on juce or the panel's own
 * contract. Everything the feature does with itself is in xpad.h.
 */
#ifndef EP122_MOD_XPAD_EXT_H
#define EP122_MOD_XPAD_EXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* Add every sounding voice into `dst`, which holds `frames` of interleaved
 * stereo float at the pool rate, and which begins at track position `pos`.
 *
 * POST-STRETCH, which is the whole reason this mix point is not the stems'. The
 * stems are summed into the stretcher's INPUT so the deck warps them with the
 * track, which is right for a part of the track and wrong for a sample: a
 * one-shot keeps its own pitch and its own length whatever the tempo fader is
 * doing, and the X-PAD's Y axis is the only thing entitled to move either.
 *
 * `pos` IS THE BLOCK'S OWN PLACE IN THE TRACK and has to be passed in, because
 * the caller is the only one who knows it. Where the stretcher last READ its
 * source -- stem_source_pos() -- is a different place: measured on a playing
 * deck it runs 8055..9271 frames further on, which is 84 to 97 ms at 96 kHz.
 * Timing the sampler off that puts every quantized hit a tenth of a second in
 * front of the grid it was quantized to.
 *
 * [audio]. No allocation, no I/O, no locks, bounded loops. Returns immediately
 * when nothing is sounding, which is every block the panel is shut. */
void xpad_mix(float *dst, int64_t frames, int64_t pos);

/* Rescan mods/loops/ if the volume or the pool rate has moved. Cheap otherwise.
 * [worker] -- the shim's one idle worker, which is also the only thread allowed
 * to decode. */
void xpad_bank_poll(void);

/* [any] WHERE THE TRACK'S BEAT CLOCK IS, on the X-PAD's own reading of it --
 * the track's grid while the play head moves, the track's tempo when it does
 * not, so it keeps running on a parked deck. Returns 0 when there is no clock at
 * all, which a track with no grid is.
 *
 * A POSITION, not an edge. Compare the boundary it falls in against the one you
 * last saw and act when that changes; do not ask whether a boundary fell inside
 * some interval, because this is advanced post-stretch and every other caller
 * runs at its own cadence. See the note at the definition.
 *
 * Shared because it is the only clock on the audio path and two features want
 * it: the sampler's snapping and the stem row's mute. */
int xpad_beat_now(double *beat);

/* ENABLE X-PAD, the master gate. OFF by default, so a deck that never opts in
 * keeps its band slot and its full track title. Declared here as well as in
 * xpad.h so the settings record can persist it without taking on the panel's
 * whole contract. */
extern int xpad_g_on;

/* [any] The deck's own QUANTIZE as a divisor of a beat -- 1, 2, 4 or 8 -- or 0
 * when it is switched off. Declared here as well as in xpad.h so a caller that
 * only wants the gate does not have to take on the panel's whole contract. */
int xpad_quantize_div(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_XPAD_EXT_H */
