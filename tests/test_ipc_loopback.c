/* Loopback tests for the IPC client over a real socket: gt_ipc_client_open
 * and the Linux socket layer (src/ipc_sock.c), against a listener the test
 * makes itself.
 *
 * Purpose: prove what tests/fake_sock.h can't, namely that the real kernel
 * behaves as the client assumes (Sensor-Pipeline.md, IPC ladder rung 2).
 * A missing hub is ENOENT, so readings buffer until a listener appears. A
 * hub that goes away is EPIPE / ECONNRESET, and it doesn't kill the test
 * process with SIGPIPE: SIGPIPE is deliberately left at its default here,
 * so a missing MSG_NOSIGNAL would end this binary on the spot. Two clients
 * can share one listener. SOCK_SEQPACKET keeps message boundaries, so a
 * 15 B packet arrives as 15 B and is rejected.
 *
 * Each test makes its own directory with mkdtemp under /tmp (Windows-mounted
 * drives in WSL2 don't reliably support socket files) and removes it after.
 * now_ms is still passed in, so nothing sleeps. Nothing blocks either: the
 * listener is SOCK_NONBLOCK and every recv uses MSG_DONTWAIT, so a message
 * that never arrives is a FAIL line, not a hang. Run under make test / asan
 * / memcheck. Implements IPL-01 through IPL-05. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.h"
#include "ipc.h"

/* One test's socket directory, the hub path inside it, and the listener
 * standing in for the hub (-1 while there is none). */
typedef struct {
    char dir[32];    /* "/tmp/gt-ipl-XXXXXX", the X's filled in by mkdtemp */
    char path[64];   /* dir + "/hub.sock" */
    int  listen_fd;
} loopback;

/* Make a fresh, empty directory and work out the socket path in it. No
 * listener yet, so IPL-02 can start with a hub that isn't there. mkdtemp
 * replaces the XXXXXX in place with a unique name and creates the
 * directory. Returns 1 on success, 0 if the directory couldn't be made. */
static int loopback_init(loopback *fixture)
{
    strcpy(fixture->dir, "/tmp/gt-ipl-XXXXXX");
    fixture->path[0] = '\0';
    fixture->listen_fd = -1;
    if (mkdtemp(fixture->dir) == NULL) {
        return 0;
    }
    snprintf(fixture->path, sizeof fixture->path, "%s/hub.sock", fixture->dir);
    return 1;
}

/* Start listening at fixture->path, as the hub would. `struct sockaddr_un`
 * is the address type for AF_UNIX sockets: a family field plus the file
 * path. bind() takes a generic `struct sockaddr *`, so the address is
 * passed through a cast; the kernel reads the family to know its real
 * type. SOCK_NONBLOCK makes accept() return -1 (EAGAIN) instead of waiting
 * when no client has connected. Returns 1 on success, 0 on failure. */
static int make_listener(loopback *fixture)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, fixture->path, strlen(fixture->path) + 1);

    int listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) {
        return 0;
    }
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(listen_fd, 4) != 0) {
        close(listen_fd);
        return 0;
    }
    fixture->listen_fd = listen_fd;
    return 1;
}

/* Receive one packet on an accepted connection, the way the hub does: into
 * a GT_MSG_SIZE + 1 buffer, so an oversized packet shows up as 17 instead
 * of being cut to 16, then gt_ipc_frame_decode. The received length goes
 * to *len_out if it isn't NULL. `ssize_t` is the signed version of size_t:
 * recv() returns a byte count, or -1 on error. MSG_DONTWAIT makes an empty
 * socket return -1 (EAGAIN) at once, which decodes as GT_ERR_IO. */
static gt_status recv_frame(int conn_fd, gt_msg *message_out, long *len_out)
{
    uint8_t buf[GT_MSG_SIZE + 1];
    ssize_t len = recv(conn_fd, buf, sizeof buf, MSG_DONTWAIT);
    if (len_out != NULL) {
        *len_out = (long)len;
    }
    return gt_ipc_frame_decode(buf, (long)len, message_out);
}

/* Close the listener and remove the socket file and the directory, so a
 * run leaves nothing behind in /tmp. Safe on a fixture with no listener. */
static void loopback_cleanup(loopback *fixture)
{
    if (fixture->listen_fd >= 0) {
        close(fixture->listen_fd);
        fixture->listen_fd = -1;
    }
    if (fixture->path[0] != '\0') {
        unlink(fixture->path);
    }
    rmdir(fixture->dir);
}

