/* Unit tests for flush failures (src/storage.c): gt_storage_flush when a
 * truncate, write or fsync fails, and the retry after it.
 *
 * Purpose: prove DESIGN.md's "Never fail silently" row. Every failure is
 * reported with its event (storage_write_error / fsync_error) and marks
 * storage degraded once (storage_degraded). Every record stays pending.
 * The next flush truncates the segment back to the committed offset
 * before writing again, so torn bytes are cut off and no record is ever
 * written twice; the first good flush reports storage_recovered. Uses
 * tests/fake_file.h; "durable" means the fake's synced view. Run under
 * make test / asan / memcheck. Implements SFE-01 through SFE-09.
 *
 * What these tests cannot prove: real disk errors (ENOSPC, EROFS, an SD
 * card going bad), which are Mechanism D Step 10 and the on-Pi tests. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TODAY_NAME    "log-20261026.bin"
#define TEST_CAPACITY 8
#define RECORD_OFFSET(record_index) (GT_SEG_HEADER_SZ + (record_index) * GT_REC_SIZE)

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* Encode a light DATA record with `seq` into `wire`. */
static void make_wire(uint8_t wire[GT_REC_SIZE], uint32_t seq)
{
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .seq = seq, .timestamp_us = 4000 + seq, .value = 300 + seq,
    };
    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
}

/* Open storage on a fresh fake (empty directory, so today's segment is
 * created) and append records seq 1 .. record_count. */
static gt_storage *open_with_records(uint32_t record_count)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = NULL;
    CHECK_EQ(gt_storage_open_ops(ops, &fs, TODAY_US, TEST_CAPACITY, &storage, NULL), GT_OK);
    /* invariant: records 1 .. seq - 1 are pending */
    for (uint32_t seq = 1; seq <= record_count; seq++) {
        uint8_t wire[GT_REC_SIZE];
        make_wire(wire, seq);
        CHECK_EQ(gt_storage_append(storage, wire, 0, NULL), GT_OK);
    }
    return storage;
}

/* Flush and check the status and the events. */
static void flush_expect(gt_storage *storage, gt_status expected_status, unsigned expected_events)
{
    unsigned events = 0xFFu;   /* flush must reset it */
    CHECK_EQ(gt_storage_flush(storage, &events), expected_status);
    CHECK_EQ(events, expected_events);
}

/* Check pending, committed and degraded together. */
static void check_state(const gt_storage *storage, size_t pending, uint64_t committed,
                        int degraded)
{
    gt_storage_stats stats;
    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    CHECK_EQ(stats.pending, pending);
    CHECK(stats.committed_records == committed);
    CHECK_EQ(stats.degraded, degraded);
}

/* Check the segment holds exactly the header and records 1 .. count, in
 * both views: no torn bytes and no duplicates, and durable. */
static void check_segment_exact(size_t count)
{
    const fake_file *file = fake_fs_file(&fs, TODAY_NAME);
    CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    CHECK_EQ(file->len, RECORD_OFFSET(count));
    CHECK_EQ(file->synced_len, RECORD_OFFSET(count));
    if (file->synced_len != RECORD_OFFSET(count)) {
        return;
    }
    for (size_t record_index = 0; record_index < count; record_index++) {
        uint8_t expected[GT_REC_SIZE];
        make_wire(expected, (uint32_t)record_index + 1);
        CHECK(memcmp(&file->data[RECORD_OFFSET(record_index)], expected, GT_REC_SIZE) == 0);
        CHECK(memcmp(&file->synced_data[RECORD_OFFSET(record_index)], expected, GT_REC_SIZE) == 0);
    }
}

/* ---- SFE-01: a failed write marks degraded ---- */
static void test_sfe01_write_error_degrades(void)
{
    gt_storage *storage = open_with_records(2);
    fs.fail_pwrite_call = fs.pwrite_calls + 1;

    flush_expect(storage, GT_ERR_IO, GT_STORAGE_EV_WRITE_ERROR | GT_STORAGE_EV_DEGRADED);
    check_state(storage, 2, 0, 1);
    gt_storage_close(storage);
}

