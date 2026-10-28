/* Loopback tests for the storage rx loop over a real socket
 * (src/storage_server.c): gt_storage_serve.
 *
 * Purpose: prove the loop does over a real AF_UNIX SOCK_SEQPACKET socket
 * what gt_storage_rx decides on paper. A hub that connects is sent
 * seq_resume at once. Its frames are stored, and a duplicate is refused
 * while the connection stays open. A malformed frame closes the
 * connection, so the hub sees end-of-file and reconnects. A newer hub
 * connection replaces an older one. On stop, frames already sent are
 * still taken before it returns.
 *
 * The loop runs in a second thread (pthread_create starts a function on
 * its own thread; pthread_join waits for it to finish), over storage on
 * tests/fake_file.h: only the socket is real. Each test uses its own
 * mkdtemp directory under /tmp and removes it after. Waits are bounded by
 * poll() timeouts, so a broken loop is a FAIL line, not a hang. Run under
 * make test / asan / memcheck. Implements SSV-01 through SSV-07.
 *
 * Note: macOS does not support SOCK_SEQPACKET for AF_UNIX, so these run on
 * Linux (WSL and the Pi) only, like test_listen_bind.c. */
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.h"
#include "fake_file.h"
#include "listen_sock.h"
#include "log_record.h"
#include "storage.h"
#include "storage_server.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TODAY_NAME    "log-20261026.bin"
#define TEST_CAPACITY 16
#define WAIT_MS       2000   /* longest any test waits for the loop */

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* Event names the loop logged, in order. Written only by the loop's
 * thread, read only after it has been joined. */
typedef struct {
    size_t count;
    char   events[32][32];
} event_book;

static event_book book;

static void record_log(void *log_ctx, gt_log_level level, const char *event, const char *fields)
{
    event_book *records = log_ctx;
    (void)level;
    (void)fields;
    if (records->count < 32) {
        strncpy(records->events[records->count], event, sizeof records->events[0] - 1);
        records->count++;
    }
}

/* How many logged events were called `event`. */
static size_t logged(const char *event)
{
    size_t matches = 0;
    for (size_t log_index = 0; log_index < book.count; log_index++) {
        if (strcmp(book.events[log_index], event) == 0) {
            matches++;
        }
    }
    return matches;
}

/* One test's server: the directory, socket path, lock, listener, stop
 * pipe (stop_pipe[1] is written to stop the loop), storage, the loop's
 * thread and what it returned. */
typedef struct {
    char        dir[32];
    char        path[64];
    int         lock_fd;
    int         listen_fd;
    int         stop_pipe[2];
    gt_storage *storage;
    pthread_t   thread;
    gt_status   result;
} server;

/* The thread's body: run the loop and keep what it returned. */
static void *serve_thread(void *arg)
{
    server *running = arg;
    running->result = gt_storage_serve(running->storage, running->listen_fd,
                                       running->stop_pipe[0], record_log, &book);
    return NULL;
}

/* Make storage (over the fake, with records 1 .. last_on_disk already in
 * today's segment), a listener in a fresh directory, and start the loop.
 * Returns 1 on success. */
static int start_server(server *running, uint32_t last_on_disk)
{
    memset(running, 0, sizeof *running);
    memset(&book, 0, sizeof book);
    const gt_file_ops *ops = fake_fs_ops(&fs);
    if (last_on_disk > 0) {
        uint8_t segment[GT_SEG_HEADER_SZ + 8 * GT_REC_SIZE];
        const gt_seg_header header = { .created_at_us = 1000 };
        CHECK_EQ(gt_seg_header_encode(&header, segment), GT_OK);
        /* invariant: records 1 .. seq - 1 are encoded after the header */
        for (uint32_t seq = 1; seq <= last_on_disk; seq++) {
            const gt_record record = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
                                       .seq = seq, .timestamp_us = seq, .value = seq };
            CHECK_EQ(gt_record_encode(&record, &segment[GT_SEG_HEADER_SZ
                                                        + (seq - 1) * GT_REC_SIZE]), GT_OK);
        }
        CHECK_EQ(fake_fs_put(&fs, TODAY_NAME, segment,
                             GT_SEG_HEADER_SZ + last_on_disk * GT_REC_SIZE), GT_OK);
    }
    gt_storage_open_args args = { .ops = ops, .ctx = &fs, .now_us = TODAY_US,
                                  .pending_capacity = TEST_CAPACITY };
    if (gt_storage_open_ops(&args, &running->storage, NULL) != GT_OK) {
        return 0;
    }
    strcpy(running->dir, "/tmp/gt-ssv-XXXXXX");
    if (mkdtemp(running->dir) == NULL) {
        return 0;
    }
    snprintf(running->path, sizeof running->path, "%s/storage.sock", running->dir);
    int already_running = 0;
    if (gt_listen_lock(running->path, &running->lock_fd, &already_running) != GT_OK
        || gt_listen_bind(running->path, 4, &running->listen_fd) != GT_OK
        || pipe(running->stop_pipe) != 0) {
        return 0;
    }
    return pthread_create(&running->thread, NULL, serve_thread, running) == 0;
}