/* A DATA reading from `sensor_id`; seq is 0 because only the hub stamps it. */
static gt_msg reading_from(uint8_t sensor_id, uint32_t counter, uint32_t raw)
{
    gt_msg message = {
        .type = GT_MSG_DATA, .sensor_id = sensor_id,
        .counter = counter, .seq = 0, .value = raw,
    };
    return message;
}

/* ---- IPL-01: the listener gets 16 B that decode back ----
 * One reading through gt_ipc_client_open and the real socket layer. The
 * hub side sees exactly one 16 B packet, and it decodes to the same
 * message. A clean first connect reports no event. */
static void test_ipl01_listener_gets_one_frame(void)
{
    loopback fixture;
    int ready = loopback_init(&fixture) && make_listener(&fixture);
    CHECK(ready);
    if (!ready) {
        loopback_cleanup(&fixture);
        return;
    }

    gt_ipc_client *client = gt_ipc_client_open(fixture.path);
    CHECK(client != NULL);
    gt_msg reading = reading_from(GT_SENSOR_LIGHT, 1, 1234);
    unsigned events = 0xFFu;
    CHECK_EQ(gt_ipc_client_send(client, &reading, 0, &events), GT_OK);
    CHECK_EQ(events, 0);

    int conn_fd = accept(fixture.listen_fd, NULL, NULL);
    CHECK(conn_fd >= 0);
    gt_msg received;
    memset(&received, 0, sizeof received);
    long len = 0;
    CHECK_EQ(recv_frame(conn_fd, &received, &len), GT_OK);
    CHECK_EQ(len, GT_MSG_SIZE);
    CHECK_EQ(received.type, reading.type);
    CHECK_EQ(received.sensor_id, reading.sensor_id);
    CHECK_EQ(received.counter, reading.counter);
    CHECK_EQ(received.seq, reading.seq);
    CHECK_EQ(received.value, reading.value);

    if (conn_fd >= 0) {
        close(conn_fd);
    }
    gt_ipc_client_close(client);
    loopback_cleanup(&fixture);
}

/* ---- IPL-02: no listener: buffered, then drained in order ----
 * The hub isn't up, so hub.sock doesn't exist and connect fails with
 * ENOENT. Readings 1 and 2 are still GT_OK (buffered) with no event. Once
 * the listener appears, the send at 100 ms (the first retry) reconnects,
 * reports RECONNECT, and the hub receives 1, 2, 3 in order and nothing
 * more. */
static void test_ipl02_no_listener_then_drained(void)
{
    loopback fixture;
    int ready = loopback_init(&fixture);
    CHECK(ready);
    if (!ready) {
        loopback_cleanup(&fixture);
        return;
    }

    gt_ipc_client *client = gt_ipc_client_open(fixture.path);
    CHECK(client != NULL);
    gt_msg readings[] = {
        reading_from(GT_SENSOR_LIGHT, 1, 100),
        reading_from(GT_SENSOR_LIGHT, 2, 200),
        reading_from(GT_SENSOR_LIGHT, 3, 300),
    };
    unsigned events = 0xFFu;
    CHECK_EQ(gt_ipc_client_send(client, &readings[0], 0, &events), GT_OK);
    CHECK_EQ(events, 0);
    CHECK_EQ(gt_ipc_client_send(client, &readings[1], 50, &events), GT_OK);
    CHECK_EQ(events, 0);

    CHECK(make_listener(&fixture));
    CHECK_EQ(gt_ipc_client_send(client, &readings[2], 100, &events), GT_OK);
    CHECK_EQ(events, GT_IPC_EV_RECONNECT);

    int conn_fd = accept(fixture.listen_fd, NULL, NULL);
    CHECK(conn_fd >= 0);
    /* Invariant: packets 0..reading_index-1 matched readings[0..reading_index-1]. */
    for (size_t reading_index = 0; reading_index < 3; reading_index++) {
        gt_msg received;
        memset(&received, 0, sizeof received);
        CHECK_EQ(recv_frame(conn_fd, &received, NULL), GT_OK);
        CHECK_EQ(received.counter, readings[reading_index].counter);
        CHECK_EQ(received.value, readings[reading_index].value);
    }
    gt_msg extra;
    CHECK_EQ(recv_frame(conn_fd, &extra, NULL), GT_ERR_IO);   /* nothing more */

    if (conn_fd >= 0) {
        close(conn_fd);
    }
    gt_ipc_client_close(client);
    loopback_cleanup(&fixture);
}

