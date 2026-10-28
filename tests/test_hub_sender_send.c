/* Unit tests for the hub sender's SENDING state (src/hub_sender.c): how
 * gt_hub_sender_pump stamps and sends queued records.
 *
 * Purpose: prove the PF-01 / PF-03 numbering rules (Sensor-Pipeline.md,
 * Hub contract). Each record gets its seq and CRC only as it is sent, and
 * a seq is used only when send() returns GT_OK. A full socket (FULL) or a
 * lost storage (IO) leaves the head record queued, unstamped, and
 * next_seq unchanged, so a failed send never makes a hole. Across a
 * reconnect the sender keeps its own counter (max rule), so records
 * storage lost in a kill -9 leave a hole that storage covers with a GAP.
 * Anything storage sends after its reply is a bad message. Runs against
 * tests/fake_sock.h; time is passed in. Run under make test / asan /
 * memcheck.
 * Implements HSS-01 through HSS-07.
 *
 * What these tests cannot prove: that storage really writes the GAP for
 * the hole (HSL does, against the real storage). */
#include <stdint.h>

#include "check.h"
#include "fake_sock.h"
#include "hub_sender.h"
#include "storage_proto.h"

#define RECEIVED_US 1700000000000000ull

/* Reset `fake`, expect 24 B records from it, and open a sender on it. */
static gt_hub_sender *open_on(fake_sock *fake)
{
    gt_hub_sender *sender = gt_hub_sender_open_ops(fake_sock_ops(fake), fake);
    fake->expected_len = GT_REC_SIZE;
    return sender;
}

/* Queue one unstamped light record whose value (and receive time) tell
 * the records apart. */
static void push_light(gt_hub_sender *sender, uint32_t value)
{
    const gt_record record = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT, .seq = 0,
                               .timestamp_us = RECEIVED_US + value, .value = value };
    CHECK_EQ(gt_hub_sender_push(sender, &record, NULL), GT_OK);
}

/* Script storage's seq_resume reply as the fake's next recv. */
static void push_resume(fake_sock *fake, uint32_t last_seq)
{
    const gt_resume resume = { .has_last = 1, .seq = last_seq };
    uint8_t reply[GT_RESUME_SIZE];
    CHECK_EQ(gt_resume_encode(&resume, reply), GT_OK);
    fake_sock_push_recv(fake, GT_OK, reply, sizeof reply);
}

/* Decode the fake's sent frame number `sent_index` into *record_out.
 * Returns 1 only if that frame exists, is 24 B and decodes (CRC included). */
static int sent_record(const fake_sock *fake, size_t sent_index, gt_record *record_out)
{
    return sent_index < fake->n_sent && fake->sent_len[sent_index] == GT_REC_SIZE &&
           gt_record_decode(fake->sent[sent_index], record_out) == GT_OK;
}

/* ---- HSS-01: records go out stamped next_seq, next_seq + 1 ----
 * Two records wait while the sender connects; storage replies 59. Both
 * go out in the same pump, oldest first, as seq 60 and 61, with valid
 * CRCs and their own value and receive time. The stats show the sender
 * now has its own counter, starting at 60. */
static void test_hss01_stamped_in_order(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    push_light(sender, 1);
    push_light(sender, 2);
    push_resume(&fake, 59);

    CHECK_EQ(gt_hub_sender_pump(sender, 0, NULL), GT_OK);
    CHECK_EQ(fake.n_sent, 2);
    gt_record first;
    gt_record second;
    CHECK(sent_record(&fake, 0, &first));
    CHECK(sent_record(&fake, 1, &second));
    CHECK_EQ(first.seq, 60);
    CHECK_EQ(first.value, 1);
    CHECK(first.timestamp_us == RECEIVED_US + 1);
    CHECK_EQ(second.seq, 61);
    CHECK_EQ(second.value, 2);
    CHECK_EQ(fake.wrong_len_sends, 0);

    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.sent, 2);
    CHECK_EQ(stats.queued, 0);
    CHECK_EQ(stats.has_own, 1);
    CHECK_EQ(stats.first_sent_seq, 60);
    CHECK_EQ(stats.next_seq, 62);
    gt_hub_sender_close(sender);
}

