/* Unit tests for the clean-shutdown marker (src/storage.c):
 * gt_storage_shutdown, and how gt_storage_open_ops reads the marker.
 *
 * Purpose: prove DESIGN.md's shutdown rules. A clean shutdown commits
 * every pending record and only then writes a durable marker; a failed
 * final flush writes no marker, so a shutdown that lost data is never
 * called clean. At the next open a missing marker (with segments on disk)
 * is reported as unclean_shutdown, a fresh install is not, and a present
 * marker is deleted durably before storage accepts data. Uses
 * tests/fake_file.h. Run under make test / asan / memcheck. Implements
 * SCL-01 through SCL-10.
 *
 * What these tests cannot prove: that SIGTERM reaches shutdown (that is
 * storage_main, Mechanism D Step 12d), or the 20-SIGTERM on-Pi test. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TODAY_NAME    "log-20261026.bin"
#define TEST_CAPACITY 8
#define RECORD_OFFSET(record_index) (GT_SEG_HEADER_SZ + (record_index) * GT_REC_SIZE)

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* Encode a light DATA record with `seq` into `wire`. */
static void make_wire(uint8_t wire[GT_REC_SIZE], uint32_t seq)
{
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .seq = seq, .timestamp_us = 6000 + seq, .value = 200 + seq,
    };
    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
}

/* Open storage over the fake as it is now (the caller set it up). */
static gt_storage *open_storage(const gt_file_ops *ops, gt_storage_open_info *info)
{
    gt_storage *storage = NULL;
    CHECK_EQ(gt_storage_open_ops(ops, &fs, TODAY_US, TEST_CAPACITY, &storage, info), GT_OK);
    return storage;
}

/* Append records seq first .. last. */
static void append_range(gt_storage *storage, uint32_t first, uint32_t last)
{
    /* invariant: records first .. seq - 1 have been appended */
    for (uint32_t seq = first; seq <= last; seq++) {
        uint8_t wire[GT_REC_SIZE];
        make_wire(wire, seq);
        CHECK_EQ(gt_storage_append(storage, wire, 0, NULL), GT_OK);
    }
}

/* Shut down and check the status and the events. */
static void shutdown_expect(gt_storage *storage, gt_status expected_status,
                            unsigned expected_events)
{
    unsigned events = 0xFFu;   /* shutdown must reset it */
    CHECK_EQ(gt_storage_shutdown(storage, &events), expected_status);
    CHECK_EQ(events, expected_events);
}

/* 1 if the marker would survive a power cut right now (its directory
 * entry is synced). */
static int marker_durable(void)
{
    const fake_file *marker = fake_fs_file(&fs, GT_STORAGE_MARKER_NAME);
    return marker != NULL && marker->synced_used;
}

/* Put today's segment with records 1 .. record_count, and the marker if
 * with_marker is 1: what an earlier run left behind. */
static void put_earlier_run(uint32_t record_count, int with_marker)
{
    uint8_t segment[RECORD_OFFSET(8)];
    const gt_seg_header header = { .created_at_us = 1000 };
    CHECK_EQ(gt_seg_header_encode(&header, segment), GT_OK);
    /* invariant: records 1 .. seq - 1 are encoded after the header */
    for (uint32_t seq = 1; seq <= record_count; seq++) {
        make_wire(&segment[RECORD_OFFSET(seq - 1)], seq);
    }
    CHECK_EQ(fake_fs_put(&fs, TODAY_NAME, segment, RECORD_OFFSET(record_count)), GT_OK);
    if (with_marker) {
        CHECK_EQ(fake_fs_put(&fs, GT_STORAGE_MARKER_NAME, NULL, 0), GT_OK);
    }
}

/* ---- SCL-01: shutdown commits, then marks ----
 * Both pending records are durable, and so is the marker. */
static void test_scl01_shutdown_commits_then_marks(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, NULL);
    gt_storage_stats stats;
    append_range(storage, 1, 2);

    shutdown_expect(storage, GT_OK, 0);
    CHECK_EQ(fake_fs_file(&fs, TODAY_NAME)->synced_len, RECORD_OFFSET(2));
    CHECK_EQ(marker_durable(), 1);
    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    CHECK_EQ(stats.pending, 0);
    CHECK(stats.committed_records == 2);
    gt_storage_close(storage);
    CHECK_EQ(fs.destroy_calls, 1);
    CHECK_EQ(fake_fs_open_handles(&fs), 0);
}

/* ---- SCL-02: an idle shutdown still marks ----
 * Nothing pending: no segment write, but the marker is made. */
static void test_scl02_idle_shutdown_marks(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, NULL);
    int pwrites_before = fs.pwrite_calls;

    shutdown_expect(storage, GT_OK, 0);
    CHECK_EQ(fs.pwrite_calls, pwrites_before);
    CHECK_EQ(marker_durable(), 1);
    gt_storage_close(storage);
}

/* ---- SCL-03: a failed final flush writes no marker ----
 * The flush's own events are passed on, the records stay pending, and
 * there is no marker, so the next open calls this run unclean. */
static void test_scl03_failed_flush_no_marker(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, NULL);
    gt_storage_stats stats;
    append_range(storage, 1, 2);
    fs.fail_pwrite_call = fs.pwrite_calls + 1;

    shutdown_expect(storage, GT_ERR_IO, GT_STORAGE_EV_WRITE_ERROR | GT_STORAGE_EV_DEGRADED);
    CHECK(fake_fs_file(&fs, GT_STORAGE_MARKER_NAME) == NULL);
    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    CHECK_EQ(stats.pending, 2);
    gt_storage_close(storage);
}

