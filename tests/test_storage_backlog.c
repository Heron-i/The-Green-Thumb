/* Unit tests for a filling pending buffer (src/storage.c): the backlog
 * thresholds, refused readings and the GAP that covers them, through
 * gt_storage_append, gt_storage_flush and gt_storage_read_stats.
 *
 * Purpose: prove DESIGN.md's "Preserve history through a storage outage"
 * row with a small test cap: storage_backlog is reported once at each of
 * 50%, 75% and 90% going up; the first refusal while full is reported
 * (storage_degraded) and every refusal is counted; and once space is back,
 * one GAP record whose seq is the first refused reading and whose count
 * equals the refused count goes in before the next record ("Outage longer
 * than the queue: 0 accepted readings lost, and the gap record's count
 * equals the refused count"). Uses tests/fake_file.h. Run under make test
 * / asan / memcheck. Implements SBL-01 through SBL-08.
 *
 * What these tests cannot prove: the real 1 MB cap (only a constant
 * here), or a disk that is full rather than a buffer (ENOSPC, deferred). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TODAY_NAME    "log-20261026.bin"
#define TEST_CAPACITY 20   /* thresholds at 10, 15 and 18 pending records */
#define RECORD_OFFSET(record_index) (GT_SEG_HEADER_SZ + (record_index) * GT_REC_SIZE)

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* Encode a soil DATA record with `seq` into `wire`. */
static void make_wire(uint8_t wire[GT_REC_SIZE], uint32_t seq)
{
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_SOIL,
        .seq = seq, .timestamp_us = 8000 + seq, .value = 900 + seq,
    };
    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
}

/* Open storage on a fresh fake (empty directory). */
static gt_storage *open_storage(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = NULL;
    gt_storage_open_args args = { .ops = ops, .ctx = &fs, .now_us = TODAY_US,
                                  .pending_capacity = TEST_CAPACITY };
    CHECK_EQ(gt_storage_open_ops(&args, &storage, NULL), GT_OK);
    return storage;
}

/* Append a record with `seq`; check the status and return the events. */
static unsigned append_seq(gt_storage *storage, uint32_t seq, gt_status expected_status)
{
    uint8_t wire[GT_REC_SIZE];
    unsigned events = 0xFFu;
    make_wire(wire, seq);
    CHECK_EQ(gt_storage_append(storage, wire, 0, &events), expected_status);
    return events;
}

/* Append seqs first .. last, all expected to be accepted; check that none
 * of them crossed a threshold or saw any other event. */
static void append_quiet(gt_storage *storage, uint32_t first, uint32_t last)
{
    /* invariant: seqs first .. seq - 1 were accepted with no event */
    for (uint32_t seq = first; seq <= last; seq++) {
        CHECK_EQ(append_seq(storage, seq, GT_OK), 0);
    }
}

/* Fill an empty buffer with seqs 1 .. TEST_CAPACITY, checking the three
 * threshold crossings land on the 10th, 15th and 18th record. */
static void fill_with_crossings(gt_storage *storage)
{
    append_quiet(storage, 1, 9);
    CHECK_EQ(append_seq(storage, 10, GT_OK), GT_STORAGE_EV_BACKLOG);
    append_quiet(storage, 11, 14);
    CHECK_EQ(append_seq(storage, 15, GT_OK), GT_STORAGE_EV_BACKLOG);
    append_quiet(storage, 16, 17);
    CHECK_EQ(append_seq(storage, 18, GT_OK), GT_STORAGE_EV_BACKLOG);
    append_quiet(storage, 19, TEST_CAPACITY);
}

/* Read the stats. */
static gt_storage_stats read_stats(const gt_storage *storage)
{
    gt_storage_stats stats;
    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    return stats;
}

/* ---- SBL-01: each threshold is reported once ----
 * 50% at the 10th record, 75% at the 15th, 90% at the 18th; the records
 * in between, and staying at 90%, are silent. */
