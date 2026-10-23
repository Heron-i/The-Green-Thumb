/* Unit tests for closing the BH1750 driver (src/light_sensor.c) against a
 * fake bus: gt_light_close.
 *
 * Purpose: prove close powers the chip down and releases the bus exactly
 * once, even when the chip no longer answers. Run under make test / asan /
 * memcheck, so leaks and double-closes also fail.
 * Implements LSC-01 and LSC-02. Opening is in test_light_open.c, reading
 * in test_light_read.c. */
#include "check.h"
#include "fake_i2c.h"
#include "light_sensor.h"

/* Reset `f` and open a sensor on it; the init traffic is left in `f`. */
static gt_light_sensor *open_on(fake_i2c *f)
{
    return gt_light_open_bus(fake_i2c_bus(f));
}

/* ---- LSC-01: close powers down and closes the bus ----
 * Power-down is the last command, and the bus is closed once. */
static void test_lsc01_close_powers_down(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);

    gt_light_close(s);
    CHECK_EQ(f.n_written, 4);
    CHECK_EQ(f.written[3], GT_BH1750_POWER_DOWN);
    CHECK_EQ(f.closed, 1);
}

/* ---- LSC-02: the bus is released when power-down fails ----
 * An unplugged sensor NACKs the power-down; the bus is still released. */
static void test_lsc02_close_when_power_down_fails(void)
{
    fake_i2c f;
    gt_light_sensor *s = open_on(&f);
    f.fail_writes = 1;

    gt_light_close(s);
    CHECK_EQ(f.closed, 1);
}

static const test_case_t tests[] = {
    {"LSC-01", "close powers down and closes the bus", test_lsc01_close_powers_down},
    {"LSC-02", "bus is released when power-down fails",
     test_lsc02_close_when_power_down_fails},
};

int main(void)
{
    return RUN_TESTS(tests);
}
