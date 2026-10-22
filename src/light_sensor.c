/* BH1750 ambient light sensor driver (see light_sensor.h).
 *
 * Purpose: speaks the BH1750 protocol over the gt_i2c_ops seam: power-on and
 * mode select at open, a 2-byte measurement read, and the read retry policy
 * from DESIGN.md's failure-mode table ("Retry 3x at 10 ms, then skip").
 * It makes no system calls itself; all bus traffic and waits go through
 * bus.ops, so tests can run every path against a fake bus.
 *
 * Device model: after POWER_ON + CONT_HRES the chip measures continuously
 * (high-res, 1 lx/count, <= 180 ms per conversion) and each 2-byte read
 * returns the latest result, high byte first. Lux = count / 1.2. The driver
 * returns the raw count; conversion happens downstream.
 *
 * Not here: offline/online tracking after repeated failures. That belongs to
 * the light_sensor process, which sees the run of consecutive errors. */
#include "light_sensor.h"

#include <stdlib.h>

struct gt_light_sensor {
    gt_i2c_bus bus;
};

/* Send a one-byte BH1750 opcode. */
static gt_status send_cmd(gt_light_sensor *s, uint8_t op)
{
    return s->bus.ops->write(s->bus.ctx, &op, 1);
}

/* Open the BH1750 on the Linux bus `i2c_dev` at GT_LIGHT_I2C_ADDR.
 * Returns NULL if the bus cannot be opened or the device does not respond. */
gt_light_sensor *gt_light_open(const char *i2c_dev)
{
    gt_i2c_bus bus;
    if (gt_i2c_linux_open(i2c_dev, GT_LIGHT_I2C_ADDR, &bus) != GT_OK) {
        return NULL;
    }
    return gt_light_open_bus(bus);
}

/* Initialise the device on an already-open bus: power on, reset the data
 * register, start continuous high-res mode, then wait one worst-case
 * conversion so the first read returns real data. The power-on write is the
 * first point where a missing device shows up (no ACK -> GT_ERR_IO).
 * Takes ownership of `bus`: on any failure the bus is closed and NULL is
 * returned, so the caller never has to clean up. */
gt_light_sensor *gt_light_open_bus(gt_i2c_bus bus)
{
    if (bus.ops == NULL) {
        return NULL;
    }

    gt_light_sensor *s = malloc(sizeof *s);
    if (s == NULL) {
        bus.ops->close(bus.ctx);
        return NULL;
    }
    s->bus = bus;

    if (send_cmd(s, GT_BH1750_POWER_ON) != GT_OK ||
        send_cmd(s, GT_BH1750_RESET) != GT_OK ||
        send_cmd(s, GT_BH1750_CONT_HRES) != GT_OK) {
        bus.ops->close(bus.ctx);
        free(s);
        return NULL;
    }

    bus.ops->sleep_ms(bus.ctx, GT_BH1750_MEAS_MAX_MS);
    return s;
}

/* Read the latest measurement as a raw 16-bit count into `raw_out`.
 * A failed read is retried up to GT_LIGHT_RETRIES times, GT_LIGHT_RETRY_GAP_MS
 * apart; if every attempt fails, returns GT_ERR_IO and leaves `raw_out`
 * untouched so the caller can skip the sample. GT_ERR_INVAL on NULL args. */
gt_status gt_light_read(gt_light_sensor *s, uint32_t *raw_out)
{
    if (s == NULL || raw_out == NULL) {
        return GT_ERR_INVAL;
    }

    uint8_t b[2];
    for (int attempt = 0; attempt <= GT_LIGHT_RETRIES; attempt++) {
        if (attempt > 0) {
            s->bus.ops->sleep_ms(s->bus.ctx, GT_LIGHT_RETRY_GAP_MS);
        }
        if (s->bus.ops->read(s->bus.ctx, b, sizeof b) == GT_OK) {
            *raw_out = ((uint32_t)b[0] << 8) | b[1];
            return GT_OK;
        }
    }
    return GT_ERR_IO;
}

/* Power the device down (best effort: an unplugged sensor must still close
 * cleanly), then release the bus and the driver state. NULL-safe. */
void gt_light_close(gt_light_sensor *s)
{
    if (s == NULL) {
        return;
    }
    (void)send_cmd(s, GT_BH1750_POWER_DOWN);
    s->bus.ops->close(s->bus.ctx);
    free(s);
}
