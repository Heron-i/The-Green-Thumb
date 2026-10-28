/* Loopback tests for the hub's rx loop over real sensor sockets
 * (src/hub_server.c): gt_hub_serve.
 *
 * Purpose: prove the loop does over a real AF_UNIX SOCK_SEQPACKET socket
 * what gt_hub_accept and the sender decide on paper (DESIGN.md Hub rows).
 * A sensor message becomes a record sent to storage; a bad frame closes
 * that sensor's connection (ipc_bad_message); a newer connection for the
 * same sensor closes the older one (sensor_duplicate_client); on stop,
 * messages already sent are still taken; and with storage down the hub
 * keeps receiving and queueing (Mechanism E).
 *
 * The loop runs in a second thread (as in test_storage_serve.c). The
 * sensor side is real sockets in a mkdtemp directory under /tmp; the
 * storage side is the sender over tests/fake_sock.h, so what reached
 * "storage" can be read back exactly. The fake and the log book are
 * touched only by the loop's thread while it runs and read only after it
 * has been joined. Waits are bounded by poll() timeouts, so a broken loop
 * is a FAIL line, not a hang. Run under make test / asan / memcheck.
 * Implements HRX-01 through HRX-06.
 *
 * What these tests cannot prove: the 3 s / 180 s timeouts on a real clock
 * (HTO proves the logic; waiting them out here would slow every run), or
 * a real storage behind the sender (HSL). Linux only, like SSV. */
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "fake_sock.h"
#include "hub.h"
#include "hub_sender.h"
#include "hub_server.h"
#include "listen_sock.h"
#include "storage_proto.h"

#define WAIT_MS 2000   /* longest any test waits for the loop */

static fake_sock fake;   /* static: with 1100 sent frames it is about 35 KB */

/* Event names the loop logged, in order. */
typedef struct {
    size_t count;
    char   events[64][32];
} event_book;

static event_book book;

