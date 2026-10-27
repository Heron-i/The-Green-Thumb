/* storage_main: the storage process (DESIGN.md Mechanism D).
 *
 * Purpose: wire the tested pieces together into one long-running process.
 * Every decision lives in the library (storage.c, storage_server.c,
 * listen_sock.c); this file only orders the start-up, owns the threads and
 * signals, and prints log lines. Start-up order:
 *   1. Take the one-listener lock for the socket path FIRST. It guards the
 *      log as well as the socket: two storage processes recovering and
 *      appending to one segment would corrupt it. A second copy exits
 *      with "already running" and touches nothing.
 *   2. Open storage. Recovery runs here, before any data can arrive
 *      (DESIGN.md "recovery runs before storage.sock accepts any data").
 *   3. Bind storage.sock: only now can the hub connect.
 *   4. Start the flush thread, then run the rx loop on this thread.
 * On SIGTERM or SIGINT the handler only sets a flag and writes one byte to
 * a pipe (all a signal handler may safely do); the rx loop wakes, takes the
 * frames already sent and returns; the flush thread is joined; then a
 * clean shutdown (final flush + marker), close, and the socket and lock
 * are released.
 *
 * Log lines go to stderr in gt_log_format's format, component "storage".
 * The heartbeat to sup.sock arrives with Mechanism E.
 *
 * Usage: storage_main /var/lib/plantpi /run/plantpi/storage.sock */
#define _DEFAULT_SOURCE   /* adjtimex() in <sys/timex.h> is a Linux extension */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/timex.h>
#endif

#include "gt_log.h"
#include "listen_sock.h"
#include "storage.h"
#include "storage_server.h"

#define LISTEN_BACKLOG 4

/* 2026-10-01 00:00 UTC: no correct clock can read earlier than the start
 * of this project, so an earlier reading means the Pi booted with no
 * network time and no RTC (DESIGN.md: "a timestamp earlier than the build
 * date"). */
#define EARLIEST_VALID_S 1790812800

/* Set by the signal handler, read by both threads. */
static atomic_int stop_requested;
/* The write end of the stop pipe, for the signal handler. */
static int stop_write_fd = -1;

/* Wall-clock time in us since the epoch, for log lines and the segment
 * name. */
static uint64_t wall_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* Monotonic time in us, for timing commits (gt_storage_set_clock, PF-08).
 * CLOCK_MONOTONIC never jumps when the wall clock is set, so a duration is
 * never negative or inflated by an NTP step. */
static uint64_t monotonic_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* The log callback: one gt_log_format line on stderr. */
static void log_line(void *log_ctx, gt_log_level level, const char *event, const char *fields)
{
    (void)log_ctx;
    char line[GT_LOG_LINE_MAX];
    if (gt_log_format(line, sizeof line, wall_us(), level, "storage", event, fields) == GT_OK) {
        fprintf(stderr, "%s\n", line);
    }
}

/* 1 if the wall clock can be trusted for rollover and retention: the
 * kernel says NTP has synced it (adjtimex returns TIME_ERROR while it
 * hasn't), and it is not earlier than this project. adjtimex is Linux
 * only; elsewhere only the date check is made. */
static int clock_synced(void)
{
    if (wall_us() / 1000000u < EARLIEST_VALID_S) {
        return 0;
    }
#ifdef __linux__
    struct timex clock_state;
    memset(&clock_state, 0, sizeof clock_state);   /* modes = 0: only read */
    int state = adjtimex(&clock_state);
    if (state == -1 || state == TIME_ERROR) {
        return 0;
    }
#endif
    return 1;
}

/* SIGTERM / SIGINT: set the flag and wake the rx loop's poll(). errno is
 * saved and restored, because write() may change it under the code the
 * signal interrupted. */
static void on_stop_signal(int signum)
{
    (void)signum;
    int saved_errno = errno;
    const char stop_byte = 's';
    atomic_store(&stop_requested, 1);
    if (stop_write_fd >= 0 && write(stop_write_fd, &stop_byte, 1) < 0) {
        /* nothing more a signal handler can do */
    }
    errno = saved_errno;
}

/* The flush thread's start function: pthread_create calls it with the
 * gt_flusher as `arg`. */
static void *flusher_thread(void *arg)
{
    gt_flusher_run(arg, &stop_requested, clock_synced);
    return NULL;
}

/* Log what open found: whether the last run ended cleanly, and what
 * recovery did to the newest segment (DESIGN.md: unclean_shutdown,
 * recovery_event, segment_corrupt). */
static void log_open_info(const gt_storage_open_info *info)
{
    char fields[160];
    if (info->unclean_shutdown) {
        log_line(NULL, GT_LOG_WARN, "unclean_shutdown", NULL);
    }
    if (info->recovered && info->recovery.outcome == GT_RECOVER_RENAMED) {
        snprintf(fields, sizeof fields, "segment=%s bytes=%llu", info->recovered_name,
                 (unsigned long long)info->recovery.bytes_removed);
        log_line(NULL, GT_LOG_ERROR, "segment_corrupt", fields);
    } else if (info->recovered) {
        snprintf(fields, sizeof fields, "segment=%s bytes_removed=%llu records_removed=%lu",
                 info->recovered_name, (unsigned long long)info->recovery.bytes_removed,
                 (unsigned long)info->recovery.records_removed);
        log_line(NULL, info->recovery.records_removed > 0 ? GT_LOG_WARN : GT_LOG_INFO,
                 "recovery_event", fields);
    }
    if (info->has_last) {
        snprintf(fields, sizeof fields, "segment=%s created=%d last_seq=%lu",
                 info->active_name, info->created, (unsigned long)info->last_seq);
    } else {
        snprintf(fields, sizeof fields, "segment=%s created=%d last_seq=none",
                 info->active_name, info->created);
    }
    log_line(NULL, GT_LOG_INFO, "storage_start", fields);
}

