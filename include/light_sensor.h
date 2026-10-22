#ifndef LIGHT_SENSOR_H
#define LIGHT_SENSOR_H

#include <stdint.h>
#include "ipc.h"

#define GT_LIGHT_I2C_ADDR 0x23   /* BH1750 */

typedef struct gt_light_sensor gt_light_sensor;

/* i2c_dev: e.g. "/dev/i2c-1" */
gt_light_sensor *gt_light_open(const char *i2c_dev);
gt_status        gt_light_read(gt_light_sensor *s, uint32_t *raw_out);
void             gt_light_close(gt_light_sensor *s);

#endif
