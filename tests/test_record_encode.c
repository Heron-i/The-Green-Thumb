/* Unit tests for encoding the 24 B record (src/log_record.c):
 * gt_record_encode.
 *
 * Purpose: prove every field lands at its documented offset (log_record.h,
 * DESIGN.md "Record") in little-endian byte order with a correct CRC, and
 * that a record storage would reject is never written: bad type, bad
 * sensor_id and NULL arguments fail without touching the output. The same
 * bytes go on disk and over storage.sock (hub -> storage). Pure byte
 * packing: no files or sockets. Run under make test / asan / memcheck.
 * Implements RCE-01 through RCE-08. Decoding is in test_record_decode.c.
 *
 * What these tests cannot prove: that the bytes reach the disk. That
 * arrives with the storage flush thread. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "log_record.h"

/* Read the little-endian u32 at wire[offset], lowest byte first. */
static uint32_t read_le32(const uint8_t *wire, size_t offset)
{
    return (uint32_t)wire[offset]
         | ((uint32_t)wire[offset + 1] << 8)
         | ((uint32_t)wire[offset + 2] << 16)
         | ((uint32_t)wire[offset + 3] << 24);
}

/* ---- RCE-01: the record is 24 bytes ----
 * Pinned because recovery's offset arithmetic and DESIGN.md's sizes
 * (2.1 MB/day, 24 KB hub queue) all assume 24 B. */
static void test_rce01_size_is_24(void)
{
    CHECK_EQ(GT_REC_SIZE, 24);
}

/* ---- RCE-02: header bytes 0-3 ----
 * magic, type, version, sensor_id. Type and version are the other way
 * round from the 16 B message, so a copied message encoder fails here. */
static void test_rce02_header_bytes(void)
{
    gt_record record = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
                         .seq = 1, .timestamp_us = 1700000000000000u, .value = 300 };
    uint8_t wire[GT_REC_SIZE];

    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
    CHECK_EQ(wire[0], GT_REC_MAGIC);
    CHECK_EQ(wire[1], GT_REC_DATA);
    CHECK_EQ(wire[2], GT_REC_VERSION);
    CHECK_EQ(wire[3], GT_SENSOR_LIGHT);
}

/* ---- RCE-03: seq, timestamp, value are little-endian at 4, 8, 16 ----
 * Every byte from 4 to 19 is different (01..10), so a swapped byte order,
 * a field at the wrong offset, or a timestamp cut to 32 bits all fail. */
static void test_rce03_fields_little_endian(void)
{
    gt_record record = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
                         .seq = 0x04030201u,
                         .timestamp_us = 0x0C0B0A0908070605u,
                         .value = 0x100F0E0Du };
    const uint8_t expected_body[16] = {
        0x01, 0x02, 0x03, 0x04,                            /* seq */
        0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,    /* timestamp */
        0x0D, 0x0E, 0x0F, 0x10,                            /* value */
    };
    uint8_t wire[GT_REC_SIZE];

    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
    CHECK(memcmp(&wire[4], expected_body, sizeof expected_body) == 0);
}

/* ---- RCE-04: bytes 20-23 are the CRC of bytes 0-19 ----
 * Stored little-endian, like every other field. The exact CRC value is
 * pinned independently in RCD-02. */
static void test_rce04_crc_covers_bytes_0_to_19(void)
{
    gt_record record = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_SOIL,
                         .seq = 77, .timestamp_us = 1700000000000000u, .value = 850 };
    uint8_t wire[GT_REC_SIZE];

    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
    CHECK_EQ(read_le32(wire, 20), gt_crc32(wire, 20));
}

/* ---- RCE-05: a GAP record for all sensors encodes ----
 * Storage writes one gap for every sensor when its own queue is full, so
 * sensor_id 0 (GT_SENSOR_ALL) is valid in a GAP record, unlike a message. */
