/* Unit tests for committing records (src/storage.c): gt_storage_flush and
 * gt_storage_flush_due.
 *
 * Purpose: prove a flush writes exactly the accepted bytes, in order, at
 * the end of the active segment and fsyncs them, so they are durable once
 * it returns (DESIGN.md "A record is committed once fsync() returns"),
 * and that nothing is durable before then. A GAP made by append lands
 * just before its record. An empty flush touches nothing, and a failed
 * write or fsync keeps every record pending. flush_due follows the N = 64
 * / T = 60 s rule. Uses tests/fake_file.h; durability is checked in the
 * fake's synced view. A GAP carries the last accepted record's time, also
 * after a restart. Run under make test / asan / memcheck. Implements
 * SFL-01 through SFL-11.
 *
 * What these tests cannot prove: a flush racing appends on another thread
 * (these are single-threaded; an append between two flushes stands in),
 * the retry after a failed flush (Mechanism D Step 8), or real ext4
 * (Step 12a). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TODAY_NAME    "log-20261026.bin"
#define TEST_CAPACITY 8
#define AGE_LIMIT_MS  ((uint64_t)GT_BATCH_MAX_AGE_S * 1000u)
#define RECORD_OFFSET(record_index) (GT_SEG_HEADER_SZ + (record_index) * GT_REC_SIZE)

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* Encode a soil DATA record with `seq` into `wire`. */
static void make_wire(uint8_t wire[GT_REC_SIZE], uint32_t seq)
{
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_SOIL,
        .seq = seq, .timestamp_us = 9000 + seq, .value = 700 + seq,
    };
    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
}

/* Open storage over the fake (already set up by the caller) with room
 * for `capacity` pending records. */
static gt_storage *open_storage(const gt_file_ops *ops, size_t capacity)
{
    gt_storage *storage = NULL;
    CHECK_EQ(gt_storage_open_ops(ops, &fs, TODAY_US, capacity, &storage, NULL), GT_OK);
    return storage;
}

/* Append a record with `seq` at now_ms and check it is accepted. */
static void append_ok(gt_storage *storage, uint32_t seq, uint64_t now_ms)
{
    uint8_t wire[GT_REC_SIZE];
    make_wire(wire, seq);
    CHECK_EQ(gt_storage_append(storage, wire, now_ms, NULL), GT_OK);
}

/* Check record number `record_index` of `bytes` is the record with `seq`
 * that make_wire builds, byte for byte. */
static void check_record_at(const uint8_t *bytes, size_t record_index, uint32_t seq)
{
    uint8_t expected[GT_REC_SIZE];
    make_wire(expected, seq);
    CHECK(memcmp(&bytes[RECORD_OFFSET(record_index)], expected, GT_REC_SIZE) == 0);
}

/* Check the active segment holds the header and exactly records with
 * seqs first_seq .. first_seq + count - 1, in both the current and the
 * synced view: written and durable. */
static void check_segment_holds(uint32_t first_seq, size_t count)
{
    const fake_file *file = fake_fs_file(&fs, TODAY_NAME);
    CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    CHECK_EQ(file->len, RECORD_OFFSET(count));
    CHECK_EQ(file->synced_len, RECORD_OFFSET(count));
    if (file->len != RECORD_OFFSET(count) || file->synced_len != RECORD_OFFSET(count)) {
        return;
    }
    for (size_t record_index = 0; record_index < count; record_index++) {
        check_record_at(file->data, record_index, first_seq + (uint32_t)record_index);
        check_record_at(file->synced_data, record_index, first_seq + (uint32_t)record_index);
    }
}

/* Check the pending count and the committed counters. */
static void check_committed(const gt_storage *storage, size_t pending, uint64_t committed,
                            uint32_t last_committed_seq)
{
    gt_storage_stats stats;
    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    CHECK_EQ(stats.pending, pending);
    CHECK(stats.committed_records == committed);
    CHECK_EQ(stats.has_last_committed, committed > 0);
    CHECK_EQ(stats.last_committed_seq, last_committed_seq);
}

/* ---- SFL-01: flushed records are on disk ----
 * Three records, one pwrite and one fsync, byte for byte in the synced
 * view: what a power cut right after flush returns would leave. */
static void test_sfl01_flushed_records_durable(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, TEST_CAPACITY);
    append_ok(storage, 1, 0);
    append_ok(storage, 2, 0);
    append_ok(storage, 3, 0);
    int pwrites_before = fs.pwrite_calls, fsyncs_before = fs.fsync_calls;

    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);
    CHECK_EQ(fs.pwrite_calls - pwrites_before, 1);
    CHECK_EQ(fs.fsync_calls - fsyncs_before, 1);
    check_segment_holds(1, 3);
    check_committed(storage, 0, 3, 3);
    gt_storage_close(storage);
}

