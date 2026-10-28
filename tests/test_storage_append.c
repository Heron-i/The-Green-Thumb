/* Unit tests for accepting records (src/storage.c): gt_storage_append.
 *
 * Purpose: prove storage accepts the next seq, refuses bad bytes and
 * duplicate or old seqs (DESIGN.md "storage rejects any seq <= last
 * committed") with the right event, checks the very first record against
 * the seq found on disk, turns a skip into a GAP record stored with the
 * record (both or neither), and refuses rather than overflows when the
 * pending buffer is full. Nothing is stored on any refusal. Counts and
 * seqs are read with gt_storage_read_stats. Uses tests/fake_file.h. Run
 * under make test / asan / memcheck. Implements SAP-01 through SAP-12.
 *
 * What these tests cannot prove: the stored bytes and the GAP-before-
 * record order, which only become visible in the file after a flush
 * (Mechanism D Step 7d, SFL tests); and appends racing a flush on another
 * thread (the lock is exercised, but single-threaded). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TODAY_NAME    "log-20261026.bin"
#define TEST_CAPACITY 8

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* Encode a light DATA record with `seq` into `wire`. */
static void make_wire(uint8_t wire[GT_REC_SIZE], uint32_t seq)
{
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .seq = seq, .timestamp_us = 5000 + seq, .value = 100 + seq,
    };
    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
}

/* Open storage on a fresh fake, after putting today's segment with
 * records seq 1 .. last_on_disk in it (none if last_on_disk is 0). */
static gt_storage *open_storage(uint32_t last_on_disk)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    if (last_on_disk > 0) {
        uint8_t segment[GT_SEG_HEADER_SZ + 16 * GT_REC_SIZE];
        const gt_seg_header header = { .created_at_us = 1000 };
        CHECK_EQ(gt_seg_header_encode(&header, segment), GT_OK);
        /* invariant: records seq 1 .. seq - 1 are encoded after the header */
        for (uint32_t seq = 1; seq <= last_on_disk; seq++) {
            make_wire(&segment[GT_SEG_HEADER_SZ + (seq - 1) * GT_REC_SIZE], seq);
        }
        CHECK_EQ(fake_fs_put(&fs, TODAY_NAME, segment,
                             GT_SEG_HEADER_SZ + last_on_disk * GT_REC_SIZE), GT_OK);
    }
    gt_storage *storage = NULL;
    gt_storage_open_args args = { .ops = ops, .ctx = &fs, .now_us = TODAY_US,
                                  .pending_capacity = TEST_CAPACITY };
    CHECK_EQ(gt_storage_open_ops(&args, &storage, ), );
    return storage;
}

/* Append a record with `seq` and check the status and events. */
static void append_expect(gt_storage *storage, uint32_t seq, gt_status expected_status,
                          unsigned expected_events)
{
    uint8_t wire[GT_REC_SIZE];
    unsigned events = 0xFFu;   /* append must reset it */
    make_wire(wire, seq);

    CHECK_EQ(gt_storage_append(storage, wire, 0, &events), expected_status);
    CHECK_EQ(events, expected_events);
}

/* The backlog level DESIGN.md defines for `pending` records out of
 * TEST_CAPACITY (the highest of 50/75/90% reached), worked out here on its
 * own rather than by calling storage, so the test can't share its bug. */
static unsigned expected_level(size_t pending)
{
    const size_t capacity = TEST_CAPACITY;   /* size_t, so both sides compare unsigned */
    if (pending * 100 >= capacity * 90) {
        return 90;
    }
    if (pending * 100 >= capacity * 75) {
        return 75;
    }
    return pending * 100 >= capacity * 50 ? 50 : 0;
}

/* The events an accepted record should report when it brings the buffer
 * to `pending_after` records (one more than before): GT_STORAGE_EV_BACKLOG
 * exactly when it reaches a new threshold. With capacity 8 that is the
 * 4th, 6th and 8th record. */
static unsigned fill_events(size_t pending_after)
{
    return expected_level(pending_after) > expected_level(pending_after - 1)
               ? GT_STORAGE_EV_BACKLOG : 0;
}

/* Check the pending count and the last accepted seq. */
static void check_accepted(const gt_storage *storage, size_t pending, uint32_t last_seq)
{
    gt_storage_stats stats;
    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    CHECK_EQ(stats.pending, pending);
    CHECK_EQ(stats.has_last_accepted, 1);
    CHECK_EQ(stats.last_accepted_seq, last_seq);
}

