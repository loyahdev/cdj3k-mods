// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/db.c - see db.h for what this is and what its rules are.
 *
 * ---- how the sqlite3 functions are reached --------------------------------
 *
 * WEAK, NOT RESOLVED. libsqlcipher.so.0 is a NEEDED of EP122, so it is already
 * loaded and its symbols are in the process's global scope by the time any mod
 * runs. Declaring them weak lets the dynamic linker do the resolution -- which
 * is the most exact form of "by identity, not by address" available anywhere in
 * this shim, because the name is matched by the linker itself -- and an
 * unresolved weak symbol is NULL rather than a load failure, so a build without
 * the library degrades to db_ready() == 0 instead of refusing to start.
 *
 * That also reaches more of the API than EP122 imports. The deck itself pulls in
 * twenty sqlite3 entry points; the library exports the whole public surface, and
 * the guards below need sqlite3_get_autocommit and sqlite3_threadsafe, which are
 * not among the twenty. Weak linking gets them anyway.
 *
 * ---- how a handle is reached ----------------------------------------------
 *
 * By hooking the deck's own SqliteUpdateTransaction::exec, whose first argument
 * holds the live sqlite3*. Not by opening anything: the files are encrypted, the
 * deck has already keyed them, and a borrowed handle means no key ever touches
 * this code.
 *
 * Threading. The hook fires on the deck's database thread. It stores a pointer
 * and nothing else -- the identification below happens on the first CALLER's
 * thread, not in the hook, because running a PRAGMA inside the app's own exec
 * would be re-entering the connection mid-statement.
 */
#include "db/db.h"

#include "core/mod_core.h"
#include "core/ep122_syms.h"
#include "kit/mod.h"

/* ---- the library, weakly ------------------------------------------------- */

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

#define DB_WEAK __attribute__((weak))

extern DB_WEAK int         sqlite3_prepare_v2(sqlite3 *, const char *, int,
                                              sqlite3_stmt **, const char **);
extern DB_WEAK int         sqlite3_step(sqlite3_stmt *);
extern DB_WEAK int         sqlite3_finalize(sqlite3_stmt *);
extern DB_WEAK int         sqlite3_reset(sqlite3_stmt *);
extern DB_WEAK const char *sqlite3_errmsg(sqlite3 *);
extern DB_WEAK int         sqlite3_bind_int64(sqlite3_stmt *, int, int64_t);
extern DB_WEAK int         sqlite3_bind_text(sqlite3_stmt *, int, const char *,
                                             int, void (*)(void *));
extern DB_WEAK int         sqlite3_bind_null(sqlite3_stmt *, int);
extern DB_WEAK int         sqlite3_column_count(sqlite3_stmt *);
extern DB_WEAK const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);
extern DB_WEAK int64_t     sqlite3_column_int64(sqlite3_stmt *, int);
extern DB_WEAK int         sqlite3_get_autocommit(sqlite3 *);
extern DB_WEAK int         sqlite3_threadsafe(void);

#define SQLITE_OK           0
#define SQLITE_ROW          100
#define SQLITE_DONE         101
/* SQLITE_TRANSIENT: SQLite copies the text before returning. Spelled as the
 * header does, which is a cast of -1 to the destructor pointer. */
#define SQLITE_TRANSIENT    ((void (*)(void *))-1)

/* ---- the borrowed handle -------------------------------------------------- */

/* [db thread] writes as the deck runs a statement, everyone reads. */
static sqlite3 *db_g_handle;

/* Resolved once, on a caller's thread, from PRAGMA database_list. */
static char db_g_path[512];
static int  db_g_named;

/* 0 unknown, 1 usable, -1 refused. Decided once. */
static int  db_g_usable;

static uintptr_t db_g_tramp;

/* Every sqlite3 entry point this file calls, in one test. A partial library is
 * not a thing that happens, but half a provider silently doing nothing is worse
 * than one that says why. */
static int db_syms_ok(void)
{
    return sqlite3_prepare_v2 && sqlite3_step && sqlite3_finalize &&
           sqlite3_reset && sqlite3_errmsg && sqlite3_bind_int64 &&
           sqlite3_bind_text && sqlite3_bind_null && sqlite3_column_count &&
           sqlite3_column_text && sqlite3_column_int64 &&
           sqlite3_get_autocommit && sqlite3_threadsafe;
}

