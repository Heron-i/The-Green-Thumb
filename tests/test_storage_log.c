/* Unit tests for logging one received frame's events
 * (src/storage_server.c): gt_storage_log_rx_events.
 *
 * Purpose: prove storage_backlog says which threshold was crossed and how
 * full the buffer is ("percent=N pending=P", DESIGN.md "storage_backlog
 * at 50/75/90%", PF-14), while every other event is logged exactly as
 * gt_storage_log_events logs it. Uses tests/fake_file.h and a 20-record
 * buffer, so the thresholds are at 10, 15 and 18 pending records. Run
 * under make test / asan / memcheck. Implements SLE-01 through SLE-04.
 *
 * What these tests cannot prove: that the rx loop calls it for every
 * frame (SSV runs the loop over a real socket, with a 1 MB buffer that
 * these thresholds can't be reached in). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"
#include "storage_server.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TEST_CAPACITY 20   /* thresholds at 10, 15 and 18 pending records */
#define MAX_LOGGED    8

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* Every call the code under test made to the log callback, in order. */
typedef struct {
    size_t       count;
    char         events[MAX_LOGGED][32];
    char         fields[MAX_LOGGED][48];   /* "" when fields was NULL */
    gt_log_level levels[MAX_LOGGED];
} log_record_book;

static log_record_book book;

/* The log callback the tests pass: copies each call into `book`. */
static void record_log(void *log_ctx, gt_log_level level, const char *event, const char *fields)
{
    log_record_book *records = log_ctx;
    if (records->count == MAX_LOGGED) {
        return;
    }
    strncpy(records->events[records->count], event, sizeof records->events[0] - 1);
    strncpy(records->fields[records->count], fields != NULL ? fields : "",
            sizeof records->fields[0] - 1);
    records->levels[records->count] = level;
    records->count++;
}

/* Open storage on a fresh fake with a fresh log book. */
static gt_storage *open_storage(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = NULL;
    gt_storage_open_args args = { .ops = ops, .ctx = &fs, .now_us = TODAY_US,
                                  .pending_capacity = TEST_CAPACITY };
    CHECK_EQ(gt_storage_open_ops(&args, &storage, ), );
    memset(&book, 0, sizeof book);
    return storage;
}

/* Append a soil DATA record with `seq` and return its events. */
static unsigned append_seq(gt_storage *storage, uint32_t seq)
{
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_SOIL,
        .seq = seq, .timestamp_us = 8000 + seq, .value = 900 + seq,
    };
    uint8_t wire[GT_REC_SIZE];
    unsigned events = 0;
    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
    CHECK_EQ(gt_storage_append(storage, wire, 0, &events), GT_OK);
    return events;
}

/* Append seqs first .. last and return the events of the last one. */
static unsigned append_through(gt_storage *storage, uint32_t first, uint32_t last)
{
    unsigned events = 0;
    /* invariant: seqs first .. seq - 1 have been accepted */
    for (uint32_t seq = first; seq <= last; seq++) {
        events = append_seq(storage, seq);
    }
    return events;
}

/* ---- SLE-01: crossing 50% is logged with its threshold ----
 * The 10th record of 20 crosses 50%: one storage_backlog at WARN with
 * "percent=50 pending=10". */
static void test_sle01_backlog_has_threshold(void)
{
    gt_storage *storage = open_storage();
    unsigned events = append_through(storage, 1, 10);
    CHECK_EQ(events, GT_STORAGE_EV_BACKLOG);

    gt_storage_log_rx_events(record_log, &book, storage, events);
    CHECK_EQ(book.count, 1);
    CHECK(strcmp(book.events[0], "storage_backlog") == 0);
    CHECK_EQ(book.levels[0], GT_LOG_WARN);
    CHECK(strcmp(book.fields[0], "percent=50 pending=10") == 0);
    gt_storage_close(storage);
}

/* ---- SLE-02: 75% and 90% are told apart ----
 * The 15th and 18th records cross 75% and 90%; each line names its own
 * threshold, so a reader can see how close the buffer came to full. */
static void test_sle02_each_threshold_named(void)
{
    gt_storage *storage = open_storage();
    append_through(storage, 1, 14);

    gt_storage_log_rx_events(record_log, &book, storage, append_seq(storage, 15));
    append_through(storage, 16, 17);
    gt_storage_log_rx_events(record_log, &book, storage, append_seq(storage, 18));
    CHECK_EQ(book.count, 2);
    CHECK(strcmp(book.fields[0], "percent=75 pending=15") == 0);
    CHECK(strcmp(book.fields[1], "percent=90 pending=18") == 0);
    gt_storage_close(storage);
}

/* ---- SLE-03: other events are logged as before ----
 * A GAP alongside a crossing: recovery_gap is logged by
 * gt_storage_log_events' rule (no fields), and storage_backlog with its
 * fields. */
static void test_sle03_other_events_unchanged(void)
{
    gt_storage *storage = open_storage();
    append_through(storage, 1, 8);
    unsigned events = append_seq(storage, 12);   /* skips 9-11: a GAP, and 10 pending */
    CHECK_EQ(events, GT_STORAGE_EV_RECOVERY_GAP | GT_STORAGE_EV_BACKLOG);

    gt_storage_log_rx_events(record_log, &book, storage, events);
    CHECK_EQ(book.count, 2);
    for (size_t log_index = 0; log_index < book.count; log_index++) {
        if (strcmp(book.events[log_index], "recovery_gap") == 0) {
            CHECK(strcmp(book.fields[log_index], "") == 0);
        } else {
            CHECK(strcmp(book.events[log_index], "storage_backlog") == 0);
            CHECK(strcmp(book.fields[log_index], "percent=50 pending=10") == 0);
        }
    }
    gt_storage_close(storage);
}

/* ---- SLE-04: NULL log or storage is safe ----
 * NULL log logs nothing; NULL storage still logs storage_backlog, just
 * without fields, so the event is never lost. */
static void test_sle04_null_args_safe(void)
{
    memset(&book, 0, sizeof book);
    gt_storage_log_rx_events(NULL, &book, NULL, GT_STORAGE_EV_BACKLOG);
    CHECK_EQ(book.count, 0);

    gt_storage_log_rx_events(record_log, &book, NULL, GT_STORAGE_EV_BACKLOG);
    CHECK_EQ(book.count, 1);
    CHECK(strcmp(book.events[0], "storage_backlog") == 0);
    CHECK(strcmp(book.fields[0], "") == 0);
}

static const test_case_t tests[] = {
    {"SLE-01", "crossing 50% is logged with its threshold", test_sle01_backlog_has_threshold},
    {"SLE-02", "75% and 90% are told apart", test_sle02_each_threshold_named},
    {"SLE-03", "other events are logged as before", test_sle03_other_events_unchanged},
    {"SLE-04", "NULL log or storage is safe", test_sle04_null_args_safe},
};

int main(void)
{
    return RUN_TESTS(tests);
}
