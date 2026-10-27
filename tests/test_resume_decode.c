/* Unit tests for decoding the 8 B seq_resume reply (src/storage_proto.c):
 * gt_resume_decode.
 *
 * Purpose: prove decode reads every field from its documented offset
 * (storage_proto.h), undoes encode exactly, and rejects anything the hub
 * must not act on: a message or record mistaken for a reply (wrong
 * magic), another version or type, a has_last other than 0/1, or a seq
 * sent with has_last 0. The output is untouched on every failure. Pure
 * byte unpacking: no sockets. Run under make test / asan / memcheck.
 * Implements RSD-01 through RSD-06. Encoding is in test_resume_encode.c.
 *
 * What these tests cannot prove: that the hub continues from seq + 1
 * (the hub process, not written yet). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "storage_proto.h"

/* A valid reply written out by hand: has_last 1, seq 0x04030201. */
static const uint8_t VALID_REPLY[GT_RESUME_SIZE] = {
    0xA8, 0x01, 0x10, 0x01, 0x01, 0x02, 0x03, 0x04,
};

/* Decode `bytes` and check it is rejected as corrupt with the output left
 * exactly as it was (pre-filled with 0x5A). */
static void check_rejected_untouched(const uint8_t bytes[GT_RESUME_SIZE])
{
    gt_resume decoded, untouched;
    memset(&decoded, 0x5A, sizeof decoded);
    memset(&untouched, 0x5A, sizeof untouched);

    CHECK_EQ(gt_resume_decode(bytes, &decoded), GT_ERR_CORRUPT);
    CHECK(memcmp(&decoded, &untouched, sizeof decoded) == 0);
}

/* ---- RSD-01: hand-built bytes decode ---- */
static void test_rsd01_hand_built_bytes(void)
{
    gt_resume decoded;

    CHECK_EQ(gt_resume_decode(VALID_REPLY, &decoded), GT_OK);
    CHECK_EQ(decoded.has_last, 1);
    CHECK_EQ(decoded.seq, 0x04030201u);
}

/* ---- RSD-02: decode undoes encode ----
 * Nothing stored, seq 0 stored, and seq UINT32_MAX stored. */
static void test_rsd02_round_trip(void)
{
    const gt_resume originals[] = {
        { .has_last = 0, .seq = 0 },
        { .has_last = 1, .seq = 0 },
        { .has_last = 1, .seq = UINT32_MAX },
    };
    for (size_t original_index = 0; original_index < 3; original_index++) {
        uint8_t bytes[GT_RESUME_SIZE];
        gt_resume decoded;

        CHECK_EQ(gt_resume_encode(&originals[original_index], bytes), GT_OK);
        CHECK_EQ(gt_resume_decode(bytes, &decoded), GT_OK);
        CHECK_EQ(decoded.has_last, originals[original_index].has_last);
        CHECK_EQ(decoded.seq, originals[original_index].seq);
    }
}

/* ---- RSD-03: a wrong magic is rejected ----
 * 0xA6 (the sensor message) and 0xA7 (the record) must never be read as
 * a reply. */
static void test_rsd03_wrong_magic(void)
{
    const uint8_t wrong_magics[] = { 0xA6, 0xA7, 0x00 };
    for (size_t magic_index = 0; magic_index < sizeof wrong_magics; magic_index++) {
        uint8_t bytes[GT_RESUME_SIZE];
        memcpy(bytes, VALID_REPLY, sizeof bytes);
        bytes[0] = wrong_magics[magic_index];
        check_rejected_untouched(bytes);
    }
}

/* ---- RSD-04: a wrong version or type is rejected ---- */
static void test_rsd04_wrong_version_or_type(void)
{
    uint8_t bytes[GT_RESUME_SIZE];

    memcpy(bytes, VALID_REPLY, sizeof bytes);
    bytes[1] = GT_RESUME_VERSION + 1;
    check_rejected_untouched(bytes);

    memcpy(bytes, VALID_REPLY, sizeof bytes);
    bytes[2] = 0x11;
    check_rejected_untouched(bytes);
}

/* ---- RSD-05: has_last must be 0 or 1 with seq ----
 * has_last 2 is rejected, and so is a non-zero seq with has_last 0. */
static void test_rsd05_has_last_rules(void)
{
    uint8_t bytes[GT_RESUME_SIZE];

    memcpy(bytes, VALID_REPLY, sizeof bytes);
    bytes[3] = 2;
    check_rejected_untouched(bytes);

    memcpy(bytes, VALID_REPLY, sizeof bytes);
    bytes[3] = 0;   /* seq bytes are still 01 02 03 04 */
    check_rejected_untouched(bytes);
}

/* ---- RSD-06: NULL arguments are rejected ---- */
static void test_rsd06_null_args(void)
{
    gt_resume decoded;

    CHECK_EQ(gt_resume_decode(NULL, &decoded), GT_ERR_INVAL);
    CHECK_EQ(gt_resume_decode(VALID_REPLY, NULL), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"RSD-01", "hand-built bytes decode", test_rsd01_hand_built_bytes},
    {"RSD-02", "decode undoes encode", test_rsd02_round_trip},
    {"RSD-03", "a wrong magic is rejected", test_rsd03_wrong_magic},
    {"RSD-04", "a wrong version or type is rejected", test_rsd04_wrong_version_or_type},
    {"RSD-05", "has_last must be 0 or 1 with seq", test_rsd05_has_last_rules},
    {"RSD-06", "NULL arguments are rejected", test_rsd06_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
