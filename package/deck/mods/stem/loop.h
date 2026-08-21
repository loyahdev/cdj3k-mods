// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem/loop.h - where a looped file reads, as arithmetic and nothing else.
 *
 * Split out of the mix because it is the part that can be WRONG QUIETLY. A
 * negative remainder, an off-by-one at the wrap, or an index that lands exactly
 * on the span is a bad pointer on the audio thread, and the audio thread is
 * where a mistake is a crash or a dropout rather than a wrong pixel. Here it is
 * pure arithmetic over its own arguments, so tests/test_stem_loop.c can hold it
 * to the boundary cases without a deck.
 *
 * Track positions are pool-rate sample indices -- the timeline the stems were
 * decoded onto, which is also the one the cue table stores. File positions are
 * indices into the loop's own buffer, which is decoded at that same rate but
 * advances at its own tempo.
 */
#ifndef EP122_MODS_STEM_LOOP_H
#define EP122_MODS_STEM_LOOP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* One loop. `from` is the track position it was engaged at and `span` its length
 * in FILE frames; `span == 0` means there is no loop. */
struct stem_loop {
    int64_t from;
    int64_t span;
};

/* Where in the file track position `at` reads, as a fractional file index in
 * [0, span). The caller interpolates between floor(u) and its successor, so the
 * result being strictly below `span` is what keeps that successor in bounds.
 *
 * `ratio` is FILE FRAMES PER TRACK FRAME -- the loop's own tempo over the
 * track's. At 1.0 the file plays at its own speed; at 0.5 a beat of the file
 * takes two of the track's, which is what a 60 BPM loop does under a 120 BPM
 * track. The tempo fader needs no term of its own: `at` already advances with
 * it, so the loop follows.
 *
 * ONE RATIO IS ONE TEMPO. A track whose tempo moves has no single ratio, and
 * this drifts against it by however much the grid drifts -- see stem_beat_at,
 * which is the same phase measured in beats instead of frames. This stays for
 * the cases that have no grid to measure against at all.
 *
 * Phase comes from `from`, not from a cursor, so the loop sits exactly where it
 * would have if it had been running since it was engaged -- engaging, releasing
 * and re-engaging all land in time, a block boundary cannot slip it, and there
 * is nothing to drift. Defined for `at` before `from` as well: the loop is a
 * line extended in both directions, not something that starts when the play head
 * reaches it. */
double stem_loop_phase(const struct stem_loop *l, int64_t at, double ratio);

/* The same wrap on its own, for a caller that computed the file index some other
 * way. Answers in [0, span), and 0 for a span that is not a length. */
double stem_loop_wrap(double u, int64_t span);

/* The track's fractional BEAT INDEX at position `at`, over `count` ascending
 * beat positions on the track's own timeline.
 *
 * WHY THE LOOP WANTS THIS. A file index is beats-elapsed times the file's beat
 * length, and beats-elapsed is only (at - from) / spb while the track holds one
 * tempo. Measured off the grid instead, it is right for a track that speeds up,
 * slows down, or was gridded by hand a bar at a time -- the loop lands on beat
 * 129 when the track does, however the tempo got there.
 *
 * Whole numbers are beats: 4.5 is exactly halfway between beats[4] and beats[5],
 * so the fraction is measured against the interval it falls in and a tempo change
 * between two beats never moves the beats either side of it.
 *
 * DEFINED OUTSIDE THE GRID TOO, by extending the first and last intervals: a
 * track has audio before its first analysed beat, and the loop is a line rather
 * than something that starts where the analysis did. Answers 0 for anything that
 * is not an array of at least two beats, and drops the fraction across an
 * interval that is not positive rather than dividing by it.
 *
 * `cursor` is an optional hint, and passing NULL is the whole function. Give it
 * one per BLOCK, initialised to -1, and the interval containing `at` is found by
 * bisection once and then walked: a block spans a fraction of a beat, so every
 * frame after the first costs a compare instead of a log. That makes the beat
 * route cheaper per frame than the single-ratio one it replaces, which is a
 * division. The hint is never trusted -- it is checked, and a wrong one only
 * costs the bisection it was meant to save, so a seek, a reordered block or an
 * uninitialised cursor are all merely slower and never wrong. */
double stem_beat_at(const int64_t *beats, int32_t count, int64_t at,
                    int32_t *cursor);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_STEM_LOOP_H */
