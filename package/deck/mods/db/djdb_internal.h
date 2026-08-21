/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/db/djdb_internal.h - what the djdb_*.c files share.
 *
 * The provider is split by job: djdb.c owns the hooks, the trampolines and the
 * write path, djdb_playlist.c the browser's open list, djdb_dump.c the debug
 * dump. Nothing here leaves the db/ directory -- db.h is the outward face.
 */
#ifndef EP122_MOD_DJDB_INTERNAL_H
#define EP122_MOD_DJDB_INTERNAL_H

#include "core/mod_core.h"
#include "db/db.h"

/* The content table, which carries the tempo column. */
#define DJDB_CONTENT_TABLE  "djdbContent"
#define DJDB_CONTENT_INDEX  "idxContent"
#define DJDB_COL_BPM        8

/* The deck's own table, index and column layout, read by every file here. */
/* The registry, as sub_1c915d0 indexes it. */
#define DJDB_BUCKETS        13
#define DJDB_BUCKETS_OFF    0x08
#define DJDB_TBL_NAME_OFF   0x10
#define DJDB_TBL_COLS_OFF   0x18
#define DJDB_TBL_NEXT_OFF   0x78
#define DJDB_COL_STRIDE     0x20
#define DJDB_COL_TYPE_OFF   0x08
/* The index chain, as sub_1c92000 walks it: a table's first index, each index's
 * packed name, and the next in the chain. Which COLUMNS an index covers is not
 * named by that function, so the words around the name are dumped and read --
 * the same way the column names themselves were found. */
#define DJDB_TBL_IDX_OFF    0x20
#define DJDB_IDX_NAME_OFF   0x40
#define DJDB_IDX_NEXT_OFF   0x58
#define DJDB_IDX_WORDS      14        /* int32s from the index, for reading */
/* Enough for any table name the image carries; the longest is 22 characters. */
#define DJDB_NAME_MAX       64
/* A column count past this is a pointer that happened to be readable rather than
 * a table, and walking it would be reading arbitrary memory. */
#define DJDB_COLS_MAX       128
/* djdbSongPlaylist: PLAYLISTID, CONTENTID, TRACKNO as COLUMNS 0, 1, 2 -- and
 * fields 2, 1, 0 on disk, which is the reversal the walk above exists to
 * disambiguate. Lower-camel as sub_1c66f80 takes it. */
#define DJDB_PLAYLIST_TABLE "djdbSongPlaylist"
#define DJDB_PLAYLIST_INDEX "idxSongPlaylist"
#define DJDB_COL_CONTENTID  1
#define DJDB_COL_TRACKNO    2
/* ---- walking a playlist, read-only ----------------------------------------
 *
 * sub_1c67420 is the QUERY: it finds the rows an index key matches AND for which
 * a row filter returns true. The filter is what makes a reorder possible at all
 * -- idxSongPlaylist keys on PLAYLISTID alone, so a key selects a whole playlist
 * and only the filter narrows it to one entry.
 *
 * THIS WALK SELECTS NOTHING. The filter logs and returns false for every row, so
 * the query matches none and does nothing; the point is the callback, which sees
 * each row and can read its columns.
 *
 * WHAT IT IS FOR: sub_1c5c1b0(row, n) takes an index, and the COLUMN order and
 * the ON-DISK FIELD order of djdbSongPlaylist are reversed -- columns are
 * PLAYLISTID/CONTENTID/TRACKNO = 0/1/2, fields are 2/1/0. Filtering on the wrong
 * one would silently match the wrong column, so all three are read and printed
 * and the answer comes from comparing them with the library on the stick rather
 * than from assuming which convention this call uses.
 *
 * Threading: [a library server thread] -- the query needs a context like every
 * other djdb call. */
/* WHY THE TEST TABLE IS djdbContent AND NOT THE PLAYLIST. This stick's only
 * playlist holds contents 1..6 with track numbers 1..6, so CONTENTID equals
 * TRACKNO in every row and reading them cannot tell the two conventions apart.
 * djdbContent cannot be confused: column 0 is the ID (7, 8, 11 here) and column
 * 8 is the BPM (12600, 12000, 12500), and that column numbering is already
 * PROVEN -- our own tempo write lands with colid 8. So if index 8 reads back a
 * tempo, this call takes COLUMN indexes; if it reads something else, it takes
 * the on-disk field index. */
