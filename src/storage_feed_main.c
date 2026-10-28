/* storage_feed: a load generator standing in for the hub on storage.sock
 * (DESIGN.md Evaluation plan, Test tooling: "writes synthetic records into
 * storage.sock at a set rate").
 *
 * Purpose: drive storage_main before the hub exists. It connects, reads
 * the seq_resume reply, and sends `count` light DATA records continuing
 * from the next seq, exactly as a freshly started hub will (gt_next_seq):
 * seq resume + 1 onward, or 0 on an empty log. It prints the reply and
 * what it sent, so a run shows whether storage kept every record across
 * a SIGTERM restart (the reply continues where the last run ended) or a
 * kill -9 (the reply is the last committed seq; a fresh sender continues
 * from there, so the lost seqs are simply reused, not marked as a GAP).
 *
 * stdout is line-buffered, so each line reaches a file or pipe as soon
 * as it is printed (a script can read the reply while the feed runs).
 *
 * Linux model: an AF_UNIX SOCK_SEQPACKET client, so each send() is one
 * whole 24 B record. MSG_NOSIGNAL turns a storage that went away into an
 * error instead of a SIGPIPE.
 *
 * With -r it acts as a hub that keeps running while storage restarts:
 * when a send fails it prints "storage gone after seq K", reconnects
 * every 100 ms, and continues from max(own next seq, reply + 1), so seqs
 * a kill -9 lost leave a hole that storage covers with a GAP (PF-03).
 * COUNT is then the total over all connections, and 0 means no limit.
 * SIGTERM / SIGINT stop the feed cleanly: it still prints its final
 * "sent N records, seq A..B" line (exit 0 with -r).
 *
 * Usage: storage_feed [-r] /run/plantpi/storage.sock COUNT [RATE_HZ]
 *   RATE_HZ defaults to 10; 0 sends as fast as possible. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "log_record.h"
#include "storage_proto.h"

#define REPLY_WAIT_MS 2000

/* Set by SIGTERM / SIGINT. sig_atomic_t is the integer type a signal
 * handler may safely write; volatile makes the loop re-read it each pass. */
static volatile sig_atomic_t stop_requested = 0;

/* Only sets the flag: printing or closing here would not be signal-safe. */
static void on_stop(int signo)
{
    (void)signo;
    stop_requested = 1;
}

/* Parse a whole non-negative decimal number. Returns 1 on success. */
static int parse_count(const char *text, unsigned long *value_out)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || text[0] == '-') {
        return 0;
    }
    *value_out = value;
    return 1;
}

/* Connect to storage.sock. Returns the fd, or -1 after printing why
 * (unless quiet is set). */
static int connect_storage(const char *path, int quiet)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    if (strlen(path) >= sizeof addr.sun_path) {
        fprintf(stderr, "storage_feed: socket path too long\n");
        return -1;
    }
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0 || connect(fd, (const struct sockaddr *)&addr, sizeof addr) != 0) {
        if (!quiet) {
            fprintf(stderr, "storage_feed: %s: %s\n", path, strerror(errno));
        }
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    return fd;
}

/* Wait for and decode the seq_resume reply. Returns 1 on success. */
static int read_resume(int fd, gt_resume *resume)
{
    struct pollfd wait_for = { .fd = fd, .events = POLLIN, .revents = 0 };
    uint8_t reply[GT_RESUME_SIZE + 1];
    if (poll(&wait_for, 1, REPLY_WAIT_MS) != 1) {
        fprintf(stderr, "storage_feed: no seq_resume reply within %d ms\n", REPLY_WAIT_MS);
        return 0;
    }
    ssize_t got = recv(fd, reply, sizeof reply, 0);
    if (got != GT_RESUME_SIZE || gt_resume_decode(reply, resume) != GT_OK) {
        fprintf(stderr, "storage_feed: bad seq_resume reply (%ld bytes)\n", (long)got);
        return 0;
    }
    return 1;
}

