/* Unit tests for storage's flush limits (src/storage.c):
 * gt_storage_set_batch, seen through gt_storage_append and
 * gt_storage_flush_due.
 *
 * Purpose: prove storage_main's -n / -t options reach the pending buffer,
 * so DESIGN.md's (N, T) experiment runs on one build (PF-11): after
 * N = 1 a single accepted record makes a flush due, T is exact to the ms,
 * and a bad value changes nothing. The pending buffer's own limit rules
 * are PNL; this file only checks storage passes them through. Uses
 * tests/fake_file.h. Run under make test / asan / memcheck. Implements
 * SBT-01 through SBT-04. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TEST_CAPACITY 8

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* Open storage on a fresh, empty fake directory (so any first seq is
 * accepted), with a pending buffer of TEST_CAPACITY records. */
static gt_storage *open_storage(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = NULL;
    gt_storage_open_args args = { .ops = ops, .ctx = &fs, .now_us = TODAY_US,
                                  .pending_capacity = TEST_CAPACITY };
    CHECK_EQ(gt_storage_open_ops(&args, &storage, ), );
    return storage;
}

/* Append a light DATA record with `seq` at now_ms, checking it is
 * accepted. */
static void append_at(gt_storage *storage, uint32_t seq, uint64_t now_ms)
{
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .seq = seq, .timestamp_us = 5000 + seq, .value = 100 + seq,
    };
    uint8_t wire[GT_REC_SIZE];
    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
    CHECK_EQ(gt_storage_append(storage, wire, now_ms, NULL), GT_OK);
}

/* ---- SBT-01: N = 1 makes one record due ----
 * Not due while empty, due after the first accepted record, with no time
 * passing: the experiment's one-fsync-per-record point. */
static void test_sbt01_n1_one_record_due(void)
{
    gt_storage *storage = open_storage();

    CHECK_EQ(gt_storage_set_batch(storage, 1, 1), GT_OK);
    CHECK_EQ(gt_storage_flush_due(storage, 0), 0);
    append_at(storage, 1, 0);
    CHECK_EQ(gt_storage_flush_due(storage, 0), 1);

    gt_storage_close(storage);
}

/* ---- SBT-02: T = 1 s is due at exactly 1000 ms ----
 * One record at 5000 ms, N set to the whole buffer so only T can trigger:
 * not due at 5999, due at 6000. T is given in seconds and kept in ms. */
static void test_sbt02_t1_due_at_1000_ms(void)
{
    gt_storage *storage = open_storage();

    CHECK_EQ(gt_storage_set_batch(storage, TEST_CAPACITY, 1), GT_OK);
    append_at(storage, 1, 5000);
    CHECK_EQ(gt_storage_flush_due(storage, 5999), 0);
    CHECK_EQ(gt_storage_flush_due(storage, 6000), 1);

    gt_storage_close(storage);
}

/* ---- SBT-03: bad values are rejected, the defaults kept ----
 * A 0 batch, a 0 age, and a batch bigger than the buffer (9 of 8) are
 * each refused. The default T (60 s) still holds afterwards; the default
 * N (64) can't trigger in an 8-record buffer, so T is what shows it. */
static void test_sbt03_bad_values_keep_defaults(void)
{
    gt_storage *storage = open_storage();

    CHECK_EQ(gt_storage_set_batch(storage, 0, 1), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_set_batch(storage, 1, 0), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_set_batch(storage, TEST_CAPACITY + 1, 1), GT_ERR_INVAL);

    append_at(storage, 1, 0);
    CHECK_EQ(gt_storage_flush_due(storage, (uint64_t)GT_BATCH_MAX_AGE_S * 1000u - 1), 0);
    CHECK_EQ(gt_storage_flush_due(storage, (uint64_t)GT_BATCH_MAX_AGE_S * 1000u), 1);

    gt_storage_close(storage);
}

/* ---- SBT-04: NULL storage is rejected ---- */
static void test_sbt04_null_storage(void)
{
    CHECK_EQ(gt_storage_set_batch(NULL, 1, 1), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"SBT-01", "N = 1 makes one record due", test_sbt01_n1_one_record_due},
    {"SBT-02", "T = 1 s is due at exactly 1000 ms", test_sbt02_t1_due_at_1000_ms},
    {"SBT-03", "bad values are rejected, defaults kept", test_sbt03_bad_values_keep_defaults},
    {"SBT-04", "NULL storage is rejected", test_sbt04_null_storage},
};

int main(void)
{
    return RUN_TESTS(tests);
}
