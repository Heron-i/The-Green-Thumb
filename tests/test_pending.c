/* Unit tests for the pending buffer (src/storage_pending.c): create, add,
 * count, due, peek, commit and destroy.
 *
 * Purpose: prove the buffer keeps records in arrival order, refuses a
 * record when full without changing anything, says a flush is due at
 * exactly N = 64 records or T = 60 s (DESIGN.md "How writes are batched"),
 * and drops records only when a flush commits them, so a failed flush
 * loses nothing. Records here are plain byte patterns: the buffer copies
 * bytes and never decodes them. No files, no clock (time is passed in).
 * Run under make test / asan / memcheck. Implements PND-01 through PND-11.
 *
 * What these tests cannot prove: locking between the rx and flush threads
 * (that is gt_storage's job, Mechanism D Step 7), and the 1 MB capacity
 * itself, which is only a constant here. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "storage_pending.h"

#define AGE_LIMIT_MS ((uint64_t)GT_BATCH_MAX_AGE_S * 1000u)

/* Fill `record` with a byte pattern unique to `record_number`, so a record
 * that is lost, repeated or reordered shows up in a memcmp. */
static void make_record(uint8_t record[GT_REC_SIZE], uint32_t record_number)
{
    for (size_t byte_index = 0; byte_index < GT_REC_SIZE; byte_index++) {
        record[byte_index] = (uint8_t)(record_number * 31u + byte_index);
    }
}

/* Add records first_number .. first_number + count - 1 at now_ms, checking
 * each add succeeds. */
static void add_records(gt_pending *pending, uint32_t first_number, size_t count,
                        uint64_t now_ms)
{
    for (size_t added = 0; added < count; added++) {
        uint8_t record[GT_REC_SIZE];
        make_record(record, first_number + (uint32_t)added);
        CHECK_EQ(gt_pending_add(pending, record, now_ms), GT_OK);
    }
}

/* Check the buffer holds exactly records first_number .. first_number +
 * count - 1, oldest first. */
static void check_holds(const gt_pending *pending, uint32_t first_number, size_t count)
{
    size_t peek_count = 999;
    const uint8_t *block = gt_pending_peek(pending, &peek_count);

    CHECK_EQ(gt_pending_count(pending), count);
    CHECK_EQ(peek_count, count);
    CHECK(block != NULL);
    if (block == NULL || peek_count != count) {
        return;
    }
    for (size_t record_index = 0; record_index < count; record_index++) {
        uint8_t expected[GT_REC_SIZE];
        make_record(expected, first_number + (uint32_t)record_index);
        CHECK(memcmp(&block[record_index * GT_REC_SIZE], expected, GT_REC_SIZE) == 0);
    }
}

/* ---- PND-01: create rejects a capacity of 0 ----
 * And destroy(NULL) is a safe no-op. */
static void test_pnd01_create_rejects_zero(void)
{
    CHECK(gt_pending_create(0) == NULL);
    gt_pending_destroy(NULL);
}

/* ---- PND-02: a new buffer is empty and not due ----
 * Not due even at a huge time: an empty buffer has nothing to flush. */
static void test_pnd02_new_buffer_empty(void)
{
    gt_pending *pending = gt_pending_create(8);
    CHECK(pending != NULL);

    CHECK_EQ(gt_pending_count(pending), 0);
    CHECK_EQ(gt_pending_due(pending, 0), 0);
    CHECK_EQ(gt_pending_due(pending, UINT64_MAX), 0);

    gt_pending_destroy(pending);
}

/* ---- PND-03: peek returns records in order ----
 * And add copies the bytes: changing the caller's array afterwards does
 * not change what is stored. */
static void test_pnd03_peek_in_order(void)
{
    gt_pending *pending = gt_pending_create(8);
    uint8_t record[GT_REC_SIZE];

    make_record(record, 1);
    CHECK_EQ(gt_pending_add(pending, record, 0), GT_OK);
    memset(record, 0, sizeof record);
    add_records(pending, 2, 2, 0);

    check_holds(pending, 1, 3);
    gt_pending_destroy(pending);
}

/* ---- PND-04: a full buffer refuses, unchanged ----
 * GT_ERR_FULL, and the records already held are exactly as before. */
static void test_pnd04_full_refuses(void)
{
    gt_pending *pending = gt_pending_create(3);
    uint8_t record[GT_REC_SIZE];
    add_records(pending, 1, 3, 0);

    make_record(record, 4);
    CHECK_EQ(gt_pending_add(pending, record, 0), GT_ERR_FULL);

    check_holds(pending, 1, 3);
    gt_pending_destroy(pending);
}

/* ---- PND-05: due at 64 records ----
 * 63 records are not due, 64 are, with no time passing at all. */
static void test_pnd05_due_at_64_records(void)
{
    gt_pending *pending = gt_pending_create(100);

    add_records(pending, 1, GT_BATCH_MAX_RECORDS - 1, 0);
    CHECK_EQ(gt_pending_due(pending, 0), 0);
    add_records(pending, 100, 1, 0);
    CHECK_EQ(gt_pending_due(pending, 0), 1);

    gt_pending_destroy(pending);
}

/* ---- PND-06: due 60 s after the oldest record ----
 * The clock starts at the first add; a later add does not restart it. */
