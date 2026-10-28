/* Unit tests for re-initialising the BH1750 driver (src/light_sensor.c)
 * against a fake bus: gt_light_reinit.
 *
 * Purpose: prove reinit re-sends the open-time init sequence and waits for
 * a fresh conversion (DESIGN.md failure-mode table, "Sensor replugged or
 * browned out" row: re-run init before marking online), and that a failed
 * reinit leaves the sensor open so the light process can probe again later.
 * Run under make test / asan / memcheck, so leaks and double-closes also
 * fail. Implements LSI-01 through LSI-03. Opening is in test_light_open.c,
 * reading in test_light_read.c, closing in test_light_close.c.
 *
 * What these tests cannot prove: that a real browned-out chip comes back
 * after these commands. That needs the Pi. */
#include "check.h"
#include "fake_i2c.h"
#include "light_sensor.h"

/* Writes and waits a successful open leaves in the fake (LSO-06). */
#define OPEN_WRITES 3
#define OPEN_SLEEPS 1

/* Reset `f` and open a sensor on it; the init traffic is left in `f`. */
static gt_light_sensor *open_on(fake_i2c *f)
{
    return gt_light_open_bus(fake_i2c_bus(f));
}

/* ---- LSI-01: reinit re-sends the init sequence ----
 * After a good open, reinit sends POWER_ON, RESET, CONT_HRES again, one
 * byte each in that order, then waits one worst-case conversion, so the
 * next read is a fresh measurement. The bus stays open. */
static void test_lsi01_reinit_sends_init_sequence(void)
{
    fake_i2c f;
    gt_light_sensor *sensor = open_on(&f);
    CHECK(sensor != NULL);

    CHECK_EQ(gt_light_reinit(sensor), GT_OK);
    CHECK_EQ(f.write_calls, OPEN_WRITES + 3);
    CHECK_EQ(f.written[OPEN_WRITES + 0], GT_BH1750_POWER_ON);
    CHECK_EQ(f.written[OPEN_WRITES + 1], GT_BH1750_RESET);
    CHECK_EQ(f.written[OPEN_WRITES + 2], GT_BH1750_CONT_HRES);
    CHECK_EQ(f.sleep_calls, OPEN_SLEEPS + 1);
    CHECK(f.slept_ms_total >= 2 * GT_BH1750_MEAS_MAX_MS);
    CHECK_EQ(f.closed, 0);

    gt_light_close(sensor);
}

/* ---- LSI-02: a NACKed reinit fails but keeps the bus ----
 * For each of the 3 commands, the chip NACKs that one (still unplugged):
 * reinit returns GT_ERR_IO, sends nothing after it and doesn't wait.
 * Unlike a failed open it doesn't close the bus: the sensor stays usable,
 * and the one close is the caller's gt_light_close. */
static void test_lsi02_reinit_nack_keeps_bus(void)
{
    /* Invariant: each pass opens and closes its own fake, so passes don't share counts. */
    for (int failing_command = 1; failing_command <= 3; failing_command++) {
        fake_i2c f;
        gt_light_sensor *sensor = open_on(&f);
        CHECK(sensor != NULL);
        f.fail_write_at = OPEN_WRITES + failing_command;

        CHECK_EQ(gt_light_reinit(sensor), GT_ERR_IO);
        CHECK_EQ(f.write_calls, OPEN_WRITES + failing_command);
        CHECK_EQ(f.sleep_calls, OPEN_SLEEPS);
        CHECK_EQ(f.closed, 0);

        gt_light_close(sensor);
        CHECK_EQ(f.closed, 1);
    }
}

/* ---- LSI-03: reinit of a NULL sensor is rejected ----
 * Rejected rather than dereferenced. */
static void test_lsi03_reinit_null_rejected(void)
{
    CHECK_EQ(gt_light_reinit(NULL), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"LSI-01", "reinit re-sends the init sequence", test_lsi01_reinit_sends_init_sequence},
    {"LSI-02", "a NACKed reinit fails but keeps the bus", test_lsi02_reinit_nack_keeps_bus},
    {"LSI-03", "reinit of a NULL sensor is rejected", test_lsi03_reinit_null_rejected},
};

int main(void)
{
    return RUN_TESTS(tests);
}
