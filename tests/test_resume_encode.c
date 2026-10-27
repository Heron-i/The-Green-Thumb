/* Unit tests for encoding the 8 B seq_resume reply (src/storage_proto.c):
 * gt_resume_encode.
 *
 * Purpose: prove every field lands at its documented offset
 * (storage_proto.h) in little-endian order, that "nothing stored yet" is
 * always written as has_last 0 with seq 0, and that NULL arguments fail
 * without touching the output. Pure byte packing: no sockets. Run under
 * make test / asan / memcheck. Implements RSE-01 through RSE-04. Decoding
 * is in test_resume_decode.c.
 *
 * What these tests cannot prove: that the reply reaches the hub when it
 * connects (storage_main, Mechanism D Step 12d). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "storage_proto.h"

/* ---- RSE-01: the reply is 8 bytes ---- */
static void test_rse01_size_is_8(void)
{
    CHECK_EQ(GT_RESUME_SIZE, 8);
}

/* ---- RSE-02: encode matches the hand-built bytes ----
 * has_last 1, seq 0x04030201: magic, version, type, has_last, then seq
 * lowest byte first. */
static void test_rse02_encode_layout(void)
{
    const gt_resume resume = { .has_last = 1, .seq = 0x04030201u };
    const uint8_t expected[GT_RESUME_SIZE] = { 0xA8, 0x01, 0x10, 0x01, 0x01, 0x02, 0x03, 0x04 };
    uint8_t out[GT_RESUME_SIZE];

    CHECK_EQ(gt_resume_encode(&resume, out), GT_OK);
    CHECK(memcmp(out, expected, sizeof out) == 0);
}

/* ---- RSE-03: nothing stored is has_last 0, seq 0 ----
 * A stray seq with has_last 0 is not sent, and a has_last of 5 is
 * written as 1. */
static void test_rse03_nothing_stored_zero_seq(void)
{
    const gt_resume empty = { .has_last = 0, .seq = 99 };
    const gt_resume truthy = { .has_last = 5, .seq = 7 };
    const uint8_t expected_empty[GT_RESUME_SIZE] = { 0xA8, 0x01, 0x10, 0x00, 0, 0, 0, 0 };
    uint8_t out[GT_RESUME_SIZE];

    CHECK_EQ(gt_resume_encode(&empty, out), GT_OK);
    CHECK(memcmp(out, expected_empty, sizeof out) == 0);
    CHECK_EQ(gt_resume_encode(&truthy, out), GT_OK);
    CHECK_EQ(out[3], 1);
    CHECK_EQ(out[4], 7);
}

/* ---- RSE-04: NULL arguments are rejected ----
 * And the output is untouched. */
static void test_rse04_null_args(void)
{
    const gt_resume resume = { .has_last = 1, .seq = 1 };
    uint8_t out[GT_RESUME_SIZE], untouched[GT_RESUME_SIZE];
    memset(out, 0x5A, sizeof out);
    memset(untouched, 0x5A, sizeof untouched);

    CHECK_EQ(gt_resume_encode(NULL, out), GT_ERR_INVAL);
    CHECK_EQ(gt_resume_encode(&resume, NULL), GT_ERR_INVAL);
    CHECK(memcmp(out, untouched, sizeof out) == 0);
}

static const test_case_t tests[] = {
    {"RSE-01", "the reply is 8 bytes", test_rse01_size_is_8},
    {"RSE-02", "encode matches the hand-built bytes", test_rse02_encode_layout},
    {"RSE-03", "nothing stored is has_last 0, seq 0", test_rse03_nothing_stored_zero_seq},
    {"RSE-04", "NULL arguments are rejected", test_rse04_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