/* Connect, read seq_resume, pick the next seq with gt_next_seq and print
 * the usual "seq_resume: ..." line. has_own/own_next are passed straight
 * to gt_next_seq. quiet hides connect errors (reconnect mode expects them
 * while storage is down). Returns the fd, or -1 on any failure. */
static int open_session(const char *path, int quiet, int has_own,
                        uint32_t own_next, uint32_t *next_out)
{
    int fd = connect_storage(path, quiet);
    gt_resume resume;
    if (fd < 0 || !read_resume(fd, &resume)) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    if (gt_next_seq(has_own, own_next, &resume, next_out) != GT_OK) {
        fprintf(stderr, "storage_feed: no next seq after %lu\n", (unsigned long)resume.seq);
        close(fd);
        return -1;
    }
    if (resume.has_last) {
        printf("seq_resume: last=%lu, sending from %lu\n", (unsigned long)resume.seq,
               (unsigned long)*next_out);
    } else {
        printf("seq_resume: empty log, sending from %lu\n", (unsigned long)*next_out);
    }
    return fd;
}

/* Wall-clock time in us since the epoch, stamped into each record as the
 * hub would. */
static uint64_t wall_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* What one pass of the send loop came to. An enum gives names to a small
 * set of int values, so the caller compares against FEED_STOP rather than
 * a bare number. */
typedef enum { FEED_SENT, FEED_RETRY, FEED_STOP } feed_result;

/* Everything the send loop changes as it goes, in one struct so a helper
 * can update it through a single pointer. */
typedef struct {
    int           fd;          /* the connection to storage; -1 if none */
    uint32_t      own_next;    /* the next seq to send */
    uint32_t      first_seq;   /* the first seq sent (valid once sent > 0) */
    unsigned long sent;        /* records sent, over all connections */
} feed_state;

/* One pass of the send loop: connect first if there is no connection
 * (only with -r), then send the record with seq state->own_next.
 *   FEED_SENT:  it went; own_next and sent moved on.
 *   FEED_RETRY: storage is not up yet or went away (only with -r); the
 *               next pass reconnects.
 *   FEED_STOP:  give up: a send failed without -r, or encoding failed. */
static feed_result send_next(feed_state *state, const char *sock_path, int reconnect)
{
    if (state->fd < 0) {
        state->fd = open_session(sock_path, 1, state->sent > 0, state->own_next,
                                 &state->own_next);
        if (state->fd < 0) {
            const struct timespec retry_wait = { 0, 100000000L }; /* 100 ms */
            nanosleep(&retry_wait, NULL);
            return FEED_RETRY;
        }
    }
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .seq = state->own_next, .timestamp_us = wall_us(),
        .value = (uint32_t)state->sent,
    };
    uint8_t wire[GT_REC_SIZE];
    if (gt_record_encode(&record, wire) != GT_OK) {
        fprintf(stderr, "storage_feed: encode seq %lu failed\n", (unsigned long)record.seq);
        return FEED_STOP;
    }
    if (send(state->fd, wire, sizeof wire, MSG_NOSIGNAL) != (ssize_t)sizeof wire) {
        if (!reconnect) {
            fprintf(stderr, "storage_feed: send seq %lu: %s\n",
                    (unsigned long)record.seq, strerror(errno));
            return FEED_STOP;
        }
        if (state->sent > 0) {
            printf("storage gone after seq %lu\n", (unsigned long)(state->own_next - 1));
        } else {
            printf("storage gone before any seq\n");
        }
        close(state->fd);
        state->fd = -1;
        return FEED_RETRY;
    }
    if (state->sent == 0) {
        state->first_seq = state->own_next;
    }
    state->own_next++;
    state->sent++;
    return FEED_SENT;
}

/* The command line, parsed. */
typedef struct {
    int           reconnect;   /* 1 if -r was given */
    const char   *sock_path;
    unsigned long count;       /* records to send; 0 with -r means no limit */
    unsigned long rate_hz;     /* sends per second; 0 means as fast as possible */
} feed_opts;