/* ---- SFE-02: a failed fsync marks degraded ---- */
static void test_sfe02_fsync_error_degrades(void)
{
    gt_storage *storage = open_with_records(2);
    fs.fail_fsync_call = fs.fsync_calls + 1;

    flush_expect(storage, GT_ERR_IO, GT_STORAGE_EV_FSYNC_ERROR | GT_STORAGE_EV_DEGRADED);
    check_state(storage, 2, 0, 1);
    gt_storage_close(storage);
}

/* ---- SFE-03: the retry cuts torn bytes first ----
 * The failed write left 10 bytes of record 1 in the file. A record is
 * appended while degraded. The retry truncates to the committed offset,
 * writes all 3 and fsyncs: exactly records 1-3, durable, no torn bytes. */
static void test_sfe03_retry_cuts_torn_bytes(void)
{
    gt_storage *storage = open_with_records(2);
    fs.fail_pwrite_call = fs.pwrite_calls + 1;
    fs.pwrite_torn_bytes = 10;
    flush_expect(storage, GT_ERR_IO, GT_STORAGE_EV_WRITE_ERROR | GT_STORAGE_EV_DEGRADED);
    CHECK_EQ(fake_fs_file(&fs, TODAY_NAME)->len, GT_SEG_HEADER_SZ + 10);

    uint8_t wire[GT_REC_SIZE];
    make_wire(wire, 3);
    CHECK_EQ(gt_storage_append(storage, wire, 0, NULL), GT_OK);
    int truncates_before = fs.ftruncate_calls;

    flush_expect(storage, GT_OK, GT_STORAGE_EV_RECOVERED);
    CHECK_EQ(fs.ftruncate_calls - truncates_before, 1);
    check_segment_exact(3);
    check_state(storage, 0, 3, 0);
    gt_storage_close(storage);
}

/* ---- SFE-04: a retry after fsync fails writes once ----
 * The write had fully reached the page cache before fsync failed. The
 * retry must not append a second copy after it. */
static void test_sfe04_retry_after_fsync_once(void)
{
    gt_storage *storage = open_with_records(2);
    fs.fail_fsync_call = fs.fsync_calls + 1;
    flush_expect(storage, GT_ERR_IO, GT_STORAGE_EV_FSYNC_ERROR | GT_STORAGE_EV_DEGRADED);
    CHECK_EQ(fake_fs_file(&fs, TODAY_NAME)->len, RECORD_OFFSET(2));

    flush_expect(storage, GT_OK, GT_STORAGE_EV_RECOVERED);
    check_segment_exact(2);
    check_state(storage, 0, 2, 0);
    gt_storage_close(storage);
}

/* ---- SFE-05: degraded is reported only once ----
 * A write failure, then an fsync failure on the retry: the second reports
 * its own error but not DEGRADED again. Then a good flush recovers. */
static void test_sfe05_degraded_reported_once(void)
{
    gt_storage *storage = open_with_records(2);
    fs.fail_pwrite_call = fs.pwrite_calls + 1;
    flush_expect(storage, GT_ERR_IO, GT_STORAGE_EV_WRITE_ERROR | GT_STORAGE_EV_DEGRADED);

    fs.fail_fsync_call = fs.fsync_calls + 1;
    flush_expect(storage, GT_ERR_IO, GT_STORAGE_EV_FSYNC_ERROR);
    check_state(storage, 2, 0, 1);

    flush_expect(storage, GT_OK, GT_STORAGE_EV_RECOVERED);
    check_segment_exact(2);
    check_state(storage, 0, 2, 0);
    gt_storage_close(storage);
}

/* ---- SFE-06: a failed truncate stops the retry ----
 * If the torn bytes can't be cut, writing after them would bury a partial
 * record in the log, so nothing is written: WRITE_ERROR, still degraded,
 * records kept. The next retry succeeds. */