#define DJDB_WALK_COLS  4
/* ---- the reorder ----------------------------------------------------------
 *
 * One entry's position is `DJDBSONGPLAYLIST.TRACKNO`, and it is addressed by the
 * playlist key plus a ROW FILTER on CONTENTID -- the index alone selects the
 * whole playlist. Otherwise this is the same call, converter and transaction as
 * the tempo write, so it inherits its durability: pages are marked dirty and hit
 * the media synchronously, which is why a yanked stick keeps the change.
 *
 * A MOVE RENUMBERS A RANGE, not one row. Taking position 6 to position 2 shifts
 * 2..5 down by one, so the whole affected span is rewritten. Entries outside it
 * are left alone rather than rewritten with the value they already hold: fewer
 * writes, and nothing to undo if one fails midway.
 *
 * Threading: [a library server thread] -- like every djdb call, it needs the
 * per-thread context. */
#define DJDB_PL_MAX  1024
/* HOW MANY PLAYLISTS THIS MEDIUM HAS, and which one if there is only one.
 *
 * The last resort under mod_djdb_playlist_now, for a medium whose list cache has
 * not been reached: with one playlist the question does not arise, and the id is
 * then a fact about the medium rather than a guess about the screen. With more
 * than one it answers 0, and the caller falls through to a refusal.
 *
 * Ids are dense from 1 on this format, so the scan stops after a run of empties
 * rather than probing to the moon. [a library server thread] */
#define DJDB_PLAYLIST_SCAN_MAX  256
#define DJDB_PLAYLIST_SCAN_GAP  16
/* ---- telling the deck its cached copy of that list is stale ----------------
 *
 * A reorder that only reaches the database is a reorder nothing on screen ever
 * shows: the deck warms a list cache when the media is announced and browses out
 * of it, sorting in memory from the rows it already holds. So the rows have to
 * be dropped, and the deck has its own call for exactly that --
 * music_library::ListCacheCollector::removePlaylistTrackListCache(id).
 *
 * The COLLECTOR is the problem, not the call: it is a member of the
 * InformationUpdater, which nothing we hold can reach. So the call is hooked to
 * learn it. The deck invalidates every playlist's rows when it builds the
 * library for a mounted medium, which is both a moment we are certain of and the
 * one before any reorder can be asked for.
 *
 * Called from the drain, on the library thread, which is the thread the deck
 * makes this call on itself. */
/* The browse category the deck names when it drops a playlist's hierarchy
 * cache; read off its own delete path, which passes this constant. */
#define DJDB_CATEGORY_PLAYLIST 5

typedef int (*djdb_query_fn)(const char *table, const char *index,
                            void *rowfn, void *rowarg, const char *op,
                            int nkeys, void **keyvals);

struct djdb_entry {
    int32_t content;
    int32_t no;
};

struct djdb_plist {
    struct djdb_entry e[DJDB_PL_MAX];
    int n;
};

/* The deck's per-thread database handle, or NULL outside one of its own
 * operations. */
void *djdb_ctx(void);

/* Run `index` over `table` for `key` and log the four columns named. */
void djdb_walk(const char *what, const char *table, const char *index,
               uint32_t key, int c0, int c1, int c2, int c3);

/* Collect a playlist's rows into a struct djdb_plist. */
int djdb_collect_row(void *ctx, uintptr_t row);

/* Report both halves of the context slot, naming the function that makes one. */
void djdb_report_slot(void);

/* Dump the registry once per (call site, table), when the debug log is on. */
void djdb_try_dump(const char *where, const char *table);

/* Walk the playlist tables once, when the debug log is on. */
void djdb_try_walk(void);

/* Latch the playlist a hooked call names. */
void djdb_note_playlist(const char *table, int nkeys, void **keyvals);

/* Note a table name seen on a hooked call, for the dump. */
void djdb_note_table(const char *name);

/* Read one playlist's rows into `pl`. 0 on success. */
int djdb_read_playlist(uint32_t pid, struct djdb_plist *pl);

/* Which playlist the browser has open, from the deck's own list caches. */
uint32_t djdb_playlist_from_caches(void);

#endif /* EP122_MOD_DJDB_INTERNAL_H */