/* Route SIGTERM and SIGINT to on_stop. No SA_RESTART: a stop signal cuts
 * nanosleep/poll short (EINTR), so the loop sees stop_requested at once. */
static void install_stop_handlers(void)
{
    struct sigaction stop_action;
    memset(&stop_action, 0, sizeof stop_action);
    stop_action.sa_handler = on_stop;
    sigemptyset(&stop_action.sa_mask);
    sigaction(SIGTERM, &stop_action, NULL);
    sigaction(SIGINT, &stop_action, NULL);
}

/* Parse "[-r] SOCK COUNT [RATE_HZ]" into `opts`. Returns 1 on success, or
 * 0 after printing usage. */
static int parse_feed_args(int argc, char **argv, feed_opts *opts)
{
    /* -r, if given, comes first; arg_base is then the index of SOCK. */
    opts->reconnect = argc > 1 && strcmp(argv[1], "-r") == 0;
    int arg_base = opts->reconnect ? 2 : 1;
    int args_left = argc - arg_base;
    opts->sock_path = argv[arg_base];
    opts->count = 0;
    opts->rate_hz = 10;
    if ((args_left != 2 && args_left != 3) || !parse_count(argv[arg_base + 1], &opts->count)
        || (args_left == 3 && !parse_count(argv[arg_base + 2], &opts->rate_hz))) {
        fprintf(stderr, "usage: %s [-r] /run/plantpi/storage.sock COUNT [RATE_HZ]\n",
                argv[0]);
        return 0;
    }
    return 1;
}

/* The pause between sends for `rate_hz` sends per second (zero for 0). */
static struct timespec rate_to_delay(unsigned long rate_hz)
{
    struct timespec delay = { 0, 0 };
    if (rate_hz > 0) {
        delay.tv_sec = (time_t)(1 / rate_hz);
        delay.tv_nsec = (long)((1000000000ul / rate_hz) % 1000000000ul);
    }
    return delay;
}

/* 1 while the send loop should go on: no stop signal yet, and either -r
 * with no limit or fewer than COUNT records sent. */
static int should_keep_sending(const feed_opts *opts, const feed_state *state)
{
    return !stop_requested && ((opts->reconnect && opts->count == 0) || state->sent < opts->count);
}

/* Print the final "sent N records" line, close the connection if one is
 * open, and return the exit code: 0 if all COUNT records went, or if -r
 * was stopped by a signal; 1 otherwise. */
static int close_and_report(feed_state *state, const feed_opts *opts)
{
    if (state->sent > 0) {
        printf("sent %lu records, seq %lu..%lu\n", state->sent, (unsigned long)state->first_seq,
               (unsigned long)(state->own_next - 1));
    } else {
        printf("sent 0 records\n");
    }
    if (state->fd >= 0) {
        close(state->fd);
    }
    return (state->sent == opts->count || (opts->reconnect && stop_requested)) ? 0 : 1;
}

int main(int argc, char **argv)
{
    /* _IOLBF = line buffering: flush stdout at every '\n', even when it is
     * redirected to a file (the default there is to hold it until exit). */
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_stop_handlers();
    feed_opts opts;
    if (!parse_feed_args(argc, argv, &opts)) {
        return 2;
    }

    struct timespec delay = rate_to_delay(opts.rate_hz);
    feed_state state = { .fd = -1, .own_next = 0, .first_seq = 0, .sent = 0 };
    if (!opts.reconnect) {
        state.fd = open_session(opts.sock_path, 0, 0, 0, &state.own_next);
        if (state.fd < 0) {
            return 1;
        }
    }
    /* invariant: every seq below state.own_next was sent exactly once; none
     * is reused */
    while (should_keep_sending(&opts, &state)) {
        feed_result result = send_next(&state, opts.sock_path, opts.reconnect);
        if (result == FEED_STOP) {
            break;
        }
        if (result == FEED_SENT && opts.rate_hz > 0) {
            nanosleep(&delay, NULL);
        }
    }
    return close_and_report(&state, &opts);
}
