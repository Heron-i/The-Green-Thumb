/* Unit tests for the sequence guard (src/storage_seq.c): gt_seq_check and
 * gt_seq_gap_record.
 *
 * Purpose: prove storage accepts the next seq, rejects a repeated or
 * backwards one (DESIGN.md "storage rejects any seq <= last committed"),
 * and turns a skip into one GAP record covering exactly the missing seqs
 * (DESIGN.md "storage writes one GAP record covering the missing range"),
 * so the log's seq still increases and every missing seq is explained.
 * Pure logic: no files, no state. Run under make test / asan / memcheck.
 * Implements SQG-01 through SQG-09.
 *
 * What these tests cannot prove: that storage keeps last_accepted up to
 * date and writes the GAP before the record. That arrives with
 * gt_storage append (Mechanism D Step 7). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "log_record.h"
#include "storage_seq.h"

/* ---- SQG-01: the first record is always accepted ----
 * An empty log has no last seq, so any seq can start it. */
static void test_sqg01_first_record_accepted(void)
{
    CHECK_EQ(gt_seq_check(0, 0, 0), GT_SEQ_ACCEPT);
    CHECK_EQ(gt_seq_check(0, 0, 7), GT_SEQ_ACCEPT);
    CHECK_EQ(gt_seq_check(0, 0, UINT32_MAX), GT_SEQ_ACCEPT);
}

/* ---- SQG-02: last_accepted is ignored with no last ----
 * A leftover value must not reject the first record. */
static void test_sqg02_no_last_ignores_value(void)
{
    CHECK_EQ(gt_seq_check(100, 0, 5), GT_SEQ_ACCEPT);
    CHECK_EQ(gt_seq_check(100, 0, 100), GT_SEQ_ACCEPT);
}

/* ---- SQG-03: the next seq is accepted ---- */
static void test_sqg03_next_seq_accepted(void)
{
    CHECK_EQ(gt_seq_check(7, 1, 8), GT_SEQ_ACCEPT);
    CHECK_EQ(gt_seq_check(0, 1, 1), GT_SEQ_ACCEPT);
    CHECK_EQ(gt_seq_check(UINT32_MAX - 1, 1, UINT32_MAX), GT_SEQ_ACCEPT);
}

/* ---- SQG-04: a repeated seq is rejected ----
 * A duplicate, e.g. a record the hub resent after reconnecting. */
static void test_sqg04_repeated_seq_rejected(void)
{
    CHECK_EQ(gt_seq_check(7, 1, 7), GT_SEQ_REJECT);
    CHECK_EQ(gt_seq_check(0, 1, 0), GT_SEQ_REJECT);
}

/* ---- SQG-05: a backwards seq is rejected ----
 * E.g. a restarted hub that started over at 0 (DESIGN.md seq_rejected). */
static void test_sqg05_backwards_seq_rejected(void)
{
    CHECK_EQ(gt_seq_check(7, 1, 3), GT_SEQ_REJECT);
    CHECK_EQ(gt_seq_check(7, 1, 0), GT_SEQ_REJECT);
    CHECK_EQ(gt_seq_check(UINT32_MAX, 1, 0), GT_SEQ_REJECT);
}

/* ---- SQG-06: a skipped seq is a gap ----
 * Skipping one value is already a gap; so is a huge jump. */
static void test_sqg06_skip_is_gap(void)
{
    CHECK_EQ(gt_seq_check(7, 1, 9), GT_SEQ_GAP);
    CHECK_EQ(gt_seq_check(0, 1, UINT32_MAX), GT_SEQ_GAP);
}

/* ---- SQG-07: the gap record covers the missing seqs ----
 * Last 7 (stamped 4000), incoming 10 (stamped 5000): seqs 8 and 9 are
 * missing, so the GAP has seq 8 and value 2. It carries 4000, the time
 * the gap could first have started, not 5000, when it ended (PF-07). Its
 * seq sits between the two records, so the log still increases, and
 * storage's own record encoder accepts it. */
static void test_sqg07_gap_record_fields(void)
{
    const gt_record incoming = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .seq = 10, .timestamp_us = 5000, .value = 321,
    };
    gt_record gap;
    uint8_t wire[GT_REC_SIZE];

    CHECK_EQ(gt_seq_gap_record(7, 4000, &incoming, &gap), GT_OK);
    CHECK_EQ(gap.type, GT_REC_GAP);
    CHECK_EQ(gap.sensor_id, GT_SENSOR_ALL);
    CHECK_EQ(gap.seq, 8);
    CHECK(gap.timestamp_us == 4000);   /* u64: too big for CHECK_EQ's long long */
    CHECK_EQ(gap.value, 2);
    CHECK(gap.seq > 7 && gap.seq < incoming.seq);
    CHECK_EQ(gt_record_encode(&gap, wire), GT_OK);
}

/* ---- SQG-08: no gap record without a gap ----
 * The next seq, a repeat and a backwards seq are not gaps: GT_ERR_INVAL
 * and the output is left exactly as it was. */
static void test_sqg08_no_gap_no_record(void)
{
    const uint32_t not_gap_seqs[] = { 8, 7, 3 };
    size_t seq_count = sizeof not_gap_seqs / sizeof not_gap_seqs[0];

    for (size_t seq_index = 0; seq_index < seq_count; seq_index++) {
        const gt_record incoming = {
            .type = GT_REC_DATA, .sensor_id = GT_SENSOR_SOIL,
            .seq = not_gap_seqs[seq_index], .timestamp_us = 1, .value = 1,
        };
        gt_record gap, untouched;
        memset(&gap, 0x5A, sizeof gap);
        memset(&untouched, 0x5A, sizeof untouched);

        CHECK_EQ(gt_seq_gap_record(7, 4000, &incoming, &gap), GT_ERR_INVAL);
        CHECK(memcmp(&gap, &untouched, sizeof gap) == 0);
    }
}

/* ---- SQG-09: NULL arguments are rejected ---- */
static void test_sqg09_null_args(void)
{
    const gt_record incoming = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .seq = 10, .timestamp_us = 1, .value = 1,
    };
    gt_record gap;

    CHECK_EQ(gt_seq_gap_record(7, 4000, NULL, &gap), GT_ERR_INVAL);
    CHECK_EQ(gt_seq_gap_record(7, 4000, &incoming, NULL), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"SQG-01", "the first record is always accepted", test_sqg01_first_record_accepted},
    {"SQG-02", "last_accepted is ignored with no last", test_sqg02_no_last_ignores_value},
    {"SQG-03", "the next seq is accepted", test_sqg03_next_seq_accepted},
    {"SQG-04", "a repeated seq is rejected", test_sqg04_repeated_seq_rejected},
    {"SQG-05", "a backwards seq is rejected", test_sqg05_backwards_seq_rejected},
    {"SQG-06", "a skipped seq is a gap", test_sqg06_skip_is_gap},
    {"SQG-07", "the gap record covers the missing seqs", test_sqg07_gap_record_fields},
    {"SQG-08", "no gap record without a gap", test_sqg08_no_gap_no_record},
    {"SQG-09", "NULL arguments are rejected", test_sqg09_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
