/* Unit tests for the startup recovery scan (src/storage_recover.c):
 * gt_recover_scan.
 *
 * Purpose: prove the scan keeps exactly the valid prefix of a segment and
 * stops at the first record DESIGN.md says recovery must cut ("bad magic,
 * unknown record_type or wrong version", "CRC mismatch", "seq that does
 * not increase", "a partial (fewer than 24 B) trailing record"), and that
 * a bad header is reported instead of scanned. Every segment here is built
 * in memory with gt_seg_header_encode and gt_record_encode: no files. The
 * last kept record's seq and time are reported, for the seq_resume reply
 * and for stamping a GAP found after a restart (PF-07). Run under make
 * test / asan / memcheck. Implements RSC-01 through RSC-10.
 *
 * What these tests cannot prove: that the file is truncated and fsynced,
 * that a bad segment is renamed to .corrupt, or that seq increases across
 * two segment files. Those arrive with gt_storage_recover_ops (Mechanism D
 * Step 6). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "log_record.h"
#include "storage_recover.h"

/* Room for a header, up to 8 records and a partial tail. */
#define SEGMENT_MAX_RECORDS 8
#define SEGMENT_BUF_SIZE    (GT_SEG_HEADER_SZ + (SEGMENT_MAX_RECORDS + 1) * GT_REC_SIZE)

/* Byte offset of record number `record_index` in a segment. */
#define RECORD_OFFSET(record_index) (GT_SEG_HEADER_SZ + (record_index) * GT_REC_SIZE)

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

/* Scan `segment` and check every field of the result against a valid
 * header with `kept` records ending at `last_seq` and `removed` cut. */
static void check_scan(const uint8_t *segment, size_t len, size_t kept,
                       size_t removed, uint32_t last_seq)
{
    gt_scan_result result;
    memset(&result, 0x5A, sizeof result);

    CHECK_EQ(gt_recover_scan(segment, len, &result), GT_OK);
    CHECK_EQ(result.header_ok, 1);
    CHECK_EQ(result.good_len, RECORD_OFFSET(kept));
    CHECK_EQ(result.records_kept, kept);
    CHECK_EQ(result.records_removed, removed);
    CHECK_EQ(result.has_records, kept > 0);
    CHECK_EQ(result.last_seq, last_seq);
}

/* Scan `segment` and check it is reported as a bad header: header_ok 0
 * and every other field 0. */
static void check_bad_header(const uint8_t *segment, size_t len)
{
    gt_scan_result result;
    memset(&result, 0x5A, sizeof result);

    CHECK_EQ(gt_recover_scan(segment, len, &result), GT_OK);
    CHECK_EQ(result.header_ok, 0);
    CHECK_EQ(result.good_len, 0);
    CHECK_EQ(result.records_kept, 0);
    CHECK_EQ(result.records_removed, 0);
    CHECK_EQ(result.has_records, 0);
    CHECK_EQ(result.last_seq, 0);
}

/* ---- RSC-01: a header-only segment is clean ----
 * What a new day's file looks like right after it is created. */
static void test_rsc01_header_only(void)
{
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, NULL, 0);

    check_scan(segment, len, 0, 0, 0);
}

/* ---- RSC-02: a clean segment is kept whole ----
 * seq may skip values (5 -> 9): a GAP record explains those, so a skip
 * is not damage. */
static void test_rsc02_clean_segment_kept(void)
{
    const uint32_t seqs[] = { 4, 5, 9 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, seqs, 3);

    check_scan(segment, len, 3, 0, 9);
}

/* ---- RSC-03: a partial tail is cut ----
 * A power cut during write() leaves 1-23 bytes of the next record. Each
 * length is tried, and the leftover bytes are non-zero so a scan that
 * only checks for zeros fails. */
static void test_rsc03_partial_tail_cut(void)
{
    const uint32_t seqs[] = { 1, 2 };
    const size_t tail_lengths[] = { 1, 12, GT_REC_SIZE - 1 };
    size_t tail_count = sizeof tail_lengths / sizeof tail_lengths[0];

    for (size_t tail_index = 0; tail_index < tail_count; tail_index++) {
        uint8_t segment[SEGMENT_BUF_SIZE];
        size_t len = build_segment(segment, seqs, 2);
        memset(&segment[len], 0xA7, tail_lengths[tail_index]);

        check_scan(segment, len + tail_lengths[tail_index], 2, 1, 2);
    }
}

/* ---- RSC-04: a CRC mismatch cuts from there ----
 * Record 1 of 4 has one damaged byte. Records 2 and 3 are still valid,
 * but the scan stops at the first bad record, so all three are removed. */
static void test_rsc04_crc_mismatch_cuts(void)
{
    const uint32_t seqs[] = { 1, 2, 3, 4 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, seqs, 4);
    segment[RECORD_OFFSET(1) + 16] ^= 0x01;   /* low byte of the value */

    check_scan(segment, len, 1, 3, 1);
}

/* ---- RSC-05: a bad record after good ones is cut ----
 * Record 2 has an unknown type with its CRC resealed, so only the decode
 * field checks can catch it. A trailing partial record is counted too:
 * 1 bad + 1 whole + 1 partial = 3 removed. */
