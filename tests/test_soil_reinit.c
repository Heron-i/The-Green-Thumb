/* Unit tests for re-initialising the STEMMA seesaw soil driver
 * (src/soil_sensor.c) against a fake bus: gt_soil_reinit.
 *
 * Purpose: prove reinit re-sends the open-time init sequence and re-checks
 * the hardware ID (DESIGN.md failure-mode table, "Sensor replugged or
 * browned out" row: re-run init before marking online), and that a failed
 * reinit leaves the sensor open so the soil process can probe again later.
 * Run under make test / asan / memcheck, so leaks and double-closes also
 * fail. Implements SSI-01 through SSI-04. Opening is in test_soil_open.c,
 * reading in test_soil_read.c, closing in test_soil_close.c.
 *
 * What these tests cannot prove: that a real replugged seesaw comes back
 * after these steps. That needs the Pi. */
#include <stddef.h>

#include "check.h"
#include "fake_i2c.h"
#include "soil_sensor.h"

/* Traffic a successful open leaves in the fake (SSO-02): the SWRST and
 * HW_ID writes (5 bytes), the reset and conversion waits, the ID read. */
#define OPEN_WRITES  2
#define OPEN_WRITTEN 5
#define OPEN_SLEEPS  2
#define OPEN_READS   1

/* Reset `f`, script the seesaw's ID byte, and open a sensor on it; the init
 * traffic is left in `f`. */
static gt_soil_sensor *open_on(fake_i2c *f)
{
    gt_i2c_bus bus = fake_i2c_bus(f);
    const uint8_t hw_id[] = { GT_SEESAW_HW_ID_CODE };
    fake_i2c_push_read(f, hw_id, sizeof hw_id);
    return gt_soil_open_bus(bus);
}

/* ---- SSI-01: reinit re-sends the init sequence ----
 * After a good open, reinit sends SWRST {0x00 0x7F 0xFF} and the HW_ID
 * request {0x00 0x01} again, waits for the reboot, and reads the ID.
 * The bus stays open. */
static void test_ssi01_reinit_sends_init_sequence(void)
{
    fake_i2c f;
    gt_soil_sensor *sensor = open_on(&f);
    CHECK(sensor != NULL);
    const uint8_t hw_id[] = { GT_SEESAW_HW_ID_CODE };
    fake_i2c_push_read(&f, hw_id, sizeof hw_id);

    CHECK_EQ(gt_soil_reinit(sensor), GT_OK);
    const uint8_t expected[] = {
        GT_SEESAW_STATUS_BASE, GT_SEESAW_STATUS_SWRST, 0xFF,
        GT_SEESAW_STATUS_BASE, GT_SEESAW_STATUS_HW_ID,
    };
    CHECK_EQ(f.n_written, OPEN_WRITTEN + sizeof expected);
    for (size_t byte_index = 0; byte_index < sizeof expected; byte_index++) {
        CHECK_EQ(f.written[OPEN_WRITTEN + byte_index], expected[byte_index]);
    }
    CHECK_EQ(f.write_calls, OPEN_WRITES + 2);
    CHECK_EQ(f.read_calls, OPEN_READS + 1);
    CHECK(f.slept_ms_total >= 2 * GT_SOIL_RESET_MS);
    CHECK_EQ(f.closed, 0);

    gt_soil_close(sensor);
}

/* ---- SSI-02: a NACKed reinit fails but keeps the bus ----
 * For each of the 2 writes, the board NACKs that one (still unplugged):
 * reinit returns GT_ERR_IO and sends nothing after it. Unlike a failed
 * open it doesn't close the bus; the one close is the caller's. */
static void test_ssi02_reinit_nack_keeps_bus(void)
{
    /* Invariant: each pass opens and closes its own fake, so passes don't share counts. */
    for (int failing_write = 1; failing_write <= 2; failing_write++) {
        fake_i2c f;
        gt_soil_sensor *sensor = open_on(&f);
        CHECK(sensor != NULL);
        f.fail_write_at = OPEN_WRITES + failing_write;

        CHECK_EQ(gt_soil_reinit(sensor), GT_ERR_IO);
        CHECK_EQ(f.write_calls, OPEN_WRITES + failing_write);
        CHECK_EQ(f.read_calls, OPEN_READS);
        CHECK_EQ(f.closed, 0);

        gt_soil_close(sensor);
        CHECK_EQ(f.closed, 1);
    }
}

/* ---- SSI-03: a wrong ID or failed ID read keeps the bus ----
 * Something answered, but not a seesaw (or the ID read failed): GT_ERR_IO,
 * and the sensor still works once the real board answers again. */
static void test_ssi03_reinit_bad_id_keeps_bus(void)
{
    fake_i2c f;
    gt_soil_sensor *sensor = open_on(&f);
    CHECK(sensor != NULL);

    CHECK_EQ(gt_soil_reinit(sensor), GT_ERR_IO);   /* no ID byte scripted */
    const uint8_t wrong_id[] = { 0x00 };
    fake_i2c_push_read(&f, wrong_id, sizeof wrong_id);
    CHECK_EQ(gt_soil_reinit(sensor), GT_ERR_IO);
    CHECK_EQ(f.closed, 0);

    const uint8_t hw_id[] = { GT_SEESAW_HW_ID_CODE };
    fake_i2c_push_read(&f, hw_id, sizeof hw_id);
    CHECK_EQ(gt_soil_reinit(sensor), GT_OK);

    gt_soil_close(sensor);
    CHECK_EQ(f.closed, 1);
}

/* ---- SSI-04: reinit of a NULL sensor is rejected ----
 * Rejected rather than dereferenced. */
static void test_ssi04_reinit_null_rejected(void)
{
    CHECK_EQ(gt_soil_reinit(NULL), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"SSI-01", "reinit re-sends the init sequence", test_ssi01_reinit_sends_init_sequence},
    {"SSI-02", "a NACKed reinit fails but keeps the bus", test_ssi02_reinit_nack_keeps_bus},
    {"SSI-03", "a wrong ID or failed ID read keeps the bus",
     test_ssi03_reinit_bad_id_keeps_bus},
    {"SSI-04", "reinit of a NULL sensor is rejected", test_ssi04_reinit_null_rejected},
};

int main(void)
{
    return RUN_TESTS(tests);
}