/* Stop the loop (one byte down the stop pipe), wait for its thread, and
 * check it returned GT_OK. Storage stays open for the test to inspect. */
static void stop_server(server *running)
{
    const char stop_byte = 's';
    CHECK(write(running->stop_pipe[1], &stop_byte, 1) == 1);
    pthread_join(running->thread, NULL);
    CHECK_EQ(running->result, GT_OK);
}

/* Free everything start_server made. */
static void remove_server(server *running)
{
    gt_storage_close(running->storage);
    close(running->stop_pipe[0]);
    close(running->stop_pipe[1]);
    gt_listen_release(running->path, running->listen_fd, running->lock_fd);
    char lock_path[80];
    snprintf(lock_path, sizeof lock_path, "%s.lock", running->path);
    unlink(lock_path);
    rmdir(running->dir);
}

/* Connect a client (the hub) to the server. Returns the fd, or -1. */
static int connect_hub(const server *running)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, running->path, sizeof addr.sun_path - 1);
    int hub_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (hub_fd >= 0 && connect(hub_fd, (const struct sockaddr *)&addr, sizeof addr) != 0) {
        close(hub_fd);
        hub_fd = -1;
    }
    return hub_fd;
}

/* Wait up to WAIT_MS for something to read on hub_fd, then recv it into
 * buf. Returns recv's result, or -2 if nothing came in time. */
static long recv_waiting(int hub_fd, uint8_t *buf, size_t buf_size)
{
    struct pollfd wait_for = { .fd = hub_fd, .events = POLLIN, .revents = 0 };
    if (poll(&wait_for, 1, WAIT_MS) != 1) {
        return -2;
    }
    return (long)recv(hub_fd, buf, buf_size, 0);
}

/* Read the seq_resume reply a new connection is sent, and decode it. */
static int read_resume(int hub_fd, gt_resume *resume)
{
    uint8_t reply[GT_RESUME_SIZE + 1];
    return recv_waiting(hub_fd, reply, sizeof reply) == GT_RESUME_SIZE
           && gt_resume_decode(reply, resume) == GT_OK;
}

/* Send a light DATA record with `seq`, as the hub would. */
static void send_record(int hub_fd, uint32_t seq)
{
    const gt_record record = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
                               .seq = seq, .timestamp_us = 1000 + seq, .value = seq };
    uint8_t wire[GT_REC_SIZE];
    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
    CHECK(send(hub_fd, wire, sizeof wire, 0) == (ssize_t)sizeof wire);
}

/* The newest accepted seq and the pending count. */
static gt_storage_stats read_stats(const server *running)
{
    gt_storage_stats stats;
    CHECK_EQ(gt_storage_read_stats(running->storage, &stats), GT_OK);
    return stats;
}

/* ---- SSV-01: a new hub is sent seq_resume ----
 * Empty log: has_last 0. Log ending at 5: seq 5. */
static void test_ssv01_resume_on_connect(void)
{
    for (uint32_t last_on_disk = 0; last_on_disk <= 5; last_on_disk += 5) {
        server running;
        gt_resume resume;
        CHECK(start_server(&running, last_on_disk));
        int hub_fd = connect_hub(&running);
        CHECK(hub_fd >= 0);

        CHECK(read_resume(hub_fd, &resume));
        CHECK_EQ(resume.has_last, last_on_disk > 0);
        CHECK_EQ(resume.seq, last_on_disk);

        close(hub_fd);
        stop_server(&running);
        CHECK_EQ(logged("hub_connected"), 1);
        remove_server(&running);
    }
}

/* ---- SSV-02: frames are stored, even at stop ----
 * The hub sends 6, 7, 8 and storage is stopped at once: all three were
 * already in the socket, so all three are accepted before it returns. */
static void test_ssv02_frames_stored_at_stop(void)
{
    server running;
    gt_resume resume;
    CHECK(start_server(&running, 5));
    int hub_fd = connect_hub(&running);
    CHECK(read_resume(hub_fd, &resume));

    send_record(hub_fd, 6);
    send_record(hub_fd, 7);
    send_record(hub_fd, 8);
    stop_server(&running);

    gt_storage_stats stats = read_stats(&running);
    CHECK_EQ(stats.pending, 3);
    CHECK_EQ(stats.last_accepted_seq, 8);
    close(hub_fd);
    remove_server(&running);
}

