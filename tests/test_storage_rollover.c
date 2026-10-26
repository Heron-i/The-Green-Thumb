/* Unit tests for daily rollover and retention (src/storage.c):
 * gt_storage_rollover.
 *
 * Purpose: prove DESIGN.md "With what retention" and the clock rows of
 * the failure-mode table. A new UTC day gets a new, durable segment and
 * the next flush writes into it. Nothing rolls over or is deleted while
 * the clock is unsynced, and a clock that went back keeps the active
 * segment. A failed create keeps the old segment and is retried. Old
 * segments are deleted only when outside the window and not among the
 * newest N, so a clock that jumps years ahead can't wipe the history.
 * Non-segment files are never deleted, and a failed delete is reported
 * while the others still go. A rollover while degraded first cuts the old
 * segment's torn tail, or waits if it can't. Uses tests/fake_file.h with a
 * 3-day window (the fake holds 8 files). Run under make test / asan /
 * memcheck. Implements SRO-01 through SRO-12.
 *
 * What these tests cannot prove: a real midnight on the Pi during the
 * soak, or how storage_main learns the clock is synced (Step 12d). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"

#define US_PER_S        1000000u
#define TODAY_US        ((uint64_t)(1792972800u + 3600u) * US_PER_S)   /* 2026-10-26 01:00 UTC */
#define TOMORROW_US     ((uint64_t)1793059200u * US_PER_S)             /* 2026-10-27 00:00 UTC */
#define TEN_YEARS_US    ((uint64_t)2108595600u * US_PER_S)             /* 2036-10-26 01:00 UTC */
#define TODAY_NAME      "log-20261026.bin"
#define TOMORROW_NAME   "log-20261027.bin"
#define TEST_CAPACITY   8
#define TEST_RETENTION  3   /* keep today and the 2 days before it */

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* Put a header-only segment called `name`, as an earlier day left it. */
static void put_old_segment(const char *name)
{
    uint8_t header_bytes[GT_SEG_HEADER_SZ];
    const gt_seg_header header = { .created_at_us = 1000 };
    CHECK_EQ(gt_seg_header_encode(&header, header_bytes), GT_OK);
    CHECK_EQ(fake_fs_put(&fs, name, header_bytes, sizeof header_bytes), GT_OK);
}

/* Open storage over the fake as it is now, at TODAY_US. With no segment
 * from today or later in it, open creates TODAY_NAME. */
static gt_storage *open_today(const gt_file_ops *ops)
{
    gt_storage *storage = NULL;
    CHECK_EQ(gt_storage_open_ops(ops, &fs, TODAY_US, TEST_CAPACITY, &storage, NULL), GT_OK);
    return storage;
}

/* Append a light DATA record with `seq`, flush it, and return which file
 * grew by one record: 1 if `name` did. */
static int flush_lands_in(gt_storage *storage, uint32_t seq, const char *name)
{
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .seq = seq, .timestamp_us = 100 + seq, .value = seq,
    };
    uint8_t wire[GT_REC_SIZE];
    const fake_file *file = fake_fs_file(&fs, name);
    size_t len_before = file != NULL ? file->synced_len : 0;

    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
    CHECK_EQ(gt_storage_append(storage, wire, 0, NULL), GT_OK);
    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);
    file = fake_fs_file(&fs, name);
    return file != NULL && file->synced_len == len_before + GT_REC_SIZE;
}

/* Roll over and check the status and the events. */
static void rollover_expect(gt_storage *storage, uint64_t now_us, int clock_synced,
                            gt_status expected_status, unsigned expected_events)
{
    unsigned events = 0xFFFFu;   /* rollover must reset it */
    CHECK_EQ(gt_storage_rollover(storage, now_us, clock_synced, TEST_RETENTION, &events),
             expected_status);
    CHECK_EQ(events, expected_events);
}

/* Read the stats. */
static gt_storage_stats read_stats(const gt_storage *storage)
{
    gt_storage_stats stats;
    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    return stats;
}

/* ---- SRO-01: the same day does not roll over ----
 * Later that day: no event, no new file opened, and records still land in
 * today's segment. */
static void test_sro01_same_day_no_rollover(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_today(ops);
    int opens_before = fs.open_calls;

    rollover_expect(storage, TODAY_US + (uint64_t)20 * 3600 * US_PER_S, 1, GT_OK, 0);
    CHECK_EQ(fs.open_calls, opens_before);
    CHECK_EQ(flush_lands_in(storage, 1, TODAY_NAME), 1);
    gt_storage_close(storage);
}

/* ---- SRO-02: a new day gets a new durable segment ----
 * At midnight a record is pending. Rollover creates tomorrow's segment
 * (header already synced, name synced), and the next flush writes the
 * record there, not into today's. */
