/* Unit tests for recovering one segment file (src/storage_recover.c):
 * gt_storage_recover_ops.
 *
 * Purpose: prove recovery does to the file exactly what DESIGN.md "What
 * happens when the power dies mid-write" says, through the file seam:
 * a clean segment is left alone, a damaged tail is truncated and fsynced
 * (so the cut itself survives the next power cut), and a segment with a
 * bad header is renamed to .corrupt and the directory fsynced. Failures of
 * any file op are reported, never hidden, and no handle or memory is
 * leaked on any path. Uses tests/fake_file.h, so no disk is touched. Run
 * under make test / asan / memcheck. Implements SRV-01 through SRV-10.
 *
 * What these tests cannot prove: that real ext4 behaves like the fake
 * (Mechanism D Step 12a's loopback tests and the on-Pi power pulls do),
 * or which segment storage picks to recover (Step 7). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage_recover.h"

#define SEGMENT_NAME   "log-20261026.bin"
#define CORRUPT_NAME   "log-20261026.bin.corrupt"
#define SEGMENT_MAX_RECORDS 8
#define SEGMENT_BUF_SIZE    (GT_SEG_HEADER_SZ + (SEGMENT_MAX_RECORDS + 1) * GT_REC_SIZE)
#define RECORD_OFFSET(record_index) (GT_SEG_HEADER_SZ + (record_index) * GT_REC_SIZE)

/* One fake directory shared by every case; fake_fs_ops() resets it. It is
 * static because a fake_fs is about 70 KB (see fake_file.h). */
static fake_fs fs;

/* Write a valid header, then one soil DATA record per entry of `seqs`,
 * into `segment`. Returns the segment's length in bytes. */
static size_t build_segment(uint8_t segment[SEGMENT_BUF_SIZE],
                            const uint32_t *seqs, size_t seq_count)
{
    const gt_seg_header header = { .created_at_us = 1000 };
    memset(segment, 0, SEGMENT_BUF_SIZE);
    CHECK_EQ(gt_seg_header_encode(&header, segment), GT_OK);

    for (size_t record_index = 0; record_index < seq_count; record_index++) {
        const gt_record record = {
            .type = GT_REC_DATA, .sensor_id = GT_SENSOR_SOIL,
            .seq = seqs[record_index], .timestamp_us = 2000 + record_index,
            .value = 800 + (uint32_t)record_index,
        };
        CHECK_EQ(gt_record_encode(&record, &segment[RECORD_OFFSET(record_index)]), GT_OK);
    }
    return RECORD_OFFSET(seq_count);
}

/* Reset the fake and put `len` bytes of `segment` in it as SEGMENT_NAME,
 * as if an earlier run left it there. Returns the ops. */
static const gt_file_ops *setup(const uint8_t *segment, size_t len)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    CHECK_EQ(fake_fs_put(&fs, SEGMENT_NAME, segment, len), GT_OK);
    return ops;
}

/* Recover SEGMENT_NAME and check the call returned `expected`; on any
 * other status the report must be untouched. Every path must leave no
 * handle open. */
static void recover_expect(const gt_file_ops *ops, gt_status expected, gt_recover_report *report)
{
    gt_recover_report untouched;
    memset(report, 0x5A, sizeof *report);
    memset(&untouched, 0x5A, sizeof untouched);

    CHECK_EQ(gt_storage_recover_ops(ops, &fs, SEGMENT_NAME, report), expected);
    if (expected != GT_OK) {
        CHECK(memcmp(report, &untouched, sizeof *report) == 0);
    }
    CHECK_EQ(fake_fs_open_handles(&fs), 0);
    CHECK_EQ(fs.bad_handle_calls, 0);
}

/* ---- SRV-01: a clean segment is left alone ----
 * No ftruncate, no fsync, no rename: recovery must not write to a good
 * file at all. */
static void test_srv01_clean_left_alone(void)
{
    const uint32_t seqs[] = { 3, 4, 5 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, seqs, 3);
    const gt_file_ops *ops = setup(segment, len);
    gt_recover_report report;

    recover_expect(ops, GT_OK, &report);
    CHECK_EQ(report.outcome, GT_RECOVER_CLEAN);
    CHECK(report.bytes_removed == 0);
    CHECK_EQ(report.records_removed, 0);
    CHECK_EQ(report.records_kept, 3);
    CHECK_EQ(report.has_records, 1);
    CHECK_EQ(report.last_seq, 5);
    CHECK_EQ(fs.ftruncate_calls, 0);
    CHECK_EQ(fs.fsync_calls, 0);
    CHECK_EQ(fs.rename_calls, 0);
    CHECK_EQ(fake_fs_file(&fs, SEGMENT_NAME)->len, len);
    CHECK(memcmp(fake_fs_file(&fs, SEGMENT_NAME)->data, segment, len) == 0);
}

