/* The light process's loop (see light_loop.h).
 *
 * Purpose: one tick of the light sensor process, as pure logic over the
 * driver (light_sensor.c), the health tracker (sensor_health.c) and the
 * IPC client (ipc_client.c). It makes no system calls and reads no clock:
 * time is passed in, every bus and socket call goes through those modules'
 * seams, and every event goes to the caller's log callback. That is what
 * lets tests/test_light_loop.c run it against fake_i2c and fake_sock.
 *
 * Not here: sleeping until the next due time, signals, opening the sensor
 * and the client, and writing lines to stderr. Those are
 * light_sensor_main.c's. */
#include "light_loop.h"

#include <stdio.h>

/* The BH1750's clipped value: direct sun. Still real data (DESIGN.md
 * Sensor (light) row), so it is sent and flagged with light_saturated. */
#define LIGHT_SATURATED_RAW 0xFFFFu

/* One client event flag and the log line it becomes. */
typedef struct {
    unsigned     flag;
    gt_log_level level;
    const char  *event;
} ipc_event_line;

/* The client's GT_IPC_EV_* flags, in the order their lines are logged. */
static const ipc_event_line ipc_event_lines[] = {
    { GT_IPC_EV_DISCONNECT, GT_LOG_WARN, "ipc_disconnect" },
    { GT_IPC_EV_RECONNECT,  GT_LOG_INFO, "ipc_reconnect" },
    { GT_IPC_EV_OVERFLOW,   GT_LOG_WARN, "ipc_overflow" },
};

/* 1 if `ctx` names everything the loop uses: a sensor, a client and a log
 * callback (log_ctx may be NULL). */
static int ctx_usable(const gt_light_loop_ctx *ctx)
{
    if (ctx == NULL) {
        return 0;
    }
    return ctx->sensor != NULL && ctx->client != NULL && ctx->log != NULL;
}

/* Set up `loop` (see light_loop.h): online, counter 0, first reading due
 * now. Nothing is written to *loop unless every argument is usable. */
gt_status gt_light_loop_init(gt_light_loop *loop, const gt_light_loop_ctx *ctx, uint64_t now_ms)
{
    if (loop == NULL || !ctx_usable(ctx)) {
        return GT_ERR_INVAL;
    }
    loop->ctx = *ctx;
    gt_health_init(&loop->health);
    loop->counter = 0;
    loop->next_due_ms = now_ms;
    return GT_OK;
}

/* Hand one line to the caller's log callback. */
static void log_event(const gt_light_loop *loop, gt_log_level level, const char *event,
                      const char *fields)
{
    loop->ctx.log(loop->ctx.log_ctx, level, event, fields);
}

/* The offline probe: a good read means the chip answers again, but a
 * replugged chip has lost its mode, so that value is stale. Re-run the
 * init (sensor_reinit once it goes through), then read again, and return
 * that read's result. A failed read or a NACKed init is a failed probe. */
static gt_status probe_offline(const gt_light_loop *loop, uint32_t *raw_out)
{
    gt_status status = gt_light_read(loop->ctx.sensor, raw_out);
    if (status == GT_OK) {
        status = gt_light_reinit(loop->ctx.sensor);
    }
    if (status == GT_OK) {
        log_event(loop, GT_LOG_INFO, "sensor_reinit", NULL);
        status = gt_light_read(loop->ctx.sensor, raw_out);
    }
    return status;
}

/* Log a health transition: sensor_offline at WARN, sensor_online at INFO,
 * nothing for GT_HEALTH_EV_NONE. */
static void log_health_event(const gt_light_loop *loop, gt_health_event event)
{
    const char *name = gt_health_event_name(event);
    if (name != NULL) {
        log_event(loop, event == GT_HEALTH_EV_OFFLINE ? GT_LOG_WARN : GT_LOG_INFO, name, NULL);
    }
}

/* Take this tick's reading (a probe while offline), feed the result to the
 * health tracker and log what happened: i2c_read_error for a failure, then
 * any transition. Returns 1 with *raw_out set if there is a value to send. */
static int take_reading(gt_light_loop *loop, uint64_t now_ms, uint32_t *raw_out)
{
    gt_status status = loop->health.state == GT_HEALTH_OFFLINE
                           ? probe_offline(loop, raw_out)
                           : gt_light_read(loop->ctx.sensor, raw_out);
    if (status != GT_OK) {
        log_event(loop, GT_LOG_WARN, GT_HEALTH_I2C_READ_ERROR, NULL);
    }
    log_health_event(loop, gt_health_record(&loop->health, status, now_ms));
    return status == GT_OK;
}