/* ---- SAP-01: the first record is accepted ----
 * Into an empty log. It is accepted, not committed: last committed stays
 * unset until a flush. */
static void test_sap01_first_record_accepted(void)
{
    gt_storage *storage = open_storage(0);
    gt_storage_stats stats;

    append_expect(storage, 1, GT_OK, 0);
    check_accepted(storage, 1, 1);
    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    CHECK_EQ(stats.has_last_committed, 0);
    CHECK(stats.committed_records == 0);
    gt_storage_close(storage);
}

/* ---- SAP-02: consecutive records are accepted ---- */
static void test_sap02_consecutive_accepted(void)
{
    gt_storage *storage = open_storage(0);

    append_expect(storage, 1, GT_OK, 0);
    append_expect(storage, 2, GT_OK, 0);
    append_expect(storage, 3, GT_OK, 0);
    check_accepted(storage, 3, 3);
    gt_storage_close(storage);
}

/* ---- SAP-03: bad bytes are refused, no event ----
 * One flipped bit fails the CRC. No event means "bad bytes": the caller
 * closes the connection. Nothing is stored. */
static void test_sap03_bad_bytes_refused(void)
{
    gt_storage *storage = open_storage(0);
    gt_storage_stats stats;
    uint8_t wire[GT_REC_SIZE];
    unsigned events = 0xFFu;
    make_wire(wire, 1);
    wire[16] ^= 0x01;

    CHECK_EQ(gt_storage_append(storage, wire, 0, &events), GT_ERR_CORRUPT);
    CHECK_EQ(events, 0);
    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    CHECK_EQ(stats.pending, 0);
    CHECK_EQ(stats.has_last_accepted, 0);
    gt_storage_close(storage);
}

/* ---- SAP-04: a duplicate seq is refused ----
 * The twin is still pending (not on disk), and is caught anyway. */
static void test_sap04_duplicate_refused(void)
{
    gt_storage *storage = open_storage(0);

    append_expect(storage, 1, GT_OK, 0);
    append_expect(storage, 1, GT_ERR_CORRUPT, GT_STORAGE_EV_SEQ_REJECTED);
    check_accepted(storage, 1, 1);
    gt_storage_close(storage);
}

/* ---- SAP-05: seqs are checked against the disk ----
 * The log ends at seq 7: a restarted hub sending 5 or 7 again is refused,
 * and 8 continues the log. */
static void test_sap05_checked_against_disk(void)
{
    gt_storage *storage = open_storage(7);

    append_expect(storage, 5, GT_ERR_CORRUPT, GT_STORAGE_EV_SEQ_REJECTED);
    append_expect(storage, 7, GT_ERR_CORRUPT, GT_STORAGE_EV_SEQ_REJECTED);
    check_accepted(storage, 0, 7);
    append_expect(storage, 8, GT_OK, 0);
    check_accepted(storage, 1, 8);
    gt_storage_close(storage);
}

/* ---- SAP-06: a skip stores a GAP and the record ----
 * 1 then 4: seqs 2-3 are missing, so a GAP goes in too (2 pending). */
static void test_sap06_skip_stores_gap(void)
{
    gt_storage *storage = open_storage(0);

    append_expect(storage, 1, GT_OK, 0);
    append_expect(storage, 4, GT_OK, GT_STORAGE_EV_RECOVERY_GAP);
    check_accepted(storage, 3, 4);
    gt_storage_close(storage);
}

/* ---- SAP-07: a skip after a restart is a gap ----
 * The log ends at 7 and the first record is 10: storage was killed with
 * records 8-9 unsynced (DESIGN.md recovery_gap row). */
static void test_sap07_skip_after_restart(void)
{
    gt_storage *storage = open_storage(7);

    append_expect(storage, 10, GT_OK, GT_STORAGE_EV_RECOVERY_GAP);
    check_accepted(storage, 2, 10);
    gt_storage_close(storage);
}

/* ---- SAP-08: a full buffer refuses ----
 * Capacity 8: the 9th record is refused and nothing changes. It is the
 * first refusal of this full episode, so it reports QUEUE_FULL. */
