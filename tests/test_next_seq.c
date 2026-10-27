/* Unit tests for picking the next seq after a seq_resume reply
 * (src/storage_proto.c): gt_next_seq.
 *
 * Purpose: prove the PF-03 numbering rule, next = max(own next, reply + 1).
 * A fresh sender (the hub just started, or storage_feed without -r) takes
 * the reply + 1, or 0 on an empty log. A running sender keeps its own
 * counter, so seqs storage lost in a kill -9 leave a hole that storage
 * turns into a GAP. The output is untouched on every failure. Pure
 * arithmetic: no sockets. Run under make test / asan / memcheck.
 * Implements NSQ-01 through NSQ-08.
 *
 * What these tests cannot prove: that storage writes the GAP for the hole
 * (SAP / SQG do) or that the senders call this (the fault rounds do). */
#include <stdint.h>

#include "check.h"
#include "storage_proto.h"

/* Value the output is pre-filled with, to show a failure leaves it alone. */
#define UNTOUCHED 0x5A5A5A5Au

/* ---- NSQ-01: fresh sender continues after reply ----
 * own_next is 999 to prove a fresh sender ignores it. */
static void test_nsq01_fresh_after_reply(void)
{
    const gt_resume resume = { .has_last = 1, .seq = 59 };
    uint32_t next = UNTOUCHED;

    CHECK_EQ(gt_next_seq(0, 999, &resume, &next), GT_OK);
    CHECK_EQ(next, 60);
}

/* ---- NSQ-02: fresh sender on an empty log starts at 0 ---- */
static void test_nsq02_fresh_empty_log(void)
{
    const gt_resume resume = { .has_last = 0, .seq = 0 };
    uint32_t next = UNTOUCHED;

    CHECK_EQ(gt_next_seq(0, 999, &resume, &next), GT_OK);
    CHECK_EQ(next, 0);
}

/* ---- NSQ-03: running sender keeps its counter past a loss ----
 * It sent up to 79, storage was killed and kept only up to 59: the next
 * seq is 80, so storage sees 60-79 missing and writes their GAP. */
static void test_nsq03_running_keeps_counter(void)
{
    const gt_resume resume = { .has_last = 1, .seq = 59 };
    uint32_t next = UNTOUCHED;

    CHECK_EQ(gt_next_seq(1, 80, &resume, &next), GT_OK);
    CHECK_EQ(next, 80);
}

/* ---- NSQ-04: running sender after a clean restart ----
 * Storage kept everything (SIGTERM), so both sides agree on 60: no hole. */
static void test_nsq04_running_clean_restart(void)
{
    const gt_resume resume = { .has_last = 1, .seq = 59 };
    uint32_t next = UNTOUCHED;

    CHECK_EQ(gt_next_seq(1, 60, &resume, &next), GT_OK);
    CHECK_EQ(next, 60);
}

/* ---- NSQ-05: a reply ahead of the counter wins ----
 * Storage already holds up to 59 (another sender filled it), so reusing
 * 10.. would only be rejected as old: continue at 60. */
static void test_nsq05_reply_ahead_wins(void)
{
    const gt_resume resume = { .has_last = 1, .seq = 59 };
    uint32_t next = UNTOUCHED;

    CHECK_EQ(gt_next_seq(1, 10, &resume, &next), GT_OK);
    CHECK_EQ(next, 60);
}

/* ---- NSQ-06: running sender on an empty log keeps its counter ----
 * The log was wiped under a running sender: there is no reply seq, so
 * the sender's own counter is the only one. */
static void test_nsq06_running_empty_log(void)
{
    const gt_resume resume = { .has_last = 0, .seq = 0 };
    uint32_t next = UNTOUCHED;

    CHECK_EQ(gt_next_seq(1, 80, &resume, &next), GT_OK);
    CHECK_EQ(next, 80);
}

/* ---- NSQ-07: NULL arguments are rejected ---- */
static void test_nsq07_null_args(void)
{
    const gt_resume resume = { .has_last = 1, .seq = 59 };
    uint32_t next = UNTOUCHED;

    CHECK_EQ(gt_next_seq(1, 80, NULL, &next), GT_ERR_INVAL);
    CHECK_EQ(next, UNTOUCHED);
    CHECK_EQ(gt_next_seq(1, 80, &resume, NULL), GT_ERR_INVAL);
}

/* ---- NSQ-08: a reply of UINT32_MAX has no next seq ----
 * reply + 1 would wrap to 0, which storage would reject as old. Checked
 * for a fresh and a running sender. */
static void test_nsq08_reply_at_max(void)
{
    const gt_resume resume = { .has_last = 1, .seq = UINT32_MAX };
    uint32_t next = UNTOUCHED;

    CHECK_EQ(gt_next_seq(0, 0, &resume, &next), GT_ERR_FULL);
    CHECK_EQ(next, UNTOUCHED);
    CHECK_EQ(gt_next_seq(1, 80, &resume, &next), GT_ERR_FULL);
    CHECK_EQ(next, UNTOUCHED);
}

static const test_case_t tests[] = {
    {"NSQ-01", "fresh sender continues after reply", test_nsq01_fresh_after_reply},
    {"NSQ-02", "fresh sender on an empty log starts at 0", test_nsq02_fresh_empty_log},
    {"NSQ-03", "running sender keeps its counter past a loss", test_nsq03_running_keeps_counter},
    {"NSQ-04", "running sender after a clean restart", test_nsq04_running_clean_restart},
    {"NSQ-05", "a reply ahead of the counter wins", test_nsq05_reply_ahead_wins},
    {"NSQ-06", "running sender on an empty log", test_nsq06_running_empty_log},
    {"NSQ-07", "NULL arguments are rejected", test_nsq07_null_args},
    {"NSQ-08", "a reply of UINT32_MAX has no next seq", test_nsq08_reply_at_max},
};

int main(void)
{
    return RUN_TESTS(tests);
}
