/* Unit tests for the pending buffer's flush limits (src/storage_pending.c):
 * gt_pending_set_limits, and gt_pending_due following them.
 *
 * Purpose: prove N and T can be changed at run time, so DESIGN.md's (N, T)
 * batching experiment, (1, 1 s), (64, 10 s) and (64, 60 s), runs on one
 * build (PF-11). N = 1 must be due after a single record, T must be exact
 * to the ms, and a bad limit must change nothing. The defaults (64 records,
 * 60 s) are PND-05/06. No files, no clock (time is passed in). Run under
 * make test / asan / memcheck. Implements PNL-01 through PNL-05. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "storage_pending.h"

/* Add `count` records at now_ms, checking each add succeeds. The bytes
 * don't matter here: the limits only count records and compare times. */
static void add_records(gt_pending *pending, size_t count, uint64_t now_ms)
{
    uint8_t record[GT_REC_SIZE];
    memset(record, 0xA7, sizeof record);
    for (size_t added = 0; added < count; added++) {
        CHECK_EQ(gt_pending_add(pending, record, now_ms), GT_OK);
    }
}

/* ---- PNL-01: a new N replaces the old one ----
 * N = 8: due at 8 records, not 7. Then N = 16: those 8 are no longer due.
 * A batch as big as the whole buffer (32) is allowed. */
static void test_pnl01_new_n_replaces_old(void)
{
    gt_pending *pending = gt_pending_create(32);

    CHECK_EQ(gt_pending_set_limits(pending, 8, 60000), GT_OK);
    add_records(pending, 7, 0);
    CHECK_EQ(gt_pending_due(pending, 0), 0);
    add_records(pending, 1, 0);
    CHECK_EQ(gt_pending_due(pending, 0), 1);

    CHECK_EQ(gt_pending_set_limits(pending, 16, 60000), GT_OK);
    CHECK_EQ(gt_pending_due(pending, 0), 0);
    CHECK_EQ(gt_pending_set_limits(pending, 32, 60000), GT_OK);

    gt_pending_destroy(pending);
}

/* ---- PNL-02: N = 1 is due after one record ----
 * The experiment's (1, 1 s) point: one fsync per record. An empty buffer
 * is still never due. */
static void test_pnl02_n1_due_after_one(void)
{
    gt_pending *pending = gt_pending_create(8);

    CHECK_EQ(gt_pending_set_limits(pending, 1, 1000), GT_OK);
    CHECK_EQ(gt_pending_due(pending, 0), 0);
    add_records(pending, 1, 0);
    CHECK_EQ(gt_pending_due(pending, 0), 1);

    gt_pending_destroy(pending);
}

/* ---- PNL-03: T = 1 s is due at exactly 1000 ms ----
 * One record added at 5000 ms, N too big to trigger: not due at 5999, due
 * at 6000. The record was added under the default 60 s, so this also
 * shows a new T applies to records already waiting. */
static void test_pnl03_t1_due_at_1000_ms(void)
{
    gt_pending *pending = gt_pending_create(8);

    add_records(pending, 1, 5000);
    CHECK_EQ(gt_pending_set_limits(pending, 8, 1000), GT_OK);
    CHECK_EQ(gt_pending_due(pending, 5999), 0);
    CHECK_EQ(gt_pending_due(pending, 6000), 1);

    gt_pending_destroy(pending);
}

/* ---- PNL-04: bad limits are rejected, the old ones kept ----
 * A 0 batch, a 0 age, and a batch bigger than the buffer (101 of 100) are
 * each refused. The defaults still hold afterwards: 63 records at 0 ms
 * are not due before 60 s, and are due at 60 s or at the 64th record. */
static void test_pnl04_bad_limits_keep_old(void)
{
    gt_pending *pending = gt_pending_create(100);

    CHECK_EQ(gt_pending_set_limits(pending, 0, 1000), GT_ERR_INVAL);
    CHECK_EQ(gt_pending_set_limits(pending, 8, 0), GT_ERR_INVAL);
    CHECK_EQ(gt_pending_set_limits(pending, 101, 1000), GT_ERR_INVAL);

    add_records(pending, GT_BATCH_MAX_RECORDS - 1, 0);
    CHECK_EQ(gt_pending_due(pending, 0), 0);
    CHECK_EQ(gt_pending_due(pending, (uint64_t)GT_BATCH_MAX_AGE_S * 1000u - 1), 0);
    CHECK_EQ(gt_pending_due(pending, (uint64_t)GT_BATCH_MAX_AGE_S * 1000u), 1);
    add_records(pending, 1, 0);
    CHECK_EQ(gt_pending_due(pending, 0), 1);

    gt_pending_destroy(pending);
}

/* ---- PNL-05: a NULL buffer is rejected ---- */
static void test_pnl05_null_buffer(void)
{
    CHECK_EQ(gt_pending_set_limits(NULL, 8, 1000), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"PNL-01", "a new N replaces the old one", test_pnl01_new_n_replaces_old},
    {"PNL-02", "N = 1 is due after one record", test_pnl02_n1_due_after_one},
    {"PNL-03", "T = 1 s is due at exactly 1000 ms", test_pnl03_t1_due_at_1000_ms},
    {"PNL-04", "bad limits are rejected, old ones kept", test_pnl04_bad_limits_keep_old},
    {"PNL-05", "a NULL buffer is rejected", test_pnl05_null_buffer},
};

int main(void)
{
    return RUN_TESTS(tests);
}
