/* Unit tests for decoding the 16 B sensor -> hub message (src/ipc_msg.c):
 * gt_msg_decode.
 *
 * Purpose: prove decode reads every field from its documented offset
 * (ipc.h), undoes encode exactly, and rejects any message the hub must
 * discard (DESIGN.md failure-mode table, Hub: "Length/magic/version check",
 * logged as ipc_bad_message) without touching the output. Pure byte
 * unpacking: no sockets. Run under make test / asan / memcheck. Implements
 * MSD-01 through MSD-07. Encoding is in test_msg_encode.c.
 *
 * What these tests cannot prove: rejection of a truncated message. recv()
 * must return exactly GT_MSG_SIZE bytes; that check arrives with the IPC
 * code. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "ipc.h"

/* ---- MSD-01: decode undoes encode ----
 * A DATA and a GAP message, at the all-zero and all-max ends of the u32
 * fields, come back field for field. */
static void test_msd01_round_trip(void)
{
    const gt_msg originals[] = {
        { .type = GT_MSG_DATA, .sensor_id = GT_SENSOR_LIGHT,
          .counter = 0, .seq = 0, .value = 0 },
        { .type = GT_MSG_GAP, .sensor_id = GT_SENSOR_SOIL,
          .counter = UINT32_MAX, .seq = UINT32_MAX, .value = UINT32_MAX },
    };
    size_t original_count = sizeof originals / sizeof originals[0];

    for (size_t message_index = 0; message_index < original_count; message_index++) {
        const gt_msg *original = &originals[message_index];
        uint8_t wire[GT_MSG_SIZE];
        gt_msg decoded;

        CHECK_EQ(gt_msg_encode(original, wire), GT_OK);
        CHECK_EQ(gt_msg_decode(wire, &decoded), GT_OK);
        CHECK_EQ(decoded.type, original->type);
        CHECK_EQ(decoded.sensor_id, original->sensor_id);
        CHECK_EQ(decoded.counter, original->counter);
        CHECK_EQ(decoded.seq, original->seq);
        CHECK_EQ(decoded.value, original->value);
    }
}

/* A valid light DATA message written out by hand from the ipc.h layout:
 * counter 0x04030201, seq 0x08070605, value 0x0C0B0A09. MSD-03..06 copy
 * it and break one byte. */
static const uint8_t VALID_WIRE[GT_MSG_SIZE] = {
    GT_MSG_MAGIC, GT_MSG_VERSION, GT_MSG_DATA, GT_SENSOR_LIGHT,
    0x01, 0x02, 0x03, 0x04,    /* counter, lowest byte first */
    0x05, 0x06, 0x07, 0x08,    /* seq */
    0x09, 0x0A, 0x0B, 0x0C,    /* value */
};

/* ---- MSD-02: hand-built bytes decode ----
 * Checks decode against the documented layout directly, so a matching bug
 * in encode and decode (which MSD-01 alone would miss) still fails. */
static void test_msd02_hand_built_bytes(void)
{
    gt_msg decoded;

    CHECK_EQ(gt_msg_decode(VALID_WIRE, &decoded), GT_OK);
    CHECK_EQ(decoded.type, GT_MSG_DATA);
    CHECK_EQ(decoded.sensor_id, GT_SENSOR_LIGHT);
    CHECK_EQ(decoded.counter, 0x04030201u);
    CHECK_EQ(decoded.seq, 0x08070605u);
    CHECK_EQ(decoded.value, 0x0C0B0A09u);
}

/* ---- MSD-03: a bad magic byte is rejected ----
 * The first sign a stream is out of sync or the sender isn't ours. The
 * output is pre-filled with 0x5A and must come back unchanged, so the hub
 * can never act on a half-decoded message. */
