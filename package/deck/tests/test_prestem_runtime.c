// SPDX-License-Identifier: MIT OR Apache-2.0
/* Exercise the shipped ownership/UI-token/audio code with host substitutes
 * only for the deck's source lookup, band widgets and waveform write. */
#include "test.h"
#include "kit/mod.h"
#undef KIT_MOD
#define KIT_MOD(sym, ...) static const struct kit_mod sym __attribute__((unused)) = {__VA_ARGS__}
#include "../mods/stem/prestem.c"
#include "../mods/stem/prestem_media.c"
#include "../mods/stem/prestem_hash.c"
#include "../mods/stem/prestem_wave.c"
int g_prestems_on, g_stems_on;
struct style_state wave_g_st[3];
static uint64_t source_lo = 1, source_hi = 618;
int stem_source_id(uint64_t *lo, uint64_t *hi)
{
    *lo = source_lo;
    *hi = source_hi;
    return 1;
}
const char *stem_decode_path_for_sid(uint64_t lo, uint64_t hi)
{
    (void)lo;
    (void)hi;
    return "/media/usb/Contents/test.mp3";
}
void mods_stem_settings_changed(void) {}
void kit_band_slots_changed(void) {}
void prestem_ui_refresh(void) {}
void wave_write_style(int style, const uint8_t *src)
{
    struct style_state *st = &wave_g_st[style];
    memcpy((void *)st->columns, src, st->ncols * st->stride);
}
static struct pst_set *fixture(void)
{
    struct pst_set *s = calloc(1, sizeof(*s));
    s->media.frames = 4;
    s->media.vocal = calloc(8, 2);
    s->media.instrumental = calloc(8, 2);
    for (int i = 0; i < 8; i++) {
        s->media.vocal[i] = 16384;
        s->media.instrumental[i] = -8192;
    }
    s->lo = 1;
    s->hi = 618;
    s->token = __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
    return s;
}
static int running;
static void *reader(void *arg)
{
    (void)arg;
    float dst[8];
    while (__atomic_load_n(&running, __ATOMIC_ACQUIRE)) {
        prestem_audio(dst, 0, 4, 1, 618, 96000);
        prestem_wave_apply();
    }
    return NULL;
}
int main(void)
{
    struct prestem_ui_snapshot snap;
    float dst[8];
    pthread_t t;
    T_CASE("ready controls and source-bound selection");
    g_worker_up = 1;
    g_enabled = 1;
    g_prestems_on = 1;
    g_set = fixture();
    CHECK(prestem_ui_snapshot(&snap));
    CHECK_INT(snap.flags, 3);
    CHECK_INT(snap.state, 3);
    CHECK(prestem_ui_request_state(snap.token, 1));
    prestem_audio(dst, 0, 4, 1, 618, 96000);
    CHECK(dst[0] == .5f);
    dst[0] = .7f;
    prestem_audio(dst, 0, 4, 1, 816, 96000);
    CHECK(dst[0] == .7f);
    prestem_audio(dst, 0, 4, 2, 618, 96000);
    CHECK(dst[0] == .7f);
    prestem_audio(dst, 0, 4, 1, 618, 0);
    CHECK(dst[0] == .7f);
    T_CASE("waveform isolation, full restoration and identity check");
    uint8_t live[4] = {5, 6, 7, 8}, original[4] = {5, 6, 7, 8}, scratch[4];
    wave_g_st[2] = (struct style_state){.pristine = original,
                                        .scratch = scratch,
                                        .ncols = 4,
                                        .columns = (uintptr_t)live,
                                        .stride = 1,
                                        .from_tid = {1, 618}};
    g_set->media.wave[0][2] = malloc(4);
    memset(g_set->media.wave[0][2], 9, 4);
    g_set->media.points[0][2] = 4;
    prestem_wave_apply();
    CHECK_INT(live[0], 9);
    CHECK(prestem_ui_request_state(snap.token, 3));
    prestem_wave_apply();
    CHECK(!memcmp(live, original, 4));
    CHECK(prestem_ui_request_state(snap.token, 0));
    prestem_wave_apply();
    CHECK_INT(live[0], 0);
    wave_g_st[2].from_tid.hi = 816;
    CHECK(prestem_ui_request_state(snap.token, 1));
    prestem_wave_apply();
    CHECK_INT(live[0], 0);
    T_CASE("RGB selection remains visible when export is shorter than the live array");
    uint8_t rgb_live[12] = {0}, rgb_orig[12] = {1}, rgb_scratch[12];
    wave_g_st[1] = (struct style_state){.pristine = rgb_orig,
                                        .scratch = rgb_scratch,
                                        .ncols = 6,
                                        .columns = (uintptr_t)rgb_live,
                                        .stride = 2,
                                        .from_tid = {1, 618}};
    g_set->media.wave[0][1] = malloc(8);
    memset(g_set->media.wave[0][1], 0x44, 8);
    g_set->media.points[0][1] = 4;
    g_set->media.wave[1][1] = malloc(8);
    memset(g_set->media.wave[1][1], 0x77, 8);
    g_set->media.points[1][1] = 4;
    prestem_wave_apply();
    CHECK_INT(rgb_live[0], 0x44);
    CHECK_INT(rgb_live[8], 0);
    CHECK(prestem_ui_request_state(snap.token, 2));
    prestem_wave_apply();
    CHECK_INT(rgb_live[0], 0x77);
    CHECK(prestem_ui_request_state(snap.token, 0));
    prestem_wave_apply();
    CHECK_INT(rgb_live[0], 0);
    CHECK(prestem_ui_request_state(snap.token, 3));
    prestem_wave_apply();
    CHECK(!memcmp(rgb_live, rgb_orig, 12));
    memset(wave_g_st, 0, sizeof(wave_g_st));
    T_CASE("track changes reject stale UI taps and old audio immediately");
    prestem_tick();
    CHECK(!prestem_ui_request_state(snap.token, 2));
    CHECK(prestem_ui_snapshot(&snap));
    CHECK_INT(snap.flags, PRESTEM_UI_ENABLED | PRESTEM_UI_LOADING);
    dst[0] = .7f;
    prestem_audio(dst, 0, 4, 1, 618, 96000);
    CHECK(dst[0] == .7f);
    retire();
    CHECK(g_set == NULL);
    T_CASE("loading accepts queued intent and keeps original audio until publication");
    CHECK(prestem_ui_selectable(snap.flags));
    CHECK(prestem_ui_request_state(snap.token, 1));
    CHECK(!prestem_ui_request_state(snap.token, 4));
    CHECK(prestem_ui_snapshot(&snap));
    CHECK_INT(snap.state, 1);
    dst[0] = .7f;
    prestem_audio(dst, 0, 4, 1, 618, 96000);
    CHECK(dst[0] == .7f);
    g_set = fixture();
    g_loading = 0;
    CHECK(prestem_ui_snapshot(&snap));
    CHECK_INT(snap.flags, PRESTEM_UI_ENABLED | PRESTEM_UI_READY);
    CHECK_INT(snap.state, 1);
    prestem_audio(dst, 0, 4, 1, 618, 96000);
    CHECK(dst[0] == .5f);
    retire();
    T_CASE("absent or failed stems are inactive, not a queued request target");
    CHECK(prestem_ui_snapshot(&snap));
    CHECK_INT(snap.flags, PRESTEM_UI_ENABLED);
    CHECK(!prestem_ui_selectable(snap.flags));
    CHECK(!prestem_ui_request_state(snap.token, 2));
    CHECK(!prestem_ui_selectable(0));
    CHECK(!prestem_ui_selectable(PRESTEM_UI_LOADING));
    CHECK(!prestem_ui_selectable(PRESTEM_UI_READY));
    dst[0] = .7f;
    T_CASE("settings exclusivity and disabled bypass");
    g_stems_on = 1;
    prestem_settings_changed();
    CHECK_INT(g_stems_on, 0);
    CHECK_INT(g_prestems_on, 1);
    g_set = fixture();
    g_prestems_on = 0;
    prestem_settings_changed();
    prestem_audio(dst, 0, 4, 1, 618, 96000);
    CHECK(dst[0] == .7f);
    retire();
    T_CASE("paged UI preserves selection while an uncached seek is loading");
    g_enabled = 1;
    g_set = fixture();
    g_set->media.pages = calloc(1, sizeof(struct pst_pages));
    struct pst_pages *pages = g_set->media.pages;
    pages->fd[0] = pages->fd[1] = -1;
    pages->bytes = 16;
    pages->count = 1;
    pages->ready = calloc(1, 1);
    snprintf(g_status, sizeof(g_status), "READY");
    CHECK(prestem_ui_snapshot(&snap));
    CHECK_INT(snap.flags, PRESTEM_UI_ENABLED | PRESTEM_UI_LOADING);
    CHECK(prestem_ui_request_state(snap.token, 1));
    dst[0] = .7f;
    prestem_audio(dst, 0, 4, 1, 618, 96000);
    CHECK(dst[0] == .7f);
    __atomic_store_n(&pages->ready[0], 1, __ATOMIC_RELEASE);
    CHECK(prestem_ui_snapshot(&snap));
    CHECK_INT(snap.flags, PRESTEM_UI_ENABLED | PRESTEM_UI_READY);
    CHECK_INT(snap.state, 1);
    prestem_audio(dst, 0, 4, 1, 618, 96000);
    CHECK(dst[0] == .5f);
    retire();
    T_CASE("buffer retirement concurrent with audio and waveform acquisition");
    g_enabled = 1;
    running = 1;
    CHECK_INT(pthread_create(&t, NULL, reader, NULL), 0);
    for (int i = 0; i < 1000; i++) {
        struct pst_set *next = fixture();
        pthread_mutex_lock(&g_lock);
        __atomic_store_n(&g_set, next, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&g_lock);
        sched_yield();
        retire();
    }
    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
    CHECK_INT(pthread_join(t, NULL), 0);
    CHECK(g_set == NULL);
    return t_done("prestem_runtime");
}
