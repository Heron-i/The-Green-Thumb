/* hub_main: the hub process (DESIGN.md "hub process"; Sensor-Pipeline.md,
 * Hub steps H8). It replaces the hub_listen stand-in.
 *
 * Purpose: wire the tested pieces together into one long-running process.
 * Every decision lives in the library (hub.c judges messages, hub_sender.c
 * numbers and sends records, hub_server.c runs the loop, listen_sock.c
 * owns the socket); this file only orders the start-up, handles signals
 * and prints log lines. Start-up order:
 *   1. Take the one-listener lock for hub.sock, then bind it. A second
 *      copy exits with "already running" and touches nothing.
 *   2. Make the sender for storage.sock. It doesn't connect yet: storage
 *      may still be starting or recovering (Mechanism E), and readings
 *      wait in its 1024-record queue meanwhile.
 *   3. Run the rx loop on this thread until SIGTERM or SIGINT.
 * The signal handler only writes one byte to a pipe (all a handler may
 * safely do); the loop wakes, takes the messages already sent and returns.
 * Then the queue gets up to DRAIN_MAX_MS to reach storage, and hub_stop
 * logs the range of seqs this run sent ("first_seq=A last_seq=B"), which
 * is what `log_verify --sent A..B` checks the log against (PF-12).
 * Records still queued after that are dropped and counted (dropped=Q).
 *
 * Log lines go to stderr in gt_log_format's format, component "hub" (the
 * shared sink, PF-18). The heartbeat to sup.sock arrives with Mechanism E.
 *
 * Usage: hub_main /run/plantpi/hub.sock /run/plantpi/storage.sock */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gt_log.h"
#include "hub.h"
#include "hub_sender.h"
#include "hub_server.h"
#include "listen_sock.h"

/* How long a stopping hub keeps trying to hand its queue to storage. The
 * supervisor allows 5 s after SIGTERM before SIGKILL, so 1 s is safe. */
#define DRAIN_MAX_MS 1000

/* The write end of the stop pipe, for the signal handler. */
static int stop_write_fd = -1;

/* Wall-clock time in us since the epoch, for log lines. */
static uint64_t wall_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* Monotonic time in ms, for the hub's timers and the drain deadline. */
static uint64_t mono_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* The log callback: one gt_log_format line on stderr. */
static void log_line(void *log_ctx, gt_log_level level, const char *event, const char *fields)
{
    (void)log_ctx;
    char line[GT_LOG_LINE_MAX];
    gt_log_entry entry = { .timestamp_us = wall_us(), .level = level, .component = "hub",
                           .event = event, .fields = fields };
    if (gt_log_format(line, sizeof line, &entry) == GT_OK) {
        fprintf(stderr, "%s\n", line);
    }
}

/* SIGTERM / SIGINT: wake the rx loop's poll() by writing one byte to the
 * stop pipe. errno is saved and restored, because write() may change it
 * under the code the signal interrupted. */
static void on_stop_signal(int signum)
{
    (void)signum;
    int saved_errno = errno;
    const char stop_byte = 's';
    if (stop_write_fd >= 0 && write(stop_write_fd, &stop_byte, 1) < 0) {
        /* nothing more a signal handler can do */
    }
    errno = saved_errno;
}

/* Install the stop handler for SIGTERM and SIGINT, and ignore SIGPIPE (a
 * second guard, besides MSG_NOSIGNAL, against a dead storage killing the
 * hub). `struct sigaction` describes what to do on a signal; no
 * SA_RESTART, so poll() returns EINTR and the loop goes round to see the
 * stop byte. */
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

/* Start-up step 1: take hub.sock's one-listener lock, then bind it (a
 * socket file left by a hub killed with kill -9 is replaced; anything else
 * in the way is left alone). Returns the listening fd and sets *lock_fd_out,
 * or returns -1 after saying why, with nothing left open. */
static int listen_on_hub_sock(const char *hub_sock, int *lock_fd_out)
{
    int lock_fd = -1, already_running = 0;
    gt_status status = gt_listen_lock(hub_sock, &lock_fd, &already_running);
    if (status != GT_OK) {
        if (already_running) {
            fprintf(stderr, "hub_main: already running on %s (%s.lock is locked)\n",
                    hub_sock, hub_sock);
        } else {
            fprintf(stderr, "hub_main: lock %s.lock: %s\n", hub_sock, strerror(errno));
        }
        return -1;
    }
    int listen_fd = -1;
    status = gt_listen_bind(hub_sock, GT_HUB_MAX_SENSOR_CONNS, &listen_fd);
    if (status != GT_OK) {
        fprintf(stderr, status == GT_ERR_INVAL
                            ? "hub_main: %s exists and is not a socket; not removing it\n"
                            : "hub_main: %s: bind failed\n", hub_sock);
        gt_listen_release(hub_sock, -1, lock_fd);
        return -1;
    }
    *lock_fd_out = lock_fd;
    return listen_fd;
}

