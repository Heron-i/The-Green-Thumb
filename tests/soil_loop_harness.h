#ifndef SOIL_LOOP_HARNESS_H
#define SOIL_LOOP_HARNESS_H

/* Shared setup for the soil loop tests (tests/test_soil_loop_*.c): a loop
 * wired to a fake_i2c seesaw, a fake_sock hub, and a log capture.
 *
 * A test declares a soil_harness on the stack, calls harness_start, scripts
 * the seesaw (harness_push_raw, harness_fail_next_read) and the hub
 * (h.sock), ticks h.loop, then inspects h.sock.sent, h.logs and h.bus.i2c.
 * It ends with gt_soil_loop_destroy(h.loop). Everything lives in the
 * harness, which the test owns, so the fakes' close/destroy only count.
 *
 * Header-only (static inline) for the same reason as fake_i2c.h. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fake_i2c.h"
#include "fake_sock.h"
#include "ipc.h"
#include "ipc_sock.h"
#include "sensor_health.h"
#include "soil_loop.h"
#include "soil_sensor.h"

#define HARNESS_PERIOD_MS 60000u
#define HARNESS_LOG_MAX   64

/* Stands in for gt_i2c_linux_open: hands out the one fake bus, or fails
 * the next fail_opens calls as if /dev/i2c-1 couldn't be opened. */
typedef struct {
    fake_i2c   i2c;
    gt_i2c_bus bus;
    int        open_calls;
    int        fail_opens;
} fake_bus_opener;

/* Every event the loop logged, in order. */
typedef struct {
    gt_log_level level[HARNESS_LOG_MAX];
    char         event[HARNESS_LOG_MAX][32];
    char         fields[HARNESS_LOG_MAX][64];
    int          count;
} log_capture;

typedef struct {
    fake_bus_opener bus;
    fake_sock       sock;
    log_capture     logs;
    gt_soil_loop   *loop;
} soil_harness;

static inline gt_status harness_bus_open(void *ctx, gt_i2c_bus *bus_out)
{
    fake_bus_opener *opener = ctx;
    opener->open_calls++;
    if (opener->fail_opens > 0) {
        opener->fail_opens--;
        return GT_ERR_IO;
    }
    *bus_out = opener->bus;
    return GT_OK;
}

static inline void harness_log(void *ctx, gt_log_level level, const char *event,
                               const char *fields)
{
    log_capture *logs = ctx;
    if (logs->count >= HARNESS_LOG_MAX) {
        return;
    }
    logs->level[logs->count] = level;
    strncpy(logs->event[logs->count], event, sizeof logs->event[0] - 1);
    strncpy(logs->fields[logs->count], fields, sizeof logs->fields[0] - 1);
    logs->count++;
}

/* Script the seesaw's hardware-ID byte, consumed by one open or reinit. */
static inline void harness_push_hw_id(soil_harness *harness)
{
    const uint8_t hw_id[] = { GT_SEESAW_HW_ID_CODE };
    fake_i2c_push_read(&harness->bus.i2c, hw_id, sizeof hw_id);
}

/* Script one moisture reading, high byte first. */
static inline void harness_push_raw(soil_harness *harness, uint16_t raw)
{
    const uint8_t data[] = { (uint8_t)(raw >> 8), (uint8_t)(raw & 0xFF) };
    fake_i2c_push_read(&harness->bus.i2c, data, sizeof data);
}

/* Make the next gt_soil_read fail: every attempt, retries included. */
static inline void harness_fail_next_read(soil_harness *harness)
{
    harness->bus.i2c.fail_reads += 1 + GT_SOIL_RETRIES;
}

/* Zero the harness, script the ID byte the first open reads, and create a
 * loop with `period_ms` whose first reading is due at `now_ms`. */
static inline void harness_start(soil_harness *harness, uint32_t period_ms, uint64_t now_ms)
{
    memset(harness, 0, sizeof *harness);
    harness->bus.bus = fake_i2c_bus(&harness->bus.i2c);
    harness_push_hw_id(harness);
    gt_ipc_client *client = gt_ipc_client_open_ops(fake_sock_ops(&harness->sock),
                                                   &harness->sock);
    gt_soil_loop_cfg cfg = {
        .period_ms = period_ms,
        .bus_open = harness_bus_open, .bus_ctx = &harness->bus,
        .log = harness_log, .log_ctx = &harness->logs,
    };
    harness->loop = gt_soil_loop_create(&cfg, client, now_ms);
}

/* How many times `event` was logged. */
static inline int harness_count(const soil_harness *harness, const char *event)
{
    int matches = 0;
    for (int log_index = 0; log_index < harness->logs.count; log_index++) {
        matches += strcmp(harness->logs.event[log_index], event) == 0;
    }
    return matches;
}

/* Index of the first `event` logged, or -1 if it never was. */
static inline int harness_find(const soil_harness *harness, const char *event)
{
    for (int log_index = 0; log_index < harness->logs.count; log_index++) {
        if (strcmp(harness->logs.event[log_index], event) == 0) {
            return log_index;
        }
    }
    return -1;
}

/* Decode the hub's received frame number `sent_index` into *message_out.
 * GT_ERR_INVAL if there is no such frame. */
static inline gt_status harness_sent(const soil_harness *harness, size_t sent_index,
                                     gt_msg *message_out)
{
    if (sent_index >= harness->sock.n_sent) {
        return GT_ERR_INVAL;
    }
    return gt_msg_decode(harness->sock.sent[sent_index], message_out);
}

#endif