static void test_rce05_gap_for_all_sensors(void)
{
    gt_record gap = { .type = GT_REC_GAP, .sensor_id = GT_SENSOR_ALL,
                      .seq = 500, .timestamp_us = 1700000000000000u, .value = 12 };
    uint8_t wire[GT_REC_SIZE];

    CHECK_EQ(gt_record_encode(&gap, wire), GT_OK);
    CHECK_EQ(wire[1], GT_REC_GAP);
    CHECK_EQ(wire[3], GT_SENSOR_ALL);
    CHECK_EQ(read_le32(wire, 16), 12);
}

/* ---- RCE-06: an unknown type is rejected ----
 * Types 0 and 3 are neither DATA nor GAP: rejected, and the output buffer
 * is left exactly as it was (all 0x5A), so no half-written record exists. */
static void test_rce06_unknown_type_rejected(void)
{
    const uint8_t unknown_types[] = { 0x00, 0x03 };
    uint8_t untouched[GT_REC_SIZE];
    memset(untouched, 0x5A, sizeof untouched);

    for (size_t type_index = 0; type_index < sizeof unknown_types; type_index++) {
        gt_record record = { .type = unknown_types[type_index],
                             .sensor_id = GT_SENSOR_LIGHT,
                             .seq = 1, .timestamp_us = 1, .value = 300 };
        uint8_t wire[GT_REC_SIZE];
        memset(wire, 0x5A, sizeof wire);

        CHECK_EQ(gt_record_encode(&record, wire), GT_ERR_INVAL);
        CHECK(memcmp(wire, untouched, sizeof wire) == 0);
    }
}

/* ---- RCE-07: a bad sensor_id is rejected ----
 * A DATA reading comes from one sensor, so 0 (ALL) is invalid for DATA;
 * 3 is no sensor for either type. Output untouched, as in RCE-06. */
static void test_rce07_bad_sensor_id_rejected(void)
{
    const gt_record bad_records[] = {
        { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_ALL,
          .seq = 1, .timestamp_us = 1, .value = 300 },
        { .type = GT_REC_DATA, .sensor_id = 3,
          .seq = 1, .timestamp_us = 1, .value = 300 },
        { .type = GT_REC_GAP, .sensor_id = 3,
          .seq = 1, .timestamp_us = 1, .value = 5 },
    };
    size_t bad_count = sizeof bad_records / sizeof bad_records[0];
    uint8_t untouched[GT_REC_SIZE];
    memset(untouched, 0x5A, sizeof untouched);

    for (size_t record_index = 0; record_index < bad_count; record_index++) {
        uint8_t wire[GT_REC_SIZE];
        memset(wire, 0x5A, sizeof wire);

        CHECK_EQ(gt_record_encode(&bad_records[record_index], wire), GT_ERR_INVAL);
        CHECK(memcmp(wire, untouched, sizeof wire) == 0);
    }
}

/* ---- RCE-08: NULL arguments are rejected ---- */
static void test_rce08_null_args(void)
{
    gt_record record = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
                         .seq = 1, .timestamp_us = 1, .value = 300 };
    uint8_t wire[GT_REC_SIZE];

    CHECK_EQ(gt_record_encode(NULL, wire), GT_ERR_INVAL);
    CHECK_EQ(gt_record_encode(&record, NULL), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"RCE-01", "the record is 24 bytes", test_rce01_size_is_24},
    {"RCE-02", "header bytes 0-3", test_rce02_header_bytes},
    {"RCE-03", "fields are little-endian at 4, 8, 16",
     test_rce03_fields_little_endian},
    {"RCE-04", "bytes 20-23 are the CRC of bytes 0-19",
     test_rce04_crc_covers_bytes_0_to_19},
    {"RCE-05", "a GAP record for all sensors encodes",
     test_rce05_gap_for_all_sensors},
    {"RCE-06", "an unknown type is rejected", test_rce06_unknown_type_rejected},
    {"RCE-07", "a bad sensor_id is rejected", test_rce07_bad_sensor_id_rejected},
    {"RCE-08", "NULL arguments are rejected", test_rce08_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
