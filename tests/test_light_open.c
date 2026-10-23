/* Unit tests for opening the BH1750 driver (src/light_sensor.c) against a
 * fake bus: gt_light_open / gt_light_open_bus.
 *
 * Purpose: prove the driver detects a missing or broken device and leaves
 * nothing open when it gives up, and that a successful open sends the init
 * command sequence and waits for the first conversion. Run under
 * make test / asan / memcheck, so leaks and double-closes also fail.
 * Implements LSO-01 through LSO-06. Reading is in test_light_read.c,
 * closing in test_light_close.c.
 *
 * What these tests cannot prove: that a real chip sits at 0x23 on the wire.
 * That needs the Pi (i2cdetect / the light_probe tool). */
#include "check.h"
#include "fake_i2c.h"
#include "light_sensor.h"

/* Reset `f` and open a sensor on it; the init traffic is left in `f`. */
static gt_light_sensor *open_on(fake_i2c *f)
{
    return gt_light_open_bus(fake_i2c_bus(f));
}

/* ---- LSO-01: I2C address is 0x23 ----
 * The address is fixed by the ADDR pin wiring (low/floating = 0x23); pin it
 * so a typo can't silently point the driver at another device. */
static void test_lso01_address_is_0x23(void)
{
    CHECK_EQ(GT_LIGHT_I2C_ADDR, 0x23);
}

/* ---- LSO-02: open without a bus device fails ----
 * No bus device at all (I2C disabled, wrong path): this runs the real
 * gt_i2c_linux_open, whose open() fails, so open must return NULL. */
static void test_lso02_open_without_bus_device_fails(void)
{
    CHECK(gt_light_open("/nonexistent/i2c-1") == NULL);
    CHECK(gt_light_open(NULL) == NULL);
}

/* ---- LSO-03: open fails when the device is missing ----
 * Nothing answers at the address: the first write (POWER_ON) is NACKed.
 * Open must fail, close the bus it was given exactly once, and not wait. */
static void test_lso03_open_missing_device_fails(void)
{
    fake_i2c f;
    gt_i2c_bus bus = fake_i2c_bus(&f);
    f.fail_writes = 1;

    CHECK(gt_light_open_bus(bus) == NULL);
    CHECK_EQ(f.write_calls, 1);
    CHECK_EQ(f.closed, 1);
    CHECK_EQ(f.sleep_calls, 0);
}

/* ---- LSO-04: open fails cleanly at each init command ----
 * The device ACKs some init commands, then NACKs one (e.g. a loose wire).
 * For each of the 3 commands: open stops at the failed one, sends nothing
 * after it, never waits, and closes the bus exactly once. */
static void test_lso04_open_fails_at_each_init_command(void)
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

/* ---- LSO-05: a bus with no ops table is rejected ----
 * Rejected rather than dereferenced. */
static void test_lso05_open_null_ops_fails(void)
{
    gt_i2c_bus bus = { .ops = NULL, .ctx = NULL };
    CHECK(gt_light_open_bus(bus) == NULL);
}

/* ---- LSO-06: open sends the init sequence ----
 * Open sends POWER_ON, RESET, CONT_HRES, one byte each in that order, then
 * waits one worst-case conversion so the first read is real data. */
static void test_lso06_open_sends_init_sequence(void)
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

static const test_case_t tests[] = {
    {"LSO-01", "I2C address is 0x23", test_lso01_address_is_0x23},
    {"LSO-02", "open without a bus device fails", test_lso02_open_without_bus_device_fails},
    {"LSO-03", "open fails when the device is missing", test_lso03_open_missing_device_fails},
    {"LSO-04", "open fails cleanly at each init command",
     test_lso04_open_fails_at_each_init_command},
    {"LSO-05", "a bus with no ops table is rejected", test_lso05_open_null_ops_fails},
    {"LSO-06", "open sends the init sequence", test_lso06_open_sends_init_sequence},
};

int main(void)
{
    return RUN_TESTS(tests);
}
