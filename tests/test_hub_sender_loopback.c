/* Loopback tests for the hub's hop-2 sender against the real storage rx
 * loop (src/hub_sender.c with src/ipc_sock.c, against src/storage_server.c):
 * gt_hub_sender_open and gt_hub_sender_pump over a real socket.
 *
 * Purpose: prove the PF-03 rule end to end, with storage's own sequence
 * guard as the judge. The sender reads the real seq_resume reply (the
 * first real use of the seam's recv op) and continues storage's numbering.
 * When storage is killed (kill -9) with records still unsynced, a sender
 * that keeps running keeps its own counter, so storage sees the lost seqs
 * as a hole and writes a GAP (recovery_gap). After a clean SIGTERM restart
 * nothing was lost, so there is no GAP.
 *
 * Storage runs in a second thread over tests/fake_file.h, as in
 * test_storage_serve.c (SSV): only the socket is real. A "kill -9" stops
 * the loop, closes storage without flushing and cuts the fake's power, so
 * only synced bytes survive; a "SIGTERM" runs gt_storage_shutdown first.
 * Storage is then reopened from the same fake (recovery runs) and served
 * on the same path. The sender runs on this thread with the real
 * monotonic clock; every wait is bounded (WAIT_MS), so a broken path is a
 * FAIL line, not a hang. Each test uses its own mkdtemp directory under
 * /tmp and removes it. Run under make test / asan / memcheck.
 * Implements HSL-01 through HSL-04.
 *
 * Note: macOS has no SOCK_SEQPACKET for AF_UNIX, so these run on Linux
 * (WSL and the Pi) only, like SSV. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "fake_file.h"
#include "hub_sender.h"
#include "listen_sock.h"
#include "log_record.h"
#include "storage.h"
#include "storage_server.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TODAY_NAME    "log-20261026.bin"
#define TEST_CAPACITY 16
#define LAST_ON_DISK  5      /* every test starts with records 1..5 committed */
#define WAIT_MS       2000   /* longest any test waits for the sender */

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */
static const gt_file_ops *fs_ops;   /* kept so a restart reopens the same fake */

/* Event names storage's loop logged, in order. Written only by the loop's
 * thread while it runs, read only after it has been joined. */
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

/* One test's storage process: its directory and socket path (kept across
 * restarts), lock, listener, stop pipe, storage, the loop's thread and
 * what the loop returned. */
typedef struct {
    char        dir[32];
    char        path[64];
    int         lock_fd;
    int         listen_fd;
    int         stop_pipe[2];
    gt_storage *storage;
    pthread_t   thread;
    gt_status   result;
} storage_proc;

/* The thread's body: run storage's rx loop and keep what it returned. */
static void *serve_thread(void *arg)
{
    storage_proc *running = arg;
    running->result = gt_storage_serve(running->storage, running->listen_fd,
                                       running->stop_pipe[0], record_log, &book);
    return NULL;
}

/* Fresh fake with records 1..LAST_ON_DISK committed in today's segment,
 * and a fresh /tmp directory for the socket. Returns 1 on success. */
static int prepare(storage_proc *running)
{
    memset(running, 0, sizeof *running);
    memset(&book, 0, sizeof book);
    fs_ops = fake_fs_ops(&fs);
    uint8_t segment[GT_SEG_HEADER_SZ + LAST_ON_DISK * GT_REC_SIZE];
    const gt_seg_header header = { .created_at_us = 1000 };
    CHECK_EQ(gt_seg_header_encode(&header, segment), GT_OK);
    /* Invariant: records 1 .. seq - 1 are encoded after the header. */
    for (uint32_t seq = 1; seq <= LAST_ON_DISK; seq++) {
        const gt_record record = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
                                   .seq = seq, .timestamp_us = seq, .value = seq };
        CHECK_EQ(gt_record_encode(&record, &segment[GT_SEG_HEADER_SZ + (seq - 1) * GT_REC_SIZE]),
                 GT_OK);
    }
    CHECK_EQ(fake_fs_put(&fs, TODAY_NAME, segment, sizeof segment), GT_OK);
    strcpy(running->dir, "/tmp/gt-hsl-XXXXXX");
    if (mkdtemp(running->dir) == NULL) {
        return 0;
    }
    snprintf(running->path, sizeof running->path, "%s/storage.sock", running->dir);
    return 1;
}

/* Start storage as its process would: open it from the fake (recovery
 * runs), take the lock, bind the socket and run the loop on a thread.
 * Returns 1 on success. */
static int start_storage(storage_proc *running)
{
    gt_storage_open_args args = { .ops = fs_ops, .ctx = &fs, .now_us = TODAY_US,
                                  .pending_capacity = TEST_CAPACITY };
    int already_running = 0;
    if (gt_storage_open_ops(&args, &running->storage, NULL) != GT_OK
        || gt_listen_lock(running->path, &running->lock_fd, &already_running) != GT_OK
        || gt_listen_bind(running->path, 4, &running->listen_fd) != GT_OK
        || pipe(running->stop_pipe) != 0) {
        return 0;
    }
    return pthread_create(&running->thread, NULL, serve_thread, running) == 0;
}

