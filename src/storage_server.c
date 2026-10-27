/* The storage process's two loops: the rx loop on storage.sock and the
 * flush thread (see storage_server.h).
 *
 * Purpose: move bytes between the socket and storage, and turn storage's
 * events into log callbacks. Every decision (accept, refuse, keep or close
 * the connection, flush, roll over) is made by storage.c; this file only
 * waits for something to happen and passes it on.
 *
 * Linux model: one poll() on the listener, the hub's connection and a stop
 * pipe, so the rx loop sleeps until there is work and wakes at once on
 * SIGTERM (storage_main writes a byte to the pipe). Every recv uses
 * MSG_DONTWAIT, so a read poll() reported can never block, and the reply
 * is sent with MSG_NOSIGNAL, so a hub that is already gone is an error
 * (EPIPE) rather than a SIGPIPE signal that would kill storage. */
#include "storage_server.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FLUSHER_PERIOD_MS 1000
#define FLUSHER_STEP_MS   100   /* how often the sleeping flush thread checks stop */

/* Monotonic time in ms (never jumps when the wall clock is set), the clock
 * every append and flush_due uses. */
static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Wall-clock time in us since the epoch: picks the day's segment. */
static uint64_t wall_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* gt_storage_event_describe names one flag at a time, so each set bit is
 * described on its own. The loop stops when bit has shifted past the top
 * of an unsigned and become 0. */
void gt_storage_log_events(gt_storage_log_fn log, void *log_ctx, unsigned events)
{
    if (log == NULL) {
        return;
    }
    /* invariant: every set bit of events below `bit` has been logged */
    for (unsigned bit = 1; bit != 0 && bit <= events; bit <<= 1) {
        gt_storage_event event;
        if (!(events & bit) || gt_storage_event_describe(bit, &event) != GT_OK) {
            continue;
        }
        char fields[48];
        if (event.reason != NULL) {
            snprintf(fields, sizeof fields, "reason=%s", event.reason);
        }
        log(log_ctx, event.level, event.name, event.reason != NULL ? fields : NULL);
    }
}

/* The backlog bit is taken out and logged here with its fields; the rest
 * go through gt_storage_log_events unchanged. Append updates
 * backlog_percent under the same call that set the bit, so the stats read
 * here describe that crossing. */
void gt_storage_log_rx_events(gt_storage_log_fn log, void *log_ctx, const gt_storage *storage,
                              unsigned events)
{
    gt_storage_stats stats;
    if (log != NULL && (events & GT_STORAGE_EV_BACKLOG) && storage != NULL
        && gt_storage_read_stats(storage, &stats) == GT_OK) {
        char fields[48];
        snprintf(fields, sizeof fields, "percent=%u pending=%lu", stats.backlog_percent,
                 (unsigned long)stats.pending);
        gt_storage_event event;
        if (gt_storage_event_describe(GT_STORAGE_EV_BACKLOG, &event) == GT_OK) {
            log(log_ctx, event.level, event.name, fields);
            events &= ~GT_STORAGE_EV_BACKLOG;
        }
    }
    gt_storage_log_events(log, log_ctx, events);
}

/* What reading one frame from the hub's connection came to. */
typedef enum { FRAME_KEEP, FRAME_CLOSE, FRAME_NONE } frame_result;

/* Read one frame (if there is one) and hand it to gt_storage_rx. The
 * buffer is one byte bigger than a record, so an oversized frame shows up
 * as 25 B instead of being silently cut to 24. FRAME_NONE: nothing was
 * waiting (EAGAIN), or a signal interrupted the read. */
static frame_result read_frame(gt_storage *storage, int conn_fd, gt_storage_log_fn log,
                               void *log_ctx)
{
    uint8_t buf[GT_REC_SIZE + 1];
    ssize_t len = recv(conn_fd, buf, sizeof buf, MSG_DONTWAIT);
    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return FRAME_NONE;
    }
    unsigned events = 0;
    gt_rx_action action = gt_storage_rx(storage, buf, (long)len, monotonic_ms(), &events);
    gt_storage_log_rx_events(log, log_ctx, storage, events);
    return action == GT_RX_KEEP ? FRAME_KEEP : FRAME_CLOSE;
}

