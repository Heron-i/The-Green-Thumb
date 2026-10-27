/* Unit tests for opening storage (src/storage.c): gt_storage_open_ops,
 * gt_storage_read_stats and gt_storage_close.
 *
 * Purpose: prove open runs recovery before anything else, picks the right
 * segment to append to (today's, or a later-dated one when the clock is
 * behind), creates a new segment durably (header, fsync, fsync_dir),
 * finds the newest seq on disk even when the newest segment has none, and
 * ignores every file that isn't a segment. On any failure it returns an
 * error, leaves *storage_out alone, calls destroy exactly once and leaves
 * no handle open. Uses tests/fake_file.h, so no disk is touched. Run under
 * make test / asan / memcheck. Implements SOP-01 through SOP-16.
 *
 * What these tests cannot prove: append, flush and the clean-shutdown
 * marker (Mechanism D Steps 7c, 7d and 9), or real ext4 (Step 12a). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"

/* 2026-10-26 01:00:00 UTC (1792972800 s is midnight, see SNM-02). */
#define TODAY_US     ((uint64_t)(1792972800u + 3600u) * 1000000u)
#define TODAY_NAME   "log-20261026.bin"
#define YESTERDAY    "log-20261025.bin"
#define TEST_CAPACITY 8

#define SEGMENT_MAX_RECORDS 8
#define SEGMENT_BUF_SIZE    (GT_SEG_HEADER_SZ + (SEGMENT_MAX_RECORDS + 1) * GT_REC_SIZE)
#define RECORD_OFFSET(record_index) (GT_SEG_HEADER_SZ + (record_index) * GT_REC_SIZE)

/* Shared by every case; fake_fs_ops() resets it. Static because a fake_fs
 * is about 70 KB (see fake_file.h). */
static fake_fs fs;

/* Write a valid header, then one light DATA record per entry of `seqs`,
 * into `segment`. Returns the segment's length in bytes. */
static size_t build_segment(uint8_t segment[SEGMENT_BUF_SIZE],
                            const uint32_t *seqs, size_t seq_count)
{
    const gt_seg_header header = { .created_at_us = 1000 };
    memset(segment, 0, SEGMENT_BUF_SIZE);
    CHECK_EQ(gt_seg_header_encode(&header, segment), GT_OK);

    for (size_t record_index = 0; record_index < seq_count; record_index++) {
        const gt_record record = {
            .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
            .seq = seqs[record_index], .timestamp_us = 2000 + record_index,
            .value = 100 + (uint32_t)record_index,
        };
        CHECK_EQ(gt_record_encode(&record, &segment[RECORD_OFFSET(record_index)]), GT_OK);
    }
    return RECORD_OFFSET(seq_count);
}

/* Put a segment named `name` holding records with `seqs` into the fake. */
static void put_segment(const char *name, const uint32_t *seqs, size_t seq_count)
{
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, seqs, seq_count);
    CHECK_EQ(fake_fs_put(&fs, name, segment, len), GT_OK);
}

/* Open storage at `now_us` and check it succeeds. Returns the storage. */
static gt_storage *open_ok(const gt_file_ops *ops, uint64_t now_us, gt_storage_open_info *info)
{
    gt_storage *storage = NULL;
    CHECK_EQ(gt_storage_open_ops(ops, &fs, now_us, TEST_CAPACITY, &storage, info), GT_OK);
    CHECK(storage != NULL);
    return storage;
}

/* Close storage and check the clean-up contract: destroy exactly once and
 * no handle left open. */
static void close_and_check(gt_storage *storage)
{
    gt_storage_close(storage);
    CHECK_EQ(fs.destroy_calls, 1);
    CHECK_EQ(fake_fs_open_handles(&fs), 0);
    CHECK_EQ(fs.bad_handle_calls, 0);
}

/* Check `name` exists and starts with a valid segment header. */
static void check_has_header(const char *name, uint64_t expected_created_us)
{
    const fake_file *file = fake_fs_file(&fs, name);
    gt_seg_header header;

    CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    CHECK_EQ(file->len, GT_SEG_HEADER_SZ);
    CHECK_EQ(gt_seg_header_decode(file->data, &header), GT_OK);
    CHECK(header.created_at_us == expected_created_us);
}

