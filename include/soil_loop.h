#ifndef SOIL_LOOP_H
#define SOIL_LOOP_H

#include <stdint.h>
#include "gt_log.h"
#include "i2c_bus.h"
#include "ipc.h"

/* The soil_sensor process's work, minus the process: open the seesaw,
 * read it on a fixed schedule, send each reading to the hub, and handle
 * the DESIGN.md failure-mode rows for a sensor (read errors, offline and
 * online, re-init after a replug, open failing at startup, overruns).
 *
 * Pure logic, so it runs in tests with no Pi and no hub: the caller passes
 * a monotonic time in ms to every tick, and the loop reaches the outside
 * only through three seams. The bus opener gives it an I2C bus (the real
 * one, or a fake_i2c), the IPC client sends to the hub (the real socket,
 * or a fake_sock), and the log callback receives each event (main formats
 * and prints it; tests capture it). src/soil_sensor_main.c is the thin
 * process around it. */

#define GT_SOIL_PERIOD_MS 60000u   /* DESIGN.md: 1 soil reading per 60 s */

/* Open an I2C bus to the seesaw's address. The real one is
 * gt_i2c_linux_open(dev, GT_SOIL_I2C_ADDR, bus_out). GT_OK or GT_ERR_IO.
 * `typedef ... (*name)(...)` names a function-pointer type: a variable of
 * this type holds the address of a function with this signature, so the
 * loop can call whichever function the caller chose. */
typedef gt_status (*gt_soil_bus_open_fn)(void *ctx, gt_i2c_bus *bus_out);

/* Receive one log event. `event` is a DESIGN.md name (e.g. "sensor_offline");
 * `fields` is "key=value ..." or "" (never NULL). Both point at memory the
 * loop owns, valid only during the call. */
typedef void (*gt_soil_log_fn)(void *ctx, gt_log_level level, const char *event,
                               const char *fields);

typedef struct {
    uint32_t            period_ms;   /* GT_SOIL_PERIOD_MS in production; > 0 */
    gt_soil_bus_open_fn bus_open;
    void               *bus_ctx;
    gt_soil_log_fn      log;
    void               *log_ctx;
} gt_soil_loop_cfg;

typedef struct gt_soil_loop gt_soil_loop;

/* Create a loop whose first reading is due at `now_ms`. Does no I/O: the
 * first tick opens the sensor. Takes ownership of `client` (closed by
 * gt_soil_loop_destroy, or here on failure). NULL if cfg or client is
 * NULL, period_ms is 0, bus_open or log is NULL, or there's no memory. */
gt_soil_loop *gt_soil_loop_create(const gt_soil_loop_cfg *cfg, gt_ipc_client *client,
                                  uint64_t now_ms);
/* One pass, meant to be called every ~100 ms. In order:
 *   - sensor not open: try to open it (at once, then every 5 s);
 *   - offline: probe with gt_soil_reinit every 5 s;
 *   - a reading due: read it and send it as {DATA, SOIL, counter, raw};
 *   - pump the client, so buffered readings drain even with no new one.
 * GT_OK, or GT_ERR_INVAL for NULL. Failures are logged, never returned:
 * the process keeps running. */
gt_status     gt_soil_loop_tick(gt_soil_loop *loop, uint64_t now_ms);
/* Close the sensor (if open) and the client, and free the loop. NULL-safe. */
void          gt_soil_loop_destroy(gt_soil_loop *loop);

#endif
