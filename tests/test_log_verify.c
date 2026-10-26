/* Unit tests for the offline log verifier (src/log_verify.c):
 * gt_verify_dir_ops.
 *
 * Purpose: prove the verifier passes every log storage can legally write
 * (several days, storage GAPs covering a jump, the hub's own GAPs,
 * .corrupt files set aside by recovery) and fails, at the right segment
 * and byte offset, every log DESIGN.md says must never be readable: a bad
 * header, a bad record, a partial tail, seq going backwards (also across
 * days), and a jump in seq no GAP explains. And that it only ever reads.
 * Segments are written into tests/fake_file.h by hand. Run under make test
 * / asan / memcheck. Implements VFY-01 through VFY-10.
 *
 * What these tests cannot prove: the comparison with the hub's sent-seq
 * log ("lost = sent - committed - covered"), which needs the hub. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "log_verify.h"

#define DAY_25 "log-20261025.bin"
#define DAY_26 "log-20261026.bin"
#define RECORD_OFFSET(record_index) (GT_SEG_HEADER_SZ + (record_index) * GT_REC_SIZE)
#define MAX_SPECS 8

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* One record to write: DATA from the light sensor unless `type` is GAP. */
typedef struct {
    uint8_t  type;
    uint8_t  sensor_id;
    uint32_t seq;
    uint32_t value;
} record_spec;

#define DATA(seq_number) { GT_REC_DATA, GT_SENSOR_LIGHT, (seq_number), 0 }

/* Write a header and the records in `specs` into the fake as `name`, plus
 * `tail_bytes` stray bytes after them. */
static void put_segment(const char *name, const record_spec *specs, size_t spec_count,
                        size_t tail_bytes)
{
    uint8_t segment[RECORD_OFFSET(MAX_SPECS) + GT_REC_SIZE];
    const gt_seg_header header = { .created_at_us = 1000 };
    memset(segment, 0xA7, sizeof segment);
    CHECK_EQ(gt_seg_header_encode(&header, segment), GT_OK);
    for (size_t spec_index = 0; spec_index < spec_count; spec_index++) {
        const gt_record record = {
            .type = specs[spec_index].type, .sensor_id = specs[spec_index].sensor_id,
            .seq = specs[spec_index].seq, .timestamp_us = 5000 + spec_index,
            .value = specs[spec_index].value,
        };
        CHECK_EQ(gt_record_encode(&record, &segment[RECORD_OFFSET(spec_index)]), GT_OK);
    }
    CHECK_EQ(fake_fs_put(&fs, name, segment, RECORD_OFFSET(spec_count) + tail_bytes), GT_OK);
}

/* Verify the fake directory, checking the call itself works and that
 * nothing was written, renamed, cut or deleted. */
static gt_verify_report verify(const gt_file_ops *ops)
{
    gt_verify_report report;
    memset(&report, 0x5A, sizeof report);
    CHECK_EQ(gt_verify_dir_ops(ops, &fs, &report), GT_OK);
    CHECK_EQ(fs.pwrite_calls, 0);
    CHECK_EQ(fs.ftruncate_calls, 0);
    CHECK_EQ(fs.rename_calls, 0);
    CHECK_EQ(fs.unlink_calls, 0);
    CHECK_EQ(fake_fs_open_handles(&fs), 0);
    return report;
}

/* Check a failed report: its failure name, segment and offset. */
static void check_failed(const gt_verify_report *report, const char *failure,
                         const char *segment, uint64_t offset)
{
    CHECK_EQ(report->ok, 0);
    CHECK(report->failure != NULL && strcmp(report->failure, failure) == 0);
    CHECK(strcmp(report->failed_segment, segment) == 0);
    CHECK(report->failed_offset == offset);
}

/* ---- VFY-01: an empty directory passes ---- */
static void test_vfy01_empty_dir_passes(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_verify_report report = verify(ops);

    CHECK_EQ(report.ok, 1);
    CHECK_EQ(report.segments, 0);
    CHECK_EQ(report.has_records, 0);
    CHECK(report.failure == NULL);
}

/* ---- VFY-02: a clean log over two days passes ----
 * Seqs 1-3 on the 25th, 4-6 on the 26th: one unbroken stream. */
static void test_vfy02_clean_two_days(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const record_spec day25[] = { DATA(1), DATA(2), DATA(3) };
    const record_spec day26[] = { DATA(4), DATA(5), DATA(6) };
    put_segment(DAY_25, day25, 3, 0);
    put_segment(DAY_26, day26, 3, 0);
    gt_verify_report report = verify(ops);

    CHECK_EQ(report.ok, 1);
    CHECK_EQ(report.segments, 2);
    CHECK(report.data_records == 6);
    CHECK(report.gap_records == 0);
    CHECK_EQ(report.has_records, 1);
    CHECK_EQ(report.first_seq, 1);
    CHECK_EQ(report.last_seq, 6);
}

/* ---- VFY-03: GAPs that explain the numbering pass ----
 * A storage GAP covers a jump: 1, 2, GAP(3, 3), 6. Then a hub GAP
 * (refused light readings: value 5) with its own seq 7, followed by 8.
 * gap_readings adds up every GAP's value: 3 + 5. */
static void test_vfy03_gaps_pass(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const record_spec day26[] = {
        DATA(1), DATA(2), { GT_REC_GAP, GT_SENSOR_ALL, 3, 3 }, DATA(6),
        { GT_REC_GAP, GT_SENSOR_LIGHT, 7, 5 }, DATA(8),
    };
    put_segment(DAY_26, day26, 6, 0);
    gt_verify_report report = verify(ops);

    CHECK_EQ(report.ok, 1);
    CHECK(report.data_records == 4);
    CHECK(report.gap_records == 2);
    CHECK(report.gap_readings == 8);
    CHECK_EQ(report.last_seq, 8);
}