/* Open with a failure scripted by the caller and check the failure
 * contract: `expected` status, *storage_out untouched, destroy exactly
 * once, no handle left open. `sentinel` is any address that is not a real
 * storage: if open wrote *storage_out, the pointer would change. */
static void open_expect_failure(const gt_file_ops *ops, gt_status expected)
{
    static char sentinel;
    gt_storage *storage = (gt_storage *)(void *)&sentinel;

    CHECK_EQ(gt_storage_open_ops(ops, &fs, TODAY_US, TEST_CAPACITY, &storage, NULL), expected);
    CHECK(storage == (gt_storage *)(void *)&sentinel);
    CHECK_EQ(fs.destroy_calls, 1);
    CHECK_EQ(fake_fs_open_handles(&fs), 0);
}

/* ---- SOP-01: an empty directory gets today's segment ----
 * The header is stamped with now_us and is already in the fake's synced
 * view when open returns: what a power cut at that moment would leave.
 * The synced view is checked directly, not with fake_fs_power_cut(),
 * because a power cut also ends the process, and storage still has to be
 * closed here. That proves the fsync (header bytes) and the fsync_dir
 * (the file name). */
static void test_sop01_empty_dir_creates_today(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage_open_info info;
    gt_storage *storage = open_ok(ops, TODAY_US, &info);
    gt_seg_header header;

    CHECK(strcmp(info.active_name, TODAY_NAME) == 0);
    CHECK_EQ(info.created, 1);
    CHECK_EQ(info.recovered, 0);
    CHECK_EQ(info.has_last, 0);
    CHECK_EQ(fs.fsync_calls, 1);
    CHECK_EQ(fs.fsync_dir_calls, 1);

    check_has_header(TODAY_NAME, TODAY_US);
    const fake_file *file = fake_fs_file(&fs, TODAY_NAME);
    CHECK(file != NULL);
    if (file != NULL) {
        CHECK_EQ(file->synced_used, 1);
        CHECK_EQ(file->synced_len, GT_SEG_HEADER_SZ);
        CHECK_EQ(gt_seg_header_decode(file->synced_data, &header), GT_OK);
        CHECK(header.created_at_us == TODAY_US);
    }
    close_and_check(storage);
}

/* ---- SOP-02: today's clean segment is reused ----
 * Nothing is written, and the seq on disk becomes both the last accepted
 * and the last committed seq. */
static void test_sop02_today_reused(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint32_t seqs[] = { 5, 6, 7 };
    put_segment(TODAY_NAME, seqs, 3);
    gt_storage_open_info info;
    gt_storage_stats stats;
    gt_storage *storage = open_ok(ops, TODAY_US, &info);

    CHECK(strcmp(info.active_name, TODAY_NAME) == 0);
    CHECK_EQ(info.created, 0);
    CHECK_EQ(info.recovered, 1);
    CHECK(strcmp(info.recovered_name, TODAY_NAME) == 0);
    CHECK_EQ(info.recovery.outcome, GT_RECOVER_CLEAN);
    CHECK_EQ(info.has_last, 1);
    CHECK_EQ(info.last_seq, 7);
    CHECK_EQ(fs.pwrite_calls, 0);

    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    CHECK_EQ(stats.pending, 0);
    CHECK(stats.committed_records == 0);
    CHECK_EQ(stats.has_last_accepted, 1);
    CHECK_EQ(stats.last_accepted_seq, 7);
    CHECK_EQ(stats.has_last_committed, 1);
    CHECK_EQ(stats.last_committed_seq, 7);
    close_and_check(storage);
}

/* ---- SOP-03: a torn tail is recovered first ----
 * 10 stray bytes after 2 records are cut before open returns. */
