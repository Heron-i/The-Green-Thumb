/* The storage process's rx loop on storage.sock (see storage_server.h);
 * the flush thread, the process's other loop, is storage_flusher.c.
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

/* Monotonic time in ms (never jumps when the wall clock is set), the clock
 * every append and flush_due uses. */
static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
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

/* Close a hub's connection and log it gone. Returns -1, the "no hub"
 * value, so a caller can write conn_fd = drop_hub(conn_fd, ...). */
static int drop_hub(int conn_fd, gt_storage_log_fn log, void *log_ctx)
{
    close(conn_fd);
    log(log_ctx, GT_LOG_INFO, "hub_disconnected", NULL);
    return -1;
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
        return drop_hub(new_fd, log, log_ctx);
    }
    return new_fd;
}

/* Stopping: take every frame the hub already sent, so nothing waiting in
 * the socket is lost. Returns the connection afterwards (-1 if the hub
 * closed it or sent a bad frame). */
static int drain_on_stop(gt_storage *storage, int conn_fd, gt_storage_log_fn log,
                         void *log_ctx)
{
    /* invariant: every frame read so far has been handed to storage */
    while (conn_fd >= 0) {
        frame_result result = read_frame(storage, conn_fd, log, log_ctx);
        if (result == FRAME_NONE) {
            break;
        }
        if (result == FRAME_CLOSE) {
            conn_fd = drop_hub(conn_fd, log, log_ctx);
        }
    }
    return conn_fd;
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
            conn_fd = drop_hub(conn_fd, log, log_ctx);
        }
        if (waits[0].revents & POLLIN) {
            conn_fd = accept_hub(storage, listen_fd, conn_fd, log, log_ctx);
        }
        if (waits[1].revents & POLLIN) {
            conn_fd = drain_on_stop(storage, conn_fd, log, log_ctx);
            break;
        }
    }
    if (conn_fd >= 0) {
        close(conn_fd);
    }
    return status;
}