static void test_sap08_full_refuses(void)
{
    gt_storage *storage = open_storage(0);

    /* invariant: records 1 .. seq - 1 are pending */
    for (uint32_t seq = 1; seq <= TEST_CAPACITY; seq++) {
        append_expect(storage, seq, GT_OK, fill_events(seq));   /* seq records pending after it */
    }
    append_expect(storage, TEST_CAPACITY + 1, GT_ERR_FULL, GT_STORAGE_EV_QUEUE_FULL);
    check_accepted(storage, TEST_CAPACITY, TEST_CAPACITY);
    gt_storage_close(storage);
}

/* ---- SAP-09: no room for the GAP refuses both ----
 * 7 of 8 slots used, then a skip: the record would fit but its GAP would
 * not, so neither goes in (a QUEUE_FULL refusal, like SAP-08). The next
 * seq still fits. */
static void test_sap09_no_room_for_gap(void)
{
    gt_storage *storage = open_storage(0);

    /* invariant: records 1 .. seq - 1 are pending */
    for (uint32_t seq = 1; seq < TEST_CAPACITY; seq++) {
        append_expect(storage, seq, GT_OK, fill_events(seq));   /* seq records pending after it */
    }
    append_expect(storage, TEST_CAPACITY + 2, GT_ERR_FULL, GT_STORAGE_EV_QUEUE_FULL);
    check_accepted(storage, TEST_CAPACITY - 1, TEST_CAPACITY - 1);
    append_expect(storage, TEST_CAPACITY, GT_OK, fill_events(TEST_CAPACITY));
    check_accepted(storage, TEST_CAPACITY, TEST_CAPACITY);
    gt_storage_close(storage);
}

/* ---- SAP-10: a GAP from the hub is accepted ----
 * An all-sensors GAP record with the next seq is an ordinary record to
 * storage. */
static void test_sap10_hub_gap_accepted(void)
{
    gt_storage *storage = open_storage(0);
    const gt_record gap = {
        .type = GT_REC_GAP, .sensor_id = GT_SENSOR_ALL,
        .seq = 1, .timestamp_us = 7000, .value = 12,
    };
    uint8_t wire[GT_REC_SIZE];
    unsigned events = 0xFFu;
    CHECK_EQ(gt_record_encode(&gap, wire), GT_OK);

    CHECK_EQ(gt_storage_append(storage, wire, 0, &events), GT_OK);
    CHECK_EQ(events, 0);
    check_accepted(storage, 1, 1);
    gt_storage_close(storage);
}

/* ---- SAP-11: events_out may be NULL ---- */
static void test_sap11_null_events_ok(void)
{
    gt_storage *storage = open_storage(0);
    uint8_t wire[GT_REC_SIZE];
    make_wire(wire, 3);

    CHECK_EQ(gt_storage_append(storage, wire, 0, NULL), GT_OK);
    make_wire(wire, 3);
    CHECK_EQ(gt_storage_append(storage, wire, 0, NULL), GT_ERR_CORRUPT);
    check_accepted(storage, 1, 3);
    gt_storage_close(storage);
}

/* ---- SAP-12: NULL storage or record is rejected ---- */
static void test_sap12_null_args(void)
{
    gt_storage *storage = open_storage(0);
    uint8_t wire[GT_REC_SIZE];
    unsigned events = 0xFFu;
    make_wire(wire, 1);

    CHECK_EQ(gt_storage_append(NULL, wire, 0, &events), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_append(storage, NULL, 0, &events), GT_ERR_INVAL);
    gt_storage_close(storage);
}

static const test_case_t tests[] = {
    {"SAP-01", "the first record is accepted", test_sap01_first_record_accepted},
    {"SAP-02", "consecutive records are accepted", test_sap02_consecutive_accepted},
    {"SAP-03", "bad bytes are refused, no event", test_sap03_bad_bytes_refused},
    {"SAP-04", "a duplicate seq is refused", test_sap04_duplicate_refused},
    {"SAP-05", "seqs are checked against the disk", test_sap05_checked_against_disk},
    {"SAP-06", "a skip stores a GAP and the record", test_sap06_skip_stores_gap},
    {"SAP-07", "a skip after a restart is a gap", test_sap07_skip_after_restart},
    {"SAP-08", "a full buffer refuses", test_sap08_full_refuses},
    {"SAP-09", "no room for the GAP refuses both", test_sap09_no_room_for_gap},
    {"SAP-10", "a GAP from the hub is accepted", test_sap10_hub_gap_accepted},
    {"SAP-11", "events_out may be NULL", test_sap11_null_events_ok},
    {"SAP-12", "NULL storage or record is rejected", test_sap12_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
