/* Sensor health tracker (see sensor_health.h).
 *
 * Purpose: implements the DESIGN.md failure-mode rule for Sensor (I2C):
 * "after 10 consecutive failures, mark offline and retry every 5 s", and
 * reports each transition once so the process logs one sensor_offline /
 * sensor_online per outage. A single failed read already includes the
 * driver's 3 retries, so 10 failures here means about 10 s of a dead sensor
 * at 1 Hz.
 *
 * Pure logic: no system calls and no clock. The caller passes each read
 * result and a monotonic time in ms, so tests can drive every path. Generic
 * across sensors; the soil process can use it too. */
#include "sensor_health.h"

#include <stddef.h>

/* Start online with no failures. NULL-safe. */
void gt_health_init(gt_health *health)
{
    if (health == NULL) {
        return;
    }
    health->state = GT_HEALTH_ONLINE;
    health->consecutive_fails = 0;
    health->next_probe_ms = 0;
}

/* Whether the caller should read the sensor now. An online sensor is read
 * whenever the sampler says a sample is due; an offline one only once its
 * next probe time arrives, so a dead sensor is probed every 5 s, not 1 Hz. */
int gt_health_should_read(const gt_health *health, uint64_t now_ms)
{
    if (health == NULL) {
        return 0;
    }
    if (health->state == GT_HEALTH_ONLINE) {
        return 1;
    }
    return now_ms >= health->next_probe_ms;
}

/* A successful read: reset the failure run, and end an outage if one is on. */
static gt_health_event record_success(gt_health *health)
{
    health->consecutive_fails = 0;
    if (health->state == GT_HEALTH_OFFLINE) {
        health->state = GT_HEALTH_ONLINE;
        return GT_HEALTH_EV_ONLINE;
    }
    return GT_HEALTH_EV_NONE;
}

/* A failed read: push the next probe while offline, else count the failure
 * and go offline on the GT_HEALTH_OFFLINE_AFTER-th in a row. */
static gt_health_event record_failure(gt_health *health, uint64_t now_ms)
{
    if (health->state == GT_HEALTH_OFFLINE) {
        health->next_probe_ms = now_ms + GT_HEALTH_RETRY_MS;
        return GT_HEALTH_EV_NONE;
    }

    health->consecutive_fails++;
    if (health->consecutive_fails >= GT_HEALTH_OFFLINE_AFTER) {
        health->state = GT_HEALTH_OFFLINE;
        health->next_probe_ms = now_ms + GT_HEALTH_RETRY_MS;
        return GT_HEALTH_EV_OFFLINE;
    }
    return GT_HEALTH_EV_NONE;
}

/* Record one read result taken at `now_ms` and return the transition, if
 * any. Any success resets the failure run (and ends an outage); the 10th
 * consecutive failure starts one. While offline, each failed probe pushes
 * the next probe GT_HEALTH_RETRY_MS past this one, measured from the probe
 * itself so a late probe never shortens the gap. */
gt_health_event gt_health_record(gt_health *health, gt_status read_result, uint64_t now_ms)
{
    if (health == NULL) {
        return GT_HEALTH_EV_NONE;
    }
    return read_result == GT_OK ? record_success(health) : record_failure(health, now_ms);
}

/* Map a transition to its DESIGN.md log event name. NONE and any stray
 * value give NULL, so the caller logs only real transitions. */
const char *gt_health_event_name(gt_health_event event)
{
    switch (event) {
    case GT_HEALTH_EV_OFFLINE: return "sensor_offline";
    case GT_HEALTH_EV_ONLINE:  return "sensor_online";
    default:                   return NULL;
    }
}
