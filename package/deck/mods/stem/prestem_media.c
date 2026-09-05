// SPDX-License-Identifier: MIT OR Apache-2.0
/* OverCue sidecar-pair/1 reader. Deliberately independent of EP122/JUCE so
 * exactly the shipped parser, validation and mixer can run in host tests. */
#include "stem/prestem_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <dlfcn.h>

struct jt {
    size_t begin, end;
    int next, kind;
};
struct json {
    const char *s;
    size_t n, p;
    struct jt *t;
    int count;
};
#define JT_MAX 65536
static void ws(struct json *j)
{
    while (j->p < j->n && j->s[j->p] && strchr(" \t\r\n", j->s[j->p]))
        j->p++;
}
static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static int value(struct json *j, int depth)
{
    int at, kind;
    char close;
    ws(j);
    if (depth > 32 || j->p >= j->n || j->count >= JT_MAX)
        return -1;
    at = j->count++;
    kind = j->s[j->p];
    j->t[at].begin = j->p;
    j->t[at].kind = kind;
    if (kind == '{' || kind == '[') {
        close = kind == '{' ? '}' : ']';
        j->p++;
        ws(j);
        if (j->p < j->n && j->s[j->p] == close)
            j->p++;
        else
            for (;;) {
                if (kind == '{') {
                    ws(j);
                    if (j->p >= j->n || j->s[j->p] != '"' || value(j, depth + 1) < 0)
                        return -1;
                    ws(j);
                    if (j->p >= j->n || j->s[j->p++] != ':')
                        return -1;
                }
                if (value(j, depth + 1) < 0)
                    return -1;
                ws(j);
                if (j->p >= j->n)
                    return -1;
                if (j->s[j->p] == close) {
                    j->p++;
                    break;
                }
                if (j->s[j->p++] != ',')
                    return -1;
            }
    } else if (kind == '"') {
        int ended = 0;
        j->p++;
        while (j->p < j->n) {
            unsigned char c = j->s[j->p++];
            if (c == '"') {
                ended = 1;
                break;
            }
            if (c < 32)
                return -1;
            if (c == '\\') {
                if (j->p >= j->n)
                    return -1;
                c = j->s[j->p++];
                if (c == 'u') {
                    int k;
                    for (k = 0; k < 4; k++)
                        if (j->p >= j->n || hexval(j->s[j->p++]) < 0)
                            return -1;
                } else if (!strchr("\"\\/bfnrt", c))
                    return -1;
            }
        }
        if (!ended)
            return -1;
    } else {
        size_t start = j->p, p;
        while (j->p < j->n && !strchr(" \t\r\n,}]", j->s[j->p]))
            j->p++;
        if (start == j->p)
            return -1;
        if ((j->p - start == 4 &&
             (!memcmp(j->s + start, "null", 4) || !memcmp(j->s + start, "true", 4))) ||
            (j->p - start == 5 && !memcmp(j->s + start, "false", 5))) {
        } else {
            p = start;
            if (j->s[p] == '-')
                p++;
            if (p == j->p)
                return -1;
            if (j->s[p] == '0')
                p++;
            else {
                if (j->s[p] < '1' || j->s[p] > '9')
                    return -1;
                do {
                    p++;
                } while (p < j->p && j->s[p] >= '0' && j->s[p] <= '9');
            }
            if (p < j->p && j->s[p] == '.') {
                size_t first = ++p;
                while (p < j->p && j->s[p] >= '0' && j->s[p] <= '9')
                    p++;
                if (p == first)
                    return -1;
            }
            if (p < j->p && (j->s[p] == 'e' || j->s[p] == 'E')) {
                size_t first;
                p++;
                if (p < j->p && (j->s[p] == '+' || j->s[p] == '-'))
                    p++;
                first = p;
                while (p < j->p && j->s[p] >= '0' && j->s[p] <= '9')
                    p++;
                if (p == first)
                    return -1;
            }
            if (p != j->p)
                return -1;
        }
    }
    j->t[at].end = j->p;
    j->t[at].next = j->count;
    return at;
}
static int jstring(const struct json *j, int at, char *out, size_t cap)
{
    size_t p, n = 0, end;
    if (at < 0 || at >= j->count || j->t[at].kind != '"' || !cap)
        return -1;
    p = j->t[at].begin + 1;
    end = j->t[at].end - 1;
    while (p < end) {
        unsigned c = (unsigned char)j->s[p++];
        if (c == '\\') {
            c = j->s[p++];
            if (c == 'u') {
                unsigned k, v = 0;
                for (k = 0; k < 4; k++)
                    v = (v << 4) | (unsigned)hexval(j->s[p++]);
                if (v >= 0xd800 && v <= 0xdbff) {
                    unsigned low = 0;
                    if (p + 6 > end || j->s[p++] != '\\' || j->s[p++] != 'u')
                        return -1;
                    for (k = 0; k < 4; k++)
                        low = (low << 4) | (unsigned)hexval(j->s[p++]);
                    if (low < 0xdc00 || low > 0xdfff)
                        return -1;
                    v = 0x10000 + ((v - 0xd800) << 10) + low - 0xdc00;
                } else if (v >= 0xdc00 && v <= 0xdfff)
                    return -1;
                if (!v)
                    return -1;
                if (v >= 0x10000) {
                    if (n + 4 >= cap)
                        return -1;
                    out[n++] = (char)(0xf0 | (v >> 18));
                    out[n++] = (char)(0x80 | ((v >> 12) & 63));
                    out[n++] = (char)(0x80 | ((v >> 6) & 63));
                    out[n++] = (char)(0x80 | (v & 63));
                    continue;
                }
                if (v >= 0x800) {
                    if (n + 3 >= cap)
                        return -1;
                    out[n++] = (char)(0xe0 | (v >> 12));
                    out[n++] = (char)(0x80 | ((v >> 6) & 63));
                    out[n++] = (char)(0x80 | (v & 63));
                    continue;
                }
                if (v >= 0x80) {
                    if (n + 2 >= cap)
                        return -1;
                    out[n++] = (char)(0xc0 | (v >> 6));
                    out[n++] = (char)(0x80 | (v & 63));
                    continue;
                }
                c = v;
            } else if (c == 'b')
                c = '\b';
            else if (c == 'f')
                c = '\f';
            else if (c == 'n')
                c = '\n';
            else if (c == 'r')
                c = '\r';
            else if (c == 't')
                c = '\t';
        }
        if (!c || n + 1 >= cap)
            return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return 0;
}
/* Duplicate keys are rejected instead of silently choosing an identity. */
static int field(const struct json *j, int obj, const char *name)
{
    int i, found = -1;
    char key[PST_PATH_MAX];
    if (obj < 0 || j->t[obj].kind != '{')
        return -1;
    for (i = obj + 1; i < j->t[obj].next;) {
        int v = i + 1;
        if (jstring(j, i, key, sizeof(key)))
            return -1;
        if (!strcmp(key, name)) {
            if (found >= 0)
                return -1;
            found = v;
        }
        i = j->t[v].next;
    }
    return found;
}
static int textfield(const struct json *j, int obj, const char *name, char *out, size_t cap)
{
    return jstring(j, field(j, obj, name), out, cap);
}
static int equal(const struct json *j, int obj, const char *name, const char *want)
{
    char s[PST_PATH_MAX];
    return !textfield(j, obj, name, s, sizeof(s)) && !strcmp(s, want);
}
static uint64_t number(const struct json *j, int obj, const char *name)
{
    int a = field(j, obj, name);
    uint64_t n = 0;
    size_t p;
    if (a < 0)
        return UINT64_MAX;
    for (p = j->t[a].begin; p < j->t[a].end; p++) {
        unsigned d = (unsigned)(j->s[p] - '0');
        if (d > 9 || n > (UINT64_MAX - d) / 10)
            return UINT64_MAX;
        n = n * 10 + d;
    }
    return n;
}
static void json_free(struct json *j)
{
    free((void *)j->s);
    free(j->t);
    memset(j, 0, sizeof(*j));
}
static void *readfile(const char *path, size_t limit, size_t *size, pst_cancel_fn cancel, void *ctx)
{
    struct stat before, after;
    uint8_t *p = NULL;
    size_t n = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size > limit)
        goto done;
    p = malloc((size_t)before.st_size + 1);
    if (!p)
        goto done;
    while (n < (size_t)before.st_size) {
        size_t count = (size_t)before.st_size - n;
        ssize_t got;
        if (cancel && cancel(ctx))
            goto fail;
        if (count > 1048576)
            count = 1048576;
        got = read(fd, p + n, count);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            goto fail;
        n += (size_t)got;
    }
    if (fstat(fd, &after) || before.st_size != after.st_size || before.st_mtime != after.st_mtime)
        goto fail;
    p[n] = 0;
    *size = n;
    goto done;
fail:
    free(p);
    p = NULL;
done:
    close(fd);
    return p;
}
static int json_read(const char *path, struct json *j)
{
    memset(j, 0, sizeof(*j));
    j->s = readfile(path, 4 * 1024 * 1024, &j->n, NULL, NULL);
    if (!j->s)
        return -1;
    j->t = calloc(JT_MAX, sizeof(*j->t));
    if (!j->t || value(j, 0) != 0) {
        json_free(j);
        return -1;
    }
    ws(j);
    if (j->p != j->n) {
        json_free(j);
        return -1;
    }
    return 0;
}
static int join(char *out, const char *root, const char *relative)
{
    return snprintf(out, PST_PATH_MAX, "%s/%s", root, relative) >= PST_PATH_MAX ? -1 : 0;
}
static int hash_ok(const void *p, size_t n, const char *expected)
{
    char hash[65];
    pst_sha256(p, n, hash);
    return strlen(expected) == 64 && !strcmp(hash, expected);
}
static int valid_hash(const char *hash)
{
    if (strlen(hash) != 64)
        return 0;
    for (int i = 0; i < 64; i++)
        if (hexval(hash[i]) < 0)
            return 0;
    return 1;
}
/* OverCue validates audio at export. Check the loaded track's identity and
 * length without reading the full song again on the deck. Content changes
 * that preserve length require re-export; they are not detected here. */