/* Send `raw` as one DATA message with the next counter, logging
 * light_saturated first for a clipped value. The counter moves on when the
 * client took the reading (OK) and when it refused it (FULL): a refused
 * reading is counted into the client's GAP, which covers its counter.
 * Returns the client's GT_IPC_EV_* flags. */
static unsigned send_reading(gt_light_loop *loop, uint32_t raw, uint64_t now_ms)
{
    if (raw == LIGHT_SATURATED_RAW) {
        log_event(loop, GT_LOG_INFO, "light_saturated", NULL);
    }
    const gt_msg message = { .type = GT_MSG_DATA, .sensor_id = GT_SENSOR_LIGHT,
                             .counter = loop->counter, .seq = 0, .value = raw };
    unsigned events = 0;
    gt_status status = gt_ipc_client_send(loop->ctx.client, &message, now_ms, &events);
    if (status == GT_OK || status == GT_ERR_FULL) {
        loop->counter++;
    }
    return events;
}

/* ipc_overflow's fields: "refused=N", the client's total refused so far,
 * written into buf. */
static const char *overflow_fields(const gt_light_loop *loop, char *buf, size_t size)
{
    gt_ipc_stats stats = { 0 };
    gt_ipc_client_stats(loop->ctx.client, &stats);
    snprintf(buf, size, "refused=%lu", (unsigned long)stats.refused);
    return buf;
}

/* Log one line per GT_IPC_EV_* flag set in `events`. */
static void log_ipc_events(const gt_light_loop *loop, unsigned events)
{
    const size_t line_count = sizeof ipc_event_lines / sizeof ipc_event_lines[0];
    /* Invariant: every flag in ipc_event_lines[0..line_index-1] set in events is logged. */
    for (size_t line_index = 0; line_index < line_count; line_index++) {
        const ipc_event_line *line = &ipc_event_lines[line_index];
        if ((events & line->flag) == 0) {
            continue;
        }
        char fields[32];
        log_event(loop, line->level, line->event,
                  line->flag == GT_IPC_EV_OVERFLOW ? overflow_fields(loop, fields, sizeof fields)
                                                   : NULL);
    }
}

/* After a due tick, move the due time on by one period. If now_ms is
 * already at or past that too, the tick ran late (a slow bus, a stalled
 * process): skip every slot it missed rather than reading them in a burst,
 * keep the due time on the original grid, and log sample_overrun
 * skipped=N. */
static void advance_schedule(gt_light_loop *loop, uint64_t now_ms)
{
    loop->next_due_ms += GT_LIGHT_PERIOD_MS;
    if (now_ms < loop->next_due_ms) {
        return;
    }
    uint64_t skipped = (now_ms - loop->next_due_ms) / GT_LIGHT_PERIOD_MS + 1;
    loop->next_due_ms += skipped * GT_LIGHT_PERIOD_MS;
    char fields[32];
    snprintf(fields, sizeof fields, "skipped=%llu", (unsigned long long)skipped);
    log_event(loop, GT_LOG_WARN, "sample_overrun", fields);
}

/* Run one tick (see light_loop.h). */
gt_status gt_light_loop_tick(gt_light_loop *loop, uint64_t now_ms, uint64_t *next_due_out)
{
    if (loop == NULL) {
        return GT_ERR_INVAL;
    }
    /* Due ticks keep the schedule even while offline; should_read decides the read. */
    int due = now_ms >= loop->next_due_ms;
    uint32_t raw = 0;
    int have_value = 0;
    if (due && gt_health_should_read(&loop->health, now_ms)) {
        have_value = take_reading(loop, now_ms, &raw);
    }
    /* Sending drains the ring too; with nothing to send, pump so it still drains. */
    unsigned events = 0;
    if (have_value) {
        events = send_reading(loop, raw, now_ms);
    } else {
        gt_ipc_client_pump(loop->ctx.client, now_ms, &events);
    }
    log_ipc_events(loop, events);
    if (due) {
        advance_schedule(loop, now_ms);
    }
    if (next_due_out != NULL) {
        *next_due_out = loop->next_due_ms;
    }
    return GT_OK;
}