/* ---- VFY-04: an unexplained jump fails ----
 * 1, 2, 5: seqs 3-4 are missing with no GAP. The failure points at
 * record 5 (index 2). */
static void test_vfy04_uncovered_jump_fails(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const record_spec day26[] = { DATA(1), DATA(2), DATA(5) };
    put_segment(DAY_26, day26, 3, 0);
    gt_verify_report report = verify(ops);

    check_failed(&report, GT_VERIFY_SEQ_JUMP_UNCOVERED, DAY_26, RECORD_OFFSET(2));
    CHECK_EQ(report.last_seq, 2);
}

/* ---- VFY-05: seq going back across days fails ----
 * The 25th ends at 3 and the 26th starts at 3 again. */
static void test_vfy05_backwards_across_days(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const record_spec day25[] = { DATA(1), DATA(2), DATA(3) };
    const record_spec day26[] = { DATA(3), DATA(4) };
    put_segment(DAY_25, day25, 3, 0);
    put_segment(DAY_26, day26, 2, 0);
    gt_verify_report report = verify(ops);

    check_failed(&report, GT_VERIFY_SEQ_BACKWARDS, DAY_26, RECORD_OFFSET(0));
    CHECK_EQ(report.segments, 2);
}

/* ---- VFY-06: a damaged record fails where it is ----
 * One flipped bit in record 1 of the 26th's segment. */
static void test_vfy06_bad_record_located(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const record_spec day26[] = { DATA(1), DATA(2), DATA(3) };
    put_segment(DAY_26, day26, 3, 0);
    fs.files[0].data[RECORD_OFFSET(1) + 16] ^= 0x01;
    gt_verify_report report = verify(ops);

    check_failed(&report, GT_VERIFY_BAD_RECORD, DAY_26, RECORD_OFFSET(1));
}

/* ---- VFY-07: a partial tail fails ----
 * 10 stray bytes after two records: a torn write recovery did not cut. */
static void test_vfy07_partial_tail_fails(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const record_spec day26[] = { DATA(1), DATA(2) };
    put_segment(DAY_26, day26, 2, 10);
    gt_verify_report report = verify(ops);

    check_failed(&report, GT_VERIFY_PARTIAL_TAIL, DAY_26, RECORD_OFFSET(2));
}

/* ---- VFY-08: a bad or short header fails ----
 * A broken magic, then (in a second run) a 5-byte file. */
static void test_vfy08_bad_header_fails(void)
{
    const record_spec day26[] = { DATA(1) };

    const gt_file_ops *ops = fake_fs_ops(&fs);
    put_segment(DAY_26, day26, 1, 0);
    fs.files[0].data[0] = 'X';
    gt_verify_report report = verify(ops);
    check_failed(&report, GT_VERIFY_BAD_HEADER, DAY_26, 0);

    ops = fake_fs_ops(&fs);
    const uint8_t stub[5] = { 'G', 'T', 'L', 'G', 1 };
    CHECK_EQ(fake_fs_put(&fs, DAY_26, stub, sizeof stub), GT_OK);
    report = verify(ops);
    check_failed(&report, GT_VERIFY_BAD_HEADER, DAY_26, 0);
}

/* ---- VFY-09: .corrupt files are counted, not failed ----
 * Recovery set them aside on purpose, so the log still passes, but the
 * count says data was set aside. */
static void test_vfy09_corrupt_counted(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    const record_spec day26[] = { DATA(1) };
    const uint8_t junk[4] = { 1, 2, 3, 4 };
    CHECK_EQ(fake_fs_put(&fs, "log-20261020.bin.corrupt", junk, sizeof junk), GT_OK);
    put_segment(DAY_26, day26, 1, 0);
    gt_verify_report report = verify(ops);

    CHECK_EQ(report.ok, 1);
    CHECK_EQ(report.corrupt_files, 1);
    CHECK_EQ(report.segments, 1);
}

/* ---- VFY-10: read failures and bad args ----
 * A failed listing is GT_ERR_IO with the report untouched; NULL args are
 * GT_ERR_INVAL. */
static void test_vfy10_failures_and_args(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_verify_report report, untouched;
    memset(&report, 0x5A, sizeof report);
    memset(&untouched, 0x5A, sizeof untouched);
    fs.fail_list_call = 1;

    CHECK_EQ(gt_verify_dir_ops(ops, &fs, &report), GT_ERR_IO);
    CHECK(memcmp(&report, &untouched, sizeof report) == 0);
    CHECK_EQ(gt_verify_dir_ops(NULL, &fs, &report), GT_ERR_INVAL);
    CHECK_EQ(gt_verify_dir_ops(ops, &fs, NULL), GT_ERR_INVAL);
    CHECK_EQ(fs.destroy_calls, 0);   /* the caller keeps ctx */
}

static const test_case_t tests[] = {
    {"VFY-01", "an empty directory passes", test_vfy01_empty_dir_passes},
    {"VFY-02", "a clean log over two days passes", test_vfy02_clean_two_days},
    {"VFY-03", "GAPs that explain the numbering pass", test_vfy03_gaps_pass},
    {"VFY-04", "an unexplained jump fails", test_vfy04_uncovered_jump_fails},
    {"VFY-05", "seq going back across days fails", test_vfy05_backwards_across_days},
    {"VFY-06", "a damaged record fails where it is", test_vfy06_bad_record_located},
    {"VFY-07", "a partial tail fails", test_vfy07_partial_tail_fails},
    {"VFY-08", "a bad or short header fails", test_vfy08_bad_header_fails},
    {"VFY-09", ".corrupt files are counted, not failed", test_vfy09_corrupt_counted},
    {"VFY-10", "read failures and bad args", test_vfy10_failures_and_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
