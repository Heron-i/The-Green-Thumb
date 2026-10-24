/* Unit tests for losing and regaining the hub in the IPC client
 * (src/ipc_client.c): connect retries in gt_ipc_client_send and
 * gt_ipc_client_pump.
 *
 * Purpose: prove the DESIGN.md Sensor -> hub row's reconnect rule. Retries
 * wait 100, 200, 400, 800 and 1600 ms, then 2000 ms each, never earlier,
 * and a successful connect resets the wait to 100 ms. A lost hub is
 * reported once (GT_IPC_EV_DISCONNECT) and regaining it once
 * (GT_IPC_EV_RECONNECT). No reading is dropped meanwhile: the buffered
 * frames go out oldest first once the hub is back. Time is passed in as
 * now_ms, so these run with no clock and no sleeps, against
 * tests/fake_sock.h. Run under make test / asan / memcheck. Implements
 * ICR-01 through ICR-08. Sending to a reachable hub is test_ipc_send.c; a
 * full ring is test_ipc_overflow.c.
 *
 * What these tests cannot prove: that a real kernel reports a dead hub as
 * ENOENT / ECONNREFUSED / EPIPE. That's test_ipc_loopback.c. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_sock.h"
#include "ipc.h"
#include "ipc_sock.h"

/* Reset `fake` and open a client on it. */
static gt_ipc_client *open_on(fake_sock *fake)
{
    return gt_ipc_client_open_ops(fake_sock_ops(fake), fake);
}

/* A light DATA reading; seq is 0 because only the hub stamps it. */
static gt_msg light_reading(uint32_t counter, uint32_t raw)
{
    gt_msg message = {
        .type = GT_MSG_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .counter = counter, .seq = 0, .value = raw,
    };
    return message;
}

/* Whether the fake's sent frame number `sent_index` is exactly
 * gt_msg_encode(message). */
static int sent_frame_is(const fake_sock *fake, size_t sent_index, const gt_msg *message)
{
    uint8_t expected[GT_MSG_SIZE];
    if (gt_msg_encode(message, expected) != GT_OK || sent_index >= fake->n_sent) {
        return 0;
    }
    return memcmp(fake->sent[sent_index], expected, GT_MSG_SIZE) == 0;
}

/* ---- ICR-01: failed connect buffers, no event ----
 * The hub isn't up yet. The reading is still accepted (GT_OK: buffered),
 * nothing is sent, and there's no event: never having connected is not a
 * lost connection. */
static void test_icr01_failed_connect_buffers(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake.fail_connects = 1;
    gt_msg reading = light_reading(1, 100);
    unsigned events = 0xFFu;

    CHECK_EQ(gt_ipc_client_send(client, &reading, 0, &events), GT_OK);
    CHECK_EQ(fake.connect_calls, 1);
    CHECK_EQ(fake.send_calls, 0);
    CHECK_EQ(events, 0);

    gt_ipc_client_close(client);
}

/* One pump at `now_ms`, after which the fake should have seen
 * `connect_calls` attempts in total. */
typedef struct {
    uint64_t now_ms;
    int      connect_calls;
} attempt_check;

/* Pump at each row's time and check the running attempt count. */
static void check_attempts(gt_ipc_client *client, const fake_sock *fake,
                           const attempt_check *rows, size_t row_count)
{
    /* Invariant: after row i, the fake has seen rows[i].connect_calls attempts. */
    for (size_t row_index = 0; row_index < row_count; row_index++) {
        gt_ipc_client_pump(client, rows[row_index].now_ms, NULL);
        CHECK_EQ(fake->connect_calls, rows[row_index].connect_calls);
    }
}

/* ---- ICR-02: retries at 0/100/300/700/1500/3100 ms ----
 * The hub stays down. Each wait doubles (100, 200, 400, 800, 1600), so
 * attempts land at 0, 100, 300, 700, 1500 and 3100 ms. A pump 1 ms before
 * each one must not attempt. */
