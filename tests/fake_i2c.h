#ifndef FAKE_I2C_H
#define FAKE_I2C_H

/* Fake gt_i2c_ops for driver tests: no /dev/i2c-1, no real sleeping.
 *
 * A test declares a fake_i2c on the stack, scripts read data and failures,
 * passes fake_i2c_bus(&f) to the driver, then inspects what the driver did:
 * bytes written, read attempts, waits and closes. The fake is the bus ctx
 * and is owned by the test, so close() only counts and never frees.
 * fake_i2c_bus() zeroes the fake, so call it before scripting anything.
 *
 * Header-only (static inline) because the Makefile links each test .c with
 * src/ only; inline keeps unused helpers from tripping -Werror. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "i2c_bus.h"

#define FAKE_I2C_MAX_WRITTEN 64
#define FAKE_I2C_MAX_READ    64

typedef struct {
    /* Every attempted write, in order (failed ones too: the bus was driven). */
    uint8_t  written[FAKE_I2C_MAX_WRITTEN];
    size_t   n_written;
    int      write_calls;

    /* Scripted read data, consumed front to back across read() calls. */
    uint8_t  read_data[FAKE_I2C_MAX_READ];
    size_t   n_read_data;
    size_t   read_pos;
    int      read_calls;

    /* Failure injection: the next N calls of each kind return GT_ERR_IO. */
    int      fail_writes;
    int      fail_reads;

    int      sleep_calls;
    uint32_t slept_ms_total;
    int      closed;
} fake_i2c;

/* Append `len` bytes to the data later reads will return. */
static inline void fake_i2c_push_read(fake_i2c *f, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len && f->n_read_data < FAKE_I2C_MAX_READ; i++) {
        f->read_data[f->n_read_data++] = buf[i];
    }
}

static inline gt_status fake_i2c_write(void *ctx, const uint8_t *buf, size_t len)
{
    fake_i2c *f = ctx;
    f->write_calls++;
    for (size_t i = 0; i < len && f->n_written < FAKE_I2C_MAX_WRITTEN; i++) {
        f->written[f->n_written++] = buf[i];
    }
    if (f->fail_writes > 0) {
        f->fail_writes--;
        return GT_ERR_IO;
    }
    return GT_OK;
}

/* A failed read consumes no data. Running out of scripted data is a short
 * read, which the real bus also reports as GT_ERR_IO. */
static inline gt_status fake_i2c_read(void *ctx, uint8_t *buf, size_t len)
{
    fake_i2c *f = ctx;
    f->read_calls++;
    if (f->fail_reads > 0) {
        f->fail_reads--;
        return GT_ERR_IO;
    }
    if (f->n_read_data - f->read_pos < len) {
        return GT_ERR_IO;
    }
    memcpy(buf, &f->read_data[f->read_pos], len);
    f->read_pos += len;
    return GT_OK;
}

static inline void fake_i2c_sleep_ms(void *ctx, uint32_t ms)
{
    fake_i2c *f = ctx;
    f->sleep_calls++;
    f->slept_ms_total += ms;
}

static inline void fake_i2c_close(void *ctx)
{
    fake_i2c *f = ctx;
    f->closed++;
}

/* Reset `f` and return a bus that routes every op to it. */
static inline gt_i2c_bus fake_i2c_bus(fake_i2c *f)
{
    static const gt_i2c_ops ops = {
        .write    = fake_i2c_write,
        .read     = fake_i2c_read,
        .sleep_ms = fake_i2c_sleep_ms,
        .close    = fake_i2c_close,
    };
    memset(f, 0, sizeof *f);
    return (gt_i2c_bus){ .ops = &ops, .ctx = f };
}

#endif