static void test_msd03_bad_magic_rejected(void)
{
    uint8_t wire[GT_MSG_SIZE];
    memcpy(wire, VALID_WIRE, sizeof wire);
    wire[0] = 0xA7;   /* the on-disk record's magic: a record is not a message */

    gt_msg decoded, untouched;
    memset(&decoded, 0x5A, sizeof decoded);
    memset(&untouched, 0x5A, sizeof untouched);

    CHECK_EQ(gt_msg_decode(wire, &decoded), GT_ERR_CORRUPT);
    CHECK(memcmp(&decoded, &untouched, sizeof decoded) == 0);
}

/* ---- MSD-04: a bad version byte is rejected ----
 * A sensor built against a newer layout must not be misread as version 1. */
static void test_msd04_bad_version_rejected(void)
{
    uint8_t wire[GT_MSG_SIZE];
    memcpy(wire, VALID_WIRE, sizeof wire);
    wire[1] = GT_MSG_VERSION + 1;

    gt_msg decoded, untouched;
    memset(&decoded, 0x5A, sizeof decoded);
    memset(&untouched, 0x5A, sizeof untouched);

    CHECK_EQ(gt_msg_decode(wire, &decoded), GT_ERR_CORRUPT);
    CHECK(memcmp(&decoded, &untouched, sizeof decoded) == 0);
}

/* ---- MSD-05: an unknown type is rejected ----
 * Types 0 and 3 are neither DATA nor GAP. As DESIGN.md says for the log,
 * an unknown type is treated as corrupt. */
static void test_msd05_unknown_type_rejected(void)
{
    const uint8_t unknown_types[] = { 0x00, 0x03 };

    for (size_t type_index = 0; type_index < sizeof unknown_types; type_index++) {
        uint8_t wire[GT_MSG_SIZE];
        memcpy(wire, VALID_WIRE, sizeof wire);
        wire[2] = unknown_types[type_index];

        gt_msg decoded, untouched;
        memset(&decoded, 0x5A, sizeof decoded);
        memset(&untouched, 0x5A, sizeof untouched);

        CHECK_EQ(gt_msg_decode(wire, &decoded), GT_ERR_CORRUPT);
        CHECK(memcmp(&decoded, &untouched, sizeof decoded) == 0);
    }
}

/* ---- MSD-06: a bad sensor_id is rejected ----
 * Mirrors MSE-06: 0 (ALL) never comes from one sensor, and 3 is no sensor. */
static void test_msd06_bad_sensor_id_rejected(void)
{
    const uint8_t bad_sensor_ids[] = { GT_SENSOR_ALL, 3 };

    for (size_t id_index = 0; id_index < sizeof bad_sensor_ids; id_index++) {
        uint8_t wire[GT_MSG_SIZE];
        memcpy(wire, VALID_WIRE, sizeof wire);
        wire[3] = bad_sensor_ids[id_index];

        gt_msg decoded, untouched;
        memset(&decoded, 0x5A, sizeof decoded);
        memset(&untouched, 0x5A, sizeof untouched);

        CHECK_EQ(gt_msg_decode(wire, &decoded), GT_ERR_CORRUPT);
        CHECK(memcmp(&decoded, &untouched, sizeof decoded) == 0);
    }
}

/* ---- MSD-07: NULL arguments are rejected ----
 * GT_ERR_INVAL (a caller bug), not GT_ERR_CORRUPT (bad bytes). */
static void test_msd07_null_args(void)
{
    gt_msg decoded;

    CHECK_EQ(gt_msg_decode(NULL, &decoded), GT_ERR_INVAL);
    CHECK_EQ(gt_msg_decode(VALID_WIRE, NULL), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"MSD-01", "decode undoes encode", test_msd01_round_trip},
    {"MSD-02", "hand-built bytes decode", test_msd02_hand_built_bytes},
    {"MSD-03", "a bad magic byte is rejected", test_msd03_bad_magic_rejected},
    {"MSD-04", "a bad version byte is rejected", test_msd04_bad_version_rejected},
    {"MSD-05", "an unknown type is rejected", test_msd05_unknown_type_rejected},
    {"MSD-06", "a bad sensor_id is rejected", test_msd06_bad_sensor_id_rejected},
    {"MSD-07", "NULL arguments are rejected", test_msd07_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
