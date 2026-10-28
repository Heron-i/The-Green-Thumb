/* light_sensor_main: the light sensor process (DESIGN.md "light_sensor
 * process"; Sensor-Pipeline.md, Hub steps H10).
 *
 * Purpose: wire the tested pieces into one long-running process. Every
 * decision lives in the library (light_sensor.c talks to the BH1750,
 * light_loop.c reads, tracks health and sends, ipc_client.c buffers and
 * reconnects); this file only orders the start-up, sleeps between ticks,
 * handles signals and prints log lines. Start-up order:
 *   1. Make the client for hub.sock. It doesn't connect yet: the hub may
 *      still be starting (Mechanism E), and readings wait in its ring.
 *   2. Open the BH1750. If it isn't there (I2C disabled, no permission, no
 *      ACK) the process doesn't exit, which would cause a restart storm: it
 *      logs i2c_open_error, starts offline and retries every 5 s (DESIGN.md
 *      Sensor (I2C) row).
 *   3. Tick the loop once a period until SIGTERM or SIGINT, sleeping until
 *      each due time on the monotonic clock, so the schedule stays anchored.
 * On stop it logs light_stop with the client's counters. Readings still in
 * the ring are dropped (Sensor-Pipeline.md, "Close drops the buffer") and
 * counted as queued=Q.
 *
 * Log lines go to stderr in gt_log_format's format, component "light" (the
 * shared sink, PF-18). The heartbeat to sup.sock arrives with Mechanism E.
 *
 * Usage: light_sensor /dev/i2c-1 /run/plantpi/hub.sock */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gt_log.h"
#include "ipc.h"
#include "light_loop.h"
#include "light_sensor.h"
#include "sensor_health.h"

/* Set by the signal handler, read by the loops. `volatile sig_atomic_t` is
 * the one type C guarantees a signal handler can safely write: `volatile`
 * makes every read go back to memory instead of a cached copy, and
 * sig_atomic_t is read and written in one step, never half-updated. */
static volatile sig_atomic_t stop_requested = 0;

/* Wall-clock time in us since the epoch, for log lines. */
static uint64_t wall_now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* Monotonic time in ms, for the loop's schedule and the health tracker. */
static uint64_t mono_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* The log callback: one gt_log_format line on stderr, component "light". */
static void write_log(void *log_ctx, gt_log_level level, const char *event, const char *fields)
{
    (void)log_ctx;
    const gt_log_entry entry = { .timestamp_us = wall_now_us(), .level = level,
                                 .component = "light", .event = event, .fields = fields };
    char line[GT_LOG_LINE_MAX];
    if (gt_log_format(line, sizeof line, &entry) == GT_OK) {
        fputs(line, stderr);
        fputc('\n', stderr);
    }
}

/* SIGTERM / SIGINT: ask the loops to stop. Setting the flag is all a
 * handler may safely do; the sleep it interrupts returns EINTR. */
static void on_stop_signal(int signum)
{
    (void)signum;
    stop_requested = 1;
}

/* Install the stop handler for SIGTERM and SIGINT, and ignore SIGPIPE (a
 * second guard, besides MSG_NOSIGNAL, against a dead hub killing the
 * process). No SA_RESTART, so a sleep is cut short by a stop signal. */
static void install_signals(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* Sleep until monotonic time due_ms. A stop signal ends it early. */
static void sleep_until_ms(uint64_t due_ms)
{
    const struct timespec due = { .tv_sec = (time_t)(due_ms / 1000u),
                                  .tv_nsec = (long)(due_ms % 1000u) * 1000000L };
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);
}

/* Start-up step 2: open the BH1750 on i2c_dev, retrying every
 * GT_HEALTH_RETRY_MS until it answers or a stop is asked for. Each failed
 * attempt logs i2c_open_error; the first also logs sensor_offline, and a
 * later success logs sensor_online. Returns NULL only on a stop. */
