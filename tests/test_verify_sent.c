/* Unit tests for the sent-range check (src/log_verify.c): gt_verify_sent.
 *
 * Purpose: prove the verifier's last step, the comparison the Evaluation
 * plan's lost-readings formula needs (PF-12): every seq one hub run sent
 * (A..B) is stored or covered by a GAP, and the log holds nothing that run
 * didn't send. Pure logic: each test builds a gt_verify_report by hand,
 * since gt_verify_dir_ops already has its own tests (VFY), so no fake
 * directory is needed. Run under make test / asan / memcheck. Implements
 * VSR-01 through VSR-07. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "log_verify.h"

/* A report that passed, holding seqs first..last (every one stored or
 * covered by a GAP, which is what a passing report guarantees). */
static gt_verify_report passing_report(uint32_t first, uint32_t last)
{
    gt_verify_report report;
    memset(&report, 0, sizeof report);
    report.ok = 1;
    report.segments = 1;
    report.has_records = 1;
    report.first_seq = first;
    report.last_seq = last;
    report.data_records = (uint64_t)last - first + 1;
    return report;
}

/* Run the check, expecting the call itself to work. The result starts
 * filled with junk so a field the function forgets to set shows up. */
static gt_verify_sent_result check_sent(const gt_verify_report *report, uint32_t sent_first,
                                        uint32_t sent_last)
{
    gt_verify_sent_result result;
    memset(&result, 0x5A, sizeof result);
    CHECK_EQ(gt_verify_sent(report, sent_first, sent_last, &result), GT_OK);
    return result;
}

/* Check a failed result: its failure name and lost count. */
static void check_failed(const gt_verify_sent_result *result, const char *failure,
                         uint64_t lost)
{
    CHECK_EQ(result->ok, 0);
    CHECK(result->failure != NULL && strcmp(result->failure, failure) == 0);
    CHECK(result->lost == lost);
}

/* ---- VSR-01: the log matches the sent range exactly ----
 * Sent 10..19, log 10..19: 10 sent, 0 lost. */
static void test_vsr01_exact_match_passes(void)
{
    const gt_verify_report report = passing_report(10, 19);
    gt_verify_sent_result result = check_sent(&report, 10, 19);

    CHECK_EQ(result.ok, 1);
    CHECK(result.sent == 10);
    CHECK(result.lost == 0);
    CHECK(result.failure == NULL);
}

/* ---- VSR-02: seqs before A (earlier hub runs) are allowed ----
 * The log holds 0..19, but this run sent only 10..19: seqs 0..9 came from
 * an earlier run, as on the Pi where the log keeps 30 days of history. */
static void test_vsr02_earlier_runs_pass(void)
{
    const gt_verify_report report = passing_report(0, 19);
    gt_verify_sent_result result = check_sent(&report, 10, 19);

    CHECK_EQ(result.ok, 1);
    CHECK(result.sent == 10);
    CHECK(result.lost == 0);
    CHECK(result.failure == NULL);
}

/* ---- VSR-03: a lost tail fails and is counted ----
 * Sent 10..19, log ends at 16: 17..19 were lost (a power cut's unsynced
 * batch). Then the widest range, 0..UINT32_MAX, with the log ending at
 * UINT32_MAX - 1: sent must be 2^32, which only fits in 64 bits. */
static void test_vsr03_lost_tail_counted(void)
{
    const gt_verify_report short_tail = passing_report(10, 16);
    gt_verify_sent_result result = check_sent(&short_tail, 10, 19);
    check_failed(&result, GT_VERIFY_SENT_SEQ_LOST, 3);
    CHECK(result.sent == 10);

    const gt_verify_report almost_all = passing_report(0, UINT32_MAX - 1);
    result = check_sent(&almost_all, 0, UINT32_MAX);
    check_failed(&result, GT_VERIFY_SENT_SEQ_LOST, 1);
    CHECK(result.sent == (uint64_t)UINT32_MAX + 1);
}

