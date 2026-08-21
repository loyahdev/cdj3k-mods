// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/audio.c - eight voices, read at the pad's own rate and summed after the
 * stretcher.
 *
 * TWO CLOCKS, and keeping them apart is the whole design.
 *
 *   The VOICE runs free. Its playhead advances one frame per output frame
 *   whatever the deck is doing -- so a sample sounds on a paused deck, keeps its
 *   own length when the tempo fader moves, and is coloured only by the X-PAD's
 *   Y axis. That is what a sampler is.
 *
 *   The ROLL runs on the TRACK. Whether to re-trigger is asked of the track's
 *   own beat grid, so a 1/8 roll lands on the track's eighths however the tempo
 *   got there, and a paused deck simply has no boundaries to cross.
 *
 * A one-shot needs no BPM stated about it for either of those, which is why the
 * banks need no config file where GROOVE CIRCUIT's loops do.
 *
 * THE PITCH IS DURATION-PRESERVING, which is what the RMX-1000 does and is not
 * what a varispeed sampler does. The playhead runs at real time and the pitch is
 * taken out of two READ HEADS that wander back through the sample and wrap a
 * window at a time; the crossfade between them hides the wrap and the wraps are
 * what put the timeline back. A two-second sample is two seconds at any pitch.
 * See xp_heads for the mechanism and what it costs.
 *
 * NOTHING THAT GROWS ALL NIGHT REACHES A FLOAT. The playhead is a count of whole
 * frames, the wrap phase is a Q32 unsigned that wraps by overflowing, and the
 * only float in the read path is a delay bounded by one window. A float index
 * carrying a sub-sample fraction on top of a magnitude that grows with uptime is
 * catastrophic cancellation: the resolution is the float spacing at the index,
 * which is already a 256th of a sample after a third of a second at 96 kHz and
 * halves with every doubling of uptime. It comes up clean and decays into
 * aliasing for as long as the deck stays on. Measured.
 *
 * Threads. [audio] owns everything below the trigger line and may not allocate,
 * lock or log; [deck] fires and releases pads; [message] silences on close.
 */
#include "xpad/xpad.h"
#include "xpad/ext.h"
#include "stem/stem.h"
#include "stem/loop.h"       /* stem_beat_at: the grid, as a beat index */

#include <math.h>

/* ---- a voice --------------------------------------------------------------
 *
 * [audio] alone. The deck never touches one: it raises a bit in xpad_g_pend and
 * the mix starts the voice at the quantize boundary, so there is no handshake to
 * get wrong and a press cannot half-arrive. */
struct xp_voice {
    uint64_t pos;           /* the playhead: whole frames, one per output frame */
    uint32_t phi;           /* the wrap phase both heads hang off, Q32 */
    float    ratio;         /* THE BEND THIS SHOT IS PLAYING AT -- see the mix */
    float    win;           /* its own crossfade window, in frames */
    int32_t  start;         /* WHICH FRAME OF THIS BLOCK IT BEGINS ON */
    int      fresh;         /* started this block; the bend is not seeded yet */
    int      sounding;
};

static struct xp_voice xpad_g_voice[XP_BANKS];

/* "Stop everything", which is the one thing the trigger counter cannot say:
 * every value of it means fire. [message] sets, [audio] takes. */
static int xpad_g_hush;

/* WHAT REACHED THE OUTPUT, which is the only honest answer to "is it audible":
 * blocks our sum was added to, and the peak of OUR OWN contribution rather than
 * of the block, so a loud track cannot stand in for a sample that never played.
 * [audio] accumulates, [message] reads and clears. A lost update costs one
 * window and is not worth a lock on this path. */
static unsigned xpad_g_mix_blocks;
static float    xpad_g_mix_peak;
static unsigned xpad_g_mix_fires;
static unsigned xpad_g_fired;       /* [deck] side of the same count */

/* The one-pole the bend glides through, and the block it was set up for. The
 * glide is per voice; only the coefficient is shared, because it depends on the
 * block and the rate and on nothing a voice knows. */
static float    xpad_g_pole;
static int64_t  xpad_g_pole_len;
static int      xpad_g_pole_rate;

/* ---- the clock ------------------------------------------------------------
 *
 * THE TEMPO IS THE TRACK'S, THE TRANSPORT IS NOT. A sampler that stops when the
 * deck is paused is a sampler that cannot be played over a stopped record, and
 * the track's BPM is a property of the track rather than of whether it happens
 * to be moving. So the roll's clock runs whether or not the play head does.
 *
 * It has two sources and takes the better one every block:
 *
 *   THE TRACK, whenever the play head advanced. The beat position comes off the
 *   grid, so the roll sits on the track's own eighths however the tempo got
 *   there, and the position is absolute rather than accumulated, so it cannot
 *   drift.
 *
 *   OUR OWN, once the play head has been still long enough to mean stopped
 *   rather than between pulls: `frames / spb` at the track's stated tempo. 125
 *   BPM parked is 125 BPM.
 *
 * XP_STILL_BLOCKS is what separates the two, and it is deliberately short: a
 * burst gap is a handful of blocks and this is about twenty milliseconds, so a
 * roll on a parked deck starts imperceptibly late and a roll on a playing one is
 * never handed to the free-running branch.
 *
 * ONE HEAD PER POINT OF THE PIPELINE, and that is the whole reason there are
 * two of them. The sampler sums into what the stretcher has just WRITTEN; the
 * stems' mute edits what it is about to READ; and those are not the same place
 * in the track. Measured on a playing deck, the read runs 8055..9271 frames
 * ahead of the write -- 84 to 97 ms at 96 kHz, wandering by thirteen of them
 * from block to block. A single head shared between the two puts whichever it
 * does not belong to that far out, which is a roll landing a tenth of a second
 * off the grid it is quantized to. Each head is advanced by the position of the
 * audio its own caller is touching. */
