/* storage_feed: a load generator standing in for the hub on storage.sock
 * (DESIGN.md Evaluation plan, Test tooling: "writes synthetic records into
 * storage.sock at a set rate").
 *
 * Purpose: drive storage_main before the hub exists. It connects, reads
 * the seq_resume reply, and sends `count` light DATA records continuing
 * from the next seq, exactly as the hub will: seq resume + 1 onward, or 0
 * on an empty log. It prints the reply and what it sent, so a run shows
 * whether storage kept every record across a SIGTERM restart (the reply
 * continues where the last run ended) or a kill -9 (the reply is the last
 * committed seq, and the next records start a recovery_gap).
 *
 * Linux model: an AF_UNIX SOCK_SEQPACKET client, so each send() is one
 * whole 24 B record. MSG_NOSIGNAL turns a storage that went away into an
 * error instead of a SIGPIPE.
 *
 * Usage: storage_feed /run/plantpi/storage.sock COUNT [RATE_HZ]
 *   RATE_HZ defaults to 10; 0 sends as fast as possible. */
#include <errno.h>
#include <poll.h>
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

/* Connect to storage.sock. Returns the fd, or -1 after printing why. */
static int connect_storage(const char *path)
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
        fprintf(stderr, "storage_feed: %s: %s\n", path, strerror(errno));
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

/* Wall-clock time in us since the epoch, stamped into each record as the
 * hub would. */
static uint64_t wall_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

int main(int argc, char **argv)
{
    unsigned long count = 0, rate_hz = 10;
    if ((argc != 3 && argc != 4) || !parse_count(argv[2], &count)
        || (argc == 4 && !parse_count(argv[3], &rate_hz))) {
        fprintf(stderr, "usage: %s /run/plantpi/storage.sock COUNT [RATE_HZ]\n", argv[0]);
        return 2;
    }
    int fd = connect_storage(argv[1]);
    gt_resume resume;
    if (fd < 0 || !read_resume(fd, &resume)) {
        if (fd >= 0) {
            close(fd);
        }
        return 1;
    }
    uint32_t first_seq = resume.has_last ? resume.seq + 1 : 0;
    if (resume.has_last) {
        printf("seq_resume: last=%lu, sending from %lu\n", (unsigned long)resume.seq,
               (unsigned long)first_seq);
    } else {
        printf("seq_resume: empty log, sending from 0\n");
    }

    struct timespec gap = { 0, 0 };
    if (rate_hz > 0) {
        gap.tv_sec = (time_t)(1 / rate_hz);
        gap.tv_nsec = (long)((1000000000ul / rate_hz) % 1000000000ul);
    }
    unsigned long sent = 0;
    /* invariant: records first_seq .. first_seq + sent - 1 have been sent */
    while (sent < count) {
        const gt_record record = {
            .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
            .seq = first_seq + (uint32_t)sent, .timestamp_us = wall_us(),
            .value = (uint32_t)sent,
        };
        uint8_t wire[GT_REC_SIZE];
        if (gt_record_encode(&record, wire) != GT_OK
            || send(fd, wire, sizeof wire, MSG_NOSIGNAL) != (ssize_t)sizeof wire) {
            fprintf(stderr, "storage_feed: send seq %lu: %s\n",
                    (unsigned long)record.seq, strerror(errno));
            break;
        }
        sent++;
        if (rate_hz > 0) {
            nanosleep(&gap, NULL);
        }
    }
    if (sent > 0) {
        printf("sent %lu records, seq %lu..%lu\n", sent, (unsigned long)first_seq,
               (unsigned long)(first_seq + sent - 1));
    } else {
        printf("sent 0 records\n");
    }
    close(fd);
    return sent == count ? 0 : 1;
}
