// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/grid_read.c - reading a grid out of the deck's own objects.
 */
#include "stem/grid_internal.h"
#include "cue/cue.h"
#include "db/db.h"
#include "kit/mod.h"

int64_t grid_to_pool(int64_t raw, int pool_rate, int grid_rate)
{
    return (int64_t)((double)raw * (double)pool_rate / (double)grid_rate);
}

double grid_beat_bpm(uintptr_t beats, int i)
{
    double b = 0.0;

    if (mod_safe_read(beats + (uintptr_t)i * BEAT_STRIDE + BEAT_BPM_OFF,
                      &b, sizeof(b)) != 0)
        return 0.0;
    return b;
}

int grid_downbeat(uintptr_t beats, int count)
{
    (void)beats; (void)count;
    return 0;
}

int grid_from_holder(uintptr_t holder, double *spb_out,
                            int64_t *beat0_out, int *rate_out,
                            const char **why)
{
    uintptr_t content = 0, beats = 0;
    int64_t first = 0, last = 0, origin = 0, anchor = 0;
    int32_t count = 0, rate = 0;
    double spb, bpm;
    int db;

    *why = "no grid";
    if (mod_safe_read(holder + HOLDER_CONTENT_OFF, &content,
                      sizeof(content)) != 0 || !content)
        return 0;

    *why = "grid rate";
    if (mod_safe_read(holder + HOLDER_RATE_OFF, &rate, sizeof(rate)) != 0 ||
        rate <= 0)
        return 0;

    *why = "beat count";
    if (mod_safe_read(content + CONTENT_COUNT_OFF, &count, sizeof(count)) != 0 ||
        count < GRID_MIN_BEATS || count > GRID_MAX_BEATS)
        return 0;

    *why = "no beats";
    if (mod_safe_read(content + CONTENT_BEATS_OFF, &beats, sizeof(beats)) != 0 ||
        !beats)
        return 0;

    *why = "beat positions";
    if (mod_safe_read(beats, &first, sizeof(first)) != 0 ||
        mod_safe_read(beats + (uintptr_t)(count - 1) * BEAT_STRIDE, &last,
                      sizeof(last)) != 0 || last <= first)
        return 0;

    spb = (double)(last - first) / (double)(count - 1);

    /* Checked against the GRID's rate, which is the one number here that does
     * not depend on anything outside the grid. */
    *why = "tempo out of range";
    bpm = (double)rate * 60.0 / spb;
    if (!(bpm >= GRID_MIN_BPM) || !(bpm <= GRID_MAX_BPM))
        return 0;

    /* The anchor needs the origin the deck adds to every beat, which the tempo
     * above did not because it cancels in a difference. A missing origin is
     * zero, not a failure: the grid is still a grid. */
    if (mod_safe_read(holder + HOLDER_ORIGIN_OFF, &origin, sizeof(origin)) != 0)
        origin = 0;
    db = grid_downbeat(beats, count);
    if (mod_safe_read(beats + (uintptr_t)db * BEAT_STRIDE,
                      &anchor, sizeof(anchor)) != 0)
        anchor = first;
    *spb_out   = spb;
    *beat0_out = origin + anchor;
    *rate_out  = rate;

    /* The anchor longhand, because "in time but turned around" and "not in time"
     * are the same complaint from the floor and different numbers here. */
    MDBG("grid: origin %lld first %lld anchor %lld (beat %d of %d, cell BPM"
         " %.2f %.2f) rate %d -> %.1f BPM from the interval, beat0 %lld\n",
         (long long)origin, (long long)first, (long long)anchor, db, count,
         grid_beat_bpm(beats, 0), grid_beat_bpm(beats, count - 1),
         rate, bpm, (long long)*beat0_out);

    *why = NULL;
    return 1;
}

void grid_probe_sid(uintptr_t src)
{
    static int said;
    uint64_t lo = 0, hi = 0, a = 0, b = 0;
    unsigned off;

    if (said || !stem_source_id(&lo, &hi) || (!lo && !hi))
        return;
    for (off = 0; off + 16 <= GRID_SID_SCAN; off += 8) {
        if (mod_safe_read(src + off, &a, sizeof(a)) != 0 ||
            mod_safe_read(src + off + 8, &b, sizeof(b)) != 0)
            continue;
        if (a == lo && b == hi) {
            said = 1;
            MDBG("grid: source carries its sourceId at +%#x\n", off);
            return;
        }
    }
    said = 1;
    MDBG("grid: source does NOT carry the playing sourceId (%llx:%llx)"
         " in its first %#x\n", (unsigned long long)lo,
         (unsigned long long)hi, GRID_SID_SCAN);
}