/* ---- SFL-02: nothing is written before a flush ----
 * Accepted records live only in memory until flush: the file is still
 * just its header, in both views. */
static void test_sfl02_nothing_before_flush(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, TEST_CAPACITY);
    append_ok(storage, 1, 0);
    append_ok(storage, 2, 0);

    check_segment_holds(1, 0);
    check_committed(storage, 2, 0, 0);
    gt_storage_close(storage);
}

/* ---- SFL-03: a GAP lands before its record ----
 * 1 then 4: the file holds 1, then the GAP for seqs 2-3 (all sensors,
 * stamped with record 1's time, when the gap could first have started:
 * PF-07), then 4. */
static void test_sfl03_gap_before_record(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, TEST_CAPACITY);
    append_ok(storage, 1, 0);
    append_ok(storage, 4, 0);

    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);
    const fake_file *file = fake_fs_file(&fs, TODAY_NAME);
    CHECK_EQ(file->synced_len, RECORD_OFFSET(3));
    check_record_at(file->synced_data, 0, 1);
    check_record_at(file->synced_data, 2, 4);

    gt_record gap;
    CHECK_EQ(gt_record_decode(&file->synced_data[RECORD_OFFSET(1)], &gap), GT_OK);
    CHECK_EQ(gap.type, GT_REC_GAP);
    CHECK_EQ(gap.sensor_id, GT_SENSOR_ALL);
    CHECK_EQ(gap.seq, 2);
    CHECK_EQ(gap.value, 2);
    CHECK(gap.timestamp_us == 9000 + 1);
    check_committed(storage, 0, 3, 4);
    gt_storage_close(storage);
}

/* ---- SFL-11: a GAP after a restart has the last stored time ----
 * Storage commits seq 7 (stamped 9007) and closes without its shutdown
 * marker, as a kill -9 leaves it. Reopened, it gets seq 10: the GAP for
 * 8-9 carries 9007, the time of the last record on disk, which open
 * learned from recovery. The missing records were lost after it (PF-07). */
static void test_sfl11_gap_after_restart(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, TEST_CAPACITY);
    append_ok(storage, 7, 0);
    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);
    gt_storage_close(storage);

    storage = open_storage(ops, TEST_CAPACITY);
    append_ok(storage, 10, 0);
    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);
    const fake_file *file = fake_fs_file(&fs, TODAY_NAME);
    CHECK_EQ(file->synced_len, RECORD_OFFSET(3));
    gt_record gap;
    CHECK_EQ(gt_record_decode(&file->synced_data[RECORD_OFFSET(1)], &gap), GT_OK);
    CHECK_EQ(gap.type, GT_REC_GAP);
    CHECK_EQ(gap.seq, 8);
    CHECK_EQ(gap.value, 2);
    CHECK(gap.timestamp_us == 9000 + 7);   /* u64: too big for CHECK_EQ's long long */
    gt_storage_close(storage);
}

/* ---- SFL-04: an empty flush touches nothing ----
 * No pwrite and no fsync: a flush thread waking with nothing to do must
 * not wear the card. */
static void test_sfl04_empty_flush_noop(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, TEST_CAPACITY);
    int pwrites_before = fs.pwrite_calls, fsyncs_before = fs.fsync_calls;

    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);
    CHECK_EQ(fs.pwrite_calls, pwrites_before);
    CHECK_EQ(fs.fsync_calls, fsyncs_before);
    check_committed(storage, 0, 0, 0);
    gt_storage_close(storage);
}

/* ---- SFL-05: later flushes append in order ----
 * A record appended after a flush waits for the next one (the
 * single-threaded stand-in for "appended during a flush"), then lands
 * right after the first batch. */
static void test_sfl05_flushes_append_in_order(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, TEST_CAPACITY);
    append_ok(storage, 1, 0);
    append_ok(storage, 2, 0);
    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);

    append_ok(storage, 3, 0);
    check_committed(storage, 1, 2, 2);
    check_segment_holds(1, 2);

    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);
    check_segment_holds(1, 3);
    check_committed(storage, 0, 3, 3);
    gt_storage_close(storage);
}

/* ---- SFL-06: an existing segment grows at its end ----
 * Records 1-3 are on disk from an earlier run; record 4 goes right after
 * them, and the old bytes are not rewritten. */
