/* Unit tests for reading the STEMMA seesaw soil driver (src/soil_sensor.c)
 * against a fake bus: gt_soil_read.
 *
 * Purpose: prove decoding, the per-read register write and wait, the read
 * retry policy (including the seesaw's 0xFFFF error value) and its timing
 * budget, and that bad reads and bad arguments never produce a value.
 * Run under make test / asan / memcheck.
 * Implements SSR-01 through SSR-10. Opening is in test_soil_open.c,
 * closing in test_soil_close.c.
 *
 * What these tests cannot prove: that sampling runs once per 60 s. That
 * belongs to the sampler, which schedules the reads this driver performs. */
#include <stdint.h>

#include "check.h"
#include "fake_i2c.h"
#include "soil_sensor.h"

/* Marker for "the driver must not have written to raw_out". */
#define UNTOUCHED 0xDEADBEEFu

/* Reset `f`, script the seesaw's ID byte, and open a sensor on it; the init
 * traffic is left in `f`. */
static gt_soil_sensor *open_on(fake_i2c *f)
{
    gt_i2c_bus bus = fake_i2c_bus(f);
    const uint8_t hw_id[] = { GT_SEESAW_HW_ID_CODE };
    fake_i2c_push_read(f, hw_id, sizeof hw_id);
    return gt_soil_open_bus(bus);
}

/* ---- SSR-01: data decodes big-endian ----
 * Data arrives high byte first: 0x01 0x90 = 400. */
static void test_ssr01_read_decodes_big_endian(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    const uint8_t data[] = { 0x01, 0x90 };
    fake_i2c_push_read(&f, data, sizeof data);

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_soil_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 400);

    gt_soil_close(s);
}

/* ---- SSR-02: each read selects the moisture register, then waits ----
 * Writes exactly {0x0F 0x10}, waits GT_SOIL_CONV_MS, then reads 2 bytes. */
static void test_ssr02_read_selects_register(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    const uint8_t data[] = { 0x01, 0x90 };
    fake_i2c_push_read(&f, data, sizeof data);
    size_t written_after_open = f.n_written;
    uint32_t slept_after_open = f.slept_ms_total;

    uint32_t raw;
    CHECK_EQ(gt_soil_read(s, &raw), GT_OK);
    CHECK_EQ(f.n_written - written_after_open, 2);
    CHECK_EQ(f.written[written_after_open], GT_SEESAW_TOUCH_BASE);
    CHECK_EQ(f.written[written_after_open + 1], GT_SEESAW_TOUCH_CH0);
    CHECK_EQ(f.slept_ms_total - slept_after_open, GT_SOIL_CONV_MS);

    gt_soil_close(s);
}

/* ---- SSR-03: back-to-back reads return measurements in order ---- */
static void test_ssr03_reads_in_order(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    const uint8_t data[] = { 0x01, 0x2C, 0x03, 0x20, 0x00, 0xC8 };
    fake_i2c_push_read(&f, data, sizeof data);

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_soil_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 300);
    CHECK_EQ(gt_soil_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 800);
    CHECK_EQ(gt_soil_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 200);

    gt_soil_close(s);
}

/* ---- SSR-04: a transient read failure is retried and recovers ----
 * Retried after a 10 ms gap, re-selecting the register each attempt. */
static void test_ssr04_read_retry_recovers(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    const uint8_t data[] = { 0x01, 0x90 };
    fake_i2c_push_read(&f, data, sizeof data);
    int writes_after_open = f.write_calls;
    int reads_after_open = f.read_calls;
    uint32_t slept_after_open = f.slept_ms_total;
    f.fail_reads = 1;

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_soil_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 400);
    CHECK_EQ(f.write_calls - writes_after_open, 2);
    CHECK_EQ(f.read_calls - reads_after_open, 2);
    CHECK_EQ(f.slept_ms_total - slept_after_open,
             2 * GT_SOIL_CONV_MS + GT_SOIL_RETRY_GAP_MS);

    gt_soil_close(s);
}

/* ---- SSR-05: 0xFFFF is a failed read, not a value ----
 * The seesaw reports an error as 0xFFFF (DESIGN.md failure-mode table):
 * it is retried, and the next good value is returned. */
static void test_ssr05_error_value_is_retried(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    const uint8_t data[] = { 0xFF, 0xFF, 0x01, 0x90 };
    fake_i2c_push_read(&f, data, sizeof data);
    int reads_after_open = f.read_calls;

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_soil_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 400);
    CHECK_EQ(f.read_calls - reads_after_open, 2);

    gt_soil_close(s);
}

