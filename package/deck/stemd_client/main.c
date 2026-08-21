// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stemd_client - the network half of the STEMS feature.
 *
 * A small daemon that sits between the EP122 shim and a `stemd` server. It
 * exists so that no HTTP client, retry loop, resolver or blocking socket runs
 * inside the process that owns the deck's audio thread: if this wedges or
 * crashes, EP122 keeps playing and the deck simply shows STEMS as unavailable.
 * On a device someone is performing on, that is worth the extra moving part.
 *
 * What deliberately does NOT move here is decoding. The decoder we need is
 * EP122's own -- decoding a second time out here would risk the two decoders
 * disagreeing about encoder delay, and the deck pads its output (4116 frames on
 * the reference track), so a stem set built from an independent decode comes
 * back misaligned and reads as a phase problem rather than an off-by-N. The
 * shim decodes and streams PCM in; this process never opens a track file.
 *
 * Shape:
 *
 *   accept on /run/stemd-client.sock   (one client: there is one EP122)
 *     HELLO      -> version check
 *     JOB_BEGIN  -> open POST /v1/jobs, stream the body as PCM frames arrive
 *     JOB_END    -> finish the request; 200 = cache hit, 202 = queued
 *                   poll GET /v1/jobs/{id}, forwarding PROGRESS
 *                   GET  /v1/jobs/{id}/stems/{name} -> tmpfs WAV -> STEM_READY
 *     CANCEL     -> DELETE /v1/jobs/{id}
 *
 * Discovery and the health probe live in discovery.c; the HTTP/1.1 client in
 * http.c. This file is the socket loop and the state machine, and nothing else.
 */
#include "stemd_client.h"

static volatile sig_atomic_t g_stop;

/* session.c blocks in read() for as long as a client is connected. With
 * SA_RESTART cleared the signal does reach it as EINTR, but a read loop that
 * simply retries on EINTR swallows that just as effectively -- which is why
 * systemd still had to SIGKILL. This is how the loop knows the difference. */
int session_should_stop(void)
{
    return g_stop != 0;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* sigaction with SA_RESTART deliberately CLEARED.
 *
 * signal() gives BSD semantics, which means SA_RESTART, which means a SIGTERM
 * arriving while we sit in accept() silently restarts the call instead of
 * returning EINTR -- so `while (!g_stop)` is never re-tested and the daemon
 * only ever dies to SIGKILL. It shows up as
 *
 *   stemd-client.service: State 'stop-sigterm' timed out. Killing.
 *
 * on every restart, and worse, a job in flight never gets the chance to close
 * its session with the server. Clearing the flag is what makes the interrupt
 * actually reach the loop. */
static void install_stop_handler(int sig)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, NULL);
}

/* Clear the spool at startup.
 *
 * /dev/shm is guest RAM. Stems are named per part now, so the live footprint is
 * two files whatever happens -- but a deck that ran an older build still has one
 * file per job sitting there (380 MB across five jobs, on a 3 GiB guest), and
 * they outlive this process because only a reboot clears tmpfs.
 *
 * Restricted to names this daemon owns: the two parts, their .part staging
 * files, and the stem-<uuid>-<part>.wav that earlier builds wrote. Nothing else
 * in /dev/shm is touched. Startup is the only safe moment for this -- there is
 * exactly one client, so nothing is in flight yet. */
static void spool_sweep(void)
{
    /* Matched on the stem name and any extension, rather than on a list of
     * suffixes: the container is the server's choice and has already changed
     * once (wav -> flac). A sweep that knows only the old one silently stops
     * working the day the format moves. */
    static const char *const own[] = { STEM_WIRE_HARMONICS, STEM_WIRE_VOCALS };
    char path[256];
    struct dirent *de;
    int removed = 0;
    DIR *d = opendir(STEM_SPOOL_DIR);

    if (!d)
        return;
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        int mine = 0, i;

        for (i = 0; i < (int)(sizeof(own) / sizeof(own[0])); i++) {
            size_t k = strlen(own[i]);

            if (!strncmp(de->d_name, own[i], k) && de->d_name[k] == '.')
                mine = 1;
        }
        /* Legacy per-job naming: stem-<uuid>-<part>.wav */
        if (!mine && !strncmp(de->d_name, "stem-", 5) && n > 4 &&
            !strcmp(de->d_name + n - 4, ".wav"))
            mine = 1;
        if (!mine)
            continue;

        if ((size_t)snprintf(path, sizeof(path), "%s/%s", STEM_SPOOL_DIR,
                             de->d_name) < sizeof(path) &&
            unlink(path) == 0)
            removed++;
    }
    closedir(d);
    if (removed)
        SINFO("swept %d stale spool file(s)\n", removed);
}

static int listen_socket(void)
{
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, STEM_SOCK_PATH, sizeof(addr.sun_path) - 1);

    /* A stale socket from a previous run refuses bind with EADDRINUSE even
     * though nothing is listening, so clear it. Safe because the path is ours
     * by convention and /run is tmpfs, wiped on boot. */
    unlink(STEM_SOCK_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int g_stemd_log = LOG_ERROR;   /* quiet until STEMD_LOGLEVEL says otherwise */

int main(int argc, char **argv)
{
    int lfd;

    (void)argc;
    (void)argv;

    {
        const char *lvl = getenv("STEMD_LOGLEVEL");
        int         n   = log_level_from(lvl);

        g_stemd_log = n < 0 ? LOG_ERROR : n;
        if (n < 0)
            SERR("STEMD_LOGLEVEL=\"%s\" names no level "
                 "(error|warn|info|debug|trace, or 0-4); staying at error\n", lvl);
    }

    /* A client that goes away mid-upload must give us EPIPE to handle, not a
     * signal that kills the daemon. */
    signal(SIGPIPE, SIG_IGN);
    install_stop_handler(SIGINT);
    install_stop_handler(SIGTERM);

    spool_sweep();

    lfd = listen_socket();
    if (lfd < 0)
        return 1;

    SINFO("listening on %s\n", STEM_SOCK_PATH);

    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);

        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        /* One EP122, so one client at a time and no concurrency to get wrong.
         * A second connection waits in the backlog until this one ends. */
        session_run(cfd);
        close(cfd);
    }

    close(lfd);
    unlink(STEM_SOCK_PATH);
    return 0;
}