/* ---- HSS-02: FULL keeps the head and its seq ----
 * Storage is alive but its socket buffer is full (EAGAIN): the sender
 * stops, stays connected, uses no seq and reports nothing. The next pump
 * sends both records as 60 and 61, so the FULL left no hole. */
static void test_hss02_full_keeps_head(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    push_light(sender, 1);
    push_light(sender, 2);
    push_resume(&fake, 59);
    fake_sock_push_send_result(&fake, GT_ERR_FULL);
    unsigned events = 0;

    CHECK_EQ(gt_hub_sender_pump(sender, 0, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_RESUMED);
    CHECK_EQ(fake.n_sent, 0);
    CHECK_EQ(fake.disconnect_calls, 0);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.queued, 2);
    CHECK_EQ(stats.next_seq, 60);
    CHECK_EQ(stats.has_own, 0);

    CHECK_EQ(gt_hub_sender_pump(sender, 10, &events), GT_OK);
    CHECK_EQ(events, 0);
    gt_record first;
    gt_record second;
    CHECK(sent_record(&fake, 0, &first));
    CHECK(sent_record(&fake, 1, &second));
    CHECK_EQ(first.seq, 60);
    CHECK_EQ(first.value, 1);
    CHECK_EQ(second.seq, 61);
    gt_hub_sender_close(sender);
}

/* ---- HSS-03: IO keeps the head unstamped for after reconnect ----
 * Record 1 goes out as 60; storage dies before record 2 (EPIPE). The
 * sender disconnects, reports storage_unreachable, and keeps record 2 at
 * the head with next_seq still 61. Storage restarts having kept 60, so
 * its reply is 60; the retry at +100 ms sends record 2 as 61, with no
 * hole and no reuse. */