/* Send a newly accepted hub the seq_resume reply. Returns 1 if it went. */
static int send_resume(gt_storage *storage, int conn_fd, gt_storage_log_fn log, void *log_ctx)
{
    gt_resume resume;
    uint8_t reply[GT_RESUME_SIZE];
    if (gt_storage_resume(storage, &resume) != GT_OK
        || gt_resume_encode(&resume, reply) != GT_OK
        || send(conn_fd, reply, sizeof reply, MSG_NOSIGNAL) != (ssize_t)sizeof reply) {
        return 0;
    }
    char fields[32];
    if (resume.has_last) {
        snprintf(fields, sizeof fields, "resume_seq=%lu", (unsigned long)resume.seq);
    } else {
        snprintf(fields, sizeof fields, "resume_seq=none");
    }
    log(log_ctx, GT_LOG_INFO, "hub_connected", fields);
    return 1;
}

/* Accept a waiting hub. FD_CLOEXEC is set by hand because accept4() (which
 * could set it in one call) is not POSIX. A newer hub replaces the
 * current one. Returns the connection to use from now on (-1 if none). */
static int accept_hub(gt_storage *storage, int listen_fd, int conn_fd, gt_storage_log_fn log,
                      void *log_ctx)
{
    int new_fd = accept(listen_fd, NULL, NULL);
    if (new_fd < 0) {
        return conn_fd;   /* nothing to accept after all; keep the current hub */
    }
    fcntl(new_fd, F_SETFD, FD_CLOEXEC);
    if (conn_fd >= 0) {
        close(conn_fd);
        log(log_ctx, GT_LOG_WARN, "hub_duplicate_client", NULL);
    }
    if (!send_resume(storage, new_fd, log, log_ctx)) {
        close(new_fd);
        log(log_ctx, GT_LOG_INFO, "hub_disconnected", NULL);
        return -1;
    }
    return new_fd;
}

