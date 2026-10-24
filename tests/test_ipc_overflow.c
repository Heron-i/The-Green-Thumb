/* Unit tests for a full ring in the IPC client (src/ipc_client.c): the
 * overflow path of gt_ipc_client_send, the GAP message, and
 * gt_ipc_client_stats.
 *
 * Purpose: prove the DESIGN.md Sensor -> hub row's overflow rule, "refuse
 * and count, then send a gap record on reconnect". With the hub down, the
 * ring holds GT_IPC_RING_SLOTS readings; the next one is refused
 * (GT_ERR_FULL) and counted, and GT_IPC_EV_OVERFLOW fires once for the
 * whole run of refusals. Once there's room for two frames, one GAP
 * {first refused counter, refused count} is queued just before the next
 * reading, so the hub sees old data, then the GAP, then new data, and
 * every counter is either delivered or covered by the GAP. Time is passed
 * in as now_ms and the hub is tests/fake_sock.h, so nothing sleeps. Run
 * under make test / asan / memcheck. Implements IOV-01 through IOV-05.
 * Reconnecting is test_ipc_reconnect.c; sending to a reachable hub is
 * test_ipc_send.c. */
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

/* Send `reading_count` light readings with counters first_counter,
 * first_counter + 1, ... (raw = counter + 1000, so each is distinct), all
 * at `now_ms`. Returns how many came back GT_OK. */
static size_t send_readings(gt_ipc_client *client, uint32_t first_counter,
                            size_t reading_count, uint64_t now_ms)
{
    size_t accepted = 0;
    /* Invariant: `accepted` readings out of the first reading_index returned GT_OK. */
    for (size_t reading_index = 0; reading_index < reading_count; reading_index++) {
        uint32_t counter = first_counter + (uint32_t)reading_index;
        gt_msg reading = light_reading(counter, counter + 1000);
        if (gt_ipc_client_send(client, &reading, now_ms, NULL) == GT_OK) {
            accepted++;
        }
    }
    return accepted;
}

/* ---- IOV-01: 64 buffered OK, the 65th is FULL + OVERFLOW ----
 * The hub is down for the whole test (the only attempt is at 0 ms and the
 * retry isn't due before 100). The first GT_IPC_RING_SLOTS readings are
 * buffered (GT_OK). The next one has no slot: it is refused with
 * GT_ERR_FULL and reported as GT_IPC_EV_OVERFLOW, and nothing was sent. */
static void test_iov01_ring_full_refuses(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake.fail_connects = 100;

    CHECK_EQ(send_readings(client, 1, GT_IPC_RING_SLOTS, 0), GT_IPC_RING_SLOTS);

    gt_msg overflowing = light_reading(GT_IPC_RING_SLOTS + 1, 5000);
    unsigned events = 0;
    CHECK_EQ(gt_ipc_client_send(client, &overflowing, 0, &events), GT_ERR_FULL);
    CHECK_EQ(events, GT_IPC_EV_OVERFLOW);
    CHECK_EQ(fake.n_sent, 0);

    gt_ipc_client_close(client);
}

/* ---- IOV-02: OVERFLOW once per run, every refusal counted ----
 * Three readings in a row hit the full ring. Each is GT_ERR_FULL, but only
 * the first reports GT_IPC_EV_OVERFLOW, so the light process logs one
 * ipc_overflow per outage rather than one per second. stats still counts
 * all three as refused, and the ring still holds exactly the 64 it had. */