/* ---- VSR-04: a lost head fails and is counted ----
 * Sent 10..19, log starts at 13: 10..12 were lost. Then a log that ends
 * before this run began (0..5): all 10 sent seqs were lost. */
static void test_vsr04_lost_head_counted(void)
{
    const gt_verify_report late_start = passing_report(13, 19);
    gt_verify_sent_result result = check_sent(&late_start, 10, 19);
    check_failed(&result, GT_VERIFY_SENT_SEQ_LOST, 3);

    const gt_verify_report only_earlier = passing_report(0, 5);
    result = check_sent(&only_earlier, 10, 19);
    check_failed(&result, GT_VERIFY_SENT_SEQ_LOST, 10);
}

/* ---- VSR-05: seqs after B fail as never sent ----
 * Sent 10..19, log runs to 25: 20..25 came from nowhere this run knows
 * of. That is the worse failure, so it is the one named, but lost is
 * still counted: with the log starting at 12, 10..11 are lost too. */
static void test_vsr05_seq_after_b_fails(void)
{
    const gt_verify_report too_long = passing_report(10, 25);
    gt_verify_sent_result result = check_sent(&too_long, 10, 19);
    check_failed(&result, GT_VERIFY_SEQ_NOT_SENT, 0);

    const gt_verify_report shifted = passing_report(12, 25);
    result = check_sent(&shifted, 10, 19);
    check_failed(&result, GT_VERIFY_SEQ_NOT_SENT, 2);
}

/* ---- VSR-06: an empty log, or a log that failed, fails ----
 * An empty log passes gt_verify_dir_ops, but holds none of the 10 sent
 * seqs: all lost. A report that already failed (here a partial tail) can't
 * be compared at all: log_failed, with lost left at 0. */
static void test_vsr06_empty_or_failed_log(void)
{
    gt_verify_report empty;
    memset(&empty, 0, sizeof empty);
    empty.ok = 1;
    gt_verify_sent_result result = check_sent(&empty, 10, 19);
    check_failed(&result, GT_VERIFY_SENT_SEQ_LOST, 10);
    CHECK(result.sent == 10);

    gt_verify_report failed = passing_report(10, 19);
    failed.ok = 0;
    failed.failure = GT_VERIFY_PARTIAL_TAIL;
    result = check_sent(&failed, 10, 19);
    check_failed(&result, GT_VERIFY_LOG_FAILED, 0);
}

/* ---- VSR-07: bad args are rejected, the result untouched ----
 * NULL report, NULL result, and a range with A > B. */
static void test_vsr07_bad_args(void)
{
    const gt_verify_report report = passing_report(10, 19);
    gt_verify_sent_result result, untouched;
    memset(&result, 0x5A, sizeof result);
    memset(&untouched, 0x5A, sizeof untouched);

    CHECK_EQ(gt_verify_sent(NULL, 10, 19, &result), GT_ERR_INVAL);
    CHECK_EQ(gt_verify_sent(&report, 10, 19, NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_verify_sent(&report, 20, 19, &result), GT_ERR_INVAL);
    CHECK(memcmp(&result, &untouched, sizeof result) == 0);
}

static const test_case_t tests[] = {
    {"VSR-01", "the log matches the sent range", test_vsr01_exact_match_passes},
    {"VSR-02", "seqs from earlier hub runs pass", test_vsr02_earlier_runs_pass},
    {"VSR-03", "a lost tail fails and is counted", test_vsr03_lost_tail_counted},
    {"VSR-04", "a lost head fails and is counted", test_vsr04_lost_head_counted},
    {"VSR-05", "seqs after B fail as never sent", test_vsr05_seq_after_b_fails},
    {"VSR-06", "an empty or failed log fails", test_vsr06_empty_or_failed_log},
    {"VSR-07", "bad args are rejected", test_vsr07_bad_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
