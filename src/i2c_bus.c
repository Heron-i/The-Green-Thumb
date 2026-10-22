/* Linux /dev/i2c-N implementation of the gt_i2c_ops seam (see i2c_bus.h).
 *
 * Purpose: this file is the only place that makes I2C system calls. It moves
 * raw bytes between the Pi and one device address and knows nothing about any
 * particular sensor; protocol logic (BH1750 commands, decoding, retry policy)
 * lives in the drivers (light_sensor.c, soil_sensor.c).
 *
 * Why it matters: drivers call only bus.ops->read/write/sleep_ms/close, so
 * tests can substitute a fake bus and exercise every driver path, including
 * injected I/O failures, in WSL/CI with no /dev/i2c-1. On the Pi, this file
 * is the real path to the hardware.
 *
 * Linux model: the kernel's i2c-bcm2835 driver exposes the bus as
 * /dev/i2c-1. open() it, ioctl(I2C_SLAVE, addr) to pick the device, then
 * each write()/read() is one complete I2C transaction (START .. STOP).
 * A device that does not ACK makes the call fail with -1 (EIO/ENXIO). */
#include "i2c_bus.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/i2c-dev.h>

/* Per-bus state, passed to every op as the opaque `ctx`. Each opened sensor
 * gets its own fd bound to its own address; the ops table is shared. */
typedef struct {
    int fd;
} linux_i2c;

/* Send `len` bytes to the device in one I2C write transaction.
 * Used by drivers for commands (e.g. BH1750 power-on / mode select).
 * Retries only on EINTR (a signal interrupted the call before any transfer);
 * any real bus error or partial transfer is GT_ERR_IO and the driver decides
 * whether to retry. */
static gt_status linux_write(void *ctx, const uint8_t *buf, size_t len)
{
    linux_i2c *b = ctx;
    ssize_t n;
    do {
        n = write(b->fd, buf, len);
    } while (n < 0 && errno == EINTR);
    /* A short write means the device NACKed partway: treat as failure. */
    return (n == (ssize_t)len) ? GT_OK : GT_ERR_IO;
}

/* Read exactly `len` bytes from the device in one I2C read transaction.
 * Used by drivers to fetch measurements (BH1750: 2 bytes, high byte first).
 * Same EINTR handling as linux_write; a short read is GT_ERR_IO. Decoding
 * and retry policy stay in the driver. */
static gt_status linux_read(void *ctx, uint8_t *buf, size_t len)
{
    linux_i2c *b = ctx;
    ssize_t n;
    do {
        n = read(b->fd, buf, len);
    } while (n < 0 && errno == EINTR);
    return (n == (ssize_t)len) ? GT_OK : GT_ERR_IO;
}

/* Block for `ms` milliseconds. Drivers wait through the bus (BH1750 settle
 * after mode select, gap between read retries) so a test fake can record the
 * wait instead of sleeping. nanosleep is used because usleep is not declared
 * under _POSIX_C_SOURCE=200809L; on EINTR it resumes with the time left. */
static void linux_sleep_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    struct timespec req = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    /* Resume with the remaining time if a signal interrupts the sleep. */
    while (nanosleep(&req, &req) < 0 && errno == EINTR) {
    }
}

/* Release the bus: close the fd and free the per-bus state. Called by the
 * driver's close, or by its open if device initialisation fails. NULL-safe. */
static void linux_close(void *ctx)
{
    linux_i2c *b = ctx;
    if (b == NULL) {
        return;
    }
    close(b->fd);
    free(b);
}

/* The real implementation of the seam. The functions above are static, so
 * the rest of the program can reach them only through this table. */
static const gt_i2c_ops linux_ops = {
    .write    = linux_write,
    .read     = linux_read,
    .sleep_ms = linux_sleep_ms,
    .close    = linux_close,
};

/* Open the I2C bus at `dev` (e.g. "/dev/i2c-1") and bind it to the 7-bit
 * device address `addr` (0x23 for the BH1750). On success fills `out` with
 * the Linux ops and a new context; the caller owns it and must call
 * out->ops->close(out->ctx). Note this does not prove the device is present:
 * ioctl(I2C_SLAVE) succeeds for an empty address, and the first missing ACK
 * shows up on the first read/write.
 * Returns GT_ERR_INVAL (NULL args), GT_ERR_NOMEM, or GT_ERR_IO (cannot open
 * dev, e.g. I2C disabled or no permission, or address rejected). Every
 * failure path undoes what it already did, so nothing leaks. */
gt_status gt_i2c_linux_open(const char *dev, uint8_t addr, gt_i2c_bus *out)
{
    if (dev == NULL || out == NULL) {
        return GT_ERR_INVAL;
    }

    linux_i2c *b = malloc(sizeof *b);
    if (b == NULL) {
        return GT_ERR_NOMEM;
    }

    b->fd = open(dev, O_RDWR);
    if (b->fd < 0) {
        free(b);
        return GT_ERR_IO;
    }

    /* Bind this fd to one 7-bit slave address; later read/write go to it. */
    if (ioctl(b->fd, I2C_SLAVE, (unsigned long)addr) < 0) {
        close(b->fd);
        free(b);
        return GT_ERR_IO;
    }

    out->ops = &linux_ops;
    out->ctx = b;
    return GT_OK;
}
