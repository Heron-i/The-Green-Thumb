/* The soil_sensor process's loop (see soil_loop.h).
 *
 * Purpose: turns the soil driver, the health tracker and the IPC client
 * into one schedule. Each tick it (1) opens the sensor if it isn't open
 * yet, (2) probes an offline sensor, (3) takes the reading that is due,
 * and (4) pumps the client. It makes no system calls itself: the bus comes
 * from cfg.bus_open, the hub is the client's socket ops, and every event
 * goes to cfg.log, so tests drive all of it with fakes and a made-up clock.
 *
 * Failure handling (DESIGN.md failure-mode table, Sensor rows):
 *   - a failed read (the driver already retried) logs i2c_read_error and
 *     sends nothing; the 10th in a row logs sensor_offline;
 *   - while offline, scheduled readings are skipped and the sensor is
 *     probed with gt_soil_reinit every 5 s (GT_HEALTH_RETRY_MS), not every
 *     60 s, so a replug comes back within ~5 s: sensor_reinit, then
 *     sensor_online;
 *   - a sensor that can't be opened at startup logs i2c_open_error and
 *     sensor_offline, and the open is retried every 5 s instead of exiting;
 *   - a tick that finds whole periods have passed logs sample_overrun and
 *     skips those slots instead of reading in a burst. */
#include "soil_loop.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "ipc_sock.h"
#include "sensor_health.h"
#include "soil_sensor.h"

struct gt_soil_loop {
    gt_soil_loop_cfg cfg;
    gt_ipc_client   *client;
    gt_soil_sensor  *sensor;        /* NULL until an open succeeds */
    gt_health        health;        /* read failures of the open sensor */
    uint64_t         next_due_ms;   /* when the next scheduled reading is due */
    uint64_t         next_open_ms;  /* when to retry a failed open */
    int              open_failed;   /* sensor_offline was logged for a failed open */
    uint32_t         counter;       /* sensor-side counter of the next reading */
};

/* Pass one event to the caller's log callback; NULL fields become "". */
static void log_event(const gt_soil_loop *loop, gt_log_level level, const char *event,
                      const char *fields)
{
    loop->cfg.log(loop->cfg.log_ctx, level, event, fields != NULL ? fields : "");
}

/* Log a health transition, if there was one (sensor_offline / sensor_online). */
static void log_health_event(const gt_soil_loop *loop, gt_health_event health_event)
{
    const char *name = gt_health_event_name(health_event);
    if (name != NULL) {
        log_event(loop, health_event == GT_HEALTH_EV_OFFLINE ? GT_LOG_WARN : GT_LOG_INFO,
                  name, NULL);
    }
}

/* Log the IPC client's event flags (see ipc.h). `events` is a bit set, so
 * more than one can be logged from the same call. */
static void log_ipc_events(const gt_soil_loop *loop, unsigned events)
{
    if (events & GT_IPC_EV_DISCONNECT) {
        log_event(loop, GT_LOG_WARN, "ipc_disconnect", NULL);
    }
    if (events & GT_IPC_EV_RECONNECT) {
        log_event(loop, GT_LOG_INFO, "ipc_reconnect", NULL);
    }
    if (events & GT_IPC_EV_OVERFLOW) {
        gt_ipc_stats stats = { 0 };
        char fields[32];
        gt_ipc_client_stats(loop->client, &stats);
        snprintf(fields, sizeof fields, "refused=%" PRIu32, stats.refused);
        log_event(loop, GT_LOG_WARN, "ipc_overflow", fields);
    }
}

/* Create the loop; it owns `client` from here on (see soil_loop.h). */
gt_soil_loop *gt_soil_loop_create(const gt_soil_loop_cfg *cfg, gt_ipc_client *client,
                                  uint64_t now_ms)
{
    if (client == NULL) {
        return NULL;
    }
    if (cfg == NULL || cfg->period_ms == 0 || cfg->bus_open == NULL || cfg->log == NULL) {
        gt_ipc_client_close(client);
        return NULL;
    }

    gt_soil_loop *loop = calloc(1, sizeof *loop);
    if (loop == NULL) {
        gt_ipc_client_close(client);
        return NULL;
    }
    loop->cfg = *cfg;
    loop->client = client;
    loop->next_due_ms = now_ms;
    loop->next_open_ms = now_ms;
    gt_health_init(&loop->health);
    return loop;
}

/* Open the sensor. On failure, log i2c_open_error (and sensor_offline the
 * first time) and retry GT_HEALTH_RETRY_MS later. On success after a
 * failure, log sensor_online. The open runs the full init, so no separate
 * reinit is needed. */
