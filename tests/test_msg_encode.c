/* Unit tests for encoding the 16 B sensor -> hub message (src/ipc_msg.c):
 * gt_msg_encode.
 *
 * Purpose: prove every field lands at its documented offset (ipc.h) in
 * little-endian byte order, and that a message the hub would reject is
 * never put on the wire: bad type, bad sensor_id and NULL arguments fail
 * without touching the output. Pure byte packing: no sockets. Run under
 * make test / asan / memcheck. Implements MSE-01 through MSE-07. Decoding
 * is in test_msg_decode.c.
 *
 * What these tests cannot prove: that the bytes cross the socket intact.
 * That arrives with the IPC client. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "ipc.h"

/* ---- MSE-01: the message is 16 bytes ----
 * Pinned because DESIGN.md's buffer sizes and the IPC latency target are
 * all stated for a 16 B message. */
static void test_mse01_size_is_16(void)
{
    CHECK_EQ(GT_MSG_SIZE, 16);
}

/* ---- MSE-02: header bytes 0-3 ----
 * magic, version, type, sensor_id, one byte each, in that order. */
static void test_mse02_header_bytes(void)
{
    gt_msg message = { .type = GT_MSG_DATA, .sensor_id = GT_SENSOR_LIGHT,
                       .counter = 1, .seq = 0, .value = 300 };
    uint8_t wire[GT_MSG_SIZE];

    CHECK_EQ(gt_msg_encode(&message, wire), GT_OK);
    CHECK_EQ(wire[0], GT_MSG_MAGIC);
    CHECK_EQ(wire[1], GT_MSG_VERSION);
    CHECK_EQ(wire[2], GT_MSG_DATA);
    CHECK_EQ(wire[3], GT_SENSOR_LIGHT);
}

/* ---- MSE-03: u32 fields are little-endian at 4, 8, 12 ----
 * Each value's bytes are all different, so a swapped byte order or a field
 * at the wrong offset changes the result. Little-endian = lowest byte
 * first: 0x04030201 goes out as 01 02 03 04. */
static void test_mse03_u32_fields_little_endian(void)
{
    gt_msg message = { .type = GT_MSG_DATA, .sensor_id = GT_SENSOR_LIGHT,
                       .counter = 0x04030201u, .seq = 0x08070605u, .value = 0x0C0B0A09u };
    const uint8_t expected_body[12] = { 0x01, 0x02, 0x03, 0x04,    /* counter */
                                        0x05, 0x06, 0x07, 0x08,    /* seq */
                                        0x09, 0x0A, 0x0B, 0x0C };  /* value */
    uint8_t wire[GT_MSG_SIZE];

    CHECK_EQ(gt_msg_encode(&message, wire), GT_OK);
    CHECK(memcmp(&wire[4], expected_body, sizeof expected_body) == 0);
}

/* ---- MSE-04: a GAP message encodes ----
 * A sensor whose ring overflowed reports the gap on reconnect: counter =
 * first refused reading, value = how many were refused. */
static void test_mse04_gap_encodes(void)
{
    gt_msg gap = { .type = GT_MSG_GAP, .sensor_id = GT_SENSOR_SOIL,
                   .counter = 500, .seq = 0, .value = 12 };
    uint8_t wire[GT_MSG_SIZE];

    CHECK_EQ(gt_msg_encode(&gap, wire), GT_OK);
    CHECK_EQ(wire[2], GT_MSG_GAP);
    CHECK_EQ(wire[3], GT_SENSOR_SOIL);
    CHECK_EQ(wire[12], 12);   /* value's low byte; 12 fits in one byte */
}

/* ---- MSE-05: an unknown type is rejected ----
 * Types 0 and 3 are neither DATA nor GAP: rejected, and the output buffer
 * is left exactly as it was (all 0x5A), so no half-written message exists. */
static void test_mse05_unknown_type_rejected(void)
{
    const uint8_t unknown_types[] = { 0x00, 0x03 };
    uint8_t untouched[GT_MSG_SIZE];
    memset(untouched, 0x5A, sizeof untouched);

    for (size_t type_index = 0; type_index < sizeof unknown_types; type_index++) {
        gt_msg message = { .type = unknown_types[type_index], .sensor_id = GT_SENSOR_LIGHT,
                           .counter = 1, .seq = 0, .value = 300 };
        uint8_t wire[GT_MSG_SIZE];
        memset(wire, 0x5A, sizeof wire);

        CHECK_EQ(gt_msg_encode(&message, wire), GT_ERR_INVAL);
        CHECK(memcmp(wire, untouched, sizeof wire) == 0);
    }
}

/* ---- MSE-06: a bad sensor_id is rejected ----
 * 0 (GT_SENSOR_ALL) is only meaningful in storage GAP records, never from
 * one sensor; 3 is no sensor at all. Output untouched, as in MSE-05. */
static void test_mse06_bad_sensor_id_rejected(void)
{
    const uint8_t bad_sensor_ids[] = { GT_SENSOR_ALL, 3 };
    uint8_t untouched[GT_MSG_SIZE];
    memset(untouched, 0x5A, sizeof untouched);

    for (size_t id_index = 0; id_index < sizeof bad_sensor_ids; id_index++) {
        gt_msg message = { .type = GT_MSG_DATA, .sensor_id = bad_sensor_ids[id_index],
                           .counter = 1, .seq = 0, .value = 300 };
        uint8_t wire[GT_MSG_SIZE];
        memset(wire, 0x5A, sizeof wire);

        CHECK_EQ(gt_msg_encode(&message, wire), GT_ERR_INVAL);
        CHECK(memcmp(wire, untouched, sizeof wire) == 0);
    }
}

/* ---- MSE-07: NULL arguments are rejected ---- */
static void test_mse07_null_args(void)
{
    gt_msg message = { .type = GT_MSG_DATA, .sensor_id = GT_SENSOR_LIGHT,
                       .counter = 1, .seq = 0, .value = 300 };
    uint8_t wire[GT_MSG_SIZE];

    CHECK_EQ(gt_msg_encode(NULL, wire), GT_ERR_INVAL);
    CHECK_EQ(gt_msg_encode(&message, NULL), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"MSE-01", "the message is 16 bytes", test_mse01_size_is_16},
    {"MSE-02", "header bytes 0-3", test_mse02_header_bytes},
    {"MSE-03", "u32 fields are little-endian at 4, 8, 12",
     test_mse03_u32_fields_little_endian},
    {"MSE-04", "a GAP message encodes", test_mse04_gap_encodes},
    {"MSE-05", "an unknown type is rejected", test_mse05_unknown_type_rejected},
    {"MSE-06", "a bad sensor_id is rejected", test_mse06_bad_sensor_id_rejected},
    {"MSE-07", "NULL arguments are rejected", test_mse07_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