#define XP_STILL_BLOCKS 30

struct xp_head {
    int64_t last_pos;       /* -1 before the first block */
    int     still;
    double  beat;           /* carried, so a parked deck goes on counting */
};

static struct xp_head xpad_g_out = { -1, 0, 0.0 };  /* what is being written */
static struct xp_head xpad_g_in  = { -1, 0, 0.0 };  /* what is being read    */

/* Which route the beat came from this block: 0 none, 1 the flat samples-per-
 * beat, 2 the track's own beat array. Reported, because a roll that plays one
 * hit and stops is a roll with no clock. */
static int xpad_g_clock_kind;

/* Presses waiting for the next block, one bit per bank. [deck] sets, [audio]
 * takes -- a bitmask rather than a queue because two presses of the same pad
 * inside one block are one hit, not two, and because it is the whole of the
 * handoff: the deck never touches a voice. */
static unsigned xpad_g_pend;

/* Which brick the roll last saw, so the moment a zone TAKES THE FINGER can be
 * told from the blocks after it. [audio] only; reset when the panel shuts. */
static int xpad_g_roll_div = XP_DIV_NONE;

/* The boundary an off-grid hit STOOD IN FOR, and whether it is still owed.
 * See xp_roll: a hit sounding a few milliseconds from a boundary has to take
 * that boundary's place rather than queue in front of it. */
static int64_t xpad_g_roll_claim;
static int     xpad_g_roll_claim_ok;

/* [audio] The hit that just sounded off-grid takes the nearest boundary of `L`
 * beats with it, so the roll does not play that one again a moment later.
 *
 * NEAREST, not the next one down: a hit lands either side of a boundary and it
 * is the nearer one it was aimed at. That is also what keeps the gap to the next
 * hit between half a division and one and a half, where rounding down leaves it
 * anywhere between nothing and one -- and nothing is the sound being fixed. */
static void xp_claim_boundary(double beat, double L)
{
    if (!(L > 0.0))
        return;
    xpad_g_roll_claim    = (int64_t)floor(beat / L + 0.5);
    xpad_g_roll_claim_ok = 1;
}

/* ---- firing (deck) -------------------------------------------------------- */

/* THE BLOCK BEING MIXED, so a shot can be placed INSIDE it rather than at its
 * edge. A block is 64 frames of the ALSA period, and starting every shot at
 * frame 0 rounds every boundary down to it. [audio] only, set once per mix. */
static int64_t xpad_g_blk_frames;
static double  xpad_g_blk_b0, xpad_g_blk_b1;

/* Which frame of this block a beat falls on. Outside the block means now, which
 * is what a hit the DJ just played wants. */
static int32_t xp_frame_of(double beat)
{
    double span = xpad_g_blk_b1 - xpad_g_blk_b0;
    double k;

    if (!(span > 0.0) || xpad_g_blk_frames <= 0)
        return 0;
    k = (beat - xpad_g_blk_b0) / span * (double)xpad_g_blk_frames;
    if (!(k > 0.0))
        return 0;
    if (k >= (double)xpad_g_blk_frames)
        return (int32_t)xpad_g_blk_frames - 1;
    return (int32_t)k;
}

int xpad_g_sel = -1;

/* SELECT AND FIRE, and that is the whole of what a pad does. The selection is
 * what the X-PAD then rolls; the shot is handed to the next block rather than
 * started here, because the deck thread does not own a voice.
 *
 * NOT QUANTIZED. A pad is a drum: what a finger asks for is the sound now, and a
 * hit held back to the next boundary is a hit the DJ did not play. The grid
 * belongs to the roll, which is a machine and is meant to be on it. There is no
 * release either: nothing was held. */
void xpad_fire(int bank)
{
    if (bank < 0 || bank >= XP_BANKS)
        return;
    __atomic_store_n(&xpad_g_sel, bank, __ATOMIC_RELAXED);
    __atomic_or_fetch(&xpad_g_pend, 1u << bank, __ATOMIC_RELAXED);
    __atomic_add_fetch(&xpad_g_fired, 1, __ATOMIC_RELAXED);
}

/* The bar's own trigger. NOT xpad_fire, and the difference is the whole of why
 * the sequencer is correct: that one is the DECK's entry point and does two
 * things the bar must not. It marks the pad HELD, which would leave a sequenced
 * hit rolling with no finger on it; and it goes through the trigger counter,
 * which is what the mix RECORDS from -- so the bar would record its own replay
 * and double its length every pass. Measured before this existed: 1, 2, 3, 4, 5
 * events on consecutive bars from two presses. */
void xpad_seq_fire_at(int bank, double beat)
{
    struct xp_voice *v;

    if (bank < 0 || bank >= XP_BANKS)
        return;
    v = &xpad_g_voice[bank];
    v->pos      = 0;
    v->phi      = 0;
    v->start    = xp_frame_of(beat);
    v->fresh    = 1;        /* the mix seeds the bend: the strip is read there */
    v->sounding = 1;
    __atomic_add_fetch(&xpad_g_mix_fires, 1, __ATOMIC_RELAXED);
}