static void test_sbl01_thresholds_once_each(void)
{
    gt_storage *storage = open_storage();

    append_quiet(storage, 1, 9);
    CHECK_EQ(read_stats(storage).backlog_percent, 0);
    CHECK_EQ(append_seq(storage, 10, GT_OK), GT_STORAGE_EV_BACKLOG);
    CHECK_EQ(read_stats(storage).backlog_percent, 50);
    append_quiet(storage, 11, 14);
    CHECK_EQ(append_seq(storage, 15, GT_OK), GT_STORAGE_EV_BACKLOG);
    CHECK_EQ(read_stats(storage).backlog_percent, 75);
    append_quiet(storage, 16, 17);
    CHECK_EQ(append_seq(storage, 18, GT_OK), GT_STORAGE_EV_BACKLOG);
    CHECK_EQ(read_stats(storage).backlog_percent, 90);
    append_quiet(storage, 19, TEST_CAPACITY);
    CHECK_EQ(read_stats(storage).backlog_percent, 90);
    gt_storage_close(storage);
}

/* ---- SBL-02: the first refusal reports QUEUE_FULL ----
 * Then two more refusals are silent, and all three are counted. */
static void test_sbl02_first_refusal_reported(void)
{
    gt_storage *storage = open_storage();
    fill_with_crossings(storage);

    CHECK_EQ(append_seq(storage, 21, GT_ERR_FULL), GT_STORAGE_EV_QUEUE_FULL);
    CHECK_EQ(append_seq(storage, 22, GT_ERR_FULL), 0);
    CHECK_EQ(append_seq(storage, 23, GT_ERR_FULL), 0);
    gt_storage_stats stats = read_stats(storage);
    CHECK(stats.refused == 3);
    CHECK_EQ(stats.pending, TEST_CAPACITY);
    CHECK_EQ(stats.last_accepted_seq, TEST_CAPACITY);
    gt_storage_close(storage);
}

/* ---- SBL-03: one GAP covers the refused readings ----
 * Refuse 21-23, flush, then accept 24: the log holds 1-20, one GAP with
 * seq 21 (the first refused) and count 3 (= refused), then 24. Every seq
 * the hub sent is either stored or covered: 0 readings lost silently. */
static void test_sbl03_gap_covers_refused(void)
{
    gt_storage *storage = open_storage();
    fill_with_crossings(storage);
    append_seq(storage, 21, GT_ERR_FULL);
    append_seq(storage, 22, GT_ERR_FULL);
    append_seq(storage, 23, GT_ERR_FULL);
    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);

    CHECK_EQ(append_seq(storage, 24, GT_OK), GT_STORAGE_EV_RECOVERY_GAP);
    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);

    const fake_file *file = fake_fs_file(&fs, TODAY_NAME);
    CHECK_EQ(file->synced_len, RECORD_OFFSET(TEST_CAPACITY + 2));
    gt_record gap, after;
    CHECK_EQ(gt_record_decode(&file->synced_data[RECORD_OFFSET(TEST_CAPACITY)], &gap), GT_OK);
    CHECK_EQ(gap.type, GT_REC_GAP);
    CHECK_EQ(gap.seq, 21);
    CHECK(gap.value == read_stats(storage).refused);
    CHECK(gap.timestamp_us == 8000 + TEST_CAPACITY);   /* seq 20's time: the gap started after it (PF-07) */
    CHECK_EQ(gt_record_decode(&file->synced_data[RECORD_OFFSET(TEST_CAPACITY + 1)], &after),
             GT_OK);
    CHECK_EQ(after.seq, 24);
    gt_storage_close(storage);
}

/* ---- SBL-04: a duplicate while full is not refused ----
 * The seq check comes first: an old seq is rejected as a duplicate, not
 * counted as a refused reading (it was never lost). */
static void test_sbl04_duplicate_while_full(void)
{
    gt_storage *storage = open_storage();
    fill_with_crossings(storage);

    CHECK_EQ(append_seq(storage, TEST_CAPACITY, GT_ERR_CORRUPT), GT_STORAGE_EV_SEQ_REJECTED);
    CHECK_EQ(append_seq(storage, 5, GT_ERR_CORRUPT), GT_STORAGE_EV_SEQ_REJECTED);
    CHECK(read_stats(storage).refused == 0);
    gt_storage_close(storage);
}

/* ---- SBL-05: a flush lowers the backlog silently ----
 * 90% before, 0% after, and the flush itself reports no event. */
static void test_sbl05_flush_lowers_silently(void)
{
    gt_storage *storage = open_storage();
    unsigned events = 0xFFu;
    append_quiet(storage, 1, 9);
    append_seq(storage, 10, GT_OK);
    append_quiet(storage, 11, 14);
    append_seq(storage, 15, GT_OK);
    append_quiet(storage, 16, 17);
    append_seq(storage, 18, GT_OK);
    CHECK_EQ(read_stats(storage).backlog_percent, 90);

    CHECK_EQ(gt_storage_flush(storage, &events), GT_OK);
    CHECK_EQ(events, 0);
    CHECK_EQ(read_stats(storage).backlog_percent, 0);
    gt_storage_close(storage);
}

