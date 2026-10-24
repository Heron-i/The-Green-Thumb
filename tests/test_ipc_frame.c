/* Unit tests for the hub's receive-side check (src/ipc_msg.c):
 * gt_ipc_frame_decode.
 *
 * Purpose: prove the hub accepts exactly one whole 16 B message per recv()
 * and rejects anything else before decoding it: a short or oversized
 * message is bad bytes (GT_ERR_CORRUPT, logged as ipc_bad_message), while
 * recv() returning 0 or -1 means the connection is gone (GT_ERR_IO). This
 * is the "a message shorter than 16 B is rejected at recv()" rule from
 * DESIGN.md. Pure logic: the length is passed in, so no socket is needed.
 * Run under make test / asan / memcheck. Implements IFD-01 through IFD-06.
 *
 * What these tests cannot prove: that SOCK_SEQPACKET really delivers one
 * message per recv(). That's IPL-05 in test_ipc_loopback.c, over a real
 * socket. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "ipc.h"

/* The hub recv()s into one byte more than a message, so an oversized
 * message shows up as GT_MSG_SIZE + 1 instead of being cut off silently. */
#define RECV_BUF_SIZE (GT_MSG_SIZE + 1)

/* A recv() buffer holding a valid light DATA message from the ipc.h layout
 * (counter 0x04030201, seq 0x08070605, value 0x0C0B0A09), plus one spare
 * byte. Each case passes a different `len` with these same bytes. */
static const uint8_t VALID_BUF[RECV_BUF_SIZE] = {
    GT_MSG_MAGIC, GT_MSG_VERSION, GT_MSG_DATA, GT_SENSOR_LIGHT,
    0x01, 0x02, 0x03, 0x04,    /* counter, lowest byte first */
    0x05, 0x06, 0x07, 0x08,    /* seq */
    0x09, 0x0A, 0x0B, 0x0C,    /* value */
    0xEE,                      /* spare byte: only read when len is 17 */
};

/* ---- IFD-01: exactly 16 B decodes ----
 * The normal case: one whole message, decoded field for field. */
static void test_ifd01_exact_size_decodes(void)
{
    gt_msg decoded;

    CHECK_EQ(gt_ipc_frame_decode(VALID_BUF, GT_MSG_SIZE, &decoded), GT_OK);
    CHECK_EQ(decoded.type, GT_MSG_DATA);
    CHECK_EQ(decoded.sensor_id, GT_SENSOR_LIGHT);
    CHECK_EQ(decoded.counter, 0x04030201u);
    CHECK_EQ(decoded.seq, 0x08070605u);
    CHECK_EQ(decoded.value, 0x0C0B0A09u);
}

/* ---- IFD-02: a 15 B message is rejected ----
 * The first 15 bytes are valid, so only the length check can catch it.
 * The output is pre-filled with 0x5A and must come back unchanged. */
static void test_ifd02_short_message_rejected(void)
{
    gt_msg decoded;
    memset(&decoded, 0x5A, sizeof decoded);
    gt_msg untouched = decoded;

    CHECK_EQ(gt_ipc_frame_decode(VALID_BUF, GT_MSG_SIZE - 1, &decoded), GT_ERR_CORRUPT);
    CHECK(memcmp(&decoded, &untouched, sizeof decoded) == 0);
}

/* ---- IFD-03: a 17 B message is rejected ----
 * A sender writing more than one message's worth. The first 16 bytes are a
 * valid message, so decoding them anyway would hide the bug. */
static void test_ifd03_long_message_rejected(void)
{
    gt_msg decoded;
    memset(&decoded, 0x5A, sizeof decoded);
    gt_msg untouched = decoded;

    CHECK_EQ(gt_ipc_frame_decode(VALID_BUF, RECV_BUF_SIZE, &decoded), GT_ERR_CORRUPT);
    CHECK(memcmp(&decoded, &untouched, sizeof decoded) == 0);
}

/* ---- IFD-04: a closed or failed recv() is IO ----
 * recv() returns 0 when the sensor closed its end and -1 when it failed.
 * Neither is a bad message: the hub just drops that connection, so these
 * must be GT_ERR_IO, not GT_ERR_CORRUPT (which would log ipc_bad_message). */
static void test_ifd04_closed_or_failed_recv_is_io(void)
{
    gt_msg decoded;
    memset(&decoded, 0x5A, sizeof decoded);
    gt_msg untouched = decoded;

    CHECK_EQ(gt_ipc_frame_decode(VALID_BUF, 0, &decoded), GT_ERR_IO);
    CHECK_EQ(gt_ipc_frame_decode(VALID_BUF, -1, &decoded), GT_ERR_IO);
    CHECK(memcmp(&decoded, &untouched, sizeof decoded) == 0);
}

/* ---- IFD-05: right size, bad magic, is rejected ----
 * Passing the length check isn't enough: the bytes still go through
 * gt_msg_decode's header checks. 0xA7 is the on-disk record's magic, so
 * this is also a record sent to the wrong socket. */
static void test_ifd05_bad_magic_still_rejected(void)
{
    uint8_t buf[RECV_BUF_SIZE];
    memcpy(buf, VALID_BUF, sizeof buf);
    buf[0] = 0xA7;

    gt_msg decoded;
    memset(&decoded, 0x5A, sizeof decoded);
    gt_msg untouched = decoded;

    CHECK_EQ(gt_ipc_frame_decode(buf, GT_MSG_SIZE, &decoded), GT_ERR_CORRUPT);
    CHECK(memcmp(&decoded, &untouched, sizeof decoded) == 0);
}

/* ---- IFD-06: NULL arguments are rejected ----
 * GT_ERR_INVAL (a caller bug) wins over every length result, even a
 * length that would otherwise be GT_ERR_IO. */
static void test_ifd06_null_args(void)
{
    gt_msg decoded;

    CHECK_EQ(gt_ipc_frame_decode(NULL, GT_MSG_SIZE, &decoded), GT_ERR_INVAL);
    CHECK_EQ(gt_ipc_frame_decode(VALID_BUF, GT_MSG_SIZE, NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_ipc_frame_decode(NULL, 0, &decoded), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"IFD-01", "exactly 16 B decodes", test_ifd01_exact_size_decodes},
    {"IFD-02", "a 15 B message is rejected", test_ifd02_short_message_rejected},
    {"IFD-03", "a 17 B message is rejected", test_ifd03_long_message_rejected},
    {"IFD-04", "a closed or failed recv() is IO", test_ifd04_closed_or_failed_recv_is_io},
    {"IFD-05", "right size, bad magic, is rejected", test_ifd05_bad_magic_still_rejected},
    {"IFD-06", "NULL arguments are rejected", test_ifd06_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
