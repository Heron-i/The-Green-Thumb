/* Unit tests for one pass of the flush thread (src/storage_server.c):
 * gt_flusher_init and gt_flusher_tick.
 *
 * Purpose: prove the flush thread commits when (and only when) a flush is
 * due, rolls over to a new day's segment, and reports every event through
 * the log callback with the names DESIGN.md uses, while logging
 * retention_skipped once per unsynced spell rather than once a second.
 * Times are passed in, so nothing sleeps; uses tests/fake_file.h. Run
 * under make test / asan / memcheck. Implements FTK-01 through FTK-06.
 *
 * What these tests cannot prove: the real one-second loop and stopping it
 * (gt_flusher_run, checked in the WSL smoke run of storage_main). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"
#include "storage_server.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TOMORROW_US   ((uint64_t)1793059200u * 1000000u)             /* 2026-10-27 00:00 UTC */
#define TODAY_NAME    "log-20261026.bin"
#define TOMORROW_NAME "log-20261027.bin"
#define TEST_CAPACITY 8
#define AGE_LIMIT_MS  ((uint64_t)GT_BATCH_MAX_AGE_S * 1000u)
#define MAX_LOGGED    16

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

/* How many logged events were called `event`. */
static size_t logged(const char *event)
{
    size_t matches = 0;
    for (size_t log_index = 0; log_index < book.count; log_index++) {
        if (strcmp(book.events[log_index], event) == 0) {
            matches++;
        }
    }
    return matches;
}

/* Open storage on a fresh fake and set up a flusher on it with a fresh
 * log book. */
static gt_storage *open_with_flusher(gt_flusher *flusher)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = NULL;
    CHECK_EQ(gt_storage_open_ops(ops, &fs, TODAY_US, TEST_CAPACITY, &storage, NULL), GT_OK);
    memset(&book, 0, sizeof book);
    gt_flusher_init(flusher, storage, 3, record_log, &book);
    return storage;
}

/* Append a light DATA record with `seq` at now_ms. */
static void append_at(gt_storage *storage, uint32_t seq, uint64_t now_ms)
{
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .seq = seq, .timestamp_us = 100 + seq, .value = seq,
    };
    uint8_t wire[GT_REC_SIZE];
    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
    CHECK_EQ(gt_storage_append(storage, wire, now_ms, NULL), GT_OK);
}

/* Records committed so far. */
static uint64_t committed(const gt_storage *storage)
{
    gt_storage_stats stats;
    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    return stats.committed_records;
}

/* ---- FTK-01: a pass with nothing due does nothing ----
 * One record, 1 s old: no write, nothing logged. */
static void test_ftk01_nothing_due(void)
{
    gt_flusher flusher;
    gt_storage *storage = open_with_flusher(&flusher);
    append_at(storage, 1, 0);
    int pwrites_before = fs.pwrite_calls;

    gt_flusher_tick(&flusher, TODAY_US, 1000, 1);
    CHECK_EQ(fs.pwrite_calls, pwrites_before);
    CHECK(committed(storage) == 0);
    CHECK_EQ(book.count, 0);
    gt_storage_close(storage);
}

/* ---- FTK-02: a due pass commits quietly ----
 * At 60 s the record is committed; a healthy flush logs nothing. */
static void test_ftk02_due_pass_commits(void)
{
    gt_flusher flusher;
    gt_storage *storage = open_with_flusher(&flusher);
    append_at(storage, 1, 0);

    gt_flusher_tick(&flusher, TODAY_US, AGE_LIMIT_MS, 1);
    CHECK(committed(storage) == 1);
    CHECK_EQ(book.count, 0);
    gt_storage_close(storage);
}

/* ---- FTK-03: a failed flush is logged ----
 * storage_write_error and storage_degraded (reason=write_failed) at
 * ERROR; the next pass recovers and logs storage_recovered. */
static void test_ftk03_failed_flush_logged(void)
{
    gt_flusher flusher;
    gt_storage *storage = open_with_flusher(&flusher);
    append_at(storage, 1, 0);
    fs.fail_pwrite_call = fs.pwrite_calls + 1;

    gt_flusher_tick(&flusher, TODAY_US, AGE_LIMIT_MS, 1);
    CHECK_EQ(logged("storage_write_error"), 1);
    CHECK_EQ(logged("storage_degraded"), 1);
    for (size_t log_index = 0; log_index < book.count; log_index++) {
        CHECK_EQ(book.levels[log_index], GT_LOG_ERROR);
        if (strcmp(book.events[log_index], "storage_degraded") == 0) {
            CHECK(strcmp(book.fields[log_index], "reason=write_failed") == 0);
        }
    }

    gt_flusher_tick(&flusher, TODAY_US, AGE_LIMIT_MS + 1000, 1);
    CHECK_EQ(logged("storage_recovered"), 1);
    CHECK(committed(storage) == 1);
    gt_storage_close(storage);
}

/* ---- FTK-04: unsynced is logged once per spell ----
 * Two unsynced passes log retention_skipped once. After a synced pass, the
 * clock turning unsynced again is logged again. */
static void test_ftk04_unsynced_once_per_spell(void)
{
    gt_flusher flusher;
    gt_storage *storage = open_with_flusher(&flusher);

    gt_flusher_tick(&flusher, TODAY_US, 0, 0);
    gt_flusher_tick(&flusher, TODAY_US, 1000, 0);
    CHECK_EQ(logged("retention_skipped"), 1);
    gt_flusher_tick(&flusher, TODAY_US, 2000, 1);
    CHECK_EQ(logged("retention_skipped"), 1);
    gt_flusher_tick(&flusher, TODAY_US, 3000, 0);
    CHECK_EQ(logged("retention_skipped"), 2);
    gt_storage_close(storage);
}

/* ---- FTK-05: a new day is logged and used ----
 * The first pass after midnight rolls over (segment_rollover, INFO) and
 * the due record is written to the new segment in the same pass. */
static void test_ftk05_new_day_used(void)
{
    gt_flusher flusher;
    gt_storage *storage = open_with_flusher(&flusher);
    append_at(storage, 1, 0);

    gt_flusher_tick(&flusher, TOMORROW_US, AGE_LIMIT_MS, 1);
    CHECK_EQ(logged("segment_rollover"), 1);
    CHECK_EQ(book.levels[0], GT_LOG_INFO);
    CHECK_EQ(fake_fs_file(&fs, TOMORROW_NAME)->synced_len, GT_SEG_HEADER_SZ + GT_REC_SIZE);
    CHECK_EQ(fake_fs_file(&fs, TODAY_NAME)->synced_len, GT_SEG_HEADER_SZ);
    gt_storage_close(storage);
}

/* ---- FTK-06: NULL is a safe no-op ---- */
static void test_ftk06_null_safe(void)
{
    gt_flusher_init(NULL, NULL, 0, record_log, &book);
    gt_flusher_tick(NULL, TODAY_US, 0, 1);
}

static const test_case_t tests[] = {
    {"FTK-01", "a pass with nothing due does nothing", test_ftk01_nothing_due},
    {"FTK-02", "a due pass commits quietly", test_ftk02_due_pass_commits},
    {"FTK-03", "a failed flush is logged", test_ftk03_failed_flush_logged},
    {"FTK-04", "unsynced is logged once per spell", test_ftk04_unsynced_once_per_spell},
    {"FTK-05", "a new day is logged and used", test_ftk05_new_day_used},
    {"FTK-06", "NULL is a safe no-op", test_ftk06_null_safe},
};

int main(void)
{
    return RUN_TESTS(tests);
}