static void test_sop03_torn_tail_recovered(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint32_t seqs[] = { 1, 2 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t good_len = build_segment(segment, seqs, 2);
    memset(&segment[good_len], 0xA7, 10);
    CHECK_EQ(fake_fs_put(&fs, TODAY_NAME, segment, good_len + 10), GT_OK);
    gt_storage_open_info info;
    gt_storage *storage = open_ok(ops, TODAY_US, &info);

    CHECK_EQ(info.recovery.outcome, GT_RECOVER_TRUNCATED);
    CHECK_EQ(info.recovery.records_removed, 1);
    CHECK_EQ(info.last_seq, 2);
    CHECK_EQ(fake_fs_file(&fs, TODAY_NAME)->len, good_len);
    close_and_check(storage);
}

/* ---- SOP-04: a new day keeps yesterday's seq ----
 * Only yesterday's segment exists: today's is created, and the last seq
 * comes from yesterday, which is left exactly as it was. */
static void test_sop04_new_day_keeps_seq(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint32_t seqs[] = { 1, 2, 3 };
    put_segment(YESTERDAY, seqs, 3);
    size_t yesterday_len = fake_fs_file(&fs, YESTERDAY)->len;
    gt_storage_open_info info;
    gt_storage *storage = open_ok(ops, TODAY_US, &info);

    CHECK(strcmp(info.active_name, TODAY_NAME) == 0);
    CHECK_EQ(info.created, 1);
    CHECK(strcmp(info.recovered_name, YESTERDAY) == 0);
    CHECK_EQ(info.has_last, 1);
    CHECK_EQ(info.last_seq, 3);
    CHECK_EQ(fake_fs_file(&fs, YESTERDAY)->len, yesterday_len);
    check_has_header(TODAY_NAME, TODAY_US);
    close_and_check(storage);
}

/* ---- SOP-05: a corrupt newest segment falls back ----
 * Today's header is bad: it becomes .corrupt, a fresh today's is made,
 * and the last seq comes from yesterday. */
static void test_sop05_corrupt_newest_falls_back(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint32_t seqs[] = { 1, 2, 3 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    put_segment(YESTERDAY, seqs, 3);
    size_t len = build_segment(segment, seqs, 1);
    segment[0] = 'X';
    CHECK_EQ(fake_fs_put(&fs, TODAY_NAME, segment, len), GT_OK);
    gt_storage_open_info info;
    gt_storage *storage = open_ok(ops, TODAY_US, &info);

    CHECK_EQ(info.recovery.outcome, GT_RECOVER_RENAMED);
    CHECK(strcmp(info.recovered_name, TODAY_NAME) == 0);
    CHECK_EQ(info.created, 1);
    CHECK_EQ(info.has_last, 1);
    CHECK_EQ(info.last_seq, 3);
    CHECK(fake_fs_file(&fs, TODAY_NAME ".corrupt") != NULL);
    check_has_header(TODAY_NAME, TODAY_US);
    close_and_check(storage);
}

/* ---- SOP-06: a header-only newest looks back for seq ----
 * Today's segment exists but holds no records (storage restarted just
 * after midnight): it is reused, and the seq comes from yesterday. */
static void test_sop06_header_only_looks_back(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint32_t seqs[] = { 4, 5 };
    put_segment(YESTERDAY, seqs, 2);
    put_segment(TODAY_NAME, NULL, 0);
    gt_storage_open_info info;
    gt_storage *storage = open_ok(ops, TODAY_US, &info);

    CHECK(strcmp(info.active_name, TODAY_NAME) == 0);
    CHECK_EQ(info.created, 0);
    CHECK_EQ(info.has_last, 1);
    CHECK_EQ(info.last_seq, 5);
    close_and_check(storage);
}

/* ---- SOP-07: a clock behind the log keeps appending ----
 * now_us = 0 (1970) with a 2026 segment on disk: append to the 2026
 * segment, and never create log-19700101.bin, which would sort before it. */
static void test_sop07_clock_behind_keeps_newest(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint32_t seqs[] = { 9 };
    put_segment(TODAY_NAME, seqs, 1);
    gt_storage_open_info info;
    gt_storage *storage = open_ok(ops, 0, &info);

    CHECK(strcmp(info.active_name, TODAY_NAME) == 0);
    CHECK_EQ(info.created, 0);
    CHECK(fake_fs_file(&fs, "log-19700101.bin") == NULL);
    close_and_check(storage);
}

/* ---- SOP-08: other files are ignored ----
 * A .corrupt file holding valid records, a stray file and the lock file
 * are not segments: their seqs are not used and they are not touched.
 * (The clean-shutdown marker is not ignored, open deletes it: that is
 * tested in test_storage_shutdown.c.) */
static void test_sop08_other_files_ignored(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint32_t seqs[] = { 50 };
    const uint8_t marker = 1;
    put_segment(YESTERDAY ".corrupt", seqs, 1);
    CHECK_EQ(fake_fs_put(&fs, "notes.txt", &marker, 1), GT_OK);
    CHECK_EQ(fake_fs_put(&fs, "storage.sock.lock", &marker, 1), GT_OK);
    gt_storage_open_info info;
    gt_storage *storage = open_ok(ops, TODAY_US, &info);

    CHECK_EQ(info.created, 1);
    CHECK_EQ(info.recovered, 0);
    CHECK_EQ(info.has_last, 0);
    CHECK_EQ(fs.rename_calls, 0);
    CHECK(fake_fs_file(&fs, YESTERDAY ".corrupt") != NULL);
    CHECK(fake_fs_file(&fs, "notes.txt") != NULL);
    CHECK(fake_fs_file(&fs, "storage.sock.lock") != NULL);
    close_and_check(storage);
}

/* ---- SOP-09: each step of creating can fail ----
 * In an empty directory, fail in turn: the listing, the create, the
 * header write, its fsync and the fsync_dir. Every one is GT_ERR_IO with
 * the full failure contract. */
static void test_sop09_create_failures(void)
{
    for (int failing_step = 0; failing_step < 5; failing_step++) {
        const gt_file_ops *ops = fake_fs_ops(&fs);
        switch (failing_step) {
        case 0: fs.fail_list_call = 1; break;
        case 1: fs.fail_open_call = 1; break;
        case 2: fs.fail_pwrite_call = 1; break;
        case 3: fs.fail_fsync_call = 1; break;
        default: fs.fail_fsync_dir_call = 1; break;
        }
        open_expect_failure(ops, GT_ERR_IO);
    }
}

/* ---- SOP-10: a failed recovery fails open ----
 * Storage must not accept data on top of a tail it could not repair. */
static void test_sop10_recovery_failure(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint32_t seqs[] = { 1 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, seqs, 1);
    CHECK_EQ(fake_fs_put(&fs, TODAY_NAME, segment, len + 5), GT_OK);
    fs.fail_ftruncate_call = 1;

    open_expect_failure(ops, GT_ERR_IO);
}

/* ---- SOP-11: close cleans up; info may be NULL ----
 * And close(NULL) is a safe no-op. */
static void test_sop11_close_and_null_info(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_ok(ops, TODAY_US, NULL);

    close_and_check(storage);
    gt_storage_close(NULL);
}

/* ---- SOP-12: bad arguments are rejected ----
 * NULL ops: the caller still owns ctx, so destroy is not called. NULL
 * storage_out or a now_us past 9999: storage owns ctx, so destroy is
 * called once. NULL args to read_stats are GT_ERR_INVAL. */
static void test_sop12_bad_args(void)
{
    gt_storage *storage = NULL;
    gt_storage_stats stats;

    const gt_file_ops *ops = fake_fs_ops(&fs);
    CHECK_EQ(gt_storage_open_ops(NULL, &fs, TODAY_US, TEST_CAPACITY, &storage, NULL),
             GT_ERR_INVAL);
    CHECK_EQ(fs.destroy_calls, 0);

    CHECK_EQ(gt_storage_open_ops(ops, &fs, TODAY_US, TEST_CAPACITY, NULL, NULL),
             GT_ERR_INVAL);
    CHECK_EQ(fs.destroy_calls, 1);

    ops = fake_fs_ops(&fs);
    CHECK_EQ(gt_storage_open_ops(ops, &fs, UINT64_MAX, TEST_CAPACITY, &storage, NULL),
             GT_ERR_INVAL);
    CHECK(storage == NULL);
    CHECK_EQ(fs.destroy_calls, 1);
    CHECK_EQ(fake_fs_open_handles(&fs), 0);

    CHECK_EQ(gt_storage_read_stats(NULL, &stats), GT_ERR_INVAL);
    ops = fake_fs_ops(&fs);
    storage = open_ok(ops, TODAY_US, NULL);
    CHECK_EQ(gt_storage_read_stats(storage, NULL), GT_ERR_INVAL);
    close_and_check(storage);
}

/* ---- SOP-13: open reports the newest record's time ----
 * Today's segment holds 2 records (stamped 2000 and 2001): 2001. Then,
 * with today's segment header-only and yesterday's holding 3 records, the
 * walk back for the last seq brings back yesterday's last time, 2002.
 * Storage stamps a GAP found after a restart with this time (PF-07). */
static void test_sop13_last_timestamp_reported(void)
{
    const uint32_t seqs[] = { 4, 5, 6 };
    const gt_file_ops *ops = fake_fs_ops(&fs);
    put_segment(TODAY_NAME, seqs, 2);
    gt_storage_open_info info;
    gt_storage *storage = open_ok(ops, TODAY_US, &info);
    CHECK_EQ(info.last_seq, 5);
    CHECK(info.last_timestamp_us == 2001);   /* u64: too big for CHECK_EQ's long long */
    close_and_check(storage);

    ops = fake_fs_ops(&fs);
    put_segment(YESTERDAY, seqs, 3);
    put_segment(TODAY_NAME, NULL, 0);
    storage = open_ok(ops, TODAY_US, &info);
    CHECK_EQ(info.last_seq, 6);
    CHECK(info.last_timestamp_us == 2002);
    close_and_check(storage);
}

/* ---- SOP-14: an older segment cut in the walk back is reported ----
 * Today's segment is header-only, so open looks back to yesterday's,
 * which ends in 10 torn bytes. Recovery cuts them, and open now says so:
 * one older entry, yesterday, TRUNCATED, 1 record removed (PF-14). Before,
 * only today's (clean) recovery was reported. */
static void test_sop14_older_cut_reported(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint32_t seqs[] = { 1, 2 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t good_len = build_segment(segment, seqs, 2);
    memset(&segment[good_len], 0xA7, 10);
    CHECK_EQ(fake_fs_put(&fs, YESTERDAY, segment, good_len + 10), GT_OK);
    put_segment(TODAY_NAME, NULL, 0);
    gt_storage_open_info info;
    gt_storage *storage = open_ok(ops, TODAY_US, &info);

    CHECK_EQ(info.recovery.outcome, GT_RECOVER_CLEAN);
    CHECK_EQ(info.last_seq, 2);
    CHECK_EQ(info.older_count, 1);
    CHECK_EQ(info.older_unlisted, 0);
    CHECK(strcmp(info.older[0].name, YESTERDAY) == 0);
    CHECK_EQ(info.older[0].report.outcome, GT_RECOVER_TRUNCATED);
    CHECK_EQ(info.older[0].report.records_removed, 1);
    close_and_check(storage);
}

/* ---- SOP-15: a renamed older segment is reported, clean ones not ----
 * Today's is header-only, yesterday's has a bad header (renamed to
 * .corrupt), and the day before holds 3 good records. The walk back
 * reports yesterday as RENAMED and lists nothing for the clean one. */
static void test_sop15_older_renamed_reported(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint32_t seqs[] = { 1, 2, 3 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    put_segment("log-20261024.bin", seqs, 3);
    size_t len = build_segment(segment, seqs, 1);
    segment[0] = 'X';
    CHECK_EQ(fake_fs_put(&fs, YESTERDAY, segment, len), GT_OK);
    put_segment(TODAY_NAME, NULL, 0);
    gt_storage_open_info info;
    gt_storage *storage = open_ok(ops, TODAY_US, &info);

    CHECK_EQ(info.last_seq, 3);
    CHECK_EQ(info.older_count, 1);
    CHECK(strcmp(info.older[0].name, YESTERDAY) == 0);
    CHECK_EQ(info.older[0].report.outcome, GT_RECOVER_RENAMED);
    CHECK(fake_fs_file(&fs, YESTERDAY ".corrupt") != NULL);
    close_and_check(storage);
}

/* ---- SOP-16: an empty log is told apart from a used one ----
 * Before open names a segment from the clock, storage_main asks whether
 * the log is empty, so an unsynced Pi can wait instead of starting
 * log-19700101.bin (PF-20). No files, or only a .corrupt file and the
 * marker: empty. One segment: not empty. Nothing is opened or changed. A
 * failed listing is GT_ERR_IO and NULL arguments GT_ERR_INVAL, with the
 * output left alone. */
static void test_sop16_empty_log_detected(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint8_t junk[] = { 'x' };
    int empty = -1;

    CHECK_EQ(gt_storage_log_empty_ops(ops, &fs, &empty), GT_OK);
    CHECK_EQ(empty, 1);

    CHECK_EQ(fake_fs_put(&fs, TODAY_NAME ".corrupt", junk, sizeof junk), GT_OK);
    CHECK_EQ(fake_fs_put(&fs, GT_STORAGE_MARKER_NAME, junk, 0), GT_OK);   /* empty file */
    CHECK_EQ(gt_storage_log_empty_ops(ops, &fs, &empty), GT_OK);
    CHECK_EQ(empty, 1);

    put_segment(YESTERDAY, NULL, 0);
    CHECK_EQ(gt_storage_log_empty_ops(ops, &fs, &empty), GT_OK);
    CHECK_EQ(empty, 0);
    CHECK_EQ(fs.open_calls, 0);
    CHECK_EQ(fs.unlink_calls, 0);

    empty = -1;
    fs.fail_list_call = fs.list_calls + 1;
    CHECK_EQ(gt_storage_log_empty_ops(ops, &fs, &empty), GT_ERR_IO);
    CHECK_EQ(empty, -1);
    CHECK_EQ(gt_storage_log_empty_ops(NULL, &fs, &empty), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_log_empty_ops(ops, &fs, NULL), GT_ERR_INVAL);
    CHECK_EQ(empty, -1);
}

static const test_case_t tests[] = {
    {"SOP-01", "an empty directory gets today's segment", test_sop01_empty_dir_creates_today},
    {"SOP-02", "today's clean segment is reused", test_sop02_today_reused},
    {"SOP-03", "a torn tail is recovered first", test_sop03_torn_tail_recovered},
    {"SOP-04", "a new day keeps yesterday's seq", test_sop04_new_day_keeps_seq},
    {"SOP-05", "a corrupt newest segment falls back", test_sop05_corrupt_newest_falls_back},
    {"SOP-06", "a header-only newest looks back for seq", test_sop06_header_only_looks_back},
    {"SOP-07", "a clock behind the log keeps appending", test_sop07_clock_behind_keeps_newest},
    {"SOP-08", "other files are ignored", test_sop08_other_files_ignored},
    {"SOP-09", "each step of creating can fail", test_sop09_create_failures},
    {"SOP-10", "a failed recovery fails open", test_sop10_recovery_failure},
    {"SOP-11", "close cleans up; info may be NULL", test_sop11_close_and_null_info},
    {"SOP-12", "bad arguments are rejected", test_sop12_bad_args},
    {"SOP-13", "open reports the newest record's time", test_sop13_last_timestamp_reported},
    {"SOP-14", "an older segment cut in the walk back", test_sop14_older_cut_reported},
    {"SOP-15", "a renamed older segment is reported", test_sop15_older_renamed_reported},
    {"SOP-16", "an empty log is told apart from a used one", test_sop16_empty_log_detected},
};

int main(void)
{
    return RUN_TESTS(tests);
}
