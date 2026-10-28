/* Unit tests for a full hub queue (src/hub_sender.c): the refuse, count
 * and GAP path of gt_hub_sender_push.
 *
 * Purpose: prove the DESIGN.md Hub -> storage row "Hub queue full". Once
 * 1024 records wait, new ones are refused and counted, with one
 * hub_queue_full per run. The next record accepted with 2 free slots is
 * preceded by one GAP record {sensor 0, the first refused record's
 * receive time, the refused readings}, so storage gets old records, then
 * the GAP, then new ones, and the GAP gets its seq at send like any other.
 * Same 2-slot rule as the sensor client's ring (IOV). Runs against
 * tests/fake_sock.h. Run under make test / asan / memcheck.
 * Implements HSQ-01 through HSQ-06.
 *
 * What these tests cannot prove: how long a real storage outage takes to
 * fill the queue (about 17 min at real rates; the soak test). */
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

/* Push one light record whose value is also its receive-time offset, and
 * return what push said; *events_out gets push's events. */
static gt_status push_light(gt_hub_sender *sender, uint32_t value, unsigned *events_out)
{
    const gt_record record = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT, .seq = 0,
                               .timestamp_us = RECEIVED_US + value, .value = value };
    return gt_hub_sender_push(sender, &record, events_out);
}

/* Fill the queue with records valued 0..GT_HUB_QUEUE_SLOTS-1 (storage is
 * down, nothing is pumped). */
static void fill_queue(gt_hub_sender *sender)
{
    /* Invariant: records 0..value-1 are queued, in order. */
    for (uint32_t value = 0; value < GT_HUB_QUEUE_SLOTS; value++) {
        CHECK_EQ(push_light(sender, value, NULL), GT_OK);
    }
}

/* ---- HSQ-01: 1024 queue, the 1025th is refused ----
 * The first refusal is GT_ERR_FULL with hub_queue_full, and is counted. */
static void test_hsq01_full_after_1024(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    fill_queue(sender);
    unsigned events = 0;

    CHECK_EQ(push_light(sender, GT_HUB_QUEUE_SLOTS, &events), GT_ERR_FULL);
    CHECK_EQ(events, GT_HUB_SENDER_EV_QUEUE_FULL);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.queued, GT_HUB_QUEUE_SLOTS);
    CHECK_EQ(stats.refused, 1);
    gt_hub_sender_close(sender);
}

/* ---- HSQ-02: hub_queue_full once per run, every refusal counted ----
 * Storage stays down for a while: four records are refused in a row.
 * Only the first reports hub_queue_full (the hub would otherwise log once
 * a second for the whole outage); all four are counted. */
static void test_hsq02_event_once_per_run(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    fill_queue(sender);
    unsigned events = 0;

    CHECK_EQ(push_light(sender, 2000, &events), GT_ERR_FULL);
    CHECK_EQ(events, GT_HUB_SENDER_EV_QUEUE_FULL);
    /* Invariant: every refusal after the first reports no event. */
    for (uint32_t value = 2001; value <= 2003; value++) {
        CHECK_EQ(push_light(sender, value, &events), GT_ERR_FULL);
        CHECK_EQ(events, 0);
    }
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.refused, 4);
    gt_hub_sender_close(sender);
}

/* Storage comes back with an empty log (has_last 0), so seqs start at 0
 * and each sent frame's index is its seq. Pump until it has all gone. */
static void storage_back_and_drain(fake_sock *fake, gt_hub_sender *sender, uint64_t now_ms)
{
    const gt_resume empty_log = { .has_last = 0, .seq = 0 };
    uint8_t reply[GT_RESUME_SIZE];
    CHECK_EQ(gt_resume_encode(&empty_log, reply), GT_OK);
    fake_sock_push_recv(fake, GT_OK, reply, sizeof reply);
    CHECK_EQ(gt_hub_sender_pump(sender, now_ms, NULL), GT_OK);
}

