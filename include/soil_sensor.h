#ifndef SOIL_SENSOR_H
#define SOIL_SENSOR_H

#include <stdint.h>
#include "ipc.h"

#define GT_SOIL_I2C_ADDR 0x36    /* Adafruit STEMMA (seesaw) */

typedef struct gt_soil_sensor gt_soil_sensor;

/* i2c_dev: e.g. "/dev/i2c-1" */
gt_soil_sensor *gt_soil_open(const char *i2c_dev);
gt_status       gt_soil_read(gt_soil_sensor *s, uint32_t *raw_out);
void            gt_soil_close(gt_soil_sensor *s);

#endif