static void test_sro02_new_day_new_segment(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_today(ops);
    CHECK_EQ(flush_lands_in(storage, 1, TODAY_NAME), 1);

    rollover_expect(storage, TOMORROW_US, 1, GT_OK, GT_STORAGE_EV_ROLLOVER);
    const fake_file *tomorrow = fake_fs_file(&fs, TOMORROW_NAME);
    CHECK(tomorrow != NULL);
    if (tomorrow != NULL) {
        gt_seg_header header;
        CHECK_EQ(tomorrow->synced_used, 1);
        CHECK_EQ(tomorrow->synced_len, GT_SEG_HEADER_SZ);
        CHECK_EQ(gt_seg_header_decode(tomorrow->synced_data, &header), GT_OK);
        CHECK(header.created_at_us == TOMORROW_US);
    }

    CHECK_EQ(flush_lands_in(storage, 2, TOMORROW_NAME), 1);
    CHECK_EQ(fake_fs_file(&fs, TODAY_NAME)->synced_len, GT_SEG_HEADER_SZ + GT_REC_SIZE);
    CHECK_EQ(fake_fs_open_handles(&fs), 1);   /* only the new active segment */
    gt_storage_close(storage);
    CHECK_EQ(fake_fs_open_handles(&fs), 0);
}

/* ---- SRO-03: an unsynced clock changes nothing ----
 * Even a day later: RETENTION_SKIPPED, no new segment, keep appending to
 * the current one. */
static void test_sro03_unsynced_changes_nothing(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_today(ops);

    rollover_expect(storage, TOMORROW_US, 0, GT_OK, GT_STORAGE_EV_RETENTION_SKIPPED);
    CHECK(fake_fs_file(&fs, TOMORROW_NAME) == NULL);
    CHECK_EQ(flush_lands_in(storage, 1, TODAY_NAME), 1);
    gt_storage_close(storage);
}

/* ---- SRO-04: a clock gone back keeps the segment ----
 * A synced clock reading 1970: no log-19700101.bin, no event. */
static void test_sro04_clock_back_keeps_segment(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_today(ops);

    rollover_expect(storage, 0, 1, GT_OK, 0);
    CHECK(fake_fs_file(&fs, "log-19700101.bin") == NULL);
    CHECK_EQ(flush_lands_in(storage, 1, TODAY_NAME), 1);
    gt_storage_close(storage);
}

/* ---- SRO-05: a failed create keeps the old segment ----
 * The create fails, then in a second run the header write fails (leaving
 * an empty tomorrow file). Each time: GT_ERR_IO, SEGMENT_CREATE_ERROR,
 * records still go to today's segment, only one handle open. The next
 * rollover succeeds. */
static void test_sro05_failed_create_retried(void)
{
    for (int failing_step = 0; failing_step < 2; failing_step++) {
        const gt_file_ops *ops = fake_fs_ops(&fs);
        gt_storage *storage = open_today(ops);
        if (failing_step == 0) {
            fs.fail_open_call = fs.open_calls + 1;
        } else {
            fs.fail_pwrite_call = fs.pwrite_calls + 1;
        }

        rollover_expect(storage, TOMORROW_US, 1, GT_ERR_IO, GT_STORAGE_EV_SEGMENT_CREATE_ERROR);
        CHECK_EQ(fake_fs_open_handles(&fs), 1);
        CHECK_EQ(flush_lands_in(storage, 1, TODAY_NAME), 1);

        rollover_expect(storage, TOMORROW_US, 1, GT_OK, GT_STORAGE_EV_ROLLOVER);
        CHECK_EQ(fake_fs_file(&fs, TOMORROW_NAME)->synced_len, GT_SEG_HEADER_SZ);
        CHECK_EQ(flush_lands_in(storage, 2, TOMORROW_NAME), 1);
        gt_storage_close(storage);
        CHECK_EQ(fake_fs_open_handles(&fs), 0);
    }
}

/* ---- SRO-06: only segments past the window go ----
 * Segments for the 20th-25th, today's (26th) made by open, tomorrow's
 * made by rollover. A 3-day window keeps the 25th-27th; the 20th-24th
 * are deleted. */
static void test_sro06_old_segments_deleted(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const char *old_names[] = {
        "log-20261020.bin", "log-20261021.bin", "log-20261022.bin",
        "log-20261023.bin", "log-20261024.bin", "log-20261025.bin",
    };
    for (size_t name_index = 0; name_index < 6; name_index++) {
        put_old_segment(old_names[name_index]);
    }
    gt_storage *storage = open_today(ops);

    rollover_expect(storage, TOMORROW_US, 1, GT_OK, GT_STORAGE_EV_ROLLOVER);
    for (size_t name_index = 0; name_index < 5; name_index++) {
        CHECK(fake_fs_file(&fs, old_names[name_index]) == NULL);
    }
    CHECK(fake_fs_file(&fs, "log-20261025.bin") != NULL);
    CHECK(fake_fs_file(&fs, TODAY_NAME) != NULL);
    CHECK(fake_fs_file(&fs, TOMORROW_NAME) != NULL);
    CHECK(read_stats(storage).segments_deleted == 5);
    gt_storage_close(storage);
}