gt_status gt_storage_serve(gt_storage *storage, int listen_fd, int stop_fd,
                           gt_storage_log_fn log, void *log_ctx)
{
    if (storage == NULL || log == NULL || listen_fd < 0 || stop_fd < 0) {
        return GT_ERR_INVAL;
    }
    int conn_fd = -1;
    gt_status status = GT_OK;

    for (;;) {
        /* poll() skips an entry whose fd is negative, so "no hub" is -1. */
        struct pollfd waits[3] = {
            { .fd = listen_fd, .events = POLLIN, .revents = 0 },
            { .fd = stop_fd,   .events = POLLIN, .revents = 0 },
            { .fd = conn_fd,   .events = POLLIN, .revents = 0 },
        };
        if (poll(waits, 3, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = GT_ERR_IO;
            break;
        }

        /* Frames before connects, as in hub_listen: a new entry's revents
         * is never read in the round it was added. */
        if (conn_fd >= 0 && (waits[2].revents & (POLLIN | POLLHUP | POLLERR))
            && read_frame(storage, conn_fd, log, log_ctx) == FRAME_CLOSE) {
            close(conn_fd);
            conn_fd = -1;
            log(log_ctx, GT_LOG_INFO, "hub_disconnected", NULL);
        }
        if (waits[0].revents & POLLIN) {
            conn_fd = accept_hub(storage, listen_fd, conn_fd, log, log_ctx);
        }
        if (waits[1].revents & POLLIN) {
            /* Stopping: take every frame the hub already sent, so nothing
             * waiting in the socket is lost.
             * invariant: every frame read so far has been handed to storage */
            while (conn_fd >= 0) {
                frame_result result = read_frame(storage, conn_fd, log, log_ctx);
                if (result == FRAME_NONE) {
                    break;
                }
                if (result == FRAME_CLOSE) {
                    close(conn_fd);
                    conn_fd = -1;
                    log(log_ctx, GT_LOG_INFO, "hub_disconnected", NULL);
                }
            }
            break;
        }
    }
    if (conn_fd >= 0) {
        close(conn_fd);
    }
    return status;
}

void gt_flusher_init(gt_flusher *flusher, gt_storage *storage, unsigned retention_days,
                     gt_storage_log_fn log, void *log_ctx)
{
    if (flusher == NULL) {
        return;
    }
    flusher->storage = storage;
    flusher->retention_days = retention_days;
    flusher->log = log;
    flusher->log_ctx = log_ctx;
    flusher->logged_unsynced = 0;
    flusher->logged_create_error = 0;
    flusher->clock_unsynced = 0;
    flusher->unsynced_since_ms = 0;
}

/* Records keep flowing while the clock is unsynced (only rollover and
 * retention wait), so the flush runs either way. */
void gt_storage_log_commit(gt_storage_log_fn log, void *log_ctx, const gt_storage *storage,
                           uint64_t commits_before)
{
    gt_storage_stats stats;
    if (log == NULL || gt_storage_read_stats(storage, &stats) != GT_OK
        || stats.commits == commits_before) {
        return;
    }
    char fields[96];
    snprintf(fields, sizeof fields, "records=%lu commit_us=%llu fsync_us=%llu",
             (unsigned long)stats.last_commit_records,
             (unsigned long long)stats.last_commit_us, (unsigned long long)stats.last_fsync_us);
    log(log_ctx, GT_LOG_INFO, "storage_commit", fields);
    if (stats.last_fsync_us > GT_FSYNC_SLOW_US) {
        snprintf(fields, sizeof fields, "fsync_us=%llu records=%lu",
                 (unsigned long long)stats.last_fsync_us,
                 (unsigned long)stats.last_commit_records);
        log(log_ctx, GT_LOG_WARN, "fsync_slow", fields);
    }
}

/* Log the start of an unsynced spell (clock_unsynced) and its end with its
 * length (clock_synced unsynced_ms=N), once each (PF-13). */
static void log_clock_change(gt_flusher *flusher, uint64_t now_ms, int clock_synced)
{
    if (!clock_synced && !flusher->clock_unsynced) {
        flusher->clock_unsynced = 1;
        flusher->unsynced_since_ms = now_ms;
        if (flusher->log != NULL) {
            flusher->log(flusher->log_ctx, GT_LOG_WARN, "clock_unsynced", NULL);
        }
    } else if (clock_synced && flusher->clock_unsynced) {
        flusher->clock_unsynced = 0;
        if (flusher->log != NULL) {
            char fields[48];
            snprintf(fields, sizeof fields, "unsynced_ms=%llu",
                     (unsigned long long)(now_ms - flusher->unsynced_since_ms));
            flusher->log(flusher->log_ctx, GT_LOG_INFO, "clock_synced", fields);
        }
    }
}

/* A pass commits at most once: either the rollover's pre-flush (PF-06),
 * which leaves nothing pending, or the due flush after it. So comparing
 * the commit count before and after finds that one commit (PF-08). */
void gt_flusher_tick(gt_flusher *flusher, uint64_t now_us, uint64_t now_ms, int clock_synced)
{
    if (flusher == NULL || flusher->storage == NULL) {
        return;
    }
    gt_storage_stats before;
    uint64_t commits_before = gt_storage_read_stats(flusher->storage, &before) == GT_OK
                                  ? before.commits : 0;
    log_clock_change(flusher, now_ms, clock_synced);
    unsigned events = 0;
    gt_storage_rollover(flusher->storage, now_us, clock_synced, flusher->retention_days,
                        &events);
    if (events & GT_STORAGE_EV_RETENTION_SKIPPED) {
        if (flusher->logged_unsynced) {
            events &= ~GT_STORAGE_EV_RETENTION_SKIPPED;   /* already logged this spell */
        }
        flusher->logged_unsynced = 1;
    } else if (clock_synced) {
        flusher->logged_unsynced = 0;
    }
    if (events & GT_STORAGE_EV_SEGMENT_CREATE_ERROR) {
        if (flusher->logged_create_error) {
            events &= ~GT_STORAGE_EV_SEGMENT_CREATE_ERROR;   /* already logged this spell */
        }
        flusher->logged_create_error = 1;
    } else if (events & GT_STORAGE_EV_ROLLOVER) {
        flusher->logged_create_error = 0;
    }
    gt_storage_log_events(flusher->log, flusher->log_ctx, events);

    if (gt_storage_flush_due(flusher->storage, now_ms)) {
        unsigned flush_events = 0;
        gt_storage_flush(flusher->storage, &flush_events);
        gt_storage_log_events(flusher->log, flusher->log_ctx, flush_events);
    }
    gt_storage_log_commit(flusher->log, flusher->log_ctx, flusher->storage, commits_before);
}

/* The sleep is cut into FLUSHER_STEP_MS pieces so a stop is noticed
 * within 0.1 s rather than a whole period. */
void gt_flusher_run(gt_flusher *flusher, const atomic_int *stop, int (*clock_synced_fn)(void))
{
    if (flusher == NULL || stop == NULL) {
        return;
    }
    const struct timespec step = { .tv_sec = 0, .tv_nsec = FLUSHER_STEP_MS * 1000000L };
    while (!atomic_load(stop)) {
        int synced = clock_synced_fn != NULL ? clock_synced_fn() : 1;
        gt_flusher_tick(flusher, wall_us(), monotonic_ms(), synced);
        /* invariant: slept_ms of this period have passed, with no stop seen */
        for (int slept_ms = 0; slept_ms < FLUSHER_PERIOD_MS && !atomic_load(stop);
             slept_ms += FLUSHER_STEP_MS) {
            nanosleep(&step, NULL);
        }
    }
}