/* ---- SRV-02: a header-only segment is clean ----
 * A new day's file with no records yet: has_records 0. */
static void test_srv02_header_only_clean(void)
{
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, NULL, 0);
    const gt_file_ops *ops = setup(segment, len);
    gt_recover_report report;

    recover_expect(ops, GT_OK, &report);
    CHECK_EQ(report.outcome, GT_RECOVER_CLEAN);
    CHECK_EQ(report.records_kept, 0);
    CHECK_EQ(report.has_records, 0);
    CHECK_EQ(report.last_seq, 0);
}

/* ---- SRV-03: a partial tail is cut and fsynced ----
 * 10 stray bytes after 2 good records. After recovery the file is exactly
 * the good prefix, and a power cut straight afterwards keeps the cut,
 * which proves the fsync came after the ftruncate. */
static void test_srv03_partial_tail_cut(void)
{
    const uint32_t seqs[] = { 1, 2 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t good_len = build_segment(segment, seqs, 2);
    memset(&segment[good_len], 0xA7, 10);
    const gt_file_ops *ops = setup(segment, good_len + 10);
    gt_recover_report report;

    recover_expect(ops, GT_OK, &report);
    CHECK_EQ(report.outcome, GT_RECOVER_TRUNCATED);
    CHECK(report.bytes_removed == 10);
    CHECK_EQ(report.records_removed, 1);
    CHECK_EQ(report.records_kept, 2);
    CHECK_EQ(report.last_seq, 2);
    CHECK_EQ(fs.ftruncate_calls, 1);
    CHECK_EQ(fs.fsync_calls, 1);

    fake_fs_power_cut(&fs);
    CHECK_EQ(fake_fs_file(&fs, SEGMENT_NAME)->len, good_len);
    CHECK(memcmp(fake_fs_file(&fs, SEGMENT_NAME)->data, segment, good_len) == 0);
}

/* ---- SRV-04: damage mid-file cuts from there ----
 * A CRC flip in record 1 of 4: records 1-3 (72 bytes) go, record 0 stays. */
static void test_srv04_mid_file_damage_cut(void)
{
    const uint32_t seqs[] = { 1, 2, 3, 4 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, seqs, 4);
    segment[RECORD_OFFSET(1) + 16] ^= 0x01;
    const gt_file_ops *ops = setup(segment, len);
    gt_recover_report report;

    recover_expect(ops, GT_OK, &report);
    CHECK_EQ(report.outcome, GT_RECOVER_TRUNCATED);
    CHECK(report.bytes_removed == 3 * GT_REC_SIZE);
    CHECK_EQ(report.records_removed, 3);
    CHECK_EQ(report.records_kept, 1);
    CHECK_EQ(report.last_seq, 1);
    CHECK_EQ(fake_fs_file(&fs, SEGMENT_NAME)->len, RECORD_OFFSET(1));
}

/* ---- SRV-05: a bad header is renamed to .corrupt ----
 * The bytes move unchanged (kept as evidence, never deleted), the old
 * name is gone, and the rename survives a power cut, which proves the
 * fsync_dir. */
static void test_srv05_bad_header_renamed(void)
{
    const uint32_t seqs[] = { 1, 2 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, seqs, 2);
    segment[0] = 'X';
    const gt_file_ops *ops = setup(segment, len);
    gt_recover_report report;

    recover_expect(ops, GT_OK, &report);
    CHECK_EQ(report.outcome, GT_RECOVER_RENAMED);
    CHECK(report.bytes_removed == len);
    CHECK_EQ(report.records_removed, 0);
    CHECK_EQ(report.records_kept, 0);
    CHECK_EQ(report.has_records, 0);
    CHECK_EQ(fs.ftruncate_calls, 0);

    fake_fs_power_cut(&fs);
    CHECK(fake_fs_file(&fs, SEGMENT_NAME) == NULL);
    CHECK(fake_fs_file(&fs, CORRUPT_NAME) != NULL);
    CHECK_EQ(fake_fs_file(&fs, CORRUPT_NAME)->len, len);
    CHECK(memcmp(fake_fs_file(&fs, CORRUPT_NAME)->data, segment, len) == 0);
}

/* ---- SRV-06: an empty or short file is renamed ----
 * 0 bytes and 10 bytes: a crash after the file was created but before its
 * header was synced. Both go to .corrupt like a bad header. */
static void test_srv06_short_file_renamed(void)
{
    const size_t short_lengths[] = { 0, 10 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    build_segment(segment, NULL, 0);

    for (size_t length_index = 0; length_index < 2; length_index++) {
        const gt_file_ops *ops = setup(segment, short_lengths[length_index]);
        gt_recover_report report;

        recover_expect(ops, GT_OK, &report);
        CHECK_EQ(report.outcome, GT_RECOVER_RENAMED);
        CHECK(report.bytes_removed == short_lengths[length_index]);
        CHECK(fake_fs_file(&fs, SEGMENT_NAME) == NULL);
        CHECK(fake_fs_file(&fs, CORRUPT_NAME) != NULL);
    }
}

/* ---- SRV-07: a missing segment is GT_ERR_IO ----
 * Nothing is created: recovery only repairs, it never makes files. */
static void test_srv07_missing_segment(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_recover_report report;

    recover_expect(ops, GT_ERR_IO, &report);
    CHECK(fake_fs_file(&fs, SEGMENT_NAME) == NULL);
}

/* ---- SRV-08: a failed truncate or fsync is reported ----
 * GT_ERR_IO, never GT_OK: a cut that isn't durable must not be reported
 * as done. */
static void test_srv08_truncate_fsync_failures(void)
{
    const uint32_t seqs[] = { 1 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, seqs, 1);
    gt_recover_report report;

    const gt_file_ops *ops = setup(segment, len + 5);
    fs.fail_ftruncate_call = 1;
    recover_expect(ops, GT_ERR_IO, &report);

    ops = setup(segment, len + 5);
    fs.fail_fsync_call = 1;
    recover_expect(ops, GT_ERR_IO, &report);
}

/* ---- SRV-09: a failed rename or fsync_dir is reported ---- */
static void test_srv09_rename_failures(void)
{
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, NULL, 0);
    segment[0] = 'X';
    gt_recover_report report;

    const gt_file_ops *ops = setup(segment, len);
    fs.fail_rename_call = 1;
    recover_expect(ops, GT_ERR_IO, &report);
    CHECK(fake_fs_file(&fs, SEGMENT_NAME) != NULL);

    ops = setup(segment, len);
    fs.fail_fsync_dir_call = 1;
    recover_expect(ops, GT_ERR_IO, &report);
}

/* ---- SRV-10: bad arguments are rejected ----
 * NULL args, and a name with no room for ".corrupt" (it could never be
 * renamed). Nothing is touched. */
static void test_srv10_bad_args(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_recover_report report;
    char long_name[GT_FILE_NAME_MAX];
    memset(long_name, 'a', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';   /* 31 chars: fits a row, not + ".corrupt" */

    CHECK_EQ(gt_storage_recover_ops(NULL, &fs, SEGMENT_NAME, &report), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_recover_ops(ops, &fs, NULL, &report), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_recover_ops(ops, &fs, SEGMENT_NAME, NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_recover_ops(ops, &fs, long_name, &report), GT_ERR_INVAL);
    CHECK_EQ(fs.open_calls, 0);
}

static const test_case_t tests[] = {
    {"SRV-01", "a clean segment is left alone", test_srv01_clean_left_alone},
    {"SRV-02", "a header-only segment is clean", test_srv02_header_only_clean},
    {"SRV-03", "a partial tail is cut and fsynced", test_srv03_partial_tail_cut},
    {"SRV-04", "damage mid-file cuts from there", test_srv04_mid_file_damage_cut},
    {"SRV-05", "a bad header is renamed to .corrupt", test_srv05_bad_header_renamed},
    {"SRV-06", "an empty or short file is renamed", test_srv06_short_file_renamed},
    {"SRV-07", "a missing segment is GT_ERR_IO", test_srv07_missing_segment},
    {"SRV-08", "a failed truncate or fsync is reported", test_srv08_truncate_fsync_failures},
    {"SRV-09", "a failed rename or fsync_dir is reported", test_srv09_rename_failures},
    {"SRV-10", "bad arguments are rejected", test_srv10_bad_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