/* Stop the loop (one byte down the stop pipe) and wait for its thread;
 * the loop closes the sender's connection as it returns. Then release
 * the socket and lock. Storage itself stays open for the caller. */
static void stop_serving(storage_proc *running)
{
    const char stop_byte = 's';
    CHECK(write(running->stop_pipe[1], &stop_byte, 1) == 1);
    pthread_join(running->thread, NULL);
    CHECK_EQ(running->result, GT_OK);
    close(running->stop_pipe[0]);
    close(running->stop_pipe[1]);
    gt_listen_release(running->path, running->listen_fd, running->lock_fd);
}

/* Close storage and remove the test's directory. */
static void finish(storage_proc *running)
{
    gt_storage_close(running->storage);
    char lock_path[80];
    snprintf(lock_path, sizeof lock_path, "%s.lock", running->path);
    unlink(lock_path);
    rmdir(running->dir);
}

/* The monotonic clock in ms, as the hub reads it. */
static uint64_t mono_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Pump the sender every millisecond until it has sent `target_sent`
 * records in total, or WAIT_MS passes. Every event seen is ORed into
 * *events_seen. Returns 1 if the target was reached. */
static int pump_until_sent(gt_hub_sender *sender, uint32_t target_sent, unsigned *events_seen)
{
    const struct timespec one_ms = { 0, 1000000L };
    uint64_t give_up_at = mono_ms() + WAIT_MS;
    gt_hub_send_stats stats;
    /* Invariant: *events_seen holds every event from the pumps so far. */
    do {
        unsigned events = 0;
        gt_hub_sender_pump(sender, mono_ms(), &events);
        *events_seen |= events;
        gt_hub_sender_stats(sender, &stats);
        if (stats.sent >= target_sent) {
            return 1;
        }
        nanosleep(&one_ms, NULL);
    } while (mono_ms() < give_up_at);
    return 0;
}

/* Queue one light DATA record (value = its place in the test). */
static void push_light(gt_hub_sender *sender, uint32_t value)
{
    const gt_record record = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT, .seq = 0,
                               .timestamp_us = TODAY_US + value, .value = value };
    CHECK_EQ(gt_hub_sender_push(sender, &record, NULL), GT_OK);
}

/* ---- HSL-01: records reach storage, continuing its seq ----
 * Storage holds 1..5, so its real seq_resume says 5 and three records go
 * out as 6, 7, 8. Storage's own guard accepts all three (no GAP, no
 * rejection): the reply was read and decoded over a real socket. */
static void test_hsl01_records_reach_storage(void)
{
    storage_proc running;
    CHECK(prepare(&running));
    CHECK(start_storage(&running));
    gt_hub_sender *sender = gt_hub_sender_open(running.path);
    CHECK(sender != NULL);
    push_light(sender, 1);
    push_light(sender, 2);
    push_light(sender, 3);
    unsigned events = 0;

    CHECK(pump_until_sent(sender, 3, &events));
    CHECK(events & GT_HUB_SENDER_EV_RESUMED);
    gt_hub_send_stats send_stats;
    gt_hub_sender_stats(sender, &send_stats);
    CHECK_EQ(send_stats.first_sent_seq, LAST_ON_DISK + 1);
    stop_serving(&running);   /* frames already in the socket are taken first */

    gt_storage_stats stored;
    CHECK_EQ(gt_storage_read_stats(running.storage, &stored), GT_OK);
    CHECK_EQ(stored.last_accepted_seq, LAST_ON_DISK + 3);
    CHECK_EQ(stored.pending, 3);
    gt_hub_sender_close(sender);
    finish(&running);
}

/* How many logged events were called `event`. */
static size_t logged(const char *event)
{
    size_t matches = 0;
    /* Invariant: matches counts book.events[0..log_index-1] equal to event. */
    for (size_t log_index = 0; log_index < book.count; log_index++) {
        if (strcmp(book.events[log_index], event) == 0) {
            matches++;
        }
    }
    return matches;
}

/* kill -9: stop the loop and close storage without flushing, then cut the
 * fake's power, so records accepted but not yet synced are gone. */
static void kill_storage(storage_proc *running)
{
    stop_serving(running);
    gt_storage_close(running->storage);
    running->storage = NULL;
    fake_fs_power_cut(&fs);
}

/* ---- HSL-02: kill -9 loses 6-7; storage writes their GAP ----
 * 6 and 7 reach storage but are still pending when it is killed. On
 * restart recovery finds 5 as the last seq, so the reply is 5. The sender
 * kept running: it notices the closed connection (storage_unreachable),
 * reconnects, and keeps its own counter, so its next record goes out as
 * 8. Storage's guard sees 6-7 missing and puts a GAP before 8
 * (recovery_gap). A sender that went back to reply + 1 would reuse 6 and
 * hide the loss. */
