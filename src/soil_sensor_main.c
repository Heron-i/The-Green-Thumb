/* soil_sensor: the soil sensor process (DESIGN.md "soil_sensor process").
 *
 * Purpose: wire the tested soil loop (src/soil_loop.c) to the real world.
 * Every decision lives in the library; this file only supplies the real
 * seams (the Linux I2C bus, the hub.sock client, stderr for log lines),
 * runs a 100 ms tick on the monotonic clock, and stops on SIGTERM/SIGINT.
 *
 * It never exits because the sensor or the hub is missing: the loop starts
 * the sensor offline and retries it every 5 s, and the client buffers up
 * to 64 readings while it reconnects to the hub (DESIGN.md: exiting would
 * cause a restart storm under the supervisor).
 *
 * Log lines go to stderr in gt_log_format's format, component
 * "soil_sensor" (the shared sink, PF-18). The heartbeat to sup.sock
 * arrives with Mechanism E.
 *
 * Usage: soil_sensor [-d /dev/i2c-1] [-p period_ms] /run/plantpi/hub.sock
 *   -p defaults to 60000 (DESIGN.md); a short period like 1000 is for
 *   bring-up and end-to-end tests. */
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gt_log.h"
#include "i2c_bus.h"
#include "ipc.h"
#include "soil_loop.h"
#include "soil_sensor.h"

#define TICK_MS 100   /* how often the loop runs; also the IPC pump rate */

/* Set by the signal handler, read by the main loop. `sig_atomic_t` is an
 * integer type the CPU writes in one step, so the loop never sees half a
 * write; `volatile` tells the compiler the value can change behind its
 * back (in the handler), so it must re-read it every pass instead of
 * keeping a copy in a register. It is the only kind of variable a signal
 * handler may safely set. */
static volatile sig_atomic_t stop_requested = 0;

/* Wall-clock time in us since the epoch, for log lines. */
static uint64_t wall_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* Monotonic time in ms, for the loop's schedule. */
static uint64_t mono_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* The log callback: one gt_log_format line on stderr. */
static void log_line(void *log_ctx, gt_log_level level, const char *event, const char *fields)
{
    (void)log_ctx;
    char line[GT_LOG_LINE_MAX];
    gt_log_entry entry = { .timestamp_us = wall_us(), .level = level,
                           .component = "soil_sensor", .event = event, .fields = fields };
    if (gt_log_format(line, sizeof line, &entry) == GT_OK) {
        fprintf(stderr, "%s\n", line);
    }
}

/* The real bus opener: /dev/i2c-N bound to the seesaw's address. */
static gt_status open_linux_bus(void *bus_ctx, gt_i2c_bus *bus_out)
{
    const char *i2c_dev = bus_ctx;
    return gt_i2c_linux_open(i2c_dev, GT_SOIL_I2C_ADDR, bus_out);
}

/* SIGTERM / SIGINT: ask the main loop to stop after its current tick. */
static void on_stop_signal(int signum)
{
    (void)signum;
    stop_requested = 1;
}

/* Install the stop handler for SIGTERM and SIGINT, and ignore SIGPIPE (a
 * second guard, besides MSG_NOSIGNAL, against a dead hub killing this
 * process). No SA_RESTART, so the tick sleep returns EINTR at once. */
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

/* Sleep until absolute CLOCK_MONOTONIC time `deadline_ms`, or until a
 * signal arrives (then return early so the loop can check stop_requested).
 * Absolute deadlines keep the tick from drifting. */
static void sleep_until_ms(uint64_t deadline_ms)
{
    struct timespec deadline = { .tv_sec = (time_t)(deadline_ms / 1000),
                                 .tv_nsec = (long)(deadline_ms % 1000) * 1000000L };
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
}

typedef struct {
    const char *i2c_dev;
    const char *hub_sock;
    uint32_t    period_ms;
} soil_opts;

/* Parse [-d dev] [-p period_ms] HUB_SOCK into `opts`. Returns 0, or -1
 * after printing usage. */
static int parse_opts(int argc, char **argv, soil_opts *opts)
{
    opts->i2c_dev = "/dev/i2c-1";
    opts->period_ms = GT_SOIL_PERIOD_MS;

    int option;
    while ((option = getopt(argc, argv, "d:p:")) != -1) {
        switch (option) {
        case 'd': opts->i2c_dev = optarg; break;
        case 'p': opts->period_ms = (uint32_t)strtoul(optarg, NULL, 10); break;
        default:  goto usage;
        }
    }
    /* A period under one tick couldn't be kept; 0 would never be due. */
    if (optind != argc - 1 || opts->period_ms < TICK_MS) {
        goto usage;
    }
    opts->hub_sock = argv[optind];
    return 0;

usage:
    fprintf(stderr, "usage: %s [-d /dev/i2c-1] [-p period_ms>=%d] /run/plantpi/hub.sock\n",
            argv[0], TICK_MS);
    return -1;
}

/* Log soil_stop with the client's totals, so a run's readings can be
 * checked against the hub's and storage's counts. */
static void log_soil_stop(const gt_ipc_stats *stats)
{
    char fields[96];
    snprintf(fields, sizeof fields, "sent=%" PRIu32 " queued=%" PRIu32 " refused=%" PRIu32,
             stats->sent, stats->queued, stats->refused);
    log_line(NULL, GT_LOG_INFO, "soil_stop", fields);
}

int main(int argc, char **argv)
{
    soil_opts opts;
    if (parse_opts(argc, argv, &opts) != 0) {
        return 2;
    }

    gt_ipc_client *client = gt_ipc_client_open(opts.hub_sock);
    if (client == NULL) {
        fprintf(stderr, "soil_sensor: bad hub socket path %s, or out of memory\n", opts.hub_sock);
        return 1;
    }
    /* Keep a pointer for the final stats: the loop owns the client, but it
     * stays valid until gt_soil_loop_destroy. */
    gt_ipc_client *client_view = client;

    gt_soil_loop_cfg cfg = {
        .period_ms = opts.period_ms,
        .bus_open = open_linux_bus, .bus_ctx = (void *)opts.i2c_dev,
        .log = log_line, .log_ctx = NULL,
    };
    uint64_t next_tick_ms = mono_ms();
    gt_soil_loop *loop = gt_soil_loop_create(&cfg, client, next_tick_ms);
    if (loop == NULL) {
        fprintf(stderr, "soil_sensor: out of memory\n");
        return 1;
    }

    install_signals();
    char fields[160];
    snprintf(fields, sizeof fields, "dev=%s hub=%s period_ms=%" PRIu32,
             opts.i2c_dev, opts.hub_sock, opts.period_ms);
    log_line(NULL, GT_LOG_INFO, "soil_start", fields);

    /* Invariant: every tick deadline before next_tick_ms has been run or skipped. */
    while (!stop_requested) {
        gt_soil_loop_tick(loop, mono_ms());
        next_tick_ms += TICK_MS;
        uint64_t now_ms = mono_ms();
        if (next_tick_ms < now_ms) {
            next_tick_ms = now_ms;   /* a slow tick (e.g. a 500 ms reinit): don't burst */
        }
        sleep_until_ms(next_tick_ms);
    }

    gt_ipc_stats stats = { 0 };
    gt_ipc_client_stats(client_view, &stats);
    log_soil_stop(&stats);
    gt_soil_loop_destroy(loop);
    return 0;
}
