#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"

/* Hardware seam: drivers talk to the bus only through these ops, so tests
 * can swap in a fake device instead of /dev/i2c-1. */
typedef struct {
    gt_status (*write)(void *ctx, const uint8_t *buf, size_t len);
    gt_status (*read)(void *ctx, uint8_t *buf, size_t len);   /* short read => GT_ERR_IO */
    void      (*sleep_ms)(void *ctx, uint32_t ms);             /* injectable so tests don't sleep */
    void      (*close)(void *ctx);
} gt_i2c_ops;

typedef struct {
    const gt_i2c_ops *ops;
    void             *ctx;
} gt_i2c_bus;

/* Linux impl: open(dev, O_RDWR) + ioctl(I2C_SLAVE, addr). dev: e.g. "/dev/i2c-1" */
gt_status gt_i2c_linux_open(const char *dev, uint8_t addr, gt_i2c_bus *out);

#endif