static void test_hsl02_kill_leaves_gap(void)
{
    storage_proc running;
    CHECK(prepare(&running));
    CHECK(start_storage(&running));
    gt_hub_sender *sender = gt_hub_sender_open(running.path);
    push_light(sender, 1);
    push_light(sender, 2);
    unsigned events = 0;
    CHECK(pump_until_sent(sender, 2, &events));

    kill_storage(&running);
    CHECK(start_storage(&running));
    push_light(sender, 3);
    events = 0;
    CHECK(pump_until_sent(sender, 3, &events));
    CHECK(events & GT_HUB_SENDER_EV_UNREACHABLE);
    CHECK(events & GT_HUB_SENDER_EV_RECONNECTED);
    stop_serving(&running);

    gt_storage_stats stored;
    CHECK_EQ(gt_storage_read_stats(running.storage, &stored), GT_OK);
    CHECK_EQ(stored.last_accepted_seq, LAST_ON_DISK + 3);
    CHECK_EQ(stored.pending, 2);   /* the GAP for 6-7, then 8 */
    CHECK_EQ(logged("recovery_gap"), 1);
    gt_hub_sender_close(sender);
    finish(&running);
}

/* SIGTERM: stop the loop, then storage's clean shutdown (final flush and
 * the clean-shutdown marker) before closing, so nothing accepted is lost. */
static void stop_storage_cleanly(storage_proc *running)
{
    stop_serving(running);
    unsigned shutdown_events = 0;
    CHECK_EQ(gt_storage_shutdown(running->storage, &shutdown_events), GT_OK);
    gt_storage_close(running->storage);
    running->storage = NULL;
}

/* ---- HSL-03: a SIGTERM restart loses nothing: no GAP ----
 * Same as HSL-02, but storage stops cleanly: 6 and 7 are flushed, so the
 * reply after the restart is 7. Own next (8) and reply + 1 (8) agree, the
 * next record goes out as 8, and storage writes no GAP. */
static void test_hsl03_sigterm_no_gap(void)
{
    storage_proc running;
    CHECK(prepare(&running));
    CHECK(start_storage(&running));
    gt_hub_sender *sender = gt_hub_sender_open(running.path);
    push_light(sender, 1);
    push_light(sender, 2);
    unsigned events = 0;
    CHECK(pump_until_sent(sender, 2, &events));

    stop_storage_cleanly(&running);
    fake_fs_power_cut(&fs);   /* a reboot keeps only what was synced: all of it */
    CHECK(start_storage(&running));
    push_light(sender, 3);
    CHECK(pump_until_sent(sender, 3, &events));
    stop_serving(&running);

    gt_storage_stats stored;
    CHECK_EQ(gt_storage_read_stats(running.storage, &stored), GT_OK);
    CHECK_EQ(stored.last_accepted_seq, LAST_ON_DISK + 3);
    CHECK_EQ(stored.pending, 1);
    CHECK_EQ(logged("recovery_gap"), 0);
    gt_hub_sender_close(sender);
    finish(&running);
}

/* ---- HSL-04: open by path: bad paths NULL, no storage is OK ----
 * NULL, "" and a 108-character path (no room for sun_path's closing
 * '\0') are refused. A good path with nothing listening still opens:
 * storage not being up yet is not an error (Mechanism E). Its first pump
 * finds nothing there and reports storage_unreachable, without crashing
 * or blocking. */
static void test_hsl04_open_by_path(void)
{
    char too_long[109];
    memset(too_long, 'a', 108);
    too_long[108] = '\0';
    CHECK(gt_hub_sender_open(NULL) == NULL);
    CHECK(gt_hub_sender_open("") == NULL);
    CHECK(gt_hub_sender_open(too_long) == NULL);

    gt_hub_sender *sender = gt_hub_sender_open("/tmp/gt-hsl-no-such-dir/storage.sock");
    CHECK(sender != NULL);
    unsigned events = 0;
    CHECK_EQ(gt_hub_sender_pump(sender, mono_ms(), &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_UNREACHABLE);
    gt_hub_sender_close(sender);
}

static const test_case_t tests[] = {
    {"HSL-01", "records reach storage, continuing its seq", test_hsl01_records_reach_storage},
    {"HSL-02", "kill -9 loses 6-7; storage writes their GAP", test_hsl02_kill_leaves_gap},
    {"HSL-03", "a SIGTERM restart loses nothing: no GAP", test_hsl03_sigterm_no_gap},
    {"HSL-04", "open by path: bad paths NULL, no storage OK", test_hsl04_open_by_path},
};

int main(void)
{
    return RUN_TESTS(tests);
}