double grid_scale(uintptr_t holder, int pool_rate, int64_t *beat0,
                         const char **why)
{
    double spb = 0.0;
    int64_t raw = 0;
    int rate = 0;

    if (pool_rate <= 0) {
        *why = "no pool rate";
        return 0.0;
    }
    if (!grid_from_holder(holder, &spb, &raw, &rate, why))
        return 0.0;
    *beat0 = grid_to_pool(raw, pool_rate, rate);
    return spb * (double)pool_rate / (double)rate;
}

struct grid_beats * grid_copy_beats(uintptr_t holder)
{
    uintptr_t content = 0, beats = 0;
    struct grid_beats *b;
    int64_t origin = 0;
    int32_t count = 0, i = 0;
    uint8_t *tmp;
    int rate = 0;

    if (mod_safe_read(holder + HOLDER_CONTENT_OFF, &content,
                      sizeof(content)) != 0 || !content ||
        mod_safe_read(holder + HOLDER_RATE_OFF, &rate, sizeof(rate)) != 0 ||
        rate <= 0 ||
        mod_safe_read(content + CONTENT_COUNT_OFF, &count, sizeof(count)) != 0 ||
        count < GRID_MIN_BEATS ||
        mod_safe_read(content + CONTENT_BEATS_OFF, &beats, sizeof(beats)) != 0 ||
        !beats)
        return NULL;

    if (count > GRID_BEATS_MAX) {
        MDBG("grid: %d beats is past the %d we keep -> the average instead\n",
             (int)count, GRID_BEATS_MAX);
        return NULL;
    }
    if (mod_safe_read(holder + HOLDER_ORIGIN_OFF, &origin, sizeof(origin)) != 0)
        origin = 0;

