/* Unit tests for reading the BH1750 driver (src/light_sensor.c) against a
 * fake bus: gt_light_read and gt_light_raw_to_lux.
 *
 * Purpose: prove decoding and lux conversion, that reads send no commands,
 * the read retry policy and its timing budget, and that bad reads and bad
 * arguments never produce a value. Run under make test / asan / memcheck.
 * Implements LSR-01 through LSR-10. Opening is in test_light_open.c,
 * closing in test_light_close.c.
 *
 * What these tests cannot prove: that sampling runs at 1 Hz. That belongs
 * to the sampler, which schedules the reads this driver only performs. */
#include <stdint.h>

#include "check.h"
#include "fake_i2c.h"
#include "light_sensor.h"

/* Marker for "the driver must not have written to raw_out". */
#define UNTOUCHED 0xDEADBEEFu

/* Reset `f` and open a sensor on it; the init traffic is left in `f`. */
static gt_light_sensor *open_on(fake_i2c *f)
{
    return gt_light_open_bus(fake_i2c_bus(f));
}

static int near(double a, double b)
{
    double d = a - b;
    return d < 0.01 && d > -0.01;
}

/* ---- LSR-01: data decodes big-endian ----
 * Data arrives high byte first: 0x01 0x2C = 300. */
static void test_lsr01_read_decodes_big_endian(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);
    const uint8_t data[] = { 0x01, 0x2C };
    fake_i2c_push_read(&f, data, sizeof data);

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_light_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 300);

    gt_light_close(s);
}

/* ---- LSR-02: full 16-bit range decodes ----
 * 0 (dark) and 65535 (saturated). */
static void test_lsr02_read_range_limits(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);
    const uint8_t data[] = { 0x00, 0x00, 0xFF, 0xFF };
    fake_i2c_push_read(&f, data, sizeof data);

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_light_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 0);
    CHECK_EQ(gt_light_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 65535);

    gt_light_close(s);
}

/* ---- LSR-03: back-to-back reads return measurements in order ---- */
static void test_lsr03_reads_in_order(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);
    const uint8_t data[] = { 0x01, 0x2C, 0x00, 0x00, 0x00, 0x78 };
    fake_i2c_push_read(&f, data, sizeof data);

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_light_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 300);
    CHECK_EQ(gt_light_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 0);
    CHECK_EQ(gt_light_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 120);

    gt_light_close(s);
}

/* ---- LSR-04: reads send no commands ----
 * In continuous mode reads are pure reads: re-sending a mode command would
 * restart the conversion and the next read would return stale data. */
static void test_lsr04_reads_send_no_commands(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);
    const uint8_t data[] = { 0x00, 0x10, 0x00, 0x20, 0x00, 0x30 };
    fake_i2c_push_read(&f, data, sizeof data);
    int writes_after_open = f.write_calls;

    uint32_t raw;
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(gt_light_read(s, &raw), GT_OK);
    }
    CHECK_EQ(f.write_calls, writes_after_open);

    gt_light_close(s);
}

/* ---- LSR-05: lux = count / 1.2 ----
 * High-res mode conversion (datasheet). */
static void test_lsr05_lux_conversion(void)
{
    CHECK(near(gt_light_raw_to_lux(0), 0.0));
    CHECK(near(gt_light_raw_to_lux(120), 100.0));
    CHECK(near(gt_light_raw_to_lux(65535), 54612.5));
}

/* ---- LSR-06: a transient failure is retried and recovers ----
 * Retried after a 10 ms gap and the read succeeds. */
static void test_lsr06_read_retry_recovers(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);
    const uint8_t data[] = { 0x01, 0x2C };
    fake_i2c_push_read(&f, data, sizeof data);
    int sleeps_after_open = f.sleep_calls;
    uint32_t slept_after_open = f.slept_ms_total;
    f.fail_reads = 2;

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_light_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 300);
    CHECK_EQ(f.read_calls, 3);
    CHECK_EQ(f.sleep_calls - sleeps_after_open, 2);
    CHECK_EQ(f.slept_ms_total - slept_after_open, 2 * GT_LIGHT_RETRY_GAP_MS);

    gt_light_close(s);
}

