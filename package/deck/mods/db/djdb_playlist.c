// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/djdb_playlist.c - which playlist the browser has open, from the deck's own caches.
 */
#include "db/djdb_internal.h"

static void djdb_count_playlists(void);

/* Which list the browser has open, and whether this deck has only one. */
/* Set while this file is querying on its own behalf. The playlist latch watches
 * the same hook our own reads go through, so without this the reorder's idea of
 * "the playlist on screen" is really "the one djdb_try_walk asked for". */
static int djdb_g_ours;
/* The medium's only playlist, when it has exactly one -- see
 * djdb_count_playlists and mod_djdb_playlist_now. */
static uint32_t djdb_g_sole_playlist;
static int      djdb_g_playlists;
/* ---- the playlist the deck is showing, and a move waiting for a thread ----
 *
 * A reorder needs a PLAYLIST ID and the UI has none: the browse view knows a
 * hierarchy, not a table key. The id comes off the list cache the deck is
 * serving the list from -- see djdb_playlist_from_caches -- and this latch is
 * only what a djdbSongPlaylist cursor happened to name, kept as a fallback for
 * before any cache has been seen.
 *
 * The move itself is QUEUED, never tried inline: it is asked for from the
 * message thread, which has no djdb context by construction, so an inline
 * attempt could only fail. It lands on the next library message, the same way a
 * held tempo does. */
static uint32_t djdb_g_seen_playlist;

/* Once per run, from whichever TABLE-LEVEL hook first brings a context. Not from
 * the page writer: that is a layer below, and a query nested inside a page flush
 * re-enters the pool underneath it. The guard matters because the query itself is
 * hooked and would otherwise re-enter here.
 *
 * READ-ONLY, and that is the point. This walk once ended with a move -- swap
 * entries 7 and 8 of playlist 1 -- as the smallest proof that the write worked.
 * It did prove it, and it also REORDERED THE DJ'S PLAYLIST ON EVERY BOOT, which
 * is what the order that differed between two mounts of an untouched stick was.
 * A probe on the library's read side is free; one on its write side is a change
 * to somebody's media that nobody asked for. The gesture proves the write now. */
void djdb_try_walk(void)
{
    static int done;

    if (done || !djdb_ctx())
        return;
    done = 1;
    /* The convention test: ID and BPM of a track we know cold. */
    djdb_walk("content", DJDB_CONTENT_TABLE, DJDB_CONTENT_INDEX, 7,
              0, DJDB_COL_BPM, 15, -1);
    /* And the playlist, for its row count and its constant column. Marked as
     * OURS while it runs: this query goes through the same hook that watches
     * for the deck's playlist cursor, so without the guard the reorder's idea of
     * "which playlist is on screen" is really "the one this probe asked for". */
    djdb_g_ours = 1;
    djdb_walk("songplaylist", DJDB_PLAYLIST_TABLE, DJDB_PLAYLIST_INDEX, 1,
              0, 1, 2, -1);
    djdb_count_playlists();
    djdb_g_ours = 0;
}

/* The playlist's entries, in whatever order the cursor yields them. */
int djdb_read_playlist(uint32_t pid, struct djdb_plist *pl)
{
    uintptr_t fn = ep122_sym(EP122_DJDB_QUERY);
    uint32_t key = pid;
    void *keyvals[1];

    pl->n = 0;
    if (!fn)
        return -1;
    keyvals[0] = &key;
    (void)((djdb_query_fn)fn)(DJDB_PLAYLIST_TABLE, DJDB_PLAYLIST_INDEX,
                              (void *)djdb_collect_row, pl, "=", 1, keyvals);
    return pl->n;
}

static void djdb_count_playlists(void)
{
    static struct djdb_plist pl;
    uint32_t id;
    int gap = 0;

    djdb_g_playlists = 0;
    djdb_g_sole_playlist = 0;
    for (id = 1; id <= DJDB_PLAYLIST_SCAN_MAX && gap < DJDB_PLAYLIST_SCAN_GAP;
         id++) {
        if (djdb_read_playlist(id, &pl) > 0) {
            gap = 0;
            if (++djdb_g_playlists == 1)
                djdb_g_sole_playlist = id;
        } else {
            gap++;
        }
    }
    if (djdb_g_playlists == 1)
        MDBG("djdb: this medium has one playlist, %u -- a reorder cannot be"
             " about any other\n", (unsigned)djdb_g_sole_playlist);
    else
        MDBG("djdb: this medium has %d playlists -- which one a reorder is about"
             " comes off the list cache it is being served from\n",
             djdb_g_playlists);
}

uint32_t mod_djdb_playlist_now(void)
{
    /* THE CACHE THE LIST IS BEING SERVED FROM, which is the only one of these
     * three that can name a playlist among several. The other two are fallbacks
     * for a collector that has not been seen yet: the deck's own cursor if it
     * ever named one -- it does not, while browsing -- and otherwise the
     * medium's ONLY playlist, which needs no naming. Zero when none holds, which
     * is a write that refuses rather than guesses. */
    uint32_t id = djdb_playlist_from_caches();

    if (id)
        return id;
    if (djdb_g_seen_playlist)
        return djdb_g_seen_playlist;
    return djdb_g_playlists == 1 ? djdb_g_sole_playlist : 0;
}

void djdb_note_playlist(const char *table, int nkeys, void **keyvals)
{
    if (djdb_g_ours)
        return;
    /* Wide enough to hold a name the log can be read from, not just wide
     * enough to compare: a buffer the length of the one table we recognise
     * truncates every other one to the same 16 characters. */
    char name[24];
    uint32_t pid = 0;

    if (!table)
        return;
    /* The pointer is the deck's argument read positionally, so it is INPUT:
     * probed rather than dereferenced, and over a fixed length so a shorter
     * name cannot run off the end of the mapping. */
    if (mod_safe_read((uintptr_t)table, name, sizeof(name)) != 0)
        return;
    name[sizeof(name) - 1] = '\0';
    djdb_note_table(name);
    if (nkeys != 1 || !keyvals || strcmp(name, DJDB_PLAYLIST_TABLE))
        return;
    if (!keyvals[0] ||
        mod_safe_read((uintptr_t)keyvals[0], &pid, sizeof(pid)) != 0 || !pid)
        return;
    if (pid != djdb_g_seen_playlist)
        MDBG("djdb: the list on screen is playlist %u\n", (unsigned)pid);
    djdb_g_seen_playlist = pid;
}