    /* ONE-SHOT OWNERSHIP PROBE, for the grid-edit work.
     *
     * Three questions decide whether a mod can hand the deck a DIFFERENT beat
     * array -- which is what a x2 needs, because doubling the beats needs 2N-1
     * cells where N exist:
     *
     *   what is BEFORE the array   an allocator header says who frees it. A
     *                              glibc chunk has a size at -8 with the low
     *                              bits as flags; a pool block looks nothing
     *                              like that, and freeing a malloc'd pointer
     *                              through a pool free is heap corruption on a
     *                              DJ's deck.
     *   what is AFTER the last cell the overrun guard the app asserts on
     *                              (Checker<Cell,int>::assertIfSignitureBroken).
     *                              A replacement array has to reproduce it.
     *   how much SLACK there is    if the allocation is rounded up far enough,
     *                              x2 can be done in place and the whole
     *                              ownership question never arises.
     *
     * Reads only, once, and only around an array the deck already holds. */
    {
        static int probed;

        if (!probed) {
            uint64_t before[4] = { 0, 0, 0, 0 }, after[6] = { 0, 0, 0, 0, 0, 0 };
            int k;

            probed = 1;
            for (k = 0; k < 4; k++)
                (void)mod_safe_read(beats - 32 + (uintptr_t)k * 8, &before[k],
                                    sizeof(before[k]));
            for (k = 0; k < 6; k++)
                (void)mod_safe_read(beats + (uintptr_t)count * BEAT_STRIDE +
                                    (uintptr_t)k * 8, &after[k], sizeof(after[k]));
            MDBG("grid: array %#lx, %d cells (%#lx bytes)\n",
                 (unsigned long)beats, (int)count,
                 (unsigned long)((uintptr_t)count * BEAT_STRIDE));
            MDBG("grid:  before -32..-1: %016llx %016llx %016llx %016llx\n",
                 (unsigned long long)before[0], (unsigned long long)before[1],
                 (unsigned long long)before[2], (unsigned long long)before[3]);
            MDBG("grid:  after  end+0..: %016llx %016llx %016llx %016llx"
                 " %016llx %016llx\n",
                 (unsigned long long)after[0], (unsigned long long)after[1],
                 (unsigned long long)after[2], (unsigned long long)after[3],
                 (unsigned long long)after[4], (unsigned long long)after[5]);

            /* THE WHOLE CELL, not just the position. A rescaled grid the deck
             * accepts but reads as 0.0 BPM means the eight bytes after the
             * position are not the nothing they looked like -- so print them
             * verbatim rather than inferring again. Plus the holder's own head,
             * in case a length or count lives there and has to move too. */
            for (k = 0; k < 4 && k < count; k++) {
                uint64_t c[2] = { 0, 0 };

                (void)mod_safe_read(beats + (uintptr_t)k * BEAT_STRIDE, &c[0],
                                    sizeof(c[0]));
                (void)mod_safe_read(beats + (uintptr_t)k * BEAT_STRIDE + 8,
                                    &c[1], sizeof(c[1]));
                MDBG("grid:  cell[%d] pos=%lld extra=%016llx\n", k,
                     (long long)c[0], (unsigned long long)c[1]);
            }
            for (k = 0; k < 12; k++) {
                uint64_t h = 0;

                (void)mod_safe_read(holder + (uintptr_t)k * 8, &h, sizeof(h));
                MDBG("grid:  holder+%#04x = %016llx\n", k * 8,
                     (unsigned long long)h);
            }

            /* AND THE BAR ARRAY, which is the other half of a Content and was
             * missing from every earlier reading of one. The PQTZ writer asks
             * sub_a1e670(content, i) for the beat's place in its bar, and that
             * reads an int32 array at content+0x38 with its count at +0x40:
             * bars[m] is the beat INDEX of the m'th downbeat, and the answer is
             * i minus the nearest one below. Rescaling the beats and leaving
             * this alone is why a saved x2 grid counted 1,2,3,4 to the end of
             * the original bars and then ran away as 5,6,7...
             *
             * Probed the same way the cell array was: what is in front of it
             * says who frees it, what is behind says which guard to reproduce. */
            {
                uintptr_t bars = 0;
                int32_t   nbar = 0, first = 0, last = 0;
                uint32_t  lo = 0, hi = 0;

                (void)mod_safe_read(content + CONTENT_BARS_OFF, &bars,
                                    sizeof(bars));
                (void)mod_safe_read(content + CONTENT_BARCNT_OFF, &nbar,
                                    sizeof(nbar));
                (void)mod_safe_read(bars - BAR_STRIDE, &lo, sizeof(lo));
                (void)mod_safe_read(bars + (uintptr_t)nbar * BAR_STRIDE, &hi,
                                    sizeof(hi));
                (void)mod_safe_read(bars, &first, sizeof(first));
                (void)mod_safe_read(bars + (uintptr_t)(nbar - 1) * BAR_STRIDE,
                                    &last, sizeof(last));
                MDBG("grid:  bars %#lx x %d, %d..%d, guards %08x %08x\n",
                     (unsigned long)bars, (int)nbar, (int)first, (int)last,
                     lo, hi);
            }
        }
    }

    tmp = malloc((size_t)GRID_BEATS_CHUNK * BEAT_STRIDE);
    b   = malloc(sizeof(*b) + (size_t)count * sizeof(b->pos[0]));
    if (!tmp || !b) {
        free(tmp);
        free(b);
        return NULL;
    }
    b->count  = count;
    b->rate   = rate;
    b->tid_lo = b->tid_hi = 0;

    while (i < count) {
        int32_t n = count - i < GRID_BEATS_CHUNK ? count - i : GRID_BEATS_CHUNK;
        int32_t k;

        if (mod_safe_read(beats + (uintptr_t)i * BEAT_STRIDE, tmp,
                          (size_t)n * BEAT_STRIDE) != 0) {
            free(tmp);
            free(b);
            return NULL;
        }
        for (k = 0; k < n; k++) {
            int64_t p;

            memcpy(&p, tmp + (size_t)k * BEAT_STRIDE, sizeof(p));
            p += origin;
            if (i + k > 0 && p < b->pos[i + k - 1]) {
                MDBG("grid: beat %d goes backwards -> the average instead\n",
                     (int)(i + k));
                free(tmp);
                free(b);
                return NULL;
            }
            b->pos[i + k] = p;
        }
        i += n;
    }
    free(tmp);
    return b;
}

void grid_hex(uintptr_t at, int n, char *out)
{
    static const char k_hex[] = "0123456789abcdef";
    uint8_t b;
    int i;

    for (i = 0; i < n; i++) {
        if (mod_safe_read(at + (uintptr_t)i, &b, sizeof(b)) != 0)
            b = 0;
        out[i * 2]     = k_hex[b >> 4];
        out[i * 2 + 1] = k_hex[b & 15];
    }
    out[n * 2] = '\0';
}