static void record_log(void *log_ctx, gt_log_level level, const char *event, const char *fields)
{
    event_book *records = log_ctx;
    (void)level;
    (void)fields;
    if (records->count < 64) {
        strncpy(records->events[records->count], event, sizeof records->events[0] - 1);
        records->count++;
    }
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

/* One test's hub: directory, socket path, lock, listener, stop pipe, the
 * hub and its sender, the loop's thread and what it returned. */
typedef struct {
    char             dir[32];
    char             path[64];
    int              lock_fd;
    int              listen_fd;
    int              stop_pipe[2];
    gt_hub_serve_ctx ctx;
    pthread_t        thread;
    gt_status        result;
} hub_proc;

/* The thread's body: run the loop and keep what it returned. */
static void *serve_thread(void *arg)
{
    hub_proc *running = arg;
    running->result = gt_hub_serve(&running->ctx, running->listen_fd, running->stop_pipe[0]);
    return NULL;
}

/* The monotonic clock in ms, for creating the hub. */
static uint64_t mono_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Make the hub and its sender, bind hub.sock in a fresh directory and
 * start the loop. With storage_up the fake storage replies "empty log" on
 * connect, so records go out from seq 0; otherwise every connect fails.
 * Returns 1 on success. */
static int start_hub(hub_proc *running, int storage_up)
{
    memset(running, 0, sizeof *running);
    memset(&book, 0, sizeof book);
    running->ctx.sender = gt_hub_sender_open_ops(fake_sock_ops(&fake), &fake);
    fake.expected_len = GT_REC_SIZE;
    if (storage_up) {
        const gt_resume empty_log = { .has_last = 0, .seq = 0 };
        uint8_t reply[GT_RESUME_SIZE];
        CHECK_EQ(gt_resume_encode(&empty_log, reply), GT_OK);
        fake_sock_push_recv(&fake, GT_OK, reply, sizeof reply);
    } else {
        fake.fail_connects = 1000000;
    }
    running->ctx.hub = gt_hub_create(mono_ms());
    running->ctx.log = record_log;
    running->ctx.log_ctx = &book;
    strcpy(running->dir, "/tmp/gt-hrx-XXXXXX");
    int already_running = 0;
    if (running->ctx.sender == NULL || running->ctx.hub == NULL
        || mkdtemp(running->dir) == NULL) {
        return 0;
    }
    snprintf(running->path, sizeof running->path, "%s/hub.sock", running->dir);
    if (gt_listen_lock(running->path, &running->lock_fd, &already_running) != GT_OK
        || gt_listen_bind(running->path, GT_HUB_MAX_SENSOR_CONNS, &running->listen_fd) != GT_OK
        || pipe(running->stop_pipe) != 0) {
        return 0;
    }
    return pthread_create(&running->thread, NULL, serve_thread, running) == 0;
}

/* Stop the loop (one byte down the stop pipe), wait for its thread, and
 * check it returned GT_OK. */
static void stop_hub(hub_proc *running)
{
    const char stop_byte = 's';
    CHECK(write(running->stop_pipe[1], &stop_byte, 1) == 1);
    pthread_join(running->thread, NULL);
    CHECK_EQ(running->result, GT_OK);
}

/* Free everything start_hub made. */
static void finish(hub_proc *running)
{
    gt_hub_sender_close(running->ctx.sender);
    gt_hub_destroy(running->ctx.hub);
    close(running->stop_pipe[0]);
    close(running->stop_pipe[1]);
    gt_listen_release(running->path, running->listen_fd, running->lock_fd);
    char lock_path[80];
    snprintf(lock_path, sizeof lock_path, "%s.lock", running->path);
    unlink(lock_path);
    rmdir(running->dir);
}

/* Connect a sensor to the hub. Returns the fd, or -1. */
static int connect_sensor(const hub_proc *running)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, running->path, sizeof addr.sun_path - 1);
    int sensor_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sensor_fd >= 0 && connect(sensor_fd, (const struct sockaddr *)&addr, sizeof addr) != 0) {
        close(sensor_fd);
        sensor_fd = -1;
    }
    return sensor_fd;
}

/* Send one 16 B DATA message, as a sensor's client would. */
static void send_reading(int sensor_fd, uint8_t sensor_id, uint32_t counter, uint32_t value)
{
    const gt_msg message = { .type = GT_MSG_DATA, .sensor_id = sensor_id, .counter = counter,
                             .seq = 0, .value = value };
    uint8_t frame[GT_MSG_SIZE];
    CHECK_EQ(gt_msg_encode(&message, frame), GT_OK);
    CHECK(send(sensor_fd, frame, sizeof frame, 0) == (ssize_t)sizeof frame);
}

/* Decode the fake storage's sent frame number `sent_index`. 1 if it
 * exists and decodes (CRC included). */
static int sent_record(size_t sent_index, gt_record *record_out)
{
    return sent_index < fake.n_sent && gt_record_decode(fake.sent[sent_index], record_out) == GT_OK;
}

/* ---- HRX-01: a sensor message reaches storage as a record ----
 * A light reading, raw 1234, goes through decode, accept and the sender:
 * storage gets one DATA record, light, 1234, seq 0 (its log was empty).
 * The hub logged the sensor's connection and storage's reply. */
static void test_hrx01_message_to_record(void)
{
    hub_proc running;
    CHECK(start_hub(&running, 1));
    int sensor_fd = connect_sensor(&running);
    CHECK(sensor_fd >= 0);
    send_reading(sensor_fd, GT_SENSOR_LIGHT, 0, 1234);
    stop_hub(&running);   /* the loop takes the waiting message before returning */

    CHECK_EQ(fake.n_sent, 1);
    gt_record stored;
    CHECK(sent_record(0, &stored));
    CHECK_EQ(stored.type, GT_REC_DATA);
    CHECK_EQ(stored.sensor_id, GT_SENSOR_LIGHT);
    CHECK_EQ(stored.value, 1234);
    CHECK_EQ(stored.seq, 0);
    CHECK_EQ(logged("sensor_connected"), 1);
    CHECK_EQ(logged("seq_resume"), 1);
    close(sensor_fd);
    finish(&running);
}

