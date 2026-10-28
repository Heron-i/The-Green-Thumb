/* Unit tests for the hub sender's wait for seq_resume (src/hub_sender.c):
 * the DISCONNECTED -> AWAITING_RESUME -> SENDING steps of
 * gt_hub_sender_pump.
 *
 * Purpose: prove the PF-02 rules (Sensor-Pipeline.md, Hub contract). After
 * connecting, the sender sends nothing until storage's 8 B reply arrives.
 * A valid reply sets next_seq by gt_next_seq. A reply that doesn't come
 * within 2 s, a bad or wrong-size one, or storage closing the connection
 * means disconnect and retry with the 100 ms -> 2 s backoff. Losing
 * storage is reported once per outage (storage_unreachable), and getting
 * it back once (storage_reconnected). Time is passed in and the socket is
 * tests/fake_sock.h, so nothing sleeps. Run under make test / asan /
 * memcheck.
 * Implements HSR-01 through HSR-08.
 *
 * What these tests cannot prove: that the real storage sends the reply
 * at once, or that a real recv() behaves like the fake's (HSL does). */
#include <stdint.h>

#include "check.h"
#include "fake_sock.h"
#include "hub_sender.h"
#include "storage_proto.h"

/* Reset `fake`, expect 24 B records from it, and open a sender on it. */
static gt_hub_sender *open_on(fake_sock *fake)
{
    gt_hub_sender *sender = gt_hub_sender_open_ops(fake_sock_ops(fake), fake);
    fake->expected_len = GT_REC_SIZE;
    return sender;
}

/* A record as gt_hub_accept hands it over: no seq yet. */
static gt_record light_record(uint32_t value)
{
    gt_record record = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT, .seq = 0,
                         .timestamp_us = 1700000000000000ull, .value = value };
    return record;
}

/* Script storage's seq_resume reply as the fake's next recv. */
static void push_resume(fake_sock *fake, int has_last, uint32_t last_seq)
{
    const gt_resume resume = { .has_last = has_last, .seq = last_seq };
    uint8_t reply[GT_RESUME_SIZE];
    CHECK_EQ(gt_resume_encode(&resume, reply), GT_OK);
    fake_sock_push_recv(fake, GT_OK, reply, sizeof reply);
}

/* ---- HSR-01: nothing is sent before seq_resume ----
 * A record is queued and storage accepts the connection but hasn't
 * replied yet: the sender polls recv and sends nothing, however often it
 * is pumped inside the 2 s window. Waiting is not trouble: no event. */
static void test_hsr01_nothing_before_reply(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    const gt_record record = light_record(1);
    unsigned events = 0xFFu;

    CHECK_EQ(gt_hub_sender_push(sender, &record, NULL), GT_OK);
    CHECK_EQ(gt_hub_sender_pump(sender, 0, &events), GT_OK);
    CHECK_EQ(events, 0);
    CHECK_EQ(gt_hub_sender_pump(sender, 1500, &events), GT_OK);
    CHECK_EQ(events, 0);
    CHECK_EQ(fake.connect_calls, 1);
    CHECK(fake.recv_calls >= 2);
    CHECK_EQ(fake.send_calls, 0);
    CHECK_EQ(fake.disconnect_calls, 0);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.resumed, 0);
    CHECK_EQ(stats.queued, 1);
    gt_hub_sender_close(sender);
}

/* ---- HSR-02: a valid reply sets next_seq to reply + 1 ----
 * A fresh sender (nothing sent yet) and storage's last seq 59: the next
 * seq is 60. The first connect was clean, so the only event is RESUMED
 * (seq_resume), not RECONNECTED. */
static void test_hsr02_reply_sets_next_seq(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    push_resume(&fake, 1, 59);
    unsigned events = 0;

    CHECK_EQ(gt_hub_sender_pump(sender, 0, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_RESUMED);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.resumed, 1);
    CHECK_EQ(stats.next_seq, 60);
    CHECK_EQ(stats.has_own, 0);
    CHECK_EQ(fake.disconnect_calls, 0);
    gt_hub_sender_close(sender);
}

/* ---- HSR-03: an empty-log reply starts at seq 0 ----
 * has_last 0: storage holds nothing yet, so a fresh sender starts at 0
 * (gt_next_seq, NSQ-02). */
