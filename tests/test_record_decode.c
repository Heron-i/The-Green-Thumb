/* Unit tests for decoding the 24 B record (src/log_record.c):
 * gt_record_decode.
 *
 * Purpose: prove decode reads every field from its documented offset
 * (log_record.h, DESIGN.md "Record"), undoes encode exactly, and rejects
 * any record storage must refuse, whether it arrives over storage.sock or
 * is found during the startup recovery scan (DESIGN.md "bad magic, unknown
 * record_type or wrong version", "CRC mismatch"), without touching the
 * output. Pure byte unpacking: no files or sockets. Run under make test /
 * asan / memcheck. Implements RCD-01 through RCD-08. Encoding is in
 * test_record_encode.c.
 *
 * What these tests cannot prove: the recovery rules that span records
 * (seq must increase, a partial trailing record). Those arrive with the
 * storage recovery scan. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "log_record.h"

/* ---- RCD-01: decode undoes encode ----
 * A DATA and a GAP record, at the all-zero and all-max ends of every
 * field, come back field for field. */
static void test_rcd01_round_trip(void)
{
    const gt_record originals[] = {
        { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
          .seq = 0, .timestamp_us = 0, .value = 0 },
        { .type = GT_REC_GAP, .sensor_id = GT_SENSOR_ALL,
          .seq = UINT32_MAX, .timestamp_us = UINT64_MAX, .value = UINT32_MAX },
    };
    size_t original_count = sizeof originals / sizeof originals[0];

    for (size_t record_index = 0; record_index < original_count; record_index++) {
        const gt_record *original = &originals[record_index];
        uint8_t wire[GT_REC_SIZE];
        gt_record decoded;

        CHECK_EQ(gt_record_encode(original, wire), GT_OK);
        CHECK_EQ(gt_record_decode(wire, &decoded), GT_OK);
        CHECK_EQ(decoded.type, original->type);
        CHECK_EQ(decoded.sensor_id, original->sensor_id);
        CHECK_EQ(decoded.seq, original->seq);
        CHECK(decoded.timestamp_us == original->timestamp_us);   /* u64: too big for CHECK_EQ's long long */
        CHECK_EQ(decoded.value, original->value);
    }
}

/* A valid light DATA record written out by hand from the log_record.h
 * layout: seq 0x04030201, timestamp 0x0C0B0A0908070605, value 0x100F0E0D.
 * The CRC 0xC57D1B67 was computed outside this codebase, with Python's
 * zlib.crc32 over bytes 0-19. RCD-03..07 copy it and break one byte. */
static const uint8_t VALID_WIRE[GT_REC_SIZE] = {
    GT_REC_MAGIC, GT_REC_DATA, GT_REC_VERSION, GT_SENSOR_LIGHT,
    0x01, 0x02, 0x03, 0x04,                            /* seq, lowest byte first */
    0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,    /* timestamp */
    0x0D, 0x0E, 0x0F, 0x10,                            /* value */
    0x67, 0x1B, 0x7D, 0xC5,                            /* crc32 0xC57D1B67 */
};

/* Rewrite bytes 20-23 with the CRC of bytes 0-19, little-endian. Used after
 * breaking a header byte so the record's CRC is still valid: otherwise the
 * CRC check alone would reject it, and the magic/version/type/sensor checks
 * would never be exercised. */
static void reseal_crc(uint8_t wire[GT_REC_SIZE])
{
    uint32_t crc = gt_crc32(wire, 20);
    wire[20] = (uint8_t)(crc);
    wire[21] = (uint8_t)(crc >> 8);
    wire[22] = (uint8_t)(crc >> 16);
    wire[23] = (uint8_t)(crc >> 24);
}

/* Decode `wire` and check it is rejected as corrupt with the output left
 * exactly as it was (pre-filled with 0x5A). */
static void check_rejected_untouched(const uint8_t wire[GT_REC_SIZE])
{
    gt_record decoded, untouched;
    memset(&decoded, 0x5A, sizeof decoded);
    memset(&untouched, 0x5A, sizeof untouched);

    CHECK_EQ(gt_record_decode(wire, &decoded), GT_ERR_CORRUPT);
    CHECK(memcmp(&decoded, &untouched, sizeof decoded) == 0);
}

/* ---- RCD-02: hand-built bytes decode ----
 * Checks decode, and the CRC, against the documented layout directly, so
 * a matching bug in encode and decode (which RCD-01 alone would miss)
 * still fails. */