/* ---- SRO-07: a clock years ahead keeps the newest N ----
 * A synced clock jumps to 2036. Every 2026 segment is outside the window,
 * but the newest 3 segments (the new 2036 one, the 26th and the 25th)
 * are kept; only the 22nd-24th go. */
static void test_sro07_jump_ahead_keeps_newest(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    put_old_segment("log-20261022.bin");
    put_old_segment("log-20261023.bin");
    put_old_segment("log-20261024.bin");
    put_old_segment("log-20261025.bin");
    gt_storage *storage = open_today(ops);

    rollover_expect(storage, TEN_YEARS_US, 1, GT_OK, GT_STORAGE_EV_ROLLOVER);
    CHECK(fake_fs_file(&fs, "log-20361026.bin") != NULL);
    CHECK(fake_fs_file(&fs, TODAY_NAME) != NULL);
    CHECK(fake_fs_file(&fs, "log-20261025.bin") != NULL);
    CHECK(fake_fs_file(&fs, "log-20261024.bin") == NULL);
    CHECK(fake_fs_file(&fs, "log-20261023.bin") == NULL);
    CHECK(fake_fs_file(&fs, "log-20261022.bin") == NULL);
    CHECK(read_stats(storage).segments_deleted == 3);
    gt_storage_close(storage);
}

/* ---- SRO-08: other files are never deleted ----
 * An old .corrupt file and a stray file survive. Of the segments, the
 * 20th goes; the 21st is outside the window but still among the newest 3
 * (27th, 26th, 21st), so it stays. */
static void test_sro08_other_files_kept(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const uint8_t stray = 1;
    put_old_segment("log-20261001.bin.corrupt");
    CHECK_EQ(fake_fs_put(&fs, "notes.txt", &stray, 1), GT_OK);
    put_old_segment("log-20261020.bin");
    put_old_segment("log-20261021.bin");
    gt_storage *storage = open_today(ops);

    rollover_expect(storage, TOMORROW_US, 1, GT_OK, GT_STORAGE_EV_ROLLOVER);
    CHECK(fake_fs_file(&fs, "log-20261001.bin.corrupt") != NULL);
    CHECK(fake_fs_file(&fs, "notes.txt") != NULL);
    CHECK(fake_fs_file(&fs, "log-20261020.bin") == NULL);
    CHECK(fake_fs_file(&fs, "log-20261021.bin") != NULL);
    CHECK(read_stats(storage).segments_deleted == 1);
    gt_storage_close(storage);
}

/* ---- SRO-09: a failed delete is reported, rest go ----
 * Three candidates (20th-22nd); deleting the 20th fails. RETENTION_ERROR,
 * still GT_OK, the 21st and 22nd are gone and the 20th is still there. */
static void test_sro09_failed_delete_reported(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    put_old_segment("log-20261020.bin");
    put_old_segment("log-20261021.bin");
    put_old_segment("log-20261022.bin");
    put_old_segment("log-20261023.bin");
    gt_storage *storage = open_today(ops);
    fs.fail_unlink_call = fs.unlink_calls + 1;

    rollover_expect(storage, TOMORROW_US, 1, GT_OK,
                    GT_STORAGE_EV_ROLLOVER | GT_STORAGE_EV_RETENTION_ERROR);
    CHECK(fake_fs_file(&fs, "log-20261020.bin") != NULL);
    CHECK(fake_fs_file(&fs, "log-20261021.bin") == NULL);
    CHECK(fake_fs_file(&fs, "log-20261022.bin") == NULL);
    CHECK(fake_fs_file(&fs, "log-20261023.bin") != NULL);   /* among the newest 3 */
    CHECK(read_stats(storage).segments_deleted == 2);
    gt_storage_close(storage);
}

/* Open today, append records 1 and 2, and make their flush fail with 10
 * torn bytes left at the end of today's segment: storage is degraded. */
static gt_storage *open_degraded_with_torn_tail(const gt_file_ops *ops)
{
    gt_storage *storage = open_today(ops);
    /* invariant: records 1 .. seq - 1 are pending */
    for (uint32_t seq = 1; seq <= 2; seq++) {
        const gt_record record = {
            .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
            .seq = seq, .timestamp_us = 100 + seq, .value = seq,
        };
        uint8_t wire[GT_REC_SIZE];
        CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
        CHECK_EQ(gt_storage_append(storage, wire, 0, NULL), GT_OK);
    }
    fs.fail_pwrite_call = fs.pwrite_calls + 1;
    fs.pwrite_torn_bytes = 10;
    CHECK_EQ(gt_storage_flush(storage, NULL), GT_ERR_IO);
    CHECK_EQ(fake_fs_file(&fs, TODAY_NAME)->len, GT_SEG_HEADER_SZ + 10);
    CHECK_EQ(read_stats(storage).degraded, 1);
    return storage;
}

