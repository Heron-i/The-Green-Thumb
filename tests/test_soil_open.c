/* Unit tests for opening the STEMMA seesaw soil driver (src/soil_sensor.c)
 * against a fake bus: gt_soil_open_bus.
 *
 * Purpose: prove a successful open resets the seesaw, waits for it to boot
 * and confirms the hardware ID, and that a missing, broken or wrong device
 * is detected with nothing left open. Run under make test / asan /
 * memcheck, so leaks and double-closes also fail.
 * Implements SSO-01 through SSO-07. Reading is in test_soil_read.c,
 * closing in test_soil_close.c.
 *
 * What these tests cannot prove: that a real seesaw sits at 0x36 on the
 * wire. That needs the Pi (i2cdetect / the soil_probe tool). */
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

/* ---- SSO-01: I2C address is 0x36 ----
 * The STEMMA board's default address; pin it so a typo can't silently
 * point the driver at another device. */
static void test_sso01_address_is_0x36(void)
{
    CHECK_EQ(GT_SOIL_I2C_ADDR, 0x36);
}

/* ---- SSO-02: open resets, then checks the hardware ID ----
 * Writes SWRST {0x00 0x7F 0xFF}, then the HW_ID register {0x00 0x01},
 * reads one byte, and keeps the bus open. */
static void test_sso02_open_sends_init_sequence(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    CHECK(s != NULL);

    const uint8_t expected[] = {
        GT_SEESAW_STATUS_BASE, GT_SEESAW_STATUS_SWRST, 0xFF,
        GT_SEESAW_STATUS_BASE, GT_SEESAW_STATUS_HW_ID,
    };
    CHECK_EQ(f.n_written, sizeof expected);
    for (size_t byte_index = 0; byte_index < sizeof expected; byte_index++) {
        CHECK_EQ(f.written[byte_index], expected[byte_index]);
    }
    CHECK_EQ(f.write_calls, 2);
    CHECK_EQ(f.read_calls, 1);
    CHECK_EQ(f.closed, 0);

    gt_soil_close(s);
}

/* ---- SSO-03: open waits for the seesaw to boot ----
 * After SWRST the chip reboots; reading the ID too early gets no answer. */
static void test_sso03_open_waits_after_reset(void)
{
    fake_i2c f;
    gt_soil_sensor *s = open_on(&f);
    CHECK(s != NULL);
    CHECK(f.slept_ms_total >= GT_SOIL_RESET_MS);

    gt_soil_close(s);
}

/* ---- SSO-04: no ACK on reset gives up cleanly ----
 * Nothing at 0x36 (unplugged, unpowered): the first write is NACKed.
 * Returns NULL and closes the bus exactly once. */
static void test_sso04_reset_nack_fails(void)
{
    fake_i2c f;
    gt_i2c_bus bus = fake_i2c_bus(&f);
    f.fail_write_at = 1;

    CHECK(gt_soil_open_bus(bus) == NULL);
    CHECK_EQ(f.closed, 1);
}

/* ---- SSO-05: a failed ID write or read gives up cleanly ----
 * The device stopped answering after the reset. */
static void test_sso05_hw_id_io_error_fails(void)
{
    fake_i2c f;
    gt_i2c_bus bus = fake_i2c_bus(&f);
    f.fail_write_at = 2;
    CHECK(gt_soil_open_bus(bus) == NULL);
    CHECK_EQ(f.closed, 1);

    bus = fake_i2c_bus(&f);       /* no ID byte scripted: the read fails */
    CHECK(gt_soil_open_bus(bus) == NULL);
    CHECK_EQ(f.closed, 1);
}

/* ---- SSO-06: a wrong hardware ID is rejected ----
 * Some other chip answered at 0x36; reading it as moisture would store
 * junk, so open refuses it. */
static void test_sso06_wrong_hw_id_fails(void)
{
    fake_i2c f;
    gt_i2c_bus bus = fake_i2c_bus(&f);
    const uint8_t wrong_id[] = { 0x00 };
    fake_i2c_push_read(&f, wrong_id, sizeof wrong_id);

    CHECK(gt_soil_open_bus(bus) == NULL);
    CHECK_EQ(f.closed, 1);
}

/* ---- SSO-07: a bus with no ops is rejected ---- */
static void test_sso07_null_ops_rejected(void)
{
    gt_i2c_bus bus = { .ops = NULL, .ctx = NULL };
    CHECK(gt_soil_open_bus(bus) == NULL);
}

static const test_case_t tests[] = {
    {"SSO-01", "address is 0x36", test_sso01_address_is_0x36},
    {"SSO-02", "open resets, then checks the hardware ID",
     test_sso02_open_sends_init_sequence},
    {"SSO-03", "open waits for the seesaw to boot", test_sso03_open_waits_after_reset},
    {"SSO-04", "no ACK on reset gives up cleanly", test_sso04_reset_nack_fails},
    {"SSO-05", "failed ID write or read gives up cleanly",
     test_sso05_hw_id_io_error_fails},
    {"SSO-06", "wrong hardware ID is rejected", test_sso06_wrong_hw_id_fails},
    {"SSO-07", "bus with no ops is rejected", test_sso07_null_ops_rejected},
};

int main(void)
{
    return RUN_TESTS(tests);
}