static void test_pnd06_due_after_60_s(void)
{
    gt_pending *pending = gt_pending_create(8);

    add_records(pending, 1, 1, 1000);
    add_records(pending, 2, 1, 30000);
    CHECK_EQ(gt_pending_due(pending, 1000 + AGE_LIMIT_MS - 1), 0);
    CHECK_EQ(gt_pending_due(pending, 1000 + AGE_LIMIT_MS), 1);

    gt_pending_destroy(pending);
}

/* ---- PND-07: commit drops only the oldest ----
 * Records added after the peek stay, in order, and the freed space can be
 * used again. */
static void test_pnd07_commit_drops_oldest(void)
{
    gt_pending *pending = gt_pending_create(3);
    size_t peek_count = 0;
    add_records(pending, 1, 2, 0);

    CHECK(gt_pending_peek(pending, &peek_count) != NULL);
    CHECK_EQ(peek_count, 2);
    add_records(pending, 3, 1, 0);   /* arrives during the flush */
    CHECK_EQ(gt_pending_commit(pending, peek_count), GT_OK);

    check_holds(pending, 3, 1);
    add_records(pending, 4, 2, 0);
    check_holds(pending, 3, 3);
    gt_pending_destroy(pending);
}

/* ---- PND-08: committing everything resets the clock ----
 * An empty buffer is not due, and the next add starts a fresh 60 s. */
static void test_pnd08_commit_all_resets_clock(void)
{
    gt_pending *pending = gt_pending_create(8);
    add_records(pending, 1, 2, 0);

    CHECK_EQ(gt_pending_commit(pending, 2), GT_OK);
    CHECK_EQ(gt_pending_count(pending), 0);
    CHECK_EQ(gt_pending_due(pending, 200000), 0);

    add_records(pending, 3, 1, 200000);
    CHECK_EQ(gt_pending_due(pending, 200000 + AGE_LIMIT_MS - 1), 0);
    CHECK_EQ(gt_pending_due(pending, 200000 + AGE_LIMIT_MS), 1);
    gt_pending_destroy(pending);
}

/* ---- PND-09: leftovers keep their own add time ----
 * A record that arrived during the flush (t = 10) is due 60 s after it
 * arrived, not 60 s after the commit, so the 62 s commit latency target
 * holds even when the flush is slow. */
static void test_pnd09_leftovers_keep_add_time(void)
{
    gt_pending *pending = gt_pending_create(8);
    add_records(pending, 1, 2, 0);
    add_records(pending, 3, 1, 10);

    CHECK_EQ(gt_pending_commit(pending, 2), GT_OK);
    CHECK_EQ(gt_pending_due(pending, 10 + AGE_LIMIT_MS - 1), 0);
    CHECK_EQ(gt_pending_due(pending, 10 + AGE_LIMIT_MS), 1);
    gt_pending_destroy(pending);
}

/* ---- PND-10: committing too many is rejected ----
 * GT_ERR_INVAL and nothing is dropped: a caller bug must not lose data. */
static void test_pnd10_commit_too_many(void)
{
    gt_pending *pending = gt_pending_create(8);
    add_records(pending, 1, 2, 0);

    CHECK_EQ(gt_pending_commit(pending, 3), GT_ERR_INVAL);
    check_holds(pending, 1, 2);
    gt_pending_destroy(pending);
}

/* ---- PND-11: NULL arguments are handled ----
 * Errors for add/commit, 0 or NULL for the queries, no crash. */
static void test_pnd11_null_args(void)
{
    gt_pending *pending = gt_pending_create(8);
    uint8_t record[GT_REC_SIZE];
    size_t peek_count = 999;
    make_record(record, 1);

    CHECK_EQ(gt_pending_add(NULL, record, 0), GT_ERR_INVAL);
    CHECK_EQ(gt_pending_add(pending, NULL, 0), GT_ERR_INVAL);
    CHECK_EQ(gt_pending_count(pending), 0);
    CHECK_EQ(gt_pending_count(NULL), 0);
    CHECK_EQ(gt_pending_due(NULL, UINT64_MAX), 0);
    CHECK(gt_pending_peek(NULL, &peek_count) == NULL);
    CHECK_EQ(peek_count, 0);
    CHECK(gt_pending_peek(pending, NULL) == NULL);
    CHECK_EQ(gt_pending_commit(NULL, 0), GT_ERR_INVAL);

    gt_pending_destroy(pending);
}

static const test_case_t tests[] = {
    {"PND-01", "create rejects a capacity of 0", test_pnd01_create_rejects_zero},
    {"PND-02", "a new buffer is empty and not due", test_pnd02_new_buffer_empty},
    {"PND-03", "peek returns records in order", test_pnd03_peek_in_order},
    {"PND-04", "a full buffer refuses, unchanged", test_pnd04_full_refuses},
    {"PND-05", "due at 64 records", test_pnd05_due_at_64_records},
    {"PND-06", "due 60 s after the oldest record", test_pnd06_due_after_60_s},
    {"PND-07", "commit drops only the oldest", test_pnd07_commit_drops_oldest},
    {"PND-08", "committing everything resets the clock", test_pnd08_commit_all_resets_clock},
    {"PND-09", "leftovers keep their own add time", test_pnd09_leftovers_keep_add_time},
    {"PND-10", "committing too many is rejected", test_pnd10_commit_too_many},
    {"PND-11", "NULL arguments are handled", test_pnd11_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