static void test_icr02_backoff_schedule(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake.fail_connects = 100;
    const attempt_check rows[] = {
        { 0, 1 },
        { 99, 1 },   { 100, 2 },
        { 299, 2 },  { 300, 3 },
        { 699, 3 },  { 700, 4 },
        { 1499, 4 }, { 1500, 5 },
        { 3099, 5 }, { 3100, 6 },
    };

    check_attempts(client, &fake, rows, sizeof rows / sizeof rows[0]);

    gt_ipc_client_close(client);
}

/* ---- ICR-03: the wait caps at 2000 ms ----
 * Continuing ICR-02's schedule: after the 1600 ms wait, doubling would
 * give 3200, but the wait stays at GT_IPC_BACKOFF_MAX_MS, so attempts land
 * at 5100 and 7100 ms. */
static void test_icr03_backoff_caps(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake.fail_connects = 100;
    const attempt_check rows[] = {
        { 0, 1 }, { 100, 2 }, { 300, 3 }, { 700, 4 }, { 1500, 5 }, { 3100, 6 },
        { 5099, 6 }, { 5100, 7 },
        { 7099, 7 }, { 7100, 8 },
    };

    check_attempts(client, &fake, rows, sizeof rows / sizeof rows[0]);

    gt_ipc_client_close(client);
}

/* ---- ICR-04: a connect resets the wait to 100 ms ----
 * Three failures (at 0, 100, 300) grow the wait to 800 ms; the attempt at
 * 700 succeeds. The hub is then lost on a send at 1000, and the next
 * attempt comes 100 ms later at 1100, not 800 ms later. */
static void test_icr04_connect_resets_backoff(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake.fail_connects = 3;
    const attempt_check before_loss[] = {
        { 0, 1 }, { 100, 2 }, { 300, 3 }, { 700, 4 },
    };
    check_attempts(client, &fake, before_loss, sizeof before_loss / sizeof before_loss[0]);
    CHECK_EQ(fake.connected, 1);

    fake_sock_push_send_result(&fake, GT_ERR_IO);
    gt_msg reading = light_reading(1, 100);
    CHECK_EQ(gt_ipc_client_send(client, &reading, 1000, NULL), GT_OK);
    CHECK_EQ(fake.connected, 0);

    const attempt_check after_loss[] = {
        { 1099, 4 }, { 1100, 5 },
    };
    check_attempts(client, &fake, after_loss, sizeof after_loss / sizeof after_loss[0]);

    gt_ipc_client_close(client);
}

/* ---- ICR-05: lost hub: DISCONNECT once, frame kept ----
 * The send fails with GT_ERR_IO (EPIPE: the hub is gone). The client
 * disconnects and reports GT_IPC_EV_DISCONNECT, but the reading is still
 * GT_OK: it stays in the ring. A second send 50 ms later, before the retry
 * is due, reports nothing more, and never sends on the dead connection.
 * The kept frame is the first to go out once the hub is back. */
static void test_icr05_lost_hub_reported_once(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake_sock_push_send_result(&fake, GT_ERR_IO);
    gt_msg first = light_reading(1, 100);
    gt_msg second = light_reading(2, 200);
    unsigned events = 0;

    CHECK_EQ(gt_ipc_client_send(client, &first, 0, &events), GT_OK);
    CHECK_EQ(events, GT_IPC_EV_DISCONNECT);
    CHECK_EQ(fake.disconnect_calls, 1);
    CHECK_EQ(fake.n_sent, 0);

    CHECK_EQ(gt_ipc_client_send(client, &second, 50, &events), GT_OK);
    CHECK_EQ(events, 0);
    CHECK_EQ(fake.disconnect_calls, 1);
    CHECK_EQ(fake.connect_calls, 1);
    CHECK_EQ(fake.send_while_disconnected, 0);

    gt_ipc_client_pump(client, 100, NULL);
    CHECK(sent_frame_is(&fake, 0, &first));

    gt_ipc_client_close(client);
}