static void test_hsr03_empty_log_starts_at_0(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    push_resume(&fake, 0, 0);
    unsigned events = 0;

    CHECK_EQ(gt_hub_sender_pump(sender, 0, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_RESUMED);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.resumed, 1);
    CHECK_EQ(stats.next_seq, 0);
    gt_hub_sender_close(sender);
}

/* ---- HSR-04: no reply in 2 s: disconnect, retry at +100 ms ----
 * Storage accepted at 1000 but is stuck (say, still recovering) and never
 * replies. At 2999 the sender is still waiting; at 3000 the 2 s are up:
 * disconnect, report storage_unreachable, and try again no earlier than
 * 3100 (the first backoff wait). */
static void test_hsr04_reply_deadline(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    unsigned events = 0;

    CHECK_EQ(gt_hub_sender_pump(sender, 1000, &events), GT_OK);
    CHECK_EQ(gt_hub_sender_pump(sender, 1000 + GT_HUB_RESUME_WAIT_MS - 1, &events), GT_OK);
    CHECK_EQ(events, 0);
    CHECK_EQ(fake.disconnect_calls, 0);

    CHECK_EQ(gt_hub_sender_pump(sender, 1000 + GT_HUB_RESUME_WAIT_MS, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_UNREACHABLE);
    CHECK_EQ(fake.disconnect_calls, 1);

    uint64_t retry_at = 1000 + GT_HUB_RESUME_WAIT_MS + GT_IPC_BACKOFF_MIN_MS;
    CHECK_EQ(gt_hub_sender_pump(sender, retry_at - 1, &events), GT_OK);
    CHECK_EQ(fake.connect_calls, 1);
    CHECK_EQ(gt_hub_sender_pump(sender, retry_at, &events), GT_OK);
    CHECK_EQ(fake.connect_calls, 2);
    CHECK_EQ(fake.recv_while_disconnected, 0);
    gt_hub_sender_close(sender);
}

/* ---- HSR-05: a bad reply disconnects as ipc_bad_message ----
 * Three connects in a row get three bad replies: 8 B with a bad magic,
 * 7 B (too short), and 9 B (too long: the sender reads into
 * GT_RESUME_SIZE + 1 bytes, so it sees all 9). Each is BAD_REPLY and a
 * disconnect. They are one outage, so storage_unreachable comes only with
 * the first. Retries follow the backoff: 0, then +100, then +200. */
static void test_hsr05_bad_reply(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    const gt_resume resume = { .has_last = 1, .seq = 59 };
    uint8_t bad_magic[GT_RESUME_SIZE + 1] = { 0 };
    CHECK_EQ(gt_resume_encode(&resume, bad_magic), GT_OK);
    bad_magic[0] ^= 0xFFu;
    const uint8_t too_short[GT_RESUME_SIZE - 1] = { 0 };
    uint8_t too_long[GT_RESUME_SIZE + 1];
    CHECK_EQ(gt_resume_encode(&resume, too_long), GT_OK);
    too_long[GT_RESUME_SIZE] = 0;
    fake_sock_push_recv(&fake, GT_OK, bad_magic, GT_RESUME_SIZE);
    fake_sock_push_recv(&fake, GT_OK, too_short, sizeof too_short);
    fake_sock_push_recv(&fake, GT_OK, too_long, sizeof too_long);
    unsigned events = 0;

    CHECK_EQ(gt_hub_sender_pump(sender, 0, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_BAD_REPLY | GT_HUB_SENDER_EV_UNREACHABLE);
    CHECK_EQ(gt_hub_sender_pump(sender, 100, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_BAD_REPLY);
    CHECK_EQ(gt_hub_sender_pump(sender, 300, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_BAD_REPLY);
    CHECK_EQ(fake.connect_calls, 3);
    CHECK_EQ(fake.disconnect_calls, 3);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.resumed, 0);
    gt_hub_sender_close(sender);
}

/* ---- HSR-06: storage closing before the reply is unreachable ----
 * Storage accepted, then died (kill -9) before replying: recv reports the
 * closed connection. The sender disconnects at once rather than waiting
 * out the 2 s, and reports storage_unreachable. */
static void test_hsr06_closed_before_reply(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    fake_sock_push_recv(&fake, GT_ERR_IO, NULL, 0);
    unsigned events = 0;

    CHECK_EQ(gt_hub_sender_pump(sender, 0, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_UNREACHABLE);
    CHECK_EQ(fake.disconnect_calls, 1);
    CHECK_EQ(fake.send_calls, 0);
    gt_hub_sender_close(sender);
}

/* ---- HSR-07: storage down at start: backoff, then reconnect ----
 * The first three connects fail (storage not started yet). The first
 * failure reports storage_unreachable and the others say nothing (one
 * outage). Attempts fall at 0, 100 and 300 ms, never early. The fourth,
 * at 700, connects and gets the reply: storage_reconnected and seq_resume
 * together. */
static void test_hsr07_down_at_start(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    fake.fail_connects = 3;
    push_resume(&fake, 1, 9);
    unsigned events = 0;

    CHECK_EQ(gt_hub_sender_pump(sender, 0, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_UNREACHABLE);
    CHECK_EQ(gt_hub_sender_pump(sender, 99, &events), GT_OK);
    CHECK_EQ(fake.connect_calls, 1);
    CHECK_EQ(gt_hub_sender_pump(sender, 100, &events), GT_OK);
    CHECK_EQ(events, 0);
    CHECK_EQ(gt_hub_sender_pump(sender, 299, &events), GT_OK);
    CHECK_EQ(fake.connect_calls, 2);
    CHECK_EQ(gt_hub_sender_pump(sender, 300, &events), GT_OK);
    CHECK_EQ(events, 0);
    CHECK_EQ(fake.connect_calls, 3);
    CHECK_EQ(gt_hub_sender_pump(sender, 700, &events), GT_OK);
    CHECK_EQ(fake.connect_calls, 4);
    CHECK_EQ(events, GT_HUB_SENDER_EV_RECONNECTED | GT_HUB_SENDER_EV_RESUMED);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.next_seq, 10);
    CHECK_EQ(fake.recv_while_disconnected, 0);
    gt_hub_sender_close(sender);
}

/* ---- HSR-08: NULL args and unencodable records are rejected ----
 * A record gt_record_encode would refuse (DATA from sensor 0, sensor 3,
 * type 3) is turned away at push, so it can never sit at the head of the
 * queue and block every record behind it. events_out is cleared even on
 * failure. */
static void test_hsr08_bad_args(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    const gt_record good = light_record(1);
    gt_record bad_records[3] = { good, good, good };
    bad_records[0].sensor_id = GT_SENSOR_ALL;
    bad_records[1].sensor_id = 3;
    bad_records[2].type = 3;
    gt_hub_send_stats stats;
    unsigned events = 0xFFu;

    CHECK_EQ(gt_hub_sender_pump(NULL, 0, &events), GT_ERR_INVAL);
    CHECK_EQ(events, 0);
    CHECK_EQ(gt_hub_sender_stats(NULL, &stats), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_sender_stats(sender, NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_sender_push(NULL, &good, NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_sender_push(sender, NULL, NULL), GT_ERR_INVAL);
    /* Invariant: bad_records[0..bad_index-1] were each refused. */
    for (size_t bad_index = 0; bad_index < 3; bad_index++) {
        events = 0xFFu;
        CHECK_EQ(gt_hub_sender_push(sender, &bad_records[bad_index], &events), GT_ERR_INVAL);
        CHECK_EQ(events, 0);
    }
    CHECK_EQ(gt_hub_sender_stats(sender, &stats), GT_OK);
    CHECK_EQ(stats.queued, 0);
    gt_hub_sender_close(sender);
}

static const test_case_t tests[] = {
    {"HSR-01", "nothing is sent before seq_resume", test_hsr01_nothing_before_reply},
    {"HSR-02", "a valid reply sets next_seq to reply + 1", test_hsr02_reply_sets_next_seq},
    {"HSR-03", "an empty-log reply starts at seq 0", test_hsr03_empty_log_starts_at_0},
    {"HSR-04", "no reply in 2 s: disconnect, retry", test_hsr04_reply_deadline},
    {"HSR-05", "a bad reply disconnects as ipc_bad_message", test_hsr05_bad_reply},
    {"HSR-06", "storage closing before the reply", test_hsr06_closed_before_reply},
    {"HSR-07", "storage down at start: backoff, reconnect", test_hsr07_down_at_start},
    {"HSR-08", "NULL args and unencodable records rejected", test_hsr08_bad_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