/* The deck's SqliteUpdateTransaction::exec(txn, query). `*txn` is the sqlite3*.
 *
 * OBSERVATION ONLY -- the stock call is made with its arguments untouched and
 * its result returned verbatim. All this takes is a pointer. */
typedef int64_t (*db_exec_fn)(void *txn, const void *query);

static int64_t db_wrap_exec(void *txn, const void *query)
{
    uintptr_t h = 0;

    if (txn && !__atomic_load_n(&db_g_handle, __ATOMIC_ACQUIRE) &&
        mod_safe_read((uintptr_t)txn, &h, sizeof(h)) == 0 && h) {
        __atomic_store_n(&db_g_handle, (sqlite3 *)h, __ATOMIC_RELEASE);
        /* Once, and only the pointer -- identifying it means running a PRAGMA,
         * and doing that here would re-enter the connection from inside the
         * deck's own statement. mod_db_poll picks it up. */
        MDBG("db: borrowed a connection (%p)\n", (void *)h);
    }

    return ((db_exec_fn)db_g_tramp)(txn, query);
}

/* One statement, prepared and bound. NULL if it will not compile -- which is
 * said out loud, because a typo in a mod's SQL is otherwise a feature that
 * quietly does nothing. */
static sqlite3_stmt *db_prepare(sqlite3 *h, const char *sql,
                                const struct db_bind *binds, int nbind)
{
    sqlite3_stmt *st = NULL;
    int i;

    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK || !st) {
        MDBG("db: will not compile: %s -- %s\n", sqlite3_errmsg(h), sql);
        if (st)
            sqlite3_finalize(st);
        return NULL;
    }
    for (i = 0; i < nbind; i++) {
        int rc;

        /* Parameters are 1-based. */
        switch (binds[i].kind) {
        case DB_INT:
            rc = sqlite3_bind_int64(st, i + 1, binds[i].num);
            break;
        case DB_TEXT:
            rc = binds[i].text
               ? sqlite3_bind_text(st, i + 1, binds[i].text, -1,
                                   SQLITE_TRANSIENT)
               : sqlite3_bind_null(st, i + 1);
            break;
        default:
            rc = sqlite3_bind_null(st, i + 1);
            break;
        }
        if (rc != SQLITE_OK) {
            MDBG("db: bind %d refused: %s\n", i + 1, sqlite3_errmsg(h));
            sqlite3_finalize(st);
            return NULL;
        }
    }
    return st;
}

/* Which database this handle is on. Asked once, on a caller's thread.
 *
 * `PRAGMA database_list` gives (seq, name, file) and the main entry's file is
 * the answer. It is a read of the connection's own state -- no table is touched
 * -- so it is safe to ask of a handle we have only just met. */
static void db_name_once(sqlite3 *h)
{
    sqlite3_stmt *st;

    if (db_g_named)
        return;
    db_g_named = 1;

    st = db_prepare(h, "PRAGMA database_list", NULL, 0);
    if (!st)
        return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);
        const unsigned char *file = sqlite3_column_text(st, 2);

        if (name && file && strcmp((const char *)name, "main") == 0) {
            snprintf(db_g_path, sizeof(db_g_path), "%s", (const char *)file);
            break;
        }
    }
    sqlite3_finalize(st);
    MDBG("db: the borrowed handle is on \"%s\"\n",
         db_g_path[0] ? db_g_path : "(no file -- in memory?)");
}

/* The connection, or NULL, with everything that has to be true about it checked
 * exactly once. */
static sqlite3 *db_get(void)
{
    sqlite3 *h = __atomic_load_n(&db_g_handle, __ATOMIC_ACQUIRE);

    if (!h)
        return NULL;
    if (__atomic_load_n(&db_g_usable, __ATOMIC_ACQUIRE) < 0)
        return NULL;

    if (!__atomic_load_n(&db_g_usable, __ATOMIC_ACQUIRE)) {
        /* SERIALISED OR NOTHING. Sharing the app's connection is only safe
         * because SQLite takes a mutex per call; built without that, two threads
         * on one connection is corruption, and no amount of care here would fix
         * it. Refused rather than risked. */
        if (sqlite3_threadsafe() == 0) {
            MDBG("db: sqlite is not serialised -> the connection is not"
                 " shareable, provider off\n");
            __atomic_store_n(&db_g_usable, -1, __ATOMIC_RELEASE);
            return NULL;
        }
        db_name_once(h);
        __atomic_store_n(&db_g_usable, 1, __ATOMIC_RELEASE);
    }
    return h;
}

