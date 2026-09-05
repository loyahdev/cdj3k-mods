// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef PRESTEM_MEDIA_H
#define PRESTEM_MEDIA_H
#include <stddef.h>
#include <stdint.h>

#define PST_PATH_MAX 2048
#define PST_RATE 96000
#define PST_MAX_BYTES (512u * 1024u * 1024u)
struct pst_pages;
struct pst_media {
    struct pst_pages *pages;
    int16_t *vocal, *instrumental;
    uint64_t frames;
    uint64_t load_ms, read_ms, hash_ms;
    uint8_t *wave[2][3];
    uint32_t points[2][3];
    char bundle_path[PST_PATH_MAX];
};
typedef int (*pst_cancel_fn)(void *);
/* Worker only; input files are opened read-only. No media writes. */
int pst_media_load(const char *track, uint32_t track_id, struct pst_media *out,
                   pst_cancel_fn cancel, void *ctx, char *error, size_t cap);
/* Worker-only service; 1 loaded a page pair, 0 idle, -1 failed/cancelled. */
int pst_media_service(struct pst_media *, pst_cancel_fn, void *);
int pst_media_ready(const struct pst_media *);
void pst_media_free(struct pst_media *media);
int pst_sha256_accelerated(void);
void pst_sha256(const void *data, size_t size, char out[65]);
int pst_wave_decode(const uint8_t *data, size_t size, int style, uint8_t **out, uint32_t *points);
int pst_wave_fit(int style, const uint8_t *src, uint32_t count, uint8_t *dst, uint32_t target);
/* Position is on the original pre-stretch pool timeline. BOTH is bit-exact. */
int pst_media_mix(const struct pst_media *media, float *dst, int64_t pos, int64_t frames,
                  int pool_rate, unsigned selection);
#endif