static void test_rcd02_hand_built_bytes(void)
{
    gt_record decoded;

    CHECK_EQ(gt_record_decode(VALID_WIRE, &decoded), GT_OK);
    CHECK_EQ(decoded.type, GT_REC_DATA);
    CHECK_EQ(decoded.sensor_id, GT_SENSOR_LIGHT);
    CHECK_EQ(decoded.seq, 0x04030201u);
    CHECK(decoded.timestamp_us == 0x0C0B0A0908070605u);
    CHECK_EQ(decoded.value, 0x100F0E0Du);
}

/* ---- RCD-03: a bad magic byte is rejected ----
 * 0xA6 is the 16 B message's magic: a message must never be stored as a
 * record. The CRC is resealed, so only the magic check can catch it. */
static void test_rcd03_bad_magic_rejected(void)
{
    uint8_t wire[GT_REC_SIZE];
    memcpy(wire, VALID_WIRE, sizeof wire);
    wire[0] = 0xA6;
    reseal_crc(wire);

    check_rejected_untouched(wire);
}

/* ---- RCD-04: a bad version byte is rejected ----
 * A record from a newer layout must not be misread as version 1. */
static void test_rcd04_bad_version_rejected(void)
{
    uint8_t wire[GT_REC_SIZE];
    memcpy(wire, VALID_WIRE, sizeof wire);
    wire[2] = GT_REC_VERSION + 1;
    reseal_crc(wire);

    check_rejected_untouched(wire);
}

/* ---- RCD-05: an unknown type is rejected ----
 * Types 0 and 3 are neither DATA nor GAP. DESIGN.md: "Any record with an
 * unknown record_type is treated as corrupt." */
static void test_rcd05_unknown_type_rejected(void)
{
    const uint8_t unknown_types[] = { 0x00, 0x03 };

    for (size_t type_index = 0; type_index < sizeof unknown_types; type_index++) {
        uint8_t wire[GT_REC_SIZE];
        memcpy(wire, VALID_WIRE, sizeof wire);
        wire[1] = unknown_types[type_index];
        reseal_crc(wire);

        check_rejected_untouched(wire);
    }
}

/* ---- RCD-06: a bad sensor_id is rejected ----
 * Mirrors RCE-07: 0 (ALL) is invalid for DATA, and 3 is no sensor for
 * either type. Each case is one {type, sensor_id} pair. `uint8_t
 * bad_pairs[][2]` is a 2-D array: a list of rows, each row exactly 2
 * bytes; the compiler counts the rows from the initializer. */
static void test_rcd06_bad_sensor_id_rejected(void)
{
    const uint8_t bad_pairs[][2] = {
        { GT_REC_DATA, GT_SENSOR_ALL },
        { GT_REC_DATA, 3 },
        { GT_REC_GAP,  3 },
    };
    size_t pair_count = sizeof bad_pairs / sizeof bad_pairs[0];

    for (size_t pair_index = 0; pair_index < pair_count; pair_index++) {
        uint8_t wire[GT_REC_SIZE];
        memcpy(wire, VALID_WIRE, sizeof wire);
        wire[1] = bad_pairs[pair_index][0];
        wire[3] = bad_pairs[pair_index][1];
        reseal_crc(wire);

        check_rejected_untouched(wire);
    }
}

/* ---- RCD-07: a CRC mismatch is rejected ----
 * One payload byte (the value's low byte) is damaged and the CRC is NOT
 * resealed: what a torn write or a bad SD sector leaves behind. */
static void test_rcd07_crc_mismatch_rejected(void)
{
    uint8_t wire[GT_REC_SIZE];
    memcpy(wire, VALID_WIRE, sizeof wire);
    wire[16] ^= 0x01;

    check_rejected_untouched(wire);
}

/* ---- RCD-08: NULL arguments are rejected ----
 * GT_ERR_INVAL (a caller bug), not GT_ERR_CORRUPT (bad bytes). */
static void test_rcd08_null_args(void)
{
    gt_record decoded;

    CHECK_EQ(gt_record_decode(NULL, &decoded), GT_ERR_INVAL);
    CHECK_EQ(gt_record_decode(VALID_WIRE, NULL), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"RCD-01", "decode undoes encode", test_rcd01_round_trip},
    {"RCD-02", "hand-built bytes decode", test_rcd02_hand_built_bytes},
    {"RCD-03", "a bad magic byte is rejected", test_rcd03_bad_magic_rejected},
    {"RCD-04", "a bad version byte is rejected", test_rcd04_bad_version_rejected},
    {"RCD-05", "an unknown type is rejected", test_rcd05_unknown_type_rejected},
    {"RCD-06", "a bad sensor_id is rejected", test_rcd06_bad_sensor_id_rejected},
    {"RCD-07", "a CRC mismatch is rejected", test_rcd07_crc_mismatch_rejected},
    {"RCD-08", "NULL arguments are rejected", test_rcd08_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