/* ---- SBL-06: a second outage is reported again ----
 * After the first episode is drained, refilling crosses 50% again, and
 * the first refusal of the new episode reports QUEUE_FULL again. The
 * refused count keeps the total. */
static void test_sbl06_second_outage_reported(void)
{
    gt_storage *storage = open_storage();
    fill_with_crossings(storage);
    append_seq(storage, 21, GT_ERR_FULL);
    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);

    /* 22 skips 21: GAP + record = 2 pending; 23-29 make 9 */
    CHECK_EQ(append_seq(storage, 22, GT_OK), GT_STORAGE_EV_RECOVERY_GAP);
    append_quiet(storage, 23, 29);
    CHECK_EQ(append_seq(storage, 30, GT_OK), GT_STORAGE_EV_BACKLOG);   /* 10 pending */
    append_quiet(storage, 31, 34);
    CHECK_EQ(append_seq(storage, 35, GT_OK), GT_STORAGE_EV_BACKLOG);   /* 15 */
    append_quiet(storage, 36, 37);
    CHECK_EQ(append_seq(storage, 38, GT_OK), GT_STORAGE_EV_BACKLOG);   /* 18 */
    append_quiet(storage, 39, 40);                                     /* 20: full */

    CHECK_EQ(append_seq(storage, 41, GT_ERR_FULL), GT_STORAGE_EV_QUEUE_FULL);
    CHECK(read_stats(storage).refused == 2);
    gt_storage_close(storage);
}

/* ---- SBL-07: a GAP can cross a threshold ----
 * 9 pending, then a skip adds a GAP and the record: 11 pending, past 50%,
 * so both events come back together. */
static void test_sbl07_gap_crosses_threshold(void)
{
    gt_storage *storage = open_storage();
    append_quiet(storage, 1, 9);

    CHECK_EQ(append_seq(storage, 12, GT_OK), GT_STORAGE_EV_RECOVERY_GAP | GT_STORAGE_EV_BACKLOG);
    gt_storage_stats stats = read_stats(storage);
    CHECK_EQ(stats.pending, 11);
    CHECK_EQ(stats.backlog_percent, 50);
    gt_storage_close(storage);
}

/* ---- SBL-08: no room for a GAP is a counted refusal ----
 * 19 of 20 pending and a skip: the record would fit, its GAP would not.
 * Refused, counted and reported like any full refusal. */
static void test_sbl08_no_room_for_gap_counted(void)
{
    gt_storage *storage = open_storage();
    append_quiet(storage, 1, 9);
    append_seq(storage, 10, GT_OK);
    append_quiet(storage, 11, 14);
    append_seq(storage, 15, GT_OK);
    append_quiet(storage, 16, 17);
    append_seq(storage, 18, GT_OK);
    append_quiet(storage, 19, 19);

    CHECK_EQ(append_seq(storage, 25, GT_ERR_FULL), GT_STORAGE_EV_QUEUE_FULL);
    gt_storage_stats stats = read_stats(storage);
    CHECK(stats.refused == 1);
    CHECK_EQ(stats.pending, 19);
    gt_storage_close(storage);
}

static const test_case_t tests[] = {
    {"SBL-01", "each threshold is reported once", test_sbl01_thresholds_once_each},
    {"SBL-02", "the first refusal reports QUEUE_FULL", test_sbl02_first_refusal_reported},
    {"SBL-03", "one GAP covers the refused readings", test_sbl03_gap_covers_refused},
    {"SBL-04", "a duplicate while full is not refused", test_sbl04_duplicate_while_full},
    {"SBL-05", "a flush lowers the backlog silently", test_sbl05_flush_lowers_silently},
    {"SBL-06", "a second outage is reported again", test_sbl06_second_outage_reported},
    {"SBL-07", "a GAP can cross a threshold", test_sbl07_gap_crosses_threshold},
    {"SBL-08", "no room for a GAP is a counted refusal", test_sbl08_no_room_for_gap_counted},
};

int main(void)
{
    return RUN_TESTS(tests);
}