void xpad_seq_fire(int bank)
{
    xpad_seq_fire_at(bank, xpad_g_blk_b0);
}

void xpad_silence(void)
{
    __atomic_store_n(&xpad_g_pend, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&xpad_g_hush, 1, __ATOMIC_RELEASE);
}

/* THE PANEL'S THREE STATES, and the X-PAD owns all eight while it is open.
 *
 * Green lit for the bank the X-PAD is on or one that is making a sound -- which
 * is what puts a sequenced hit on the pads as OVERDUB replays it, with no extra
 * bookkeeping: a voice is sounding whether a finger, the roll or the bar started
 * it. Green dim for a bank that is loaded and quiet. GREY for a pad with no
 * sample behind it.
 *
 * The grey matters more than it looks. Answering 0 for an empty pad hands it
 * back to the app, which lights it as a HOT CUE -- so an empty sample slot came
 * up wearing whatever colour that pad's cue happens to be, which reads as a
 * loaded bank of some other kind. While the panel is open the pads are sample
 * slots and every one of them says so, loaded or not.
 *
 * 0 only when the panel is shut, and that is exactly right: the pads go back to
 * being hot cues and their lamps back to the app's own. */

int xpad_pad_lamp(int pad, struct lamp *out)
{
    if (!xpad_open() || pad < 0 || pad >= XP_BANKS)
        return 0;

    if (!xpad_bank_ready(pad)) {
        /* WHITE on the dim level, which this panel renders as its own
         * near-neutral -- and which is the exact colour the deck puts on a pad
         * holding no cue, so an empty bank looks like an empty slot rather than
         * like anything of ours. */
        lamp_set(out, 255, 255, 255, LAMP_DIM);
        return 1;
    }
    lamp_set(out, 0, 255, 0,
             (pad == __atomic_load_n(&xpad_g_sel, __ATOMIC_RELAXED) ||
              xpad_voice_lit(pad)) ? LAMP_LIT : LAMP_DIM);
    return 1;
}

int xpad_voice_lit(int bank)
{
    if (bank < 0 || bank >= XP_BANKS)
        return 0;
    return __atomic_load_n(&xpad_g_voice[bank].sounding, __ATOMIC_RELAXED);
}

void xpad_mix_stat(struct xpad_mix_stat *out)
{
    out->blocks = __atomic_exchange_n(&xpad_g_mix_blocks, 0, __ATOMIC_RELAXED);
    out->fires  = __atomic_exchange_n(&xpad_g_mix_fires,  0, __ATOMIC_RELAXED);
    out->fired  = __atomic_exchange_n(&xpad_g_fired,      0, __ATOMIC_RELAXED);
    out->peak   = xpad_g_mix_peak;
    xpad_g_mix_peak = 0.0f;
    out->clock  = xpad_g_clock_kind;
    out->beat   = xpad_g_out.beat;
    out->span   = xpad_g_blk_b1 - xpad_g_blk_b0;
}

/* ---- the beat clock (audio) -----------------------------------------------
 *
 * The track's fractional beat index at a pool position, by whichever route the
 * track affords. The beat array is right for a track whose tempo moves or that
 * was gridded by hand; a single samples-per-beat is right while the tempo holds
 * still and is all an un-analysed grid can offer. Neither exists on a track with
 * no grid at all, and then there is no roll and no bar -- which is correct
 * rather than a fallback: a sampler with nothing to be in time with is a set of
 * one-shots, and that still works.
 *
 * `beats`/`count` are borrowed for the block by the caller, so this only reads. */
struct xp_clock {
    const int64_t *beats;
    int32_t        count;
    double         spb;         /* the flat route, 0 when the array is in use */
    int64_t        beat0;       /* ...and where its beat 0 is */
    int32_t        cursor;
};

/* THE FLAT ROUTE COUNTS FROM beat0, NOT FROM ZERO. Pool position 0 is the head
 * of the file and has nothing to do with the grid, so pos/spb numbers the beats
 * of a bar that starts wherever the track was cut -- every boundary off it sits
 * a constant beat0/spb from the one the DJ can see. Measured on a 148 BPM track
 * with beat0 at 4483: 0.1152 beat, 47 ms, which is a roll that never lands on
 * the needle. The array route has the origin built in, since its own cell 0 is
 * that beat. */
static double xp_beat_at(struct xp_clock *c, int64_t pos)
{
    if (c->beats)
        return stem_beat_at(c->beats, c->count, pos, &c->cursor);
    if (c->spb > 0.0)
        return ((double)pos - (double)c->beat0) / c->spb;
    return 0.0;
}

static int xp_clock_ok(const struct xp_clock *c)
{
    return c->beats != NULL || c->spb > 0.0;
}

/* Advance `h` to `pos` and hand back the span of beats THIS BLOCK COVERS.
 *
 * SELF-CONTAINED, and that is what makes a seek nothing to detect: the span is
 * [pos, pos + adv) where `adv` is how far the source moved for the last block --
 * the block's own length measured in the track, which is its output length
 * scaled by the tempo. A jump simply puts the span somewhere else, and nothing
 * is ever played across one because a span is never longer than a block.
 *
 * `adv` is clamped, because the difference between two positions is a seek as
 * readily as it is a block, and it falls back to the output length -- right at
 * 1.0x, and the tempo cannot have moved before the first block. */
