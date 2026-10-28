#ifndef LIGHT_SENSOR_H
#define LIGHT_SENSOR_H

#include <stdint.h>
#include "i2c_bus.h"
#include "ipc.h"

#define GT_LIGHT_I2C_ADDR 0x23   /* BH1750 */

/* BH1750 opcodes and timing (datasheet). */
#define GT_BH1750_POWER_DOWN   0x00
#define GT_BH1750_POWER_ON     0x01
#define GT_BH1750_RESET        0x07   /* clears data register; only valid while powered on */
#define GT_BH1750_CONT_HRES    0x10   /* continuous high-res mode, 1 lx/count */
#define GT_BH1750_MEAS_MAX_MS  180    /* worst-case high-res conversion time */
#define GT_LIGHT_RETRIES       3      /* extra read attempts after a failure */
#define GT_LIGHT_RETRY_GAP_MS  10

typedef struct gt_light_sensor gt_light_sensor;

/* Raw counts are what get stored; convert only for display/analysis. */
static inline double gt_light_raw_to_lux(uint32_t raw) { return raw / 1.2; }

/* i2c_dev: e.g. "/dev/i2c-1" */
gt_light_sensor *gt_light_open(const char *i2c_dev);
/* Takes ownership of `bus` (closed on failure or by gt_light_close). */
gt_light_sensor *gt_light_open_bus(gt_i2c_bus bus);
gt_status        gt_light_read(gt_light_sensor *s, uint32_t *raw_out);
/* Re-run the open-time init (power-on, reset, mode select, 180 ms wait) on
 * an open sensor, for a chip that was replugged or browned out and lost its
 * mode. GT_ERR_IO if a command is NACKed; unlike open, the bus stays open,
 * so the caller can try again later. GT_ERR_INVAL for NULL. */
gt_status        gt_light_reinit(gt_light_sensor *s);
void             gt_light_close(gt_light_sensor *s);

#endif
