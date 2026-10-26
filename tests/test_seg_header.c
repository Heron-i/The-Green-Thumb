/* Unit tests for the 16 B segment header (src/log_record.c):
 * gt_seg_header_encode and gt_seg_header_decode.
 *
 * Purpose: prove every header field lands at its documented offset
 * (log_record.h, DESIGN.md "Segment header") in little-endian byte order,
 * that decode undoes encode, and that a header the recovery scan must
 * refuse (DESIGN.md "Check the current segment's header. If it is invalid,
 * rename the file to .corrupt") is rejected without touching the output.
 * Pure byte packing: no files. Run under make test / asan / memcheck.
 * Implements SGH-01 through SGH-08.
 *
 * What these tests cannot prove: that a bad header leads to the .corrupt
 * rename. That arrives with gt_storage_recover_ops (Mechanism D Step 6). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "log_record.h"

/* A valid header written out by hand from the log_record.h layout, with
 * created_at 0x0807060504030201. SGH-05..07 copy it and break one field. */
static const uint8_t VALID_HEADER[GT_SEG_HEADER_SZ] = {
    'G', 'T', 'L', 'G',                                /* magic */
    0x01, 0x00,                                        /* version 1 */
    0x18, 0x00,                                        /* record_size 24 */
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,    /* created_at, lowest byte first */
};

/* Decode `bytes` and check it is rejected as corrupt with the output left
 * exactly as it was (pre-filled with 0x5A). */
static void check_rejected_untouched(const uint8_t bytes[GT_SEG_HEADER_SZ])
{
    gt_seg_header decoded, untouched;
    memset(&decoded, 0x5A, sizeof decoded);
    memset(&untouched, 0x5A, sizeof untouched);

    CHECK_EQ(gt_seg_header_decode(bytes, &decoded), GT_ERR_CORRUPT);
    CHECK(memcmp(&decoded, &untouched, sizeof decoded) == 0);
}

/* ---- SGH-01: the header is 16 bytes ----
 * Pinned because the recovery scan starts reading records at offset 16. */
static void test_sgh01_size_is_16(void)
{
    CHECK_EQ(GT_SEG_HEADER_SZ, 16);
}

/* ---- SGH-02: encode matches the hand-built bytes ----
 * Checked against the documented layout directly, so a matching bug in
 * encode and decode (which SGH-04 alone would miss) still fails. */
static void test_sgh02_encode_layout(void)
{
    const gt_seg_header header = { .created_at_us = 0x0807060504030201u };
    uint8_t out[GT_SEG_HEADER_SZ];

    CHECK_EQ(gt_seg_header_encode(&header, out), GT_OK);
    CHECK(memcmp(out, VALID_HEADER, sizeof out) == 0);
}

/* ---- SGH-03: hand-built bytes decode ---- */
static void test_sgh03_decode_hand_built(void)
{
    gt_seg_header decoded;

    CHECK_EQ(gt_seg_header_decode(VALID_HEADER, &decoded), GT_OK);
    CHECK(decoded.created_at_us == 0x0807060504030201u);   /* u64: too big for CHECK_EQ's long long */
}

/* ---- SGH-04: decode undoes encode ----
 * The all-zero and all-max ends of created_at come back unchanged. */
static void test_sgh04_round_trip(void)
{
    const uint64_t created_values[] = { 0, UINT64_MAX };
    size_t value_count = sizeof created_values / sizeof created_values[0];

    for (size_t value_index = 0; value_index < value_count; value_index++) {
        const gt_seg_header original = { .created_at_us = created_values[value_index] };
        uint8_t bytes[GT_SEG_HEADER_SZ];
        gt_seg_header decoded;

        CHECK_EQ(gt_seg_header_encode(&original, bytes), GT_OK);
        CHECK_EQ(gt_seg_header_decode(bytes, &decoded), GT_OK);
        CHECK(decoded.created_at_us == original.created_at_us);
    }
}

/* ---- SGH-05: a bad magic is rejected ----
 * Each of the 4 magic bytes is broken in turn, so a check that only looks
 * at the first byte fails. */
static void test_sgh05_bad_magic_rejected(void)
{
    /* invariant: every magic byte before magic_index has been tested */
    for (size_t magic_index = 0; magic_index < 4; magic_index++) {
        uint8_t bytes[GT_SEG_HEADER_SZ];
        memcpy(bytes, VALID_HEADER, sizeof bytes);
        bytes[magic_index] ^= 0x20;   /* e.g. 'G' becomes 'g' */

        check_rejected_untouched(bytes);
    }
}

/* ---- SGH-06: a bad version is rejected ----
 * Version 2 in the low byte, and version 0x0101 via the high byte, so a
 * check that reads only one byte of the u16 fails. */
static void test_sgh06_bad_version_rejected(void)
{
    uint8_t bytes[GT_SEG_HEADER_SZ];

    memcpy(bytes, VALID_HEADER, sizeof bytes);
    bytes[4] = GT_SEG_VERSION + 1;
    check_rejected_untouched(bytes);

    memcpy(bytes, VALID_HEADER, sizeof bytes);
    bytes[5] = 0x01;
    check_rejected_untouched(bytes);
}

/* ---- SGH-07: a bad record_size is rejected ----
 * 16 (the message size, a plausible mix-up) and 24 + 256 via the high
 * byte. A segment of a different record size can't be scanned as 24 B. */
static void test_sgh07_bad_record_size_rejected(void)
{
    uint8_t bytes[GT_SEG_HEADER_SZ];

    memcpy(bytes, VALID_HEADER, sizeof bytes);
    bytes[6] = 16;
    check_rejected_untouched(bytes);

    memcpy(bytes, VALID_HEADER, sizeof bytes);
    bytes[7] = 0x01;
    check_rejected_untouched(bytes);
}

/* ---- SGH-08: NULL arguments are rejected ----
 * GT_ERR_INVAL (a caller bug), not GT_ERR_CORRUPT (bad bytes). */
static void test_sgh08_null_args(void)
{
    const gt_seg_header header = { .created_at_us = 1 };
    uint8_t out[GT_SEG_HEADER_SZ];
    gt_seg_header decoded;

    CHECK_EQ(gt_seg_header_encode(NULL, out), GT_ERR_INVAL);
    CHECK_EQ(gt_seg_header_encode(&header, NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_seg_header_decode(NULL, &decoded), GT_ERR_INVAL);
    CHECK_EQ(gt_seg_header_decode(VALID_HEADER, NULL), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"SGH-01", "the header is 16 bytes", test_sgh01_size_is_16},
    {"SGH-02", "encode matches the hand-built bytes", test_sgh02_encode_layout},
    {"SGH-03", "hand-built bytes decode", test_sgh03_decode_hand_built},
    {"SGH-04", "decode undoes encode", test_sgh04_round_trip},
    {"SGH-05", "a bad magic is rejected", test_sgh05_bad_magic_rejected},
    {"SGH-06", "a bad version is rejected", test_sgh06_bad_version_rejected},
    {"SGH-07", "a bad record_size is rejected", test_sgh07_bad_record_size_rejected},
    {"SGH-08", "NULL arguments are rejected", test_sgh08_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
