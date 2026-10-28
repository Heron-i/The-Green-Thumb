/* Unit tests for closing the STEMMA seesaw soil driver (src/soil_sensor.c)
 * against a fake bus: gt_soil_close.
 *
 * Purpose: prove close releases the bus exactly once and is NULL-safe. The
 * seesaw has no power-down command, so close sends nothing. Run under
 * make test / asan / memcheck, so leaks and double-closes also fail.
 * Implements SSC-01 and SSC-02. Opening is in test_soil_open.c, reading
 * in test_soil_read.c. */
#include <stddef.h>

#include "check.h"
#include "fake_i2c.h"
#include "soil_sensor.h"

/* Reset `f`, script the seesaw's ID byte, and open a sensor on it; the init
 * traffic is left in `f`. */
static gt_soil_sensor *open_on(fake_i2c *f)
{
    gt_i2c_bus bus = fake_i2c_bus(f);
    const uint8_t hw_id[] = { GT_SEESAW_HW_ID_CODE };
    fake_i2c_push_read(f, hw_id, sizeof hw_id);
    return gt_soil_open_bus(bus);
}

/* ---- SSC-01: close releases the bus once and sends nothing ---- */
static void test_ssc01_close_releases_bus(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    CHECK(s != NULL);
    int writes_after_open = f.write_calls;

    gt_soil_close(s);
    CHECK_EQ(f.write_calls, writes_after_open);
    CHECK_EQ(f.closed, 1);
}

/* ---- SSC-02: close(NULL) is safe ---- */
static void test_ssc02_close_null_is_safe(void)
{
    gt_soil_close(NULL);
    CHECK(1);
}

static const test_case_t tests[] = {
    {"SSC-01", "close releases the bus once, sends nothing",
     test_ssc01_close_releases_bus},
    {"SSC-02", "close(NULL) is safe", test_ssc02_close_null_is_safe},
};

int main(void)
{
    return RUN_TESTS(tests);
}