int db_ready(void)
{
    return db_get() != NULL;
}

const char *db_path(void)
{
    if (!db_get() || !db_g_path[0])
        return NULL;
    return db_g_path;
}

int db_select(const char *sql, const struct db_bind *binds, int nbind,
              db_row_fn fn, void *user)
{
    const char *text[DB_MAX_COLS];
    int64_t     num[DB_MAX_COLS];
    struct db_row row = { 0, text, num };
    sqlite3 *h = db_get();
    sqlite3_stmt *st;
    int rows = 0, ncol;

    if (!h || !sql || !fn)
        return -1;
    st = db_prepare(h, sql, binds, nbind);
    if (!st)
        return -1;

    ncol = sqlite3_column_count(st);
    if (ncol > DB_MAX_COLS) {
        MDBG("db: %d columns is past the %d a row can carry -> refused: %s\n",
             ncol, DB_MAX_COLS, sql);
        sqlite3_finalize(st);
        return -1;
    }
    row.ncol = ncol;

    while (sqlite3_step(st) == SQLITE_ROW) {
        int i;

        for (i = 0; i < ncol; i++) {
            text[i] = (const char *)sqlite3_column_text(st, i);
            num[i]  = sqlite3_column_int64(st, i);
        }
        rows++;
        if (fn(&row, user))
            break;
    }
    sqlite3_finalize(st);
    return rows;
}

int db_run(const char *sql, const struct db_bind *binds, int nbind)
{
    sqlite3 *h = db_get();
    sqlite3_stmt *st;
    int rc;

    if (!h || !sql)
        return -1;

    /* NOT INTO SOMEONE ELSE'S TRANSACTION. Autocommit off means the app has a
     * BEGIN open on this connection; a statement issued now is committed or
     * rolled back by whatever the app decides, which is not a thing to hand a
     * DJ's library over to. The deck's transactions are short -- the caller
     * retrying in a moment is the whole remedy. */
    if (!sqlite3_get_autocommit(h)) {
        MDBG("db: the deck is mid-transaction -> refused: %s\n", sql);
        return -1;
    }

    st = db_prepare(h, sql, binds, nbind);
    if (!st)
        return -1;

    rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        /* A statement handed to db_run returned rows, or failed. Both are the
         * caller's mistake and both are worth the line. */
        MDBG("db: step %d (%s): %s\n", rc, sqlite3_errmsg(h), sql);
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 0;
}

/* Identify the connection as soon as one has been borrowed, rather than waiting
 * for a mod to want it. Two reasons it is worth a poll: WHICH database this is
 * decides whether a write travels with the stick, which is a thing to know
 * before anything is written rather than after; and a provider whose only
 * evidence of working is a client using it is one that cannot be told from a
 * broken hook. Costs one atomic load per idle tick once it has run.
 *
 * [worker] */
void mod_db_poll(void)
{
    if (__atomic_load_n(&db_g_handle, __ATOMIC_ACQUIRE) && !db_g_named)
        (void)db_get();
}

/* ---- install -------------------------------------------------------------- */

static int db_install(void)
{
    if (!db_syms_ok()) {
        MDBG("db: libsqlcipher is not in this process -> no database access\n");
        return -1;
    }
    if (mod_patch_fn("sqliteUpdateExec", ep122_sym(EP122_SQLITE_UPDATE_EXEC),
                     (void *)db_wrap_exec, &db_g_tramp) != 0) {
        MDBG("db: no update-transaction hook -> no handle to borrow\n");
        return -1;
    }
    return 0;
}

KIT_MOD(k_mod_db,
        .name = "db", .prio = 3, .install = db_install,
        .what = "database: borrow the media's Device Library Plus connection");