static gt_light_sensor *open_sensor(const char *i2c_dev)
{
    char fields[96];
    snprintf(fields, sizeof fields, "dev=%s", i2c_dev);
    gt_light_sensor *sensor = gt_light_open(i2c_dev);
    int was_offline = 0;
    /* Invariant: every failed attempt so far logged i2c_open_error. */
    while (sensor == NULL && !stop_requested) {
        write_log(NULL, GT_LOG_WARN, "i2c_open_error", fields);
        if (!was_offline) {
            write_log(NULL, GT_LOG_WARN, gt_health_event_name(GT_HEALTH_EV_OFFLINE), NULL);
            was_offline = 1;
        }
        sleep_until_ms(mono_now_ms() + GT_HEALTH_RETRY_MS);
        sensor = stop_requested ? NULL : gt_light_open(i2c_dev);
    }
    if (sensor != NULL && was_offline) {
        write_log(NULL, GT_LOG_INFO, gt_health_event_name(GT_HEALTH_EV_ONLINE), NULL);
    }
    return sensor;
}

/* Start-up step 3: tick the loop once a period until a stop. Returns the
 * counter the next reading would have carried (readings handed to the
 * client this run). */
static uint32_t run_loop(gt_light_sensor *sensor, gt_ipc_client *client)
{
    const gt_light_loop_ctx ctx = { .sensor = sensor, .client = client,
                                    .log = write_log, .log_ctx = NULL };
    gt_light_loop loop;
    if (gt_light_loop_init(&loop, &ctx, mono_now_ms()) != GT_OK) {
        return 0;
    }
    uint64_t next_due_ms = 0;
    /* Invariant: every due time before next_due_ms has been ticked or skipped. */
    while (!stop_requested) {
        gt_light_loop_tick(&loop, mono_now_ms(), &next_due_ms);
        sleep_until_ms(next_due_ms);
    }
    return loop.counter;
}

/* Log light_stop: counter=N readings handed to the client, sent=S taken by
 * the hub, refused=R (overflow) and queued=Q still in the ring, which close
 * drops. Dropped readings are WARN: a real loss with no GAP to cover it. */
static void log_light_stop(const gt_ipc_client *client, uint32_t counter)
{
    gt_ipc_stats stats = { 0 };
    gt_ipc_client_stats(client, &stats);
    char fields[128];
    snprintf(fields, sizeof fields, "counter=%lu sent=%lu refused=%lu queued=%lu",
             (unsigned long)counter, (unsigned long)stats.sent, (unsigned long)stats.refused,
             (unsigned long)stats.queued);
    write_log(NULL, stats.queued > 0 ? GT_LOG_WARN : GT_LOG_INFO, "light_stop", fields);
}

/* The whole life of the process after the arguments are read: start-up
 * steps 1-3, then light_stop and release everything. Returns the exit
 * code: 0 for a clean stop, 1 if the client can't be made. */
static int run_light(const char *i2c_dev, const char *hub_sock)
{
    gt_ipc_client *client = gt_ipc_client_open(hub_sock);
    if (client == NULL) {
        fprintf(stderr, "light_sensor: can't start (bad hub socket path %s, or out of memory)\n",
                hub_sock);
        return 1;
    }
    install_signals();
    char fields[160];
    snprintf(fields, sizeof fields, "dev=%s hub_sock=%s", i2c_dev, hub_sock);
    write_log(NULL, GT_LOG_INFO, "light_start", fields);

    gt_light_sensor *sensor = open_sensor(i2c_dev);
    uint32_t counter = sensor != NULL ? run_loop(sensor, client) : 0;

    log_light_stop(client, counter);
    gt_light_close(sensor);
    gt_ipc_client_close(client);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s /dev/i2c-1 /run/plantpi/hub.sock\n", argv[0]);
        return 2;
    }
    return run_light(argv[1], argv[2]);
}