/* ---- SSR-06: a register-write NACK is a failed attempt ----
 * Without the register selected there is nothing to read, so the attempt
 * skips the read and the next attempt starts over. */
static void test_ssr06_write_nack_is_retried(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    const uint8_t data[] = { 0x01, 0x90 };
    fake_i2c_push_read(&f, data, sizeof data);
    int writes_after_open = f.write_calls;
    int reads_after_open = f.read_calls;
    f.fail_writes = 1;

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_soil_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 400);
    CHECK_EQ(f.write_calls - writes_after_open, 2);
    CHECK_EQ(f.read_calls - reads_after_open, 1);

    gt_soil_close(s);
}

/* ---- SSR-07: retries exhausted returns GT_ERR_IO within budget ----
 * Every attempt fails: 1 try + 3 retries, then GT_ERR_IO with raw_out
 * untouched so the caller skips the sample. Worst-case wait is
 * 4 x 5 ms + 3 x 10 ms = 50 ms, far inside the 60 s soil period. */
static void test_ssr07_read_retry_exhausted(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    int reads_after_open = f.read_calls;
    uint32_t slept_after_open = f.slept_ms_total;
    f.fail_reads = 1 + GT_SOIL_RETRIES;

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_soil_read(s, &raw), GT_ERR_IO);
    CHECK_EQ(raw, UNTOUCHED);
    CHECK_EQ(f.read_calls - reads_after_open, 1 + GT_SOIL_RETRIES);

    uint32_t waited = f.slept_ms_total - slept_after_open;
    CHECK_EQ(waited, (1 + GT_SOIL_RETRIES) * GT_SOIL_CONV_MS
                     + GT_SOIL_RETRIES * GT_SOIL_RETRY_GAP_MS);
    CHECK(waited < 1000);

    gt_soil_close(s);
}

/* ---- SSR-08: only 0xFFFF on every attempt is GT_ERR_IO ----
 * A stuck seesaw that answers but keeps reporting its error value never
 * produces a stored reading. */
static void test_ssr08_error_value_exhausted(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    const uint8_t error_value[] = { 0xFF, 0xFF };
    for (int attempt = 0; attempt <= GT_SOIL_RETRIES; attempt++) {
        fake_i2c_push_read(&f, error_value, sizeof error_value);
    }

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_soil_read(s, &raw), GT_ERR_IO);
    CHECK_EQ(raw, UNTOUCHED);

    gt_soil_close(s);
}

/* ---- SSR-09: the handle works again after an outage ----
 * After a read gives up (probe cable briefly pulled), the same handle
 * works again once the device answers; no reopen is needed. */
static void test_ssr09_read_works_after_outage(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    f.fail_reads = 1 + GT_SOIL_RETRIES;

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_soil_read(s, &raw), GT_ERR_IO);

    const uint8_t data[] = { 0x02, 0x58 };
    fake_i2c_push_read(&f, data, sizeof data);
    CHECK_EQ(gt_soil_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 600);

    gt_soil_close(s);
}

/* ---- SSR-10: NULL arguments are rejected ----
 * Rejected without touching the bus. */
static void test_ssr10_null_args_rejected(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    int writes_after_open = f.write_calls;
    int reads_after_open = f.read_calls;

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_soil_read(NULL, &raw), GT_ERR_INVAL);
    CHECK_EQ(gt_soil_read(s, NULL), GT_ERR_INVAL);
    CHECK_EQ(raw, UNTOUCHED);
    CHECK_EQ(f.write_calls, writes_after_open);
    CHECK_EQ(f.read_calls, reads_after_open);

    gt_soil_close(s);
}

static const test_case_t tests[] = {
    {"SSR-01", "data decodes big-endian", test_ssr01_read_decodes_big_endian},
    {"SSR-02", "read selects moisture register, then waits",
     test_ssr02_read_selects_register},
    {"SSR-03", "back-to-back reads return in order", test_ssr03_reads_in_order},
    {"SSR-04", "transient read failure is retried", test_ssr04_read_retry_recovers},
    {"SSR-05", "0xFFFF is a failed read, not a value", test_ssr05_error_value_is_retried},
    {"SSR-06", "register-write NACK is a failed attempt",
     test_ssr06_write_nack_is_retried},
    {"SSR-07", "retries exhausted -> GT_ERR_IO in budget", test_ssr07_read_retry_exhausted},
    {"SSR-08", "0xFFFF on every attempt -> GT_ERR_IO", test_ssr08_error_value_exhausted},
    {"SSR-09", "handle works again after an outage", test_ssr09_read_works_after_outage},
    {"SSR-10", "NULL arguments are rejected", test_ssr10_null_args_rejected},
};

int main(void)
{
    return RUN_TESTS(tests);
}