static void test_sfe06_failed_truncate_stops_retry(void)
{
    gt_storage *storage = open_with_records(2);
    fs.fail_pwrite_call = fs.pwrite_calls + 1;
    fs.pwrite_torn_bytes = 5;
    flush_expect(storage, GT_ERR_IO, GT_STORAGE_EV_WRITE_ERROR | GT_STORAGE_EV_DEGRADED);

    fs.fail_ftruncate_call = fs.ftruncate_calls + 1;
    int pwrites_before = fs.pwrite_calls;
    flush_expect(storage, GT_ERR_IO, GT_STORAGE_EV_WRITE_ERROR);
    CHECK_EQ(fs.pwrite_calls, pwrites_before);
    check_state(storage, 2, 0, 1);

    flush_expect(storage, GT_OK, GT_STORAGE_EV_RECOVERED);
    check_segment_exact(2);
    gt_storage_close(storage);
}

/* ---- SFE-07: a healthy flush has no events ----
 * And no truncate: the extra call is only for the retry after a failure. */
static void test_sfe07_healthy_flush_quiet(void)
{
    gt_storage *storage = open_with_records(2);
    int truncates_before = fs.ftruncate_calls;

    flush_expect(storage, GT_OK, 0);
    CHECK_EQ(fs.ftruncate_calls, truncates_before);
    check_state(storage, 0, 2, 0);
    gt_storage_close(storage);
}

/* ---- SFE-08: committed work survives a later failure ----
 * Records 1-2 are committed. A failure flushing record 3 must not cut or
 * disturb them: the truncate goes back only to the committed offset. */
static void test_sfe08_committed_survives_failure(void)
{
    gt_storage *storage = open_with_records(2);
    flush_expect(storage, GT_OK, 0);

    uint8_t wire[GT_REC_SIZE];
    make_wire(wire, 3);
    CHECK_EQ(gt_storage_append(storage, wire, 0, NULL), GT_OK);
    fs.fail_pwrite_call = fs.pwrite_calls + 1;
    fs.pwrite_torn_bytes = 7;
    flush_expect(storage, GT_ERR_IO, GT_STORAGE_EV_WRITE_ERROR | GT_STORAGE_EV_DEGRADED);
    check_state(storage, 1, 2, 1);

    flush_expect(storage, GT_OK, GT_STORAGE_EV_RECOVERED);
    check_segment_exact(3);
    check_state(storage, 0, 3, 0);
    gt_storage_close(storage);
}

/* ---- SFE-09: events_out may be NULL ----
 * The failure is still handled (degraded, records kept). */
static void test_sfe09_null_events_ok(void)
{
    gt_storage *storage = open_with_records(1);
    fs.fail_fsync_call = fs.fsync_calls + 1;

    CHECK_EQ(gt_storage_flush(storage, NULL), GT_ERR_IO);
    check_state(storage, 1, 0, 1);
    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);
    check_state(storage, 0, 1, 0);
    gt_storage_close(storage);
}

static const test_case_t tests[] = {
    {"SFE-01", "a failed write marks degraded", test_sfe01_write_error_degrades},
    {"SFE-02", "a failed fsync marks degraded", test_sfe02_fsync_error_degrades},
    {"SFE-03", "the retry cuts torn bytes first", test_sfe03_retry_cuts_torn_bytes},
    {"SFE-04", "a retry after fsync fails writes once", test_sfe04_retry_after_fsync_once},
    {"SFE-05", "degraded is reported only once", test_sfe05_degraded_reported_once},
    {"SFE-06", "a failed truncate stops the retry", test_sfe06_failed_truncate_stops_retry},
    {"SFE-07", "a healthy flush has no events", test_sfe07_healthy_flush_quiet},
    {"SFE-08", "committed work survives a later failure", test_sfe08_committed_survives_failure},
    {"SFE-09", "events_out may be NULL", test_sfe09_null_events_ok},
};

int main(void)
{
    return RUN_TESTS(tests);
}