/* ---- SCL-04: a marker that can't be made is reported ----
 * The records are committed; only the marker failed: creating it, then
 * (in a second run) its fsync_dir. Both are GT_ERR_IO with WRITE_ERROR. */
static void test_scl04_marker_failure_reported(void)
{
    for (int failing_step = 0; failing_step < 2; failing_step++) {
        const gt_file_ops *ops = fake_fs_ops(&fs);
        gt_storage *storage = open_storage(ops, NULL);
        gt_storage_stats stats;
        append_range(storage, 1, 1);
        if (failing_step == 0) {
            fs.fail_open_call = fs.open_calls + 1;
        } else {
            fs.fail_fsync_dir_call = fs.fsync_dir_calls + 1;
        }

        shutdown_expect(storage, GT_ERR_IO, GT_STORAGE_EV_WRITE_ERROR);
        CHECK_EQ(marker_durable(), 0);
        CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
        CHECK(stats.committed_records == 1);
        gt_storage_close(storage);
        CHECK_EQ(fake_fs_open_handles(&fs), 0);
    }
}

/* ---- SCL-05: open after a clean shutdown ----
 * Not unclean, and the marker is deleted durably: after close and a
 * power cut it is still gone. (close writes nothing, so the power cut
 * shows exactly what open made durable.) */
static void test_scl05_open_after_clean_shutdown(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage_open_info info;
    put_earlier_run(3, 1);
    gt_storage *storage = open_storage(ops, &info);

    CHECK_EQ(info.unclean_shutdown, 0);
    CHECK(fake_fs_file(&fs, GT_STORAGE_MARKER_NAME) == NULL);
    gt_storage_close(storage);
    fake_fs_power_cut(&fs);
    CHECK(fake_fs_file(&fs, GT_STORAGE_MARKER_NAME) == NULL);
}

/* ---- SCL-06: open after a crash reports unclean ----
 * Segments on disk, no marker: the last run was killed or lost power. */
static void test_scl06_open_after_crash_unclean(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage_open_info info;
    put_earlier_run(3, 0);
    gt_storage *storage = open_storage(ops, &info);

    CHECK_EQ(info.unclean_shutdown, 1);
    CHECK_EQ(info.last_seq, 3);
    gt_storage_close(storage);
}

/* ---- SCL-07: a fresh install is not unclean ----
 * An empty directory has no marker but nothing to recover either. */
static void test_scl07_fresh_install_not_unclean(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage_open_info info;
    gt_storage *storage = open_storage(ops, &info);

    CHECK_EQ(info.unclean_shutdown, 0);
    gt_storage_close(storage);
}

/* ---- SCL-08: a failed marker delete fails open ----
 * If the delete can't be made durable, the marker could outlive this run
 * and hide a later crash, so open fails (destroy once, no handles). */
static void test_scl08_marker_delete_failure(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = NULL;
    put_earlier_run(2, 1);
    fs.fail_fsync_dir_call = 1;   /* the only fsync_dir: today's segment is reused */

    CHECK_EQ(gt_storage_open_ops(ops, &fs, TODAY_US, TEST_CAPACITY, &storage, NULL), GT_ERR_IO);
    CHECK(storage == NULL);
    CHECK_EQ(fs.destroy_calls, 1);
    CHECK_EQ(fake_fs_open_handles(&fs), 0);
}

/* ---- SCL-09: a full run round-trips ----
 * Open empty, append 1-3, shut down, close; reopen: clean, seq 3, marker
 * gone, and the next record continues at 4. */
static void test_scl09_full_round_trip(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage_open_info info;
    gt_storage *storage = open_storage(ops, NULL);
    append_range(storage, 1, 3);
    shutdown_expect(storage, GT_OK, 0);
    gt_storage_close(storage);

    fs.destroy_calls = 0;
    storage = open_storage(ops, &info);
    CHECK_EQ(info.unclean_shutdown, 0);
    CHECK_EQ(info.has_last, 1);
    CHECK_EQ(info.last_seq, 3);
    CHECK(fake_fs_file(&fs, GT_STORAGE_MARKER_NAME) == NULL);
    append_range(storage, 4, 4);
    gt_storage_close(storage);
    CHECK_EQ(fs.destroy_calls, 1);
}

/* ---- SCL-10: NULL storage is rejected ----
 * And events_out may be NULL. */
static void test_scl10_null_args(void)
{
    unsigned events = 0xFFu;
    CHECK_EQ(gt_storage_shutdown(NULL, &events), GT_ERR_INVAL);
    CHECK_EQ(events, 0);

    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = open_storage(ops, NULL);
    CHECK_EQ(gt_storage_shutdown(storage, NULL), GT_OK);
    gt_storage_close(storage);
}

static const test_case_t tests[] = {
    {"SCL-01", "shutdown commits, then marks", test_scl01_shutdown_commits_then_marks},
    {"SCL-02", "an idle shutdown still marks", test_scl02_idle_shutdown_marks},
    {"SCL-03", "a failed final flush writes no marker", test_scl03_failed_flush_no_marker},
    {"SCL-04", "a marker that can't be made is reported", test_scl04_marker_failure_reported},
    {"SCL-05", "open after a clean shutdown", test_scl05_open_after_clean_shutdown},
    {"SCL-06", "open after a crash reports unclean", test_scl06_open_after_crash_unclean},
    {"SCL-07", "a fresh install is not unclean", test_scl07_fresh_install_not_unclean},
    {"SCL-08", "a failed marker delete fails open", test_scl08_marker_delete_failure},
    {"SCL-09", "a full run round-trips", test_scl09_full_round_trip},
    {"SCL-10", "NULL storage is rejected", test_scl10_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