/* After the loop stops: keep pumping the sender every 10 ms until its
 * queue is empty or DRAIN_MAX_MS has passed, so a SIGTERM costs no
 * readings while storage is up. If storage is down the deadline ends it
 * and the rest is reported as dropped. The sender's events are not logged
 * here: the hub is stopping, and hub_stop sums it up. */
static void drain_queue(gt_hub_sender *sender)
{
    const struct timespec ten_ms = { 0, 10000000L };
    uint64_t give_up_at = mono_ms() + DRAIN_MAX_MS;
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    /* Invariant: every record that left the queue was sent with a fresh seq. */
    while (stats.queued > 0 && mono_ms() < give_up_at) {
        gt_hub_sender_pump(sender, mono_ms(), NULL);
        gt_hub_sender_stats(sender, &stats);
        if (stats.queued > 0) {
            nanosleep(&ten_ms, NULL);
        }
    }
}

/* Log hub_stop with what this run sent: sent=N, first_seq=A and
 * last_seq=B (the range `log_verify --sent A..B` takes: within one hub
 * run the seqs sent are exactly A..B, PF-12), refused=R (readings refused
 * while the queue was full) and dropped=Q (still queued at exit, lost).
 * A run that sent nothing has no range: none..none. Dropped records are
 * WARN, because they are a real loss with no GAP to cover them. */
static void log_hub_stop(const gt_hub_sender *sender)
{
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    char first_seq[16] = "none";
    char last_seq[16] = "none";
    if (stats.has_own) {
        snprintf(first_seq, sizeof first_seq, "%lu", (unsigned long)stats.first_sent_seq);
        snprintf(last_seq, sizeof last_seq, "%lu", (unsigned long)(stats.next_seq - 1u));
    }
    char fields[128];
    snprintf(fields, sizeof fields, "sent=%lu first_seq=%s last_seq=%s refused=%lu dropped=%lu",
             (unsigned long)stats.sent, first_seq, last_seq, (unsigned long)stats.refused,
             (unsigned long)stats.queued);
    log_line(NULL, stats.queued > 0 ? GT_LOG_WARN : GT_LOG_INFO, "hub_stop", fields);
}

/* The whole life of the process after the arguments are read: start-up
 * steps 1-3, the loop, the drain and hub_stop, then release everything.
 * Returns the exit code: 0 for a clean stop, 1 for any failure. */
static int run_hub(const char *hub_sock, const char *storage_sock)
{
    /* 1. The lock and hub.sock. */
    int lock_fd = -1;
    int listen_fd = listen_on_hub_sock(hub_sock, &lock_fd);
    if (listen_fd < 0) {
        return 1;
    }

    /* 2. The sender (no connect yet), the hub's per-sensor state, and the
     * stop pipe the signal handler writes to. */
    gt_hub_sender *sender = gt_hub_sender_open(storage_sock);
    gt_hub *hub = gt_hub_create(mono_ms());
    int stop_pipe[2] = { -1, -1 };
    if (sender == NULL || hub == NULL || pipe(stop_pipe) != 0) {
        fprintf(stderr, "hub_main: can't start (bad storage socket path %s, or out of memory)\n",
                storage_sock);
        gt_hub_sender_close(sender);
        gt_hub_destroy(hub);
        gt_listen_release(hub_sock, listen_fd, lock_fd);
        return 1;
    }
    stop_write_fd = stop_pipe[1];
    install_signals();
    char fields[160];
    snprintf(fields, sizeof fields, "hub_sock=%s storage_sock=%s", hub_sock, storage_sock);
    log_line(NULL, GT_LOG_INFO, "hub_start", fields);

    /* 3. The loop, until a stop. */
    const gt_hub_serve_ctx ctx = { .hub = hub, .sender = sender, .log = log_line, .log_ctx = NULL };
    gt_status serve_status = gt_hub_serve(&ctx, listen_fd, stop_pipe[0]);
    if (serve_status != GT_OK) {
        log_line(NULL, GT_LOG_ERROR, "rx_loop_error", NULL);
    }

    /* Stop: give the queue its last chance, report the run, release. */
    drain_queue(sender);
    log_hub_stop(sender);
    stop_write_fd = -1;
    close(stop_pipe[0]);
    close(stop_pipe[1]);
    gt_hub_sender_close(sender);
    gt_hub_destroy(hub);
    gt_listen_release(hub_sock, listen_fd, lock_fd);
    return serve_status == GT_OK ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s /run/plantpi/hub.sock /run/plantpi/storage.sock\n", argv[0]);
        return 2;
    }
    return run_hub(argv[1], argv[2]);
}