/* ---- HSQ-03: order is old records, the GAP, then new ----
 * Records 1024 and 1025 are refused while the queue is full. Storage
 * comes back and takes the 1024 old records (seqs 0-1023). The next
 * record, 3000, finds room for 2, so a GAP goes in first: sensor 0, the
 * first refused record's receive time, count 2. On the wire: GAP as seq
 * 1024, then 3000 as seq 1025. */
static void test_hsq03_old_gap_new_order(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    fill_queue(sender);
    CHECK_EQ(push_light(sender, 1024, NULL), GT_ERR_FULL);
    CHECK_EQ(push_light(sender, 1025, NULL), GT_ERR_FULL);
    storage_back_and_drain(&fake, sender, 0);
    CHECK_EQ(fake.n_sent, GT_HUB_QUEUE_SLOTS);

    CHECK_EQ(push_light(sender, 3000, NULL), GT_OK);
    CHECK_EQ(gt_hub_sender_pump(sender, 10, NULL), GT_OK);
    CHECK_EQ(fake.n_sent, GT_HUB_QUEUE_SLOTS + 2);
    gt_record last_old;
    gt_record gap;
    gt_record new_record;
    CHECK_EQ(gt_record_decode(fake.sent[GT_HUB_QUEUE_SLOTS - 1], &last_old), GT_OK);
    CHECK_EQ(gt_record_decode(fake.sent[GT_HUB_QUEUE_SLOTS], &gap), GT_OK);
    CHECK_EQ(gt_record_decode(fake.sent[GT_HUB_QUEUE_SLOTS + 1], &new_record), GT_OK);
    CHECK_EQ(last_old.value, GT_HUB_QUEUE_SLOTS - 1);
    CHECK_EQ(gap.type, GT_REC_GAP);
    CHECK_EQ(gap.sensor_id, GT_SENSOR_ALL);
    CHECK_EQ(gap.seq, GT_HUB_QUEUE_SLOTS);
    CHECK_EQ(gap.value, 2);
    CHECK(gap.timestamp_us == RECEIVED_US + 1024);
    CHECK_EQ(new_record.type, GT_REC_DATA);
    CHECK_EQ(new_record.value, 3000);
    CHECK_EQ(new_record.seq, GT_HUB_QUEUE_SLOTS + 1);
    gt_hub_sender_close(sender);
}

/* ---- HSQ-04: one free slot refuses into the same gap ----
 * Record 1024 is refused. Storage takes just one record before its
 * buffer fills (send OK, then FULL), leaving one free slot: too few for
 * the GAP and a record, so 1025 is refused into the same gap with no new
 * event. Once the queue drains, 3000 goes in behind one GAP of count 2,
 * still stamped with 1024's receive time. */
static void test_hsq04_one_slot_same_gap(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    fill_queue(sender);
    unsigned events = 0;
    CHECK_EQ(push_light(sender, 1024, &events), GT_ERR_FULL);
    fake_sock_push_send_result(&fake, GT_OK);
    fake_sock_push_send_result(&fake, GT_ERR_FULL);
    storage_back_and_drain(&fake, sender, 0);
    CHECK_EQ(fake.n_sent, 1);

    CHECK_EQ(push_light(sender, 1025, &events), GT_ERR_FULL);
    CHECK_EQ(events, 0);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.refused, 2);
    CHECK_EQ(stats.queued, GT_HUB_QUEUE_SLOTS - 1);

    CHECK_EQ(gt_hub_sender_pump(sender, 10, NULL), GT_OK);
    CHECK_EQ(push_light(sender, 3000, NULL), GT_OK);
    CHECK_EQ(gt_hub_sender_pump(sender, 20, NULL), GT_OK);
    gt_record gap;
    CHECK_EQ(gt_record_decode(fake.sent[GT_HUB_QUEUE_SLOTS], &gap), GT_OK);
    CHECK_EQ(gap.type, GT_REC_GAP);
    CHECK_EQ(gap.value, 2);
    CHECK(gap.timestamp_us == RECEIVED_US + 1024);
    gt_hub_sender_close(sender);
}

