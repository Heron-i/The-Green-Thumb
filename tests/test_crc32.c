/* Unit tests for the record checksum (src/log_record.c): gt_crc32.
 *
 * Purpose: prove gt_crc32 is the standard CRC-32 (the one zlib, Ethernet
 * and PNG use), so the log can be checked by any off-the-shelf tool, and
 * that it catches the single-bit damage a power cut or bad SD write can
 * leave behind. Pure arithmetic: no files. Run under make test / asan /
 * memcheck. Implements CRC-01 through CRC-03. The record codec that uses
 * it is in test_record_encode.c and test_record_decode.c.
 *
 * What these tests cannot prove: that every possible corruption is caught.
 * CRC-32 misses about 1 in 4 billion random multi-byte errors. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "log_record.h"

/* The standard check input: the CRC of these 9 ASCII bytes is published
 * for every CRC variant, so matching it pins down the exact algorithm. */
static const char CHECK_INPUT[] = "123456789";
#define CHECK_INPUT_LEN 9

/* ---- CRC-01: "123456789" gives the standard check value ----
 * 0xCBF43926 is the published CRC-32 check value (also what Python's
 * zlib.crc32(b"123456789") returns). */
static void test_crc01_standard_check_value(void)
{
    CHECK_EQ(gt_crc32(CHECK_INPUT, CHECK_INPUT_LEN), 0xCBF43926u);
}

/* ---- CRC-02: zero-length input gives 0 ----
 * The start value and the final XOR are both 0xFFFFFFFF, so with no bytes
 * they cancel out. The buffer is real but must not be read. */
static void test_crc02_empty_input_is_zero(void)
{
    const uint8_t unused_byte = 0xFF;

    CHECK_EQ(gt_crc32(&unused_byte, 0), 0x00000000u);
}

/* ---- CRC-03: changing any one bit changes the CRC ----
 * Flips each of the 72 bits of the check input in turn. CRC-32 is
 * guaranteed to catch every single-bit error, so none may match. */
static void test_crc03_one_bit_flip_detected(void)
{
    uint32_t original_crc = gt_crc32(CHECK_INPUT, CHECK_INPUT_LEN);
    uint8_t damaged[CHECK_INPUT_LEN];

    /* invariant: before each flip, `damaged` equals the original input */
    for (size_t bit_index = 0; bit_index < CHECK_INPUT_LEN * 8; bit_index++) {
        memcpy(damaged, CHECK_INPUT, CHECK_INPUT_LEN);
        damaged[bit_index / 8] ^= (uint8_t)(1u << (bit_index % 8));

        CHECK(gt_crc32(damaged, CHECK_INPUT_LEN) != original_crc);
    }
}

static const test_case_t tests[] = {
    {"CRC-01", "\"123456789\" gives the standard value",
     test_crc01_standard_check_value},
    {"CRC-02", "zero-length input gives 0", test_crc02_empty_input_is_zero},
    {"CRC-03", "changing any one bit changes the CRC",
     test_crc03_one_bit_flip_detected},
};

int main(void)
{
    return RUN_TESTS(tests);
}