static void xp_head_step(struct xp_head *h, struct xp_clock *k, int64_t pos,
                         int64_t frames, double *b0, double *b1)
{
    int64_t prev = h->last_pos;
    int64_t adv  = 0;

    if (pos >= 0) {
        if (prev >= 0 && pos > prev)
            adv = pos - prev;
        h->last_pos = pos;
    }
    if (adv <= 0 || adv > frames * 8)
        adv = frames;

    if (pos >= 0 && prev >= 0 && pos != prev) {
        h->still = 0;
        *b0 = xp_beat_at(k, pos);
        *b1 = xp_beat_at(k, pos + adv);
        if (!(*b1 > *b0))
            *b1 = *b0;          /* a grid that does not move here */
    } else if (h->still < XP_STILL_BLOCKS) {
        h->still++;             /* between pulls, not stopped */
        *b0 = h->beat;
        *b1 = *b0;
    } else if (k->spb > 0.0) {
        /* Parked: our own, carried on from where the track left it, so the
         * phase a stopped deck rolls at is still the phase it stopped on. */
        *b0 = h->beat;
        *b1 = *b0 + (double)frames / k->spb;
    } else {
        *b0 = h->beat;
        *b1 = *b0;
    }
    h->beat = *b1;
}

/* ---- the read heads ------------------------------------------------------- */

/* The crossfade window, recovered from the DSP image and exact. Computed inline
 * because the firmware computes it inline: there is no window table anywhere in
 * the flash.
 *
 * The two branches meet C0-continuously at x = 0.28348, where both evaluate to
 * 0.632145 = 1 - 1/e. The branch order is forced -- the quadratic form goes
 * negative past x = 0.5, so it can only be the lower segment. */
static inline float xp_xfade_window(float x)
{
    if (x < 0.28348f)
        return 18.16515f * x * x * (1.0f - 2.0f * x);
    {
        float u = 1.0f - x;
        return 1.0f - u * u * u;
    }
}

/* What one block's worth of pitch comes to. Built PER VOICE, because a voice
 * holds the bend it was released at and two of them can be at different ones. */
struct xp_pitch {
    float    w;             /* the crossfade window, in frames                  */
    float    wq;            /* ...times 2^-32, so a Q32 phase reads out in frames */
    float    half;          /* the anchor: half a window, in frames             */
    uint32_t step;          /* what the phase advances by per frame, wrapping    */
};

/* One head, `d` frames from the playhead, linearly interpolated. SIGNED: the
 * anchor puts half a window of `d` in front of the playhead -- see xp_heads.
 *
 * LINEAR AND NOTHING BETTER. The firmware has no higher-order kernel and its
 * position-dependent lowpass is part of the character; a sinc kernel here would
 * be a different effect that happened to have the same pitch.
 *
 * Off either end of the sample is SILENT rather than clamped. Before the start
 * there is nothing to hold, and holding the last frame past the end is a DC step
 * that stays until the voice ends. Both happen every time a voice plays: the
 * heads straddle the playhead by half a window either way, so the first half
 * window of a shot and the last are always partly off the end. */
static inline void xp_tap(const int16_t *pcm, int64_t frames, int64_t base,
                          float d, float *l, float *r)
{
    const float q = 1.0f / 32767.0f;
    int32_t  di = (int32_t)floorf(d);
    float    df = d - (float)di;
    int64_t  i  = base - di;
    float    a, b;

    if (i < 0 || i >= frames) {
        *l = 0.0f;
        *r = 0.0f;
        return;
    }
    /* THE FRAME BEFORE THE FIRST IS SILENCE, not out of range. Refusing i == 0
     * outright drops the first frame of every shot -- inaudible on anything that
     * fades in and a whole sample of level on anything gated, which a one-shot
     * usually is. Interpolating towards zero is also what is actually there. */
    if (i >= 1) {
        a  = (float)pcm[i * 2];
        b  = (float)pcm[(i - 1) * 2];
        *l = q * (a + df * (b - a));
        a  = (float)pcm[i * 2 + 1];
        b  = (float)pcm[(i - 1) * 2 + 1];
        *r = q * (a + df * (b - a));
    } else {
        *l = q * (1.0f - df) * (float)pcm[0];
        *r = q * (1.0f - df) * (float)pcm[1];
    }
}