/* ---- HSQ-05: a refused sensor GAP adds its readings ----
 * While the queue is full the hub refuses a light GAP covering 26
 * readings, then one DATA. The hub's GAP must count readings, not
 * records, or the Evaluation plan's lost-readings formula comes up short:
 * 26 + 1 = 27. stats.refused still counts records (2). */
static void test_hsq05_refused_gap_counts_readings(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    fill_queue(sender);
    const gt_record sensor_gap = { .type = GT_REC_GAP, .sensor_id = GT_SENSOR_LIGHT, .seq = 0,
                                   .timestamp_us = RECEIVED_US + 1024, .value = 26 };
    CHECK_EQ(gt_hub_sender_push(sender, &sensor_gap, NULL), GT_ERR_FULL);
    CHECK_EQ(push_light(sender, 1025, NULL), GT_ERR_FULL);
    storage_back_and_drain(&fake, sender, 0);

    CHECK_EQ(push_light(sender, 3000, NULL), GT_OK);
    CHECK_EQ(gt_hub_sender_pump(sender, 10, NULL), GT_OK);
    gt_record gap;
    CHECK_EQ(gt_record_decode(fake.sent[GT_HUB_QUEUE_SLOTS], &gap), GT_OK);
    CHECK_EQ(gap.type, GT_REC_GAP);
    CHECK_EQ(gap.sensor_id, GT_SENSOR_ALL);
    CHECK_EQ(gap.value, 27);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.refused, 2);
    gt_hub_sender_close(sender);
}

/* ---- HSQ-06: a second overflow is a new run ----
 * The first run ends when its GAP is queued (behind record 3000). The
 * queue then fills again while storage is down, and the next refusal is
 * a new run: hub_queue_full again, which its own GAP will cover. (Its
 * bytes aren't checked: draining a second full queue would pass the
 * fake's 1100 recorded frames; HSQ-03 to 05 cover the GAP's content.) */
static void test_hsq06_second_run_reports_again(void)
{
    fake_sock fake;
    gt_hub_sender *sender = open_on(&fake);
    fill_queue(sender);
    unsigned events = 0;
    CHECK_EQ(push_light(sender, 1024, &events), GT_ERR_FULL);
    CHECK_EQ(events, GT_HUB_SENDER_EV_QUEUE_FULL);
    storage_back_and_drain(&fake, sender, 0);
    CHECK_EQ(push_light(sender, 3000, NULL), GT_OK);   /* queues the GAP, then 3000 */

    /* Invariant: the queue holds the GAP, 3000 and records 4000..value-1. */
    for (uint32_t value = 4000; value < 4000 + GT_HUB_QUEUE_SLOTS - 2; value++) {
        CHECK_EQ(push_light(sender, value, NULL), GT_OK);
    }
    CHECK_EQ(push_light(sender, 5000, &events), GT_ERR_FULL);
    CHECK_EQ(events, GT_HUB_SENDER_EV_QUEUE_FULL);
    gt_hub_send_stats stats;
    gt_hub_sender_stats(sender, &stats);
    CHECK_EQ(stats.refused, 2);
    CHECK_EQ(stats.queued, GT_HUB_QUEUE_SLOTS);
    gt_hub_sender_close(sender);
}

static const test_case_t tests[] = {
    {"HSQ-01", "1024 queue, the 1025th is refused", test_hsq01_full_after_1024},
    {"HSQ-02", "hub_queue_full once per run", test_hsq02_event_once_per_run},
    {"HSQ-03", "order is old records, the GAP, then new", test_hsq03_old_gap_new_order},
    {"HSQ-04", "one free slot refuses into the same gap", test_hsq04_one_slot_same_gap},
    {"HSQ-05", "a refused sensor GAP adds its readings", test_hsq05_refused_gap_counts_readings},
    {"HSQ-06", "a second overflow is a new run", test_hsq06_second_run_reports_again},
};

int main(void)
{
    return RUN_TESTS(tests);
}
