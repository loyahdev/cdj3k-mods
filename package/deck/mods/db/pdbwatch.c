// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/pdbwatch.c - when does the deck actually write a DJ's library?
 *
 * THE QUESTION THIS EXISTS TO SETTLE. djdb.c watches the entry points the Dsql
 * family calls, and in a whole session of browsing, loading and cueing, not one
 * of them fired. The library files do change, so the deck persists somehow --
 * just not through anything we had a name for.
 *
 * Statics were guessing at that. This does not: the shim already interposes
 * open() and write(), so it can watch the file itself and report WHEN it is
 * opened for writing, HOW MUCH is written, and -- from the return address --
 * WHICH code did it. One measured caller beats any number of plausible ones.
 *
 * OBSERVATION ONLY. Nothing here changes a flag, a path or a byte; every call
 * is on its way to the real syscall either way.
 *
 * The volume is bounded by construction: only paths under a rekordbox library
 * are watched at all, and a repeated write from a return address already seen
 * is counted rather than printed. So a busy flush is a handful of lines.
 */
#include "db/db.h"

#include "core/ep122_syms.h"
#include "kit/mod.h"
#include "core/mod_core.h"

/* Both media libraries: the rekordbox device files and the SQLCipher one that
 * sits beside them, because "which of these actually moves" is half the
 * question. */
#define PDB_WATCH_MAX     8
#define PDB_NAME_MAX      24
#define PDB_SITES_MAX     4

struct pdb_watch {
    int       fd;                      /* -1 when the slot is free */
    char      name[PDB_NAME_MAX];
    uint64_t  bytes;
    unsigned  writes;
    uintptr_t site[PDB_SITES_MAX];     /* distinct return addresses seen */
    int       nsite;
};

static struct pdb_watch pdb_g[PDB_WATCH_MAX];
static int pdb_g_ready;

/* The tail of a path, for a log line that fits. */
static const char *pdb_base(const char *path)
{
    const char *s = path, *last = path;

    for (; *s; s++)
        if (*s == '/')
            last = s + 1;
    return last;
}

/* Is this one of the library files?
 *
 * The whole PIONEER directory, not just the rekordbox one inside it: a track's
 * beat grid is not in the library at all, it is the Quantize atom of that
 * track's ANLZ under PIONEER/USBANLZ -- so registering a grid moves a file this
 * would not have been watching. Two directories, one prefix. */
static int pdb_interesting(const char *path)
{
    return strstr(path, "/PIONEER/") != NULL ||
           strstr(path, "exportLibrary.db") != NULL;
}

void db_watch_open(const char *path, int fd, int flags)
{
    int i;

    if (!path || fd < 0 || !pdb_interesting(path))
        return;

    if (!pdb_g_ready) {
        for (i = 0; i < PDB_WATCH_MAX; i++)
            pdb_g[i].fd = -1;
        pdb_g_ready = 1;
    }

    /* The FLAGS are the interesting half of this line: a read-only open is the
     * deck reading its library, and a writable one is the moment we are looking
     * for -- which is worth seeing even if no write ever follows it. */
    MDBG("pdb: open %s fd %d flags %#x (%s)\n", pdb_base(path), fd, flags,
         (flags & 3) ? "WRITABLE" : "read-only");

    if (!(flags & 3))
        return;                        /* nothing to attribute */

    for (i = 0; i < PDB_WATCH_MAX; i++) {
        if (pdb_g[i].fd >= 0)
            continue;
        pdb_g[i].fd     = fd;
        pdb_g[i].bytes  = 0;
        pdb_g[i].writes = 0;
        pdb_g[i].nsite  = 0;
        snprintf(pdb_g[i].name, sizeof(pdb_g[i].name), "%s", pdb_base(path));
        return;
    }
}

void db_watch_write(int fd, size_t count, uintptr_t ra)
{
    int i, s;

    if (!pdb_g_ready || fd < 0)
        return;

    for (i = 0; i < PDB_WATCH_MAX; i++) {
        if (pdb_g[i].fd != fd)
            continue;

        pdb_g[i].bytes += count;
        pdb_g[i].writes++;

        /* EVERY write, not just the first from each site. Measured: a whole
         * session is three writes per file -- one at mount and the rest at
         * eject -- so the volume argument for suppressing them was wrong, and
         * WHEN each one happens is the entire question. The site is still
         * tracked, because a second one appearing would mean a second writer. */
        for (s = 0; s < pdb_g[i].nsite; s++)
            if (pdb_g[i].site[s] == ra)
                break;
        if (s == pdb_g[i].nsite && pdb_g[i].nsite < PDB_SITES_MAX)
            pdb_g[i].site[pdb_g[i].nsite++] = ra;

        MDBG("pdb: %s write #%u, %zu bytes, from %#lx%s\n", pdb_g[i].name,
             pdb_g[i].writes, count, (unsigned long)ra,
             s == pdb_g[i].nsite - 1 && s > 0 ? "  <- a NEW writer" : "");
        return;
    }
}

void db_watch_close(int fd)
{
    int i;

    if (!pdb_g_ready || fd < 0)
        return;

    for (i = 0; i < PDB_WATCH_MAX; i++) {
        if (pdb_g[i].fd != fd)
            continue;
        if (pdb_g[i].writes)
            MDBG("pdb: close %s -- %u writes, %llu bytes\n", pdb_g[i].name,
                 pdb_g[i].writes, (unsigned long long)pdb_g[i].bytes);
        pdb_g[i].fd = -1;
        return;
    }
}