static void test_sfl06_existing_segment_grows(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    uint8_t segment[RECORD_OFFSET(3)];
    const gt_seg_header header = { .created_at_us = 1000 };
    CHECK_EQ(gt_seg_header_encode(&header, segment), GT_OK);
    /* invariant: records seq 1 .. seq - 1 are encoded after the header */
    for (uint32_t seq = 1; seq <= 3; seq++) {
        make_wire(&segment[RECORD_OFFSET(seq - 1)], seq);
    }
    CHECK_EQ(fake_fs_put(&fs, TODAY_NAME, segment, sizeof segment), GT_OK);
    gt_storage *storage = open_storage(ops, TEST_CAPACITY);
    append_ok(storage, 4, 0);

    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);
    check_segment_holds(1, 4);
    CHECK(memcmp(fake_fs_file(&fs, TODAY_NAME)->synced_data, segment, sizeof segment) == 0);
    check_committed(storage, 0, 1, 4);
    gt_storage_close(storage);
}

/* ---- SFL-07: a failed write keeps every record ----
 * GT_ERR_IO, both records still pending, nothing counted as committed. */
static void test_sfl07_failed_write_keeps_records(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, TEST_CAPACITY);
    append_ok(storage, 1, 0);
    append_ok(storage, 2, 0);
    fs.fail_pwrite_call = fs.pwrite_calls + 1;

    CHECK_EQ(gt_storage_flush(storage, NULL), GT_ERR_IO);
    check_committed(storage, 2, 0, 0);
    gt_storage_close(storage);
}

/* ---- SFL-08: a failed fsync keeps every record ----
 * The bytes may be in the page cache, but without fsync they are not
 * committed: still pending, nothing counted. */
static void test_sfl08_failed_fsync_keeps_records(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, TEST_CAPACITY);
    append_ok(storage, 1, 0);
    append_ok(storage, 2, 0);
    fs.fail_fsync_call = fs.fsync_calls + 1;

    CHECK_EQ(gt_storage_flush(storage, NULL), GT_ERR_IO);
    check_committed(storage, 2, 0, 0);
    CHECK_EQ(fake_fs_file(&fs, TODAY_NAME)->synced_len, GT_SEG_HEADER_SZ);
    gt_storage_close(storage);
}

/* ---- SFL-09: flush_due follows N and T ----
 * One record added at t = 1000 is due at exactly 1000 + 60 s; 64 records
 * are due at once. After a flush nothing is due. */
static void test_sfl09_flush_due_rules(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, GT_BATCH_MAX_RECORDS + 4);

    CHECK_EQ(gt_storage_flush_due(storage, UINT64_MAX), 0);
    append_ok(storage, 1, 1000);
    CHECK_EQ(gt_storage_flush_due(storage, 1000 + AGE_LIMIT_MS - 1), 0);
    CHECK_EQ(gt_storage_flush_due(storage, 1000 + AGE_LIMIT_MS), 1);
    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);
    CHECK_EQ(gt_storage_flush_due(storage, UINT64_MAX), 0);

    /* invariant: records 2 .. seq - 1 are pending */
    for (uint32_t seq = 2; seq < 2 + GT_BATCH_MAX_RECORDS - 1; seq++) {
        append_ok(storage, seq, 5000);
    }
    CHECK_EQ(gt_storage_flush_due(storage, 5000), 0);
    append_ok(storage, 2 + GT_BATCH_MAX_RECORDS - 1, 5000);
    CHECK_EQ(gt_storage_flush_due(storage, 5000), 1);
    gt_storage_close(storage);
}

/* ---- SFL-10: NULL storage is rejected ---- */
static void test_sfl10_null_args(void)
{
    CHECK_EQ(gt_storage_flush(NULL, NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_flush_due(NULL, UINT64_MAX), 0);
}

static const test_case_t tests[] = {
    {"SFL-01", "flushed records are on disk", test_sfl01_flushed_records_durable},
    {"SFL-02", "nothing is written before a flush", test_sfl02_nothing_before_flush},
    {"SFL-03", "a GAP lands before its record", test_sfl03_gap_before_record},
    {"SFL-04", "an empty flush touches nothing", test_sfl04_empty_flush_noop},
    {"SFL-05", "later flushes append in order", test_sfl05_flushes_append_in_order},
    {"SFL-06", "an existing segment grows at its end", test_sfl06_existing_segment_grows},
    {"SFL-07", "a failed write keeps every record", test_sfl07_failed_write_keeps_records},
    {"SFL-08", "a failed fsync keeps every record", test_sfl08_failed_fsync_keeps_records},
    {"SFL-09", "flush_due follows N and T", test_sfl09_flush_due_rules},
    {"SFL-10", "NULL storage is rejected", test_sfl10_null_args},
    {"SFL-11", "a GAP after a restart has the last time", test_sfl11_gap_after_restart},
};

int main(void)
{
    return RUN_TESTS(tests);
}