/* ---- SSV-03: a malformed frame closes the hub ----
 * A 16 B frame (a sensor message on the wrong socket): the hub sees
 * end-of-file (recv returns 0), and ipc_bad_message is logged. */
static void test_ssv03_malformed_closes(void)
{
    server running;
    gt_resume resume;
    uint8_t junk[16] = { 0xA6 };
    uint8_t buf[8];
    CHECK(start_server(&running, 0));
    int hub_fd = connect_hub(&running);
    CHECK(read_resume(hub_fd, &resume));

    CHECK(send(hub_fd, junk, sizeof junk, 0) == (ssize_t)sizeof junk);
    CHECK_EQ(recv_waiting(hub_fd, buf, sizeof buf), 0);

    close(hub_fd);
    stop_server(&running);
    CHECK_EQ(logged("ipc_bad_message"), 1);
    CHECK_EQ(logged("hub_disconnected"), 1);
    CHECK_EQ(read_stats(&running).pending, 0);
    remove_server(&running);
}

/* ---- SSV-04: a newer hub replaces the older ----
 * The first connection is closed (end-of-file) when a second arrives, and
 * the second's records are stored. */
static void test_ssv04_newer_hub_replaces(void)
{
    server running;
    gt_resume resume;
    uint8_t buf[8];
    CHECK(start_server(&running, 0));
    int old_hub = connect_hub(&running);
    CHECK(read_resume(old_hub, &resume));
    int new_hub = connect_hub(&running);
    CHECK(read_resume(new_hub, &resume));

    CHECK_EQ(recv_waiting(old_hub, buf, sizeof buf), 0);
    send_record(new_hub, 1);
    stop_server(&running);

    CHECK_EQ(logged("hub_duplicate_client"), 1);
    CHECK_EQ(read_stats(&running).last_accepted_seq, 1);
    close(old_hub);
    close(new_hub);
    remove_server(&running);
}

/* ---- SSV-05: a duplicate keeps the connection ----
 * 6 twice, then 7: the repeat is refused (seq_rejected) but the hub is
 * not disconnected, and 7 is stored on the same connection. */
static void test_ssv05_duplicate_keeps(void)
{
    server running;
    gt_resume resume;
    CHECK(start_server(&running, 5));
    int hub_fd = connect_hub(&running);
    CHECK(read_resume(hub_fd, &resume));

    send_record(hub_fd, 6);
    send_record(hub_fd, 6);
    send_record(hub_fd, 7);
    stop_server(&running);

    CHECK_EQ(logged("seq_rejected"), 1);
    CHECK_EQ(logged("hub_disconnected"), 0);
    CHECK_EQ(read_stats(&running).last_accepted_seq, 7);
    close(hub_fd);
    remove_server(&running);
}

/* ---- SSV-06: stop with no hub returns ----
 * Nothing connected: the loop still returns GT_OK at once. */
static void test_ssv06_stop_with_no_hub(void)
{
    server running;
    CHECK(start_server(&running, 0));
    stop_server(&running);
    CHECK_EQ(book.count, 0);
    remove_server(&running);
}

/* ---- SSV-07: bad arguments are rejected ---- */
static void test_ssv07_bad_args(void)
{
    gt_storage *storage = NULL;
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage_open_args args = { .ops = ops, .ctx = &fs, .now_us = TODAY_US,
                                  .pending_capacity = TEST_CAPACITY };
    CHECK_EQ(gt_storage_open_ops(&args, &storage, NULL), GT_OK);

    CHECK_EQ(gt_storage_serve(NULL, 3, 4, record_log, &book), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_serve(storage, -1, 4, record_log, &book), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_serve(storage, 3, -1, record_log, &book), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_serve(storage, 3, 4, NULL, &book), GT_ERR_INVAL);
    gt_storage_close(storage);
}

static const test_case_t tests[] = {
    {"SSV-01", "a new hub is sent seq_resume", test_ssv01_resume_on_connect},
    {"SSV-02", "frames are stored, even at stop", test_ssv02_frames_stored_at_stop},
    {"SSV-03", "a malformed frame closes the hub", test_ssv03_malformed_closes},
    {"SSV-04", "a newer hub replaces the older", test_ssv04_newer_hub_replaces},
    {"SSV-05", "a duplicate keeps the connection", test_ssv05_duplicate_keeps},
    {"SSV-06", "stop with no hub returns", test_ssv06_stop_with_no_hub},
    {"SSV-07", "bad arguments are rejected", test_ssv07_bad_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