static void test_rsc05_bad_type_cuts(void)
{
    const uint32_t seqs[] = { 1, 2, 3, 4 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, seqs, 4);
    uint8_t *bad_record = &segment[RECORD_OFFSET(2)];
    bad_record[1] = 0x03;
    uint32_t crc = gt_crc32(bad_record, 20);
    bad_record[20] = (uint8_t)(crc);
    bad_record[21] = (uint8_t)(crc >> 8);
    bad_record[22] = (uint8_t)(crc >> 16);
    bad_record[23] = (uint8_t)(crc >> 24);

    check_scan(segment, len + 10, 2, 3, 2);
}

/* ---- RSC-06: a seq that does not increase is cut ----
 * A repeated seq (a duplicate) and a backwards seq (an old record) are
 * both damage, even though each record's own bytes are valid. */
static void test_rsc06_seq_not_increasing_cuts(void)
{
    const uint32_t repeated[] = { 7, 8, 8, 9 };
    const uint32_t backwards[] = { 7, 8, 3, 9 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len;

    len = build_segment(segment, repeated, 4);
    check_scan(segment, len, 2, 2, 8);

    len = build_segment(segment, backwards, 4);
    check_scan(segment, len, 2, 2, 8);
}

/* ---- RSC-07: seq 0 counts as a record ----
 * last_seq is 0 both with no records and with one record of seq 0, so
 * has_records is what tells them apart. */
static void test_rsc07_seq_zero_is_a_record(void)
{
    const uint32_t seqs[] = { 0 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, seqs, 1);

    check_scan(segment, len, 1, 0, 0);
}

/* ---- RSC-08: a bad or short header is reported ----
 * A broken magic, an empty file, and a file cut off inside the header
 * (a crash right after the file was created). */
static void test_rsc08_bad_header_reported(void)
{
    const uint32_t seqs[] = { 1 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, seqs, 1);

    segment[0] = 'X';
    check_bad_header(segment, len);

    len = build_segment(segment, seqs, 1);
    check_bad_header(segment, 0);
    check_bad_header(segment, GT_SEG_HEADER_SZ - 1);
}

/* ---- RSC-09: NULL arguments are rejected ----
 * GT_ERR_INVAL, and the result is left exactly as it was. */
static void test_rsc09_null_args(void)
{
    uint8_t segment[SEGMENT_BUF_SIZE];
    size_t len = build_segment(segment, NULL, 0);
    gt_scan_result result, untouched;
    memset(&result, 0x5A, sizeof result);
    memset(&untouched, 0x5A, sizeof untouched);

    CHECK_EQ(gt_recover_scan(NULL, len, &result), GT_ERR_INVAL);
    CHECK(memcmp(&result, &untouched, sizeof result) == 0);
    CHECK_EQ(gt_recover_scan(segment, len, NULL), GT_ERR_INVAL);
}

/* ---- RSC-10: the last kept record's time is reported ----
 * Records 1-3 are stamped 2000-2002. A clean segment reports 2002. With
 * record 3 damaged, the scan keeps 1-2 and reports 2001: the time of the
 * last record kept, not of the one cut. A header-only segment reports 0.
 * Storage stamps a GAP found after a restart with this time (PF-07). */
static void test_rsc10_last_kept_timestamp(void)
{
    const uint32_t seqs[] = { 1, 2, 3 };
    uint8_t segment[SEGMENT_BUF_SIZE];
    gt_scan_result result;
    size_t len = build_segment(segment, seqs, 3);

    CHECK_EQ(gt_recover_scan(segment, len, &result), GT_OK);
    CHECK(result.last_timestamp_us == 2002);   /* u64: too big for CHECK_EQ's long long */

    segment[RECORD_OFFSET(2) + 16] ^= 0x01;   /* low byte of record 3's value */
    CHECK_EQ(gt_recover_scan(segment, len, &result), GT_OK);
    CHECK_EQ(result.records_kept, 2);
    CHECK(result.last_timestamp_us == 2001);

    len = build_segment(segment, NULL, 0);
    CHECK_EQ(gt_recover_scan(segment, len, &result), GT_OK);
    CHECK(result.last_timestamp_us == 0);
}

static const test_case_t tests[] = {
    {"RSC-01", "a header-only segment is clean", test_rsc01_header_only},
    {"RSC-02", "a clean segment is kept whole", test_rsc02_clean_segment_kept},
    {"RSC-03", "a partial tail is cut", test_rsc03_partial_tail_cut},
    {"RSC-04", "a CRC mismatch cuts from there", test_rsc04_crc_mismatch_cuts},
    {"RSC-05", "a bad record after good ones is cut", test_rsc05_bad_type_cuts},
    {"RSC-06", "a seq that does not increase is cut", test_rsc06_seq_not_increasing_cuts},
    {"RSC-07", "seq 0 counts as a record", test_rsc07_seq_zero_is_a_record},
    {"RSC-08", "a bad or short header is reported", test_rsc08_bad_header_reported},
    {"RSC-09", "NULL arguments are rejected", test_rsc09_null_args},
    {"RSC-10", "the last kept record's time is reported", test_rsc10_last_kept_timestamp},
};

int main(void)
{
    return RUN_TESTS(tests);
}
