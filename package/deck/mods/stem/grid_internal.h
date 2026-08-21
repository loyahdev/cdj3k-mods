/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/stem/grid_internal.h - what the grid files share.
 *
 * grid.c owns the reply cache and the beats table it publishes, grid_read.c
 * reads a grid out of the deck's own objects, grid_edit.c moves and rebuilds
 * one. stem.h is the outward face.
 */
#ifndef EP122_MOD_STEM_GRID_INTERNAL_H
#define EP122_MOD_STEM_GRID_INTERNAL_H

#include "stem/stem.h"

struct grid_beats {
    int32_t  count;
    int      rate;                      /* what `pos` is counted at */
    uint64_t tid_lo, tid_hi;            /* which track, as the reply named it */
    int64_t  pos[];                     /* ascending, origin already added */
};

/* The deck's Content and beat-cell layout. */
/* Every meow::RefCountedObjEx carries this at +0x10 and its count at +0x0c --
 * the app checks it on every retain, and the assert it fires is
 * "sig == RefCountedObjExSig". */
#define OBJ_SIG_OFF         0x10
#define OBJ_SIG             0x52434f58      /* 'XOCR' */
/* The chain, as above. */
#define PWSI_SOURCE_OFF     0x28
#define SRC_HOLDER_OFF      0x68
#define HOLDER_CONTENT_OFF  0x28
#define HOLDER_RATE_OFF     0x40
#define HOLDER_ORIGIN_OFF   0x30
#define CONTENT_BEATS_OFF   0x28
#define CONTENT_COUNT_OFF   0x30
/* THE OTHER ARRAY. A Content is two: the beats, and the BARS -- an int32 per
 * downbeat holding that downbeat's BEAT INDEX, with its own count.
 *
 * Nothing reads it here, and for a long time nothing knew it was there. What
 * found it was a saved grid: the deck writes each beat's place in its bar into
 * the analysis file, and it gets that place from sub_a1e670(content, i), which
 * subtracts the nearest bar index at or below i. Rescale the beats alone and
 * that answer is right until the last old bar and then counts away from it --
 * 1,2,3,4 to beat 847 and then 5,6,7 to the end, measured.
 *
 * Regular by construction on every grid seen: bars[0] < 4 (the deck's own reader
 * assumes the first downbeat is inside the first bar) and a step of 4 after. */
#define CONTENT_BARS_OFF    0x38
#define CONTENT_BARCNT_OFF  0x40
#define BAR_STRIDE          4
#define BEATS_PER_BAR       4
/* One beat: an int64 POSITION and then a DOUBLE, which is that beat's BPM.
 *
 * This was twice recorded as "eight bytes that read as zero", and twice that was
 * an artefact of how it was read: an int32 at +8 is the LOW half of a
 * little-endian double, and for a round tempo the low half is exactly zero.
 * 125.0 stores as 405f4000_00000000, whose bottom four bytes are 0. Measured
 * properly -- printing all eight -- every cell of a 125.0 track holds 125.0.
 *
 * It matters: the play-screen BPM comes from HERE, not from the interval, so a
 * rescaled grid that leaves this field alone reads as 0.0 BPM on the deck. A
 * per-beat tempo is also the natural place for it, because that is what lets a
 * grid describe a track whose tempo moves. */
#define BEAT_STRIDE         0x10
#define BEAT_BPM_OFF        0x08
#define BEAT_PER_BAR        4
/* Where a cue slot keeps its PositionWithSourceInfo. Same offset preview.c
 * copies one from. */
#define SLOT_PWSI_OFF       0x28
/* What a grid has to look like to be believed. The beat count is bounded on
 * both sides because one beat cannot give a length and a hundred million is a
 * wild pointer that happened to be readable. */
#define GRID_MIN_BEATS      2
#define GRID_MAX_BEATS      1000000
#define GRID_MIN_BPM        20.0
#define GRID_MAX_BPM        400.0
/* The highest CueKind worth asking: the eight hot cues, the memory cue, and the
 * preview needle at 9, which is the last kind cue.h accounts for.
 *
 * ASK ALL OF THEM, because which one answers is not a property of the track the
 * DJ can see. Measured: one track answered on kind 0 with every hot cue silent,
 * another answered on kinds 1-3 with kind 0 silent -- including a kind whose cue
 * is set and visible on the overview. A slot holds a source once something has
 * put one there, and nothing about the cue says whether that has happened. */