/* Install the stop handler for SIGTERM and SIGINT, and ignore SIGPIPE (a
 * second guard, besides MSG_NOSIGNAL, against a dead hub killing
 * storage). `struct sigaction` describes what to do on a signal; no
 * SA_RESTART, so a blocked call returns EINTR and the loops can check
 * the flag. */
static void install_signals(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* Start the flush thread with SIGTERM and SIGINT blocked in it, so the
 * signals are always handled on this (the rx) thread: a new thread
 * inherits the signal mask of the one that creates it, so the mask is
 * set, the thread made, and the mask put back. Returns 1 on success. */
static int start_flusher(pthread_t *thread, gt_flusher *flusher)
{
    sigset_t stop_signals, previous;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &stop_signals, &previous);
    int created = pthread_create(thread, NULL, flusher_thread, flusher) == 0;
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return created;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s /var/lib/plantpi /run/plantpi/storage.sock\n", argv[0]);
        return 2;
    }
    const char *log_dir = argv[1];
    const char *sock_path = argv[2];

    /* 1. The lock, before anything touches the log. */
    int lock_fd = -1, already_running = 0;
    if (gt_listen_lock(sock_path, &lock_fd, &already_running) != GT_OK) {
        if (already_running) {
            fprintf(stderr, "storage_main: already running on %s (%s.lock is locked)\n",
                    sock_path, sock_path);
        } else {
            fprintf(stderr, "storage_main: lock %s.lock: %s\n", sock_path, strerror(errno));
        }
        return 1;
    }

    /* 2. Open storage; recovery runs here. The log directory is made if
     * it isn't there yet (a fresh install). */
    if (mkdir(log_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "storage_main: %s: %s\n", log_dir, strerror(errno));
        gt_listen_release(sock_path, -1, lock_fd);
        return 1;
    }
    gt_storage *storage = NULL;
    gt_storage_open_info info;
    gt_status status = gt_storage_open(log_dir, wall_us(), &storage, &info);
    if (status != GT_OK) {
        char fields[48];
        snprintf(fields, sizeof fields, "status=%d", (int)status);
        log_line(NULL, GT_LOG_ERROR, "storage_open_error", fields);
        gt_listen_release(sock_path, -1, lock_fd);
        return 1;
    }
    log_open_info(&info);
    gt_storage_set_clock(storage, monotonic_us);   /* before the flush thread starts */

    /* 3. Bind storage.sock, and make the stop pipe. */
    int listen_fd = -1;
    int stop_pipe[2] = { -1, -1 };
    if (gt_listen_bind(sock_path, LISTEN_BACKLOG, &listen_fd) != GT_OK || pipe(stop_pipe) != 0) {
        fprintf(stderr, "storage_main: %s: %s\n", sock_path, strerror(errno));
        gt_storage_shutdown(storage, NULL);
        gt_storage_close(storage);
        gt_listen_release(sock_path, listen_fd, lock_fd);
        return 1;
    }
    stop_write_fd = stop_pipe[1];
    install_signals();

    /* 4. The flush thread, then the rx loop until a stop. */
    gt_flusher flusher;
    gt_flusher_init(&flusher, storage, 0, log_line, NULL);
    pthread_t flush_thread;
    int flusher_running = start_flusher(&flush_thread, &flusher);
    if (!flusher_running) {
        log_line(NULL, GT_LOG_ERROR, "flusher_start_error", NULL);
    } else {
        status = gt_storage_serve(storage, listen_fd, stop_pipe[0], log_line, NULL);
        if (status != GT_OK) {
            log_line(NULL, GT_LOG_ERROR, "rx_loop_error", NULL);
        }
    }

    /* Shut down: stop and join the flush thread, then the final flush and
     * the marker, then release everything. */
    atomic_store(&stop_requested, 1);
    if (flusher_running) {
        pthread_join(flush_thread, NULL);
    }
    unsigned events = 0;
    gt_storage_stats stats;
    gt_storage_read_stats(storage, &stats);
    uint64_t commits_before = stats.commits;
    gt_status shutdown_status = gt_storage_shutdown(storage, &events);
    gt_storage_log_events(log_line, NULL, events);
    gt_storage_log_commit(log_line, NULL, storage, commits_before);   /* the final flush */
    gt_storage_read_stats(storage, &stats);
    char fields[96];
    snprintf(fields, sizeof fields, "clean=%d committed=%llu refused=%llu",
             shutdown_status == GT_OK, (unsigned long long)stats.committed_records,
             (unsigned long long)stats.refused);
    log_line(NULL, shutdown_status == GT_OK ? GT_LOG_INFO : GT_LOG_ERROR, "storage_stop", fields);

    gt_storage_close(storage);
    stop_write_fd = -1;
    close(stop_pipe[0]);
    close(stop_pipe[1]);
    gt_listen_release(sock_path, listen_fd, lock_fd);
    return shutdown_status == GT_OK && flusher_running ? 0 : 1;
}
