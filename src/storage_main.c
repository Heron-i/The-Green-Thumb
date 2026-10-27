/* storage_main: the storage process (DESIGN.md Mechanism D).
 *
 * Purpose: wire the tested pieces together into one long-running process.
 * Every decision lives in the library (storage*.c, storage_server.c,
 * storage_flusher.c, listen_sock.c); this file only orders the start-up,
 * owns the threads and signals, and prints log lines. Start-up order:
 *   1. Take the one-listener lock for the socket path FIRST. It guards the
 *      log as well as the socket: two storage processes recovering and
 *      appending to one segment would corrupt it. A second copy exits
 *      with "already running" and touches nothing.
 *   2. Open storage. Recovery runs here, before any data can arrive
 *      (DESIGN.md "recovery runs before storage.sock accepts any data").
 *      On an empty log with an unsynced clock it first waits up to 120 s
 *      for the clock, so the first segment isn't named 1970 (PF-20).
 *   3. Bind storage.sock: only now can the hub connect.
 *   4. Start the flush thread, then run the rx loop on this thread.
 * On SIGTERM or SIGINT the handler only sets a flag and writes one byte to
 * a pipe (all a signal handler may safely do); the rx loop wakes, takes the
 * frames already sent and returns; the flush thread is joined; then a
 * clean shutdown (final flush + marker), close, and the socket and lock
 * are released.
 *
 * After the rx loop returns, gt_storage_wake ends the flush thread's wait
 * so it sees the stop at once.
 *
 * -n N and -t T_SECONDS set when a flush is due (default 64 records and
 * 60 s), so DESIGN.md's (N, T) experiment runs on one build (PF-11);
 * storage_start logs them as batch_n and batch_t_s.
 *
 * Log lines go to stderr in gt_log_format's format, component "storage"
 * (the shared sink, PF-18). The heartbeat to sup.sock arrives with
 * Mechanism E.
 *
 * Usage: storage_main [-n N] [-t T_SECONDS] /var/lib/plantpi /run/plantpi/storage.sock */
#define _DEFAULT_SOURCE   /* adjtimex() in <sys/timex.h> is a Linux extension */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "storage_pending.h"
#include "storage_server.h"

#define LISTEN_BACKLOG 4

/* 2026-10-01 00:00 UTC: no correct clock can read earlier than the start
 * of this project, so an earlier reading means the Pi booted with no
 * network time and no RTC (DESIGN.md: "a timestamp earlier than the build
 * date"). */
#define EARLIEST_VALID_S 1790812800

/* How long an empty log waits for the clock before the first segment is
 * named (wait_for_clock_if_empty, PF-20). */
#define CLOCK_WAIT_MAX_S 120

/* The largest -t: a day. Longer would only hold records in memory past
 * DESIGN.md's commit-latency target for no gain. */
#define BATCH_AGE_MAX_S 86400

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
    /* Designated initializers: each field is set by name. */
    gt_log_entry entry = { .timestamp_us = wall_us(), .level = level, .component = "storage",
                           .event = event, .fields = fields };
    if (gt_log_format(line, sizeof line, &entry) == GT_OK) {
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

/* On an empty log, wait up to CLOCK_WAIT_MAX_S for the clock before open
 * names the first segment, so a Pi that booted without network time
 * doesn't start log-19700101.bin (PF-20). A log that already holds a
 * segment needs no wait: open reuses the newest, and rollover is skipped
 * while the clock is unsynced. The hub's 1024-record queue (about 17 min
 * at 1 Hz) holds readings meanwhile. If the clock is still unsynced at the
 * end, open goes ahead with the time it has (logged at WARN). Signals are
 * not installed yet, so a SIGTERM here simply ends the process. */
static void wait_for_clock_if_empty(const char *log_dir)
{
    int empty = 0;
    if (gt_storage_log_empty(log_dir, &empty) != GT_OK || !empty || clock_synced()) {
        return;
    }
    char fields[48];
    snprintf(fields, sizeof fields, "max_s=%d", CLOCK_WAIT_MAX_S);
    log_line(NULL, GT_LOG_WARN, "clock_wait", fields);
    const struct timespec one_second = { 1, 0 };
    int waited_s = 0;
    /* invariant: the clock was unsynced at every one of the waited_s checks */
    while (waited_s < CLOCK_WAIT_MAX_S && !clock_synced()) {
        nanosleep(&one_second, NULL);
        waited_s++;
    }
    int synced = clock_synced();
    snprintf(fields, sizeof fields, "waited_s=%d synced=%d", waited_s, synced);
    log_line(NULL, synced ? GT_LOG_INFO : GT_LOG_WARN, "clock_wait_done", fields);
}

/* Parse a whole decimal number from 1 to max_value. Returns 1 on success;
 * *value_out is untouched on failure. */
static int parse_bounded(const char *text, unsigned long max_value, unsigned long *value_out)
{
    if (text[0] < '0' || text[0] > '9') {
        return 0;   /* empty, or a sign/space strtoul would accept */
    }
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > max_value) {
        return 0;
    }
    *value_out = value;
    return 1;
}