/* BOTH HEADS, one frame, mixed. This is the whole pitch engine.
 *
 * The playhead advances at real time and never at the pitch ratio. What moves at
 * the ratio is the DELAY: head A sits `phi` of a window behind the playhead and
 * phi travels at (1 - r) / W, so the material coming out of head A advances at r
 * while the timeline it comes out on does not. Pitching up walks the delay down
 * to nothing and then wraps it back a whole window, which REPEATS a window of
 * material; pitching down walks it out to a window and wraps it to nothing,
 * which SKIPS one. Neither direction is special-cased -- both are which way the
 * delay was travelling when it hit the end.
 *
 * TWO HEADS HALF A PHASE APART, which is measured and not assumed. A head reads
 * any given point of the sample once per wrap, and consecutive reads of it land
 * W/r apart; the second head's reads fall halfway between, so copies of one
 * transient come out W/(2r) apart. A 4 ms burst comes back off the hardware at
 * full pitch as THREE copies 6.67 ms apart, and W/(2r) is 6.67 -- one head would
 * give 13.3 and only two of them. The half phase is free here: it is the top bit
 * of the phase word.
 *
 * The same halving is why the window is twice what the sidebands look like they
 * say. Two staggered heads wrap alternately, so the pattern repeats twice per
 * phase cycle and the comb spacing is 2|1 - r| / W, not |1 - r| / W.
 *
 * ANCHORED HALF A WINDOW FORWARD, which is what makes the engine INERT AT UNITY.
 * The loud head is always the one near the middle of its sweep -- a head is
 * silent at both ends of its own, so it cannot be loud at delay zero -- which
 * would put half a window of delay on every shot including the ones with no bend
 * on them at all. Subtracting that half from both delays makes the taps straddle
 * the playhead instead of trailing it: at unity, on a voice that has not been
 * bent, head B reads the playhead exactly and head A is silent, so the sample
 * comes out bit for bit and a one-shot lands where it was fired. With a bend the
 * onset wanders half a window either side of the beat rather than a whole window
 * behind it. The RMX's pitch is an insert on a running loop, where a fixed delay
 * is invisible; ours fires one-shots on a quantized grid, where it is not.
 *
 * THE GAINS ARE COMPLEMENTARY AND EACH HEAD IS SILENT AT ITS OWN WRAP. That is
 * what makes the wrap inaudible: only one head jumps at a time, it is at zero
 * gain when it does, and the other one is carrying the signal. The window is a
 * fade-in curve, so it is used as one -- over each half of the phase the head
 * that is about to wrap fades out by 1 - w while the other fades in by w, and
 * the two swap roles at the half. The heads come out identical to each other,
 * shifted by half a phase, which is what a two-head design means.
 *
 * WHAT THIS COSTS. Duration is preserved on AVERAGE, not per event: inside a
 * wrap the read runs at r, so a transient comes out scaled by 1/r and the wraps
 * put the timeline back around it. Pitching up gives every transient a triple
 * flam of compressed copies and pitching down stretches and thins it into one.
 * That is the character, measured on the hardware, and not an artefact to fix.
 * The hardware's own onsets move by 9 ms across the range for the same reason. */
static inline void xp_heads(const int16_t *pcm, int64_t frames, int64_t base,
                           uint32_t phi, const struct xp_pitch *p,
                           float *l, float *r)
{
    float ga, gb, al, ar, bl, br;

    xp_tap(pcm, frames, base, (float)phi * p->wq - p->half, &al, &ar);
    xp_tap(pcm, frames, base,
           (float)(phi + 0x80000000u) * p->wq - p->half, &bl, &br);

    ga = xp_xfade_window((float)(phi << 1) * (1.0f / 4294967296.0f));
    if (phi & 0x80000000u)
        ga = 1.0f - ga;
    gb = 1.0f - ga;

    *l = al * ga + bl * gb;
    *r = ar * ga + br * gb;
}

/* ---- the mix -------------------------------------------------------------- */

/* One block's worth of "what does the pad say". Read once so a finger moving
 * during the block cannot change the answer halfway down it. */
struct xp_gesture {
    int   div;              /* XP_DIV_NONE when nothing is engaged */
    float semis;
};

static void xp_gesture_read(struct xp_gesture *g)
{
    g->div   = XP_DIV_NONE;
    g->semis = 0.0f;
    if (!xpad_g_open)
        return;

    /* THE GESTURE IS ONLY LIVE WHILE A FINGER IS ON IT, unless HOLD is set --
     * which is the whole of what HOLD means. The strip leaves `div` and `semis`
     * where the finger left them so the readout can go on showing them; whether
     * they act is decided here.
     *
     * A FINGER OUTRANKS THE BAR. What OVERDUB recorded plays the strip only
     * while nobody is playing it: a DJ reaching for the pad is correcting the
     * loop, not competing with it. */
    if (xpad_gesture_live()) {
        g->div   = xpad_g_touch.div;
        g->semis = xpad_g_touch.semis;
        return;
    }
    xpad_seq_auto(&g->div, &g->semis);
}

/* The one-pole the bend glides through, as a per-block coefficient. Recomputed
 * only when the block or the rate moves, because the coefficient is
 * rate-dependent where the time constant is not. */
static float xp_pole(int64_t len, int rate)
{
    if (len == xpad_g_pole_len && rate == xpad_g_pole_rate)
        return xpad_g_pole;
    xpad_g_pole_len  = len;
    xpad_g_pole_rate = rate;
    xpad_g_pole = (rate > 0)
        ? expf(-((float)len / (float)rate) / (XP_PITCH_TAU_MS * 0.001f))
        : 0.0f;
    return xpad_g_pole;
}

/* The block's pitch: the window it wraps at, and the phase step that carries the
 * delay round it.
 *
 * THE WINDOW IS A CONSTANT PER DIRECTION and the two differ -- see
 * XP_XFADE_UP_MS. It GLIDES between them on the bend's own time constant,
 * because W scales both tap positions and stepping it would step both heads at
 * once. What that costs is a sweep through unity dragging the taps by up to half
 * the difference over one time constant, which is a fraction of a semitone
 * underneath a sweep that is already moving; the alternative is a click at every
 * crossing.
 *
 * The step is SIGNED and kept in the phase's own unsigned arithmetic, so the
 * wrap is an overflow in both directions and there is no modulo and no branch.
 * At unity it is zero, which leaves the whole wrap mechanism inert -- which is
 * what the hardware does: at centre the sine comes back with no comb at all. */
