// SPDX-License-Identifier: MIT OR Apache-2.0
/* Source-built OverCue playback. One worker owns files and buffers; the audio
 * hook only reads an immutable, source-bound set. No recovered stems calls. */
#include "stem/stem.h"
#include "stem/prestem_media.h"
#include "stem/mode.h"
#include "wave/wave.h"
#include "kit/band.h"
#include "kit/mod.h"
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

struct pst_set {
    struct pst_media media;
    uint64_t lo, hi, token;
};
static struct pst_set *g_set;
static unsigned g_users;
static uint64_t g_generation = 1;
static unsigned g_selection = 3;
static int g_enabled, g_worker_up;
static int g_loading; /* g_lock: accepts queued selections until validation finishes. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_path[PST_PATH_MAX], g_status[32] = "OFF";
static uint64_t g_lo, g_hi;
static unsigned long long g_blocks, g_misses, g_routed[4];
static int g_audio_rate, g_ui_attached, g_ui_open;
static uint32_t g_wave_live[3], g_wave_export[3], g_wave_state[3] = {4, 4, 4};
static void report_write(const char *path, const char *text)
{
    size_t left = strlen(text);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        return;
    while (left) {
        ssize_t n = write(fd, text, left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        text += n;
        left -= (size_t)n;
    }
    close(fd);
}
/* A bounded diagnostic snapshot, written by the file worker only. No remounts.
 * The USB copy lets a hardware test return evidence without SSH or a second UPD. */
static void report(void)
{
    static time_t last;
    static unsigned writes;
    static uint64_t token;
    time_t now = time(NULL);
    char text[1024], path[PST_PATH_MAX], track[PST_PATH_MAX], status[32];
    uint64_t gen, lo, hi, load_ms = 0, read_ms = 0, hash_ms = 0;
    unsigned state;
    int on, ready, n, paged = 0;
    const char *contents;
    if (now - last < 5)
        return;
    last = now;
    pthread_mutex_lock(&g_lock);
    gen = __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
    if (token != gen) {
        token = gen;
        writes = 0;
    }
    if (writes >= 60) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    snprintf(track, sizeof(track), "%s", g_path);
    snprintf(status, sizeof(status), "%s", g_status);
    lo = g_lo;
    hi = g_hi;
    on = g_enabled;
    ready = g_set && g_set->token == gen;
    state = __atomic_load_n(&g_selection, __ATOMIC_ACQUIRE);
    if (ready) {
        paged = g_set->media.pages != NULL;
        ready = pst_media_ready(&g_set->media);
        if (!ready && !strcmp(status, "READY")) snprintf(status, sizeof(status), "LOADING");
        load_ms = g_set->media.load_ms;
        read_ms = g_set->media.read_ms;
        hash_ms = g_set->media.hash_ms;
    }
    pthread_mutex_unlock(&g_lock);
    n = snprintf(
        text, sizeof(text),
        "engine=source-prestems-paged-v13\nstatus=%s\nenabled=%d\nready=%d\n"
        "track_id=%u\nsource=%llx:%llx\ngeneration=%llu\nselection=%u\n"
        "ui_attached=%d\npanel_open=%d\npool_rate=%d\n"
        "accepted_blocks=%llu\nfallback_blocks=%llu\n"
        "muted_blocks=%llu\nvocal_blocks=%llu\ninstrumental_blocks=%llu\nfull_blocks=%llu\n"
        "load_ms=%llu\nread_ms=%llu\nhash_ms=%llu\nsha2_accelerated=%d\npaged=%d\n"
        "wave_3band_live=%u\nwave_3band_export=%u\nwave_3band_state=%u\n"
        "wave_rgb_live=%u\nwave_rgb_export=%u\nwave_rgb_state=%u\n"
        "wave_blue_live=%u\nwave_blue_export=%u\nwave_blue_state=%u\n",
        status, on, ready, (unsigned)hi, (unsigned long long)lo, (unsigned long long)hi,
        (unsigned long long)gen, state, __atomic_load_n(&g_ui_attached, __ATOMIC_RELAXED),
        __atomic_load_n(&g_ui_open, __ATOMIC_RELAXED),
        __atomic_load_n(&g_audio_rate, __ATOMIC_RELAXED),
        __atomic_load_n(&g_blocks, __ATOMIC_RELAXED), __atomic_load_n(&g_misses, __ATOMIC_RELAXED),
        __atomic_load_n(&g_routed[0], __ATOMIC_RELAXED),
        __atomic_load_n(&g_routed[1], __ATOMIC_RELAXED),
        __atomic_load_n(&g_routed[2], __ATOMIC_RELAXED),
        __atomic_load_n(&g_routed[3], __ATOMIC_RELAXED), (unsigned long long)load_ms,
        (unsigned long long)read_ms, (unsigned long long)hash_ms, pst_sha256_accelerated(), paged,
        __atomic_load_n(&g_wave_live[0], __ATOMIC_RELAXED),
        __atomic_load_n(&g_wave_export[0], __ATOMIC_RELAXED),
        __atomic_load_n(&g_wave_state[0], __ATOMIC_RELAXED),
        __atomic_load_n(&g_wave_live[1], __ATOMIC_RELAXED),
        __atomic_load_n(&g_wave_export[1], __ATOMIC_RELAXED),
        __atomic_load_n(&g_wave_state[1], __ATOMIC_RELAXED),
        __atomic_load_n(&g_wave_live[2], __ATOMIC_RELAXED),
        __atomic_load_n(&g_wave_export[2], __ATOMIC_RELAXED),
        __atomic_load_n(&g_wave_state[2], __ATOMIC_RELAXED));
    report_write("/tmp/prestems-source-status.txt", text);
    contents = strstr(track, "/Contents/");
    if (contents && contents > track) {
        n = snprintf(path, sizeof(path), "%.*s/CDJMODS/prestems-source-status.txt",
                     (int)(contents - track), track);
        if (n > 0 && n < (int)sizeof(path)) {
            report_write(path, text);
        }
    }
    writes++;
}
void prestem_ui_observed(int attached, int open)
{
    __atomic_store_n(&g_ui_attached, attached, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ui_open, open, __ATOMIC_RELAXED);
}

