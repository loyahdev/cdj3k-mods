// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/djdb_dump.c - naming and dumping the deck's own table registry, for the debug log.
 */
#include "db/djdb_internal.h"

/* Decode a packed djdb string into `out`. 0 on success. */
static int djdb_name(uintptr_t p, char *out, size_t cap)
{
    uint32_t hdr = 0;
    uintptr_t at;
    size_t len;

    out[0] = '\0';
    if (!p || mod_safe_read(p, &hdr, sizeof(hdr)) != 0)
        return -1;

    if (hdr & 1) {                       /* short: one-byte header */
        len = ((hdr & 0xff) >> 1);
        if (len < 1)
            return -1;
        len -= 1;
        at = p + 1;
    } else if ((hdr & 0xff) == 0x40) {   /* long: four-byte header */
        len = hdr >> 8;
        if (len < 4)
            return -1;
        len -= 4;
        at = p + 4;
    } else {
        return -1;
    }
    if (len >= cap)
        len = cap - 1;
    if (mod_safe_read(at, out, len) != 0)
        return -1;
    out[len] = '\0';
    return 0;
}

/* One table: its name, and every column's type. */
static void djdb_dump_table(uintptr_t tbl)
{
    char name[DJDB_NAME_MAX], line[256];
    uintptr_t nameptr = 0, cols = 0;
    int32_t ncol = 0;
    int i, n = 0;

    if (mod_safe_read(tbl + DJDB_TBL_NAME_OFF, &nameptr, sizeof(nameptr)) != 0 ||
        djdb_name(nameptr, name, sizeof(name)) != 0)
        return;

    /* The count sub_1c66f80 compares against before it walks the columns. */
    if (mod_safe_read(tbl + 0x04, &ncol, sizeof(ncol)) != 0 ||
        mod_safe_read(tbl + DJDB_TBL_COLS_OFF, &cols, sizeof(cols)) != 0 ||
        !cols || ncol <= 0 || ncol > DJDB_COLS_MAX) {
        MDBG("djdb:   %-24s %d columns (not walked)\n", name, (int)ncol);
        return;
    }

    for (i = 0; i < ncol; i++) {
        uintptr_t typeobj = 0;
        int32_t type = -1;

        if (mod_safe_read(cols + (uintptr_t)i * DJDB_COL_STRIDE + DJDB_COL_TYPE_OFF,
                          &typeobj, sizeof(typeobj)) == 0 && typeobj)
            (void)mod_safe_read(typeobj + 8, &type, sizeof(type));
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%d",
                      i ? " " : "", (int)type);
        if ((size_t)n >= sizeof(line) - 8) {
            snprintf(line + n, sizeof(line) - (size_t)n, " ...");
            break;
        }
    }
    MDBG("djdb:   %-24s %2d cols, types [%s]\n", name, (int)ncol, line);

    /* COLUMN NAMES, for the tables a writer actually needs. A type list says a
     * playlist entry is three integers; it does not say which one is the order,
     * and guessing that is how a DJ's playlist ends up shuffled. The name is a
     * packed string at col+0x00 -- found by dumping the whole 32-byte entry and
     * seeing which field decoded for every column.
     *
     * Named tables only, so this stays a readable block rather than 74 of them. */
    /* EXACT names: "DJDBCONTENT" is a prefix of DJDBCONTENTPREPARE and a
     * substring of DJDBEXCONTENTOPTION, so a strstr here quietly dumps three
     * tables and the interesting one scrolls off. */
    if (strcmp(name, "DJDBSONGPLAYLIST") != 0 &&
        strcmp(name, "DJDBPLAYLIST") != 0 &&
        strcmp(name, "DJDBCONTENT") != 0)
        return;

    n = 0;
    for (i = 0; i < ncol; i++) {
        uintptr_t nptr = 0;
        char cn[DJDB_NAME_MAX];

        cn[0] = '\0';
        if (mod_safe_read(cols + (uintptr_t)i * DJDB_COL_STRIDE, &nptr,
                          sizeof(nptr)) == 0 && nptr > 0x10000)
            (void)djdb_name(nptr, cn, sizeof(cn));

        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%d:%s",
                      i ? " " : "", i, cn[0] ? cn : "?");
        /* Flushed in chunks: one line of fifty names is unreadable and the
         * journal truncates it anyway. */
        if ((size_t)n >= sizeof(line) - 40 || i == ncol - 1) {
            MDBG("djdb:     %s\n", line);
            n = 0;
            line[0] = '\0';
        }
    }

    /* THE INDEXES, and WHICH COLUMNS EACH COVERS. A write addresses a row
     * through an index, so this decides what a key even is: the deck's own
     * playlist cursor passes ONE key to idxSongPlaylist and gets a whole
     * playlist back, which means that key is a prefix and the index is
     * compound. Whether its second column is TRACKNO or CONTENTID is the
     * difference between addressing a row by its position and addressing it by
     * its track, and a reorder wants the first. */
    {
        uintptr_t idx = 0;
        int guard = 0;

        if (mod_safe_read(tbl + DJDB_TBL_IDX_OFF, &idx, sizeof(idx)) != 0)
            return;
        while (idx && guard++ < 16) {
            uintptr_t nptr = 0, next = 0;
            char in[DJDB_NAME_MAX];
            int32_t w[DJDB_IDX_WORDS];
            int j;

            in[0] = '\0';
            if (mod_safe_read(idx + DJDB_IDX_NAME_OFF, &nptr, sizeof(nptr)) == 0)
                (void)djdb_name(nptr, in, sizeof(in));
            if (mod_safe_read(idx, w, sizeof(w)) != 0)
                break;
            /* The pointer slots, decoded as column descriptors: a column's name
             * is a packed string at its +0x00, so a slot that names a column is
             * a key column and one that does not is something else. */
            n = 0;
            for (j = 0x08; j <= 0x38; j += 8) {
                uintptr_t p = 0;
                char cn[DJDB_NAME_MAX];

                cn[0] = '\0';
                if (mod_safe_read(idx + (unsigned)j, &p, sizeof(p)) == 0 &&
                    p > 0x10000)
                    (void)djdb_name(p, cn, sizeof(cn));
                n += snprintf(line + n, sizeof(line) - (size_t)n, " +%02x:%s",
                              j, cn[0] ? cn : "-");
            }
            MDBG("djdb:     index %-22s keys=%d%s\n",
                 in[0] ? in : "?", (int)w[0], line);
            if (mod_safe_read(idx + DJDB_IDX_NEXT_OFF, &next, sizeof(next)) != 0)
                break;
            idx = next;
        }
    }
}