static void xp_pitch_block(struct xp_pitch *out, float *win, float ratio,
                           float a, int rate)
{
    float target = (ratio >= 1.0f ? XP_XFADE_UP_MS : XP_XFADE_DOWN_MS)
                 * 0.001f * (float)rate;

    if (!(*win > 0.0f))
        *win = target;                  /* a fresh shot, or after a rate change */
    *win = a * *win + (1.0f - a) * target;

    out->w    = *win;
    out->wq   = *win * (1.0f / 4294967296.0f);
    out->step = (uint32_t)(int32_t)((1.0 - (double)ratio) / (double)*win
                                    * 4294967296.0);
    /* THE ANCHOR IS FIXED, not half of the gliding window: its whole job is to
     * put the unbent read exactly on the playhead, and a moving anchor would
     * drag both taps every time the window did. Half of the UP window, because
     * that is the one unity resolves to. */
    out->half = XP_XFADE_UP_MS * 0.0005f * (float)rate;
}

/* Does a whole multiple of `L` beats fall in (b0, b1]?
 *
 * On the multiple's INDEX changing, so there is no modulo of a number that grows
 * all night: a track an hour in is the same arithmetic as one that just started.
 * Half-open at the low end, so a boundary landing exactly on a block edge fires
 * once rather than twice. */
static int xp_crossed(double b0, double b1, double L)
{
    if (!(b1 > b0) || !(L > 0.0))
        return 0;
    return floor(b1 / L) > floor(b0 / L);
}

/* WHERE THE CLOCK IS, published as a POSITION and not as an edge.
 *
 * The sampler's own snapping asks "did a boundary fall inside this block", which
 * is right for it: the same call advances the clock and consumes the answer. It
 * is wrong for anyone else. The pre-stretch mix runs at its own cadence -- the
 * stretcher pulls its source in bursts, so it is called neither once per block
 * nor in step with this one -- and on most blocks the play head has not moved,
 * so the span is empty and an edge test rejects it outright. A caller sampling
 * that waits for a coincidence of two unrelated clocks, which is a wait that can
 * simply never end.
 *
 * A position needs no coincidence: whoever reads it compares the boundary it
 * falls in against the one they last saw, and acts on the first call after it
 * changes, whenever that call happens to be. */
static double xpad_g_beat_pub;
static int    xpad_g_beat_pub_ok;

int xpad_beat_now(double *beat)
{
    if (!xpad_g_beat_pub_ok)
        return 0;
    *beat = xpad_g_beat_pub;
    return 1;
}

/* The roll re-triggers THE SELECTED BANK, which is the only one it could mean:
 * a pad is a selection, so what a finger on the strip repeats is whatever the
 * pads last picked. Through xpad_seq_fire for the same two reasons the bar uses
 * it -- nothing is held, and a roll is not something OVERDUB should record.
 *
 * THE FIRST HIT OF A ZONE IS IMMEDIATE, everything after it is on the grid. A
 * zone taking the finger is a hit the DJ just played and it sounds now; the
 * repeats are the machine and belong on the beat. Sliding into another zone is
 * the same event again, so a sweep across the bricks stutters under the hand
 * instead of waiting a boundary per zone.
 *
 * The repeats stay on the ABSOLUTE grid rather than counting from the first hit,
 * so a roll started late is still in time from its second hit onwards.
 *
 * AND THE FIRST HIT TAKES A BOUNDARY WITH IT. A finger landing a few
 * milliseconds before one fired twice inside that gap -- once for the touch and
 * once for the boundary -- which is not a flam anyone played, it is the control
 * stuttering under the hand. The immediate hit claims the nearest boundary and
 * the roll skips it, so whatever follows is a division away wherever inside the
 * division the finger came down. */
static void xp_roll(double b0, double b1, int div)
{
    int sel   = __atomic_load_n(&xpad_g_sel, __ATOMIC_RELAXED);
    int fresh = div != xpad_g_roll_div;
    double L;

    xpad_g_roll_div = div;
    if (sel < 0 || sel >= XP_BANKS || div < 0 || div >= XP_BRICKS)
        return;
    L = (double)xpad_div_beats[div];

    if (fresh) {
        xp_claim_boundary(b1, L);
        xpad_seq_fire(sel);
        return;
    }
    if (!xp_crossed(b0, b1, L))
        return;
    if (xpad_g_roll_claim_ok && (int64_t)floor(b1 / L) == xpad_g_roll_claim) {
        xpad_g_roll_claim_ok = 0;       /* the touch already played this one */
        return;
    }
    xpad_g_roll_claim_ok = 0;
    /* ON THE BOUNDARY ITSELF, which is a frame somewhere inside this block and
     * not its edge -- the whole point of a repeat is that it is on the grid. */
    xpad_seq_fire_at(sel, floor(b1 / L) * L);
}

/* The strip's own gesture, sampled onto XP_QUANTIZE_PAD while OVERDUB is armed,
 * so a sweep across the bricks becomes part of the loop. A quarter beat rather
 * than the pad's sixteenth -- see the note there for why a swept surface wants a
 * coarser grid than a button.
 *
 * ONLY WHEN IT HAS MOVED. A finger held still costs the bar nothing and a sweep
 * costs it one event per audible step, which is what keeps four beats of
 * automation inside a bar that also has to hold the hits. The LIFT is recorded
 * too -- as XP_DIV_NONE -- or the bar would hold the last value it saw for ever
 * and the strip would never come back to rest. */
