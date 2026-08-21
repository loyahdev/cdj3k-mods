// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/audio_rate.c - the two clocks: the deck's pool rate and the stretcher's engine rate.
 */
#include "stem/audio_internal.h"

/* Every rate the deck's engine can be set to. A measured `posrate` is snapped to
 * whichever of these it is within 1 % of; anything else is refused rather than
 * rounded, because a rate we invented would put every stem out of alignment in a
 * way that looks like a bad separation rather than like a bug. */
static const int k_pool_rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };

/* How long stem_engine_rate_measure watches the stretcher for. */
#define ENGINE_SAMPLE_US (250 * 1000)
#include <math.h>
#include "xpad/ext.h"
#include "stem/loop.h"
#include "kit/mod.h"
#include "wave/wave.h"
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

int64_t stem_source_pos(void)
{
    int64_t p = __atomic_load_n(&g_src.pos, __ATOMIC_RELAXED);

    return p > 0 ? p : -1;
}

int stem_source_id(uint64_t *lo, uint64_t *hi)
{
    *lo = __atomic_load_n(&g_src.sid_lo, __ATOMIC_RELAXED);
    *hi = __atomic_load_n(&g_src.sid_hi, __ATOMIC_RELAXED);
    return *lo || *hi;
}

int snap_rate(uint64_t r)
{
    unsigned i;

    for (i = 0; i < sizeof(k_pool_rates) / sizeof(k_pool_rates[0]); i++) {
        int k = k_pool_rates[i];
        uint64_t tol = (uint64_t)k / 100;

        if (r + tol >= (uint64_t)k && r <= (uint64_t)k + tol)
            return k;
    }
    return 0;
}

int stem_engine_rate_measure(void)
{
    uint64_t a = stem_engine_frames(), b;
    int r;

    if (!a)
        return 0;
    usleep(ENGINE_SAMPLE_US);
    b = stem_engine_frames();
    r = snap_rate((b - a) * 1000000ull / ENGINE_SAMPLE_US);
    if (r && r != __atomic_load_n(&g_engine_rate, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_engine_rate, r, __ATOMIC_RELAXED);
        MDBG("stem_audio: engine rate %d Hz measured on demand\n", r);
    }
    return r;
}

int stem_pool_rate(void)
{
    int measured = __atomic_load_n(&g_pool_rate, __ATOMIC_RELAXED);
    int engine = __atomic_load_n(&g_engine_rate, __ATOMIC_RELAXED);

    /* Three sources, in descending order of how directly they observe the thing
     * being asked about.
     *
     * The POSITION measurement is the pool itself and wins whenever it exists,
     * but it needs playback -- a paused deck is not read at all -- so it never
     * answers for a track loaded and left at the cue point. It is also slow to
     * commit on this emulator, needing two consecutive agreeing windows against
     * a series like 22671, 14494, 83626, 96025, 53995.
     *
     * The STRETCHER runs on the same timeline and does not need playback, which
     * makes it the answer for everything the position measurement cannot reach.
     * A caller that needs one and finds none should ask for a fresh measurement
     * rather than settle for something else -- see stem_engine_rate_measure.
     *
     * There is deliberately no third source. /proc/asound was one, and it knows
     * the DAC's rate, which stops being the pool's the moment anything resamples
     * between them: measured on a deck whose DAC runs at 48000 while its pool
     * runs at 96000, it decoded a whole stem set at half rate.
     *
     * Answering before playback is safe only because the decode is held off the
     * track loader explicitly -- see wait_for_deck. */
    return measured ? measured : engine;
}

void pool_rate_observe(uint64_t rate)
{
    static int last_match;

    /* g_pool_rate, NOT stem_pool_rate(): that answers from the stretcher before
     * anything has played, and gating on it would stop this measurement ever
     * running -- and this measurement is what CHECKS the stretcher. */
    if (__atomic_load_n(&g_pool_rate, __ATOMIC_RELAXED))
        return;
    {
        int r = snap_rate(rate);

        if (!r) {
            last_match = 0;
            return;
        }
        if (last_match == r) {
            /* What the LOADED stems were decoded at -- asked of the store, not
             * inferred from whichever source would answer now. Inferring it is
             * how this check was silently disarmed: the engine rate landed a few
             * seconds after the decode had already run on the driver's figure,
             * so the comparison was 96000 against 96000 and agreed, while the
             * buffers underneath it were built at 48000. */
            int assumed = stem_store_rate();

            __atomic_store_n(&g_pool_rate, r, __ATOMIC_RELAXED);
            MDBG("stem_audio: pool rate %d Hz (measured %llu)\n",
                 r, (unsigned long long)rate);
            /* If the pool turns out to run at a different rate, the stems that
             * are loaded are stretched against the mix for the whole track --
             * which sounds like a bad separation, not like a bug, so it has to
             * be both loud and self-correcting. Drop the set and ask for it
             * again; the request now resolves the rate from this measurement. */
            if (assumed && assumed != r) {
                MDBG("stem_audio: ASSUMED %d Hz, POOL RUNS AT %d Hz"
                     " -> dropping the stems and reloading them\n", assumed, r);
                stem_track_gone();
                if (g_stems_on)
                    stem_job_request();
            }
        }
        last_match = r;
    }
}

uint64_t stem_engine_frames(void)
{
    /* The stretcher's own read first, the manager's operate as the fallback --
     * they see the same blocks, so either answers, and requiring both would make
     * the gate depend on two hooks where one is enough. */
    if (g_probe[PROBE_STRETCH].orig)
        return __atomic_load_n(&g_probe[PROBE_STRETCH].frames,
                               __ATOMIC_RELAXED);
    if (g_orig_operate)
        return __atomic_load_n(&g_op_probe.frames, __ATOMIC_RELAXED);
    return 0;
}
