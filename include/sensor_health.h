#ifndef SENSOR_HEALTH_H
#define SENSOR_HEALTH_H

#include <stdint.h>
#include "ipc.h"

/* Offline/online tracking for one sensor (DESIGN.md failure-mode table,
 * Sensor (I2C) row). Pure logic: the caller passes each read result and a
 * monotonic time in ms, and logs the returned transition. Times are
 * uint64_t because a 32-bit ms clock wraps after about 49.7 days. */
#define GT_HEALTH_OFFLINE_AFTER 10     /* consecutive failed reads -> offline */
#define GT_HEALTH_RETRY_MS      5000   /* probe period while offline */

typedef enum { GT_HEALTH_ONLINE, GT_HEALTH_OFFLINE } gt_health_state;

/* Returned by gt_health_record; the caller logs sensor_offline / sensor_online. */
typedef enum {
    GT_HEALTH_EV_NONE,
    GT_HEALTH_EV_OFFLINE,
    GT_HEALTH_EV_ONLINE
} gt_health_event;

typedef struct {
    gt_health_state state;
    uint32_t        consecutive_fails;
    uint64_t        next_probe_ms;     /* valid only while OFFLINE */
} gt_health;

void            gt_health_init(gt_health *health);
/* Online: always 1. Offline: 1 only once now_ms >= next_probe_ms. NULL -> 0. */
int             gt_health_should_read(const gt_health *health, uint64_t now_ms);
/* Feed one read result; any non-GT_OK is a failure (a failed gt_light_read
 * already includes the driver's retries). NULL -> GT_HEALTH_EV_NONE. */
gt_health_event gt_health_record(gt_health *health, gt_status read_result, uint64_t now_ms);

/* Log event names (DESIGN.md failure-mode table, Sensor (I2C) row).
 * i2c_read_error is logged on every failed read, not only on a transition. */
#define GT_HEALTH_I2C_READ_ERROR "i2c_read_error"
/* "sensor_offline" / "sensor_online"; NULL for GT_HEALTH_EV_NONE or an
 * unknown value (nothing to log). */
const char     *gt_health_event_name(gt_health_event event);

#endif
