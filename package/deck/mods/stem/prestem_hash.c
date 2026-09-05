// SPDX-License-Identifier: MIT OR Apache-2.0
#include "stem/prestem_media.h"
#include <string.h>

struct pst_hash {
    uint32_t h[8];
    uint64_t bytes;
    unsigned used;
    uint8_t buf[64];
};
static uint32_t rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}
static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void hash_block(struct pst_hash *s, const uint8_t *p)
{
    uint32_t w[64], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    unsigned i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++)
        w[i] = w[i - 16] + (rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
               w[i - 7] + (rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10));
    for (i = 0; i < 64; i++) {
        uint32_t t1 =
            h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}
/* Optional ARMv8 SHA-256 instructions. The rest of this translation unit and
 * the entire shim keep their original CPU target. Linux's HWCAP check is the
 * prerequisite for entering this function on either hardware profile. */
#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <arm_neon.h>
__attribute__((target("+crypto"))) static void hash_blocks_crypto(struct pst_hash *s,
                                                                  const uint8_t *p, size_t blocks)
{
    uint32x4_t a = vld1q_u32(s->h), b = vld1q_u32(s->h + 4);
    while (blocks--) {
        uint32x4_t saved_a = a, saved_b = b, w[4];
        for (unsigned j = 0; j < 4; j++)
            w[j] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * j)));
        for (unsigned r = 0; r < 16; r++) {
            unsigned q = r & 3;
            if (r >= 4)
                w[q] = vsha256su1q_u32(vsha256su0q_u32(w[q], w[(q + 1) & 3]), w[(q + 2) & 3],
                                       w[(q + 3) & 3]);
            uint32x4_t wk = vaddq_u32(w[q], vld1q_u32(k + 4 * r)), old_a = a;
            a = vsha256hq_u32(a, b, wk);
            b = vsha256h2q_u32(b, old_a, wk);
        }
        a = vaddq_u32(a, saved_a);
        b = vaddq_u32(b, saved_b);
        p += 64;
    }
    vst1q_u32(s->h, a);
    vst1q_u32(s->h + 4, b);
}
#endif
int pst_sha256_accelerated(void)
{
#if defined(__aarch64__) && defined(__linux__)
    return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#else
    return 0;
#endif
}
static void sha256_impl(const void *data, size_t size, char out[65], int accelerated)
{
    struct pst_hash s = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
                          0x1f83d9ab, 0x5be0cd19},
                         0,
                         0,
                         {0}};
    const uint8_t *p = data;
    size_t left = size;
    unsigned i;
    static const char hex[] = "0123456789abcdef";
#if defined(__aarch64__) && defined(__linux__)
    if (accelerated && left >= 64) {
        size_t n = left / 64;
        hash_blocks_crypto(&s, p, n);
        p += n * 64;
        left -= n * 64;
    }
#else
    (void)accelerated;
#endif
    while (left >= 64) {
        hash_block(&s, p);
        p += 64;
        left -= 64;
    }
    memcpy(s.buf, p, left);
    s.buf[left] = 0x80;
    if (left >= 56) {
        hash_block(&s, s.buf);
        memset(s.buf, 0, 64);
    }
    for (i = 0; i < 8; i++)
        s.buf[63 - i] = (uint8_t)(((uint64_t)size * 8) >> (8 * i));
    hash_block(&s, s.buf);
    for (i = 0; i < 32; i++) {
        unsigned v = (s.h[i / 4] >> (24 - 8 * (i % 4))) & 255;
        out[2 * i] = hex[v >> 4];
        out[2 * i + 1] = hex[v & 15];
    }
    out[64] = 0;
}

void pst_sha256(const void *data, size_t size, char out[65])
{
    sha256_impl(data, size, out, pst_sha256_accelerated());
}