/* Wait up to WAIT_MS for something to read on sensor_fd, then recv it.
 * Returns recv's result (0 means the hub closed the connection), or -2 if
 * nothing came in time. */
static long recv_waiting(int sensor_fd)
{
    uint8_t buf[GT_MSG_SIZE + 1];
    struct pollfd wait_for = { .fd = sensor_fd, .events = POLLIN, .revents = 0 };
    if (poll(&wait_for, 1, WAIT_MS) != 1) {
        return -2;
    }
    return (long)recv(sensor_fd, buf, sizeof buf, 0);
}

/* ---- HRX-02: a bad frame closes that sensor's connection ----
 * A 15 B packet is not a message: the hub logs ipc_bad_message and closes
 * the connection, so the sensor sees end-of-file and its client
 * reconnects (DESIGN.md Hub row "Malformed message"). Nothing reaches
 * storage. */
static void test_hrx02_bad_frame_closes(void)
{
    hub_proc running;
    CHECK(start_hub(&running, 1));
    int sensor_fd = connect_sensor(&running);
    const uint8_t short_frame[GT_MSG_SIZE - 1] = { GT_MSG_MAGIC };
    CHECK(send(sensor_fd, short_frame, sizeof short_frame, 0) == (ssize_t)sizeof short_frame);

    CHECK_EQ(recv_waiting(sensor_fd), 0);
    stop_hub(&running);
    CHECK_EQ(logged("ipc_bad_message"), 1);
    CHECK_EQ(fake.n_sent, 0);
    close(sensor_fd);
    finish(&running);
}

/* ---- HRX-03: a newer connection for a sensor closes the older ----
 * Light sends counter 0 on one connection, then 1 on a second (an old
 * instance not yet reaped, DESIGN.md Hub row "Two clients claim the same
 * sensor ID"). The hub keeps the newest: the first connection gets
 * end-of-file and sensor_duplicate_client is logged. All three readings,
 * including 2 sent on the new connection afterwards, reach storage. */
static void test_hrx03_newer_client_wins(void)
{
    hub_proc running;
    CHECK(start_hub(&running, 1));
    int old_fd = connect_sensor(&running);
    send_reading(old_fd, GT_SENSOR_LIGHT, 0, 10);
    int new_fd = connect_sensor(&running);
    send_reading(new_fd, GT_SENSOR_LIGHT, 1, 11);

    CHECK_EQ(recv_waiting(old_fd), 0);
    send_reading(new_fd, GT_SENSOR_LIGHT, 2, 12);
    stop_hub(&running);
    CHECK_EQ(logged("sensor_duplicate_client"), 1);
    CHECK_EQ(fake.n_sent, 3);
    gt_record last;
    CHECK(sent_record(2, &last));
    CHECK_EQ(last.value, 12);
    close(old_fd);
    close(new_fd);
    finish(&running);
}

/* ---- HRX-04: stop takes every waiting message first ----
 * Light sends three readings and soil one, and the hub is stopped at
 * once (SIGTERM). Whatever the sensors already handed to the kernel is
 * read, accepted and sent before the loop returns: four records, light's
 * in counter order. */