#define GRID_KIND_MAX       9
/* Reception::replyBeatGridRequest, slot 3. */
#define GRID_RECEPTION_SLOT 0x18
/* ---- the whole grid, for a track whose tempo moves ------------------------
 *
 * The average beat above is one number for a track that has one tempo. Every
 * beat, kept as an array, is what a loop needs to stay in time under a track
 * that does not -- see stem_beat_at, which turns a position into a fractional
 * beat index against it.
 *
 * COPIED, NEVER POINTED AT. The deck's own array belongs to an object whose
 * lifetime we do not control, and reading it from the audio thread would be
 * trusting a pointer for the length of a track rather than for the length of one
 * read. It is a few kilobytes -- a beat is eight bytes and a long track has a
 * couple of thousand -- so the copy costs less than the reasoning would.
 *
 * COPIED WHEN THE REPLY LANDS, which is the one moment the object is certainly
 * alive: the deck is in the middle of handing it over. That is [message], and a
 * pad press is [deck], so the two are joined by a single atomic exchange -- the
 * reply leaves an array behind, the next arm takes it, and whichever thread ends
 * up holding one nobody took is the one that frees it. */
#define GRID_BEATS_MAX      32768       /* ~4 h at 128 BPM; 256 kB of int64 */
#define GRID_BEATS_CHUNK    512         /* beats a mod_safe_read at a time    */
/* A FEW OF THEM, KEPT BY TRACK, not one "last reply".
 *
 * One slot is enough only while every load produces a reply, and loading does
 * not: the deck answers when it has to READ a grid, so a track it has already
 * read is silent when it comes back. Measured -- Chemistry loaded, replaced by
 * Chase The Sun, then loaded again: `track change` with no reply behind it. With
 * a single slot the cached grid is the other track's, the ids do not match, and
 * nothing arms for the rest of that load. No clock at all, which is a roll
 * playing one hit per touch and a stems mute landing wherever it likes.
 *
 * SCALARS ONLY -- samples per beat, the rate they are counted at, and beat 0 --
 * so nothing kept here points at an object whose lifetime we do not own. The
 * beat ARRAY is not cached that way and a returning track gets the flat route,
 * which is what a constant tempo needs and is what the cue walk would have
 * given it anyway.
 *
 * Written by [message] alone, on the reply; read from [deck] as well, so each
 * slot publishes its id last and a reader re-checks it after taking the
 * scalars. A slot is only ever rewritten eight tracks later. */
#define GRID_REPLY_SLOTS 16
/* Grid units to pool samples. ONE expression, used for the beat array and for
 * the anchor alike: the anchor IS one of the beats, and the two have to land on
 * the same sample or the loop starts a fraction of a beat off its own downbeat.
 * Two spellings of the same arithmetic do not guarantee that in floating point;
 * one does. */
/* That beat's own BPM. The field the bar-count guess used to read, correctly
 * this time: a double, not an int32. */
/* The grid's anchor is its FIRST BEAT.
 *
 * The cell's second field is a BPM, not a bar number, and nothing in the cell
 * says which beat starts a bar. If the deck stores a bar anywhere, it is not
 * here, so there is nothing to search and the anchor is beat 0. */
/* One grid holder -- meow::UpdatableSharedObject<appnd_trk_info::BeatGrid> --
 * reduced to a beat length and a downbeat, or 0 if anything about it does not
 * hold up.
 *
 * SPLIT FROM THE WALK ABOVE IT because there are two ways to a holder and only
 * one way to read it: a cue slot names one through its source, and the deck's
 * own beat-grid reply carries one directly. Everything from here down is the
 * same arithmetic either way, so it is written once. */
/* IN THE GRID'S OWN UNITS, not the pool's. The deck answers a grid request while
 * the track is loading, which is BEFORE the audio path has a pool rate at all --
 * measured, `pool rate 0` -- so a reading that needs one cannot be taken when the
 * answer arrives. The grid carries the rate its own positions are counted at, and
 * that is enough to check the tempo is a tempo; converting to pool samples is the
 * caller's, at the point where a rate exists. */
/* Where in a page source its OWN sourceId sits, if it is there at all.
 *
 * THE QUESTION THIS ANSWERS: is the source a cue slot names still the LOADED
 * track's? A slot keeps whatever was last put in it, so it can name the track
 * before -- and nothing about the slot says so. The audio thread knows which
 * source it is reading, by id, on every block. If the source object carries that
 * id, the walk can check its own answer instead of trusting it.
 *
 * A probe: scans the object's head for the pair the audio path reports and says
 * where it found them. Once per arm, and only while the offset is unknown. */
#define GRID_SID_SCAN       0x200
/* ---- the deck's own beat-grid reply ---------------------------------------
 *
 * trackinfo_stocker::BeatGridRequestHandler::Reception::replyBeatGridRequest,
 * the one route to a grid that does not go through a cue slot. A track with no
 * cues has no reachable source, so the walk above finds nothing on it at all --
 * measured live on a deliberately cleared track, all ten kinds silent.
 *
 * A PROBE FOR NOW. Two things are unverified and both are one log line away:
 * whether the SharedBeatGridPtr arrives as the holder or as a pointer to it, and
 * whether the TrackID that comes with it is the LOADED track's or a browsed
 * one's -- the waveform Reception in this same family is keyed on the browse
 * request and fires for tracks nobody loaded. Nothing is published from here
 * until that is answered; taking whatever arrives would phase the loop to a
 * track the DJ only scrolled past. */