/* ---- the half that is NOT a syscall ---------------------------------------
 *
 * Everything above hangs off the shim's own open() and write(), and for a long
 * time that looked like the whole picture: the library files were seen opening
 * and being written, and nothing else was. It was not the whole picture. A beat
 * grid registered through the repository lands in the track's ANLZ, that file
 * grew by 13 kB while this said nothing at all, and widening the path match
 * changed nothing.
 *
 * THE DB ENGINE USES STDIO. track_info_repository::CFpHandle opens with fopen
 * and writes with fwrite, and glibc calls open64/write from INSIDE those --
 * bound within libc, not through EP122's PLT, which is the only place an
 * LD_PRELOAD can stand. So those writes were never going to appear here, and
 * interposing fopen/fwrite is not open to this shim either: it carries no dlsym
 * by design, and every passthrough is a raw syscall.
 *
 * The handle's own methods are virtual, so they are taken the way everything
 * else here is taken -- by slot. That is also the better tap: the return
 * address is the DB layer's caller rather than a frame inside libc.
 *
 * OBSERVATION ONLY, like the rest of this file. Each wrapper calls the stock
 * one and reports what it saw.
 */

/* Where a CFpHandle keeps its FILE*. Read out of its own open, which stores the
 * fopen result there and fcloses whatever was there before. */
#define PDB_FP_OFF        0x28

/* Its three I/O slots, each named by the only libc call it reaches. */
#define PDB_FP_SLOT_OPEN  0x38
#define PDB_FP_SLOT_CLOSE 0x40
#define PDB_FP_SLOT_WRITE 0x78

static uintptr_t pdb_g_fp_open, pdb_g_fp_close, pdb_g_fp_write;

/* The fd behind the handle, or -1. Everything above is keyed on one, and stdio
 * has one underneath -- so the two halves land in the same table rather than
 * needing a second. */
static int pdb_fp_fd(uintptr_t handle)
{
    uintptr_t fp = 0;

    if (!handle ||
        mod_safe_read(handle + PDB_FP_OFF, &fp, sizeof(fp)) != 0 || !fp)
        return -1;
    return fileno((FILE *)fp);
}

static uint64_t pdb_wrap_fp_open(uintptr_t self, void *src, int mode, void *err)
{
    /* A handle can be re-opened, and its own open fcloses whatever it was
     * holding first. Retire that entry here or its fd number comes back on
     * another file and every write to it is reported under the old name. */
    int was = pdb_fp_fd(self);
    uint64_t r;
    char link[32], path[512];
    int  fd;
    ssize_t n;

    if (was >= 0)
        db_watch_close(was);
    r = ((uint64_t (*)(uintptr_t, void *, int, void *))
         pdb_g_fp_open)(self, src, mode, err);
    fd = pdb_fp_fd(self);

    if (fd < 0)
        return r;
    /* The path from the fd rather than from the argument: the caller passes an
     * object the handle downcasts to reach the string, and asking the kernel
     * what the descriptor is open on needs none of that. The flags likewise --
     * fopen's mode is an index into a table of its own, and F_GETFL is what the
     * file actually got. */
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    n = readlink(link, path, sizeof(path) - 1);
    if (n <= 0)
        return r;
    path[n] = '\0';
    db_watch_open(path, fd, fcntl(fd, F_GETFL));
    return r;
}

static uint64_t pdb_wrap_fp_write(uintptr_t self, const void *buf, size_t len)
{
    int fd = pdb_fp_fd(self);

    if (fd >= 0)
        db_watch_write(fd, len, (uintptr_t)__builtin_return_address(0));
    return ((uint64_t (*)(uintptr_t, const void *, size_t))
            pdb_g_fp_write)(self, buf, len);
}

static uint64_t pdb_wrap_fp_close(uintptr_t self)
{
    /* Before, not after: the fd is gone once the stock one has run. */
    int fd = pdb_fp_fd(self);

    if (fd >= 0)
        db_watch_close(fd);
    return ((uint64_t (*)(uintptr_t))pdb_g_fp_close)(self);
}

/* The open is the one that matters: without a path nothing else has a name, so
 * a failure there is the whole mod failing. The other two are taken if they can
 * be -- a watch that knows a file opened is worth more than none. */
static int pdb_install(void)
{
    if (mod_patch_vslot("dbFpOpen", EP122_DB_FPHANDLE, PDB_FP_SLOT_OPEN,
                        (void *)pdb_wrap_fp_open, &pdb_g_fp_open) != 0) {
        MDBG("pdb: no CFpHandle -> the DB engine's own files stay unwatched\n");
        return -1;
    }
    (void)mod_patch_vslot("dbFpWrite", EP122_DB_FPHANDLE, PDB_FP_SLOT_WRITE,
                          (void *)pdb_wrap_fp_write, &pdb_g_fp_write);
    (void)mod_patch_vslot("dbFpClose", EP122_DB_FPHANDLE, PDB_FP_SLOT_CLOSE,
                          (void *)pdb_wrap_fp_close, &pdb_g_fp_close);
    return 0;
}

KIT_MOD(k_mod_pdbwatch,
        .name = "pdbwatch", .prio = 62, .install = pdb_install,
        .what = "library files: watch the DB engine's own stdio handle too");