/* ---- IPL-03: hub closes: DISCONNECT, no SIGPIPE ----
 * Reading 1 arrives. Then the hub dies: both its connection and its
 * listener close. Sending reading 2 meets EPIPE / ECONNRESET. With
 * MSG_NOSIGNAL that is just an error code; without it the kernel would
 * raise SIGPIPE and this process would die here. The client reports
 * DISCONNECT and keeps reading 2. The retry at 110 ms finds no listener
 * (ECONNREFUSED) and quietly stays disconnected. */
static void test_ipl03_hub_closes_no_sigpipe(void)
{
    loopback fixture;
    int ready = loopback_init(&fixture) && make_listener(&fixture);
    CHECK(ready);
    if (!ready) {
        loopback_cleanup(&fixture);
        return;
    }

    gt_ipc_client *client = gt_ipc_client_open(fixture.path);
    CHECK(client != NULL);
    gt_msg first = reading_from(GT_SENSOR_LIGHT, 1, 100);
    gt_msg second = reading_from(GT_SENSOR_LIGHT, 2, 200);
    CHECK_EQ(gt_ipc_client_send(client, &first, 0, NULL), GT_OK);

    int conn_fd = accept(fixture.listen_fd, NULL, NULL);
    CHECK(conn_fd >= 0);
    gt_msg received;
    CHECK_EQ(recv_frame(conn_fd, &received, NULL), GT_OK);

    /* The hub dies. */
    if (conn_fd >= 0) {
        close(conn_fd);
    }
    close(fixture.listen_fd);
    fixture.listen_fd = -1;

    unsigned events = 0;
    CHECK_EQ(gt_ipc_client_send(client, &second, 10, &events), GT_OK);
    CHECK_EQ(events, GT_IPC_EV_DISCONNECT);   /* still alive: no SIGPIPE */

    gt_ipc_stats stats;
    CHECK_EQ(gt_ipc_client_stats(client, &stats), GT_OK);
    CHECK_EQ(stats.queued, 1);
    CHECK_EQ(stats.sent, 1);

    CHECK_EQ(gt_ipc_client_pump(client, 110, &events), GT_OK);
    CHECK_EQ(events, 0);

    gt_ipc_client_close(client);
    loopback_cleanup(&fixture);
}

/* ---- IPL-04: light + soil share one listener, in order ----
 * Both sensor processes connect to the same hub.sock, as on the Pi. Their
 * sends are interleaved (light 1, soil 1, light 2, soil 2, light 3). Each
 * accepted connection must carry exactly one sensor's readings, with
 * counters 1, 2, 3... and no skips: light 3 readings, soil 2. */
static void test_ipl04_two_sensors_one_listener(void)
{
    loopback fixture;
    int ready = loopback_init(&fixture) && make_listener(&fixture);
    CHECK(ready);
    if (!ready) {
        loopback_cleanup(&fixture);
        return;
    }

    gt_ipc_client *light = gt_ipc_client_open(fixture.path);
    gt_ipc_client *soil = gt_ipc_client_open(fixture.path);
    CHECK(light != NULL);
    CHECK(soil != NULL);
    gt_msg light_1 = reading_from(GT_SENSOR_LIGHT, 1, 101);
    gt_msg soil_1  = reading_from(GT_SENSOR_SOIL, 1, 501);
    gt_msg light_2 = reading_from(GT_SENSOR_LIGHT, 2, 102);
    gt_msg soil_2  = reading_from(GT_SENSOR_SOIL, 2, 502);
    gt_msg light_3 = reading_from(GT_SENSOR_LIGHT, 3, 103);
    CHECK_EQ(gt_ipc_client_send(light, &light_1, 0, NULL), GT_OK);
    CHECK_EQ(gt_ipc_client_send(soil, &soil_1, 0, NULL), GT_OK);
    CHECK_EQ(gt_ipc_client_send(light, &light_2, 1000, NULL), GT_OK);
    CHECK_EQ(gt_ipc_client_send(soil, &soil_2, 1000, NULL), GT_OK);
    CHECK_EQ(gt_ipc_client_send(light, &light_3, 2000, NULL), GT_OK);

    int conn_fds[2];
    conn_fds[0] = accept(fixture.listen_fd, NULL, NULL);
    conn_fds[1] = accept(fixture.listen_fd, NULL, NULL);
    CHECK(conn_fds[0] >= 0);
    CHECK(conn_fds[1] >= 0);

    uint32_t light_received = 0;
    uint32_t soil_received = 0;
    for (size_t conn_index = 0; conn_index < 2; conn_index++) {
        uint8_t stream_sensor = 0;
        uint32_t expected_counter = 1;
        gt_msg received;
        /* Invariant: every packet so far on this connection was from
         * stream_sensor, with counters 1..expected_counter-1 in order. */
        while (recv_frame(conn_fds[conn_index], &received, NULL) == GT_OK) {
            if (expected_counter == 1) {
                stream_sensor = received.sensor_id;
            }
            CHECK_EQ(received.sensor_id, stream_sensor);
            CHECK_EQ(received.counter, expected_counter);
            expected_counter++;
        }
        if (stream_sensor == GT_SENSOR_LIGHT) {
            light_received = expected_counter - 1;
        } else if (stream_sensor == GT_SENSOR_SOIL) {
            soil_received = expected_counter - 1;
        }
        if (conn_fds[conn_index] >= 0) {
            close(conn_fds[conn_index]);
        }
    }
    CHECK_EQ(light_received, 3);
    CHECK_EQ(soil_received, 2);

    gt_ipc_client_close(light);
    gt_ipc_client_close(soil);
    loopback_cleanup(&fixture);
}

