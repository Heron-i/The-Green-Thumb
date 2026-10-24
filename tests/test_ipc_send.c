/* Unit tests for sending through the IPC client while the hub is reachable
 * (src/ipc_client.c): gt_ipc_client_send.
 *
 * Purpose: prove the bytes the hub receives are exactly gt_msg_encode's
 * output, in the order the sensor sent them, that a bad message is turned
 * away before it takes a ring slot, and that a full kernel buffer (EAGAIN:
 * the hub is alive but slow) keeps the frame instead of dropping it or
 * treating the hub as gone. Runs against tests/fake_sock.h. Run under make
 * test / asan / memcheck. Implements ICS-01 through ICS-06. Losing and
 * regaining the hub is test_ipc_reconnect.c; a full ring is
 * test_ipc_overflow.c.
 *
 * What these tests cannot prove: that the kernel really returns EAGAIN
 * instead of blocking. That's the non-blocking socket in src/ipc_sock.c. */
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

/* A light DATA reading, as the light process will build it: seq is 0
 * because only the hub stamps it. */
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

/* ---- ICS-01: first send connects, sends the encoding ----
 * open doesn't connect, so the first send makes the one connect attempt,
 * then sends one whole 16 B frame equal to gt_msg_encode's output. A clean
 * first connect is not a reconnect, so no event. */
static void test_ics01_first_send_connects_and_sends(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    gt_msg reading = light_reading(1, 300);
    unsigned events = 0xFFu;   /* send must clear it */

    CHECK_EQ(gt_ipc_client_send(client, &reading, 0, &events), GT_OK);
    CHECK_EQ(fake.connect_calls, 1);
    CHECK_EQ(fake.n_sent, 1);
    CHECK(sent_frame_is(&fake, 0, &reading));
    CHECK_EQ(events, 0);
    CHECK_EQ(fake.wrong_len_sends, 0);

    gt_ipc_client_close(client);
}

/* ---- ICS-02: several sends arrive in order ----
 * Three readings one second apart go out oldest first over one connection
 * (no reconnect between them). events_out may be NULL. */
static void test_ics02_sends_arrive_in_order(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    gt_msg readings[] = { light_reading(1, 100), light_reading(2, 200), light_reading(3, 300) };
    size_t reading_count = sizeof readings / sizeof readings[0];

    for (size_t reading_index = 0; reading_index < reading_count; reading_index++) {
        CHECK_EQ(gt_ipc_client_send(client, &readings[reading_index], reading_index * 1000, NULL), GT_OK);
    }
    CHECK_EQ(fake.connect_calls, 1);
    CHECK_EQ(fake.n_sent, reading_count);
    for (size_t reading_index = 0; reading_index < reading_count; reading_index++) {
        CHECK(sent_frame_is(&fake, reading_index, &readings[reading_index]));
    }

    gt_ipc_client_close(client);
}

/* ---- ICS-03: a bad message is rejected, not queued ----
 * An unknown type and a sensor_id of 0 (ALL is not valid from a sensor)
 * fail gt_msg_encode, so send returns GT_ERR_INVAL before the message takes
 * a ring slot or triggers a connect. A later good message still goes out
 * alone, which shows nothing bad was left in the ring. */
static void test_ics03_bad_message_not_queued(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    gt_msg bad_type = light_reading(1, 100);
    bad_type.type = 0x09;
    gt_msg bad_sensor = light_reading(2, 100);
    bad_sensor.sensor_id = GT_SENSOR_ALL;

    CHECK_EQ(gt_ipc_client_send(client, &bad_type, 0, NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_ipc_client_send(client, &bad_sensor, 0, NULL), GT_ERR_INVAL);
    CHECK_EQ(fake.connect_calls, 0);
    CHECK_EQ(fake.send_calls, 0);

    gt_msg good = light_reading(3, 300);
    CHECK_EQ(gt_ipc_client_send(client, &good, 1000, NULL), GT_OK);
    CHECK_EQ(fake.n_sent, 1);
    CHECK(sent_frame_is(&fake, 0, &good));

    gt_ipc_client_close(client);
}

/* ---- ICS-04: NULL arguments are rejected ----
 * GT_ERR_INVAL (a caller bug), with no connect or send. */
static void test_ics04_null_args(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    gt_msg reading = light_reading(1, 100);

    CHECK_EQ(gt_ipc_client_send(NULL, &reading, 0, NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_ipc_client_send(client, NULL, 0, NULL), GT_ERR_INVAL);
    CHECK_EQ(fake.connect_calls, 0);
    CHECK_EQ(fake.send_calls, 0);

    gt_ipc_client_close(client);
}

/* ---- ICS-05: EAGAIN keeps the frame, stays connected ----
 * The hub is alive but its socket buffer is full (the fake's GT_ERR_FULL).
 * The reading is still accepted (GT_OK: it's buffered), the hub is not
 * treated as gone (no disconnect, no event), and nothing was delivered. */
static void test_ics05_eagain_keeps_frame(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake_sock_push_send_result(&fake, GT_ERR_FULL);
    gt_msg reading = light_reading(1, 100);
    unsigned events = 0xFFu;

    CHECK_EQ(gt_ipc_client_send(client, &reading, 0, &events), GT_OK);
    CHECK_EQ(fake.send_calls, 1);
    CHECK_EQ(fake.n_sent, 0);
    CHECK_EQ(fake.disconnect_calls, 0);
    CHECK_EQ(fake.connected, 1);
    CHECK_EQ(events, 0);

    gt_ipc_client_close(client);
}

/* ---- ICS-06: next send flushes the held frame first ----
 * After an EAGAIN the buffer has room again: the held frame goes out
 * before the new one (oldest first), over the same connection. */
static void test_ics06_next_send_flushes_held_frame(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake_sock_push_send_result(&fake, GT_ERR_FULL);
    gt_msg held = light_reading(1, 100);
    gt_msg next = light_reading(2, 200);

    CHECK_EQ(gt_ipc_client_send(client, &held, 0, NULL), GT_OK);
    CHECK_EQ(fake.n_sent, 0);
    CHECK_EQ(gt_ipc_client_send(client, &next, 1000, NULL), GT_OK);
    CHECK_EQ(fake.n_sent, 2);
    CHECK(sent_frame_is(&fake, 0, &held));
    CHECK(sent_frame_is(&fake, 1, &next));
    CHECK_EQ(fake.connect_calls, 1);

    gt_ipc_client_close(client);
}

static const test_case_t tests[] = {
    {"ICS-01", "first send connects, sends the encoding", test_ics01_first_send_connects_and_sends},
    {"ICS-02", "several sends arrive in order", test_ics02_sends_arrive_in_order},
    {"ICS-03", "a bad message is rejected, not queued", test_ics03_bad_message_not_queued},
    {"ICS-04", "NULL arguments are rejected", test_ics04_null_args},
    {"ICS-05", "EAGAIN keeps the frame, stays connected", test_ics05_eagain_keeps_frame},
    {"ICS-06", "next send flushes the held frame first", test_ics06_next_send_flushes_held_frame},
};

int main(void)
{
    return RUN_TESTS(tests);
}
