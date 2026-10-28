#ifndef SOIL_SENSOR_H
#define SOIL_SENSOR_H

#include <stdint.h>
#include "i2c_bus.h"
#include "ipc.h"

#define GT_SOIL_I2C_ADDR 0x36    /* Adafruit STEMMA (seesaw) */

/* Seesaw registers (Adafruit seesaw protocol). Every access writes a
 * 2-byte register address {base, function}, waits, then reads. */
#define GT_SEESAW_STATUS_BASE  0x00
#define GT_SEESAW_STATUS_HW_ID 0x01   /* 1-byte chip ID */
#define GT_SEESAW_STATUS_SWRST 0x7F   /* write 0xFF to reset */
#define GT_SEESAW_HW_ID_CODE   0x55   /* SAMD09 seesaw */
#define GT_SEESAW_TOUCH_BASE   0x0F
#define GT_SEESAW_TOUCH_CH0    0x10   /* the moisture (capacitive touch) channel */

/* Timing and read policy (DESIGN.md failure-mode table). */
#define GT_SOIL_RESET_MS       500    /* seesaw boot time after SWRST */
#define GT_SOIL_CONV_MS        5      /* wait between register write and read */
#define GT_SOIL_ERR_RAW        0xFFFF /* seesaw's error value: a failed read */
#define GT_SOIL_RETRIES        3      /* extra read attempts after a failure */
#define GT_SOIL_RETRY_GAP_MS   10

typedef struct gt_soil_sensor gt_soil_sensor;

/* Raw counts (roughly 200-2000) are what get stored; calibration to relative
 * moisture happens downstream, so it can change without rewriting history. */

/* i2c_dev: e.g. "/dev/i2c-1" */
gt_soil_sensor *gt_soil_open(const char *i2c_dev);
/* Takes ownership of `bus` (closed on failure or by gt_soil_close). */
gt_soil_sensor *gt_soil_open_bus(gt_i2c_bus bus);
gt_status       gt_soil_read(gt_soil_sensor *s, uint32_t *raw_out);
/* Re-run the open-time init (software reset, 500 ms boot wait, hardware-ID
 * check) on an open sensor, for a board that was replugged or browned out.
 * GT_ERR_IO if a step is NACKed or the ID is wrong; unlike open, the bus
 * stays open, so the caller can try again later. GT_ERR_INVAL for NULL. */
gt_status       gt_soil_reinit(gt_soil_sensor *s);
void           gt_soil_close(gt_soil_sensor *s);

#endif