static void test_iov02_overflow_once_per_run(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake.fail_connects = 100;
    CHECK_EQ(send_readings(client, 1, GT_IPC_RING_SLOTS, 0), GT_IPC_RING_SLOTS);

    const unsigned expected_events[] = { GT_IPC_EV_OVERFLOW, 0, 0 };
    /* Invariant: refusals 0..refusal_index-1 each returned GT_ERR_FULL with their expected event. */
    for (size_t refusal_index = 0; refusal_index < 3; refusal_index++) {
        gt_msg refused = light_reading(GT_IPC_RING_SLOTS + 1 + (uint32_t)refusal_index, 5000);
        unsigned events = 0xFFu;
        CHECK_EQ(gt_ipc_client_send(client, &refused, 0, &events), GT_ERR_FULL);
        CHECK_EQ(events, expected_events[refusal_index]);
    }

    gt_ipc_stats stats;
    CHECK_EQ(gt_ipc_client_stats(client, &stats), GT_OK);
    CHECK_EQ(stats.refused, 3);
    CHECK_EQ(stats.queued, GT_IPC_RING_SLOTS);
    CHECK_EQ(stats.sent, 0);

    gt_ipc_client_close(client);
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

/* ---- IOV-03: drain order is 64 data, GAP, new reading ----
 * The hub is down at 0 ms: counters 1-64 fill the ring and 65-66 are
 * refused. At 100 ms the hub is back and counter 67 is sent. The 64
 * buffered readings go out first, then one GAP saying "2 readings from
 * counter 65 are missing", then 67. Only RECONNECT is reported: the
 * overflow was already reported when it started. */
static void test_iov03_drain_order_data_gap_new(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake.fail_connects = 1;
    CHECK_EQ(send_readings(client, 1, GT_IPC_RING_SLOTS, 0), GT_IPC_RING_SLOTS);
    CHECK_EQ(send_readings(client, GT_IPC_RING_SLOTS + 1, 2, 0), 0);

    gt_msg after = light_reading(GT_IPC_RING_SLOTS + 3, 7000);
    unsigned events = 0;
    CHECK_EQ(gt_ipc_client_send(client, &after, 100, &events), GT_OK);
    CHECK_EQ(events, GT_IPC_EV_RECONNECT);
    CHECK_EQ(fake.n_sent, GT_IPC_RING_SLOTS + 2);

    /* Invariant: frames 0..sent_index-1 were the buffered readings 1..sent_index. */
    for (size_t sent_index = 0; sent_index < GT_IPC_RING_SLOTS; sent_index++) {
        uint32_t counter = (uint32_t)sent_index + 1;
        gt_msg buffered = light_reading(counter, counter + 1000);
        CHECK(sent_frame_is(&fake, sent_index, &buffered));
    }
    gt_msg gap = {
        .type = GT_MSG_GAP, .sensor_id = GT_SENSOR_LIGHT,
        .counter = GT_IPC_RING_SLOTS + 1, .seq = 0, .value = 2,
    };
    CHECK(sent_frame_is(&fake, GT_IPC_RING_SLOTS, &gap));
    CHECK(sent_frame_is(&fake, GT_IPC_RING_SLOTS + 1, &after));

    gt_ipc_stats stats;
    CHECK_EQ(gt_ipc_client_stats(client, &stats), GT_OK);
    CHECK_EQ(stats.sent, GT_IPC_RING_SLOTS + 2);
    CHECK_EQ(stats.queued, 0);
    CHECK_EQ(stats.refused, 2);

    gt_ipc_client_close(client);
}

/* ---- IOV-04: 1 free slot: refused into the same gap ----
 * Counter 65 is refused while the hub is down. At 100 ms the hub is back
 * but slow: it takes one frame, then says EAGAIN (GT_ERR_FULL), so only 1
 * slot is free. Counter 66 needs 2 (GAP + reading), so it is refused too,
 * into the same gap: no second OVERFLOW. Once the hub catches up, counter
 * 67 goes out behind a single GAP covering both refusals (65, count 2). */
static void test_iov04_one_free_slot_same_gap(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake.fail_connects = 1;
    CHECK_EQ(send_readings(client, 1, GT_IPC_RING_SLOTS, 0), GT_IPC_RING_SLOTS);
    CHECK_EQ(send_readings(client, GT_IPC_RING_SLOTS + 1, 1, 0), 0);

    fake_sock_push_send_result(&fake, GT_OK);
    fake_sock_push_send_result(&fake, GT_ERR_FULL);   /* first drain stops */
    fake_sock_push_send_result(&fake, GT_ERR_FULL);   /* second drain stops */
    gt_msg squeezed = light_reading(GT_IPC_RING_SLOTS + 2, 6000);
    unsigned events = 0;
    CHECK_EQ(gt_ipc_client_send(client, &squeezed, 100, &events), GT_ERR_FULL);
    CHECK_EQ(events, GT_IPC_EV_RECONNECT);
    CHECK_EQ(fake.n_sent, 1);

    gt_msg after = light_reading(GT_IPC_RING_SLOTS + 3, 7000);
    CHECK_EQ(gt_ipc_client_send(client, &after, 200, &events), GT_OK);
    CHECK_EQ(events, 0);
    CHECK_EQ(fake.n_sent, GT_IPC_RING_SLOTS + 2);
    gt_msg gap = {
        .type = GT_MSG_GAP, .sensor_id = GT_SENSOR_LIGHT,
        .counter = GT_IPC_RING_SLOTS + 1, .seq = 0, .value = 2,
    };
    CHECK(sent_frame_is(&fake, GT_IPC_RING_SLOTS, &gap));
    CHECK(sent_frame_is(&fake, GT_IPC_RING_SLOTS + 1, &after));

    gt_ipc_stats stats;
    CHECK_EQ(gt_ipc_client_stats(client, &stats), GT_OK);
    CHECK_EQ(stats.refused, 2);

    gt_ipc_client_close(client);
}

/* ---- IOV-05: a second overflow gives a second GAP ----
 * Run 1 is IOV-03's: 1-64 buffered, 65-66 refused, and at 100 ms GAP(65, 2)
 * goes out before 67. At 200 ms the hub is lost again (EPIPE), the ring
 * refills with 68-131, and 132-134 are refused. That is a new run, so it
 * reports OVERFLOW again, and at 300 ms its own GAP(132, 3) goes out
 * before 135. The first GAP's counts don't leak into the second. */
static void test_iov05_second_overflow_second_gap(void)
{
    fake_sock fake;
    gt_ipc_client *client = open_on(&fake);
    fake.fail_connects = 1;
    CHECK_EQ(send_readings(client, 1, GT_IPC_RING_SLOTS, 0), GT_IPC_RING_SLOTS);
    CHECK_EQ(send_readings(client, GT_IPC_RING_SLOTS + 1, 2, 0), 0);
    CHECK_EQ(send_readings(client, GT_IPC_RING_SLOTS + 3, 1, 100), 1);
    CHECK_EQ(fake.n_sent, GT_IPC_RING_SLOTS + 2);

    /* Run 2: the first of these sends meets EPIPE and the rest are buffered. */
    const uint32_t second_start = GT_IPC_RING_SLOTS + 4;
    const uint32_t second_refused = second_start + GT_IPC_RING_SLOTS;
    fake_sock_push_send_result(&fake, GT_ERR_IO);
    CHECK_EQ(send_readings(client, second_start, GT_IPC_RING_SLOTS, 200), GT_IPC_RING_SLOTS);

    gt_msg first_refused = light_reading(second_refused, 8000);
    unsigned events = 0;
    CHECK_EQ(gt_ipc_client_send(client, &first_refused, 200, &events), GT_ERR_FULL);
    CHECK_EQ(events, GT_IPC_EV_OVERFLOW);
    CHECK_EQ(send_readings(client, second_refused + 1, 2, 200), 0);

    gt_msg after = light_reading(second_refused + 3, 9000);
    CHECK_EQ(gt_ipc_client_send(client, &after, 300, &events), GT_OK);
    CHECK_EQ(events, GT_IPC_EV_RECONNECT);

    size_t second_gap_index = 2 * GT_IPC_RING_SLOTS + 2;
    CHECK_EQ(fake.n_sent, second_gap_index + 2);
    gt_msg second_gap = {
        .type = GT_MSG_GAP, .sensor_id = GT_SENSOR_LIGHT,
        .counter = second_refused, .seq = 0, .value = 3,
    };
    CHECK(sent_frame_is(&fake, second_gap_index, &second_gap));
    CHECK(sent_frame_is(&fake, second_gap_index + 1, &after));

    gt_ipc_stats stats;
    CHECK_EQ(gt_ipc_client_stats(client, &stats), GT_OK);
    CHECK_EQ(stats.refused, 5);

    gt_ipc_client_close(client);
}

static const test_case_t tests[] = {
    {"IOV-01", "64 buffered OK, the 65th is FULL+OVERFLOW", test_iov01_ring_full_refuses},
    {"IOV-02", "OVERFLOW once per run, every refusal counted", test_iov02_overflow_once_per_run},
    {"IOV-03", "drain order is 64 data, GAP, new reading", test_iov03_drain_order_data_gap_new},
    {"IOV-04", "1 free slot: refused into the same gap", test_iov04_one_free_slot_same_gap},
    {"IOV-05", "a second overflow gives a second GAP", test_iov05_second_overflow_second_gap},
};

int main(void)
{
    return RUN_TESTS(tests);
}