/* ---- SRO-11: a degraded rollover cuts the old tail ----
 * Midnight arrives while a flush has failed. Yesterday's segment is cut
 * back to its committed length (just the header) and fsynced before the
 * switch, since recovery would never repair a segment that is no longer
 * the newest. The pending records then land in the new segment. */
static void test_sro11_degraded_rollover_cuts_tail(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_degraded_with_torn_tail(ops);

    rollover_expect(storage, TOMORROW_US, 1, GT_OK, GT_STORAGE_EV_ROLLOVER);
    const fake_file *yesterday = fake_fs_file(&fs, TODAY_NAME);
    CHECK_EQ(yesterday->len, GT_SEG_HEADER_SZ);
    CHECK_EQ(yesterday->synced_len, GT_SEG_HEADER_SZ);

    unsigned events = 0;
    CHECK_EQ(gt_storage_flush(storage, &events), GT_OK);
    CHECK_EQ(events, GT_STORAGE_EV_RECOVERED);
    CHECK_EQ(fake_fs_file(&fs, TOMORROW_NAME)->synced_len, GT_SEG_HEADER_SZ + 2 * GT_REC_SIZE);
    gt_storage_close(storage);
}

/* ---- SRO-12: a failed tail cut delays the rollover ----
 * If the torn bytes can't be cut, the switch would strand them for good,
 * so it waits: WRITE_ERROR, GT_ERR_IO, no new segment, the old one still
 * active. The next call cuts and rolls over. */
static void test_sro12_failed_cut_delays_rollover(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_degraded_with_torn_tail(ops);
    fs.fail_ftruncate_call = fs.ftruncate_calls + 1;

    rollover_expect(storage, TOMORROW_US, 1, GT_ERR_IO, GT_STORAGE_EV_WRITE_ERROR);
    CHECK(fake_fs_file(&fs, TOMORROW_NAME) == NULL);
    CHECK_EQ(fake_fs_open_handles(&fs), 1);

    rollover_expect(storage, TOMORROW_US, 1, GT_OK, GT_STORAGE_EV_ROLLOVER);
    CHECK_EQ(fake_fs_file(&fs, TODAY_NAME)->synced_len, GT_SEG_HEADER_SZ);
    CHECK(fake_fs_file(&fs, TOMORROW_NAME) != NULL);
    gt_storage_close(storage);
}

/* ---- SRO-10: bad arguments are rejected ----
 * NULL storage and a year past 9999 are GT_ERR_INVAL; events_out may be
 * NULL. */
static void test_sro10_bad_args(void)
{
    unsigned events = 0xFFFFu;
    CHECK_EQ(gt_storage_rollover(NULL, TODAY_US, 1, 0, &events), GT_ERR_INVAL);
    CHECK_EQ(events, 0);

    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_today(ops);
    CHECK_EQ(gt_storage_rollover(storage, UINT64_MAX, 1, 0, &events), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_rollover(storage, TODAY_US, 1, 0, NULL), GT_OK);
    gt_storage_close(storage);
}

static const test_case_t tests[] = {
    {"SRO-01", "the same day does not roll over", test_sro01_same_day_no_rollover},
    {"SRO-02", "a new day gets a new durable segment", test_sro02_new_day_new_segment},
    {"SRO-03", "an unsynced clock changes nothing", test_sro03_unsynced_changes_nothing},
    {"SRO-04", "a clock gone back keeps the segment", test_sro04_clock_back_keeps_segment},
    {"SRO-05", "a failed create keeps the old segment", test_sro05_failed_create_retried},
    {"SRO-06", "only segments past the window go", test_sro06_old_segments_deleted},
    {"SRO-07", "a clock years ahead keeps the newest N", test_sro07_jump_ahead_keeps_newest},
    {"SRO-08", "other files are never deleted", test_sro08_other_files_kept},
    {"SRO-09", "a failed delete is reported, rest go", test_sro09_failed_delete_reported},
    {"SRO-10", "bad arguments are rejected", test_sro10_bad_args},
    {"SRO-11", "a degraded rollover cuts the old tail", test_sro11_degraded_rollover_cuts_tail},
    {"SRO-12", "a failed tail cut delays the rollover", test_sro12_failed_cut_delays_rollover},
};

int main(void)
{
    return RUN_TESTS(tests);
}