/* Legacy v7 exclusion gates now all allow the shared source implementation. */
int prestem_native_owner_active(void)
{
    return 0;
}
static void retire(void)
{
    struct pst_set *old;
    pthread_mutex_lock(&g_lock);
    old = __atomic_exchange_n(&g_set, NULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_lock);
    if (!old)
        return;
    while (__atomic_load_n(&g_users, __ATOMIC_SEQ_CST))
        usleep(1000);
    pst_media_free(&old->media);
    free(old);
}
static int cancelled(void *token)
{
    return *(uint64_t *)token != __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
}
static void *worker(void *unused)
{
    uint64_t seen = 0;
    (void)unused;
    for (;;) {
        uint64_t gen = __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
        struct pst_set *set;
        char path[PST_PATH_MAX], status[32];
        int on, rc;
        if (gen == seen) {
            struct pst_set *active = __atomic_load_n(&g_set, __ATOMIC_SEQ_CST);
            int serviced = active ? pst_media_service(&active->media, cancelled, &gen) : 0;
            if (serviced < 0 && !cancelled(&gen)) {
                pthread_mutex_lock(&g_lock);
                snprintf(g_status, sizeof(g_status), "STEM READ ERROR");
                pthread_mutex_unlock(&g_lock);
            }
            report();
            if (serviced > 0) continue;
            usleep(50000);
            continue;
        }
        retire();
        set = calloc(1, sizeof(*set));
        pthread_mutex_lock(&g_lock);
        gen = __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
        seen = gen;
        on = g_enabled;
        snprintf(path, sizeof(path), "%s", g_path);
        if (set) {
            set->lo = g_lo;
            set->hi = g_hi;
            set->token = gen;
        }
        g_loading = on && path[0] && set;
        snprintf(g_status, sizeof(g_status), "%s",
                 !on        ? "OFF"
                 : !path[0] ? "LOAD TRACK"
                 : !set     ? "NO MEMORY"
                            : "LOADING");
        pthread_mutex_unlock(&g_lock);
        if (!on || !path[0] || !set) {
            free(set);
            continue;
        }
        rc = pst_media_load(path, (uint32_t)set->hi, &set->media, cancelled, &gen, status,
                            sizeof(status));
        pthread_mutex_lock(&g_lock);
        if (!cancelled(&gen)) {
            g_loading = 0;
            snprintf(g_status, sizeof(g_status), "%s", status);
            if (!rc) {
                __atomic_store_n(&g_set, set, __ATOMIC_SEQ_CST);
                set = NULL;
            }
            MINFO("prestem: %s source=%llx:%llx generation=%llu\n", status,
                  (unsigned long long)g_lo, (unsigned long long)g_hi, (unsigned long long)gen);
        }
        pthread_mutex_unlock(&g_lock);
        if (set) {
            pst_media_free(&set->media);
            free(set);
        }
    }
    return NULL;
}
/* Called from the existing display timer, including while the panel is shut. */
void prestem_tick(void)
{
    uint64_t lo = 0, hi = 0;
    const char *path = NULL;
    if (!g_worker_up)
        return;
    if (g_prestems_on && !g_stems_on && stem_source_id(&lo, &hi))
        path = stem_decode_path_for_sid(lo, hi);
    pthread_mutex_lock(&g_lock);
    if (lo != g_lo || hi != g_hi || strcmp(g_path, path ? path : "")) {
        g_lo = lo;
        g_hi = hi;
        snprintf(g_path, sizeof(g_path), "%s", path ? path : "");
        g_loading = g_enabled && g_path[0];
        snprintf(g_status, sizeof(g_status), "%s",
                 g_enabled ? (g_path[0] ? "LOADING" : "LOAD TRACK") : "OFF");
        __atomic_store_n(&g_blocks, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_misses, 0, __ATOMIC_RELAXED);
        for (int i = 0; i < 4; i++)
            __atomic_store_n(&g_routed[i], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_selection, 3, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_generation, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_lock);
}
void prestem_settings_changed(void)
{
    int server_was_on = g_stems_on;
    stem_mode_exclusive(&g_stems_on, &g_prestems_on, STEM_MODE_PREFER_PRESTEM);
    if (server_was_on && !g_stems_on)
        mods_stem_settings_changed();
    pthread_mutex_lock(&g_lock);
    __atomic_store_n(&g_enabled, g_prestems_on && !g_stems_on, __ATOMIC_RELEASE);
    g_loading = g_enabled && g_path[0];
    snprintf(g_status, sizeof(g_status), "%s",
             g_enabled ? (g_loading ? "LOADING" : "LOAD TRACK") : "OFF");
    __atomic_store_n(&g_selection, 3, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_generation, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lock);
    kit_band_slots_changed();
    prestem_tick();
    prestem_ui_refresh();
}
int prestem_ui_snapshot(struct prestem_ui_snapshot *out)
{
    struct pst_set *set;
    if (!out || !g_worker_up)
        return 0;
    memset(out, 0, sizeof(*out));
    out->version = 1;
    out->size = sizeof(*out);
    pthread_mutex_lock(&g_lock);
    set = __atomic_load_n(&g_set, __ATOMIC_SEQ_CST);
    out->token = g_generation;
    out->state = __atomic_load_n(&g_selection, __ATOMIC_RELAXED);
    out->flags = g_enabled ? PRESTEM_UI_ENABLED : 0;
    if (g_enabled && set && set->token == g_generation) {
        if (pst_media_ready(&set->media)) out->flags |= PRESTEM_UI_READY;
        else if (strcmp(g_status, "STEM READ ERROR")) out->flags |= PRESTEM_UI_LOADING;
    }
    else if (g_enabled && g_loading)
        out->flags |= PRESTEM_UI_LOADING;
    pthread_mutex_unlock(&g_lock);
    return 1;
}
void prestem_status(char *out, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    struct pst_set *set = __atomic_load_n(&g_set, __ATOMIC_SEQ_CST);
    snprintf(out, cap, "%s", set && !strcmp(g_status, "READY") &&
             !pst_media_ready(&set->media) ? "LOADING" : g_status);
    pthread_mutex_unlock(&g_lock);
}
int prestem_ui_request_state(uint64_t token, unsigned state)
{
    struct pst_set *set;
    int ok = 0;
    pthread_mutex_lock(&g_lock);
    set = __atomic_load_n(&g_set, __ATOMIC_SEQ_CST);
    /* Store intent while the worker validates this exact track. Audio still
     * requires a published, source-bound set; publication preserves the choice. */
    if (state <= 3 && g_enabled && token == g_generation &&
        ((set && set->token == token) || g_loading)) {
        __atomic_store_n(&g_selection, state, __ATOMIC_RELEASE);
        ok = 1;
        MINFO("prestem: selection=%u generation=%llu\n", state, (unsigned long long)token);
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
void prestem_audio(float *dst, int64_t pos, int64_t frames, uint64_t lo, uint64_t hi, int rate)
{
    struct pst_set *set;
    unsigned state = __atomic_load_n(&g_selection, __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&g_audio_rate, rate, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_users, 1, __ATOMIC_SEQ_CST);
    set = __atomic_load_n(&g_set, __ATOMIC_SEQ_CST);
    if (set && set->lo == lo && set->hi == hi &&
        set->token == __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE) &&
        pst_media_mix(&set->media, dst, pos, frames, rate, state)) {
        __atomic_fetch_add(&g_blocks, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_routed[state], 1, __ATOMIC_RELAXED);
    } else
        __atomic_fetch_add(&g_misses, 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&g_users, 1, __ATOMIC_SEQ_CST);
}
/* Called only by the existing waveform worker after its source-bound capture. */
void prestem_wave_apply(void)
{
    static uint64_t last_token[3];
    static unsigned last_state[3];
    static uintptr_t last_columns[3];
    struct pst_set *set;
    unsigned state;
    int style;
    __atomic_fetch_add(&g_users, 1, __ATOMIC_SEQ_CST);
    set = __atomic_load_n(&g_set, __ATOMIC_SEQ_CST);
    state = __atomic_load_n(&g_selection, __ATOMIC_ACQUIRE);
    if (!set || set->token != __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE))
        goto done;
    for (style = 0; style < 3; style++) {
        struct style_state *st = &wave_g_st[style];
        if (!st->pristine || st->from_tid.lo != set->lo || (uint32_t)st->from_tid.hi != set->hi)
            continue;
        __atomic_store_n(&g_wave_live[style], st->ncols, __ATOMIC_RELAXED);
        __atomic_store_n(&g_wave_export[style], set->media.points[state == 2 ? 1 : 0][style],
                         __ATOMIC_RELAXED);
        if (state == 3) {
            if (st->modified) {
                wave_write_style(style, st->pristine);
                st->modified = 0;
            }
            __atomic_store_n(&g_wave_state[style], 3, __ATOMIC_RELAXED);
            continue;
        }
        if (st->modified && last_token[style] == set->token && last_state[style] == state &&
            last_columns[style] == st->columns)
            continue;
        if (!st->scratch)
            continue;
        if (state == 0) {
            memset(st->scratch, 0, (size_t)st->ncols * st->stride);
            if (style == WS_STYLE_3BAND) {
                uint32_t i;
                for (i = 0; i < st->ncols; i++) {
                    st->scratch[6 * i] = 8;
                    st->scratch[6 * i + 1] = 16;
                }
            }
        } else {
            unsigned part = state == 1 ? 0 : 1;
            if (!pst_wave_fit(style, set->media.wave[part][style], set->media.points[part][style],
                              st->scratch, st->ncols)) {
                if (st->modified) {
                    wave_write_style(style, st->pristine);
                    st->modified = 0;
                }
                continue;
            }
        }
        if (set->token != __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE))
            break;
        wave_write_style(style, st->scratch);
        st->modified = 1;
        __atomic_store_n(&g_wave_state[style], state, __ATOMIC_RELAXED);
        last_token[style] = set->token;
        last_state[style] = state;
        last_columns[style] = st->columns;
    }
done:
    __atomic_fetch_sub(&g_users, 1, __ATOMIC_SEQ_CST);
}
static int prestem_install(void)
{
    pthread_t thread;
    stem_mode_exclusive(&g_stems_on, &g_prestems_on, STEM_MODE_PREFER_SERVER);
    g_enabled = g_prestems_on && !g_stems_on;
    if (pthread_create(&thread, NULL, worker, NULL))
        return -1;
    pthread_detach(thread);
    g_worker_up = 1;
    MINFO("prestem: source-built OverCue sidecar engine installed\n");
    return 0;
}
KIT_MOD(k_mod_prestem, .name = "prestem", .prio = 20, .install = prestem_install,
        .what = "source-built OverCue sidecar playback");