static int track_size_ok(const char *path, uint64_t expected)
{
    struct stat st;
    int fd, ok;
    if (!expected || expected > PST_MAX_BYTES)
        return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return 0;
    ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 &&
         (uint64_t)st.st_size == expected;
    close(fd);
    return ok;
}
static uint64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return 0;
    return (uint64_t)t.tv_sec * 1000 + (unsigned long)t.tv_nsec / 1000000;
}
static void *media_read(struct pst_media *m, const char *path, size_t limit, size_t *size,
                        pst_cancel_fn cancel, void *ctx)
{
    uint64_t start = now_ms();
    void *data = readfile(path, limit, size, cancel, ctx);
    m->read_ms += now_ms() - start;
    return data;
}
static int media_hash(struct pst_media *m, const void *data, size_t size, const char *hash)
{
    uint64_t start = now_ms();
    int result = hash_ok(data, size, hash);
    m->hash_ms += now_ms() - start;
    return result;
}
/* PGZ tables and page hashes are authenticated against the export index.
 * Only the file worker writes PCM. Release/acquire page flags publish immutable
 * ranges to audio; audio only requests ranges, never reads USB or takes locks. */
struct pst_pages {
    int fd[2], failed;
    uint32_t bytes, count, requested;
    uint8_t *table[2], *ready;
    int (*inflate)(unsigned char *, unsigned long *, const unsigned char *, unsigned long);
    void *zlib;
};
static uint32_t pg32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t pg64(const uint8_t *p) { return (uint64_t)pg32(p) << 32 | pg32(p + 4); }
static int pgread(int fd, void *dst, size_t n, uint64_t offset, pst_cancel_fn cancel, void *ctx)
{
    size_t got = 0;
    while (got < n) {
        if (cancel && cancel(ctx)) return 0;
        ssize_t r = pread(fd, (uint8_t *)dst + got, n - got, (off_t)(offset + got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}
static int pgopen(struct pst_media *m, unsigned role, const char *path, uint64_t file_bytes,
                  const char *digest, pst_cancel_fn cancel, void *ctx)
{
    struct pst_pages *p = m->pages;
    struct stat st;
    uint8_t h[24];
    p->fd[role] = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (p->fd[role] < 0 || fstat(p->fd[role], &st) || !S_ISREG(st.st_mode) ||
        st.st_size < 24 || (uint64_t)st.st_size != file_bytes ||
        !pgread(p->fd[role], h, sizeof(h), 0, cancel, ctx) || memcmp(h, "OVPGZ001", 8) ||
        pg32(h + 8) != p->bytes || pg32(h + 12) != p->count || pg64(h + 16) != m->frames * 4)
        return 0;
    size_t table_bytes = 24 + (size_t)p->count * 48;
    uint8_t *table = malloc(table_bytes);
    p->table[role] = table;
    if (!table || !pgread(p->fd[role], table, table_bytes, 0, cancel, ctx) ||
        !hash_ok(table, table_bytes, digest)) return 0;
    uint64_t end = table_bytes;
    for (uint32_t i = 0; i < p->count; i++) {
        const uint8_t *e = table + 24 + 48 * i;
        uint64_t offset = pg64(e);
        uint32_t packed = pg32(e + 8), raw = pg32(e + 12);
        uint64_t remaining = m->frames * 4 - (uint64_t)i * p->bytes;
        if (raw != (remaining < p->bytes ? remaining : p->bytes) ||
            !packed || packed > p->bytes + 65536 || offset < end ||
            offset > file_bytes || packed > file_bytes - offset) return 0;
        end = offset + packed;
    }
    return end == file_bytes;
}
int pst_media_service(struct pst_media *m, pst_cancel_fn cancel, void *ctx)
{
    struct pst_pages *p = m->pages;
    if (!p) return 0;
    if (__atomic_load_n(&p->failed, __ATOMIC_ACQUIRE)) return -1;
    uint32_t target = __atomic_load_n(&p->requested, __ATOMIC_ACQUIRE);
    if (target >= p->count) target = 0;
    uint32_t page = target;
    /* Current page, then forward read-ahead, then the rest of the track. */
    while (__atomic_load_n(&p->ready[page], __ATOMIC_ACQUIRE)) {
        page = (page + 1) % p->count;
        if (page == target) return 0;
    }
    for (unsigned role = 0; role < 2; role++) {
        const uint8_t *e = p->table[role] + 24 + 48 * page;
        uint32_t packed = pg32(e + 8), raw = pg32(e + 12);
        uint8_t *compressed = malloc(packed);
        uint8_t *dst = (uint8_t *)(role ? m->instrumental : m->vocal) + (size_t)page * p->bytes;
        unsigned long size = raw;
        char digest[65], expected[65];
        uint64_t started = now_ms();
        int ok = compressed && pgread(p->fd[role], compressed, packed, pg64(e), cancel, ctx);
        if (!m->load_ms) m->read_ms += now_ms() - started;
        if (ok) ok = p->inflate(dst, &size, compressed, packed) == 0 && size == raw;
        free(compressed);
        if (ok) {
            started = now_ms();
            pst_sha256(dst, raw, digest);
            if (!m->load_ms) m->hash_ms += now_ms() - started;
            for (unsigned j = 0; j < 32; j++) snprintf(expected + 2*j, 3, "%02x", e[16+j]);
            ok = !strcmp(digest, expected);
        }
        if (!ok || (cancel && cancel(ctx))) {
            __atomic_store_n(&p->failed, 1, __ATOMIC_RELEASE);
            return -1;
        }
    }
    __atomic_store_n(&p->ready[page], 1, __ATOMIC_RELEASE);
    return 1;
}
int pst_media_ready(const struct pst_media *m)
{
    struct pst_pages *p = m->pages;
    if (!p) return 1;
    uint32_t page = __atomic_load_n(&p->requested, __ATOMIC_ACQUIRE);
    return !__atomic_load_n(&p->failed, __ATOMIC_ACQUIRE) && page < p->count &&
        __atomic_load_n(&p->ready[page], __ATOMIC_ACQUIRE);
}
static int pgrange(const struct pst_media *m, uint64_t first, uint64_t last)
{
    struct pst_pages *p = m->pages;
    if (!p) return 1;
    uint32_t a = first * 4 / p->bytes, b = last * 4 / p->bytes;
    __atomic_store_n(&p->requested, a, __ATOMIC_RELEASE);
    if (__atomic_load_n(&p->failed, __ATOMIC_ACQUIRE) || b >= p->count) return 0;
    for (uint32_t i = a; i <= b; i++)
        if (!__atomic_load_n(&p->ready[i], __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&p->requested, i, __ATOMIC_RELEASE);
            return 0;
        }
    return 1;
}
void pst_media_free(struct pst_media *m)
{
    if (m) {
        int p, s;
        if (m->pages) {
            for (p = 0; p < 2; p++) {
                if (m->pages->fd[p] >= 0) close(m->pages->fd[p]);
                free(m->pages->table[p]);
            }
            if (m->pages->zlib) dlclose(m->pages->zlib);
            free(m->pages->ready);
            free(m->pages);
        }
        free(m->vocal);
        free(m->instrumental);
        for (p = 0; p < 2; p++)
            for (s = 0; s < 3; s++)
                free(m->wave[p][s]);
        memset(m, 0, sizeof(*m));
    }
}
int pst_media_load(const char *track, uint32_t track_id, struct pst_media *out,
                   pst_cancel_fn cancel, void *ctx, char *error, size_t cap)
{
    struct json idx = {0}, man = {0};
    char root[PST_PATH_MAX], path[PST_PATH_MAX], id[16];
    char bundle[65], rel[PST_PATH_MAX], hash[65];
    const char *contents;
    const char *names[2] = {"stems-sidecar-vocal.s16le", "stems-sidecar-instrumental.s16le"};
    const char *problem = "NO STEMS";
    int entry, runtime, full, files, i, rc = -1;
    size_t size;
    uint64_t frames, bytes, started = now_ms();
    memset(out, 0, sizeof(*out));
    /* Derive the volume from the actual loaded track, never sda1 vs sdb1. */
    if (!track || !(contents = strstr(track, "/Contents/")) ||
        (size_t)(contents - track) >= sizeof(root))
        goto done;
    memcpy(root, track, (size_t)(contents - track));
    root[contents - track] = 0;
    if (strstr(track, "/../") || strstr(track, "/./") || !root[0])
        goto done;
    if (join(path, root, "CDJMODS/index.json") || json_read(path, &idx))
        goto done;
    problem = "BAD INDEX";
    if (!equal(&idx, 0, "schema", "overcue-index/1") || !equal(&idx, 0, "keyed_by", "export.pdb"))
        goto done;
    snprintf(id, sizeof(id), "%u", track_id);
    entry = field(&idx, field(&idx, 0, "tracks"), id);
    problem = "NO STEMS";
    if (entry < 0)
        goto done;
    problem = "TRACK MISMATCH";
    if (textfield(&idx, entry, "file_path", rel, sizeof(rel)) || strcmp(rel, contents))
        goto done;
    if (textfield(&idx, entry, "bundle", bundle, sizeof(bundle)) || strlen(bundle) != 16)
        goto done;
    for (i = 0; i < 16; i++)
        if (hexval(bundle[i]) < 0)
            goto done;
    if (snprintf(out->bundle_path, sizeof(out->bundle_path), "%s/CDJMODS/stems/%s", root, bundle) >=
        (int)sizeof(out->bundle_path))
        goto done;
    if (join(path, out->bundle_path, "overcue-manifest.json") || json_read(path, &man))
        goto done;
    problem = "BAD FORMAT";
    runtime = field(&man, 0, "runtime");
    full = field(&man, field(&man, 0, "sources"), "full");
    files = field(&man, 0, "sidecars");
    frames = number(&man, runtime, "frames");
    if (!equal(&man, 0, "schema", "overcue-sidecar-pair/1") || !equal(&man, 0, "bundle", bundle) ||
        !equal(&man, runtime, "format", "s16le") ||
        number(&man, runtime, "sample_rate") != PST_RATE ||
        number(&man, runtime, "channels") != 2 || !frames ||
        frames != number(&idx, entry, "frames") ||
        number(&man, runtime, "latency_pad_frames") > frames)
        goto done;
    problem = "TOO LARGE";
    if (frames > PST_MAX_BYTES / 8)
        goto done;
    bytes = frames * 4;
    problem = "TRACK MISMATCH";
    if (textfield(&man, full, "sha256", hash, sizeof(hash)) || !valid_hash(hash) ||
        strncmp(hash, bundle, 16) ||
        !track_size_ok(track, number(&man, full, "size_bytes")))
        goto done;
    out->frames = frames;
    /* Advertised paged exports fail closed if damaged; raw-only exports retain
     * their existing loader. Never silently fall back to a long bulk load. */
    if (field(&idx, entry, "page_bytes") >= 0) {
        uint64_t page_bytes = number(&idx, entry, "page_bytes");
        problem = "BAD PGZ";
        if (page_bytes < 4096 || page_bytes > 4 * 1024 * 1024 || page_bytes % 4) goto done;
        out->pages = calloc(1, sizeof(*out->pages));
        if (!out->pages) goto done;
        struct pst_pages *p = out->pages;
        p->fd[0] = p->fd[1] = -1;
        p->bytes = page_bytes;
        p->count = (bytes + page_bytes - 1) / page_bytes;
        if (p->count > 65536) goto done;
        p->ready = calloc(p->count, 1);
        out->vocal = malloc(bytes);
        out->instrumental = malloc(bytes);
#ifdef __APPLE__
        p->zlib = dlopen("/usr/lib/libz.dylib", RTLD_NOW | RTLD_LOCAL);
#else
        p->zlib = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
        if (p->zlib) p->inflate = dlsym(p->zlib, "uncompress");
        if (!p->ready || !out->vocal || !out->instrumental || !p->inflate) goto done;
        for (i = 0; i < 2; i++) {
            char leaf[80];
            snprintf(leaf, sizeof(leaf), "%s.pgz", names[i]);
            int f = field(&man, files, leaf);
            if (textfield(&idx, entry, i ? "instrumental_page_table_sha256" :
                          "vocal_page_table_sha256", hash, sizeof(hash)) || !valid_hash(hash) ||
                join(path, out->bundle_path, leaf) ||
                !pgopen(out, i, path, number(&man, f, "bytes"), hash, cancel, ctx)) goto done;
        }
        if (pst_media_service(out, cancel, ctx) != 1) goto done;
    } else
    for (i = 0; i < 2; i++) {
        int f = field(&man, files, names[i]);
        int16_t *pcm;
        problem = "STEM METADATA";
        if (number(&man, f, "bytes") != bytes || textfield(&man, f, "sha256", hash, sizeof(hash)) ||
            !valid_hash(hash) ||
            !equal(&idx, entry, i ? "instrumental_sha256" : "vocal_sha256", hash) ||
            join(path, out->bundle_path, names[i]))
            goto done;
        /* Load prepared PCM once. Keep manifest/index agreement above, but
         * do not repeat export-time full-audio hashing on every track load. */
        problem = "STEM SIZE";
        pcm = media_read(out, path, (size_t)bytes, &size, cancel, ctx);
        if (i)
            out->instrumental = pcm;
        else
            out->vocal = pcm;
        if (!pcm || size != bytes)
            goto done;
    }
    /* Older bundles without waveform metadata remain playable. If metadata is
     * supplied, every named display file must validate before we publish. */
    if (field(&man, 0, "waveforms") >= 0) {
        int part, ext;
        for (part = 0; part < 2; part++)
            for (ext = 0; ext < 2; ext++) {
                char name[64];
                uint8_t *data;
                int f, s;
                snprintf(name, sizeof(name), "stems-%s-waveform.%s",
                         part ? "instrumental" : "vocal", ext ? "2EX" : "EXT");
                f = field(&man, field(&man, 0, "waveforms"), name);
                problem = "BAD WAVEFORM";
                if (textfield(&man, f, "sha256", hash, sizeof(hash)) ||
                    join(path, out->bundle_path, name))
                    goto done;
                data = media_read(out, path, 4 * 1024 * 1024, &size, cancel, ctx);
                if (!data || size != number(&man, f, "bytes") ||
                    !media_hash(out, data, size, hash)) {
                    free(data);
                    goto done;
                }
                for (s = ext ? 0 : 1; s < (ext ? 1 : 3); s++)
                    if (!pst_wave_decode(data, size, s, &out->wave[part][s],
                                         &out->points[part][s])) {
                        free(data);
                        goto done;
                    }
                free(data);
            }
    }
    if (cancel && cancel(ctx)) {
        problem = "CANCELLED";
        goto done;
    }
    out->load_ms = now_ms() - started;
    if (!out->load_ms) out->load_ms = 1;
    out->frames = frames;
    problem = "READY";
    rc = 0;
done:
    json_free(&idx);
    json_free(&man);
    if (rc)
        pst_media_free(out);
    if (error && cap)
        snprintf(error, cap, "%s", problem);
    return rc;
}
int pst_media_mix(const struct pst_media *m, float *dst, int64_t pos, int64_t frames, int rate,
                  unsigned selection)
{
    int64_t i;
    double step, last;
    if (!m || !dst || !m->vocal || !m->instrumental || pos < 0 || frames <= 0 || frames > 262144 ||
        selection > 3)
        return 0;
    if (rate != 44100 && rate != 48000 && rate != 88200 && rate != 96000 && rate != 176400 &&
        rate != 192000)
        return 0;
    step = (double)PST_RATE / rate;
    last = ((double)pos + frames - 1) * step;
    if (last >= (double)m->frames)
        return 0; /* fail whole block to stock */
    uint64_t end = (uint64_t)last;
    if (end + 1 < m->frames) end++;
    int range_ready = pgrange(m, (uint64_t)((double)pos * step), end);
    if (selection == 3) return 1;
    if (!range_ready) return 0;
    for (i = 0; i < frames; i++) {
        double p = ((double)pos + i) * step;
        uint64_t a = (uint64_t)p, b = a + 1 < m->frames ? a + 1 : a;
        float t = (float)(p - a);
        int ch;
        for (ch = 0; ch < 2; ch++) {
            float sample = 0;
            if (selection & 1)
                sample += (1 - t) * m->vocal[2 * a + ch] + t * m->vocal[2 * b + ch];
            if (selection & 2)
                sample += (1 - t) * m->instrumental[2 * a + ch] + t * m->instrumental[2 * b + ch];
            dst[2 * i + ch] = sample * (1.0f / 32768.0f);
        }
    }
    return 1;
}