static void try_open(gt_soil_loop *loop, uint64_t now_ms)
{
    gt_i2c_bus bus;
    gt_soil_sensor *sensor = NULL;
    if (loop->cfg.bus_open(loop->cfg.bus_ctx, &bus) == GT_OK) {
        sensor = gt_soil_open_bus(bus);   /* closes the bus itself on failure */
    }

    if (sensor == NULL) {
        log_event(loop, GT_LOG_WARN, "i2c_open_error", NULL);
        if (!loop->open_failed) {
            log_event(loop, GT_LOG_WARN, "sensor_offline", NULL);
            loop->open_failed = 1;
        }
        loop->next_open_ms = now_ms + GT_HEALTH_RETRY_MS;
        return;
    }

    loop->sensor = sensor;
    gt_health_init(&loop->health);
    if (loop->open_failed) {
        log_event(loop, GT_LOG_INFO, "sensor_online", NULL);
        loop->open_failed = 0;
    }
}

/* Probe an offline sensor by re-running its init: a replugged or
 * browned-out board needs that anyway before its readings can be trusted.
 * The health tracker turns a good probe into sensor_online. */
static void probe_offline(gt_soil_loop *loop, uint64_t now_ms)
{
    gt_status status = gt_soil_reinit(loop->sensor);
    if (status == GT_OK) {
        log_event(loop, GT_LOG_INFO, "sensor_reinit", NULL);
    } else {
        log_event(loop, GT_LOG_WARN, GT_HEALTH_I2C_READ_ERROR, NULL);
    }
    log_health_event(loop, gt_health_record(&loop->health, status, now_ms));
}

/* Read the sensor and send the reading as {DATA, SOIL, counter, raw}. A
 * failed read logs i2c_read_error and uses no counter, so the hub sees no
 * gap for a reading that never existed. A reading the client refuses
 * (ring full) still uses its counter: the client's GAP covers it. */
static void take_reading(gt_soil_loop *loop, uint64_t now_ms)
{
    uint32_t raw = 0;
    gt_status status = gt_soil_read(loop->sensor, &raw);
    if (status != GT_OK) {
        log_event(loop, GT_LOG_WARN, GT_HEALTH_I2C_READ_ERROR, NULL);
    }
    log_health_event(loop, gt_health_record(&loop->health, status, now_ms));
    if (status != GT_OK) {
        return;
    }

    gt_msg message = {
        .type = GT_MSG_DATA, .sensor_id = GT_SENSOR_SOIL,
        .counter = loop->counter, .seq = 0, .value = raw,
    };
    unsigned events = 0;
    gt_status sent = gt_ipc_client_send(loop->client, &message, now_ms, &events);
    if (sent == GT_OK || sent == GT_ERR_FULL) {
        loop->counter++;
    }
    log_ipc_events(loop, events);
}

/* If a reading is due, advance the schedule by one period (after skipping
 * any whole periods already missed) and take it, unless the sensor is
 * closed or offline. The schedule stays anchored to the create time, so a
 * late tick never shifts later readings. */
static void sample_if_due(gt_soil_loop *loop, uint64_t now_ms)
{
    if (now_ms < loop->next_due_ms) {
        return;
    }
    uint64_t period_ms = loop->cfg.period_ms;
    uint64_t late_ms = now_ms - loop->next_due_ms;
    if (late_ms >= period_ms) {
        uint64_t skipped = late_ms / period_ms;
        char fields[32];
        snprintf(fields, sizeof fields, "skipped=%" PRIu64, skipped);
        log_event(loop, GT_LOG_WARN, "sample_overrun", fields);
        loop->next_due_ms += skipped * period_ms;
    }
    loop->next_due_ms += period_ms;

    if (loop->sensor != NULL && loop->health.state == GT_HEALTH_ONLINE) {
        take_reading(loop, now_ms);
    }
}

/* One pass of the loop (see soil_loop.h for the order). */
gt_status gt_soil_loop_tick(gt_soil_loop *loop, uint64_t now_ms)
{
    if (loop == NULL) {
        return GT_ERR_INVAL;
    }

    if (loop->sensor == NULL) {
        if (now_ms >= loop->next_open_ms) {
            try_open(loop, now_ms);
        }
    } else if (loop->health.state == GT_HEALTH_OFFLINE &&
               gt_health_should_read(&loop->health, now_ms)) {
        probe_offline(loop, now_ms);
    }

    sample_if_due(loop, now_ms);

    unsigned events = 0;
    gt_ipc_client_pump(loop->client, now_ms, &events);
    log_ipc_events(loop, events);
    return GT_OK;
}

/* Close the sensor and the client, then free the loop. NULL-safe. */
void gt_soil_loop_destroy(gt_soil_loop *loop)
{
    if (loop == NULL) {
        return;
    }
    gt_soil_close(loop->sensor);
    gt_ipc_client_close(loop->client);
    free(loop);
}