/* ---- IPL-05: a 15 B packet arrives as 15 B, rejected ----
 * A raw socket (not the client) sends a truncated 15 B packet, then a
 * valid 16 B one. With SOCK_SEQPACKET the hub's first recv returns exactly
 * 15, which gt_ipc_frame_decode rejects as GT_ERR_CORRUPT, leaving the
 * message untouched. The second recv returns exactly 16 and decodes: the
 * kernel never merged the two packets or split one. A byte stream
 * (SOCK_STREAM) could have returned all 31 bytes at once. */
static void test_ipl05_short_packet_keeps_boundary(void)
{
    loopback fixture;
    int ready = loopback_init(&fixture) && make_listener(&fixture);
    CHECK(ready);
    if (!ready) {
        loopback_cleanup(&fixture);
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, fixture.path, strlen(fixture.path) + 1);
    int raw_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    CHECK(raw_fd >= 0);
    CHECK_EQ(connect(raw_fd, (struct sockaddr *)&addr, sizeof addr), 0);

    gt_msg valid = reading_from(GT_SENSOR_LIGHT, 7, 700);
    uint8_t frame[GT_MSG_SIZE];
    CHECK_EQ(gt_msg_encode(&valid, frame), GT_OK);
    CHECK_EQ(send(raw_fd, frame, GT_MSG_SIZE - 1, MSG_NOSIGNAL), GT_MSG_SIZE - 1);
    CHECK_EQ(send(raw_fd, frame, GT_MSG_SIZE, MSG_NOSIGNAL), GT_MSG_SIZE);

    int conn_fd = accept(fixture.listen_fd, NULL, NULL);
    CHECK(conn_fd >= 0);
    gt_msg received;
    memset(&received, 0xAB, sizeof received);
    gt_msg untouched;   /* memcpy, not `=`: copies the padding bytes too */
    memcpy(&untouched, &received, sizeof untouched);
    long len = 0;
    CHECK_EQ(recv_frame(conn_fd, &received, &len), GT_ERR_CORRUPT);
    CHECK_EQ(len, GT_MSG_SIZE - 1);
    CHECK(memcmp(&received, &untouched, sizeof received) == 0);

    CHECK_EQ(recv_frame(conn_fd, &received, &len), GT_OK);
    CHECK_EQ(len, GT_MSG_SIZE);
    CHECK_EQ(received.counter, valid.counter);
    CHECK_EQ(received.value, valid.value);

    if (conn_fd >= 0) {
        close(conn_fd);
    }
    if (raw_fd >= 0) {
        close(raw_fd);
    }
    loopback_cleanup(&fixture);
}

static const test_case_t tests[] = {
    {"IPL-01", "the listener gets 16 B that decode back", test_ipl01_listener_gets_one_frame},
    {"IPL-02", "no listener: buffered, then drained in order", test_ipl02_no_listener_then_drained},
    {"IPL-03", "hub closes: DISCONNECT, no SIGPIPE", test_ipl03_hub_closes_no_sigpipe},
    {"IPL-04", "light + soil share one listener, in order", test_ipl04_two_sensors_one_listener},
    {"IPL-05", "a 15 B packet arrives as 15 B, rejected", test_ipl05_short_packet_keeps_boundary},
};

int main(void)
{
    return RUN_TESTS(tests);
}