/* ---- ICR-06: reconnect: RECONNECT, oldest first ----
 * The hub is down for the first attempt (0 ms). Readings at 0 and 50 ms
 * wait in the ring; the send at 100 ms reconnects, reports
 * GT_IPC_EV_RECONNECT, and all three go out in the order they were sent. */
static void test_icr06_reconnect_drains_oldest_first(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake.fail_connects = 1;
    gt_msg readings[] = { light_reading(1, 100), light_reading(2, 200), light_reading(3, 300) };
    unsigned events = 0;

    CHECK_EQ(gt_ipc_client_send(client, &readings[0], 0, &events), GT_OK);
    CHECK_EQ(gt_ipc_client_send(client, &readings[1], 50, &events), GT_OK);
    CHECK_EQ(fake.n_sent, 0);

    CHECK_EQ(gt_ipc_client_send(client, &readings[2], 100, &events), GT_OK);
    CHECK_EQ(events, GT_IPC_EV_RECONNECT);
    CHECK_EQ(fake.n_sent, 3);
    CHECK(sent_frame_is(&fake, 0, &readings[0]));
    CHECK(sent_frame_is(&fake, 1, &readings[1]));
    CHECK(sent_frame_is(&fake, 2, &readings[2]));

    gt_ipc_client_close(client);
}

/* ---- ICR-07: a clean first connect: no RECONNECT ----
 * The hub was up from the start, so there's nothing to recover from: the
 * light process shouldn't log ipc_reconnect on every normal startup. */
static void test_icr07_clean_first_connect_no_event(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    unsigned events = 0xFFu;

    CHECK_EQ(gt_ipc_client_pump(client, 0, &events), GT_OK);
    CHECK_EQ(fake.connect_calls, 1);
    CHECK_EQ(fake.connected, 1);
    CHECK_EQ(events, 0);

    gt_ipc_client_close(client);
}

/* ---- ICR-08: pump reconnects and drains, no new msg ----
 * The sensor has gone offline, so no new readings arrive to trigger a
 * send. The light process's per-tick pump alone must still reconnect and
 * deliver the reading buffered while the hub was down. */
static void test_icr08_pump_reconnects_and_drains(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake.fail_connects = 1;
    gt_msg reading = light_reading(1, 100);
    unsigned events = 0;

    CHECK_EQ(gt_ipc_client_send(client, &reading, 0, NULL), GT_OK);
    CHECK_EQ(fake.n_sent, 0);

    CHECK_EQ(gt_ipc_client_pump(client, 100, &events), GT_OK);
    CHECK_EQ(events, GT_IPC_EV_RECONNECT);
    CHECK_EQ(fake.n_sent, 1);
    CHECK(sent_frame_is(&fake, 0, &reading));

    CHECK_EQ(gt_ipc_client_pump(NULL, 200, NULL), GT_ERR_INVAL);

    gt_ipc_client_close(client);
}

static const test_case_t tests[] = {
    {"ICR-01", "failed connect buffers, no event", test_icr01_failed_connect_buffers},
    {"ICR-02", "retries at 0/100/300/700/1500/3100 ms", test_icr02_backoff_schedule},
    {"ICR-03", "the wait caps at 2000 ms", test_icr03_backoff_caps},
    {"ICR-04", "a connect resets the wait to 100 ms", test_icr04_connect_resets_backoff},
    {"ICR-05", "lost hub: DISCONNECT once, frame kept", test_icr05_lost_hub_reported_once},
    {"ICR-06", "reconnect: RECONNECT, oldest first", test_icr06_reconnect_drains_oldest_first},
    {"ICR-07", "a clean first connect: no RECONNECT", test_icr07_clean_first_connect_no_event},
    {"ICR-08", "pump reconnects and drains, no new msg", test_icr08_pump_reconnects_and_drains},
};

int main(void)
{
    return RUN_TESTS(tests);
}
