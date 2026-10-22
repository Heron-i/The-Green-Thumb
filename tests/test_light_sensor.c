/* Unit tests for the BH1750 driver (src/light_sensor.c) against a fake bus.
 *
 * Purpose: prove the driver's side of every hardware interaction without a
 * Pi: detection of a missing device, the init command sequence, decoding
 * and lux conversion, the read retry policy, and cleanup on every path.
 * Run under make test / asan / memcheck, so leaks and double-closes also fail.
 *
 * What these tests cannot prove: that a real chip sits at 0x23 on the wire,
 * or that sampling runs at 1 Hz. The first needs the Pi (i2cdetect / the
 * light_probe tool); the second belongs to the sampler, which schedules the
 * reads this driver only performs. */
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

/* --- detection and address ---------------------------------------------- */

/* The address is fixed by the ADDR pin wiring (low/floating = 0x23); pin it
 * so a typo can't silently point the driver at another device. */
static void test_address_is_0x23(void)
{
    CHECK_EQ(GT_LIGHT_I2C_ADDR, 0x23);
}

/* No bus device at all (I2C disabled, wrong path): this runs the real
 * gt_i2c_linux_open, whose open() fails, so open must return NULL. */
static void test_open_without_bus_device_fails(void)
{
    CHECK(gt_light_open("/nonexistent/i2c-1") == NULL);
    CHECK(gt_light_open(NULL) == NULL);
}

/* Nothing answers at the address: the first write (POWER_ON) is NACKed.
 * Open must fail, close the bus it was given exactly once, and not wait. */
static void test_open_missing_device_fails(void)
{
    fake_i2c f;
    gt_i2c_bus bus = fake_i2c_bus(&f);
    f.fail_writes = 1;

    CHECK(gt_light_open_bus(bus) == NULL);
    CHECK_EQ(f.write_calls, 1);
    CHECK_EQ(f.closed, 1);
    CHECK_EQ(f.sleep_calls, 0);
}

/* The device ACKs some init commands, then NACKs one (e.g. a loose wire).
 * For each of the 3 commands: open stops at the failed one, sends nothing
 * after it, never waits, and closes the bus exactly once. */
static void test_open_fails_at_each_init_command(void)
{
    for (int k = 1; k <= 3; k++) {
        fake_i2c f;
        gt_i2c_bus bus = fake_i2c_bus(&f);
        f.fail_write_at = k;

        CHECK(gt_light_open_bus(bus) == NULL);
        CHECK_EQ(f.write_calls, k);
        CHECK_EQ(f.closed, 1);
        CHECK_EQ(f.sleep_calls, 0);
    }
}

/* A bus with no ops table is rejected rather than dereferenced. */
static void test_open_null_ops_fails(void)
{
    gt_i2c_bus bus = { .ops = NULL, .ctx = NULL };
    CHECK(gt_light_open_bus(bus) == NULL);
}

/* --- initialisation ----------------------------------------------------- */

/* Open sends POWER_ON, RESET, CONT_HRES, one byte each in that order, then
 * waits one worst-case conversion so the first read is real data. */
static void test_open_sends_init_sequence(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);

    CHECK(s != NULL);
    CHECK_EQ(f.write_calls, 3);
    CHECK_EQ(f.n_written, 3);
    CHECK_EQ(f.written[0], GT_BH1750_POWER_ON);
    CHECK_EQ(f.written[1], GT_BH1750_RESET);
    CHECK_EQ(f.written[2], GT_BH1750_CONT_HRES);
    CHECK_EQ(f.sleep_calls, 1);
    CHECK(f.slept_ms_total >= GT_BH1750_MEAS_MAX_MS);
    CHECK_EQ(f.closed, 0);

    gt_light_close(s);
}

/* --- reading and decoding ----------------------------------------------- */

/* Data arrives high byte first: 0x01 0x2C = 300. */
static void test_read_decodes_big_endian(void)
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

/* The full 16-bit range decodes: 0 (dark) and 65535 (saturated). */
static void test_read_range_limits(void)
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

/* Back-to-back reads return successive measurements in order. */
static void test_reads_in_order(void)
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

/* In continuous mode reads are pure reads: re-sending a mode command would
 * restart the conversion and the next read would return stale data. */
static void test_reads_send_no_commands(void)
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

/* Lux = count / 1.2 in high-res mode (datasheet). */
static void test_lux_conversion(void)
{
    CHECK(near(gt_light_raw_to_lux(0), 0.0));
    CHECK(near(gt_light_raw_to_lux(120), 100.0));
    CHECK(near(gt_light_raw_to_lux(65535), 54612.5));
}

/* --- error handling ----------------------------------------------------- */

/* A transient failure is retried after a 10 ms gap and the read succeeds. */
static void test_read_retry_recovers(void)
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

/* Every attempt fails: 1 try + 3 retries, 3 gaps, then GT_ERR_IO with
 * raw_out untouched so the caller skips the sample instead of storing junk.
 * Also the timing budget: even this worst case waits only 30 ms, well
 * inside the 1 s light period, so retries cannot make sampling miss a slot. */
static void test_read_retry_exhausted(void)
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

/* A short read (device stopped after 1 byte) is a failure, not a value. */
static void test_read_short_is_error(void)
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

/* After a read gives up (sensor briefly unplugged), the same handle works
 * again once the device answers; no reopen is needed. */
static void test_read_works_after_outage(void)
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

/* NULL arguments are rejected without touching the bus. */
static void test_null_args(void)
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

/* --- cleanup ------------------------------------------------------------ */

/* Close powers the chip down as its last command and closes the bus once. */
static void test_close_powers_down(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);

    gt_light_close(s);
    CHECK_EQ(f.n_written, 4);
    CHECK_EQ(f.written[3], GT_BH1750_POWER_DOWN);
    CHECK_EQ(f.closed, 1);
}

/* An unplugged sensor NACKs the power-down; the bus is still released. */
static void test_close_when_power_down_fails(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);
    f.fail_writes = 1;

    gt_light_close(s);
    CHECK_EQ(f.closed, 1);
}

int main(void)
{
    test_address_is_0x23();
    test_open_without_bus_device_fails();
    test_open_missing_device_fails();
    test_open_fails_at_each_init_command();
    test_open_null_ops_fails();
    test_open_sends_init_sequence();
    test_read_decodes_big_endian();
    test_read_range_limits();
    test_reads_in_order();
    test_reads_send_no_commands();
    test_lux_conversion();
    test_read_retry_recovers();
    test_read_retry_exhausted();
    test_read_short_is_error();
    test_read_works_after_outage();
    test_null_args();
    test_close_powers_down();
    test_close_when_power_down_fails();
    return CHECK_RESULT();
}
