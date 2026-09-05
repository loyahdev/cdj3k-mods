// SPDX-License-Identifier: MIT OR Apache-2.0
#include "stem/prestem_media.h"
#include "wave/wave.h"

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
int pst_wave_decode(const uint8_t *data, size_t size, int style, uint8_t **out, uint32_t *points)
{
    static const char *tags[] = {"PWV7", "PWV5", "PWV3"};
    static const unsigned in_stride[] = {3, 2, 1}, out_stride[] = {6, 2, 1};
    size_t pos, total;
    int found = 0;
    *out = NULL;
    *points = 0;
    if (style < 0 || style > 2 || size < 12 || memcmp(data, "PMAI", 4))
        return 0;
    pos = be32(data + 4);
    total = be32(data + 8);
    if (pos < 12 || pos > total || total != size)
        return 0;
    while (pos < total) {
        uint32_t header, bytes, count;
        size_t i;
        uint8_t *dst;
        if (total - pos < 12)
            goto fail;
        header = be32(data + pos + 4);
        bytes = be32(data + pos + 8);
        if (header < 12 || bytes < header || bytes > total - pos)
            goto fail;
        if (memcmp(data + pos, tags[style], 4)) {
            pos += bytes;
            continue;
        }
        if (found || header != 24 || be32(data + pos + 12) != in_stride[style])
            goto fail;
        count = be32(data + pos + 16);
        if (!count || count > 300000 || (uint64_t)count * in_stride[style] + header != bytes)
            goto fail;
        dst = calloc(count, out_stride[style]);
        if (!dst)
            goto fail;
        *out = dst;
        *points = count;
        found = 1;
        for (i = 0; i < count; i++) {
            const uint8_t *src = data + pos + header + i * in_stride[style];
            if (style == 0) {
                uint16_t hdr;
                uint8_t bands[3] = {src[0] & 127, src[1] & 127, src[2] & 127};
                mod_wave_encode(bands, &hdr, dst + 6 * i + 2);
                dst[6 * i] = (uint8_t)hdr;
                dst[6 * i + 1] = (uint8_t)(hdr >> 8);
            } else if (style == 1) {
                unsigned word = (unsigned)src[0] << 8 | src[1];
                unsigned native = (word >> 13) | (((word >> 10) & 7) << 3) |
                                  (((word >> 7) & 7) << 6) | (((word >> 2) & 31) << 9);
                dst[2 * i] = (uint8_t)native;
                dst[2 * i + 1] = (uint8_t)(native >> 8);
            } else
                dst[i] = (uint8_t)((src[0] << 3) | (src[0] >> 5));
        }
        pos += bytes;
    }
    return found;
fail:
    free(*out);
    *out = NULL;
    *points = 0;
    return 0;
}

/* Detailed analysis columns share the deck's 150 Hz time grid. MP3 padding
 * and rounded native arrays can differ in length from the stem's analysis.
 * Preserve time positions: trim the tail or fill it with native silence, never
 * stretch a beat across the whole song just to make the counts equal. */
int pst_wave_fit(int style, const uint8_t *src, uint32_t count, uint8_t *dst, uint32_t target)
{
    static const unsigned stride[] = {6, 2, 1};
    uint32_t copy;
    if (style < 0 || style > 2 || !src || !dst || !count || !target || count > 300000 ||
        target > 300000)
        return 0;
    copy = count < target ? count : target;
    memcpy(dst, src, (size_t)copy * stride[style]);
    if (copy < target) {
        memset(dst + (size_t)copy * stride[style], 0, (size_t)(target - copy) * stride[style]);
        if (style == 0)
            for (uint32_t i = copy; i < target; i++) {
                dst[6 * i] = 8;
                dst[6 * i + 1] = 16;
            }
    }
    return 1;
}
