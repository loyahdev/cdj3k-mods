// SPDX-License-Identifier: MIT OR Apache-2.0
#include "test.h"
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
static size_t load_bytes_read;
static ssize_t counted_read(int fd, void *buf, size_t n)
{
    ssize_t result = read(fd, buf, n);
    if (result > 0)
        load_bytes_read += (size_t)result;
    return result;
}
#define read counted_read
#include "../mods/stem/prestem_media.c"
#undef read
#include "../mods/stem/prestem_hash.c"
#include "../mods/stem/prestem_wave.c"

struct paged_reader_test { struct pst_media *media; int stop, failed; };
static void *paged_reader_test_main(void *arg)
{
    struct paged_reader_test *t = arg;
    uint64_t seed = 1;
    while (!__atomic_load_n(&t->stop, __ATOMIC_ACQUIRE)) {
        seed = seed * 6364136223846793005ULL + 1;
        uint64_t pos = seed % (t->media->frames - 4);
        float audio[8];
        for (int i = 0; i < 8; i++) audio[i] = 0.125f;
        unsigned state = 1 + (seed & 1);
        int mixed = pst_media_mix(t->media, audio, pos, 4, 96000, state);
        const int16_t *pcm = state == 1 ? t->media->vocal : t->media->instrumental;
        for (int i = 0; i < 8; i++) {
            float expected = mixed ? pcm[2 * pos + i] / 32768.0f : 0.125f;
            if (audio[i] != expected) t->failed = 1;
        }
        sched_yield();
    }
    return NULL;
}
static void put(const char *p, const void *s, size_t n)
{
    FILE *f = fopen(p, "wb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fwrite(s, 1, n, f) == n);
        fclose(f);
    }
}
static int stop(void *p)
{
    (void)p;
    return 1;
}
static void putbe(uint8_t *p, uint32_t n)
{
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
static void wave_fit_tests(void)
{
    T_CASE("different native column counts preserve the 150 Hz timeline");
    uint8_t out[48], src[24];
    for (unsigned i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)(i + 1);
    for (int style = 0; style < 3; style++) {
        unsigned stride = style == 0 ? 6 : style == 1 ? 2 : 1;
        memset(out, 0xaa, sizeof(out));
        CHECK(pst_wave_fit(style, src, 4, out, 6));
        CHECK(!memcmp(out, src, 4 * stride));
        for (unsigned i = 4 * stride; i < 6 * stride; i++) {
            unsigned expected = style == 0 && i % 6 == 0 ? 8 : style == 0 && i % 6 == 1 ? 16 : 0;
            CHECK_INT(out[i], expected);
        }
        CHECK_INT(out[6 * stride], 0xaa);
        memset(out, 0xaa, sizeof(out));
        CHECK(pst_wave_fit(style, src, 4, out, 3));
        CHECK(!memcmp(out, src, 3 * stride));
        CHECK_INT(out[3 * stride], 0xaa);
        CHECK(pst_wave_fit(style, src, 4, out, 4));
        CHECK(!memcmp(out, src, 4 * stride));
    }
    CHECK(!pst_wave_fit(3, src, 4, out, 6));
    CHECK(!pst_wave_fit(1, NULL, 4, out, 6));
    CHECK(!pst_wave_fit(1, src, 4, out, 300001));
}
static void hash_dispatch_tests(void)
{
    T_CASE("runtime SHA dispatch equals portable implementation for unaligned inputs and tails");
    uint8_t data[2056];
    char reference[65], actual[65];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(73 * i + 19);
    for (unsigned offset = 0; offset < 8; offset++)
        for (unsigned size = 0; size <= 2048; size += 7) {
            sha256_impl(data + offset, size, reference, 0);
            pst_sha256(data + offset, size, actual);
            CHECK(!strcmp(reference, actual));
        }
    printf("SHA2 acceleration selected: %d\n", pst_sha256_accelerated());
}
static void malformed_tests(void)
{
    uint8_t data[40] = {0}, *out = NULL;
    uint32_t points = 0;
    T_CASE("waveform bounds, layout, and malformed tags");
    memcpy(data, "PMAI", 4);
    putbe(data + 4, 12);
    putbe(data + 8, sizeof(data));
    memcpy(data + 12, "PWV3", 4);
    putbe(data + 16, 24);
    putbe(data + 20, 28);
    putbe(data + 24, 1);
    putbe(data + 28, 4);
    data[36] = 0xff;
    data[37] = 0x20;
    data[38] = 0x1f;
    CHECK(pst_wave_decode(data, sizeof(data), 2, &out, &points));
    CHECK_INT(points, 4);
    CHECK_INT(out[0], 255);
    CHECK_INT(out[1], 1);
    CHECK_INT(out[2], 248);
    free(out);
    for (size_t n = 0; n < sizeof(data); n++) {
        CHECK(!pst_wave_decode(data, n, 2, &out, &points));
        CHECK(out == NULL);
    }
    putbe(data + 20, 0xffffffff);
    CHECK(!pst_wave_decode(data, sizeof(data), 2, &out, &points));
    putbe(data + 20, 28);
    putbe(data + 28, 0xffffffff);
    CHECK(!pst_wave_decode(data, sizeof(data), 2, &out, &points));
    T_CASE("strict JSON numbers and malformed identities");
    const char *bad[] = {"+1", "01", "-.1", "1.", "1e", "NaN", "[1,]", "{\"a\":}", "\"bad\\q\""};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        struct json j = {.s = bad[i], .n = strlen(bad[i]), .t = calloc(JT_MAX, sizeof(struct jt))};
        CHECK(value(&j, 0) < 0);
        free(j.t);
    }
}
int main(int argc, char **argv)
{
    char hash[65], root[] = "/tmp/prestem-test-XXXXXX", track[PST_PATH_MAX], path[PST_PATH_MAX];
    char bundle[PST_PATH_MAX], index[2048], manifest[4096], vh[65], ih[65], error[64];
    struct pst_media m = {0};
    int16_t v[] = {0, 0, 16384, -16384, 8192, -8192, 32767, -32768};
    int16_t ins[] = {0, 0, -8192, 8192, 4096, 4096, -16384, 16384};
    float dst[8], original[] = {.1f, .2f, .3f, .4f, .5f, .6f, .7f, .8f};
    int i;
    if (argc == 3) {
        int rc = pst_media_load(argv[1], (uint32_t)strtoul(argv[2], NULL, 10), &m, NULL, NULL,
                                error, sizeof(error));
        printf("real export: %s frames=%llu initial_ms=%llu paged=%d\n", error,
               (unsigned long long)m.frames, (unsigned long long)m.load_ms, m.pages != NULL);
        if (!rc && m.pages) {
            T_CASE("real PGZ loads only the first page pair before publication");
            CHECK(m.pages->ready[0]);
            CHECK(!m.pages->ready[m.pages->count - 1]);
            T_CASE("PGZ table authentication, truncation, magic and offset bounds");
            size_t tn = 24 + 48 * m.pages->count;
            uint8_t *table_copy = malloc(tn);
            CHECK(table_copy != NULL);
            if (table_copy) {
                for (int scenario = 0; scenario < 4; scenario++) {
                    char tmp[] = "/tmp/pgz-invalid-XXXXXX", th[65];
                    int fd = mkstemp(tmp);
                    CHECK(fd >= 0);
                    if (fd < 0) break;
                    memcpy(table_copy, m.pages->table[0], tn);
                    pst_sha256(table_copy, tn, th);
                    if (scenario == 0) table_copy[40] ^= 1; /* unauthenticated table */
                    if (scenario == 1) table_copy[0] = 'X';
                    if (scenario == 2) {
                        memset(table_copy + 24, 0xff, 8); /* authenticated invalid offset */
                        pst_sha256(table_copy, tn, th);
                    }
                    size_t written = scenario == 3 ? 23 : tn;
                    CHECK(write(fd, table_copy, written) == (ssize_t)written);
                    close(fd);
                    struct pst_media bad = {.frames = m.frames};
                    bad.pages = calloc(1, sizeof(*bad.pages));
                    CHECK(bad.pages != NULL);
                    if (bad.pages) {
                        bad.pages->fd[0] = bad.pages->fd[1] = -1;
                        bad.pages->bytes = m.pages->bytes;
                        bad.pages->count = m.pages->count;
                        CHECK(!pgopen(&bad, 0, tmp, written, th, NULL, NULL));
                        pst_media_free(&bad);
                    }
                    unlink(tmp);
                }
                free(table_copy);
            }
            float seek[8] = {1,2,3,4,5,6,7,8}, saved[8];
            memcpy(saved, seek, sizeof(seek));
            T_CASE("uncached end seek requests that page without changing original audio");
            CHECK(!pst_media_mix(&m, seek, m.frames - 4, 4, 96000, 1));
            CHECK(!memcmp(seek, saved, sizeof(seek)));
            CHECK(!pst_media_ready(&m));
            CHECK(pst_media_service(&m, NULL, NULL) == 1);
            CHECK(m.pages->ready[m.pages->count - 1]);
            CHECK(pst_media_ready(&m));
            CHECK(pst_media_mix(&m, seek, m.frames - 4, 4, 96000, 1));
            T_CASE("interpolation crossing a page boundary waits for both pages");
            CHECK(!pst_media_mix(&m, seek, m.pages->bytes / 4 - 1, 2, 96000, 2));
            CHECK(pst_media_service(&m, NULL, NULL) == 1);
            CHECK(pst_media_mix(&m, seek, m.pages->bytes / 4 - 1, 2, 96000, 2));
            T_CASE("concurrent random seeks see only fully published page pairs");
            struct paged_reader_test reader = {.media = &m};
            pthread_t thread;
            int created = pthread_create(&thread, NULL, paged_reader_test_main, &reader);
            CHECK(created == 0);
            int serviced;
            while ((serviced = pst_media_service(&m, NULL, NULL)) > 0) {}
            __atomic_store_n(&reader.stop, 1, __ATOMIC_RELEASE);
            if (!created) CHECK(pthread_join(thread, NULL) == 0);
            CHECK(!reader.failed);
            CHECK(serviced == 0);
            T_CASE("every decoded PGZ byte equals the raw OverCue export");
            uint8_t buffer[65536];
            for (int role = 0; role < 2; role++) {
                join(path, m.bundle_path, role ? "stems-sidecar-instrumental.s16le" : "stems-sidecar-vocal.s16le");
                FILE *raw = fopen(path, "rb");
                CHECK(raw != NULL);
                if (!raw) { rc = 1; break; }
                size_t offset = 0, n;
                const uint8_t *pcm = (const uint8_t *)(role ? m.instrumental : m.vocal);
                while ((n = fread(buffer, 1, sizeof(buffer), raw))) {
                    CHECK(offset + n <= m.frames * 4);
                    if (offset + n > m.frames * 4) break;
                    CHECK(!memcmp(buffer, pcm + offset, n));
                    offset += n;
                }
                CHECK(offset == m.frames * 4);
                CHECK(!ferror(raw));
                fclose(raw);
            }
        }
        if (!rc) {
            float block[2048];
            unsigned long long checked = 0;
            int failed = 0;
            for (unsigned state = 0; state < 4; state++) {
                for (uint64_t pos = 0; pos < m.frames; pos += 1024) {
                    int n = (int)(m.frames - pos < 1024 ? m.frames - pos : 1024);
                    for (int i = 0; i < 2 * n; i++)
                        block[i] = 0.123456f;
                    if (!pst_media_mix(&m, block, pos, n, 96000, state)) {
                        failed = 1;
                        break;
                    }
                    for (int i = 0; i < 2 * n; i++) {
                        float expected = state == 3   ? 0.123456f
                                         : state == 1 ? m.vocal[2 * pos + i] / 32768.0f
                                         : state == 2 ? m.instrumental[2 * pos + i] / 32768.0f
                                                      : 0;
                        if (block[i] != expected) {
                            failed = 1;
                            break;
                        }
                        checked++;
                    }
                }
            }
            printf("real export: four-state comparison samples=%llu exact=%d\n", checked, !failed);
            for (int part = 0; part < 2; part++)
                for (int style = 0; style < 3; style++)
                    printf("real export: part=%d style=%d points=%u\n", part, style,
                           m.points[part][style]);
            rc = failed;
        }
        pst_media_free(&m);
        if (!rc) {
            T_CASE("corrupt chunk hash never publishes audio");
            CHECK(!pst_media_load(argv[1], (uint32_t)strtoul(argv[2], NULL, 10), &m, NULL, NULL, error, sizeof(error)));
            if (m.pages) {
                m.pages->table[1][24 + 48 + 16] ^= 1;
                m.pages->requested = 1;
                CHECK(pst_media_service(&m, NULL, NULL) == -1);
                CHECK(!m.pages->ready[1]);
                CHECK(!pst_media_ready(&m));
            }
            pst_media_free(&m);
            T_CASE("cancelled page load never publishes audio");
            CHECK(!pst_media_load(argv[1], (uint32_t)strtoul(argv[2], NULL, 10), &m, NULL, NULL, error, sizeof(error)));
            if (m.pages) {
                CHECK(pst_media_service(&m, stop, NULL) == -1);
                CHECK(!m.pages->ready[1]);
            }
            pst_media_free(&m);
        }
        return rc != 0 || t_done("real_paged_export");
    }
    malformed_tests();
    wave_fit_tests();
    hash_dispatch_tests();
    T_CASE("SHA-256 published vectors");
    pst_sha256("", 0, hash);
    CHECK(!strcmp(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    pst_sha256("abc", 3, hash);
    CHECK(!strcmp(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    {
        char million[1000000];
        memset(million, 'a', sizeof(million));
        pst_sha256(million, sizeof(million), hash);
        CHECK(!strcmp(hash, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    }
    T_CASE("valid export with Unicode path");
    CHECK(mkdtemp(root) != NULL);
    snprintf(path, sizeof(path), "%s/Contents", root);
    CHECK(!mkdir(path, 0700));
    snprintf(track, sizeof(track), "%s/Contents/Té.mp3", root);
    put(track, "abc", 3);
    snprintf(path, sizeof(path), "%s/CDJMODS", root);
    CHECK(!mkdir(path, 0700));
    snprintf(path, sizeof(path), "%s/CDJMODS/stems", root);
    CHECK(!mkdir(path, 0700));
    snprintf(bundle, sizeof(bundle), "%s/CDJMODS/stems/ba7816bf8f01cfea", root);
    CHECK(!mkdir(bundle, 0700));
    pst_sha256(v, sizeof(v), vh);
    pst_sha256(ins, sizeof(ins), ih);
    snprintf(
        index, sizeof(index),
        "{\"schema\":\"overcue-index/"
        "1\",\"keyed_by\":\"export.pdb\",\"tracks\":{\"618\":{\"bundle\":\"ba7816bf8f01cfea\","
        "\"file_path\":\"/Contents/"
        "T\\u00e9.mp3\",\"frames\":4,\"vocal_sha256\":\"%s\",\"instrumental_sha256\":\"%s\"}}}",
        vh, ih);
    snprintf(path, sizeof(path), "%s/CDJMODS/index.json", root);
    put(path, index, strlen(index));
    snprintf(
        manifest, sizeof(manifest),
        "{\"schema\":\"overcue-sidecar-pair/"
        "1\",\"bundle\":\"ba7816bf8f01cfea\",\"runtime\":{\"sample_rate\":96000,\"channels\":2,"
        "\"format\":\"s16le\",\"frames\":4,\"latency_pad_frames\":1},\"sources\":{\"full\":{\"size_"
        "bytes\":3,\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}"
        "},\"sidecars\":{\"stems-sidecar-vocal.s16le\":{\"bytes\":16,\"sha256\":\"%s\"},\"stems-"
        "sidecar-instrumental.s16le\":{\"bytes\":16,\"sha256\":\"%s\"}}}",
        vh, ih);
    join(path, bundle, "overcue-manifest.json");
    put(path, manifest, strlen(manifest));
    join(path, bundle, "stems-sidecar-vocal.s16le");
    put(path, v, sizeof(v));
    join(path, bundle, "stems-sidecar-instrumental.s16le");
    put(path, ins, sizeof(ins));
    load_bytes_read = 0;
    CHECK(!pst_media_load(track, 618, &m, NULL, NULL, error, sizeof(error)));
    CHECK(load_bytes_read == strlen(index) + strlen(manifest) + sizeof(v) + sizeof(ins));
    CHECK_INT(m.frames, 4);
    T_CASE("Full is bit-exact; isolated parts and mute");
    memcpy(dst, original, sizeof(dst));
    CHECK(pst_media_mix(&m, dst, 0, 4, 96000, 3));
    CHECK(!memcmp(dst, original, sizeof(dst)));
    CHECK(pst_media_mix(&m, dst, 0, 4, 96000, 1));
    for (i = 0; i < 8; i++)
        CHECK(dst[i] == v[i] / 32768.0f);
    CHECK(pst_media_mix(&m, dst, 0, 4, 96000, 2));
    for (i = 0; i < 8; i++)
        CHECK(dst[i] == ins[i] / 32768.0f);
    CHECK(pst_media_mix(&m, dst, 0, 4, 96000, 0));
    for (i = 0; i < 8; i++)
        CHECK(dst[i] == 0);
    T_CASE("seek, pool-rate conversion, and original fallback");
    CHECK(pst_media_mix(&m, dst, 1, 2, 96000, 1));
    CHECK(dst[0] == .5f);
    CHECK(dst[2] == .25f);
    CHECK(pst_media_mix(&m, dst, 0, 2, 48000, 1));
    CHECK(dst[0] == 0);
    CHECK(dst[2] == .25f);
    memcpy(dst, original, sizeof(dst));
    CHECK(!pst_media_mix(&m, dst, 3, 2, 96000, 0));
    CHECK(!memcmp(dst, original, sizeof(dst)));
    CHECK(!pst_media_mix(&m, dst, -1, 1, 96000, 1));
    CHECK(!pst_media_mix(&m, dst, 0, 1, 12345, 1));
    pst_media_free(&m);
    T_CASE("wrong TrackID, cancelled load, truncated source and stems");
    CHECK(pst_media_load(track, 816, &m, NULL, NULL, error, sizeof(error)) != 0);
    CHECK(pst_media_load(track, 618, &m, stop, NULL, error, sizeof(error)) != 0);
    CHECK(m.vocal == NULL);
    put(track, "ab", 2);
    CHECK(pst_media_load(track, 618, &m, NULL, NULL, error, sizeof(error)) != 0);
    CHECK(!strcmp(error, "TRACK MISMATCH"));
    put(track, "abc", 3);
    join(path, bundle, "stems-sidecar-vocal.s16le");
    put(path, ins, sizeof(ins) - 2);
    CHECK(pst_media_load(track, 618, &m, NULL, NULL, error, sizeof(error)) != 0);
    CHECK(!strcmp(error, "STEM SIZE"));
    put(path, v, sizeof(v));
    T_CASE("export content integrity is trusted; equal-size audio is not rehashed");
    put(track, "abd", 3);
    put(path, ins, sizeof(ins));
    CHECK(!pst_media_load(track, 618, &m, NULL, NULL, error, sizeof(error)));
    CHECK(!memcmp(m.vocal, ins, sizeof(ins)));
    pst_media_free(&m);
    put(track, "abc", 3);
    put(path, v, sizeof(v));
    T_CASE("track symlinks and mismatched manifest/index digests are rejected");
    unlink(track);
    CHECK(!symlink(path, track));
    CHECK(pst_media_load(track, 618, &m, NULL, NULL, error, sizeof(error)) != 0);
    CHECK(!strcmp(error, "TRACK MISMATCH"));
    unlink(track);
    put(track, "abc", 3);
    {
        char *digest = strstr(manifest, vh);
        CHECK(digest != NULL);
        if (digest) {
            char saved = *digest;
            *digest = saved == '0' ? '1' : '0';
            join(path, bundle, "overcue-manifest.json");
            put(path, manifest, strlen(manifest));
            CHECK(pst_media_load(track, 618, &m, NULL, NULL, error, sizeof(error)) != 0);
            CHECK(!strcmp(error, "STEM METADATA"));
            *digest = saved;
            put(path, manifest, strlen(manifest));
        }
    }
    T_CASE("truncated JSON and duplicate identity");
    snprintf(path, sizeof(path), "%s/CDJMODS/index.json", root);
    put(path, index, strlen(index) - 1);
    CHECK(pst_media_load(track, 618, &m, NULL, NULL, error, sizeof(error)) != 0);
    {
        struct json j = {0};
        const char *s = "{\"id\":1,\"id\":2}";
        j.s = s;
        j.n = strlen(s);
        j.t = calloc(JT_MAX, sizeof(*j.t));
        CHECK(value(&j, 0) == 0);
        CHECK(field(&j, 0, "id") == -1);
        free(j.t);
    }
    unlink(path);
    unlink(track);
    snprintf(path, sizeof(path), "%s/Contents", root);
    rmdir(path);
    join(path, bundle, "overcue-manifest.json");
    unlink(path);
    join(path, bundle, "stems-sidecar-vocal.s16le");
    unlink(path);
    join(path, bundle, "stems-sidecar-instrumental.s16le");
    unlink(path);
    rmdir(bundle);
    snprintf(path, sizeof(path), "%s/CDJMODS/stems", root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/CDJMODS", root);
    rmdir(path);
    rmdir(root);
    return t_done("prestem_media");
}