/* Read the optional "-n N" and "-t T_SECONDS" pairs at the front of argv,
 * in either order, into *batch_out and *age_s_out (left at their defaults
 * if not given). N runs from 1 to the pending buffer's size, T from 1 s to
 * a day. *arg_base_out is the index of the first argument after them.
 * Returns 1 on success, 0 for a bad or repeated option. */
static int parse_batch_options(int argc, char **argv, unsigned long *batch_out,
                               unsigned long *age_s_out, int *arg_base_out)
{
    int seen_batch = 0, seen_age = 0;
    int arg_index = 1;
    /* invariant: argv[1 .. arg_index - 1] are whole, valid option pairs */
    while (arg_index + 1 < argc && argv[arg_index][0] == '-') {
        const char *flag = argv[arg_index];
        int ok = 0;   /* stays 0 for an unknown or repeated option */
        if (strcmp(flag, "-n") == 0 && !seen_batch) {
            seen_batch = parse_bounded(argv[arg_index + 1], GT_PENDING_MAX_RECORDS, batch_out);
            ok = seen_batch;
        } else if (strcmp(flag, "-t") == 0 && !seen_age) {
            seen_age = parse_bounded(argv[arg_index + 1], BATCH_AGE_MAX_S, age_s_out);
            ok = seen_age;
        }
        if (!ok) {
            return 0;
        }
        arg_index += 2;
    }
    *arg_base_out = arg_index;
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

/* Log what recovery did to one segment: segment_corrupt if it was renamed
 * to .corrupt, otherwise recovery_event with what was cut (WARN if
 * anything was). */
static void log_recovery(const char *name, const gt_recover_report *report)
{
    char fields[160];
    if (report->outcome == GT_RECOVER_RENAMED) {
        snprintf(fields, sizeof fields, "segment=%s bytes=%llu", name,
                 (unsigned long long)report->bytes_removed);
        log_line(NULL, GT_LOG_ERROR, "segment_corrupt", fields);
    } else {
        snprintf(fields, sizeof fields, "segment=%s bytes_removed=%llu records_removed=%lu",
                 name, (unsigned long long)report->bytes_removed,
                 (unsigned long)report->records_removed);
        log_line(NULL, report->records_removed > 0 ? GT_LOG_WARN : GT_LOG_INFO,
                 "recovery_event", fields);
    }
}

/* Log what open found: whether the last run ended cleanly, and what
 * recovery did to the newest segment (DESIGN.md: unclean_shutdown,
 * recovery_event, segment_corrupt). storage_start also carries N and T,
 * so a run of the (N, T) experiment says which point it was (PF-11). */
static void log_open_info(const gt_storage_open_info *info, unsigned long batch_records,
                          unsigned long batch_age_s)
{
    char fields[160];
    if (info->unclean_shutdown) {
        log_line(NULL, GT_LOG_WARN, "unclean_shutdown", NULL);
    }
    if (info->recovered) {
        log_recovery(info->recovered_name, &info->recovery);
    }
    /* Older segments the walk back for the last seq changed (PF-14). */
    for (size_t older_index = 0; older_index < info->older_count; older_index++) {
        log_recovery(info->older[older_index].name, &info->older[older_index].report);
    }
    if (info->older_unlisted > 0) {
        snprintf(fields, sizeof fields, "segments=%lu", (unsigned long)info->older_unlisted);
        log_line(NULL, GT_LOG_WARN, "recovery_unlisted", fields);
    }
    if (info->has_last) {
        snprintf(fields, sizeof fields, "segment=%s created=%d last_seq=%lu batch_n=%lu batch_t_s=%lu",
                 info->active_name, info->created, (unsigned long)info->last_seq,
                 batch_records, batch_age_s);
    } else {
        snprintf(fields, sizeof fields, "segment=%s created=%d last_seq=none batch_n=%lu batch_t_s=%lu",
                 info->active_name, info->created, batch_records, batch_age_s);
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

/* Say why the one-listener lock wasn't taken: another storage_main holds
 * it, or the lock file itself failed (errno says how). */
static void report_lock_error(const char *sock_path, int already_running)
{
    if (already_running) {
        fprintf(stderr, "storage_main: already running on %s (%s.lock is locked)\n",
                sock_path, sock_path);
        return;
    }
    fprintf(stderr, "storage_main: lock %s.lock: %s\n", sock_path, strerror(errno));
}

/* Start-up step 2: make the log directory if it isn't there yet (a fresh
 * install), open storage (recovery runs here), log what open found, and
 * set the clock and (N, T) before the flush thread starts. N and T were
 * range-checked by the caller, so set_batch can't fail (PF-11). Returns
 * NULL on failure, already reported. */
static gt_storage *open_storage(const char *log_dir, unsigned long batch_records,
                                unsigned long batch_age_s)
{
    if (mkdir(log_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "storage_main: %s: %s\n", log_dir, strerror(errno));
        return NULL;
    }
    wait_for_clock_if_empty(log_dir);
    gt_storage *storage = NULL;
    gt_storage_open_info info;
    gt_status status = gt_storage_open(log_dir, wall_us(), &storage, &info);
    if (status != GT_OK) {
        char fields[48];
        snprintf(fields, sizeof fields, "status=%d", (int)status);
        log_line(NULL, GT_LOG_ERROR, "storage_open_error", fields);
        return NULL;
    }
    log_open_info(&info, batch_records, batch_age_s);
    gt_storage_set_clock(storage, monotonic_us);
    gt_storage_set_batch(storage, (size_t)batch_records, (unsigned)batch_age_s);
    return storage;
}

/* The whole life of the process after the arguments are read: start-up
 * steps 1-4, the rx loop, then shutdown. Returns the exit code: 0 for a
 * clean stop, 1 for any failure. */
static int run_storage(const char *log_dir, const char *sock_path,
                       unsigned long batch_records, unsigned long batch_age_s)
{
    /* 1. The lock, before anything touches the log. */
    int lock_fd = -1, already_running = 0;
    if (gt_listen_lock(sock_path, &lock_fd, &already_running) != GT_OK) {
        report_lock_error(sock_path, already_running);
        return 1;
    }

    /* 2. Open storage; recovery runs here. */
    gt_storage *storage = open_storage(log_dir, batch_records, batch_age_s);
    if (storage == NULL) {
        gt_listen_release(sock_path, -1, lock_fd);
        return 1;
    }

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
    }
    if (flusher_running
        && gt_storage_serve(storage, listen_fd, stop_pipe[0], log_line, NULL) != GT_OK) {
        log_line(NULL, GT_LOG_ERROR, "rx_loop_error", NULL);
    }

    /* Shut down: stop and join the flush thread, then the final flush and
     * the marker, then release everything. */
    atomic_store(&stop_requested, 1);
    gt_storage_wake(storage);   /* the flush thread sees the stop at once (PF-11) */
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
    /* The segment and last committed seq say where the log ended (PF-14),
     * matching storage_start's fields. The flush thread has been joined,
     * so reading the active segment's name is safe. */
    char last_seq[16] = "none";
    if (stats.has_last_committed) {
        snprintf(last_seq, sizeof last_seq, "%lu", (unsigned long)stats.last_committed_seq);
    }
    char segment[GT_FILE_NAME_MAX] = "none";
    gt_storage_active_segment(storage, segment);
    char fields[160];
    snprintf(fields, sizeof fields, "clean=%d segment=%s last_seq=%s committed=%llu refused=%llu",
             shutdown_status == GT_OK, segment, last_seq,
             (unsigned long long)stats.committed_records, (unsigned long long)stats.refused);
    log_line(NULL, shutdown_status == GT_OK ? GT_LOG_INFO : GT_LOG_ERROR, "storage_stop", fields);

    gt_storage_close(storage);
    stop_write_fd = -1;
    close(stop_pipe[0]);
    close(stop_pipe[1]);
    gt_listen_release(sock_path, listen_fd, lock_fd);
    return shutdown_status == GT_OK && flusher_running ? 0 : 1;
}

int main(int argc, char **argv)
{
    unsigned long batch_records = GT_BATCH_MAX_RECORDS;
    unsigned long batch_age_s = GT_BATCH_MAX_AGE_S;
    int arg_base = 1;
    if (!parse_batch_options(argc, argv, &batch_records, &batch_age_s, &arg_base)
        || argc - arg_base != 2) {
        fprintf(stderr,
                "usage: %s [-n N] [-t T_SECONDS] /var/lib/plantpi /run/plantpi/storage.sock\n"
                "  N: flush at N records, 1..%u (default %u)\n"
                "  T: flush once the oldest record is T s old, 1..%u (default %u)\n",
                argv[0], (unsigned)GT_PENDING_MAX_RECORDS, (unsigned)GT_BATCH_MAX_RECORDS,
                (unsigned)BATCH_AGE_MAX_S, (unsigned)GT_BATCH_MAX_AGE_S);
        return 2;
    }
    return run_storage(argv[arg_base], argv[arg_base + 1], batch_records, batch_age_s);
}