static void test_hrx04_stop_takes_waiting(void)
{
    hub_proc running;
    CHECK(start_hub(&running, 1));
    int light_fd = connect_sensor(&running);
    int soil_fd = connect_sensor(&running);
    send_reading(light_fd, GT_SENSOR_LIGHT, 0, 100);
    send_reading(light_fd, GT_SENSOR_LIGHT, 1, 101);
    send_reading(light_fd, GT_SENSOR_LIGHT, 2, 102);
    send_reading(soil_fd, GT_SENSOR_SOIL, 0, 900);
    stop_hub(&running);

    CHECK_EQ(fake.n_sent, 4);
    uint32_t light_values_seen = 0;
    uint32_t next_light_value = 100;
    /* Invariant: light records among sent[0..sent_index-1] were 100,
     * 101, ... in order. */
    for (size_t sent_index = 0; sent_index < fake.n_sent; sent_index++) {
        gt_record stored;
        CHECK(sent_record(sent_index, &stored));
        if (stored.sensor_id == GT_SENSOR_LIGHT) {
            CHECK_EQ(stored.value, next_light_value);
            next_light_value++;
            light_values_seen++;
        }
    }
    CHECK_EQ(light_values_seen, 3);
    CHECK_EQ(logged("sensor_seq_gap"), 0);
    close(light_fd);
    close(soil_fd);
    finish(&running);
}

/* ---- HRX-05: storage down: the hub still receives and queues ----
 * Every connect to storage fails (DESIGN.md Mechanism E: "Sensor
 * communication must continue during failures elsewhere"). The sensor's
 * readings are still received and wait in the sender's queue; nothing is
 * sent, and storage_unreachable is logged once, not on every retry. */
static void test_hrx05_storage_down_still_receives(void)
{
    hub_proc running;
    CHECK(start_hub(&running, 0));
    int sensor_fd = connect_sensor(&running);
    send_reading(sensor_fd, GT_SENSOR_LIGHT, 0, 1);
    send_reading(sensor_fd, GT_SENSOR_LIGHT, 1, 2);
    send_reading(sensor_fd, GT_SENSOR_LIGHT, 2, 3);
    const struct timespec settle = { 0, 300000000L };   /* 300 ms: a few retries */
    nanosleep(&settle, NULL);
    stop_hub(&running);

    gt_hub_send_stats stats;
    CHECK_EQ(gt_hub_sender_stats(running.ctx.sender, &stats), GT_OK);
    CHECK_EQ(stats.queued, 3);
    CHECK_EQ(fake.n_sent, 0);
    CHECK(fake.connect_calls >= 2);
    CHECK_EQ(logged("storage_unreachable"), 1);
    CHECK_EQ(logged("sensor_connected"), 1);
    close(sensor_fd);
    finish(&running);
}

/* ---- HRX-06: bad arguments are rejected ----
 * Each call returns at once, before any poll(). The fds are never used,
 * so placeholder numbers are fine. */
static void test_hrx06_bad_args(void)
{
    gt_hub_serve_ctx ctx = {
        .hub = gt_hub_create(0),
        .sender = gt_hub_sender_open_ops(fake_sock_ops(&fake), &fake),
        .log = record_log, .log_ctx = &book,
    };
    gt_hub_serve_ctx no_hub = ctx;
    no_hub.hub = NULL;
    gt_hub_serve_ctx no_sender = ctx;
    no_sender.sender = NULL;
    gt_hub_serve_ctx no_log = ctx;
    no_log.log = NULL;

    CHECK_EQ(gt_hub_serve(NULL, 3, 4), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_serve(&no_hub, 3, 4), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_serve(&no_sender, 3, 4), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_serve(&no_log, 3, 4), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_serve(&ctx, -1, 4), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_serve(&ctx, 3, -1), GT_ERR_INVAL);
    gt_hub_sender_close(ctx.sender);
    gt_hub_destroy(ctx.hub);
}

static const test_case_t tests[] = {
    {"HRX-01", "a sensor message reaches storage as a record", test_hrx01_message_to_record},
    {"HRX-02", "a bad frame closes that sensor's connection", test_hrx02_bad_frame_closes},
    {"HRX-03", "a newer connection closes the older", test_hrx03_newer_client_wins},
    {"HRX-04", "stop takes every waiting message first", test_hrx04_stop_takes_waiting},
    {"HRX-05", "storage down: hub still receives, queues", test_hrx05_storage_down_still_receives},
    {"HRX-06", "bad arguments are rejected", test_hrx06_bad_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