static void xp_automate(double b0, double b1)
{
    static int   was_div = XP_DIV_NONE;
    static float was_semis;
    int   div   = xpad_g_touch.held ? xpad_g_touch.div : XP_DIV_NONE;
    float semis = xpad_g_touch.held ? xpad_g_touch.semis : 0.0f;
    float d;
    int   q;

    if (!xpad_g_overdub || !xpad_g_open) {
        was_div = XP_DIV_NONE;
        was_semis = 0.0f;
        return;
    }
    q = xpad_quantize_div();
    if (!q)
        q = XP_QUANTIZE_PAD;    /* quantize off: still needs a sampling rate */
    if (!xp_crossed(b0, b1, 1.0 / (double)q))
        return;

    d = semis - was_semis;
    if (d < 0.0f) d = -d;
    if (div == was_div && (div == XP_DIV_NONE || d < XP_AUTO_ST))
        return;
    was_div   = div;
    was_semis = semis;
    xpad_seq_record_pad(div, semis, b1);
}

/* The presses the deck handed over, started here because [audio] owns the
 * voices. Taken on the very next block: a pad is not quantized. */
static void xp_pending(double beat)
{
    unsigned pend, i;

    pend = __atomic_exchange_n(&xpad_g_pend, 0, __ATOMIC_RELAXED);
    if (!pend)
        return;
    /* A PAD PRESS TAKES A BOUNDARY TOO, for the same reason a touchdown does: a
     * press landing beside one while the roll is running is the same double in
     * the same gap. It claims against whatever brick the roll is on, which is
     * the grid the doubling would have happened against. */
    if (xpad_g_roll_div >= 0 && xpad_g_roll_div < XP_BRICKS)
        xp_claim_boundary(beat, (double)xpad_div_beats[xpad_g_roll_div]);

    for (i = 0; i < XP_BANKS; i++) {
        if (!(pend & (1u << i)))
            continue;
        xpad_seq_fire((int)i);
        /* Recorded HERE, at the moment it sounds, so the bar replays it at the
         * phase it was heard at rather than the one the press was posted at. */
        xpad_seq_record((int)i, beat);
    }
}

