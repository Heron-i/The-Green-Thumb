/* Adafruit STEMMA capacitive soil sensor driver (see soil_sensor.h).
 *
 * Purpose: speaks the seesaw protocol over the gt_i2c_ops seam: software
 * reset and hardware-ID check at open, a 2-byte moisture read, and the read
 * retry policy from DESIGN.md's failure-mode table ("Retry 3x at 10 ms, then
 * skip"; the seesaw's 0xFFFF error value counts as a failed read).
 * It makes no system calls itself; all bus traffic and waits go through
 * bus.ops, so tests can run every path against a fake bus.
 *
 * Device model: the board is a SAMD09 microcontroller running Adafruit's
 * seesaw firmware. Every access writes a 2-byte register address
 * {base, function}, waits for the firmware to fetch the value, then reads
 * it. Moisture is the capacitive touch channel 0 (TOUCH_BASE 0x0F,
 * offset 0x10): a raw count of roughly 200 (dry/air) to 2000 (wet), high
 * byte first. The driver returns the raw count; calibration happens
 * downstream.
 *
 * Not here: offline/online tracking after repeated failures. That belongs to
 * the soil_sensor process, which sees the run of consecutive errors. */
#include "soil_sensor.h"

#include <stdlib.h>

struct gt_soil_sensor {
    gt_i2c_bus bus;
};

/* Select register {base, function} so the next read returns its value. */
static gt_status select_reg(gt_soil_sensor *s, uint8_t base, uint8_t function)
{
    const uint8_t reg[2] = { base, function };
    return s->bus.ops->write(s->bus.ctx, reg, sizeof reg);
}

/* Open the seesaw on the Linux bus `i2c_dev` at GT_SOIL_I2C_ADDR.
 * Returns NULL if the bus cannot be opened or the device does not respond. */
gt_soil_sensor *gt_soil_open(const char *i2c_dev)
{
    gt_i2c_bus bus;
    if (gt_i2c_linux_open(i2c_dev, GT_SOIL_I2C_ADDR, &bus) != GT_OK) {
        return NULL;
    }
    return gt_soil_open_bus(bus);
}

/* Initialise the device on an already-open bus: software reset, wait for the
 * firmware to reboot, then read the hardware ID and require 0x55. The reset
 * write is the first point where a missing device shows up (no ACK ->
 * GT_ERR_IO); the ID check catches some other chip answering at 0x36.
 * Takes ownership of `bus`: on any failure the bus is closed and NULL is
 * returned, so the caller never has to clean up. */
gt_soil_sensor *gt_soil_open_bus(gt_i2c_bus bus)
{
    if (bus.ops == NULL) {
        return NULL;
    }

    gt_soil_sensor *s = malloc(sizeof *s);
    if (s == NULL) {
        bus.ops->close(bus.ctx);
        return NULL;
    }
    s->bus = bus;

    const uint8_t reset[3] = { GT_SEESAW_STATUS_BASE, GT_SEESAW_STATUS_SWRST, 0xFF };
    uint8_t hw_id = 0;
    gt_status status = bus.ops->write(bus.ctx, reset, sizeof reset);
    if (status == GT_OK) {
        bus.ops->sleep_ms(bus.ctx, GT_SOIL_RESET_MS);
        status = select_reg(s, GT_SEESAW_STATUS_BASE, GT_SEESAW_STATUS_HW_ID);
    }
    if (status == GT_OK) {
        bus.ops->sleep_ms(bus.ctx, GT_SOIL_CONV_MS);
        status = bus.ops->read(bus.ctx, &hw_id, 1);
    }
    if (status != GT_OK || hw_id != GT_SEESAW_HW_ID_CODE) {
        bus.ops->close(bus.ctx);
        free(s);
        return NULL;
    }
    return s;
}

/* Read the moisture channel as a raw 16-bit count into `raw_out`.
 * Each attempt selects the register, waits GT_SOIL_CONV_MS, and reads 2
 * bytes; a NACK, a short read, or the 0xFFFF error value is a failed
 * attempt. Up to GT_SOIL_RETRIES retries, GT_SOIL_RETRY_GAP_MS apart; if
 * every attempt fails, returns GT_ERR_IO and leaves `raw_out` untouched so
 * the caller can skip the sample. GT_ERR_INVAL on NULL args. */
gt_status gt_soil_read(gt_soil_sensor *s, uint32_t *raw_out)
{
    if (s == NULL || raw_out == NULL) {
        return GT_ERR_INVAL;
    }

    uint8_t data[2];
    /* Invariant: every earlier attempt failed and raw_out is untouched. */
    for (int attempt = 0; attempt <= GT_SOIL_RETRIES; attempt++) {
        if (attempt > 0) {
            s->bus.ops->sleep_ms(s->bus.ctx, GT_SOIL_RETRY_GAP_MS);
        }
        if (select_reg(s, GT_SEESAW_TOUCH_BASE, GT_SEESAW_TOUCH_CH0) != GT_OK) {
            continue;
        }
        s->bus.ops->sleep_ms(s->bus.ctx, GT_SOIL_CONV_MS);
        if (s->bus.ops->read(s->bus.ctx, data, sizeof data) != GT_OK) {
            continue;
        }
        uint32_t raw = ((uint32_t)data[0] << 8) | data[1];
        if (raw != GT_SOIL_ERR_RAW) {
            *raw_out = raw;
            return GT_OK;
        }
    }
    return GT_ERR_IO;
}

/* Release the bus and the driver state. The seesaw has no power-down
 * command, so nothing is sent; an unplugged sensor still closes cleanly.
 * NULL-safe. */
void gt_soil_close(gt_soil_sensor *s)
{
    if (s == NULL) {
        return;
    }
    s->bus.ops->close(s->bus.ctx);
    free(s);
}