static void test_hss03_io_keeps_head(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    push_light(sender, 1);
    push_light(sender, 2);
    push_resume(&fake, 59);
    fake_sock_push_send_result(&fake, GT_OK);
    fake_sock_push_send_result(&fake, GT_ERR_IO);
    unsigned events = 0;

    CHECK_EQ(gt_hub_sender_pump(sender, 0, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_RESUMED | GT_HUB_SENDER_EV_UNREACHABLE);
    CHECK_EQ(fake.disconnect_calls, 1);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.queued, 1);
    CHECK_EQ(stats.sent, 1);
    CHECK_EQ(stats.next_seq, 61);

    push_resume(&fake, 60);
    CHECK_EQ(gt_hub_sender_pump(sender, GT_IPC_BACKOFF_MIN_MS, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_RECONNECTED | GT_HUB_SENDER_EV_RESUMED);
    CHECK_EQ(fake.n_sent, 2);
    gt_record resent;
    CHECK(sent_record(&fake, 1, &resent));
    CHECK_EQ(resent.seq, 61);
    CHECK_EQ(resent.value, 2);
    CHECK_EQ(fake.send_while_disconnected, 0);
    gt_hub_sender_close(sender);
}

/* ---- HSS-04: own counter survives a kill -9 (hole, not reuse) ----
 * Records go out as 60-64. Storage is killed with 62-64 still unsynced,
 * so after its restart its reply is 61. The sender keeps its own counter
 * (next 65 > 61 + 1), so the next record goes out as 65: storage sees
 * 62-64 missing and writes their GAP (PF-03). Going back to 62 would
 * reuse the lost seqs and hide the loss. */
static void test_hss04_counter_survives_kill(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    /* Invariant: records 1..value-1 are queued, in order. */
    for (uint32_t value = 1; value <= 5; value++) {
        push_light(sender, value);
    }
    push_resume(&fake, 59);
    CHECK_EQ(gt_hub_sender_pump(sender, 0, NULL), GT_OK);
    CHECK_EQ(fake.n_sent, 5);

    push_light(sender, 6);
    fake_sock_push_send_result(&fake, GT_ERR_IO);   /* the 6th send: storage is gone */
    unsigned events = 0;
    CHECK_EQ(gt_hub_sender_pump(sender, 10, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_UNREACHABLE);

    push_resume(&fake, 61);
    CHECK_EQ(gt_hub_sender_pump(sender, 10 + GT_IPC_BACKOFF_MIN_MS, &events), GT_OK);
    gt_record after_restart;
    CHECK(sent_record(&fake, 5, &after_restart));
    CHECK_EQ(after_restart.seq, 65);
    CHECK_EQ(after_restart.value, 6);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.first_sent_seq, 60);
    CHECK_EQ(stats.next_seq, 66);
    gt_hub_sender_close(sender);
}

/* ---- HSS-05: a reply ahead of the counter wins ----
 * The sender has sent 60 and 61 (own next 62). After a reconnect storage
 * says it already holds up to 100 (say, a storage_feed run filled it), so
 * 62 would only be rejected as old: the next record goes out as 101. */
static void test_hss05_reply_ahead_wins(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    push_light(sender, 1);
    push_light(sender, 2);
    push_resume(&fake, 59);
    CHECK_EQ(gt_hub_sender_pump(sender, 0, NULL), GT_OK);

    push_light(sender, 3);
    fake_sock_push_send_result(&fake, GT_ERR_IO);
    CHECK_EQ(gt_hub_sender_pump(sender, 10, NULL), GT_OK);
    push_resume(&fake, 100);
    CHECK_EQ(gt_hub_sender_pump(sender, 10 + GT_IPC_BACKOFF_MIN_MS, NULL), GT_OK);
    gt_record after_reconnect;
    CHECK(sent_record(&fake, 2, &after_reconnect));
    CHECK_EQ(after_reconnect.seq, 101);
    CHECK_EQ(after_reconnect.value, 3);
    gt_hub_sender_close(sender);
}

/* ---- HSS-06: bytes after the reply are ipc_bad_message ----
 * Storage sends nothing after seq_resume, so a stray message means the
 * two sides disagree about the protocol: report it, drop the connection
 * and send nothing on it. The record waiting stays queued, unstamped. */
static void test_hss06_bytes_after_reply(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    push_resume(&fake, 59);
    CHECK_EQ(gt_hub_sender_pump(sender, 0, NULL), GT_OK);

    push_light(sender, 1);
    const uint8_t stray[GT_RESUME_SIZE] = { 0xA8, 1, 0x10, 1, 60, 0, 0, 0 };
    fake_sock_push_recv(&fake, GT_OK, stray, sizeof stray);
    unsigned events = 0;
    CHECK_EQ(gt_hub_sender_pump(sender, 10, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_BAD_REPLY | GT_HUB_SENDER_EV_UNREACHABLE);
    CHECK_EQ(fake.disconnect_calls, 1);
    CHECK_EQ(fake.n_sent, 0);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.queued, 1);
    CHECK_EQ(stats.next_seq, 60);
    gt_hub_sender_close(sender);
}

/* ---- HSS-07: an idle sender notices storage closing ----
 * Nothing is queued, so there is no send to fail; the recv check in
 * SENDING still sees storage close the connection (a kill -9) and reports
 * storage_unreachable straight away, not at the next reading. */
static void test_hss07_idle_sees_close(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    push_resume(&fake, 59);
    CHECK_EQ(gt_hub_sender_pump(sender, 0, NULL), GT_OK);

    fake_sock_push_recv(&fake, GT_ERR_IO, NULL, 0);
    unsigned events = 0;
    CHECK_EQ(gt_hub_sender_pump(sender, 10, &events), GT_OK);
    CHECK_EQ(events, GT_HUB_SENDER_EV_UNREACHABLE);
    CHECK_EQ(fake.disconnect_calls, 1);
    gt_hub_sender_close(sender);
}

static const test_case_t tests[] = {
    {"HSS-01", "records go out stamped next_seq, + 1", test_hss01_stamped_in_order},
    {"HSS-02", "FULL keeps the head and its seq", test_hss02_full_keeps_head},
    {"HSS-03", "IO keeps the head unstamped for reconnect", test_hss03_io_keeps_head},
    {"HSS-04", "own counter survives a kill -9", test_hss04_counter_survives_kill},
    {"HSS-05", "a reply ahead of the counter wins", test_hss05_reply_ahead_wins},
    {"HSS-06", "bytes after the reply are ipc_bad_message", test_hss06_bytes_after_reply},
    {"HSS-07", "an idle sender notices storage closing", test_hss07_idle_sees_close},
};

int main(void)
{
    return RUN_TESTS(tests);
}