/* ---- LSR-07: retries exhausted returns GT_ERR_IO within budget ----
 * Every attempt fails: 1 try + 3 retries, 3 gaps, then GT_ERR_IO with
 * raw_out untouched so the caller skips the sample instead of storing junk.
 * Also the timing budget: even this worst case waits only 30 ms, well
 * inside the 1 s light period, so retries cannot make sampling miss a slot. */
static void test_lsr07_read_retry_exhausted(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);
    int sleeps_after_open = f.sleep_calls;
    uint32_t slept_after_open = f.slept_ms_total;
    f.fail_reads = 1 + GT_LIGHT_RETRIES;

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_light_read(s, &raw), GT_ERR_IO);
    CHECK_EQ(raw, UNTOUCHED);
    CHECK_EQ(f.read_calls, 1 + GT_LIGHT_RETRIES);
    CHECK_EQ(f.sleep_calls - sleeps_after_open, GT_LIGHT_RETRIES);

    uint32_t waited = f.slept_ms_total - slept_after_open;
    CHECK_EQ(waited, GT_LIGHT_RETRIES * GT_LIGHT_RETRY_GAP_MS);
    CHECK(waited < 1000);

    gt_light_close(s);
}

/* ---- LSR-08: a short read is an error ----
 * The device stopped after 1 byte: a failure, not a value. */
static void test_lsr08_read_short_is_error(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);
    const uint8_t half[] = { 0x01 };
    fake_i2c_push_read(&f, half, sizeof half);

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_light_read(s, &raw), GT_ERR_IO);
    CHECK_EQ(raw, UNTOUCHED);

    gt_light_close(s);
}

/* ---- LSR-09: the handle works again after an outage ----
 * After a read gives up (sensor briefly unplugged), the same handle works
 * again once the device answers; no reopen is needed. */
static void test_lsr09_read_works_after_outage(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);
    f.fail_reads = 1 + GT_LIGHT_RETRIES;

    uint32_t raw = UNTOUCHED;
    CHECK_EQ(gt_light_read(s, &raw), GT_ERR_IO);

    const uint8_t data[] = { 0x00, 0x78 };
    fake_i2c_push_read(&f, data, sizeof data);
    CHECK_EQ(gt_light_read(s, &raw), GT_OK);
    CHECK_EQ(raw, 120);

    gt_light_close(s);
}

/* ---- LSR-10: NULL arguments are rejected ----
 * Rejected without touching the bus. */
static void test_lsr10_null_args(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);
    uint32_t raw = UNTOUCHED;

    CHECK_EQ(gt_light_read(NULL, &raw), GT_ERR_INVAL);
    CHECK_EQ(gt_light_read(s, NULL), GT_ERR_INVAL);
    CHECK_EQ(f.read_calls, 0);
    CHECK_EQ(raw, UNTOUCHED);

    gt_light_close(s);
    gt_light_close(NULL);
}

static const test_case_t tests[] = {
    {"LSR-01", "data decodes big-endian", test_lsr01_read_decodes_big_endian},
    {"LSR-02", "full 16-bit range decodes", test_lsr02_read_range_limits},
    {"LSR-03", "back-to-back reads return in order", test_lsr03_reads_in_order},
    {"LSR-04", "reads send no commands", test_lsr04_reads_send_no_commands},
    {"LSR-05", "lux = count / 1.2", test_lsr05_lux_conversion},
    {"LSR-06", "transient failure is retried and recovers", test_lsr06_read_retry_recovers},
    {"LSR-07", "retries exhausted returns GT_ERR_IO in budget",
     test_lsr07_read_retry_exhausted},
    {"LSR-08", "a short read is an error", test_lsr08_read_short_is_error},
    {"LSR-09", "handle works again after an outage", test_lsr09_read_works_after_outage},
    {"LSR-10", "NULL arguments are rejected", test_lsr10_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