void xpad_mix(float *dst, int64_t frames, int64_t pos)
{
    struct stem_grid_view gv;
    struct xp_clock clk = { NULL, 0, 0.0, 0, -1 };
    struct xp_gesture g;
    float target, a, vol;
    int rate, i, engaged = 0, any = 0, have_beats = 0, mixed = 0, have_now = 0;
    double b0 = 0.0, b1 = 0.0, pub0 = 0.0, pub1 = 0.0;

    if (!dst || frames <= 0)
        return;

    if (__atomic_exchange_n(&xpad_g_hush, 0, __ATOMIC_ACQUIRE)) {
        for (i = 0; i < XP_BANKS; i++)
            xpad_g_voice[i].sounding = 0;
        xpad_g_roll_div = XP_DIV_NONE;   /* the next zone is a fresh one */
        xpad_g_roll_claim_ok = 0;
    }

    rate = stem_pool_rate();
    if (rate <= 0)
        return;

    /* THE CLOCK. See XP_STILL_BLOCKS: the track's own beat position while the
     * play head moves, our own at the track's tempo once it has stopped. It is
     * the TRACK'S beat index either way -- boundaries off it are the grid the DJ
     * is looking at, which is the whole point of quantizing to it.
     *
     * `pos` IS THIS BLOCK'S OWN PLACE IN THE TRACK, handed in by the caller
     * because only the caller knows it: the sampler sums into audio that has
     * already been through the stretcher, and where the stretcher last READ is
     * most of a tenth of a second further on. Advanced even when there is
     * nothing to mix, because it is a difference: skipping quiet blocks would
     * leave the first one after a press spanning however long the panel was
     * shut, and the bar would fire every event it holds at once. */
    clk.spb   = stem_grid_spb();
    clk.beat0 = stem_grid_beat0();
    if (stem_grid_beats_acquire(&gv)) {
        have_beats = 1;
        clk.beats  = gv.beats;
        clk.count  = gv.count;
    }
    xpad_g_clock_kind = clk.beats ? 2 : clk.spb > 0.0 ? 1 : 0;
    if (xp_clock_ok(&clk)) {
        xp_head_step(&xpad_g_out, &clk, pos, frames, &b0, &b1);
        /* AND THE OTHER END OF THE PIPELINE, for whoever edits the block the
         * stretcher is about to read rather than the one it just wrote. */
        xp_head_step(&xpad_g_in, &clk, stem_source_pos(), frames, &pub0, &pub1);
        have_now = 1;
    }
    xpad_g_blk_frames = frames;
    xpad_g_blk_b0     = b0;
    xpad_g_blk_b1     = b1;
    /* RELEASED THE MOMENT THE CLOCK IS READ, and not at the end of the mix.
     * Nothing below this line touches the array -- the rest of the block works
     * off b0 and b1 -- and holding it any longer is a deadlock waiting for a
     * track change: the writer takes the array out of the mix's reach and then
     * SPINS until every reader has gone, so one reader left behind by any
     * early-out below stalls whichever thread publishes. That thread is the
     * deck's, which is the one that answers the panel, so the symptom is the
     * whole front of the deck going dead while the screen carries on drawing. */
    if (have_beats) {
        stem_grid_beats_release();
        have_beats = 0;
    }

    /* Published BEFORE the sampler's own early-out, because the stem row's mute
     * rides this clock too and it must keep running whether or not the X-PAD is
     * doing anything. The IN head, because a mute is applied to the block the
     * stretcher is about to read. */
    xpad_g_beat_pub    = pub1;
    xpad_g_beat_pub_ok = have_now;

    /* Is anything to do at all? A voice sounding, a press waiting for its
     * boundary, a bar to replay, or a finger on the strip with a bank selected
     * -- the last because the roll needs the clock running before anything is
     * audible. */
    for (i = 0; i < XP_BANKS; i++)
        if (xpad_g_voice[i].sounding) {
            any = 1;
            break;
        }
    if (!any && !__atomic_load_n(&xpad_g_pend, __ATOMIC_RELAXED) &&
        xpad_seq_count() == 0 &&
        !(xpad_g_open && xpad_g_touch.div != XP_DIV_NONE))
        return;

    /* THE BAR FIRST, THEN THE ROLL, THEN THE FRESH PRESSES -- newest last, so a
     * press the DJ just made wins the boundary it shares with either.
     *
     * The gesture is read between the two, because the bar's own replay may have
     * just moved it: automation that arrived this block should roll this block. */
    if (b1 > b0)
        xpad_seq_play(b0, b1);
    xp_gesture_read(&g);

    /* WHERE THE STRIP IS, and whether it is being played at all. A voice takes
     * both in the loop below; nothing is glided here, because the glide belongs
     * to the voice and not to the control. */
    target  = exp2f(g.semis / 12.0f);
    engaged = g.div != XP_DIV_NONE;
    a       = xp_pole(frames, rate);

    /* NOT under the b1 > b0 guard, because a zone taking the finger fires on the
     * spot and the clock may not have moved this block. The crossing test inside
     * needs the span and gets it. */
    xp_roll(b0, b1, g.div);
    /* AFTER the replay, so a live finger's sample overwrites the automation the
     * bar just laid down rather than being overwritten by it. */
    if (b1 > b0)
        xp_automate(b0, b1);
    xp_pending(b1);

    vol = (float)__atomic_load_n(&xpad_g_vol, __ATOMIC_RELAXED) * 0.01f;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;

    for (i = 0; i < XP_BANKS; i++) {
        struct xp_voice *v = &xpad_g_voice[i];
        struct xpad_bank_view bv;
        struct xp_pitch pitch;
        float pk = xpad_g_mix_peak;
        int64_t k, tail;
        uint64_t ph;
        uint32_t phi;

        if (!v->sounding)
            continue;
        if (!xpad_bank_acquire(i, &bv)) {
            v->sounding = 0;        /* the stick went, or the table is rebuilding */
            continue;
        }

        /* A SHOT KEEPS THE BEND IT WAS PLAYED AT. The strip moves a voice only
         * while a finger is on it (or HOLD, or the bar); the moment it is let
         * go every sounding voice stays where it was left, and the shot rings
         * out at the pitch the DJ heard it start at. Sliding back to unity
         * instead would retune a tail nobody is touching any more.
         *
         * It costs nothing, because the next shot is seeded from the strip: a
         * pad hit with no finger down is unity whatever the last one froze at.
         *
         * A FRESH SHOT TAKES THE VALUE WHOLE and glides to nothing -- there is
         * no continuity to protect on a voice that has not sounded yet, and
         * gliding up to a bend would put the attack at the wrong pitch, which is
         * the part of a one-shot that carries it. */
        if (v->fresh) {
            v->fresh = 0;
            v->ratio = engaged ? target : 1.0f;
            v->win   = 0.0f;
        } else if (engaged) {
            v->ratio = a * v->ratio + (1.0f - a) * target;
        }
        xp_pitch_block(&pitch, &v->win, v->ratio, a, rate);

        /* THE PLAYHEAD RUNS PAST THE LAST FRAME by whatever the trailing head is
         * behind it, because the end of the sample is still coming out of that
         * head. Stopping at the last frame would cut the tail off every shot. */
        tail = bv.frames + (int64_t)(pitch.w - pitch.half) + 2;
        ph   = v->pos;
        phi  = v->phi;
        /* FROM THE FRAME THE BOUNDARY FELL ON, not from the block's edge. See
         * xp_frame_of: a block is 64 frames and rounding every hit down to its
         * start is 0.67 ms of the sampler being early, every time. */
        for (k = v->start; k < frames; k++) {
            float sl, sr, a;

            if ((int64_t)ph >= tail) {
                v->sounding = 0;
                break;
            }
            xp_heads(bv.pcm, bv.frames, (int64_t)ph, phi, &pitch, &sl, &sr);
            phi += pitch.step;
            ph++;
            sl *= vol;
            sr *= vol;
            dst[k * 2]     += sl;
            dst[k * 2 + 1] += sr;
            a = sl < 0.0f ? -sl : sl;
            if (a > pk) pk = a;
            a = sr < 0.0f ? -sr : sr;
            if (a > pk) pk = a;
        }
        v->pos   = ph;
        v->phi   = phi;
        v->start = 0;           /* the wait is this block's alone */
        xpad_bank_release();
        xpad_g_mix_peak = pk;
        mixed = 1;
    }
    if (mixed)
        __atomic_add_fetch(&xpad_g_mix_blocks, 1, __ATOMIC_RELAXED);
}