/* Walk the registry once and say what is in it. */
static void djdb_dump(void *ctx)
{
    int b, tables = 0;

    for (b = 0; b < DJDB_BUCKETS; b++) {
        uintptr_t t = 0;

        if (mod_safe_read((uintptr_t)ctx + DJDB_BUCKETS_OFF +
                          (uintptr_t)b * sizeof(t), &t, sizeof(t)) != 0)
            continue;
        /* The chain, with a bound: a corrupt or misread `next` must not spin. */
        while (t && tables < 256) {
            uintptr_t next = 0;

            djdb_dump_table(t);
            tables++;
            if (mod_safe_read(t + DJDB_TBL_NEXT_OFF, &next, sizeof(next)) != 0)
                break;
            t = next;
        }
    }
    MDBG("djdb: %d tables in the registry\n", tables);
}

void djdb_try_dump(const char *where, const char *table)
{
    static int dumped;
    void *ctx;

    if (dumped)
        return;
    dumped = 1;

    ctx = djdb_ctx();
    if (!ctx) {
        MDBG("djdb: inside %s but the accessor answers NULL -- the context is"
             " reached some other way\n", where);
        return;
    }
    MDBG("djdb: context %p live inside %s%s%s -- the media library's tables:\n",
         ctx, where, table ? " on " : "", table ? table : "");
    djdb_dump(ctx);
}

/* A playlist id seen going past on a djdbSongPlaylist cursor, which
 * idxSongPlaylist takes ALONE, so the single key is it. Cleared by nothing: it
 * is a fallback for the moment before the collector has been seen, and the id
 * that gets used is the one the cache names. */
/* EVERY query's table, in order, with consecutive repeats collapsed.
 *
 * The playlist latch is held over a timeout rather than dropped when some other
 * list is filled, and that is wrong in a way the deck showed: it CACHES a list,
 * so returning to a playlist already visited re-queries nothing and the id is
 * never re-stated. Which query fills what -- and whether a cached re-entry
 * queries at all -- is the thing to know before any rule can replace the
 * timeout, and only the deck can say. Consecutive repeats collapse because a
 * list fill is one table asked many times. */
void djdb_note_table(const char *name)
{
    static char last[24];
    static unsigned run;

    if (!strcmp(last, name)) {
        run++;
        return;
    }
    if (run)
        MDBG("djdb: query   ... x%u\n", run + 1);
    run = 0;
    snprintf(last, sizeof(last), "%s", name);
    MDBG("djdb: query   %s\n", name);
}

/* The context slot holds a pointer and a flag. The flag is always set, so every
 * context this firmware hands out is manufactured by the function beside it --
 * which is why the plain pointer is always NULL and caching one is not a route.
 * Both halves are reported here so the producer can be named. */
void djdb_report_slot(void)
{
    uintptr_t slot = ep122_sym(EP122_DJDB_CONTEXT_SLOT);
    uintptr_t raw = 0, arg = 0, fn = 0;
    uint8_t   flag = 0;

    if (!slot)
        return;
    (void)mod_safe_read(slot, &flag, sizeof(flag));
    (void)mod_safe_read(slot + 0x08, &raw, sizeof(raw));
    (void)mod_safe_read(slot + 0x10, &arg, sizeof(arg));
    (void)mod_safe_read(slot + 0x18, &fn, sizeof(fn));
    MDBG("djdb: context slot: override %u, raw %#lx, fn %#lx(arg %#lx)\n",
         flag, (unsigned long)raw, (unsigned long)fn, (unsigned long)arg);
}