#define GRID_TID_BYTES      24
/* The red zones, as the deck writes them: ONE ELEMENT WIDE, either side.
 * Sixteen bytes around the 16-byte cells, four around the 4-byte bar indices --
 * measured on both, and the chunk header in front sizes exactly for
 * guard + n*stride + guard in each case. */
#define GRID_RED_LO         0xAF
#define GRID_RED_HI         0xEF
/* A rescale past this is a mis-tap, not a grid. */
#define GRID_EDIT_K_MIN     0.125
#define GRID_EDIT_K_MAX     8.0
/* Slot 0x50 of ITrackInfoRepositoryCache:
 *
 *   bool registerBeatGrid(const AsyncCommand::RequestID &, const TrackID &,
 *                         const appnd_trk_info::BeatGrid &,
 *                         ListenerReference<ICachedBeatGridRegistListener>,
 *                         AsyncCommand::Priority)
 *
 * Taken from the LIVE object's own vtable and checked against the class's, which
 * is the same identity rule the panel uses for a Component. */
#define GRID_TIR_REGISTER   0x50
/* The BeatGrid VALUE, which is not the holder. The holder is
 * meow::UpdatableSharedObject<BeatGrid> and the value sits inside it at +0x28:
 * 0x20 bytes of { Content *content; int64 origin; int64; int32 rate; int16
 * offset; }. Every offset this file already reads -- content +0x28, origin
 * +0x30, rate +0x40 -- falls where it should, and the deck's own copy of the
 * same struct (it builds one at every offset register) reads exactly these five
 * fields from exactly these places.
 *
 * COPIED WHOLE rather than rebuilt field by field, so the two words we have no
 * name for travel too. */
#define GRID_VALUE_OFF      0x28
#define GRID_VALUE_BYTES    0x20
/* AsyncCommand::Priority, as the deck passes it for a grid write. */
#define GRID_SAVE_PRIO      2
/* meow::ObjectMap's key: a hash of the object's NAME.
 *
 *     h(s) = (s[0] + 0x89 * h(s+1)) % M,  h("") = 0
 *
 * Reproduced rather than called because the app's own copy is a constexpr helper
 * with no signature worth matching, and it is 64-bit arithmetic either way -- so
 * the same expression in the same order gives the same word, overflow included.
 * Nothing rests on that being right: a wrong hash resolves to another object or
 * to none, and the vtable check below refuses both. */
#define GRID_MAP_MUL        0x89ULL
#define GRID_MAP_MOD        0x1de5d6e3f8868a2ULL
#define GRID_CACHE_NAME     "TrackInfoRepositoryCachePtr"

/* The loaded track's holder, and the original grid kept for a reset.
 * grid.c owns them; grid_edit.c restores from them. */
extern uintptr_t grid_g_holder;
extern uintptr_t grid_g_orig_content;
extern uintptr_t grid_g_orig_beats;
extern int32_t   grid_g_orig_count;
extern uintptr_t grid_g_orig_bars;
extern int32_t   grid_g_orig_barcnt;

int64_t grid_to_pool(int64_t raw, int pool_rate, int grid_rate);
double grid_beat_bpm(uintptr_t beats, int i);
int grid_downbeat(uintptr_t beats, int count);
int grid_from_holder(uintptr_t holder, double *spb_out, int64_t *beat0_out, int *rate_out, const char **why);
void grid_probe_sid(uintptr_t src);
double grid_scale(uintptr_t holder, int pool_rate, int64_t *beat0, const char **why);
struct grid_beats * grid_copy_beats(uintptr_t holder);
void grid_hex(uintptr_t at, int n, char *out);

/* Declared before the typedef that points at it: naming the struct first inside a
 * prototype scopes it to that prototype, and the definition further down is then
 * a DIFFERENT type to a compiler that enforces it. */
struct grid_mapped_ptr;

typedef int  (*grid_link_fn)(struct grid_mapped_ptr *);
typedef void (*grid_reqid_fn)(void *);
typedef int  (*grid_register_fn)(uintptr_t cache, const void *req,
                                 const void *tid, const void *grid,
                                 const void *listener, int prio);



struct grid_mapped_ptr {
    uintptr_t obj;
    uint64_t  id;
};
struct grid_listener_ref {
    int32_t   tag;
    uintptr_t iface;
    uintptr_t owner;
};

/* The track the holder belongs to, cleared when a grid is edited
 * but not saved. */
extern uint64_t grid_g_holder_tid_lo, grid_g_holder_tid_hi;

#endif /* EP122_MOD_STEM_GRID_INTERNAL_H */
